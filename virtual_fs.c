// 代码参考: https://github.com/osxfuse/filesystems

#include <AvailabilityMacros.h>

#if MAC_OS_X_VERSION_MIN_REQUIRED < 1050
#error "This file system requires Leopard and above."
#endif

#if MAC_OS_X_VERSION_MIN_REQUIRED < 1060
#define HAVE_FSETATTR_X 0
#else
#define HAVE_FSETATTR_X 1
#endif

#define FUSE_USE_VERSION 29

#include <fuse.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <sys/attr.h>
#include <sys/param.h>
#include <sys/vnode.h>

#if defined(_POSIX_C_SOURCE)
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;
#endif

#define G_PREFIX                       "org"
#define G_KAUTH_FILESEC_XATTR G_PREFIX ".apple.system.Security"
#define A_PREFIX                       "com"
#define A_KAUTH_FILESEC_XATTR A_PREFIX ".apple.system.Security"

struct loopback {
    int case_insensitive;
};

static struct loopback loopback;

// 存储映射点路径
static const char *map_point = NULL;

// 获取真实的文件路径，将映射点路径和相对路径拼接起来
static char *get_real_path(const char *path) {
    // 分配足够的内存来存储映射点路径和相对路径
    char *real_path = (char *) malloc(strlen(map_point) + strlen(path) + 1);
    // 拷贝映射点路径
    strcpy(real_path, map_point);
    // 拼接相对路径
    strcat(real_path, path);
    // 返回真实路径
    return real_path;
}

// 获取文件或目录的属性
static int loopback_getattr(const char *path, struct stat *stbuf) {
    int res;

    // 获取真实路径
    char *real_path = get_real_path(path);
    // 获取文件或目录属性
    res = lstat(real_path, stbuf);
    // 释放真实路径的内存
    free(real_path);

    // 设置文件块大小，FUSE_VERSION >= 29时有效
#if FUSE_VERSION >= 29
    stbuf->st_blksize = 0;
#endif

    if (res == -1) {
        // 若出错，返回相应错误码
        return -errno;
    }

    return 0;
}

// 获取文件或目录的属性（使用文件句柄）
static int loopback_fgetattr(const char *path, struct stat *stbuf,
                             struct fuse_file_info *fi) {
    int res;

    // 忽略传入的路径参数
    (void) path;

    // 获取文件属性
    res = fstat((int) fi->fh, stbuf);

    // 设置文件块大小，FUSE_VERSION >= 29时有效
#if FUSE_VERSION >= 29
    // 回退到全局I/O大小，参见loopback_getattr()
    stbuf->st_blksize = 0;
#endif

    if (res == -1) {
        // 若出错，返回相应错误码
        return -errno;
    }

    return 0;
}

// 读取符号链接
static int loopback_readlink(const char *path, char *buf, size_t size) {
    int res;

    // 获取真实路径
    char *real_path = get_real_path(path);
    // 读取符号链接内容
    res = (int) readlink(real_path, buf, size - 1);
    // 释放真实路径的内存
    free(real_path);

    if (res == -1) {
        // 若出错，返回相应错误码
        return -errno;
    }

    buf[res] = '\0';

    return 0;
}

// 目录流结构
struct loopback_dirp {
    DIR *dp;
    struct dirent *entry;
    off_t offset;
};

// 打开目录
static int loopback_opendir(const char *path, struct fuse_file_info *fi) {
    int res;

    // 分配目录流结构的内存
    struct loopback_dirp *d = malloc(sizeof(struct loopback_dirp));
    if (d == NULL) {
        // 若内存分配失败，返回内存不足错误码
        return -ENOMEM;
    }

    // 获取真实路径
    char *real_path = get_real_path(path);
    // 打开目录流
    d->dp = opendir(real_path);
    // 释放真实路径的内存
    free(real_path);
    if (d->dp == NULL) {
        // 若打开目录流失败，返回相应错误码
        res = -errno;
        free(d);
        return res;
    }

    // 初始化目录流结构
    d->offset = 0;
    d->entry = NULL;

    // 将目录流结构指针作为文件句柄保存在fi中
    fi->fh = (unsigned long) d;

    return 0;
}

// 获取目录流结构指针
static inline struct loopback_dirp *get_dirp(struct fuse_file_info *fi) {
    return (struct loopback_dirp *) (uintptr_t) fi->fh;
}

// 读取目录
static int loopback_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                            off_t offset, struct fuse_file_info *fi) {
    // 获取目录流结构指针
    struct loopback_dirp *d = get_dirp(fi);

    // 忽略传入的路径参数
    (void) path;

    if (offset == 0) {
        // 若偏移量为0，重置目录流，初始化目录流结构
        rewinddir(d->dp);
        d->entry = NULL;
        d->offset = 0;
    } else if (offset != d->offset) {
        // 若偏移量不等于当前目录偏移量，设置目录流到指定偏移量，初始化目录流结构
        seekdir(d->dp, offset);
        d->entry = NULL;
        d->offset = offset;
    }

    while (1) {
        struct stat st;
        off_t nextoff;

        if (!d->entry) {
            // 若当前目录项为空，读取下一个目录项
            d->entry = readdir(d->dp);
            if (!d->entry) {
                // 若读取完所有目录项，退出循环
                break;
            }
        }

        // 将目录项信息填充到结构体st中
        memset(&st, 0, sizeof(st));
        st.st_ino = d->entry->d_ino;
        st.st_mode = d->entry->d_type << 12;
        nextoff = telldir(d->dp);
        // 使用filler函数将目录项信息填充到buf中，获取下一个目录项的偏移量
        if (filler(buf, d->entry->d_name, &st, nextoff)) {
            // 若填充操作完成，退出循环
            break;
        }

        // 清空目录流结构中的当前目录项和偏移量
        d->entry = NULL;
        d->offset = nextoff;
    }

    return 0;
}

// 关闭目录
static int loopback_releasedir(const char *path, struct fuse_file_info *fi) {
    // 获取目录流结构指针
    struct loopback_dirp *d = get_dirp(fi);

    // 忽略传入的路径参数
    (void) path;

    // 关闭目录流，释放目录流结构的内存
    closedir(d->dp);
    free(d);

    return 0;
}

// 创建节点
static int loopback_mknod(const char *path, mode_t mode, dev_t rdev) {
    int res;

    // 获取真实路径
    char *real_path = get_real_path(path);
    path = real_path;

    // 如果是FIFO节点，使用mkfifo创建；否则，使用mknod创建
    if (S_ISFIFO(mode)) {
        res = mkfifo(path, mode);
    } else {
        res = mknod(path, mode, rdev);
    }

    // 释放真实路径的内存
    free(real_path);

    if (res == -1) {
        // 若出错，返回相应错误码
        return -errno;
    }

    return 0;
}

// 创建目录
static int loopback_mkdir(const char *path, mode_t mode) {
    int res;

    // 获取真实路径
    char *real_path = get_real_path(path);
    // 使用mkdir创建目录
    res = mkdir(real_path, mode);
    // 释放真实路径的内存
    free(real_path);

    if (res == -1) {
        // 若出错，返回相应错误码
        return -errno;
    }

    return 0;
}

// 删除文件
static int loopback_unlink(const char *path) {
    int res;

    // 获取真实路径
    char *real_path = get_real_path(path);
    // 调用unlink函数删除文件
    res = unlink(real_path);
    // 释放真实路径的内存
    free(real_path);

    if (res == -1) {
        // 若出错，返回相应错误码
        return -errno;
    }

    return 0;
}

// 删除目录
static int loopback_rmdir(const char *path) {
    int res;

    // 获取真实路径
    char *real_path = get_real_path(path);
    // 调用rmdir函数删除目录
    res = rmdir(real_path);
    // 释放真实路径的内存
    free(real_path);

    if (res == -1) {
        // 若出错，返回相应错误码
        return -errno;
    }

    return 0;
}

// 创建符号链接
static int loopback_symlink(const char *from, const char *to) {
    int res;

    // 调用symlink函数创建符号链接
    res = symlink(from, to);

    if (res == -1) {
        // 若出错，返回相应错误码
        return -errno;
    }

    return 0;
}

// 重命名文件或目录
static int loopback_rename(const char *from, const char *to) {
    int res;

    // 调用rename函数进行重命名
    res = rename(from, to);

    if (res == -1) {
        // 若出错，返回相应错误码
        return -errno;
    }

    return 0;
}

// 交换两个文件或目录的内容
static int loopback_exchange(const char *path1, const char *path2, __attribute__((unused)) unsigned long options) {
    int res;

    // 使用系统调用进行文件或目录内容的交换
#if MAC_OS_X_VERSION_MIN_REQUIRED < 101200
    // 如果系统版本低于10.12，使用exchangedata函数
#if MAC_OS_X_VERSION_MAX_ALLOWED >= 101200
    if (renamex_np) {
        res = renamex_np(path1, path2, RENAME_SWAP);
    } else
#endif
    {
        res = exchangedata(path1, path2, options);
    }
#else
    // 如果系统版本不低于10.12，使用renamex_np函数
    res = renamex_np(path1, path2, RENAME_SWAP);
#endif

    if (res == -1) {
        // 若出错，返回相应错误码
        return -errno;
    }

    return 0;
}

// 创建硬链接
static int loopback_link(const char *from, const char *to) {
    int res;

    // 使用link函数创建硬链接
    res = link(from, to);

    if (res == -1) {
        // 若出错，返回相应错误码
        return -errno;
    }

    return 0;
}

// 如果系统支持 fsetattr_x 特性
#if HAVE_FSETATTR_X

// 函数用于设置文件属性，与 fsetattrlist 相关
static int loopback_fsetattr_x(__attribute__((unused)) const char *path, struct setattr_x *attr,
                               struct fuse_file_info *fi) {
    int res;
    uid_t uid = -1;
    gid_t gid = -1;

    // 设置文件权限
    if (SETATTR_WANTS_MODE(attr)) {
        res = fchmod((int) fi->fh, attr->mode);
        if (res == -1) {
            return -errno;
        }
    }

    // 设置用户 ID 和组 ID
    if (SETATTR_WANTS_UID(attr)) {
        uid = attr->uid;
    }

    if (SETATTR_WANTS_GID(attr)) {
        gid = attr->gid;
    }

    if ((uid != -1) || (gid != -1)) {
        res = fchown((int) fi->fh, uid, gid);
        if (res == -1) {
            return -errno;
        }
    }

    // 设置文件大小
    if (SETATTR_WANTS_SIZE(attr)) {
        res = ftruncate((int) fi->fh, attr->size);
        if (res == -1) {
            return -errno;
        }
    }

    // 设置修改时间和访问时间
    if (SETATTR_WANTS_MODTIME(attr)) {
        struct timeval tv[2];
        if (!SETATTR_WANTS_ACCTIME(attr)) {
            gettimeofday(&tv[0], NULL);
        } else {
            tv[0].tv_sec = attr->acctime.tv_sec;
            tv[0].tv_usec = (int) attr->acctime.tv_nsec / 1000;
        }
        tv[1].tv_sec = attr->modtime.tv_sec;
        tv[1].tv_usec = (int) attr->modtime.tv_nsec / 1000;
        res = futimes((int) fi->fh, tv);
        if (res == -1) {
            return -errno;
        }
    }

    // 设置创建时间
    if (SETATTR_WANTS_CRTIME(attr)) {
        struct attrlist attributes;

        attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
        attributes.reserved = 0;
        attributes.commonattr = ATTR_CMN_CRTIME;
        attributes.dirattr = 0;
        attributes.fileattr = 0;
        attributes.forkattr = 0;
        attributes.volattr = 0;

        res = fsetattrlist((int) fi->fh, &attributes, &attr->crtime,
                           sizeof(struct timespec), FSOPT_NOFOLLOW);

        if (res == -1) {
            return -errno;
        }
    }

    // 设置更改时间
    if (SETATTR_WANTS_CHGTIME(attr)) {
        struct attrlist attributes;

        attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
        attributes.reserved = 0;
        attributes.commonattr = ATTR_CMN_CHGTIME;
        attributes.dirattr = 0;
        attributes.fileattr = 0;
        attributes.forkattr = 0;
        attributes.volattr = 0;

        res = fsetattrlist((int) fi->fh, &attributes, &attr->chgtime,
                           sizeof(struct timespec), FSOPT_NOFOLLOW);

        if (res == -1) {
            return -errno;
        }
    }

    // 设置备份时间
    if (SETATTR_WANTS_BKUPTIME(attr)) {
        struct attrlist attributes;

        attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
        attributes.reserved = 0;
        attributes.commonattr = ATTR_CMN_BKUPTIME;
        attributes.dirattr = 0;
        attributes.fileattr = 0;
        attributes.forkattr = 0;
        attributes.volattr = 0;

        res = fsetattrlist((int) fi->fh, &attributes, &attr->bkuptime,
                           sizeof(struct timespec), FSOPT_NOFOLLOW);

        if (res == -1) {
            return -errno;
        }
    }

    // 设置文件标志
    if (SETATTR_WANTS_FLAGS(attr)) {
        res = fchflags((int) fi->fh, attr->flags);
        if (res == -1) {
            return -errno;
        }
    }

    return 0;
}

#endif /* HAVE_FSETATTR_X */

// 函数用于设置文件属性，支持 macOS 下的 extended attributes
static int loopback_setattr_x(const char *path, struct setattr_x *attr) {
    int res;
    uid_t uid = -1;
    gid_t gid = -1;
    char *real_path = get_real_path(path);
    path = real_path;

    // 设置文件权限
    if (SETATTR_WANTS_MODE(attr)) {
        res = lchmod(path, attr->mode);
        if (res == -1) {
            return -errno;
        }
    }

    // 设置用户 ID 和组 ID
    if (SETATTR_WANTS_UID(attr)) {
        uid = attr->uid;
    }

    if (SETATTR_WANTS_GID(attr)) {
        gid = attr->gid;
    }

    // 如果需要设置用户 ID 或组 ID，执行设置
    if ((uid != -1) || (gid != -1)) {
        res = lchown(path, uid, gid);
        if (res == -1) {
            return -errno;
        }
    }

    // 设置文件大小
    if (SETATTR_WANTS_SIZE(attr)) {
        res = truncate(path, attr->size);
        if (res == -1) {
            return -errno;
        }
    }

    // 设置修改时间和访问时间
    if (SETATTR_WANTS_MODTIME(attr)) {
        struct timeval tv[2];
        if (!SETATTR_WANTS_ACCTIME(attr)) {
            gettimeofday(&tv[0], NULL);
        } else {
            tv[0].tv_sec = attr->acctime.tv_sec;
            tv[0].tv_usec = (int) attr->acctime.tv_nsec / 1000;
        }
        tv[1].tv_sec = attr->modtime.tv_sec;
        tv[1].tv_usec = (int) attr->modtime.tv_nsec / 1000;
        res = lutimes(path, tv);
        if (res == -1) {
            return -errno;
        }
    }

    // 设置创建时间
    if (SETATTR_WANTS_CRTIME(attr)) {
        struct attrlist attributes;

        attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
        attributes.reserved = 0;
        attributes.commonattr = ATTR_CMN_CRTIME;
        attributes.dirattr = 0;
        attributes.fileattr = 0;
        attributes.forkattr = 0;
        attributes.volattr = 0;

        res = setattrlist(path, &attributes, &attr->crtime,
                          sizeof(struct timespec), FSOPT_NOFOLLOW);

        if (res == -1) {
            return -errno;
        }
    }

    // 设置更改时间
    if (SETATTR_WANTS_CHGTIME(attr)) {
        struct attrlist attributes;

        attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
        attributes.reserved = 0;
        attributes.commonattr = ATTR_CMN_CHGTIME;
        attributes.dirattr = 0;
        attributes.fileattr = 0;
        attributes.forkattr = 0;
        attributes.volattr = 0;

        res = setattrlist(path, &attributes, &attr->chgtime,
                          sizeof(struct timespec), FSOPT_NOFOLLOW);

        if (res == -1) {
            return -errno;
        }
    }

    // 设置备份时间
    if (SETATTR_WANTS_BKUPTIME(attr)) {
        struct attrlist attributes;

        attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
        attributes.reserved = 0;
        attributes.commonattr = ATTR_CMN_BKUPTIME;
        attributes.dirattr = 0;
        attributes.fileattr = 0;
        attributes.forkattr = 0;
        attributes.volattr = 0;

        res = setattrlist(path, &attributes, &attr->bkuptime,
                          sizeof(struct timespec), FSOPT_NOFOLLOW);

        if (res == -1) {
            return -errno;
        }
    }

    // 设置文件标志
    if (SETATTR_WANTS_FLAGS(attr)) {
        res = lchflags(path, attr->flags);
        if (res == -1) {
            return -errno;
        }
    }

    free(real_path);
    return 0;
}

// 函数用于获取文件的备份时间和创建时间
static int loopback_getxtimes(const char *path, struct timespec *bkuptime,
                              struct timespec *crtime) {
    int res;
    struct attrlist attributes;
    char *real_path = get_real_path(path);
    path = real_path;

    attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
    attributes.reserved = 0;
    attributes.commonattr = 0;
    attributes.dirattr = 0;
    attributes.fileattr = 0;
    attributes.forkattr = 0;
    attributes.volattr = 0;

    // 结构用于存储获取到的扩展时间属性
    struct xtimeattrbuf {
        uint32_t size;
        struct timespec xtime;
    } __attribute__ ((packed));

    struct xtimeattrbuf buf;

    // 获取备份时间
    attributes.commonattr = ATTR_CMN_BKUPTIME;
    res = getattrlist(path, &attributes, &buf, sizeof(buf), FSOPT_NOFOLLOW);
    if (res == 0) {
        (void) memcpy(bkuptime, &(buf.xtime), sizeof(struct timespec));
    } else {
        (void) memset(bkuptime, 0, sizeof(struct timespec));
    }

    // 获取创建时间
    attributes.commonattr = ATTR_CMN_CRTIME;
    res = getattrlist(path, &attributes, &buf, sizeof(buf), FSOPT_NOFOLLOW);
    if (res == 0) {
        (void) memcpy(crtime, &(buf.xtime), sizeof(struct timespec));
    } else {
        (void) memset(crtime, 0, sizeof(struct timespec));
    }

    free(real_path);
    return 0;
}

// 函数用于创建文件并打开，返回文件描述符，若失败则返回相应错误码
static int loopback_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    int fd;

    // 获取真实文件路径
    char *real_path = get_real_path(path);
    // 以指定标志位和权限模式打开或创建文件
    fd = open(real_path, fi->flags, mode);
    free(real_path);
    if (fd == -1) {
        return -errno;
    }

    // 将文件描述符存储在fuse_file_info结构中的fh字段
    fi->fh = fd;
    return 0;
}

// 函数用于打开文件，返回文件描述符，若失败则返回相应错误码
static int loopback_open(const char *path, struct fuse_file_info *fi) {
    int fd;

    // 获取真实文件路径
    char *real_path = get_real_path(path);
    // 以指定标志位打开文件
    fd = open(real_path, fi->flags);
    free(real_path);
    if (fd == -1) {
        return -errno;
    }

    // 将文件描述符存储在fuse_file_info结构中的fh字段
    fi->fh = fd;
    return 0;
}

// 函数用于读取文件内容，始终返回成功，但不进行实际读取
static int loopback_read(__attribute__((unused)) const char *path, __attribute__((unused)) char *buf,
                         __attribute__((unused)) size_t size, __attribute__((unused)) off_t offset,
                         __attribute__((unused)) struct fuse_file_info *fi) {
    return 0;
}

// 函数用于写入文件内容，返回写入的字节数，始终返回size
static int
loopback_write(__attribute__((unused)) const char *path, __attribute__((unused)) const char *buf, size_t size,
               __attribute__((unused)) off_t offset, __attribute__((unused)) struct fuse_file_info *fi) {
    return (int) size;
}

// 函数用于获取文件系统统计信息
static int loopback_statfs(const char *path, struct statvfs *stbuf) {
    int res;

    // 获取真实文件路径
    char *real_path = get_real_path(path);
    // 获取文件系统统计信息
    res = statvfs(real_path, stbuf);
    free(real_path);
    if (res == -1) {
        return -errno;
    }

    return 0;
}

// 函数用于刷新文件状态，将文件数据同步到磁盘
static int loopback_flush(const char *path, struct fuse_file_info *fi) {
    int res;

    (void) path;

    // 刷新文件状态，将文件数据同步到磁盘
    res = close(dup((int) fi->fh));
    if (res == -1) {
        return -errno;
    }

    return 0;
}

// 函数用于释放文件资源
static int loopback_release(const char *path, struct fuse_file_info *fi) {
    (void) path;

    // 关闭文件描述符
    close((int) fi->fh);

    return 0;
}

// 函数用于同步文件数据到磁盘
static int loopback_fsync(const char *path, int isdatasync, struct fuse_file_info *fi) {
    int res;

    (void) path;

    (void) isdatasync;

    // 同步文件数据到磁盘
    res = fsync((int) fi->fh);
    if (res == -1) {
        return -errno;
    }

    return 0;
}

// 函数用于设置扩展属性，支持 macOS 下的 extended attributes
static int loopback_setxattr(const char *path, const char *name, const char *value,
                             size_t size, __attribute__((unused)) int flags, uint32_t position) {
    int res;
    char *real_path = get_real_path(path);
    path = real_path;

    // 若属性名为 A_KAUTH_FILESEC_XATTR，则添加前缀 G_PREFIX
    if (!strcmp(name, A_KAUTH_FILESEC_XATTR)) {

        char new_name[MAXPATHLEN];

        memcpy(new_name, A_KAUTH_FILESEC_XATTR, sizeof(A_KAUTH_FILESEC_XATTR));
        memcpy(new_name, G_PREFIX, sizeof(G_PREFIX) - 1);

        // 设置扩展属性
        res = setxattr(path, new_name, value, size, position, XATTR_NOFOLLOW);

    } else {
        // 设置扩展属性
        res = setxattr(path, name, value, size, position, XATTR_NOFOLLOW);
    }

    if (res == -1) {
        return -errno;
    }

    free(real_path);
    return 0;
}

// 函数用于获取扩展属性，支持 macOS 下的 extended attributes
static int loopback_getxattr(const char *path, const char *name, char *value, size_t size,
                             uint32_t position) {
    int res;
    char *real_path = get_real_path(path);
    path = real_path;

    // 若属性名为 A_KAUTH_FILESEC_XATTR，则添加前缀 G_PREFIX
    if (strcmp(name, A_KAUTH_FILESEC_XATTR) == 0) {

        char new_name[MAXPATHLEN];

        memcpy(new_name, A_KAUTH_FILESEC_XATTR, sizeof(A_KAUTH_FILESEC_XATTR));
        memcpy(new_name, G_PREFIX, sizeof(G_PREFIX) - 1);

        // 获取扩展属性
        res = (int) getxattr(path, new_name, value, size, position, XATTR_NOFOLLOW);

    } else {
        // 获取扩展属性
        res = (int) getxattr(path, name, value, size, position, XATTR_NOFOLLOW);
    }

    if (res == -1) {
        return -errno;
    }

    free(real_path);
    return res;
}

// 函数用于获取指定路径的扩展属性列表
static int loopback_listxattr(const char *path, char *list, size_t size) {
    char *real_path = get_real_path(path);
    path = real_path;
    ssize_t res = listxattr(path, list, size, XATTR_NOFOLLOW);
    free(real_path);

    // 移除扩展属性 G_KAUTH_FILESEC_XATTR
    if (res > 0) {
        if (list) {
            size_t len = 0;
            char *curr = list;
            do {
                size_t thislen = strlen(curr) + 1;
                if (strcmp(curr, G_KAUTH_FILESEC_XATTR) == 0) {
                    memmove(curr, curr + thislen, res - len - thislen);
                    res -= (ssize_t) thislen;
                    break;
                }
                curr += thislen;
                len += thislen;
            } while (len < res);
        }
    }

    if (res == -1) {
        return -errno;
    }

    return (int) res;
}

// 函数用于移除指定路径的扩展属性
static int loopback_removexattr(const char *path, const char *name) {
    int res;
    char *real_path = get_real_path(path);
    path = real_path;

    // 若属性名为 A_KAUTH_FILESEC_XATTR，则添加前缀 G_PREFIX
    if (strcmp(name, A_KAUTH_FILESEC_XATTR) == 0) {
        char new_name[MAXPATHLEN];
        memcpy(new_name, A_KAUTH_FILESEC_XATTR, sizeof(A_KAUTH_FILESEC_XATTR));
        memcpy(new_name, G_PREFIX, sizeof(G_PREFIX) - 1);

        // 移除扩展属性
        res = removexattr(path, new_name, XATTR_NOFOLLOW);
    } else {
        // 移除扩展属性
        res = removexattr(path, name, XATTR_NOFOLLOW);
    }

    if (res == -1) {
        return -errno;
    }

    free(real_path);
    return 0;
}

#if FUSE_VERSION >= 29

// 函数用于预分配或空洞创建文件空间
static int loopback_fallocate(__attribute__((unused)) const char *path, int mode, off_t offset, off_t length,
                              struct fuse_file_info *fi) {
    fstore_t fstore;

    // 检查是否支持预分配
    if (!(mode & PREALLOCATE)) {
        return -ENOTSUP;
    }

    fstore.fst_flags = 0;
    if (mode & ALLOCATECONTIG) {
        fstore.fst_flags |= F_ALLOCATECONTIG;
    }
    if (mode & ALLOCATEALL) {
        fstore.fst_flags |= F_ALLOCATEALL;
    }

    if (mode & ALLOCATEFROMPEOF) {
        fstore.fst_posmode = F_PEOFPOSMODE;
    } else if (mode & ALLOCATEFROMVOL) {
        fstore.fst_posmode = F_VOLPOSMODE;
    }

    fstore.fst_offset = offset;
    fstore.fst_length = length;

    // 使用fcntl调用执行预分配操作
    if (fcntl((int) fi->fh, F_PREALLOCATE, &fstore) == -1) {
        return -errno;
    } else {
        return 0;
    }
}

#endif /* FUSE_VERSION >= 29 */

// 函数用于设置文件系统的卷标名称，返回0表示成功
static int loopback_setvolname(__attribute__((unused)) const char *name) {
    return 0;
}

// 函数用于初始化FUSE文件系统连接信息
void *loopback_init(struct fuse_conn_info *conn) {
    // 启用设置卷标名称功能
    FUSE_ENABLE_SETVOLNAME(conn);
    // 启用扩展时间支持
    FUSE_ENABLE_XTIMES(conn);

#ifdef FUSE_ENABLE_CASE_INSENSITIVE
    // 如果启用了不区分大小写的功能，则启用该功能
    if (loopback.case_insensitive) {
        FUSE_ENABLE_CASE_INSENSITIVE(conn);
    }
#endif

    return NULL;
}

// 函数用于销毁FUSE文件系统连接信息
void loopback_destroy(__attribute__((unused)) void *userdata) {
    /* nothing */
}

// 定义FUSE文件系统操作的回调函数集合
static struct fuse_operations loopback_oper = {
        .init        = loopback_init,
        .destroy     = loopback_destroy,
        .getattr     = loopback_getattr,
        .fgetattr    = loopback_fgetattr,
        // .access      = loopback_access,  // 注释掉未实现的函数
        .readlink    = loopback_readlink,
        .opendir     = loopback_opendir,
        .readdir     = loopback_readdir,
        .releasedir  = loopback_releasedir,
        .mknod       = loopback_mknod,
        .mkdir       = loopback_mkdir,
        .symlink     = loopback_symlink,
        .unlink      = loopback_unlink,
        .rmdir       = loopback_rmdir,
        .rename      = loopback_rename,
        .link        = loopback_link,
        .create      = loopback_create,
        .open        = loopback_open,
        .read        = loopback_read,
        .write       = loopback_write,
        .statfs      = loopback_statfs,
        .flush       = loopback_flush,
        .release     = loopback_release,
        .fsync       = loopback_fsync,
        .setxattr    = loopback_setxattr,
        .getxattr    = loopback_getxattr,
        .listxattr   = loopback_listxattr,
        .removexattr = loopback_removexattr,
        .exchange    = loopback_exchange,
        .getxtimes   = loopback_getxtimes,
        .setattr_x   = loopback_setattr_x,
#if HAVE_FSETATTR_X
        .fsetattr_x  = loopback_fsetattr_x,
#endif
#if FUSE_VERSION >= 29
        .fallocate   = loopback_fallocate,
#endif
        .setvolname  = loopback_setvolname,

#if FUSE_VERSION >= 29
#if HAVE_FSETATTR_X
        .flag_nullpath_ok = 1,
        .flag_nopath = 1,
#else
        .flag_nullpath_ok = 0,
.flag_nopath = 0,
#endif
#endif /* FUSE_VERSION >= 29 */
};

// 定义FUSE文件系统的选项数组
static const struct fuse_opt loopback_opts[] = {
        {"case_insensitive", offsetof(struct loopback, case_insensitive), 1},
        FUSE_OPT_END
};

// 主函数，用于启动FUSE文件系统
int main(int argc, char *argv[]) {
    int fuse_version_;
    fuse_version_ = fuse_version();
    fprintf(stderr, "本地fuse版本: %d\n", fuse_version_);

    // 检查FUSE版本是否符合要求
    if (FUSE_USE_VERSION > fuse_version_) {
        fprintf(stderr, "本地fuse版本太低! 编译要求版本: FUSE_USE_VERSION\n");
        exit(EXIT_FAILURE);
    }

    // 检查命令行参数数量
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <挂载路径> <映射路径>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // 设置映射路径
    map_point = argv[2];
    argc--;

    int res;
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    // 初始化FUSE文件系统选项
    loopback.case_insensitive = 0;
    if (fuse_opt_parse(&args, &loopback, loopback_opts, NULL) == -1) {
        exit(1);
    }

    umask(0);
    // 启动FUSE文件系统
    res = fuse_main(args.argc, args.argv, &loopback_oper, NULL);

    fuse_opt_free_args(&args);
    fprintf(stderr, "✅启动成功!");
    return res;
}


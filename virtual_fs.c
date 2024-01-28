// 代码参考: https://github.com/macos-fuse-t/libfuse/tree/master/example

#define FUSE_USE_VERSION 29

#define HAVE_SETXATTR    1

#ifdef __APPLE__
//#define _DARWIN_C_SOURCE
#else
//#define _GNU_SOURCE
#endif

#include <fuse.h>

#ifndef __APPLE__
#include <ulockmgr.h>
#endif

#include <stdio.h>
//#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>

#ifdef HAVE_SETXATTR

#include <sys/xattr.h>

#endif
#ifndef __APPLE__
#include <sys/file.h> /* flock(2) */
#endif

#include <sys/param.h>

#ifdef __APPLE__

//#include <fcntl.h>
#include <sys/vnode.h>

#if defined(_POSIX_C_SOURCE)
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;
#endif

#include <sys/attr.h>

#define G_PREFIX            "org"
#define G_KAUTH_FILESEC_XATTR G_PREFIX    ".apple.system.Security"
#define A_PREFIX            "com"
#define A_KAUTH_FILESEC_XATTR A_PREFIX    ".apple.system.Security"
#define XATTR_APPLE_PREFIX        "com.apple."

#endif /* __APPLE__ */

// 全局变量，用于存储/dev/null的文件描述符
static int dev_null_fd;

// 全局变量，用于存储预设的符号链接路径
static const char *linkpath = "/dev/null";

// 全局变量，用于存储预设的目录流
//static DIR *dev_null_dir;

// 存储映射点路径
//static const char *map_point = NULL;

// 全局变量保存虚拟文件的状态信息
struct stat virtual_file_stat;

// 函数用于判断路径是否指向一个目录
static int is_directory(const char *path) {
    // 从路径中获取文件名
    const char *filename = strrchr(path, '/');

    // 如果找到了文件名，则进行判断
    if (filename != NULL) {
        // 获取文件名中的后缀
        const char *suffix = strrchr(filename, '.');

        // 如果找到了后缀，并且后缀中第一个不是数字，则认为是文件
        if (suffix != NULL) {
            suffix++;  // 移动到后缀的第一个字符
            if (*suffix < '0' || *suffix > '9') {
                return 0;
            }
        }
    }

    return 1;
}

// 获取真实的文件路径，将映射点路径和相对路径拼接起来
//static char *get_real_path(const char *path) {
//    // 分配足够的内存来存储映射点路径和相对路径
//    char *real_path = (char *) malloc(strlen(map_point) + strlen(path) + 1);
//    // 拷贝映射点路径
//    strcpy(real_path, map_point);
//    // 拼接相对路径
//    strcat(real_path, path);
//    // 返回真实路径
//    return real_path;
//}

static int xmp_getattr(const char *path, struct stat *stbuf) {
    int res;

    res = lstat(path, stbuf);
    if (res == -1) {
        // 直接使用全局变量中的虚拟文件状态信息
//        memcpy(stbuf, &virtual_file_stat, sizeof(struct stat));
        // 直接赋值虚拟文件的状态信息到 stbuf
        if (is_directory(path)) {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
        } else {
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
//            stbuf->st_size = 4096;
            *stbuf = virtual_file_stat;
        }
//        return -errno;
    }

    return 0;
}

static int xmp_fgetattr(const char *path, struct stat *stbuf,
                        struct fuse_file_info *fi) {
    int res;

    (void) path;

    res = fstat((int) fi->fh, stbuf);
    if (res == -1) {
        if (is_directory(path)) {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
        } else {
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
//            stbuf->st_size = 4096;
            *stbuf = virtual_file_stat;
        }
//        return -errno;
    }

    return 0;
}

static int xmp_access(__attribute__((unused)) const char *path, __attribute__((unused)) int mask) {
//    int res;

//    res = access(path, mask);
//    if (res == -1)
//        return -errno;

    return 0;
}

static int xmp_readlink(__attribute__((unused)) const char *path, char *buf, __attribute__((unused)) size_t size) {
    // 直接将预设的符号链接路径的地址赋值给buf
    *buf = *(char *) linkpath;
    return 0;
//    int res;
//
//    res = (int) readlink(path, buf, size - 1);
//    if (res == -1)
//        return -errno;
//
//    buf[res] = '\0';
//    return 0;
}

struct xmp_dirp {
    DIR *dp;
    __attribute__((unused)) struct dirent *entry;
    off_t offset;
};

static int xmp_opendir(const char *path, struct fuse_file_info *fi) {
//    int res;
    struct xmp_dirp *d = malloc(sizeof(struct xmp_dirp));
    if (d == NULL)
        return -ENOMEM;

    d->dp = opendir(path);
    if (d->dp == NULL) {
        // 目录不存在，伪装一个虚拟的目录
        d->dp = opendir("/tmp"); // 伪装成/tmp目录
//        res = -errno;
//        free(d);
//        return res;
    }
    d->offset = 0;
    d->entry = NULL;

    fi->fh = (unsigned long) d;
    return 0;
}

static inline struct xmp_dirp *get_dirp(struct fuse_file_info *fi) {
    return (struct xmp_dirp *) (uintptr_t) fi->fh;
}

static int xmp_readdir(__attribute__((unused)) const char *path, void *buf, fuse_fill_dir_t filler,
                       __attribute__((unused)) off_t offset, __attribute__((unused)) struct fuse_file_info *fi) {
    // 只返回"."和".."两个目录项
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    return 0;

//    struct xmp_dirp *d = get_dirp(fi);
//
//    (void) path;
//    if (offset != d->offset) {
//        seekdir(d->dp, offset);
//        d->entry = NULL;
//        d->offset = offset;
//    }
//    while (1) {
//        struct stat st;
//        off_t nextoff;
//
//        if (!d->entry) {
//            d->entry = readdir(d->dp);
//            if (!d->entry)
//                break;
//        }
//
//        memset(&st, 0, sizeof(st));
//        st.st_ino = d->entry->d_ino;
//        st.st_mode = d->entry->d_type << 12;
//        nextoff = telldir(d->dp);
//        nextoff++;
//        if (filler(buf, d->entry->d_name, &st, /*nextoff*/0))
//            break;
//
//        d->entry = NULL;
//        d->offset = nextoff;
//    }
//
//    return 0;
}

static int xmp_releasedir(const char *path, struct fuse_file_info *fi) {
    struct xmp_dirp *d = get_dirp(fi);
    (void) path;
    closedir(d->dp);
    free(d);
    return 0;
}

static int xmp_mknod(__attribute__((unused)) const char *path, __attribute__((unused)) mode_t mode,
                     __attribute__((unused)) dev_t rdev) {
//    int res;

//    if (S_ISFIFO(mode))
//        res = mkfifo(path, mode);
//    else
//        res = mknod(path, mode, rdev);
//    if (res == -1)
//        return -errno;

    return 0;
}

static int xmp_mkdir(__attribute__((unused)) const char *path, __attribute__((unused)) mode_t mode) {
//    int res;

//    res = mkdir(path, mode);
//    if (res == -1)
//        return -errno;

    return 0;
}

static int xmp_unlink(__attribute__((unused)) const char *path) {
//    int res;

//    res = unlink(path);
//    if (res == -1)
//        return -errno;

    return 0;
}

static int xmp_rmdir(__attribute__((unused)) const char *path) {
//    int res;

//    res = rmdir(path);
//    if (res == -1)
//        return -errno;

    return 0;
}

static int xmp_symlink(__attribute__((unused)) const char *from, __attribute__((unused)) const char *to) {
//    int res;

//    res = symlink(from, to);
//    if (res == -1)
//        return -errno;

    return 0;
}

static int xmp_rename(__attribute__((unused)) const char *from, __attribute__((unused)) const char *to) {
//    int res;

//    res = rename(from, to);
//    if (res == -1)
//        return -errno;

    return 0;
}

#ifdef __APPLE__

static int xmp_setvolname(const char *volname) {
    (void) volname;
    return 0;
}

static int xmp_exchange(__attribute__((unused)) const char *path1, __attribute__((unused)) const char *path2,
                        __attribute__((unused)) unsigned long options) {
//    int res;

//    res = exchangedata(path1, path2, options);
//    if (res == -1)
//        return -errno;

    return 0;
}

#endif /* __APPLE__ */

static int xmp_link(__attribute__((unused)) const char *from, __attribute__((unused)) const char *to) {
//    int res;

//    res = link(from, to);
//    if (res == -1)
//        return -errno;

    return 0;
}

#ifdef __APPLE__

static int xmp_fsetattr_x(__attribute__((unused)) const char *path, __attribute__((unused)) struct setattr_x *attr,
                          __attribute__((unused)) struct fuse_file_info *fi) {
    return 0;
//    int res;
//    uid_t uid = -1;
//    gid_t gid = -1;
//
//    if (SETATTR_WANTS_MODE(attr)) {
//        res = lchmod(path, attr->mode);
//        if (res == -1)
//            return -errno;
//    }
//
//    if (SETATTR_WANTS_UID(attr))
//        uid = attr->uid;
//
//    if (SETATTR_WANTS_GID(attr))
//        gid = attr->gid;
//
//    if ((uid != -1) || (gid != -1)) {
//        res = lchown(path, uid, gid);
//        if (res == -1)
//            return -errno;
//    }
//
//    if (SETATTR_WANTS_SIZE(attr)) {
//        if (fi)
//            res = ftruncate((int) fi->fh, attr->size);
//        else
//            res = truncate(path, attr->size);
//        if (res == -1)
//            return -errno;
//    }
//
//    if (SETATTR_WANTS_MODTIME(attr)) {
//        struct timeval tv[2];
//        if (!SETATTR_WANTS_ACCTIME(attr))
//            gettimeofday(&tv[0], NULL);
//        else {
//            tv[0].tv_sec = attr->acctime.tv_sec;
//            tv[0].tv_usec = (int) attr->acctime.tv_nsec / 1000;
//        }
//        tv[1].tv_sec = attr->modtime.tv_sec;
//        tv[1].tv_usec = (int) attr->modtime.tv_nsec / 1000;
//        res = utimes(path, tv);
//        if (res == -1)
//            return -errno;
//    }
//
//    if (SETATTR_WANTS_CRTIME(attr)) {
//        struct attrlist attributes;
//
//        attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
//        attributes.reserved = 0;
//        attributes.commonattr = ATTR_CMN_CRTIME;
//        attributes.dirattr = 0;
//        attributes.fileattr = 0;
//        attributes.forkattr = 0;
//        attributes.volattr = 0;
//
//        res = setattrlist(path, &attributes, &attr->crtime,
//                          sizeof(struct timespec), FSOPT_NOFOLLOW);
//
//        if (res == -1)
//            return -errno;
//    }
//
//    if (SETATTR_WANTS_CHGTIME(attr)) {
//        struct attrlist attributes;
//
//        attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
//        attributes.reserved = 0;
//        attributes.commonattr = ATTR_CMN_CHGTIME;
//        attributes.dirattr = 0;
//        attributes.fileattr = 0;
//        attributes.forkattr = 0;
//        attributes.volattr = 0;
//
//        res = setattrlist(path, &attributes, &attr->chgtime,
//                          sizeof(struct timespec), FSOPT_NOFOLLOW);
//
//        if (res == -1)
//            return -errno;
//    }
//
//    if (SETATTR_WANTS_BKUPTIME(attr)) {
//        struct attrlist attributes;
//
//        attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
//        attributes.reserved = 0;
//        attributes.commonattr = ATTR_CMN_BKUPTIME;
//        attributes.dirattr = 0;
//        attributes.fileattr = 0;
//        attributes.forkattr = 0;
//        attributes.volattr = 0;
//
//        res = setattrlist(path, &attributes, &attr->bkuptime,
//                          sizeof(struct timespec), FSOPT_NOFOLLOW);
//
//        if (res == -1)
//            return -errno;
//    }
//
//    if (SETATTR_WANTS_FLAGS(attr)) {
//        res = chflags(path, attr->flags);
//        if (res == -1)
//            return -errno;
//    }
//
//    return 0;
}

static int xmp_setattr_x(__attribute__((unused)) const char *path, __attribute__((unused)) struct setattr_x *attr) {
    return 0;
//    return xmp_fsetattr_x(path, attr, (struct fuse_file_info *) 0);
}

static int xmp_chflags(const char *path, uint32_t flags) {
    int res;

    res = chflags(path, flags);

    if (res == -1)
        return -errno;

    return 0;
}

static int xmp_getxtimes(const char *path, struct timespec *bkuptime,
                         struct timespec *crtime) {
    int res;
    struct attrlist attributes;

    attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
    attributes.reserved = 0;
    attributes.commonattr = 0;
    attributes.dirattr = 0;
    attributes.fileattr = 0;
    attributes.forkattr = 0;
    attributes.volattr = 0;


    struct xtimeattrbuf {
        uint32_t size;
        struct timespec xtime;
    } __attribute__ ((packed));


    struct xtimeattrbuf buf;

    attributes.commonattr = ATTR_CMN_BKUPTIME;
    res = getattrlist(path, &attributes, &buf, sizeof(buf), FSOPT_NOFOLLOW);
    if (res == 0)
        (void) memcpy(bkuptime, &(buf.xtime), sizeof(struct timespec));
    else
        (void) memset(bkuptime, 0, sizeof(struct timespec));

    attributes.commonattr = ATTR_CMN_CRTIME;
    res = getattrlist(path, &attributes, &buf, sizeof(buf), FSOPT_NOFOLLOW);
    if (res == 0)
        (void) memcpy(crtime, &(buf.xtime), sizeof(struct timespec));
    else
        (void) memset(crtime, 0, sizeof(struct timespec));

    return 0;
}

static int xmp_setbkuptime(const char *path, const struct timespec *bkuptime) {
    int res;

    struct attrlist attributes;

    attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
    attributes.reserved = 0;
    attributes.commonattr = ATTR_CMN_BKUPTIME;
    attributes.dirattr = 0;
    attributes.fileattr = 0;
    attributes.forkattr = 0;
    attributes.volattr = 0;

    res = setattrlist(path, &attributes, (void *) bkuptime,
                      sizeof(struct timespec), FSOPT_NOFOLLOW);

    if (res == -1)
        return -errno;

    return 0;
}

static int xmp_setchgtime(const char *path, const struct timespec *chgtime) {
    int res;

    struct attrlist attributes;

    attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
    attributes.reserved = 0;
    attributes.commonattr = ATTR_CMN_CHGTIME;
    attributes.dirattr = 0;
    attributes.fileattr = 0;
    attributes.forkattr = 0;
    attributes.volattr = 0;

    res = setattrlist(path, &attributes, (void *) chgtime,
                      sizeof(struct timespec), FSOPT_NOFOLLOW);

    if (res == -1)
        return -errno;

    return 0;
}

static int xmp_setcrtime(const char *path, const struct timespec *crtime) {
    int res;

    struct attrlist attributes;

    attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
    attributes.reserved = 0;
    attributes.commonattr = ATTR_CMN_CRTIME;
    attributes.dirattr = 0;
    attributes.fileattr = 0;
    attributes.forkattr = 0;
    attributes.volattr = 0;

    res = setattrlist(path, &attributes, (void *) crtime,
                      sizeof(struct timespec), FSOPT_NOFOLLOW);

    if (res == -1)
        return -errno;

    return 0;
}

#endif /* __APPLE__ */

static int xmp_chmod(const char *path, mode_t mode) {
    int res;

#ifdef __APPLE__
    res = lchmod(path, mode);
#else
    res = chmod(path, mode);
#endif
    if (res == -1)
        return -errno;

    return 0;
}

static int xmp_chown(const char *path, uid_t uid, gid_t gid) {
    int res;

    res = lchown(path, uid, gid);
    if (res == -1)
        return -errno;

    return 0;
}

static int xmp_truncate(const char *path, off_t size) {
    int res;

    res = truncate(path, size);
    if (res == -1)
        return -errno;

    return 0;
}

static int xmp_ftruncate(const char *path, off_t size,
                         struct fuse_file_info *fi) {
    int res;

    (void) path;

    res = ftruncate((int) fi->fh, size);
    if (res == -1)
        return -errno;

    return 0;
}

#ifdef HAVE_UTIMENSAT
static int xmp_utimens(const char *path, const struct timespec ts[2])
{
    int res;

    /* don't use utime/utimes since they follow symlinks */
    res = utimensat(0, path, ts, AT_SYMLINK_NOFOLLOW);
    if (res == -1)
        return -errno;

    return 0;
}
#endif

static int xmp_create(__attribute__((unused)) const char *path, __attribute__((unused)) mode_t mode, struct fuse_file_info *fi) {
    fi->fh = dev_null_fd;
    return 0; // 欺骗性返回成功，但实际上并未创建文件
}

static int xmp_open(__attribute__((unused)) const char *path, struct fuse_file_info *fi) {
//    已知问题: 无法读取有数据的文件,问题不大
    fi->fh = dev_null_fd;
    return 0; // 欺骗性返回成功，但实际上并未打开文件
//    int fd;
//
//    fd = open(path, fi->flags);
//    if (fd == -1)
//        return -errno;
//
//    fi->fh = fd;
//    return 0;
}

static int xmp_read(__attribute__((unused)) const char *path, __attribute__((unused)) char *buf, size_t size,
                    __attribute__((unused)) off_t offset,
                    struct fuse_file_info *fi) {
    fi->fh = dev_null_fd;
    return (int)size; // 欺骗性返回读取的字节数，但实际上并未进行读取
//    int res;
//
//    (void) path;
//    res = (int) pread((int) fi->fh, buf, size, offset);
//    if (res == -1)
//        res = -errno;
//
//    return res;
}

static int xmp_read_buf(const char *path, struct fuse_bufvec **bufp,
                        size_t size, off_t offset, __attribute__((unused)) struct fuse_file_info *fi) {
    struct fuse_bufvec *src;

    (void) path;

    src = malloc(sizeof(struct fuse_bufvec));
    if (src == NULL)
        return -ENOMEM;

    *src = FUSE_BUFVEC_INIT(size);

    src->buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
    src->buf[0].fd = dev_null_fd; // 使用/dev/null的文件描述符 (int) fi->fh;
    src->buf[0].pos = offset;

    *bufp = src;

    return 0;
}

static int xmp_write(__attribute__((unused)) const char *path, __attribute__((unused)) const char *buf, size_t size,
                     __attribute__((unused)) off_t offset, __attribute__((unused)) struct fuse_file_info *fi) {
    return (int)size; // 欺骗性返回写入的字节数，但实际上并未进行写入
//    int res;
//
//    (void) path;
//    res = (int) pwrite((int) fi->fh, buf, size, offset);
//    if (res == -1)
//        res = -errno;
//
//    return res;
}

static int xmp_write_buf(const char *path, struct fuse_bufvec *buf,
                         off_t offset, __attribute__((unused)) struct fuse_file_info *fi) {
    struct fuse_bufvec dst = FUSE_BUFVEC_INIT(fuse_buf_size(buf));
    (void) path;

    dst.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
    dst.buf[0].fd = dev_null_fd; // 使用/dev/null的文件描述符 (int) fi->fh;
    dst.buf[0].pos = offset;

    return (int) fuse_buf_copy(&dst, buf, FUSE_BUF_SPLICE_NONBLOCK);
}

static int xmp_statfs(const char *path, struct statvfs *stbuf) {
    int res;

    res = statvfs(path, stbuf);
    if (res == -1) {
//        返回预设的状态信息
        stbuf->f_bsize = 4096;  //块大小
        stbuf->f_frsize = 4096; //基本块大小
        stbuf->f_blocks = 1000000;    //文件系统数据块总数
        stbuf->f_bfree = 500000;     //可用块数
        stbuf->f_bavail = 500000;    //非超级用户可获取的块数
        stbuf->f_files = 50000;     //文件结点总数
        stbuf->f_ffree = 25000;     //可用文件结点数
        stbuf->f_favail = 25000;    //非超级用户的可用文件结点数
        stbuf->f_fsid = 0;      //文件系统标识
        stbuf->f_flag = 0;      //挂载标志
        stbuf->f_namemax = 255;   //最大文件名长度
//        return -errno;
    }

    return 0;
}

static int xmp_flush(const char *path, struct fuse_file_info *fi) {
    int res;

    (void) path;
    /* This is called from every close on an open file, so call the
       close on the underlying filesystem.	But since flush may be
       called multiple times for an open file, this must not really
       close the file.  This is important if used on a network
       filesystem like NFS which flush the data/metadata on close() */
    res = close(dup((int) fi->fh));
    if (res == -1)
        return -errno;

    return 0;
}

static int xmp_release(const char *path, struct fuse_file_info *fi) {
    (void) path;
    close((int) fi->fh);

    return 0;
}

static int xmp_fsync(__attribute__((unused)) const char *path, __attribute__((unused)) int isdatasync,
                     __attribute__((unused)) struct fuse_file_info *fi) {
//    int res;
//    (void) path;
//
//#ifndef HAVE_FDATASYNC
//    (void) isdatasync;
//#else
//    if (isdatasync)
//        res = fdatasync(fi->fh);
//    else
//#endif
//    res = fsync((int) fi->fh);
//    if (res == -1)
//        return -errno;

    return 0;
}

#if defined(HAVE_POSIX_FALLOCATE) || defined(__APPLE__)

static int xmp_fallocate(__attribute__((unused)) const char *path, int mode,
                         off_t offset, off_t length, struct fuse_file_info *fi) {
#ifdef __APPLE__
    fstore_t fstore;

    if (!(mode & PREALLOCATE))
        return -ENOTSUP;

    fstore.fst_flags = 0;
    if (mode & ALLOCATECONTIG)
        fstore.fst_flags |= F_ALLOCATECONTIG;
    if (mode & ALLOCATEALL)
        fstore.fst_flags |= F_ALLOCATEALL;

    if (mode & ALLOCATEFROMPEOF)
        fstore.fst_posmode = F_PEOFPOSMODE;
    else if (mode & ALLOCATEFROMVOL)
        fstore.fst_posmode = F_VOLPOSMODE;

    fstore.fst_offset = offset;
    fstore.fst_length = length;

    if (fcntl((int) fi->fh, F_PREALLOCATE, &fstore) == -1)
        return -errno;
    else
        return 0;
#else
    (void) path;

    if (mode)
        return -EOPNOTSUPP;

    return -posix_fallocate(fi->fh, offset, length);
#endif
}

#endif

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
#ifdef __APPLE__

static int xmp_setxattr(const char *path, const char *name, const char *value,
                        size_t size, int flags, uint32_t position)
#else
static int xmp_setxattr(const char *path, const char *name, const char *value,
            size_t size, int flags)
#endif
{
#ifdef __APPLE__
    int res;
    if (!strncmp(name, XATTR_APPLE_PREFIX, sizeof(XATTR_APPLE_PREFIX) - 1)) {
        flags &= ~(XATTR_NOSECURITY);
    }
    if (!strcmp(name, A_KAUTH_FILESEC_XATTR)) {
        char new_name[MAXPATHLEN];
        memcpy(new_name, A_KAUTH_FILESEC_XATTR, sizeof(A_KAUTH_FILESEC_XATTR));
        memcpy(new_name, G_PREFIX, sizeof(G_PREFIX) - 1);
        res = setxattr(path, new_name, value, size, position, flags);
    } else {
        res = setxattr(path, name, value, size, position, flags);
    }
#else
    int res = lsetxattr(path, name, value, size, flags);
#endif
    if (res == -1)
        return -errno;
    return 0;
}

#ifdef __APPLE__

static int xmp_getxattr(const char *path, const char *name, char *value,
                        size_t size, uint32_t position)
#else
static int xmp_getxattr(const char *path, const char *name, char *value,
            size_t size)
#endif
{
#ifdef __APPLE__
    int res;
    if (strcmp(name, A_KAUTH_FILESEC_XATTR) == 0) {
        char new_name[MAXPATHLEN];
        memcpy(new_name, A_KAUTH_FILESEC_XATTR, sizeof(A_KAUTH_FILESEC_XATTR));
        memcpy(new_name, G_PREFIX, sizeof(G_PREFIX) - 1);
        res = (int) getxattr(path, new_name, value, size, position, XATTR_NOFOLLOW);
    } else {
        res = (int) getxattr(path, name, value, size, position, XATTR_NOFOLLOW);
    }
#else
    int res = lgetxattr(path, name, value, size);
#endif
    if (res == -1) {
        // 文件不存在，返回预设的文件状态信息
        if (strcmp(path, "/") == 0) { // 貌似不需要
            // 返回预设的文件状态信息
            memcpy(value, &virtual_file_stat, sizeof(struct stat));
            return sizeof(struct stat);
        }
        strncpy(value, "default_value", size);
        return strlen("default_value");
//        return -errno;
    }

    return res;
}

static int xmp_listxattr(const char *path, char *list, size_t size) {
#ifdef __APPLE__
    ssize_t res = listxattr(path, list, size, XATTR_NOFOLLOW);
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
        } else {
            /*
            ssize_t res2 = getxattr(path, G_KAUTH_FILESEC_XATTR, NULL, 0, 0,
                        XATTR_NOFOLLOW);
            if (res2 >= 0) {
                res -= sizeof(G_KAUTH_FILESEC_XATTR);
            }
            */
        }
    }
#else
    int res = llistxattr(path, list, size);
#endif
    if (res == -1) {
        // 文件不存在，返回预设的文件状态信息
        const char *default_attr = "default_attr";
        if (size > strlen(default_attr)) {
            strcpy(list, default_attr);
        }
        return (int)strlen(default_attr);
//        return -errno;
    }

    return (int) res;
}

static int xmp_removexattr(const char *path, const char *name) {
#ifdef __APPLE__
    int res;
    if (strcmp(name, A_KAUTH_FILESEC_XATTR) == 0) {
        char new_name[MAXPATHLEN];
        memcpy(new_name, A_KAUTH_FILESEC_XATTR, sizeof(A_KAUTH_FILESEC_XATTR));
        memcpy(new_name, G_PREFIX, sizeof(G_PREFIX) - 1);
        res = removexattr(path, new_name, XATTR_NOFOLLOW);
    } else {
        res = removexattr(path, name, XATTR_NOFOLLOW);
    }
#else
    int res = lremovexattr(path, name);
#endif
    if (res == -1)
        return -errno;
    return 0;
}

#endif /* HAVE_SETXATTR */

#ifndef __APPLE__
static int xmp_lock(const char *path, struct fuse_file_info *fi, int cmd,
            struct flock *lock)
{
    (void) path;

    return ulockmgr_op(fi->fh, cmd, lock, &fi->lock_owner,
               sizeof(fi->lock_owner));
}
#endif

void *
xmp_init(struct fuse_conn_info *conn) {
#ifdef __APPLE__
    FUSE_ENABLE_SETVOLNAME(conn);
    FUSE_ENABLE_XTIMES(conn);
#endif
    return NULL;
}

void
xmp_destroy(__attribute__((unused)) void *userdata) {
//    closedir(dev_null_dir);
    close(dev_null_fd);
}

#ifndef __APPLE__
static int xmp_flock(const char *path, struct fuse_file_info *fi, int op)
{
//    int res;
//    (void) path;

//    res = flock(fi->fh, op);
//    if (res == -1)
//        return -errno;

    return 0;
}
#endif

static struct fuse_operations xmp_oper = {
        .init        = xmp_init,
        .destroy    = xmp_destroy,
        .getattr    = xmp_getattr,
        .fgetattr    = xmp_fgetattr,
#ifndef __APPLE__
        .access		= xmp_access,
#endif
        .readlink    = xmp_readlink,
        .opendir    = xmp_opendir,
        .readdir    = xmp_readdir,
        .releasedir    = xmp_releasedir,
        .mknod        = xmp_mknod,
        .mkdir        = xmp_mkdir,
        .symlink    = xmp_symlink,
        .unlink        = xmp_unlink,
        .rmdir        = xmp_rmdir,
        .rename        = xmp_rename,
        .link        = xmp_link,
        .chmod        = xmp_chmod,
        .chown        = xmp_chown,
        .truncate    = xmp_truncate,
        .ftruncate    = xmp_ftruncate,
#ifdef HAVE_UTIMENSAT
        .utimens	= xmp_utimens,
#endif
        .create        = xmp_create,
        .open        = xmp_open,
        .read        = xmp_read,
        .read_buf    = xmp_read_buf,
        .write        = xmp_write,
        .write_buf    = xmp_write_buf,
        .statfs        = xmp_statfs,
        .flush        = xmp_flush,
        .release    = xmp_release,
        .fsync        = xmp_fsync,
#if defined(HAVE_POSIX_FALLOCATE) || defined(__APPLE__)
        .fallocate    = xmp_fallocate,
#endif
#ifdef HAVE_SETXATTR
        .setxattr    = xmp_setxattr,
        .getxattr    = xmp_getxattr,
        .listxattr    = xmp_listxattr,
        .removexattr    = xmp_removexattr,
#endif
#ifndef __APPLE__
        .lock		= xmp_lock,
    .flock		= xmp_flock,
#endif
#ifdef __APPLE__
        .setvolname    = xmp_setvolname,
        .exchange    = xmp_exchange,
        .getxtimes    = xmp_getxtimes,
        .setbkuptime    = xmp_setbkuptime,
        .setchgtime    = xmp_setchgtime,
        .setcrtime    = xmp_setcrtime,
        .chflags    = xmp_chflags,
        .setattr_x    = xmp_setattr_x,
        .fsetattr_x    = xmp_fsetattr_x,
#endif

        .flag_nullpath_ok = 1,
#if HAVE_UTIMENSAT
        .flag_utime_omit_ok = 1,
#endif
};

int main(int argc, char *argv[]) {
    fprintf(stderr, "编译使用fuse版本: %d\n", FUSE_USE_VERSION);
    fprintf(stderr, "本地安装fuse版本: %d\n", FUSE_VERSION);

    // 检查命令行参数数量
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <挂载路径>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    dev_null_fd = open("/dev/null", O_RDWR);
    if (dev_null_fd == -1) {
        fprintf(stderr, "Cannot open /dev/null: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // 初始化虚拟文件的状态信息
    memset(&virtual_file_stat, 0, sizeof(struct stat));
    virtual_file_stat.st_mode = S_IFREG | 0644; // 设置文件类型和权限
    virtual_file_stat.st_nlink = 1; // 设置硬链接数
    virtual_file_stat.st_size = 0; // 设置文件大小
    virtual_file_stat.st_blocks = 0; // 设置文件块数
    virtual_file_stat.st_atime = virtual_file_stat.st_mtime = virtual_file_stat.st_ctime = time(NULL); // 设置文件时间

//    int fd = open(argv[1], O_RDWR);
//    dev_null_dir = fdopendir(fd);
//    if (dev_null_dir == NULL) {
//        fprintf(stderr, "Cannot open directory stream for %s: %s\n", argv[1], strerror(errno));
//        exit(EXIT_FAILURE);
//    }

    umask(0);
    return fuse_main(argc, argv, &xmp_oper, NULL);
}


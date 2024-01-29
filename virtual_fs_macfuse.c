// 代码参考: https://github.com/osxfuse/filesystems

//#include <AvailabilityMacros.h>

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
//#include <sys/time.h>
#include <sys/xattr.h>
//#include <sys/attr.h>
//#include <sys/param.h>
//#include <sys/vnode.h>

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

// 全局变量，用于存储/dev/null的文件描述符
static int dev_null_fd;

// 全局变量，用于存储预设的目录流
//static DIR *dev_null_dir;

// 初始化函数，初始化FUSE文件系统连接信息,打开/dev/null并保存其文件描述符和目录流
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

// 销毁函数，关闭/dev/null的文件描述符和目录流
// 销毁FUSE文件系统连接信息
void loopback_destroy(__attribute__((unused)) void *userdata) {
//    closedir(dev_null_dir);
    close(dev_null_fd);
}

// 函数用于判断路径是否指向一个目录
static int is_directory(const char *path) {
//    size_t len = strlen(path);
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

// 全局变量，用于存储预设的符号链接路径
static const char *linkpath = "/dev/null";

// 获取文件或目录的属性
static int loopback_getattr(const char *path, struct stat *stbuf) {
    // 清空stbuf结构体
    memset(stbuf, 0, sizeof(struct stat));
    // 设置文件大小为1
    stbuf->st_size = 1;
    // 设置文件块大小
    stbuf->st_blksize = 0;
    // 设置文件权限为可读可写
    stbuf->st_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;

    // 如果路径指向一个目录，设置文件类型为目录
    if (is_directory(path)) {
        stbuf->st_mode |= S_IFDIR;
    } else {
        // 否则，设置文件类型为普通文件
        stbuf->st_mode |= S_IFREG;
    }

    return 0;
}

// 获取文件或目录的属性（使用文件句柄）
static int loopback_fgetattr(__attribute__((unused)) const char *path, struct stat *stbuf,
                             __attribute__((unused)) struct fuse_file_info *fi) {
    // 清空stbuf结构体
    memset(stbuf, 0, sizeof(struct stat));
    // 设置文件大小为1
    stbuf->st_size = 1;
    // 设置文件块大小
    stbuf->st_blksize = 0;
    // 设置文件权限为可读可写
    stbuf->st_mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;

    // 如果路径指向一个目录，设置文件类型为目录
    if (is_directory(path)) {
        stbuf->st_mode |= S_IFDIR;
    } else {
        // 否则，设置文件类型为普通文件
        stbuf->st_mode |= S_IFREG;
    }

    return 0;
}

// 读取符号链接
static int loopback_readlink(const char *path, char *buf, size_t size) {
    // 直接将预设的符号链接路径的地址赋值给buf
    *buf = *(char *) linkpath;

    return 0;
}

// 目录流结构
struct loopback_dirp {
    DIR *dp;
    struct dirent *entry;
    off_t offset;
};

// 打开目录
static int loopback_opendir(__attribute__((unused)) const char *path, struct fuse_file_info *fi) {
    int res;

    // 分配目录流结构的内存
    struct loopback_dirp *d = malloc(sizeof(struct loopback_dirp));
    if (d == NULL) {
        // 若内存分配失败，返回内存不足错误码
        return -ENOMEM;
    }

    // 直接将预设的目录流赋值给d->dp
    d->dp = opendir(path);

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
                            off_t offset, struct fuse_file_info *fi)
{
    struct loopback_dirp *d = get_dirp(fi);

    (void)path;

    if (offset == 0) {
        rewinddir(d->dp);
        d->entry = NULL;
        d->offset = 0;
    } else if (offset != d->offset) {
        seekdir(d->dp, offset);
        d->entry = NULL;
        d->offset = offset;
    }

    while (1) {
        struct stat st;
        off_t nextoff;

        if (!d->entry) {
            d->entry = readdir(d->dp);
            if (!d->entry) {
                break;
            }
        }

        memset(&st, 0, sizeof(st));
        st.st_ino = d->entry->d_ino;
        st.st_mode = d->entry->d_type << 12;
        nextoff = telldir(d->dp);
        if (filler(buf, d->entry->d_name, &st, nextoff)) {
            break;
        }

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
static int loopback_mknod(__attribute__((unused)) const char *path, __attribute__((unused)) mode_t mode,
                          __attribute__((unused)) dev_t rdev) {
    // 不进行实际的节点创建操作，直接返回成功
    return 0;
}

// 创建目录
static int loopback_mkdir(__attribute__((unused)) const char *path, __attribute__((unused)) mode_t mode) {
    // 不进行实际的目录创建操作，直接返回成功
    return 0;
}

// 删除文件
static int loopback_unlink(__attribute__((unused)) const char *path) {
    // 不进行实际的文件删除操作，直接返回成功
    return 0;
}

// 删除目录
static int loopback_rmdir(__attribute__((unused)) const char *path) {
    // 不进行实际的目录删除操作，直接返回成功
    return 0;
}

// 创建符号链接
static int loopback_symlink(const char *from, const char *to) {
    // 不进行实际的符号链接创建操作，直接返回成功
    return 0;
}

// 重命名文件或目录
static int loopback_rename(__attribute__((unused)) const char *from, __attribute__((unused)) const char *to) {
    // 不进行实际的重命名操作，直接返回成功
    return 0;
}

// 交换两个文件或目录的内容
static int loopback_exchange(__attribute__((unused)) const char *path1, __attribute__((unused)) const char *path2,
                             __attribute__((unused)) unsigned long options) {
    // 不进行实际的文件或目录交换操作，直接返回成功
    return 0;
}

// 创建硬链接
static int loopback_link(const char *from, const char *to) {
    // 不进行实际的硬链接创建操作，直接返回成功
    return 0;
}

// 函数用于设置文件属性，与 fsetattrlist 相关
static int loopback_fsetattr_x(__attribute__((unused)) const char *path, __attribute__((unused)) struct setattr_x *attr,
                               __attribute__((unused)) struct fuse_file_info *fi) {
    // 不进行实际的文件属性设置操作，直接返回成功
    return 0;
}

// 函数用于设置文件属性，支持 macOS 下的 extended attributes
static int
loopback_setattr_x(__attribute__((unused)) const char *path, __attribute__((unused)) struct setattr_x *attr) {
    // 不进行实际的文件属性设置操作，直接返回成功
    return 0;
}

// 函数用于获取文件的备份时间和创建时间
static int loopback_getxtimes(__attribute__((unused)) const char *path, struct timespec *bkuptime,
                              struct timespec *crtime) {
    // 不进行实际的时间获取操作，直接返回成功
    bkuptime->tv_sec = 0;
    bkuptime->tv_nsec = 0;
    crtime->tv_sec = 0;
    crtime->tv_nsec = 0;
    return 0;
}

// 函数用于创建文件并打开，返回文件描述符，若失败则返回相应错误码
static int loopback_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    fi->fh = dev_null_fd;
    return 0;
}

// 打开文件函数，直接返回/dev/null的文件描述符
static int loopback_open(const char *path, struct fuse_file_info *fi) {
    fi->fh = dev_null_fd;
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
static int loopback_statfs(__attribute__((unused)) const char *path, struct statvfs *stbuf) {
    // 不进行实际的文件系统统计信息获取操作，直接返回成功
    stbuf->f_bsize = 1024;
    stbuf->f_frsize = 1024;
    stbuf->f_blocks = 1024 * 1024;  // 假设有1GB的空间
    stbuf->f_bfree = 1024 * 1024;   // 假设1GB的空间都是空闲的
    stbuf->f_bavail = 1024 * 1024;  // 假设1GB的空间都是可用的
    stbuf->f_files = 1024;          // 假设有1024个文件
    stbuf->f_ffree = 1024;          // 假设1024个文件都是空闲的
    stbuf->f_favail = 1024;         // 假设1024个文件都是可用的
    stbuf->f_fsid = 0;              // 文件系统的ID
    stbuf->f_flag = 0;              // 挂载标志
    stbuf->f_namemax = 1024;        // 文件名的最大长度
    return 0;
}

// 函数用于刷新文件状态，将文件数据同步到磁盘
static int loopback_flush(__attribute__((unused)) const char *path, __attribute__((unused)) struct fuse_file_info *fi) {
    // 不进行实际的文件状态刷新操作，直接返回成功
    return 0;
}

// 函数用于释放文件资源
static int
loopback_release(__attribute__((unused)) const char *path, __attribute__((unused)) struct fuse_file_info *fi) {
    // 不进行实际的文件资源释放操作，直接返回成功
    return 0;
}

// 函数用于同步文件数据到磁盘
static int loopback_fsync(const char *path, int isdatasync, struct fuse_file_info *fi) {
    // 不进行实际的文件数据同步操作，直接返回成功
    return 0;
}

// 函数用于设置扩展属性，支持 macOS 下的 extended attributes
static int loopback_setxattr(__attribute__((unused)) const char *path, __attribute__((unused)) const char *name,
                             __attribute__((unused)) const char *value,
                             __attribute__((unused)) size_t size, __attribute__((unused)) int flags,
                             __attribute__((unused)) uint32_t position) {
    // 不进行实际的扩展属性设置操作，直接返回成功
    return 0;
}

// 函数用于获取扩展属性，支持 macOS 下的 extended attributes
static int loopback_getxattr(__attribute__((unused)) const char *path, __attribute__((unused)) const char *name,
                             __attribute__((unused)) char *value, __attribute__((unused)) size_t size,
                             __attribute__((unused)) uint32_t position) {
    // 不进行实际的扩展属性获取操作，直接返回成功
    return 0;
}

// 函数用于获取指定路径的扩展属性列表
static int loopback_listxattr(const char *path, char *list, size_t size)
{
    ssize_t res = listxattr(path, list, size, XATTR_NOFOLLOW);
    if (res > 0) {
        if (list) {
            size_t len = 0;
            char *curr = list;
            do {
                size_t thislen = strlen(curr) + 1;
                if (strcmp(curr, G_KAUTH_FILESEC_XATTR) == 0) {
                    memmove(curr, curr + thislen, res - len - thislen);
                    res -= thislen;
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

    if (res == -1) {
        return -errno;
    }

    return res;
}

// 函数用于移除指定路径的扩展属性
static int loopback_removexattr(const char *path, const char *name) {
    // 不进行实际的扩展属性移除操作，直接返回成功
    return 0;
}

// 函数用于预分配或空洞创建文件空间
static int loopback_fallocate(__attribute__((unused)) const char *path, __attribute__((unused)) int mode,
                              __attribute__((unused)) off_t offset, __attribute__((unused)) off_t length,
                              __attribute__((unused)) struct fuse_file_info *fi) {
    // 不进行实际的文件空间预分配或空洞创建操作，直接返回成功
    return 0;
}

// 函数用于设置文件系统的卷标名称，返回0表示成功
static int loopback_setvolname(__attribute__((unused)) const char *name) {
    return 0;
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
        .fsetattr_x  = loopback_fsetattr_x,
        .fallocate   = loopback_fallocate,
        .setvolname  = loopback_setvolname,
        .flag_nullpath_ok = 1,
        .flag_nopath = 1,
};

// 定义FUSE文件系统的选项数组
static const struct fuse_opt loopback_opts[] = {
        {"case_insensitive", offsetof(struct loopback, case_insensitive), 1},
        FUSE_OPT_END
};

// 主函数，用于启动FUSE文件系统
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

//    dev_null_dir = fdopendir(dev_null_fd);
//    if (dev_null_dir == NULL) {
//        fprintf(stderr, "Cannot open directory stream for /dev/null: %s\n", strerror(errno));
//        exit(EXIT_FAILURE);
//    }

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
    return res;
}


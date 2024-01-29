// 代码参考: https://github.com/macos-fuse-t/libfuse/tree/master/example

#define FUSE_USE_VERSION 29

#define HAVE_SETXATTR    1

#ifdef __APPLE__
//#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif

#include <fuse.h>

#ifndef __APPLE__
#include <ulockmgr.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>

#ifdef HAVE_SETXATTR


#endif
#ifndef __APPLE__
#include <sys/file.h>
#endif

#ifdef __APPLE__

#if defined(_POSIX_C_SOURCE)
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned int   u_int;
typedef unsigned long  u_long;
#endif

#endif /* __APPLE__ */

#include <stdarg.h>

// 全局变量，用于存储/dev/null的文件描述符
static int dev_null_fd;

// 全局变量，用于存储预设的符号链接路径
static const char *linkpath = "/dev/null";

// 全局变量保存虚拟文件的状态信息
struct stat virtual_file_stat;

// 全局变量，用于记录文件是否被访问,默认为true
static bool isfileAccessed = true;

// 哈希环的长度
enum {
    HASH_RING_SIZE = 10,    // 哈希环的长度
    HASH_MULTIPLIER = 5     // 哈希环的乘数2^5 = 32
};

// 哈希环节点
typedef struct {
    char *path;
} HashNode;

// 哈希环
HashNode hashRing[HASH_RING_SIZE];

// 计算路径的哈希值
static unsigned int hashFunction(const char *path) {
    unsigned int hash = 0;
    while (*path) {
        hash = (hash << HASH_MULTIPLIER) + *path++;
    }
    return hash % HASH_RING_SIZE;
}

// 检查路径是否存在于哈希环中
static bool pathExists(const char *path) {
    unsigned int index = hashFunction(path);
    return (hashRing[index].path != NULL) && (strcmp(hashRing[index].path, path) == 0);
}

// 将路径写入哈希环中，覆盖已存在的路径
static void writePath(const char *path) {
    unsigned int index = hashFunction(path);
    if (hashRing[index].path != NULL) {
        // 覆盖已存在的路径
        free(hashRing[index].path);
    }
    // 分配内存并复制路径
    hashRing[index].path = strdup(path);
}

// 释放哈希环的内存
static void freeHashRing() {
    for (int i = 0; i < HASH_RING_SIZE; i++) {
        if (hashRing[i].path != NULL) {
            free(hashRing[i].path);
        }
    }
}

// 字符串前缀匹配函数
unsigned short int startsWith(const char *str, const char *prefix) {
    while (*prefix) {
        if (*prefix++ != *str++) {
            return 0;  // 字符不匹配，返回假
        }
    }
    return 1;  // 字符匹配，返回真
}

// 判断字符串是否以指定后缀结尾
unsigned short int endsWith(const char *str, int num_suffix, ...) {
    size_t str_len = strlen(str);
    va_list suffix_list;
    va_start(suffix_list, num_suffix);

    for (int i = 0; i < num_suffix; i++) {
        const char *suffix = va_arg(suffix_list, const char *);
        size_t suffix_len = strlen(suffix);

        // 确保字符串长度大于后缀长度，否则无法以后缀结尾
        if (str_len < suffix_len) {
            va_end(suffix_list);
            return 0;
        }

        // 使用指针遍历
        const char *end_of_str = str + (str_len - suffix_len);
        while (*suffix != '\0') {
            if (*end_of_str++ != *suffix++) {
                va_end(suffix_list);
                return 0; // 字符不匹配，不是以后缀结尾
            }
        }
    }

    va_end(suffix_list);
    return 1; // 字符匹配，以后缀结尾

}

// 函数用于判断路径是否指向一个目录
static unsigned short int is_directory(const char *path) {
    // 从路径中获取文件名
    const char *filename = strrchr(path, '/');

    // 如果找到了文件名，则进行判断
    if (filename != NULL) {
        // 获取文件名中的后缀
        const char *suffix = strrchr(filename, '.');

        // 如果找到了后缀，并且后缀中第一个不是数字，则认为是文件
        if (suffix != NULL) {
            // debug,打印信息到文件
//            FILE *fp = fopen("/tmp/debug.log", "a");
//            fprintf(fp, "is_directory path: %s\n", path);
//            fprintf(fp, "filename: %s\n", filename);
//            fprintf(fp, "suffix: %s\n", suffix);
//            // 关闭文件
//            fclose(fp);

            suffix++;  // 移动到后缀的第一个字符
            const unsigned short int JetBrain_path = startsWith((path + 1), "JetBrains");
            if ((*suffix < '0' || *suffix > '9') || (JetBrain_path &&
                                                     ((suffix[-4] == 'c') && (suffix[-3] == 's') &&
                                                      (suffix[-2] == 'v')))) { // 匹配JB中.csv.0 文件
                // 针对jetbrains的文件进行特殊处理
                if (JetBrain_path &&
                    !endsWith(filename, 2, ".log", ".txt")) {
                    filename++;  // 移动到文件名的第一个字符
                    if (!pathExists(filename)) {
                        // 哈希环中不存在该文件名
                        writePath(filename);
                        isfileAccessed = false;
                    }
                }
                return 0;
            }
        }
    }

    return 1;
}

static int xmp_getattr(const char *path, struct stat *stbuf) {
//     debug,打印信息到文件
//    FILE *fp = fopen("/tmp/debug.log", "a");
//    fprintf(fp, "xmp_getattr path: %s\n", path);
//    fclose(fp);

    if (!(*(path + 1)) || is_directory(path)) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    } else {
        if (!isfileAccessed) {
            // 初次访问文件，返回文件不存在
            isfileAccessed = true; // 重置文件访问标志
            return -ENOENT;
        }
        stbuf->st_mode = S_IFREG | 0644;
        stbuf->st_nlink = 1;
        stbuf->st_size = 0;
    }

    return 0;
}

static int xmp_fgetattr(const char *path, struct stat *stbuf,
                        __attribute__((unused)) struct fuse_file_info *fi) {
    const char *path_ = path + 1;

    if (!(*path_) || is_directory(path_)) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    } else {
        stbuf->st_mode = S_IFREG | 0644;
        stbuf->st_nlink = 1;
        stbuf->st_size = 0;
    }
    return 0;
}

static int xmp_access(__attribute__((unused)) const char *path, __attribute__((unused)) int mask) {
    return 0;
}

static int xmp_readlink(__attribute__((unused)) const char *path, char *buf, __attribute__((unused)) size_t size) {
    // 直接将预设的符号链接路径的地址赋值给buf
    *buf = *(char *) linkpath;
    return 0;
}

struct xmp_dirp {
    DIR *dp;
    __attribute__((unused)) struct dirent *entry;
    __attribute__((unused)) off_t offset;
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
    }
    d->offset = 0;
    d->entry = NULL;

    fi->fh = (unsigned long) d;
    return 0;
}

__attribute__((unused)) static inline struct xmp_dirp *get_dirp(struct fuse_file_info *fi) {
    return (struct xmp_dirp *) (uintptr_t) fi->fh;
}

static int xmp_readdir(__attribute__((unused)) const char *path, void *buf, fuse_fill_dir_t filler,
                       __attribute__((unused)) off_t offset, __attribute__((unused)) struct fuse_file_info *fi) {
    // 只返回"."和".."两个目录项
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    return 0;
}

static int xmp_releasedir(__attribute__((unused)) const char *path, __attribute__((unused)) struct fuse_file_info *fi) {
    return 0;
}

static int xmp_mknod(__attribute__((unused)) const char *path, __attribute__((unused)) mode_t mode,
                     __attribute__((unused)) dev_t rdev) {
    return 0;
}

static int xmp_mkdir(__attribute__((unused)) const char *path, __attribute__((unused)) mode_t mode) {
    return 0;
}

static int xmp_unlink(__attribute__((unused)) const char *path) {
    return 0;
}

static int xmp_rmdir(__attribute__((unused)) const char *path) {
    return 0;
}

static int xmp_symlink(__attribute__((unused)) const char *from, __attribute__((unused)) const char *to) {
    return 0;
}

static int xmp_rename(__attribute__((unused)) const char *from, __attribute__((unused)) const char *to) {
    return 0;
}

#ifdef __APPLE__

static int xmp_setvolname(const char *volname) {
    (void) volname;
    return 0;
}

static int xmp_exchange(__attribute__((unused)) const char *path1, __attribute__((unused)) const char *path2,
                        __attribute__((unused)) unsigned long options) {
    return 0;
}

#endif /* __APPLE__ */

static int xmp_link(__attribute__((unused)) const char *from, __attribute__((unused)) const char *to) {
    return 0;
}

#ifdef __APPLE__

static int xmp_fsetattr_x(__attribute__((unused)) const char *path, __attribute__((unused)) struct setattr_x *attr,
                          __attribute__((unused)) struct fuse_file_info *fi) {
    return 0;
}

static int xmp_setattr_x(__attribute__((unused)) const char *path, __attribute__((unused)) struct setattr_x *attr) {
    return 0;
}

static int xmp_chflags(__attribute__((unused)) const char *path, __attribute__((unused)) uint32_t flags) {
    return 0;
}

static int xmp_getxtimes(__attribute__((unused)) const char *path, struct timespec *bkuptime,
                         struct timespec *crtime) {
    // 预设的备份时间和创建时间
    struct timespec preset_time;
    preset_time.tv_sec = 1706716800;  // 2024-02-01 00:00:00 UTC
    preset_time.tv_nsec = 0;

    // 设置备份时间和创建时间为预设的时间
    *bkuptime = preset_time;
    *crtime = preset_time;

    return 0;
}

static int
xmp_setbkuptime(__attribute__((unused)) const char *path, __attribute__((unused)) const struct timespec *bkuptime) {
    return 0;
}

static int
xmp_setchgtime(__attribute__((unused)) const char *path, __attribute__((unused)) const struct timespec *chgtime) {
    return 0;
}

static int
xmp_setcrtime(__attribute__((unused)) const char *path, __attribute__((unused)) const struct timespec *crtime) {
    return 0;
}

#endif /* __APPLE__ */

static int xmp_chmod(__attribute__((unused)) const char *path, __attribute__((unused)) mode_t mode) {
    return 0;
}

static int xmp_chown(__attribute__((unused)) const char *path, __attribute__((unused)) uid_t uid,
                     __attribute__((unused)) gid_t gid) {
    return 0;
}

static int xmp_truncate(__attribute__((unused)) const char *path, __attribute__((unused)) off_t size) {
    return 0;
}

static int xmp_ftruncate(__attribute__((unused)) const char *path, __attribute__((unused)) off_t size,
                         __attribute__((unused)) struct fuse_file_info *fi) {
    return 0;
}

#ifdef HAVE_UTIMENSAT
static int xmp_utimens(const char *path, const struct timespec ts[2])
{
    return 0;
}
#endif

static int
xmp_create(__attribute__((unused)) const char *path, __attribute__((unused)) mode_t mode, struct fuse_file_info *fi) {
    fi->fh = dev_null_fd;
    return 0; // 欺骗性返回成功，但实际上并未创建文件
}

static int xmp_open(__attribute__((unused)) const char *path, struct fuse_file_info *fi) {
//    已知问题: 无法读取有数据的文件,问题不大
    fi->fh = dev_null_fd;
    return 0; // 欺骗性返回成功，但实际上并未打开文件
}

static int xmp_read(__attribute__((unused)) const char *path, __attribute__((unused)) char *buf,
                    __attribute__((unused)) size_t size,
                    __attribute__((unused)) off_t offset,
                    __attribute__((unused)) struct fuse_file_info *fi) {
    return 0; // 欺骗性返回读取的字节数，但实际上并未进行读取
}

static int xmp_read_buf(__attribute__((unused)) const char *path, __attribute__((unused)) struct fuse_bufvec **bufp,
                        __attribute__((unused)) size_t size, __attribute__((unused)) off_t offset,
                        __attribute__((unused)) struct fuse_file_info *fi) {
    return 0;
}

static int xmp_write(__attribute__((unused)) const char *path, __attribute__((unused)) const char *buf, size_t size,
                     __attribute__((unused)) off_t offset, __attribute__((unused)) struct fuse_file_info *fi) {
    return (int) size; // 欺骗性返回写入的字节数，但实际上并未进行写入
}

static int xmp_write_buf(__attribute__((unused)) const char *path, __attribute__((unused)) struct fuse_bufvec *buf,
                         __attribute__((unused)) off_t offset, __attribute__((unused)) struct fuse_file_info *fi) {
    return 0;
}

static int xmp_statfs(__attribute__((unused)) const char *path, struct statvfs *stbuf) {
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

    return 0;
}

static int xmp_flush(__attribute__((unused)) const char *path, __attribute__((unused)) struct fuse_file_info *fi) {
    return 0;
}

static int xmp_release(__attribute__((unused)) const char *path, __attribute__((unused)) struct fuse_file_info *fi) {
    return 0;
}

static int xmp_fsync(__attribute__((unused)) const char *path, __attribute__((unused)) int isdatasync,
                     __attribute__((unused)) struct fuse_file_info *fi) {
    return 0;
}

#if defined(HAVE_POSIX_FALLOCATE) || defined(__APPLE__)

static int xmp_fallocate(__attribute__((unused)) const char *path, __attribute__((unused)) int mode,
                         __attribute__((unused)) off_t offset, __attribute__((unused)) off_t length,
                         __attribute__((unused)) struct fuse_file_info *fi) {
    return 0;
}

#endif

#ifdef HAVE_SETXATTR

static int xmp_setxattr(__attribute__((unused)) const char *path, __attribute__((unused)) const char *name,
                        __attribute__((unused)) const char *value,
                        __attribute__((unused)) size_t size, __attribute__((unused)) int flags,
                        __attribute__((unused)) uint32_t position) {
    return 0;
}


static int xmp_getxattr(__attribute__((unused)) const char *path, __attribute__((unused)) const char *name, char *value,
                        __attribute__((unused)) size_t size, __attribute__((unused)) uint32_t position)
{
    // 预设的数据
    const char *preset_data = "";
    size_t preset_data_size = strlen(preset_data) + 1;  // 加1是为了包含字符串结束符'\0'

    // 将预设的数据复制到缓冲区
    memcpy(value, preset_data, preset_data_size);

    return 0;
}

static int xmp_listxattr(__attribute__((unused)) const char *path, char *list, size_t size) {
    return 0;
}

static int xmp_removexattr(__attribute__((unused)) const char *path, __attribute__((unused)) const char *name) {
    return 0;
}

#endif /* HAVE_SETXATTR */

#ifndef __APPLE__
static int xmp_lock(const char *path, struct fuse_file_info *fi, int cmd,
            struct flock *lock)
{
    return 0;
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
    // 释放哈希环的内存
    freeHashRing();
    close(dev_null_fd);
}

#ifndef __APPLE__
static int xmp_flock(const char *path, struct fuse_file_info *fi, int op)
{
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
    if (argc == 1) {
        fprintf(stderr, "用法: %s [-delete] <挂载路径>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // 检查是否包含 "-delete" 参数
    if (argc > 2 && strcmp(argv[1], "-delete") == 0) {
        // 计算需要的内存大小，包括命令字符串和终结符 '\0'
        size_t command_size = strlen(argv[2]) + 11;  // "rm -rf '‘" 长度为 9，额外留两个字符给目录路径和终结符 '\0'
        // 检查内存大小是否超过限制
        if (command_size > 1 * 1024 * 1024 * 1024) {
            fprintf(stderr, "Memory allocation size exceeds limit (1GB)\n");
            return 1;
        }
        // 分配足够的内存
        char *command = (char *) malloc(command_size);

        if (command == NULL) {
            fprintf(stderr, "分配内存大小: %zu 失败\n", command_size);
            perror("Error allocating memory");
            return 1;
        }
        // 构建删除命令并执行
        snprintf(command, command_size, "rm -rf '%s'", argv[2]);
        fprintf(stderr, "将要执行命令: %s\n", command);

        // 提示用户确认
        printf("Are you sure you want to execute this command %s? Press y key to confirm...\n", command);
        printf("是否执行这条命令%s?按y键确认...\n", command);
        char input = (char) getchar();  // 等待用户按下任意键
        if (input != 'y') {
            fprintf(stderr, "用户取消执行命令\n");
            return 1;
        }

        system(command);
        // 释放动态分配的内存
        free(command);

        argv[1] = argv[2];
        argc--;
    }

    // 判断文件夹是否存在
    if (access(argv[1], F_OK) == -1) {
//        fprintf(stderr, "挂载路径不存在: %s\n", argv[1]);
        mkdir(argv[1], 0777);
        fprintf(stderr, "已创建路径: %s\n", argv[1]);
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

    // 初始化哈希环
    for (int i = 0; i < HASH_RING_SIZE; i++) {
        hashRing[i].path = NULL;
    }

//    int fd = open(argv[1], O_RDWR);
//    dev_null_dir = fdopendir(fd);
//    if (dev_null_dir == NULL) {
//        fprintf(stderr, "Cannot open directory stream for %s: %s\n", argv[1], strerror(errno));
//        exit(EXIT_FAILURE);
//    }

    umask(0);
    return fuse_main(argc, argv, &xmp_oper, NULL);
}


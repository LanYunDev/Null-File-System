// 代码参考: https://github.com/macos-fuse-t/libfuse/tree/master/example
// 该代码仅适用于macOS

#define FUSE_USE_VERSION 29

#define HAVE_SETXATTR 1

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <libproc.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(_POSIX_C_SOURCE)
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
#endif

#define MEGABYTE (1024 * 1024)          // 1MB
#define MEMORY_THRESHOLD (15 * MEGABYTE)// 15MB

#define MAX_LISTS 10     // stringLists数据结构的长度
#define MAX_VISIT_COUNT 10// 最大访问次数
#define TIME_LIMIT 20    // 时间限制

typedef struct {
    char *str;
    unsigned short int visit_count;
    time_t first_visit_time;
} StringInfo;

static StringInfo stringLists[MAX_LISTS];

static unsigned short int FirstAccessCount = 0;// 计数动态变化的文件名
static char *dynamicBlackLists[11] = {NULL};   // 存储动态黑名单

// 获取指定 pid 进程的名称
static char processName[PROC_PIDPATHINFO_MAXSIZE];

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER; // 初始化互斥锁

// 全局变量，用于存储/dev/null的文件描述符
static int dev_null_fd;

// 全局变量，用于存储挂载路径
static const char *point_path;
static const char *file_path;

// 全局变量，用于存储预设的符号链接路径
static const char *linkpath = "/dev/null";

// 全局变量, 保存虚拟文件的状态信息
struct stat virtual_file_stat;

// 全局变量，用于记录文件是否被访问,默认为true
static bool isfileAccessed = true;

// 全局变量，用于保存调试信息的日志文件路径
static const char *debugFilePath = "/tmp/fs_debug.log";
static const char *Monitor_debugFilePath = "/tmp/fs_Memory.log";
static const char *logFilePath = "/tmp/fs.log";

static const size_t thresholdMB = 1;

// 全局变量，用于标志内存是否泄露,Release下默认为false
#ifdef DEBUG
static bool isMemoryLeak = true;
#else
static bool isMemoryLeak = false;
#endif

static FILE *debug_fp;

// 默认开启黑名单模式
static unsigned short int blackMode = 1;

//static const char *blacklists[] = {};
// ".dat.nosync",".nfs"
//static const size_t blacklists_size =
//        sizeof(blacklists) / sizeof(blacklists[0]);

static const char *whitelists[] = {};
static const size_t whitelists_size =
        sizeof(whitelists) / sizeof(whitelists[0]);

static const char *special_lists[] = {"apache2"};
static const size_t special_lists_size =
        sizeof(special_lists) / sizeof(special_lists[0]);
// 主要将/apache2 下的 access_log error_log 文件识别为文件.

// 全局变量，用于保存读取的数据
static struct fuse_bufvec *read_null_buf;

static time_t firstAccess_time = 0;
static time_t current_time;
static time_t lastAccess_time;
static char time_str[20];
static const unsigned short int time_str_size = sizeof(time_str) / sizeof(time_str[0]);

static char index_str[3] = {0};

// 全局变量
static pid_t pid;
// 用于保存子进程pid
static pid_t monitorPid = 0;
// 全局变量，用于存储pid字符串
static const char *pid_str;

// 哈希环的长度
enum {
    HASH_RING_SIZE = 10,// 哈希环的长度
    HASH_MULTIPLIER = 5 // 哈希环的乘数2^5 = 32
};

// 哈希环节点
//typedef struct {
//    char *path;
//} HashNode;

// 哈希环
//static HashNode hashRing[HASH_RING_SIZE] = {NULL};
static char *hashRing[HASH_RING_SIZE] = {NULL}; // 初始化哈希环

static unsigned short int arrayIncludes(const char *array[], size_t size,
                                        const char *target);
static unsigned short int endsWith(const char *str, int num_suffix, ...);
static void safeFree(char **node);

// 计算路径的哈希值
static unsigned int hashFunction(const char *string) {
    unsigned int hash = 5381;
    while (*string) {
        hash = ((hash << HASH_MULTIPLIER) + hash) + *string++;// hash * 33 + string
    }
    return hash % HASH_RING_SIZE;
}

// 检查路径是否存在于哈希环中
static bool pathExists(const char *string) {
    unsigned int index = hashFunction(string);
    return (hashRing[index] != NULL) &&
           (strcmp(hashRing[index], string) == 0);
}

// 将路径写入哈希环中，覆盖已存在的路径
static void writePath(const char *string) {
    unsigned int index = hashFunction(string);
    if (hashRing[index] != NULL) {
        safeFree(&hashRing[index]);
    }
    // 分配内存并复制路径
    hashRing[index] = strdup(string);
}

// 释放哈希环的内存
static void freeHashRing() {
    for (int i = 0; i < HASH_RING_SIZE; i++) {
        if (hashRing[i] != NULL) {
            safeFree(&hashRing[i]);
        }
    }
}

// 向日志文件中输入内容函数
unsigned short int writeLog(const char *logContent) {
    FILE *log_fp = fopen(logFilePath, "a");
    if (log_fp == NULL) {
        perror("Filed to open log file\n");
        return 1;
    }
    fprintf(log_fp, "\n%d: %s", pid, logContent);
    fclose(log_fp);
    return 0;
}

// 合并多个字符串函数
char *strmerge(const char* strings[]) {
    size_t total_length = 0; // 计算总长度
    for (size_t i = 0; strings[i] != NULL; i++) {
        total_length += strlen(strings[i]);
    }
    char *result = (char *) malloc(total_length + 1); // +1 用于存储字符串结束符 '\0'
    if (result == NULL) {
        perror("Memory allocation failed\n");
        writeLog("Memory allocation failed\n");
        return NULL;
    }
    result[0] = '\0'; // 确保开始为空字符串
    for (size_t i = 0; strings[i] != NULL; i++) {
        strcat(result, strings[i]);
    }
    return result;
}

static void safeFree(char **node) {
    pthread_mutex_lock(&mutex);
    if (*node != NULL) {
        free(*node);// 释放内存
        *node = NULL;
    }
    pthread_mutex_unlock(&mutex);
}

// 动态添加黑名单函数
unsigned short int AddDynamicBlackLists(unsigned short int index, const char *input_str) {
    FirstAccessCount = 0;// 重置访问次数

    if (dynamicBlackLists[10] == NULL) {
        // 异常情况:dynamicBlackLists[10]为空
        // 试图截取字符串前1/4长度作为黑名单路径特征
        dynamicBlackLists[index] = strdup(strncpy(malloc(strlen(input_str) / 4 + 1), input_str, strlen(input_str) / 4));
        sprintf(index_str, "%d", index);
        writeLog(strmerge((const char *[]) {"异常情况! dynamicBlackLists[10]为空!\n","input_str:",input_str,"\ndynamicBlackLists[",  index_str, "]:",dynamicBlackLists[index], NULL}));
        return 1;
    }

    if (dynamicBlackLists[index] != NULL) {
        safeFree(&dynamicBlackLists[index]);
    }

    // 比较两个字符串,将2个文件名相同部分截取出来,并存储
    unsigned short int diff_position;// 记录第一个不同的位置
    for (unsigned short int i = 0;; i++) {
        if (input_str[i] != dynamicBlackLists[10][i] ||  input_str[i] == '\0' || dynamicBlackLists[10][i] == '\0') {
            diff_position = i;
            break;
        }
    }

    if (diff_position == 0) {
        // 起始路径不同,遇到这种情况可以做特殊处理,目前没想好怎么处理,先拒绝好了
        dynamicBlackLists[index] = strdup(strncpy(malloc(strlen(input_str) / 4 + 1), input_str, strlen(input_str) / 4));
        sprintf(index_str, "%d", index);
        writeLog(strmerge((const char *[]) {"异常情况! 起始路径不同!\n","input_str:",input_str,"\ndynamicBlackLists[10]:",dynamicBlackLists[10],"\ndynamicBlackLists[",  index_str, "]:",dynamicBlackLists[index],NULL}));
        return 1;
    }

    dynamicBlackLists[index] = strdup(strncpy(malloc((diff_position + 1 + 1) * sizeof(char)), input_str, diff_position + 1));
    dynamicBlackLists[index][diff_position + 1] = '\0';// 实际是第 diff_position + 1 + 1 位
    sprintf(index_str, "%d", index);
//    if (isMemoryLeak) {}
    #ifdef DEBUG
    writeLog(strmerge((const char *[]) {"新增动态黑名单:\n","dynamicBlackLists[",  index_str, "]:",dynamicBlackLists[index], NULL}));
    #endif
    lastAccess_time = time(NULL); // 初始化上次访问时间

    return 0;
}

// 判断字符串是否在动态黑名单中
static unsigned short int isInDynamicBlackLists(const char *input_str) {
    // 特殊规则处理
    if (endsWith(input_str, 1, ".xlog")) {
        return 0;
    }

    current_time = time(NULL);

    for (size_t i = 0; i < ((sizeof(dynamicBlackLists) / sizeof(dynamicBlackLists[0])) - 1); ++i) {
        if ((dynamicBlackLists[i] != NULL) && memcmp(dynamicBlackLists[i], input_str, strlen(dynamicBlackLists[i])) == 0) {
            // 判断名单是否过期
            if (current_time - lastAccess_time > TIME_LIMIT*3) {
                safeFree(&dynamicBlackLists[i]);
            } else {
                lastAccess_time = current_time;
                return 1;// 字符串数组中包含目标字符串
            }
        }
    }

    unsigned short int index = hashFunction(input_str);

    if (stringLists[index].str != NULL) {
        if (strcmp(stringLists[index].str, input_str) != 0) {
            // 不相同字符串,但相同index
            // 注意此处可能会释放掉dynamicBlackLists[10]指向的内存块
            if (stringLists[index].str == dynamicBlackLists[10]) {
                // 指向同一块内存
                dynamicBlackLists[10] = NULL;
            }
            safeFree((char **) &stringLists[index]);
            goto FirstAccess;
        }

        if (current_time - stringLists[index].first_visit_time <= TIME_LIMIT) {
            stringLists[index].visit_count++;
            if (stringLists[index].visit_count >= MAX_VISIT_COUNT) {
                // 添加到动态黑名单
                AddDynamicBlackLists(index, input_str);
                return 1;
            }
        } else {
            stringLists[index].visit_count = 1;
            stringLists[index].first_visit_time = current_time;
        }

        return 0;
    } else {
        // 首次访问
    FirstAccess:
        if (current_time - firstAccess_time <= TIME_LIMIT * 3) {
            FirstAccessCount++;
        } else {
            FirstAccessCount = 1;
        }

        firstAccess_time = current_time;

        if (FirstAccessCount >= MAX_VISIT_COUNT) {
            // 添加到动态黑名单
            AddDynamicBlackLists(index, input_str);
            return 1;
        }

        stringLists[index].str = dynamicBlackLists[10] = strdup(input_str);
        stringLists[index].visit_count = 1;
        stringLists[index].first_visit_time = time(NULL);
        return 0;
    }
}

// 字符串前缀匹配函数
// unsigned short int startsWith(const char *str, const char *prefix) {
//    while (*prefix) {
//        if (*prefix++ != *str++) {
//            return 0;  // 字符不匹配，返回假
//        }
//    }
//    return 1;  // 字符匹配，返回真
//}

static unsigned short int arrayIncludes(const char *array[], size_t size,
                                        const char *target) {
    for (size_t i = 0; i < size; ++i) {
        if ((array[i] != NULL) && memcmp(array[i], target, strlen(array[i])) == 0) {
            return 1;// 字符串数组中包含目标字符串
        }
    }
    return 0;// 字符串数组中不包含目标字符串
}

// 判断字符串是否以指定后缀结尾
static unsigned short int endsWith(const char *str, int num_suffix, ...) {
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
                return 0;// 字符不匹配，不是以后缀结尾
            }
        }
    }

    va_end(suffix_list);
    return 1;// 字符匹配，以后缀结尾
}

// 文件名判定规则
static unsigned short int rule_filename(const char *path) {
    // 以.开头的文件报错返回
    const char *filename = strrchr(path, '/');
    if (filename != NULL) {
        filename++;// 移动到文件名的第一个字符
        if (*filename == '.' && (++filename) != NULL) {
            return 1;
        }
    }
    return 0;
}

// 函数用于判断路径是否指向一个目录
static unsigned short int is_directory(const char *path) {
    // 从路径中获取文件名
    const char *filename = strrchr(path, '/');// 得到文件名依然带有'/'
    // 如果找到了文件名，则进行判断
    if (filename != NULL) {
        // 获取文件名中的后缀
        const char *suffix = strrchr(filename, '.');

        // 如果找到了后缀，并且后缀中第一个不是数字，则认为是文件
        if (suffix != NULL) {
            suffix++;// 移动到后缀的第一个字符

            static unsigned short int JetBrain_path;
            if (blackMode) {
                JetBrain_path = (!(memcmp((path + 1), "JetBrains", 9)));
            } else {
                JetBrain_path = 1;
            }
            //            const unsigned short int JetBrain_path = (!(strcmp( (path +
            //            1), "JetBrains")));
            if ((*suffix < '0' || *suffix > '9') ||
                (JetBrain_path && ((suffix[-4] == 'c') && (suffix[-3] == 's') &&
                                   (suffix[-2] == 'v')))) {// 匹配JB中.csv.0 文件
                // 针对jetbrains的文件进行特殊处理
                if (JetBrain_path && !endsWith(filename, 2, ".log", ".txt")) {
                    filename++;// 移动到文件名的第一个字符
                    if (!pathExists(filename)) {
                        // 哈希环中不存在该文件名
                        writePath(filename);
                        isfileAccessed = false;
                    }
                }
                return 0;
            }
        } else if (arrayIncludes(special_lists, special_lists_size, (path + 1)) && (!arrayIncludes(special_lists, special_lists_size, (filename + 1)))) {
            // 特殊处理
            return 0;
        }
    }

    return 1;
}

static unsigned short int execute_command(const char *command_prefix,
                                          const char *command_suffix) {
    // 计算需要的内存大小，包括命令字符串和终结符 '\0'
    size_t command_size =
            strlen(command_suffix) + strlen(command_prefix) +
            5;// 2个双引号加上一个空格长度为 3，额外留两个字符给目录路径和终结符 '\0'

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
    snprintf(command, command_size, "%s \"%s\"", command_prefix, command_suffix);
    fprintf(stderr, "执行命令: %s\n", command);

    // 提示用户确认,都带上参数了,就没必要了.
    //    printf("Are you sure you want to execute this command %s? Press y key to
    //    confirm...\n", command); printf("是否执行这条命令%s?按y键确认...\n",
    //    command); char input = (char) getchar();  // 等待用户按下任意键
    //
    //    if (input != 'y') {
    //        fprintf(stderr, "用户取消执行命令\n");
    //        free(command);
    //        return 1;
    //    }

    unsigned short int ret = system(command);

    // 释放动态分配的内存
    free(command);

    return ret;
}

// 函数用于检查文件大小是否超过指定大小
static unsigned short int fileSizeCheck(const char *filePath) {
    struct stat fileStat;

    // 获取文件信息
    if (stat(filePath, &fileStat) != 0) {
        return 0;// 文件不存在
    }

    // 判断文件大小是否超过指定大小
    if (fileStat.st_size > thresholdMB * MEGABYTE) {
        return 1;// 文件大小超过指定大小
    } else {
        return 0;// 文件大小未超过指定大小
    }
}

static unsigned short int logFileCheck(const char *filePath, const char *description) {
    if (fileSizeCheck(filePath)) {
        fprintf(stderr, "⚠️警告: %s日志文件大小超过阈值: %zu MB\n", description, thresholdMB);
        size_t command_size = strlen("tell application \\\"Finder\\\" to delete POSIX file \\\"\\\"") + strlen(filePath) + 1;
        char *command_suffix = malloc(command_size);
        snprintf(command_suffix, command_size, "%s \\\"%s\\\"", "tell application \\\"Finder\\\" to delete POSIX file", filePath);

        if (execute_command("osascript -e", command_suffix)) {
            fprintf(stderr, "❌无法将文件移动到废纸篓,请手动处理.\n");
            return 1;
        }
        //        tell application \"Finder\" to delete POSIX file \"point_path\"
        fprintf(stderr, "⚠️已自动将文件移动到废纸篓🗑️\n");
    }
    return 0;
}

static void handle_sigterm(int signum) {
    time(&current_time);
    strftime(time_str, time_str_size, "%Y-%m-%d %H:%M:%S",
             localtime(&current_time));
    if (signum == SIGTERM) {
        //        printf("Received SIGTERM signal. Performing cleanup...\n");
        writeLog(strmerge((const char *[]) {"Received SIGTERM signal. Performing cleanup...\n", "退出时间: ", time_str, NULL}));
        execute_command("umount", point_path);
        exit(EXIT_SUCCESS);
    } else if (signum == SIGUSR1) {
        //     debug,打印信息到文件
        debug_fp = fopen(debugFilePath, "a");
        fprintf(debug_fp, "Received SIGUSR1 signal.\n");
        fprintf(debug_fp, "当前挂载路径: %s\n", point_path);
        fprintf(debug_fp, "退出时间: %s\n", time_str);
        isMemoryLeak = true;
    } else if (signum == SIGSEGV || signum == SIGABRT) {
        // 进程崩溃,重启进程
        writeLog(strmerge((const char *[]) {"进程崩溃! 开始重启进程!\n", "崩溃时间: ", time_str, NULL}));
        execute_command("umount", point_path);
        const  pid_t pid_ = fork();
        if (pid_ == 0) {
//            char *new_argv[] = {(char *)file_path, (char *)point_path, NULL};
//            execvp(new_argv[0], new_argv);
            execl(file_path, file_path, point_path, NULL);
            writeLog(strmerge((const char *[]) {"重启进程失败\n", "重启时间: ", time_str, NULL}));
            exit(EXIT_FAILURE);
        } else if (pid_ < 0) {
            // 创建子进程失败
            writeLog("创建新进程失败\n");
            fprintf(stderr, "创建新进程失败\n");
            exit(EXIT_FAILURE);
        }
        asprintf((char **) &pid_str, "%d", pid_);
        writeLog(strmerge((const char *[]) {"新进程pid: ", pid_str, "\n", "重启时间: ", time_str, NULL}));
        fprintf(stderr, "新进程pid: %d\n", pid_);
        if (!monitorPid) {
            if (proc_pidpath(monitorPid, processName, sizeof(processName)) > 0) {
                // 判断进程名是否为 "virtual_fs_monitor"
                if (strcmp(processName, "virtual_fs_monitor") == 0) {
                    kill(monitorPid, SIGTERM);
                }
            }
        }
        exit(EXIT_SUCCESS);
    }
}

static unsigned short int delete_empty_directory(const char *path) {
    DIR *dir;
    struct dirent *entry;

    // 打开目录
    dir = opendir(path);
    if (dir == NULL) {
        perror("Error opening directory");
        return 1;
    }
    // 遍历目录
    while ((entry = readdir(dir)) != NULL) {
        // 忽略 "." 和 ".." 目录
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            closedir(dir);
            fprintf(stderr, "目录不为空\n");
            return 1;
        }
    }
    // 关闭目录
    closedir(dir);

    // 删除空目录
    if (rmdir(path) != 0) {
        perror("Error deleting directory");
        return 1;
    }
    return 0;
}

static int xmp_getattr(const char *path, struct stat *stbuf) {
    //    获取指定路径的文件或目录的属性

    if (isMemoryLeak) {
        fprintf(debug_fp, "%d:xmp_getattr path: %s\n",pid, path);
    }

    // 黑名单
    const char *path_plus = path + 1;
    if (*path_plus) {
        if (blackMode) {
            if ((*path_plus == '.') ||
                //                            arrayIncludes(blacklists, blacklists_size, (path + 1)) ||
                rule_filename(path_plus)
                ) {
                return -ENOENT;
            }
        } else {
            if (!arrayIncludes(whitelists, whitelists_size, path_plus)) {
                return -ENOENT;
            }
        }
    }

    if (!(*path_plus) || is_directory(path)) {
        stbuf->st_mode = S_IFDIR | 0777;// 目录权限
        stbuf->st_nlink = 2;            // 硬链接数
        if (isMemoryLeak) {
            fprintf(debug_fp, "xmp_getattr 伪装为文件夹\n");
        }
    } else {
        if (!isfileAccessed ||
            isInDynamicBlackLists(path_plus)) {
            // 初次访问文件，返回文件不存在
            isfileAccessed = true;// 重置文件访问标志
            return -ENOENT;
        }
        *stbuf = virtual_file_stat;
        if (isMemoryLeak) {
            fprintf(debug_fp, "xmp_getattr 伪装为文件\n");
        }
        //        stbuf->st_mode = S_IFREG | 0777;
        //        stbuf->st_nlink = 1;
        //        stbuf->st_size = 0;
    }

    return 0;
}

static int xmp_fgetattr(__attribute__((unused)) const char *path,
                        __attribute__((unused)) struct stat *stbuf,
                        __attribute__((unused)) struct fuse_file_info *fi) {
    //    在已打开的文件描述符上获取文件或目录的属性
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_fgetattr path: %s\n", path);
    }
    // 黑名单
    if (blackMode) {
//        if (arrayIncludes(blacklists, blacklists_size, (path + 1))) {
//            return -ENOENT;
//        }
    } else {
        if (!(*(path + 1)) ||
            !arrayIncludes(whitelists, whitelists_size, (path + 1))) {
            return -ENOENT;
        }
    }
    *stbuf = virtual_file_stat;
    return 0;
    //    if (!(*(path + 1)) || is_directory(path)) {
    //        stbuf->st_mode = S_IFDIR | 0777;
    //        stbuf->st_nlink = 2;
    //    } else {
    //        *stbuf = virtual_file_stat;
    //        stbuf->st_mode = S_IFREG | 0777;
    //        stbuf->st_nlink = 1;
    //        stbuf->st_size = 0;
    //    }
    //    return 0;
}

static int xmp_access(__attribute__((unused)) const char *path,
                      __attribute__((unused)) int mask) {
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_access path: %s\n", path);
    }
    return 0;
}

static int xmp_readlink(__attribute__((unused)) const char *path, char *buf,
                        __attribute__((unused)) size_t size) {
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_readlink path: %s\n", path);
    }
    // 直接将预设的符号链接路径的地址赋值给buf
    *buf = *(char *) linkpath;
    return 0;
}

struct xmp_dirp {
    __attribute__((unused)) DIR *dp;
    __attribute__((unused)) struct dirent *entry;
    __attribute__((unused)) off_t offset;
};

static int xmp_opendir(__attribute__((unused)) const char *path,
                       __attribute__((unused)) struct fuse_file_info *fi) {
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_opendir path: %s\n", path);
    }
    return 0;
}

__attribute__((unused)) static inline struct xmp_dirp *
get_dirp(struct fuse_file_info *fi) {
    return (struct xmp_dirp *) (uintptr_t) fi->fh;
}

static int xmp_readdir(__attribute__((unused)) const char *path, void *buf,
                       fuse_fill_dir_t filler,
                       __attribute__((unused)) off_t offset,
                       __attribute__((unused)) struct fuse_file_info *fi) {
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_readdir path: %s\n", path);
    }
    // 只返回"."和".."两个目录项
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    return 0;
}

static int xmp_releasedir(__attribute__((unused)) const char *path,
                          __attribute__((unused)) struct fuse_file_info *fi) {
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_releasedir path: %s\n", path);
    }
    return 0;
}

static int xmp_mknod(__attribute__((unused)) const char *path,
                     __attribute__((unused)) mode_t mode,
                     __attribute__((unused)) dev_t rdev) {
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_mknod path: %s\n", path);
    }
    return 0;
}

static int xmp_mkdir(__attribute__((unused)) const char *path,
                     __attribute__((unused)) mode_t mode) {
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_mkdir path: %s\n", path);
    }
    return 0;
}

static int xmp_unlink(__attribute__((unused)) const char *path) {
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_unlink path: %s\n", path);
    }
    return 0;
}

static int xmp_rmdir(__attribute__((unused)) const char *path) {
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_rmdir path: %s\n", path);
    }
    return 0;
}

static int xmp_symlink(__attribute__((unused)) const char *from,
                       __attribute__((unused)) const char *to) {
    return 0;
}

static int xmp_rename(__attribute__((unused)) const char *from,
                      __attribute__((unused)) const char *to) {
    return 0;
}

#ifdef __APPLE__

static int xmp_setvolname(const char *volname) {
    (void) volname;
    return 0;
}

static int xmp_exchange(__attribute__((unused)) const char *path1,
                        __attribute__((unused)) const char *path2,
                        __attribute__((unused)) unsigned long options) {
    return 0;
}

#endif /* __APPLE__ */

static int xmp_link(__attribute__((unused)) const char *from,
                    __attribute__((unused)) const char *to) {
    return 0;
}

#ifdef __APPLE__

static int xmp_fsetattr_x(__attribute__((unused)) const char *path,
                          __attribute__((unused)) struct setattr_x *attr,
                          __attribute__((unused)) struct fuse_file_info *fi) {
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_fsetattr_x path: %s\n", path);
    }
    return 0;
}

static int xmp_setattr_x(__attribute__((unused)) const char *path,
                         __attribute__((unused)) struct setattr_x *attr) {
    return 0;
}

static int xmp_chflags(__attribute__((unused)) const char *path,
                       __attribute__((unused)) uint32_t flags) {
    return 0;
}

static int xmp_getxtimes(__attribute__((unused)) const char *path,
                         struct timespec *bkuptime, struct timespec *crtime) {
    // 预设的备份时间和创建时间
    struct timespec preset_time;
    preset_time.tv_sec = 1706716800;// 2024-02-01 00:00:00 UTC
    preset_time.tv_nsec = 0;

    // 设置备份时间和创建时间为预设的时间
    *bkuptime = preset_time;
    *crtime = preset_time;

    return 0;
}

static int xmp_setbkuptime(__attribute__((unused)) const char *path,
                           __attribute__((unused))
                           const struct timespec *bkuptime) {
    return 0;
}

static int xmp_setchgtime(__attribute__((unused)) const char *path,
                          __attribute__((unused))
                          const struct timespec *chgtime) {
    return 0;
}

static int xmp_setcrtime(__attribute__((unused)) const char *path,
                         __attribute__((unused))
                         const struct timespec *crtime) {
    return 0;
}

#endif /* __APPLE__ */

static int xmp_chmod(__attribute__((unused)) const char *path,
                     __attribute__((unused)) mode_t mode) {
    return 0;
}

static int xmp_chown(__attribute__((unused)) const char *path,
                     __attribute__((unused)) uid_t uid,
                     __attribute__((unused)) gid_t gid) {
    return 0;
}

static int xmp_truncate(__attribute__((unused)) const char *path,
                        __attribute__((unused)) off_t size) {
    return 0;
}

static int xmp_ftruncate(__attribute__((unused)) const char *path,
                         __attribute__((unused)) off_t size,
                         __attribute__((unused)) struct fuse_file_info *fi) {
    return 0;
}

#ifdef HAVE_UTIMENSAT
static int xmp_utimens(const char *path, const struct timespec ts[2]) {
    return 0;
}
#endif

static int xmp_create(__attribute__((unused)) const char *path,
                      __attribute__((unused)) mode_t mode,
                      struct fuse_file_info *fi) {
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_create path: %s\n", path);
    }
    fi->fh = dev_null_fd;
    return 0;// 欺骗性返回成功，但实际上并未创建文件
}

static int xmp_open(__attribute__((unused)) const char *path,
                    struct fuse_file_info *fi) {
    //    已知问题: 无法读取有数据的文件,问题不大
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_open path: %s\n", path);
    }
    fi->fh = dev_null_fd;
    return 0;// 欺骗性返回成功，但实际上并未打开文件
}

static int xmp_read(__attribute__((unused)) const char *path,
                    __attribute__((unused)) char *buf,
                    __attribute__((unused)) size_t size,
                    __attribute__((unused)) off_t offset,
                    __attribute__((unused)) struct fuse_file_info *fi) {
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_read path: %s\n", path);
    }
    return 0;// 欺骗性返回读取的字节数，但实际上并未进行读取
}

static int xmp_read_buf(__attribute__((unused)) const char *path,
                        struct fuse_bufvec **bufp, size_t size,
                        __attribute__((unused)) off_t offset,
                        __attribute__((unused)) struct fuse_file_info *fi) {
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_read_buf path: %s\n", path);
    }
    // 将预设的数据复制到缓冲区
    *read_null_buf = FUSE_BUFVEC_INIT(size);
    read_null_buf->buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
    read_null_buf->buf[0].fd =
            dev_null_fd;// 使用/dev/null的文件描述符 (int) fi->fh;
    read_null_buf->buf[0].pos = offset;

    *bufp = read_null_buf;

    return 0;
}

static int xmp_write(__attribute__((unused)) const char *path,
                     __attribute__((unused)) const char *buf, size_t size,
                     __attribute__((unused)) off_t offset,
                     __attribute__((unused)) struct fuse_file_info *fi) {
    return (int) size;// 欺骗性返回写入的字节数，但实际上并未进行写入
}

static int xmp_write_buf(__attribute__((unused)) const char *path,
                         struct fuse_bufvec *buf, off_t offset,
                         __attribute__((unused)) struct fuse_file_info *fi) {
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_write_buf path: %s\n", path);
    }
    struct fuse_bufvec dst = FUSE_BUFVEC_INIT(fuse_buf_size(buf));
    dst.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
    dst.buf[0].fd = dev_null_fd;// 使用/dev/null的文件描述符 (int) fi->fh;
    dst.buf[0].pos = offset;

    return (int) fuse_buf_copy(&dst, buf, FUSE_BUF_SPLICE_NONBLOCK);
}

static int xmp_statfs(__attribute__((unused)) const char *path,
                      __attribute__((unused)) struct statvfs *stbuf) {

    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_statfs path: %s\n", path);
    }
    stbuf->f_bsize = 512;  // 块大小
    stbuf->f_frsize = 512; // 基本块大小
    stbuf->f_blocks = 1000;// 文件系统数据块总数
    stbuf->f_bfree = 500;  // 可用块数
    stbuf->f_bavail = 500; // 非超级用户可获取的块数
    stbuf->f_files = 50;   // 文件结点总数
    stbuf->f_ffree = 25;   // 可用文件结点数
    stbuf->f_favail = 25;  // 非超级用户的可用文件结点数
    stbuf->f_fsid = 0;     // 文件系统标识
    stbuf->f_flag = 1;     // 挂载标志
    stbuf->f_namemax = 255;// 最大文件名长度

    return 0;
}

static int xmp_flush(__attribute__((unused)) const char *path,
                     __attribute__((unused)) struct fuse_file_info *fi) {
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_flush path: %s\n", path);
    }
    return 0;
}

static int xmp_release(__attribute__((unused)) const char *path,
                       __attribute__((unused)) struct fuse_file_info *fi) {
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_release path: %s\n", path);
    }
    return 0;
}

static int xmp_fsync(__attribute__((unused)) const char *path,
                     __attribute__((unused)) int isdatasync,
                     __attribute__((unused)) struct fuse_file_info *fi) {
    return 0;
}

#if defined(HAVE_POSIX_FALLOCATE) || defined(__APPLE__)

static int xmp_fallocate(__attribute__((unused)) const char *path,
                         __attribute__((unused)) int mode,
                         __attribute__((unused)) off_t offset,
                         __attribute__((unused)) off_t length,
                         __attribute__((unused)) struct fuse_file_info *fi) {
    return 0;
}

#endif

#ifdef HAVE_SETXATTR

static int xmp_setxattr(__attribute__((unused)) const char *path,
                        __attribute__((unused)) const char *name,
                        __attribute__((unused)) const char *value,
                        __attribute__((unused)) size_t size,
                        __attribute__((unused)) int flags,
                        __attribute__((unused)) uint32_t position) {
    return 0;
}

static int xmp_getxattr(__attribute__((unused)) const char *path,
                        __attribute__((unused)) const char *name, char *value,
                        __attribute__((unused)) size_t size,
                        __attribute__((unused)) uint32_t position) {
    if (isMemoryLeak) {
        fprintf(debug_fp, "xmp_getxattr path: %s\n", path);
    }
    // 预设的数据
    const char *preset_data = "";
    size_t preset_data_size =
            strlen(preset_data) + 1;// 加1是为了包含字符串结束符'\0'

    // 将预设的数据复制到缓冲区
    memcpy(value, preset_data, preset_data_size);

    return 0;
}

static int xmp_listxattr(__attribute__((unused)) const char *path,
                         __attribute__((unused)) char *list,
                         __attribute__((unused)) size_t size) {
    return 0;
}

static int xmp_removexattr(__attribute__((unused)) const char *path,
                           __attribute__((unused)) const char *name) {
    return 0;
}

#endif /* HAVE_SETXATTR */

#ifndef __APPLE__
static int xmp_lock(const char *path, struct fuse_file_info *fi, int cmd,
                    struct flock *lock) {
    return 0;
}
#endif

void *xmp_init(struct fuse_conn_info *conn) {
#ifdef __APPLE__
    FUSE_ENABLE_SETVOLNAME(conn);
    FUSE_ENABLE_XTIMES(conn);
#endif

    dev_null_fd = open("/dev/null", O_RDWR);
    if (dev_null_fd == -1) {
        fprintf(stderr, "Cannot open /dev/null: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // 初始化虚拟文件的状态信息
    memset(&virtual_file_stat, 0, sizeof(struct stat));
    virtual_file_stat.st_mode = S_IFREG | 0644;// 设置文件类型和权限
    virtual_file_stat.st_nlink = 1;            // 设置硬链接数
    virtual_file_stat.st_size = 0;             // 设置文件大小
    virtual_file_stat.st_blocks = 0;           // 设置文件块数
    virtual_file_stat.st_atime = virtual_file_stat.st_mtime =
            virtual_file_stat.st_ctime = time(NULL);// 设置文件时间

    read_null_buf = malloc(sizeof(struct fuse_bufvec));
    if (read_null_buf == NULL) {
        fprintf(stderr, "分配内存大小: %zu 失败\n", sizeof(struct fuse_bufvec));
        perror("Error allocating memory");
    }

    // 初始化缓冲区
    for (int i = 0; i < MAX_LISTS; i++) {
        stringLists[i].str = NULL;
    }

    // 设置 SIGTERM 信号的处理函数
    signal(SIGTERM, handle_sigterm);
    signal(SIGUSR1, handle_sigterm);
    signal(SIGSEGV, handle_sigterm);
    signal(SIGABRT, handle_sigterm);

    pid = getpid();
    writeLog(strmerge((const char *[]) {"挂载路径:", point_path,  NULL}));
    return NULL;
}

void xmp_destroy(__attribute__((unused)) void *userdata) {
    //    fprintf(stderr, "即将退出! \n");
    time(&current_time);
    strftime(time_str, time_str_size, "%Y-%m-%d %H:%M:%S",
             localtime(&current_time));
    fprintf(debug_fp, "退出时间: %s\n", time_str);
    fclose(debug_fp);

//    for (int i = 0; i < MAX_LISTS; i++) {
//        free(stringLists[i].str);
//        free(dynamicBlackLists[i]);
//    }
//    free(dynamicBlackLists[10]);
    freeHashRing();                    // 释放哈希环的内存
//    free(read_null_buf);               // 释放缓冲区的内存
    close(dev_null_fd);                // 关闭/dev/null的文件描述符
    delete_empty_directory(point_path);// 删除空目录
    if (!monitorPid) {
        if (proc_pidpath(monitorPid, processName, sizeof(processName)) > 0) {
            // 判断进程名是否为 "virtual_fs_monitor"
            if (strcmp(processName, "virtual_fs_monitor") == 0) {
                kill(monitorPid, SIGTERM);
            }
        }
    }
}

#ifndef __APPLE__
static int xmp_flock(const char *path, struct fuse_file_info *fi, int op) {
    return 0;
}
#endif

static struct fuse_operations xmp_oper = {
        .init = xmp_init,
        .destroy = xmp_destroy,
        .getattr = xmp_getattr,
        .fgetattr = xmp_fgetattr,
#ifndef __APPLE__
        .access = xmp_access,
#endif
        .readlink = xmp_readlink,
        .opendir = xmp_opendir,
        .readdir = xmp_readdir,
        .releasedir = xmp_releasedir,
        .mknod = xmp_mknod,
        .mkdir = xmp_mkdir,
        .symlink = xmp_symlink,
        .unlink = xmp_unlink,
        .rmdir = xmp_rmdir,
        .rename = xmp_rename,
        .link = xmp_link,
        .chmod = xmp_chmod,
        .chown = xmp_chown,
        .truncate = xmp_truncate,
        .ftruncate = xmp_ftruncate,
#ifdef HAVE_UTIMENSAT
        .utimens = xmp_utimens,
#endif
        .create = xmp_create,
        .open = xmp_open,
        .read = xmp_read,
        .read_buf = xmp_read_buf,
        .write = xmp_write,
        .write_buf = xmp_write_buf,
        .statfs = xmp_statfs,
        .flush = xmp_flush,
        .release = xmp_release,
        .fsync = xmp_fsync,
#if defined(HAVE_POSIX_FALLOCATE) || defined(__APPLE__)
        .fallocate = xmp_fallocate,
#endif
#ifdef HAVE_SETXATTR
        .setxattr = xmp_setxattr,
        .getxattr = xmp_getxattr,
        .listxattr = xmp_listxattr,
        .removexattr = xmp_removexattr,
#endif
#ifndef __APPLE__
        .lock = xmp_lock,
        .flock = xmp_flock,
#endif
#ifdef __APPLE__
        .setvolname = xmp_setvolname,
        .exchange = xmp_exchange,
        .getxtimes = xmp_getxtimes,
        .setbkuptime = xmp_setbkuptime,
        .setchgtime = xmp_setchgtime,
        .setcrtime = xmp_setcrtime,
        .chflags = xmp_chflags,
        .setattr_x = xmp_setattr_x,
        .fsetattr_x = xmp_fsetattr_x,
#endif

        .flag_nullpath_ok = 1,
#if HAVE_UTIMENSAT
        .flag_utime_omit_ok = 1,
#endif
};

int main(int argc, char *argv[]) {
    boolean_t monitor = true;

    // 检查命令行参数数量
    if (argc == 1) {
    usage_info:
        fprintf(stderr, "用法: %s [-delete] [-disable_blackMode] <挂载路径>\n",
                argv[0]);
        exit(EXIT_FAILURE);
    }

    // 检查参数
    if (argc > 2) {
        unsigned short int flag = 0;
        if (argc == 3) {
            if (strcmp(argv[1], "-delete") == 0) {
                flag = 1;
            } else if (strcmp(argv[1], "-disable_blackMode") == 0) {
                flag = 2;
            } else if (strcmp(argv[1], "-d") == 0) {
                point_path = argv[2]; // 挂载路径
                goto start_run;
            } else if (strcmp(argv[1], "-no_monitor") == 0) {
                monitor = false;
                flag = 4;
            }
            argv[1] = argv[2];
            argc--;
        } else if (argc == 4) {
            if ((strcmp(argv[1], "-delete") == 0) &&
                (strcmp(argv[2], "-disable_blackMode") == 0)) {
                flag = 3;
            } else if (strcmp(argv[1], "-d") == 0 && strcmp(argv[2], "-no_monitor") == 0) {
                monitor = false;
                point_path = argv[3]; // 挂载路径
                argv[2] = argv[3];
                argc--;
                goto start_run;
            }
            argv[1] = argv[3];
            argc = argc - 2;
        }
        switch (flag) {
            case 1:
                flag = execute_command("rm -rf", argv[1]);
                break;
            case 2:
                flag = 0;
                blackMode = 0;
                break;
            case 3:
                flag = execute_command("rm -rf", argv[1]);
                blackMode = 0;
                break;
            case 4:
                goto start_run;
            default:
                goto usage_info;
        }
        if (flag) {
            fprintf(stderr, "❌路径 %s 删除失败!\n", argv[1]);
            fprintf(stderr, "请手动处理该错误!\n");
            exit(EXIT_FAILURE);
        }
    }

    point_path = argv[1]; // 挂载路径

start_run:
#ifdef DEBUG
    fprintf(stderr, "编译使用fuse版本: %d\n", FUSE_USE_VERSION);
    fprintf(stderr, "本地安装fuse版本: %d\n", FUSE_VERSION);
    debug_fp = fopen(debugFilePath, "a");
    fprintf(stderr, "⚠️警告: 已开启Debug日志记录!\n");
    fprintf(debug_fp, "当前挂载路径: %s\n", point_path);
    time(&current_time);
    strftime(time_str, time_str_size, "%Y-%m-%d %H:%M:%S",
             localtime(&current_time));
    fprintf(debug_fp, "开始时间: %s\n", time_str);
#endif
    file_path = argv[0]; // 文件路径
    // 判断路径是否为目录
    struct stat file_stat;
    if (stat(point_path, &file_stat) == 0 && !S_ISDIR(file_stat.st_mode)) {
        fprintf(stderr, "⚠️警告: 路径: %s 不是一个目录\n", point_path);
        char *command_suffix = malloc(
                strlen(
                        "tell application \\\"Finder\\\" to delete POSIX file \\\"\\\"") +
                strlen(point_path) + 1);
        if (execute_command("osascript -e", command_suffix)) {
            fprintf(stderr, "❌无法将文件移动到废纸篓,请手动处理.\n");
            exit(EXIT_FAILURE);
        }
        //        tell application \"Finder\" to delete POSIX file \"point_path\"
        fprintf(stderr, "⚠️已自动将文件移动到废纸篓🗑️\n");
    }
    // 判断路径是否存在
    if (access(point_path, F_OK) == -1) {
        if (mkdir(point_path, 0777)) {
            fprintf(stderr, "创建路径: %s 失败\n", point_path);
            exit(EXIT_FAILURE);
        }
        fprintf(stderr, "已创建路径: %s\n", point_path);
    }

    // 检查日志文件大小是否超过阈值
    if (logFileCheck(Monitor_debugFilePath, "监控程序") || logFileCheck(debugFilePath, "主程序Debug")) {
        exit(EXIT_FAILURE);
    }

    if (monitor) {
        // 启动子进程来监视内存使用情况,防止内存泄漏
        monitorPid = fork();

        if (monitorPid == 0) {
            char *new_argv[] = {NULL, (char *)point_path, NULL, (char *)NULL};
            // 构建新的 argv，将 argv[0] 加上 "_monitor"
            new_argv[0] = strmerge((const char *[]) {file_path, "_monitor", NULL});
            asprintf((char **) &new_argv[2], "%d", getpid() + 2);

            // 使用 execvp 执行指定路径的程序
            execvp(new_argv[0], new_argv);

            // 如果 execvp 失败，打印错误信息
            perror("execvp");
            free(new_argv[0]);
            free(new_argv[1]);
            return 1;
        } else if (monitorPid < 0) {
            // 创建子进程失败
            fprintf(stderr, "创建子进程失败\n");
            exit(EXIT_FAILURE);
        }
        // 打印monitorPid
        fprintf(stderr, "监控进程pid: %d\n", monitorPid);
    }

    umask(0);

    return fuse_main(argc, argv, &xmp_oper, NULL);
}

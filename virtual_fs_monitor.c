// 该代码仅用于监控 virtual_fs 进程的内存使用情况,防止发生内存泄露
#include <dirent.h>
#include <libproc.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/time.h>
#include <unistd.h>

#define MEGABYTE (1024 * 1024)          // 1MB
#define MEMORY_THRESHOLD (15 * MEGABYTE)// 15MB

// 全局变量，用于保存主进程的pid
static pid_t targetPid;
static pid_t pid;

// 全局变量，用于存储pid字符串
static const char *targetPid_str;
static const char *pid_str;

static const char *tmp_str1;
static const char *tmp_str2;

// 全局变量，用于存储挂载路径
static const char *point_path;

// 全局变量，用于调试信息的日志文件路径
static const char *debugFilePath = "/tmp/fs_Memory.log";
static const char *Main_debugFilePath = "/tmp/fs_debug.log";
static const char *logFilePath = "/tmp/fs.log";
static const char *umount_str;
static char *mergedString = NULL;
pthread_mutex_t mergedStringMutex = PTHREAD_MUTEX_INITIALIZER;

static const size_t thresholdMB = 4;
static time_t current_time;
static char time_str[20];

static const unsigned short int time_str_size = sizeof(time_str) / sizeof(time_str[0]);

// 是否发送收集日志信号标志
static unsigned short int sendCollectLogSignal = 0;

// 获取指定 pid 进程的名称
static char processName[PROC_PIDPATHINFO_MAXSIZE];

static boolean_t isDebug = false;

static void safeFree(char **node) {
//    pthread_mutex_lock(&mutex);
    if (*node != NULL) {
        free(*node);// 释放内存
        *node = NULL;
    }
//    pthread_mutex_unlock(&mutex);
}

// 向日志文件中输入内容函数
static unsigned short int writeLog(const char *logContent) {
    FILE *log_fp = fopen(logFilePath, "a");
    if (log_fp == NULL) {
        perror("Filed to open log file\n");
        return 1;
    }
    fprintf(log_fp, "\n%d: %s", pid, logContent);
    fclose(log_fp);
    if (logContent == mergedString) {
        pthread_mutex_lock(&mergedStringMutex);
        safeFree(&mergedString);
        pthread_mutex_unlock(&mergedStringMutex);
    }
    return 0;
}

// 合并多个字符串函数
static char *strmerge(const char *strings[]) {
    size_t total_length = 0;// 计算总长度
    for (size_t i = 0; strings[i] != NULL; i++) {
        total_length += strlen(strings[i]);
    }
    pthread_mutex_lock(&mergedStringMutex);
    mergedString = (char *) malloc(total_length + 1);// +1 用于存储字符串结束符 '\0'
    if (mergedString == NULL) {
        perror("Memory allocation failed\n");
        writeLog("Memory allocation failed\n");
        return NULL;
    }
    mergedString[0] = '\0';// 确保开始为空字符串
    for (size_t i = 0; strings[i] != NULL; i++) {
        strcat(mergedString, strings[i]);
    }
    pthread_mutex_unlock(&mergedStringMutex);
    return mergedString;
}

static unsigned short int execute_command(const char *command) {
    FILE *log_fp = fopen(logFilePath, "a");
    fprintf(log_fp, "\n%d: 执行命令: %s", pid, command);
    fclose(log_fp);
    fprintf(stderr, "执行命令: %s\n", command);

    unsigned short int ret = system(command);

    // 释放动态分配的内存
    if (mergedString != NULL) {
        pthread_mutex_lock(&mergedStringMutex);
        safeFree(&mergedString);
        pthread_mutex_unlock(&mergedStringMutex);
    }

    return ret;
}

// 函数用于检查文件大小是否超过指定大小
static unsigned short int fileSizeCheck(const char *filePath) {
    struct stat fileStat;

    // 获取文件信息
    if (stat(filePath, &fileStat) != 0) {
        return 0;// 文件不存在
                 //        perror("stat");
                 //        return -1; // 获取文件信息失败
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
        asprintf((char **) &tmp_str1, "%zu", thresholdMB);
        writeLog(strmerge((const char *[]){"⚠️警告: ", description, "日志文件大小超过阈值: ", tmp_str1, " MB",NULL}));
        fprintf(stderr, "⚠️警告: %s日志文件大小超过阈值: %zu MB\n", description, thresholdMB);

        if (execute_command(strmerge((const char *[]){"osascript -e ", "'tell application \"Finder\" to delete POSIX file \"", filePath, "\"'", NULL}))) {
            writeLog(strmerge((const char *[]){"❌无法将文件移动到废纸篓,请手动处理.\n", "文件路径: ", filePath, NULL}));
            fprintf(stderr, "❌无法将文件移动到废纸篓,请手动处理.\n");
            return 1;
        }
        //        tell application \"Finder\" to delete POSIX file \"point_path\"
        writeLog(strmerge((const char *[]){"⚠️已自动将文件移动到废纸篓🗑️\n", "文件路径: ", filePath, NULL}));
        fprintf(stderr, "⚠️已自动将文件移动到废纸篓🗑️\n");
    }
    return 0;
}

static void exit_process(FILE *fp) {
    if (proc_pidpath(targetPid, processName, sizeof(processName)) > 0) {
        if (strstr(processName, "virtual_fs") != NULL) {
            kill(targetPid, SIGTERM);
            execute_command(strmerge((const char *[]){"umount ", point_path, NULL}));
            sleep(3);
            if (!system(umount_str)) {
                execute_command(strmerge((const char *[]){"diskutil umount force ", point_path, NULL}));
                sleep(3);
                if (!system(umount_str)) {
                    fprintf(fp, "%d: 结束进程失败!\n", targetPid);
                    return;
                }
            }
        }
        fprintf(fp, "%d: 结束进程成功!\n", targetPid);
    } else {
        writeLog(strmerge((const char *[]){"获取进程名失败,targetPid: ", targetPid_str, NULL}));
        fprintf(fp, "%d: 获取进程名失败\n", pid);
    }
}

// 信号处理函数
static void handleMonitor(int signum) {
    // 获取当前时间
    time(&current_time);
    // 将时间格式化为字符串
    strftime(time_str, time_str_size, "%Y-%m-%d %H:%M:%S", localtime(&current_time));
    writeLog(strmerge((const char *[]){"监控进程收到信号: ", strsignal(signum), "\n","时间: ", time_str, NULL}));
    if (signum == SIGTERM || signum == SIGINT) {
        FILE *fp = fopen(debugFilePath, "a");
        // 写入格式化后的时间字符串到文件
        fprintf(fp, "%d: 时间: %s\n", pid, time_str);
        fprintf(fp, "%d: 监控进程被杀死, 开始结束指定进程pid: %d\n", pid, targetPid);

        if (proc_pidpath(targetPid, processName, sizeof(processName)) > 0) {
            writeLog(strmerge((const char *[]){"监控进程被杀死,开始结束指定进程,targetPid: ", targetPid_str, NULL}));
            exit_process(fp);
        } else {
            writeLog(strmerge((const char *[]){"主进程被杀死,获取主进程名失败,targetPid: ", targetPid_str, NULL}));
            fprintf(fp, "%d: 获取主进程名失败\n", pid);
        }
        fclose(fp);
        execute_command(strmerge((const char *[]){"open ", debugFilePath, NULL}));
        // 结束当前进程
        exit(EXIT_SUCCESS);
    }
}

static void monitorMemory() {
    // 获取指定 pid 进程的内存使用情况
    struct proc_taskinfo taskInfo;
    unsigned short int searchDepth = 10;

SearchProcess:
//    if (proc_pidpath(targetPid, processName, sizeof(processName)) <= 0) {
//        asprintf((char **) &tmp_str1, "%d", isDebug);
//        writeLog(strmerge((const char *[]){"获取进程名失败,targetPid: ", targetPid_str,",isDebug:", tmp_str1, NULL}));
//        perror("获取进程名失败");
//        exit(EXIT_FAILURE);
//    }
    proc_pidpath(targetPid, processName, sizeof(processName));
    // 判断进程名是否包含 "virtual_fs"
    if (strstr(processName, "virtual_fs") == NULL) {
        if (searchDepth > 0) {
            // 递归查找父进程
            if (isDebug) {
                targetPid--;
            } else {
                targetPid++;
            }
            searchDepth--;
            goto SearchProcess;
        }
        // 不包含 "virtual_fs"，不进行内存监视
        fprintf(stderr, "未找到包含virtual_fs的进程名, 不进行内存监视\n");
        writeLog("未找到包含virtual_fs的进程名, 不进行内存监视\n");
        exit(EXIT_SUCCESS);
    }

    if (searchDepth != 10) {
        asprintf((char **) &targetPid_str, "%d", targetPid);
    }
    writeLog(strmerge((const char *[]){"开始监控进程: ", processName, " pid: ", targetPid_str, NULL}));

    // 设置信号处理函数
    signal(SIGTERM, handleMonitor);
    signal(SIGINT, handleMonitor);

    unsigned long memoryUsageMB;
    while (1) {
        if (proc_pidinfo(targetPid, PROC_PIDTASKINFO, 0, &taskInfo, sizeof(taskInfo)) <= 0 || strstr(processName, "virtual_fs") == NULL) {
            writeLog(strmerge((const char *[]){"监控进程获取主进程名失败,pid: ", targetPid_str, NULL}));
            perror("获取进程名失败!");
            exit(EXIT_FAILURE);
        }
        //        unsigned long memoryUsageKB = taskInfo.pti_resident_size / 1024;
        memoryUsageMB = taskInfo.pti_resident_size / 1024 / 1024;

        // 如果内存使用超过阈值，发送信号给主进程
        if (sendCollectLogSignal && memoryUsageMB > MEMORY_THRESHOLD / MEGABYTE) {
            FILE *fp = fopen(debugFilePath, "a");
            time(&current_time);
            strftime(time_str, time_str_size, "%Y-%m-%d %H:%M:%S", localtime(&current_time));
            // 写入格式化后的时间字符串到文件
            fprintf(fp, "时间: %s\n", time_str);
            fprintf(fp, "内存使用超过阈值: %d MB\n", MEMORY_THRESHOLD / MEGABYTE);
            fprintf(fp, "内存使用: %ld MB\n", memoryUsageMB);
            asprintf((char **) &tmp_str1, "%d", MEMORY_THRESHOLD / MEGABYTE);
            asprintf((char **) &tmp_str2, "%ld", memoryUsageMB);
            writeLog(strmerge((const char *[]){"时间: ", time_str, "\n内存使用超过阈值: ", tmp_str1, " MB\n", "内存使用: ", tmp_str2, " MB", NULL}));
            exit_process(fp);
            fclose(fp);
            // 结束当前进程
            exit(EXIT_SUCCESS);
        } else if (memoryUsageMB > MEMORY_THRESHOLD / MEGABYTE / 3) {
            sendCollectLogSignal = 1;
            kill(targetPid, SIGUSR1);// 发送收集日志信号
        } else if (
//                logFileCheck(logFilePath, "virtual_fs日志") ||
                logFileCheck(Main_debugFilePath, "主程序Debug")) {
            FILE *fp = fopen(debugFilePath, "a");
            time(&current_time);
            strftime(time_str, time_str_size, "%Y-%m-%d %H:%M:%S", localtime(&current_time));
            // 写入格式化后的时间字符串到文件
            fprintf(fp, "时间: %s\n", time_str);
            fprintf(fp, "文件大小超过阈值: %zu MB\n", thresholdMB);
            asprintf((char **) &tmp_str1, "%zu", thresholdMB);
            writeLog(strmerge((const char *[]){"时间: ", time_str, "\n文件大小超过阈值: ", tmp_str1, " MB", NULL}));
            exit_process(fp);
            fclose(fp);
            execute_command(strmerge((const char *[]){"open ", Main_debugFilePath, NULL}));
            // 结束当前进程
            exit(EXIT_SUCCESS);
        }

        // 每隔一段时间检查一次，避免频繁检查
        sleep(10);
    }
}

int main(int argc, char *argv[]) {
    // 检查命令行参数数量
    if (argc == 4 && strcmp(argv[3], "-d") == 0) {
        isDebug = true;
    } else if (argc != 3) {
        fprintf(stderr, "用法: %s <挂载路径> <主进程pid>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    char *endptr;
    targetPid = (int) strtol(argv[2], &endptr, 10);
    fprintf(stderr, "targetPid: %d\n", targetPid);

    // 检查是否转换成功
    if (*endptr != '\0' || endptr == argv[2]) {
        fprintf(stderr, "Invalid number: %s\n", argv[2]);
        return 1;
    }
    point_path = argv[1];

    umount_str = strmerge((const char *[]){"mount | grep \"", point_path, "\" | grep \"fuse-t\"", NULL});

    // 获取当前进程的pid
    pid = getpid();
    asprintf((char **) &pid_str, "%d", pid);
    asprintf((char **) &targetPid_str, "%d", targetPid);

    sleep(3);// 等待 virtual_fs 进程启动
    monitorMemory();

    return 0;
}

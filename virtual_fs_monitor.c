// 该代码仅用于监控 virtual_fs 进程的内存使用情况,防止发生内存泄露
#include <dirent.h>
#include <libproc.h>
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
static const char *pid_str;

static const char *tmp_str1;
static const char *tmp_str2;

// 全局变量，用于存储挂载路径
static const char *point_path;

// 全局变量，用于调试信息的日志文件路径
static const char *debugFilePath = "/tmp/fs_Memory.log";
static const char *Main_debugFilePath = "/tmp/fs_debug.log";
static const char *logFilePath = "/tmp/fs.log";

static const size_t thresholdMB = 4;

static time_t current_time;
static char time_str[20];
static const unsigned short int time_str_size = sizeof(time_str) / sizeof(time_str[0]);

// 是否发送收集日志信号标志
static unsigned short int sendCollectLogSignal = 0;

// 获取指定 pid 进程的名称
static char processName[PROC_PIDPATHINFO_MAXSIZE];

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

static unsigned short int execute_command(const char *command_prefix, const char *directory_path) {
    // 计算需要的内存大小，包括命令字符串和终结符 '\0'
    size_t command_size =
            strlen(directory_path) + strlen(command_prefix) + 5;// 2个单引号加上一个空格长度为 3，额外留两个字符给目录路径和终结符 '\0'

    // 分配足够的内存
    char *command = (char *) malloc(command_size);

    if (command == NULL) {
        fprintf(stderr, "分配内存大小: %zu 失败\n", command_size);
        perror("Error allocating memory");
        return 1;
    }

    // 构建删除命令并执行
    snprintf(command, command_size, "%s '%s'", command_prefix, directory_path);

    unsigned short int ret = system(command);

    // 释放动态分配的内存
    free(command);

    return ret;
}

// 函数用于检查文件大小是否超过指定大小
unsigned short int fileSizeCheck(const char *filePath) {
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

static void exit_process(FILE *fp) {
    if (strstr(processName, "virtual_fs") != NULL) {
        kill(targetPid, SIGTERM);
        sleep(1);
        if (!access(point_path, F_OK)) {
            execute_command("diskutil umount force", point_path);
            sleep(1);
            if (!(strstr(processName, "virtual_fs") != NULL && kill(targetPid, SIGKILL) == 0 && access(point_path, F_OK))) {
                fprintf(fp, "%d: 结束进程失败!\n", targetPid);
                return;
            }
        }
    }
    fprintf(fp, "%d: 结束进程成功!\n", targetPid);
}

// 信号处理函数
static void handleMonitor(__attribute__((unused)) int signal) {
    FILE *fp = fopen(debugFilePath, "a");
    // 获取当前时间
    time(&current_time);
    // 将时间格式化为字符串
    strftime(time_str, time_str_size, "%Y-%m-%d %H:%M:%S", localtime(&current_time));
    // 写入格式化后的时间字符串到文件
    fprintf(fp, "%d: 时间: %s\n",pid, time_str);
    fprintf(fp, "%d: 监控进程被杀死, 开始结束指定进程pid: %d\n", pid,targetPid);

    if (proc_pidpath(targetPid, processName, sizeof(processName)) > 0) {
        writeLog(strmerge((const char *[]) { "时间: ",time_str,"\n监控进程被杀死, 开始结束指定进程pid: ", pid_str, "\n"}));
        exit_process(fp);
    } else {
        fprintf(fp, "%d: 获取进程名失败\n",pid);
    }
    fclose(fp);
    execute_command("open", debugFilePath);
    // 结束当前进程
    exit(EXIT_SUCCESS);
}

void monitorMemory() {
    // 获取指定 pid 进程的内存使用情况
    struct proc_taskinfo taskInfo;
    unsigned short int searchDepth = 10;

SearchProcess:
    if (proc_pidpath(targetPid, processName, sizeof(processName)) <= 0) {
        writeLog(strmerge((const char *[]) {"Failed to get process name,pid: ", pid_str, "\n"}));
        perror("Failed to get process name");
        exit(EXIT_FAILURE);
    }
    // 判断进程名是否包含 "virtual_fs"
    if (strstr(processName, "virtual_fs") == NULL) {
        if (searchDepth > 0) {
            // 递归查找父进程
            targetPid++;
            searchDepth--;
            goto SearchProcess;
        }
        // 不包含 "virtual_fs"，不进行内存监视
        fprintf(stderr, "未找到包含virtual_fs的进程名, 不进行内存监视\n");
        writeLog("未找到包含virtual_fs的进程名, 不进行内存监视\n");
        exit(EXIT_SUCCESS);
    }
    // 设置信号处理函数
    signal(SIGTERM, handleMonitor);

    unsigned long memoryUsageMB;
    while (1) {
        if (proc_pidinfo(targetPid, PROC_PIDTASKINFO, 0, &taskInfo, sizeof(taskInfo)) <= 0) {
            asprintf((char **) &tmp_str1, "%d", targetPid);
            writeLog(strmerge((const char *[]) {"Failed to get process information,pid: ", tmp_str1, "\n"}));
            perror("Failed to get process information");
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
            writeLog(strmerge((const char *[]) { "时间: ",time_str,"\n内存使用超过阈值: ", tmp_str1, " MB\n", "内存使用: ",tmp_str2," MB\n"}) );
            exit_process(fp);
            fclose(fp);
            // 结束当前进程
            exit(EXIT_SUCCESS);
        } else if (memoryUsageMB > MEMORY_THRESHOLD / MEGABYTE / 3) {
            sendCollectLogSignal = 1;
            kill(targetPid, SIGUSR1);// 发送收集日志信号
        } else if (fileSizeCheck(Main_debugFilePath)) {
            FILE *fp = fopen(debugFilePath, "a");
            time(&current_time);
            strftime(time_str, time_str_size, "%Y-%m-%d %H:%M:%S", localtime(&current_time));
            // 写入格式化后的时间字符串到文件
            fprintf(fp, "时间: %s\n", time_str);
            fprintf(fp, "文件大小超过阈值: %zu MB\n", thresholdMB);
            asprintf((char **) &tmp_str1, "%zu", thresholdMB);
            writeLog(strmerge((const char *[]) { "时间: ",time_str,"\n文件大小超过阈值: ", tmp_str1, " MB\n"}));
            exit_process(fp);
            fclose(fp);
            execute_command("open", Main_debugFilePath);
            // 结束当前进程
            exit(EXIT_SUCCESS);
        }

        // 每隔一段时间检查一次，避免频繁检查
        sleep(10);
    }
}

int main(int argc, char *argv[]) {
    // 检查命令行参数数量
    if (argc < 3) {
        fprintf(stderr, "用法: %s <主进程Pid> <挂载路径>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    char *endptr;
    targetPid = (int) strtol(argv[1], &endptr, 10);
    fprintf(stderr, "targetPid: %d\n", targetPid);

    // 检查是否转换成功
    if (*endptr != '\0' || endptr == argv[1]) {
        fprintf(stderr, "Invalid number: %s\n", argv[1]);
        return 1;
    }
    point_path = argv[2];

    // 获取当前进程的pid
    pid = getpid();
    asprintf((char **) &pid_str, "%d", pid);

    sleep(3);// 等待 virtual_fs 进程启动
    monitorMemory();

    return 0;
}

// 该代码仅用于监控 virtual_fs 进程的内存使用情况,防止发生内存泄露
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/proc_info.h>
#include <libproc.h>

#define MEGABYTE (1024 * 1024)  // 1MB
#define MEMORY_THRESHOLD (10 * MEGABYTE)    // 10MB

// 全局变量，用于保存监视进程的pid
static pid_t targetPid;

// 信号处理函数
static void handleMonitor(__attribute__((unused)) int signal) {
    FILE *fp = fopen("/tmp/fs_Memory.log", "a");
    // 获取当前时间
    time_t current_time;
    time(&current_time);
    // 将时间格式化为字符串
    char time_str[100];  // 适当大小的字符数组
    strftime(time_str, sizeof(time_str), "时间: %Y-%m-%d %H:%M:%S", localtime(&current_time));
    // 写入格式化后的时间字符串到文件
    fprintf(fp, "%s\n", time_str);
    fprintf(fp, "监控进程被杀死, 开始杀死指定进程pid: %d\n", targetPid);
    fclose(fp);
    kill(targetPid, SIGTERM);
    // 结束当前进程
    exit(EXIT_SUCCESS);
}

void monitorMemory() {
    // 获取指定 pid 进程的内存使用情况
    struct proc_taskinfo taskInfo;
    unsigned short int searchDepth = 20;

    // 获取指定 pid 进程的名称
    char processName[PROC_PIDPATHINFO_MAXSIZE];
    SearchProcess:
    if (proc_pidpath(targetPid, processName, sizeof(processName)) <= 0) {
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
        fprintf(stderr, "未找到包含 virtual_fs的进程名, 不进行内存监视\n");
        exit(EXIT_SUCCESS);
    }
    // 设置信号处理函数
    signal(SIGTERM, handleMonitor);

    unsigned long memoryUsageMB;
    while (1) {
        if (proc_pidinfo(targetPid, PROC_PIDTASKINFO, 0, &taskInfo, sizeof(taskInfo)) <= 0) {
            perror("Failed to get process information");
            exit(EXIT_FAILURE);
        }
//        unsigned long memoryUsageKB = taskInfo.pti_resident_size / 1024;
        memoryUsageMB = taskInfo.pti_resident_size / 1024 / 1024;

        // 如果内存使用超过阈值，发送信号给主进程
        if (memoryUsageMB > MEMORY_THRESHOLD / MEGABYTE) {
            FILE *fp = fopen("/tmp/fs_Memory.log", "a");
            // 获取当前时间
            time_t current_time;
            time(&current_time);
            // 将时间格式化为字符串
            char time_str[100];  // 适当大小的字符数组
            strftime(time_str, sizeof(time_str), "时间: %Y-%m-%d %H:%M:%S", localtime(&current_time));
            // 写入格式化后的时间字符串到文件
            fprintf(fp, "%s\n", time_str);
            fprintf(fp, "内存使用超过阈值: %d MB\n", MEMORY_THRESHOLD / MEGABYTE);
            fprintf(fp, "内存使用: %ld MB\n", memoryUsageMB);
            fclose(fp);
            kill(targetPid, SIGTERM);
            // 结束当前进程
            exit(EXIT_SUCCESS);
        }
        // 每隔一段时间检查一次，避免频繁检查
        sleep(10);
    }
}
int main(int argc, char *argv[]) {
    // 检查命令行参数数量
    if (argc == 1) {
        fprintf(stderr, "用法: %s <监控进程pid>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    char *endptr;
    targetPid = (int)strtol(argv[1], &endptr, 10);

    // 检查是否转换成功
    if (*endptr != '\0' || endptr == argv[1]) {
        fprintf(stderr, "Invalid number: %s\n", argv[1]);
        return 1;
    }

    sleep(3);  // 等待 virtual_fs 进程启动
    monitorMemory();

    return 0;
}


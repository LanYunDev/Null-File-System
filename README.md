由于技术力有限,不太懂这个fuse.

欢迎大佬提PR,我会尽快合并的.

目前在参考代码基础上做了一点修改,能满足我的需求了.

我最初是想实现`文件黑洞系统`,即无论是读取/写入/创建等I/O操作都(欺骗性)返回成功,实际上并未做实际的文件I/O操作.

但,我不会整😂.

编译后(前提已安装osxfuse),用法: 

命令行运行编译出的二进制文件,带上参数 <挂载路径> <映射路径>

示例: `./virtual_fs /xx/挂载路径 /xx/映射路径`

其他参数请参考[osxfuse](https://github.com/osxfuse/osxfuse/wiki/FAQ)

效果: 挂载路径 中显示的内容为 映射路径 下的内容,但写入/读取操作均返回空(我测试是这样的,不代表未来也如此),其他操作**应该**和osx差不多(但有没有bug,能不能用,我就不知道了).

版本信息: macOS Sonoma 14.3 Apple Silicon
macFUSE 4.6.0 cmake 3.28.1 ninja 1.11.1

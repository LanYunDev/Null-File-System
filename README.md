由于技术力有限,不太懂这个fuse.

欢迎大佬提PR~  ~~(估计都是拿了就跑)~~

目前在参考代码基础上做了一点修改,能满足我的需求了.

我最初是想实现`文件黑洞系统`,即无论是读取/写入/创建等I/O操作都(欺骗性)返回成功,并未做实际的文件I/O操作. (但,我搞不来😂.)

编译后(前提已安装[fuse-t](https://github.com/macos-fuse-t/fuse-t)),用法: 

命令行运行编译出的二进制文件~~(如果有Releases版)~~,带上参数 <挂载路径>  ~~<(可选,默认:/)映射路径>~~ ~~(影响性能,已移除)~~ (没必要,用不上,已移除)

示例: `./virtual_fs /xx/挂载路径`

效果: 向 挂载路径 中,读取/写入"任意文件"均返回成功,"任意文件夹"均已创建. (这个效果可能存在一些问题,我只粗略测试通过了,欢迎提PR修复,提issue我不一定会修,也未必有时间搞.)

版本信息: Apple Silicon macOS Sonoma 14.3 macFUSE 4.6.0 cmake 3.28.1 ninja 1.11.1

备注: 任意文件 定义:(判断依据)存在后缀,且不为第一个字符不为数字.其他都识别为文件夹. 

`virtual_fs_macfuse.c` 为历史遗留代码,目前已从`osxfuse`换成`fuse-t`,取消对内核扩展的依赖.

`virtual_fs_origin.c` 为遗留代码,将原始函数实现作为参考.

Releases 默认仅适用于macOS arm64,其他平台请自行~~编译~~写代码(没全平台的技术力😂).

从v0.0.5版本开始,新增一个用于监控内存占用的二进制程序.程序结束后默认删除空目录.

启动`virtual_fs`,默认会启动`virtual_fs`所在路径下的`virtual_fs_monitor`.

~~暂~~不支持手动指定路径.需要保证这2个二进制在一起.否则启动失败.(我不会怎么弄在一起.就这样了)

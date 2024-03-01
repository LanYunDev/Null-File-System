## 前言

由于技术力有限,不太懂这个fuse.

欢迎大佬提PR~  ~~(估计都是拿了就跑)~~

目前在参考代码基础上做了一点修改,能满足我的需求了.

我最初是想实现`文件黑洞系统`,即无论是读取/写入/创建等I/O操作都(欺骗性)返回成功,并未做实际的文件I/O操作. (但,我搞不来😂.)

### 用法:

#### 前提:

已安装[fuse-t](https://github.com/macos-fuse-t/fuse-t),且有符合本机架构的二进制文件.建议使用自己编译后的二进制文件.Releases中暂只有arm64的二进制文件.

命令行运行编译出的二进制文件,带上参数 `[-delete] [-disable_blackMode] <挂载路径>`  ~~<(可选,默认:/)映射路径>~~ ~~(影响性能,已移除)~~ (没必要,用不上,已移除)

示例: `./virtual_fs /xx/挂载路径`

### 效果: 

向 挂载路径 中,读取/写入"任意文件"均返回成功,"任意文件夹"均已创建. (这个效果可能存在一些问题,我只粗略测试通过了,欢迎提PR修复,提issue我不一定会修,也未必有时间搞.)

### 参数解释: 

`-delete`: 删除挂载路径下的文件 `-disable_blackMode`: 禁用黑名单模式,启用白名单模式.

目前特殊名单有: `apache2`

目前黑名单有: (目前已停用⛔️)

目前白名单有: `JetBrains`

目前规则有: 1️⃣以`.`开头文件名执行策略是报错返回.

### 关于模式和名单解释: 

特殊名单: 起始路径位于特殊名单单内,对该路径下的任意名称访问均返回文件类型.例如: 获取/apache2/access_log信息,返回为文件类型.(算是对仅通过路径判断文件类型的一个补充,如果创建真实文件,就不会有这种问题了.但这使用了磁盘I/O)

动态黑名单: 起始路径(除了白名单外)位于动态黑名单内,对该路径执行策略是报错返回.目前动态黑名单最大数量设定为10.判定: 30秒内访问次数达到15次将进入动态黑名单.此名单中的记录不会主动移除,只会被 新数据 且 新数据在主程序中定义的`hashFunction`函数所得到相同的返回值的情况下 覆写.

黑名单模式: 起始路径位于黑名单内,对该路径执行策略是不伪装,报错返回.因为我搞不好,所以直接拉黑了.(但应该能修好,但不会😂)

白名单模式: 起始路径除了白名单(和根目录)外,都不进行伪装,报错返回.

注: 此处 起始路径 定义为 在原有路径上右移一位,即表示`/`之后的内容.如`/JetBrains/xx`,起始路径为`JetBrains/xx`.

### 关于日志:

如果发生内存泄露,监控程序会告诉主程序开始记录处理日志(地址: `/tmp/fs_debug.log`)

然后监控程序本身会记录内存泄露情况到日志(地址: `/tmp/fs_Memory.log`)中,并延迟结束主程序,随后监控程序本身也退出.

如果遇到内存泄露,可以将日志发到issue中,我会看看我会修不,不会修就拉黑名单看看OK不,不的话就寄咯.

### 备注: 

版本信息: Apple Silicon macOS Sonoma 14.3 macFUSE 4.6.0 cmake 3.28.1 ninja 1.11.1

任意文件 定义:(判断依据)存在后缀,且不为第一个字符不为数字.其他都识别为文件夹. 

文中描述**主程序**是指: `virtual_fs.c` ,**监控程序**是指: `virtual_fs_monitor`

`virtual_fs_macfuse.c` 为历史遗留代码,目前已从`osxfuse`换成`fuse-t`,取消对内核扩展的依赖.

`virtual_fs_origin.c` 为遗留代码,将原始函数实现作为参考.

Releases 默认仅适用于macOS arm64,其他平台请自行~~编译~~写代码(没全平台的技术力😂).

从v0.0.5版本开始,新增一个用于监控内存占用的二进制程序.程序结束后默认删除空目录.

从v1.1.2版本开始,新增动态黑名单(以解决可能出现的严重内存泄露),当存在某个路径频繁访问时,将会把这个路径加入动态黑名单中.

启动`virtual_fs`,默认会启动`virtual_fs`所在路径下的`virtual_fs_monitor`.

~~暂~~不支持手动指定路径.需要保证这2个二进制在一起.否则启动失败.(我不会怎么弄在一起.就这样了)

不支持`SMB`作为后端,目前仅支持`NFS`作为后端.

关于内存泄露,通过leaks工具可以看到存在内存泄露,例如`OS_xpc_pipe ObjC libxpc.dylib`等.

内存泄露(大概率)不是由我写的代码引入的,而是原本就存在内存泄露的问题.

由于某些情况下会出现内存泄露问题,故做了黑名单和监控程序等防止内存泄露并试图结束主程序.

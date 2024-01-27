cmake_minimum_required(VERSION 3.27)
project(VirtualFS C)
set(CMAKE_C_STANDARD 23)

# 指定 macFUSE 的头文件路径
set(MAC_FUSE_INCLUDE_DIRS /usr/local/include)

# 指定 macFUSE 的库文件路径
set(MAC_FUSE_LIBRARIES /usr/local/lib/libfuse.dylib)

# 添加 macFUSE 的包含目录，并添加宏定义
add_definitions(-D_FILE_OFFSET_BITS=64)
add_definitions(-D_DARWIN_USE_64_BIT_INODE)
include_directories(${MAC_FUSE_INCLUDE_DIRS})

# 为项目设置源文件
set(SOURCE_FILES virtual_fs.c)

# 创建可执行文件
add_executable(virtual_fs ${SOURCE_FILES})

# 链接 macFUSE 库
target_link_libraries(virtual_fs ${MAC_FUSE_LIBRARIES})


cmake_minimum_required(VERSION 3.27)
project(VirtualFS C)
set(CMAKE_C_STANDARD 23)
set(CMAKE_OSX_ARCHITECTURES arm64;x86_64)

# 指定 macFUSE 的头文件路径
#set(MAC_FUSE_INCLUDE_DIRS /usr/local/include)
set(MAC_FUSE_INCLUDE_DIRS "/usr/local/include/fuse")

# 指定 macFUSE 的库文件路径
set(MAC_FUSE_LIBRARIES "/usr/local/lib/libfuse-t.dylib")

# 添加 macFUSE 的包含目录，并添加宏定义
#add_definitions(-D_FILE_OFFSET_BITS=64)
#add_definitions(-D_DARWIN_USE_64_BIT_INODE)
add_definitions(-D_FILE_OFFSET_BITS=64 -D_REENTRANT)
#set(LIBS "-liconv -framework CoreFoundation -framework DiskArbitration")
include_directories(${MAC_FUSE_INCLUDE_DIRS})

# 为项目设置源文件
set(SOURCE_FILES virtual_fs.c)

# 创建可执行文件
add_executable(virtual_fs ${SOURCE_FILES})
add_executable(virtual_fs_monitor virtual_fs_monitor.c)

# 链接 macFUSE 库
#target_link_libraries(virtual_fs ${MAC_FUSE_LIBRARIES})
#target_link_libraries (virtual_fs LINK_PUBLIC fuse-t ${LIBS})
target_link_libraries (virtual_fs LINK_PUBLIC ${MAC_FUSE_LIBRARIES} ${LIBS})

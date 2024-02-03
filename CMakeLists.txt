cmake_minimum_required(VERSION 3.27)
project(VirtualFS C)
set(CMAKE_C_STANDARD 23)
set(CMAKE_OSX_ARCHITECTURES arm64;x86_64)

# 设置构建类型，默认为Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 设置编译器优化选项
# 设置Release模式下的编译标志
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

# 设置Debug模式下的编译标志
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")

# 指定 fuse-t 的头文件路径
set(FUSE_INCLUDE_DIRS "/usr/local/include/fuse")
# 指定 macFUSE 的头文件路径
#set(MAC_FUSE_INCLUDE_DIRS /usr/local/include)

# 指定 fuse-t 的库文件路径
set(FUSE_LIBRARIES "/usr/local/lib/libfuse-t.dylib")
# 指定 macFUSE 的库文件路径
#set(MAC_FUSE_LIBRARIES /usr/local/lib/libfuse.dylib)

## 添加 fuse-t 的包含目录，并添加宏定义
add_definitions(-D_FILE_OFFSET_BITS=64 -D_REENTRANT)
#set(LIBS "-liconv -framework CoreFoundation -framework DiskArbitration")
include_directories(${FUSE_INCLUDE_DIRS})
# 添加 macFUSE 的包含目录，并添加宏定义
#add_definitions(-D_FILE_OFFSET_BITS=64)
#add_definitions(-D_DARWIN_USE_64_BIT_INODE)
#include_directories(${MAC_FUSE_INCLUDE_DIRS})

# 为项目设置源文件
set(SOURCE_FILES virtual_fs.c)

# 创建可执行文件
add_executable(virtual_fs ${SOURCE_FILES})
add_executable(virtual_fs_monitor virtual_fs_monitor.c)

## 链接 fuse-t 库
target_link_libraries (virtual_fs LINK_PUBLIC ${FUSE_LIBRARIES} ${LIBS})
# 链接 macFUSE 库
#target_link_libraries(virtual_fs ${MAC_FUSE_LIBRARIES})

# 根据构建类型设置编译选项
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(virtual_fs PRIVATE DEBUG)
endif()

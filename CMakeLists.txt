# Kernel-key-mapping-ai — 内核驱动键鼠映射
# 目标平台: Android (aarch64)，需 Root + 内核驱动(twt/rt/qx/zero...)
# 也可在标准 Linux 上编译运行测试逻辑。
cmake_minimum_required(VERSION 3.10)
project(kma_mapper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 编译选项：去掉不必要的警告，保留内核相关定义
add_compile_options(-Wall -Wno-unused-parameter -Wno-unused-variable)

# Android 平台额外处理
if(ANDROID)
    add_definitions(-D__ANDROID__)
endif()

add_executable(kma_mapper
    src/main.cpp
    src/driver_adapter.cpp
    src/input_reader.cpp
    src/mapper.cpp
)

target_include_directories(kma_mapper PRIVATE src)

# Linux/Android 都有 epoll、input.h，无需额外库
find_package(Threads REQUIRED)
target_link_libraries(kma_mapper PRIVATE Threads::Threads)

# 安装
install(TARGETS kma_mapper RUNTIME DESTINATION bin)

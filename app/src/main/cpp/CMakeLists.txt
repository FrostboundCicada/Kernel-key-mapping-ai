# Kernel-key-mapping-ai — Android JNI 共享库
# 构建共享库 libkma.so，桥接 Kotlin UI 与 C++ 映射核心。
cmake_minimum_required(VERSION 3.22.1)
project(kma LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 共享库 kma —— 引用仓库根 src/ 下的 C++ 核心 + JNI 桥接
add_library(kma SHARED
    ../../../src/driver_adapter.cpp
    ../../../src/input_reader.cpp
    ../../../src/mapper.cpp
    kma_jni.cpp
)

target_include_directories(kma PRIVATE ../../../src)

# Android 平台库
find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(kma
    ${log-lib}
    ${android-lib}
)

# C++17 + 异常 + RTTI
target_compile_options(kma PRIVATE
    -fexceptions
    -frtti
)

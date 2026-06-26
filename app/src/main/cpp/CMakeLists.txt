# Kernel-key-mapping-ai — Android JNI 共享库
# 构建共享库 libkma.so，桥接 Kotlin UI 与 C++ 映射核心。
cmake_minimum_required(VERSION 3.22.1)
project(kma LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 仓库根目录：当前文件在 app/src/main/cpp/，路径 cpp->main->app->src->根，向上 4 级
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(KMA_SRC_DIR "${REPO_ROOT}/src")

# 共享库 kma —— 引用仓库根 src/ 下的 C++ 核心 + JNI 桥接
add_library(kma SHARED
    ${KMA_SRC_DIR}/driver_adapter.cpp
    ${KMA_SRC_DIR}/input_reader.cpp
    ${KMA_SRC_DIR}/mapper.cpp
    kma_jni.cpp
)

target_include_directories(kma PRIVATE ${KMA_SRC_DIR})

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
    -Wno-unused-parameter
)

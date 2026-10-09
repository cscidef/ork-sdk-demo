# =============================================================================
#  Ork SDK — C / C++ 示例工程（顶层 CMake）
#
#  本目录是一个 **完全自包含** 的示例工程：
#    include/            Ork SDK 公开头文件（C ABI + C++ SDK）
#    thirdparty/include/ 随包分发的 header-only 依赖（asio / fmt / ikcp / ...）
#    lib/<triplet>/      预编译静态库（libork_core.a、libork_platform.a、...）
#    cmake/FindOrk.cmake 把上面三者装配成 Ork::Core 这个 IMPORTED 目标
#
#  因此构建 demo **不需要** harmonia_ork 源码仓，只需要机器上有 OpenSSL 3 与
#  libsodium（它们不进包，见 cpp/README.md「前置条件」）。
#
#  用法：
#    cmake -B build -DCMAKE_BUILD_TYPE=Debug
#    cmake --build build -j
#    cd build && ctest --output-on-failure
# =============================================================================
cmake_minimum_required(VERSION 3.20)

project(OrkSdkCppDemo
        VERSION 1.0.0
        DESCRIPTION "Ork SDK C/C++ integration demos"
        LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# 语言标准
#   C   —— C11：C ABI 头（ork_abi.h / ork_client_abi.h）的最低要求，
#                用到匿名初始化宏 + <stdint.h> + <stdbool.h>
#   C++ —— C++20：SDK 公开头使用 std::span、<source_location>、嵌套命名空间
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
             Debug Release RelWithDebInfo MinSizeRel)

# ---------------------------------------------------------------------------
# 定位预编译 Ork SDK。
# 产出三个 IMPORTED 目标：
#   Ork::Core        libork_core.a + 全部下游依赖（demo 只需链接这一个）
#   Ork::Platform    libork_platform.a（很少需要单独用）
#   Ork::Thirdparty  vendored header-only 依赖的 include 路径
# 以及 Ork::Core 上挂载的编译宏 ORK_HAVE_TLS / ORK_ALLOW_PLAINTEXT_BOOTSTRAP
# （必须与预编译库的构建配置一致，否则 OrkClient 的成员布局会漂移 → ODR 破坏）。
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(Ork REQUIRED)

# 所有 demo / test 可执行文件统一落到 build/bin，README 里的命令才好写，
# 对多配置生成器（Xcode / Visual Studio）也一并生效。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# C++ demo / test 共用的小工具头（日志初始化、临时端口、25B 探针帧、配置模板）。
# 必须在定义 ork_configure_demo() 之前捕获绝对路径：函数体内的
# CMAKE_CURRENT_SOURCE_DIR 会解析成 *调用方* 的目录，而不是顶层。
set(ORK_DEMO_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/demos/common")

# ---------------------------------------------------------------------------
# ork_configure_demo(<target>) —— 统一的 demo / test 目标装配
#
#   1. 链接 Ork::Core：一次性带进 include 路径、编译宏、静态库与系统库。
#   2. LINKER_LANGUAGE CXX：**关键**。libork_core.a / libork_platform.a 是
#      C++ 归档（asio、std::span、异常、RTTI、静态初始化）。纯 C 的 main.c
#      如果按 C 链接，编译器驱动不会带上 C++ 运行时库，链接期会炸出成片的
#      undefined symbol（__cxa_*、operator new、std::__1::*）。
#   3. 打开常用告警 + Windows 上 asio 需要的宏。
# ---------------------------------------------------------------------------
function(ork_configure_demo target)
    target_link_libraries("${target}" PRIVATE Ork::Core)
    set_target_properties("${target}" PROPERTIES LINKER_LANGUAGE CXX)

    # demos/common/demo_support.hpp（纯 C 目标用不到，但挂着也无害）
    target_include_directories("${target}" PRIVATE "${ORK_DEMO_COMMON_DIR}")

    if(MSVC)
        target_compile_options("${target}" PRIVATE /W4 /utf-8 /bigobj /permissive-)
        target_compile_definitions("${target}" PRIVATE
            _WIN32_WINNT=0x0A00
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options("${target}" PRIVATE -Wall -Wextra)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# ctest
# ---------------------------------------------------------------------------
enable_testing()

add_subdirectory(demos)
add_subdirectory(tests)

message(STATUS "-----------------------------------------------------------")
message(STATUS " Ork C/C++ demos — configure summary")
message(STATUS "   triplet    : ${ORK_PLATFORM}")
message(STATUS "   C ABI ver  : ${ORK_ABI_VERSION}")
message(STATUS "   headers    : ${ORK_INCLUDE_DIR}")
message(STATUS "   vendored   : ${ORK_THIRDPARTY_INCLUDE_DIR}")
message(STATUS "   libraries  : ${ORK_LIBRARY_DIR}")
message(STATUS "   OpenSSL    : ${OPENSSL_VERSION}")
message(STATUS "   libsodium  : ${SODIUM_LIBRARY}")
message(STATUS "   binaries   : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "-----------------------------------------------------------")

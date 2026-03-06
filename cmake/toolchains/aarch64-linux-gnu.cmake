cmake_minimum_required(VERSION 3.20)

# Generic Linux ARM64 cross-compilation toolchain.
#
# Environment variables:
#   ARM_GNU_TOOLCHAIN_ROOT - Optional toolchain root containing bin/<triple>-gcc
#   ARM_TARGET_TRIPLE      - Optional target triple (default: aarch64-linux-gnu)
#   ARM_SYSROOT            - Optional target sysroot path
#
# Usage:
#   export ARM_GNU_TOOLCHAIN_ROOT=/opt/toolchains/gcc-aarch64
#   export ARM_SYSROOT=/opt/sysroots/aarch64-linux-gnu
#   cmake --preset arm64-release

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_CROSSCOMPILING TRUE)

if(DEFINED ENV{ARM_TARGET_TRIPLE} AND NOT "$ENV{ARM_TARGET_TRIPLE}" STREQUAL "")
    set(ARM_TARGET_TRIPLE "$ENV{ARM_TARGET_TRIPLE}")
else()
    set(ARM_TARGET_TRIPLE "aarch64-linux-gnu")
endif()

if(DEFINED ENV{ARM_GNU_TOOLCHAIN_ROOT} AND NOT "$ENV{ARM_GNU_TOOLCHAIN_ROOT}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{ARM_GNU_TOOLCHAIN_ROOT}" ARM_GNU_TOOLCHAIN_ROOT)
else()
    set(ARM_GNU_TOOLCHAIN_ROOT "")
endif()

if(DEFINED ENV{ARM_SYSROOT} AND NOT "$ENV{ARM_SYSROOT}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{ARM_SYSROOT}" ARM_SYSROOT)
else()
    set(ARM_SYSROOT "")
endif()

if(ARM_GNU_TOOLCHAIN_ROOT)
    set(ARM_TOOLCHAIN_BIN "${ARM_GNU_TOOLCHAIN_ROOT}/bin")
    set(CMAKE_C_COMPILER "${ARM_TOOLCHAIN_BIN}/${ARM_TARGET_TRIPLE}-gcc")
    set(CMAKE_CXX_COMPILER "${ARM_TOOLCHAIN_BIN}/${ARM_TARGET_TRIPLE}-g++")

    if(NOT EXISTS "${CMAKE_C_COMPILER}")
        message(
            FATAL_ERROR
            "C compiler not found at ${CMAKE_C_COMPILER}. "
            "Check ARM_GNU_TOOLCHAIN_ROOT and ARM_TARGET_TRIPLE."
        )
    endif()

    if(NOT EXISTS "${CMAKE_CXX_COMPILER}")
        message(
            FATAL_ERROR
            "CXX compiler not found at ${CMAKE_CXX_COMPILER}. "
            "Check ARM_GNU_TOOLCHAIN_ROOT and ARM_TARGET_TRIPLE."
        )
    endif()
else()
    find_program(ARM_GCC "${ARM_TARGET_TRIPLE}-gcc")
    find_program(ARM_GXX "${ARM_TARGET_TRIPLE}-g++")

    if(NOT ARM_GCC OR NOT ARM_GXX)
        message(
            FATAL_ERROR
            "Could not find ${ARM_TARGET_TRIPLE}-gcc / ${ARM_TARGET_TRIPLE}-g++ on PATH. "
            "Install a cross toolchain or set ARM_GNU_TOOLCHAIN_ROOT."
        )
    endif()

    set(CMAKE_C_COMPILER "${ARM_GCC}")
    set(CMAKE_CXX_COMPILER "${ARM_GXX}")
endif()

if(ARM_SYSROOT)
    if(NOT EXISTS "${ARM_SYSROOT}")
        message(
            FATAL_ERROR
            "ARM sysroot not found at ${ARM_SYSROOT}. Check ARM_SYSROOT."
        )
    endif()

    set(CMAKE_SYSROOT "${ARM_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH "${ARM_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

    set(_sysroot_flag "--sysroot=${CMAKE_SYSROOT}")
    set(CMAKE_C_FLAGS_INIT "${_sysroot_flag}")
    set(CMAKE_CXX_FLAGS_INIT "${_sysroot_flag}")
    set(CMAKE_EXE_LINKER_FLAGS_INIT "${_sysroot_flag}")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_sysroot_flag}")
else()
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE NEVER)
endif()

# Avoid try-run during compiler checks when cross-compiling.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(ARM_CROSS_COMPILE TRUE CACHE BOOL "Indicates generic ARM cross-compilation")
set(ARM_TARGET_TRIPLE "${ARM_TARGET_TRIPLE}" CACHE STRING "ARM target triple")
set(ARM_GNU_TOOLCHAIN_ROOT "${ARM_GNU_TOOLCHAIN_ROOT}" CACHE PATH "Toolchain root")
set(ARM_SYSROOT "${ARM_SYSROOT}" CACHE PATH "Target sysroot")

message(STATUS "Generic ARM64 toolchain configuration:")
message(STATUS "  Target triple: ${ARM_TARGET_TRIPLE}")
message(STATUS "  Toolchain root: ${ARM_GNU_TOOLCHAIN_ROOT}")
message(STATUS "  Sysroot: ${ARM_SYSROOT}")
message(STATUS "  C compiler: ${CMAKE_C_COMPILER}")
message(STATUS "  CXX compiler: ${CMAKE_CXX_COMPILER}")

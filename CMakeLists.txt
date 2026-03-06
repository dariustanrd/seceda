cmake_minimum_required(VERSION 3.29)

project(
    seceda
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Edge-first hybrid inference build orchestration"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Top-level build selection
option(SECEDA_BUILD_LLAMA_CPP "Build llama.cpp" ON)
option(SECEDA_BUILD_EXECUTORCH "Build ExecuTorch" ON)
option(SECEDA_BUILD_CUSTOM "Build custom code in src/" ON)

# Backend options shared across downstream components
option(SECEDA_ENABLE_VULKAN "Enable Vulkan backends where available" OFF)
option(SECEDA_ENABLE_METAL "Enable Metal backend support where available" OFF)
option(SECEDA_ENABLE_XNNPACK "Enable XNNPACK backend support" ON)
option(SECEDA_ENABLE_OPENMP "Enable OpenMP support for llama.cpp" ON)

# llama.cpp specific options
option(SECEDA_LLAMA_ENABLE_KLEIDIAI "Enable KleidiAI CPU kernels for llama.cpp" OFF)
option(SECEDA_LLAMA_ENABLE_CURL "Enable CURL support for llama.cpp" OFF)
option(SECEDA_LLAMA_CPU_AARCH64 "Enable AArch64 CPU optimizations for llama.cpp" OFF)

# ExecuTorch specific options
option(SECEDA_EXECUTORCH_ENABLE_LLM "Enable ExecuTorch LLM extensions" ON)
option(SECEDA_EXECUTORCH_ENABLE_QUANTIZED "Enable ExecuTorch quantized kernels" ON)
option(SECEDA_EXECUTORCH_ENABLE_APPLE "Enable ExecuTorch Apple backends and extensions" OFF)
option(SECEDA_EXECUTORCH_ENABLE_METAL "Enable experimental ExecuTorch Metal backend" OFF)

set(SECEDA_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(SECEDA_THIRDPARTY_DIR ${SECEDA_ROOT_DIR}/thirdparty)
set(SECEDA_LLAMA_CPP_DIR ${SECEDA_THIRDPARTY_DIR}/llama.cpp)
set(SECEDA_EXECUTORCH_DIR ${SECEDA_THIRDPARTY_DIR}/executorch)
set(SECEDA_BUILD_DIR ${CMAKE_BINARY_DIR})
set(SECEDA_INSTALL_DIR ${CMAKE_BINARY_DIR}/install)

if(SECEDA_BUILD_LLAMA_CPP AND NOT EXISTS "${SECEDA_LLAMA_CPP_DIR}/CMakeLists.txt")
    message(
        FATAL_ERROR
        "llama.cpp submodule is missing. Run: git submodule update --init --recursive"
    )
endif()

if(SECEDA_BUILD_EXECUTORCH AND NOT EXISTS "${SECEDA_EXECUTORCH_DIR}/CMakeLists.txt")
    message(
        FATAL_ERROR
        "ExecuTorch submodule is missing. Run: git submodule update --init --recursive"
    )
endif()

if((SECEDA_ENABLE_METAL OR SECEDA_EXECUTORCH_ENABLE_APPLE OR SECEDA_EXECUTORCH_ENABLE_METAL) AND NOT APPLE)
    message(
        FATAL_ERROR
        "Apple-specific build options require an Apple target. "
        "Use the Apple presets on macOS or disable Metal/Apple flags."
    )
endif()

if(SECEDA_EXECUTORCH_ENABLE_METAL)
    message(WARNING "ExecuTorch Metal backend is experimental.")
endif()

# Detect cross-compilation and enable ARM64-specific optimizations when appropriate.
if(CMAKE_CROSSCOMPILING)
    message(STATUS "Cross-compilation detected")
    message(STATUS "  Target system: ${CMAKE_SYSTEM_NAME}")
    message(STATUS "  Target processor: ${CMAKE_SYSTEM_PROCESSOR}")

    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(SECEDA_LLAMA_CPU_AARCH64 ON CACHE BOOL "Enable AArch64 optimizations" FORCE)
    endif()
endif()

if(SECEDA_ENABLE_VULKAN)
    if(DEFINED ENV{VULKAN_SDK})
        set(VULKAN_SDK "$ENV{VULKAN_SDK}")
        message(STATUS "Vulkan SDK: ${VULKAN_SDK}")
        find_program(GLSLC_EXECUTABLE glslc HINTS "${VULKAN_SDK}/bin" REQUIRED)
    else()
        find_program(GLSLC_EXECUTABLE glslc REQUIRED)
    endif()
    message(STATUS "GLSLC compiler: ${GLSLC_EXECUTABLE}")
endif()

if(SECEDA_BUILD_EXECUTORCH)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    message(STATUS "Python interpreter: ${Python3_EXECUTABLE}")
endif()

include(ExternalProject)

if(SECEDA_BUILD_LLAMA_CPP)
    message(STATUS "")
    message(STATUS "=== Configuring llama.cpp ===")

    set(
        LLAMA_CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_INSTALL_PREFIX=${SECEDA_INSTALL_DIR}/llama.cpp
        -DLLAMA_CURL=${SECEDA_LLAMA_ENABLE_CURL}
        -DLLAMA_BUILD_TESTS=OFF
        -DLLAMA_BUILD_EXAMPLES=OFF
        -DLLAMA_BUILD_TOOLS=ON
        -DLLAMA_BUILD_SERVER=ON
        -DGGML_CPU_AARCH64=${SECEDA_LLAMA_CPU_AARCH64}
        "-DCMAKE_INSTALL_RPATH=\$ORIGIN/../lib:\$ORIGIN"
    )

    if(CMAKE_TOOLCHAIN_FILE)
        list(APPEND LLAMA_CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
    endif()

    if(CMAKE_CROSSCOMPILING)
        list(APPEND LLAMA_CMAKE_ARGS -DGGML_NATIVE=OFF)
    endif()

    if(SECEDA_ENABLE_OPENMP)
        list(APPEND LLAMA_CMAKE_ARGS -DGGML_OPENMP=ON)
    else()
        list(APPEND LLAMA_CMAKE_ARGS -DGGML_OPENMP=OFF)
    endif()

    if(SECEDA_ENABLE_VULKAN)
        list(APPEND LLAMA_CMAKE_ARGS -DGGML_VULKAN=ON)
        list(APPEND LLAMA_CMAKE_ARGS -DVulkan_GLSLC_EXECUTABLE=${GLSLC_EXECUTABLE})
        if(DEFINED VULKAN_SDK)
            list(APPEND LLAMA_CMAKE_ARGS -DVulkan_INCLUDE_DIR=${VULKAN_SDK}/include)
        endif()
    else()
        list(APPEND LLAMA_CMAKE_ARGS -DGGML_VULKAN=OFF)
    endif()

    if(SECEDA_ENABLE_METAL)
        list(APPEND LLAMA_CMAKE_ARGS -DGGML_METAL=ON)
    else()
        list(APPEND LLAMA_CMAKE_ARGS -DGGML_METAL=OFF)
    endif()

    if(SECEDA_LLAMA_ENABLE_KLEIDIAI)
        list(
            APPEND
            LLAMA_CMAKE_ARGS
            -DGGML_CPU_KLEIDIAI=ON
            -DGGML_SYSTEM_ARCH=ARM
            -DGGML_NATIVE=OFF
            -DGGML_CPU_ARM_ARCH=armv8-a
            -DGGML_SSE42=OFF
            -DGGML_F16C=OFF
            -DGGML_FMA=OFF
            -DGGML_BMI2=OFF
            -DGGML_AVX=OFF
            -DGGML_AVX2=OFF
        )
    endif()

    ExternalProject_Add(
        llama_cpp
        SOURCE_DIR ${SECEDA_LLAMA_CPP_DIR}
        BINARY_DIR ${SECEDA_BUILD_DIR}/llama.cpp-build
        CMAKE_ARGS ${LLAMA_CMAKE_ARGS}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel
        INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
    )
endif()

if(SECEDA_BUILD_EXECUTORCH)
    message(STATUS "")
    message(STATUS "=== Configuring ExecuTorch ===")

    set(
        EXECUTORCH_CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_INSTALL_PREFIX=${SECEDA_INSTALL_DIR}/executorch
        -DPYTHON_EXECUTABLE=${Python3_EXECUTABLE}
        -DEXECUTORCH_BUILD_TESTS=OFF
        -DEXECUTORCH_BUILD_DEVTOOLS=OFF
        -DEXECUTORCH_BUILD_PYBIND=OFF
        -DEXECUTORCH_BUILD_EXECUTOR_RUNNER=OFF
        -DEXECUTORCH_BUILD_EXTENSION_DATA_LOADER=ON
        -DEXECUTORCH_BUILD_EXTENSION_FLAT_TENSOR=ON
        -DEXECUTORCH_BUILD_EXTENSION_MODULE=ON
        -DEXECUTORCH_BUILD_EXTENSION_NAMED_DATA_MAP=ON
        -DEXECUTORCH_BUILD_EXTENSION_RUNNER_UTIL=ON
        -DEXECUTORCH_BUILD_EXTENSION_TENSOR=ON
        -DEXECUTORCH_BUILD_KERNELS_OPTIMIZED=ON
        "-DCMAKE_INSTALL_RPATH=\$ORIGIN/../lib:\$ORIGIN"
    )

    if(CMAKE_TOOLCHAIN_FILE)
        list(APPEND EXECUTORCH_CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
    endif()

    if(SECEDA_ENABLE_XNNPACK)
        list(APPEND EXECUTORCH_CMAKE_ARGS -DEXECUTORCH_BUILD_XNNPACK=ON)
    else()
        list(APPEND EXECUTORCH_CMAKE_ARGS -DEXECUTORCH_BUILD_XNNPACK=OFF)
    endif()

    if(SECEDA_ENABLE_VULKAN)
        list(APPEND EXECUTORCH_CMAKE_ARGS -DEXECUTORCH_BUILD_VULKAN=ON)
    else()
        list(APPEND EXECUTORCH_CMAKE_ARGS -DEXECUTORCH_BUILD_VULKAN=OFF)
    endif()

    if(SECEDA_EXECUTORCH_ENABLE_APPLE)
        list(
            APPEND
            EXECUTORCH_CMAKE_ARGS
            -DEXECUTORCH_BUILD_COREML=ON
            -DEXECUTORCH_BUILD_MPS=ON
            -DEXECUTORCH_BUILD_EXTENSION_APPLE=ON
        )
    else()
        list(
            APPEND
            EXECUTORCH_CMAKE_ARGS
            -DEXECUTORCH_BUILD_COREML=OFF
            -DEXECUTORCH_BUILD_MPS=OFF
            -DEXECUTORCH_BUILD_EXTENSION_APPLE=OFF
        )
    endif()

    if(SECEDA_EXECUTORCH_ENABLE_METAL)
        list(APPEND EXECUTORCH_CMAKE_ARGS -DEXECUTORCH_BUILD_METAL=ON)
    else()
        list(APPEND EXECUTORCH_CMAKE_ARGS -DEXECUTORCH_BUILD_METAL=OFF)
    endif()

    if(SECEDA_EXECUTORCH_ENABLE_LLM)
        list(
            APPEND
            EXECUTORCH_CMAKE_ARGS
            -DEXECUTORCH_BUILD_EXTENSION_LLM=ON
            -DEXECUTORCH_BUILD_EXTENSION_LLM_RUNNER=ON
            -DEXECUTORCH_BUILD_KERNELS_LLM=ON
        )

        if(SECEDA_EXECUTORCH_ENABLE_APPLE)
            list(APPEND EXECUTORCH_CMAKE_ARGS -DEXECUTORCH_BUILD_EXTENSION_LLM_APPLE=ON)
        else()
            list(APPEND EXECUTORCH_CMAKE_ARGS -DEXECUTORCH_BUILD_EXTENSION_LLM_APPLE=OFF)
        endif()
    else()
        list(
            APPEND
            EXECUTORCH_CMAKE_ARGS
            -DEXECUTORCH_BUILD_EXTENSION_LLM=OFF
            -DEXECUTORCH_BUILD_EXTENSION_LLM_RUNNER=OFF
            -DEXECUTORCH_BUILD_KERNELS_LLM=OFF
            -DEXECUTORCH_BUILD_EXTENSION_LLM_APPLE=OFF
        )
    endif()

    if(SECEDA_EXECUTORCH_ENABLE_QUANTIZED)
        list(APPEND EXECUTORCH_CMAKE_ARGS -DEXECUTORCH_BUILD_KERNELS_QUANTIZED=ON)
    else()
        list(APPEND EXECUTORCH_CMAKE_ARGS -DEXECUTORCH_BUILD_KERNELS_QUANTIZED=OFF)
    endif()

    ExternalProject_Add(
        executorch_lib
        SOURCE_DIR ${SECEDA_EXECUTORCH_DIR}
        BINARY_DIR ${SECEDA_BUILD_DIR}/executorch-build
        CMAKE_ARGS ${EXECUTORCH_CMAKE_ARGS}
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --parallel
        INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
    )
endif()

if(SECEDA_BUILD_CUSTOM)
    if(EXISTS ${SECEDA_ROOT_DIR}/src/CMakeLists.txt)
        message(STATUS "")
        message(STATUS "=== Configuring custom code ===")
        add_subdirectory(src)
    else()
        message(STATUS "No src/CMakeLists.txt found - skipping custom code build")
    endif()
endif()

message(STATUS "")
message(STATUS "=== Build Configuration Summary ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Install prefix: ${SECEDA_INSTALL_DIR}")
message(STATUS "")
message(STATUS "Components:")
message(STATUS "  llama.cpp: ${SECEDA_BUILD_LLAMA_CPP}")
message(STATUS "  ExecuTorch: ${SECEDA_BUILD_EXECUTORCH}")
message(STATUS "  Custom code: ${SECEDA_BUILD_CUSTOM}")
message(STATUS "")
message(STATUS "Backends:")
message(STATUS "  Vulkan: ${SECEDA_ENABLE_VULKAN}")
message(STATUS "  Metal: ${SECEDA_ENABLE_METAL}")
message(STATUS "  XNNPACK: ${SECEDA_ENABLE_XNNPACK}")
message(STATUS "  OpenMP: ${SECEDA_ENABLE_OPENMP}")
if(SECEDA_BUILD_LLAMA_CPP)
    message(STATUS "")
    message(STATUS "llama.cpp options:")
    message(STATUS "  KleidiAI: ${SECEDA_LLAMA_ENABLE_KLEIDIAI}")
    message(STATUS "  CURL: ${SECEDA_LLAMA_ENABLE_CURL}")
    message(STATUS "  AArch64: ${SECEDA_LLAMA_CPU_AARCH64}")
endif()
if(SECEDA_BUILD_EXECUTORCH)
    message(STATUS "")
    message(STATUS "ExecuTorch options:")
    message(STATUS "  LLM extensions: ${SECEDA_EXECUTORCH_ENABLE_LLM}")
    message(STATUS "  Quantized kernels: ${SECEDA_EXECUTORCH_ENABLE_QUANTIZED}")
    message(STATUS "  Apple backends/extensions: ${SECEDA_EXECUTORCH_ENABLE_APPLE}")
    message(STATUS "  Metal backend (experimental): ${SECEDA_EXECUTORCH_ENABLE_METAL}")
endif()
message(STATUS "")

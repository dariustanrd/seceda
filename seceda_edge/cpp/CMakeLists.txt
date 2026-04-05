cmake_minimum_required(VERSION 3.29)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LLAMA_CPP_INSTALL_DIR ${SECEDA_INSTALL_DIR}/llama.cpp)
set(EXECUTORCH_INSTALL_DIR ${SECEDA_INSTALL_DIR}/executorch)
set(EXECUTORCH_BUILD_DIR ${SECEDA_BUILD_DIR}/executorch-build)

set(
    SECEDA_EDGE_CPP_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SECEDA_ROOT_DIR}
)

set(SECEDA_LLAMA_VENDOR_DIR ${SECEDA_LLAMA_CPP_DIR}/vendor)

include(FetchContent)

FetchContent_Declare(
    tomlplusplus
    URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.3.0.tar.gz
)
message(STATUS "Fetching toml++ v3.3.0")
FetchContent_MakeAvailable(tomlplusplus)

if(SECEDA_BUILD_LLAMA_CPP)
    set(LLAMA_CPP_INCLUDE_DIRS ${LLAMA_CPP_INSTALL_DIR}/include)
    set(LLAMA_CPP_LIBRARY_DIRS ${LLAMA_CPP_INSTALL_DIR}/lib)

    message(STATUS "llama.cpp include: ${LLAMA_CPP_INCLUDE_DIRS}")
    message(STATUS "llama.cpp lib: ${LLAMA_CPP_LIBRARY_DIRS}")
endif()

if(SECEDA_BUILD_EXECUTORCH)
    list(APPEND CMAKE_PREFIX_PATH ${EXECUTORCH_INSTALL_DIR})
    list(APPEND CMAKE_PREFIX_PATH ${EXECUTORCH_INSTALL_DIR}/lib/cmake/ExecuTorch)
    list(APPEND CMAKE_FIND_ROOT_PATH ${EXECUTORCH_INSTALL_DIR})
    list(APPEND CMAKE_FIND_ROOT_PATH ${EXECUTORCH_BUILD_DIR})

    set(gflags_DIR ${EXECUTORCH_BUILD_DIR}/third-party/gflags)

    find_package(executorch CONFIG QUIET)

    if(executorch_FOUND)
        message(STATUS "Found ExecuTorch via find_package")
        message(STATUS "  EXECUTORCH_INCLUDE_DIRS: ${EXECUTORCH_INCLUDE_DIRS}")
        message(STATUS "  EXECUTORCH_LIBRARIES: ${EXECUTORCH_LIBRARIES}")
    else()
        message(STATUS "ExecuTorch not found via find_package yet - using expected install paths")
        set(
            EXECUTORCH_INCLUDE_DIRS
            ${EXECUTORCH_INSTALL_DIR}/include
            ${EXECUTORCH_INSTALL_DIR}/include/executorch/runtime/core/portable_type/c10
        )
    endif()

    list(
        APPEND
        EXECUTORCH_INCLUDE_DIRS
        ${SECEDA_EXECUTORCH_DIR}/third-party/json/include
        ${EXECUTORCH_BUILD_DIR}/third-party/gflags/include
    )

    set(
        EXECUTORCH_LIBRARY_DIRS
        ${EXECUTORCH_INSTALL_DIR}/lib
        ${EXECUTORCH_BUILD_DIR}/lib
        ${EXECUTORCH_BUILD_DIR}
        ${EXECUTORCH_BUILD_DIR}/third-party/gflags
    )

    message(STATUS "ExecuTorch include: ${EXECUTORCH_INCLUDE_DIRS}")
    message(STATUS "ExecuTorch lib: ${EXECUTORCH_LIBRARY_DIRS}")
endif()

find_package(CURL REQUIRED)

add_library(seceda_edge_common INTERFACE)
target_include_directories(
    seceda_edge_common
    INTERFACE
        ${SECEDA_EDGE_CPP_INCLUDE_DIRS}
        ${SECEDA_LLAMA_VENDOR_DIR}
)
target_link_libraries(seceda_edge_common INTERFACE pthread)

add_library(seceda_edge_https INTERFACE)
target_link_libraries(
    seceda_edge_https
    INTERFACE
        seceda_edge_common
        CURL::libcurl
)

add_library(
    seceda_edge_httplib
    STATIC
        ${SECEDA_LLAMA_VENDOR_DIR}/cpp-httplib/httplib.cpp
)
target_link_libraries(seceda_edge_httplib PUBLIC seceda_edge_common)

function(seceda_finalize_target TARGET_NAME)
    set_target_properties(
        ${TARGET_NAME}
        PROPERTIES
            INSTALL_RPATH "$ORIGIN/../lib"
    )

    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT APPLE)
        target_link_options(${TARGET_NAME} PRIVATE "LINKER:-s")
    endif()
endfunction()

function(seceda_add_edge_executable TARGET_NAME)
    cmake_parse_arguments(TARGET "" "" "SOURCES;EXTRA_LIBS;EXTRA_INCLUDES" ${ARGN})

    add_executable(${TARGET_NAME} ${TARGET_SOURCES})

    target_include_directories(
        ${TARGET_NAME}
        PRIVATE
            ${TARGET_EXTRA_INCLUDES}
    )

    target_link_libraries(
        ${TARGET_NAME}
        PRIVATE
            seceda_edge_common
            ${TARGET_EXTRA_LIBS}
    )

    seceda_finalize_target(${TARGET_NAME})
endfunction()

function(seceda_add_edge_library TARGET_NAME)
    cmake_parse_arguments(TARGET "" "" "SOURCES;EXTRA_LIBS;EXTRA_INCLUDES" ${ARGN})

    add_library(${TARGET_NAME} STATIC ${TARGET_SOURCES})

    target_include_directories(
        ${TARGET_NAME}
        PRIVATE
            ${TARGET_EXTRA_INCLUDES}
    )

    target_link_libraries(
        ${TARGET_NAME}
        PUBLIC
            seceda_edge_common
            ${TARGET_EXTRA_LIBS}
    )
endfunction()

function(seceda_link_llama TARGET_NAME)
    if(NOT SECEDA_BUILD_LLAMA_CPP)
        message(FATAL_ERROR "Cannot link ${TARGET_NAME}: SECEDA_BUILD_LLAMA_CPP is OFF")
    endif()

    target_include_directories(
        ${TARGET_NAME}
        PUBLIC
            ${LLAMA_CPP_INCLUDE_DIRS}
    )

    target_link_directories(${TARGET_NAME} PUBLIC ${LLAMA_CPP_LIBRARY_DIRS})
    target_link_libraries(${TARGET_NAME} PUBLIC llama ggml)
    add_dependencies(${TARGET_NAME} llama_cpp)
endfunction()

function(add_llama_runner TARGET_NAME)
    cmake_parse_arguments(RUNNER "" "" "SOURCES;EXTRA_LIBS;EXTRA_INCLUDES" ${ARGN})

    if(NOT SECEDA_BUILD_LLAMA_CPP)
        message(WARNING "Cannot build ${TARGET_NAME}: SECEDA_BUILD_LLAMA_CPP is OFF")
        return()
    endif()

    add_executable(${TARGET_NAME} ${RUNNER_SOURCES})

    target_include_directories(
        ${TARGET_NAME}
        PRIVATE
            ${RUNNER_EXTRA_INCLUDES}
    )

    target_link_libraries(
        ${TARGET_NAME}
        PRIVATE
            seceda_edge_common
            ${RUNNER_EXTRA_LIBS}
    )

    seceda_link_llama(${TARGET_NAME})
    seceda_finalize_target(${TARGET_NAME})
endfunction()

function(add_executorch_runner TARGET_NAME)
    cmake_parse_arguments(RUNNER "" "" "SOURCES;EXTRA_LIBS;EXTRA_INCLUDES" ${ARGN})

    if(NOT SECEDA_BUILD_EXECUTORCH)
        message(WARNING "Cannot build ${TARGET_NAME}: SECEDA_BUILD_EXECUTORCH is OFF")
        return()
    endif()

    add_executable(${TARGET_NAME} ${RUNNER_SOURCES})

    target_include_directories(
        ${TARGET_NAME}
        PRIVATE
            ${EXECUTORCH_INCLUDE_DIRS}
            ${RUNNER_EXTRA_INCLUDES}
    )

    target_link_directories(${TARGET_NAME} PRIVATE ${EXECUTORCH_LIBRARY_DIRS})

    target_link_libraries(
        ${TARGET_NAME}
        PRIVATE
            seceda_edge_common
            executorch
            executorch_core
            extension_module_static
            extension_data_loader
            extension_tensor
            ${RUNNER_EXTRA_LIBS}
    )

    if(SECEDA_ENABLE_XNNPACK)
        target_link_libraries(${TARGET_NAME} PRIVATE xnnpack_backend)
    endif()

    if(SECEDA_ENABLE_VULKAN)
        target_link_libraries(${TARGET_NAME} PRIVATE vulkan_backend)
    endif()

    add_dependencies(${TARGET_NAME} executorch_lib)
    seceda_finalize_target(${TARGET_NAME})
endfunction()

foreach(subdir IN ITEMS src apps tests)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${subdir}/CMakeLists.txt")
        add_subdirectory(${subdir})
    endif()
endforeach()

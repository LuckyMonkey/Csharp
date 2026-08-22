cmake_minimum_required(VERSION 3.16)
project(Csharp VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(CSHARP_ENABLE_DIRECT_NVDEC "Build experimental direct NVIDIA NVDECODE backend" OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LIBHEIF REQUIRED IMPORTED_TARGET libheif)
pkg_check_modules(LIBAVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(LIBAVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(LIBSWSCALE REQUIRED IMPORTED_TARGET libswscale)

add_executable(heicprobe
    src/heicprobe.c
    src/heif_boxes.c
    src/nvdec_plugin.c
)
target_link_libraries(heicprobe PRIVATE
    PkgConfig::LIBHEIF
    PkgConfig::LIBAVCODEC
    PkgConfig::LIBAVUTIL
    PkgConfig::LIBSWSCALE
)

add_executable(csharp-bench-parallel
    src/bench_parallel.c
    src/nvdec_plugin.c
)
target_link_libraries(csharp-bench-parallel PRIVATE
    PkgConfig::LIBHEIF
    PkgConfig::LIBAVCODEC
    PkgConfig::LIBAVUTIL
    PkgConfig::LIBSWSCALE
    Threads::Threads
)

target_compile_options(heicprobe PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
target_compile_options(csharp-bench-parallel PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

set(CSHARP_INSTALL_TARGETS heicprobe csharp-bench-parallel)

if(CSHARP_ENABLE_DIRECT_NVDEC)
    find_path(NVCUVID_INCLUDE_DIR nvcuvid.h
        PATH_SUFFIXES ffnvcodec
        DOC "Path containing nvcuvid.h")
    find_path(CUDA_DRIVER_INCLUDE_DIR cuda.h
        PATHS /usr/local/cuda/include /usr/include
        DOC "Path containing CUDA driver headers")
    find_library(NVCUVID_LIBRARY NAMES nvcuvid)
    find_library(CUDA_DRIVER_LIBRARY NAMES cuda
        PATHS /usr/lib/x86_64-linux-gnu /usr/local/cuda/lib64)

    if(NOT NVCUVID_INCLUDE_DIR OR NOT CUDA_DRIVER_INCLUDE_DIR OR
       NOT NVCUVID_LIBRARY OR NOT CUDA_DRIVER_LIBRARY)
        message(FATAL_ERROR
            "CSHARP_ENABLE_DIRECT_NVDEC=ON requires nvcuvid.h, cuda.h, libnvcuvid and libcuda")
    endif()

    add_library(csharp-direct-nvdecode STATIC
        src/direct_nvdec.c
    )
    target_include_directories(csharp-direct-nvdecode SYSTEM PRIVATE
        ${NVCUVID_INCLUDE_DIR}
        ${CUDA_DRIVER_INCLUDE_DIR}
    )
    target_link_libraries(csharp-direct-nvdecode PRIVATE
        ${NVCUVID_LIBRARY}
        ${CUDA_DRIVER_LIBRARY}
    )
    target_compile_options(csharp-direct-nvdecode PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    add_executable(csharp-nvdecode-probe
        src/direct_nvdec_probe.c
    )
    target_link_libraries(csharp-nvdecode-probe PRIVATE csharp-direct-nvdecode)
    target_compile_options(csharp-nvdecode-probe PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    list(APPEND CSHARP_INSTALL_TARGETS csharp-nvdecode-probe)
endif()

install(TARGETS ${CSHARP_INSTALL_TARGETS} RUNTIME DESTINATION bin)

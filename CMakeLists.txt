cmake_minimum_required(VERSION 3.16)
project(Csharp VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

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

install(TARGETS heicprobe csharp-bench-parallel RUNTIME DESTINATION bin)

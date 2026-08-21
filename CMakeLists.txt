cmake_minimum_required(VERSION 3.16)
project(Csharp VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBHEIF REQUIRED IMPORTED_TARGET libheif)

add_executable(heicprobe
    src/heicprobe.c
    src/heif_boxes.c
)
target_link_libraries(heicprobe PRIVATE PkgConfig::LIBHEIF)

target_compile_options(heicprobe PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

install(TARGETS heicprobe RUNTIME DESTINATION bin)

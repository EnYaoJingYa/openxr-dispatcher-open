# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.22)
project(OpenXRDispatcher LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

# Android (build.sh builds arm64-v8a and armeabi-v7a). The host build for tests is tests/run.sh.
file(GLOB XD_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
add_library(openxr_loader SHARED ${XD_SOURCES} src/third_party/cJSON.c)
set_target_properties(openxr_loader PROPERTIES OUTPUT_NAME openxr_loader C_VISIBILITY_PRESET hidden)
target_include_directories(openxr_loader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(openxr_loader PRIVATE -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-format-truncation)
set_source_files_properties(src/third_party/cJSON.c PROPERTIES COMPILE_OPTIONS "-w")
target_link_options(openxr_loader PRIVATE
  -Wl,-soname,libopenxr_loader.so -Wl,-Bsymbolic -Wl,--no-undefined -Wl,-z,max-page-size=16384 -Wl,-s)
target_link_libraries(openxr_loader PRIVATE log dl m)

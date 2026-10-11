cmake_minimum_required(VERSION 3.16)
project(deadspace_port CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# dynarmic: 32-bit ARM JIT (only the A32 frontend is needed)
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/dynarmic EXCLUDE_FROM_ALL)

find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(ZLIB REQUIRED)

add_executable(deadspace
    src/main.cpp
    src/log.cpp
    src/apk.cpp
    src/settings.cpp
    src/crash.cpp
    src/sbin.cpp
    src/game_patches.cpp
    src/input.cpp
    src/devmode.cpp
    src/objects.cpp
    src/interact.cpp
    src/lights.cpp
    src/debugview.cpp
    src/console.cpp
    src/menu_nav.cpp
    src/render/native.cpp
    src/guest_util.cpp
    src/memory.cpp
    src/cpu.cpp
    src/elf_loader.cpp
    src/hle/hle.cpp
    src/hle/timing.cpp
    src/hle/libc.cpp
    src/hle/stdio.cpp
    src/hle/printf.cpp
    src/hle/pthread.cpp
    src/hle/android.cpp
    src/hle/gles.cpp
    src/jni/jni.cpp
    src/jni/java_classes.cpp
)
target_include_directories(deadspace PRIVATE src)
target_link_libraries(deadspace PRIVATE dynarmic OpenGL::GL)
if (WIN32)
    # fully static: the exe runs without any MSYS2 DLLs next to it
    target_link_libraries(deadspace PRIVATE SDL2::SDL2-static z ws2_32 winmm dbghelp)
    target_link_options(deadspace PRIVATE -static)
else()
    target_link_libraries(deadspace PRIVATE SDL2::SDL2 ZLIB::ZLIB pthread dl)
endif()

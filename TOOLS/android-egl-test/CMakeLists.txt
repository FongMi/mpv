cmake_minimum_required(VERSION 3.22)
project(mpv_egl_test C)

foreach(header "${MPV_ROOT}/video/out/opengl/context.h"
               "${FFMPEG_INCLUDE_DIR}/libavutil/avutil.h"
               "${PLACEBO_INCLUDE_DIR}/libplacebo/renderer.h"
               "${CONTEXT_SOURCE}")
    if(NOT EXISTS "${header}")
        message(FATAL_ERROR "Missing test input: ${header}")
    endif()
endforeach()
file(TO_CMAKE_PATH "${CONTEXT_SOURCE}" CONTEXT_SOURCE)
add_library(mpvEglTest SHARED native/probe.c)
target_include_directories(mpvEglTest PRIVATE native "${MPV_ROOT}"
    "${MPV_ROOT}/video/out/opengl" "${FFMPEG_INCLUDE_DIR}" "${PLACEBO_INCLUDE_DIR}")
target_compile_definitions(mpvEglTest PRIVATE MPV_EGL_CONTEXT_SOURCE="${CONTEXT_SOURCE}")
target_compile_options(mpvEglTest PRIVATE -UNDEBUG -fvisibility=hidden
    -ffunction-sections -fdata-sections -Wall -Wextra -Werror
    -Wno-unused-parameter -Wno-sign-compare)
target_link_options(mpvEglTest PRIVATE -Wl,--gc-sections -Wl,--no-undefined)
target_link_libraries(mpvEglTest EGL GLESv2 android nativewindow log dl m)

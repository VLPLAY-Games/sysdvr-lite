cmake_minimum_required(VERSION 3.16)
project(sysdvr_lite VERSION 1.2.0 LANGUAGES C)

# The client targets the Orange Pi Zero 2W (Allwinner H618): arm64 Linux only.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
  message(FATAL_ERROR "sysdvr-lite supports only arm64 Linux (found ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR})")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# libusb is required, the rest enables optional features when found
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
pkg_check_modules(FFMPEG IMPORTED_TARGET libavcodec libavutil)
pkg_check_modules(ALSA   IMPORTED_TARGET alsa)
pkg_check_modules(DRM    IMPORTED_TARGET libdrm)

add_executable(sysdvr-lite
  src/main.c src/proto.c src/transport_usb.c src/transport_file.c src/queue.c
  src/decoder_null.c src/decoder_v4l2.c src/h264_parse.c
  src/sink_file.c src/sink_null.c)
target_link_libraries(sysdvr-lite Threads::Threads PkgConfig::LIBUSB)

# Version: the release number, plus `git describe` when built from a checkout (re-read on every configure)
set(SYSDVR_GIT "")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE SYSDVR_GIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
target_compile_definitions(sysdvr-lite PRIVATE SYSDVR_VERSION="${PROJECT_VERSION}" SYSDVR_GIT="${SYSDVR_GIT}")
if(FFMPEG_FOUND)
  target_sources(sysdvr-lite PRIVATE src/decoder_ffmpeg.c)
  target_link_libraries(sysdvr-lite PkgConfig::FFMPEG)
  target_compile_definitions(sysdvr-lite PRIVATE HAVE_FFMPEG)
endif()
if(ALSA_FOUND)
  target_sources(sysdvr-lite PRIVATE src/audio_alsa.c)
  target_link_libraries(sysdvr-lite PkgConfig::ALSA)
  target_compile_definitions(sysdvr-lite PRIVATE HAVE_ALSA)
endif()
if(DRM_FOUND)
  target_sources(sysdvr-lite PRIVATE src/sink_drm.c src/font.c)
  target_link_libraries(sysdvr-lite PkgConfig::DRM)
  target_compile_definitions(sysdvr-lite PRIVATE HAVE_DRM)
  # GPU colour conversion + zero-copy scanout needs EGL/GLES2/GBM (mesa)
  pkg_check_modules(GL IMPORTED_TARGET egl glesv2 gbm)
  if(GL_FOUND)
    target_link_libraries(sysdvr-lite PkgConfig::GL)
    target_compile_definitions(sysdvr-lite PRIVATE HAVE_GL)
  endif()
  add_executable(drm-probe tools/drm_probe.c)
  target_link_libraries(drm-probe PkgConfig::DRM)
endif()

# Test tools: a test stream generator and an offline reference decoder
if(FFMPEG_FOUND)
  add_executable(gen-h264 tools/gen_h264.c)
  target_link_libraries(gen-h264 PkgConfig::FFMPEG)
  add_executable(ref-decode tools/ref_decode.c)
  target_link_libraries(ref-decode PkgConfig::FFMPEG)
endif()

install(TARGETS sysdvr-lite RUNTIME DESTINATION bin)

message(STATUS "ffmpeg=${FFMPEG_FOUND} alsa=${ALSA_FOUND} drm=${DRM_FOUND} gl=${GL_FOUND}")

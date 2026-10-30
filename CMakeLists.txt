cmake_minimum_required(VERSION 3.16)
project(sysdvr_lite C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# Everything except Threads is optional, features are enabled when the library is found
pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
pkg_check_modules(FFMPEG IMPORTED_TARGET libavcodec libavutil)
pkg_check_modules(ALSA   IMPORTED_TARGET alsa)
pkg_check_modules(DRM    IMPORTED_TARGET libdrm)

add_executable(sysdvr-lite
  src/main.c src/proto.c src/transport_tcp.c src/queue.c
  src/decoder_null.c src/decoder_v4l2.c src/h264_parse.c
  src/sink_file.c src/sink_null.c)
target_link_libraries(sysdvr-lite Threads::Threads)

if(LIBUSB_FOUND)
  target_sources(sysdvr-lite PRIVATE src/transport_usb.c)
  target_link_libraries(sysdvr-lite PkgConfig::LIBUSB)
  target_compile_definitions(sysdvr-lite PRIVATE HAVE_LIBUSB)
endif()
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
  target_sources(sysdvr-lite PRIVATE src/sink_drm.c)
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

# Test tools: a fake console, a test stream generator and an offline reference decoder
add_executable(mock-switch tools/mock_switch.c)
target_link_libraries(mock-switch Threads::Threads)
if(FFMPEG_FOUND)
  add_executable(gen-h264 tools/gen_h264.c)
  target_link_libraries(gen-h264 PkgConfig::FFMPEG)
  add_executable(ref-decode tools/ref_decode.c)
  target_link_libraries(ref-decode PkgConfig::FFMPEG)
endif()

install(TARGETS sysdvr-lite RUNTIME DESTINATION bin)

message(STATUS "libusb=${LIBUSB_FOUND} ffmpeg=${FFMPEG_FOUND} alsa=${ALSA_FOUND} drm=${DRM_FOUND} gl=${GL_FOUND}")

cmake_minimum_required(VERSION 3.16)
project(rcfpv-vps-edge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- libdatachannel(静态链接;部署时与源码同级放置 libdatachannel/) ----
set(BUILD_SHARED_LIBS OFF)
set(RTC_ENABLE_MEDIA ON CACHE BOOL "" FORCE)
set(RTC_ENABLE_WEBSOCKET ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../libdatachannel
                 ${CMAKE_CURRENT_BINARY_DIR}/libdatachannel-build)

# ---- FFmpeg(仅 RTSP 拉流/解析,不转码) ----
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat
  libavcodec
  libavutil)

find_package(Threads REQUIRED)

add_executable(rcfpv-edge
  src/main.cpp
  src/core/config.cpp
  src/media/rtsp_capture.cpp
  src/media/udp_audio_bridge.cpp
  src/mavlink/bridge.cpp
  src/rtc/signaling_client.cpp
  src/rtc/streamer.cpp
)

target_include_directories(rcfpv-edge PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  # nlohmann/json:复用 libdatachannel 自带依赖,零新增
  ${CMAKE_CURRENT_SOURCE_DIR}/../libdatachannel/deps/json/include
  # MAVLink 官方 C 库(header-only,vendor)
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mavlink/vendor
)

target_link_libraries(rcfpv-edge PRIVATE
  LibDataChannel::LibDataChannel
  PkgConfig::FFMPEG
  Threads::Threads
)

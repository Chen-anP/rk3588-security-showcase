cmake_minimum_required(VERSION 3.22)

# 版本号的唯一来源：发布时打的 git 标签 vX.Y.Z 要和这里一致（CI 会检查）
project(rk3588_security VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(RKSEC_BUILD_TESTS "编译单元测试" ON)
option(RKSEC_WERROR "警告当作错误（CI 打开）" OFF)
option(RKSEC_ASAN "AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(RKSEC_TSAN "ThreadSanitizer" OFF)

if(RKSEC_ASAN AND RKSEC_TSAN)
  message(FATAL_ERROR "ASan 和 TSan 不能同时打开")
endif()

include(cmake/CompilerOptions.cmake)
include(cmake/Version.cmake)
include(cmake/Lvgl.cmake)

# 系统库（Ubuntu 22.04：libdrm-dev 2.4.113、libturbojpeg0-dev 2.1.2、libsqlite3-dev 3.37.2），用 pkg-config 找
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto>=2.0)
pkg_check_modules(GPIOD REQUIRED IMPORTED_TARGET libgpiod>=1.6)
if(NOT GPIOD_VERSION VERSION_LESS 2.0)
  message(FATAL_ERROR "GPIO backend requires libgpiod 1.x")
endif()
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(TURBOJPEG REQUIRED IMPORTED_TARGET libturbojpeg)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)  # 人脸库（第 7d 步）
# 事件录像写 MP4（第 8b 步）：只用 FFmpeg 的封装库，不用它的编解码（编码是 MPP 硬件做的）。LGPL，动态链接
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)
# RTSP 实时流（第 8c 步）：GStreamer 的 rtsp-server（LGPL，动态链接）。运行时还要插件：base（app）、good（rtp）、bad（h264parse）
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtsp-1.0 gstreamer-rtsp-server-1.0)

# MPP（瑞芯微硬件编解码，第 5 步）：板子上有（librockchip-mpp-dev），CI（x86）上没有。
# AUTO：找到就编译硬件解码，找不到就只有 CPU 解码；ON：必须有（板子上用，防止悄悄退回 CPU）
set(RKSEC_WITH_MPP AUTO CACHE STRING "MPP 硬件解码：AUTO / ON / OFF")
set_property(CACHE RKSEC_WITH_MPP PROPERTY STRINGS AUTO ON OFF)
if(NOT RKSEC_WITH_MPP STREQUAL "OFF")
  pkg_check_modules(MPP IMPORTED_TARGET rockchip_mpp)
endif()
if(MPP_FOUND)
  message(STATUS "MPP ${MPP_VERSION}：编译硬件解码")
elseif(RKSEC_WITH_MPP STREQUAL "ON")
  message(FATAL_ERROR "RKSEC_WITH_MPP=ON，但找不到 rockchip_mpp（sudo apt install librockchip-mpp-dev）")
else()
  message(STATUS "没有 MPP：不编译硬件解码（CI、电脑上正常）")
endif()

# RGA（瑞芯微 2D 图像硬件，第 5c 步）：同 MPP，板子上有（librga-dev），CI 上没有
set(RKSEC_WITH_RGA AUTO CACHE STRING "RGA：AUTO / ON / OFF")
set_property(CACHE RKSEC_WITH_RGA PROPERTY STRINGS AUTO ON OFF)
if(NOT RKSEC_WITH_RGA STREQUAL "OFF")
  pkg_check_modules(RGA IMPORTED_TARGET librga)
endif()
if(RGA_FOUND)
  message(STATUS "librga ${RGA_VERSION}：编译 RGA 缩放和颜色转换")
elseif(RKSEC_WITH_RGA STREQUAL "ON")
  message(FATAL_ERROR "RKSEC_WITH_RGA=ON，但找不到 librga（sudo apt install librga-dev）")
else()
  message(STATUS "没有 RGA：不编译 RGA 缩放和颜色转换（CI、电脑上正常）")
endif()

# RKNN（NPU 推理运行库，第 5d 步）：板子上有 librknnrt.so 和 rknn_api.h，没有 pkg-config 文件，按文件名找。
# 头文件不在系统目录时（比如只有 rknn_model_zoo 里那份），用 -DRKSEC_RKNN_ROOT=目录（下面有 include/ 和 lib/）指定
set(RKSEC_WITH_RKNN AUTO CACHE STRING "RKNN NPU 推理：AUTO / ON / OFF")
set_property(CACHE RKSEC_WITH_RKNN PROPERTY STRINGS AUTO ON OFF)
set(RKSEC_RKNN_ROOT "" CACHE PATH "rknn_api.h 和 librknnrt.so 所在目录（include/、lib/ 的上一级），空 = 系统目录")
if(NOT RKSEC_WITH_RKNN STREQUAL "OFF")
  find_path(RKNN_INCLUDE_DIR rknn_api.h HINTS "${RKSEC_RKNN_ROOT}/include")
  find_library(RKNN_LIBRARY rknnrt HINTS "${RKSEC_RKNN_ROOT}/lib")
endif()
if(RKNN_INCLUDE_DIR AND RKNN_LIBRARY)
  set(RKNN_FOUND TRUE)
  add_library(rknn_rt UNKNOWN IMPORTED)
  set_target_properties(rknn_rt PROPERTIES IMPORTED_LOCATION "${RKNN_LIBRARY}"
                                           INTERFACE_INCLUDE_DIRECTORIES "${RKNN_INCLUDE_DIR}")
  message(STATUS "RKNN：${RKNN_LIBRARY}（头文件 ${RKNN_INCLUDE_DIR}）：编译 NPU 检测")
elseif(RKSEC_WITH_RKNN STREQUAL "ON")
  message(FATAL_ERROR "RKSEC_WITH_RKNN=ON，但找不到 rknn_api.h 或 librknnrt.so（-DRKSEC_RKNN_ROOT=目录）")
else()
  message(STATUS "没有 RKNN：不编译 NPU 检测（CI、电脑上正常）")
endif()

add_subdirectory(src)

if(RKSEC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# core：所有模块共用的基础库（版本、配置、日志、队列、帧句柄、计时）
add_library(rksec_core STATIC
  core/config.cpp
  core/frame_pool.cpp
  core/ini.cpp
  core/instance_lock.cpp
  core/log.cpp
  core/stage_timer.cpp
  core/version.cpp
  ${RKSEC_VERSION_CPP})
add_dependencies(rksec_core rksec_version_info)
target_include_directories(rksec_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rksec_core PRIVATE rksec_options)

# capture：V4L2 摄像头采集（第 3 步）。只依赖 Linux 内核头文件，CI 上也能编译和测试（用假设备）
add_library(rksec_capture STATIC
  capture/capture_service.cpp
  capture/sys.cpp
  capture/v4l2_camera.cpp)
target_link_libraries(rksec_capture PUBLIC rksec_core PRIVATE rksec_options)

# display：DRM/KMS 显示输出（第 4 步）。选卡、选模式的规则（drm_select）在 CI 上测试
add_library(rksec_display STATIC
  display/drm_display.cpp
  display/drm_select.cpp)
target_link_libraries(rksec_display PUBLIC rksec_core PkgConfig::LIBDRM PRIVATE rksec_options)

# media：图像编解码。CPU 解码 MJPG（libjpeg-turbo，第 4 步）；MPP 硬件解码（第 5 步，只在板子上）
add_library(rksec_media STATIC
  media/dma_buffer.cpp
  media/draw.cpp
  media/h264_stream.cpp
  media/jpeg_decoder.cpp
  media/jpeg_encoder.cpp
  media/letterbox.cpp
  media/yuv_frame.cpp)
target_link_libraries(rksec_media PUBLIC rksec_core PRIVATE PkgConfig::TURBOJPEG rksec_options)
if(MPP_FOUND)
  # 用到 MPP 的代码都用 RKSEC_HAVE_MPP 包起来，没有 MPP 的机器上照样能编译
  target_sources(rksec_media PRIVATE media/mpp_jpeg_decoder.cpp media/mpp_h264_encoder.cpp)
  target_compile_definitions(rksec_media PUBLIC RKSEC_HAVE_MPP)
  target_link_libraries(rksec_media PUBLIC PkgConfig::MPP)
endif()
if(RGA_FOUND)
  target_sources(rksec_media PRIVATE media/rga_converter.cpp)
  target_compile_definitions(rksec_media PUBLIC RKSEC_HAVE_RGA)
  target_link_libraries(rksec_media PUBLIC PkgConfig::RGA)
endif()

# detect：检测的后处理（纯 CPU，CI 上测试）；RKNN 推理（第 5d 步，只在板子上）
add_library(rksec_detect STATIC
  detect/yolo_post.cpp)
target_link_libraries(rksec_detect PUBLIC rksec_media rksec_core PRIVATE rksec_options)
if(RKNN_FOUND)
  target_sources(rksec_detect PRIVATE detect/rknn_model.cpp)
  target_compile_definitions(rksec_detect PUBLIC RKSEC_HAVE_RKNN)
  target_link_libraries(rksec_detect PUBLIC rknn_rt)
endif()

# track：多目标跟踪（第 7a 步），纯 CPU 计算，CI 上测试
add_library(rksec_track STATIC
  track/kalman.cpp
  track/tracker.cpp)
target_link_libraries(rksec_track PUBLIC rksec_detect PRIVATE rksec_options)

# face：人脸检测、识别的前后处理（第 7 步）：检测解码、对齐、特征比对、给跟踪编号配名字，纯 CPU 计算，CI 上测试
add_library(rksec_face STATIC
  face/align.cpp
  face/enroll.cpp
  face/face_db.cpp
  face/face_pick.cpp
  face/gallery.cpp
  face/identity.cpp
  face/retinaface.cpp
  face/score_stats.cpp)
target_link_libraries(rksec_face PUBLIC rksec_track rksec_detect PRIVATE PkgConfig::SQLITE rksec_options)
if(RKNN_FOUND)
  # 两个人脸模型的推理（第 7c 步），只在板子上
  target_sources(rksec_face PRIVATE face/face_engine.cpp)
endif()

# record：事件录像（第 8b 步）：预录缓冲、事件状态机、配额、事件表（SQLite）、MP4 封装（libavformat），
# 导出（转普通 MP4）和删除（第 8d 步），CI 上测试
add_library(rksec_record STATIC
  record/event_admin.cpp
  record/event_logic.cpp
  record/event_recorder.cpp
  record/event_store.cpp
  record/mp4_remux.cpp
  record/mp4_writer.cpp
  record/pre_record.cpp
  record/retention.cpp)
target_link_libraries(rksec_record PUBLIC rksec_face rksec_media rksec_core PRIVATE PkgConfig::SQLITE PkgConfig::AVFORMAT
                                   PkgConfig::TURBOJPEG rksec_options)

# stream：RTSP 实时流（第 8c 步）：账号文件、关键帧门（纯逻辑），GStreamer rtsp-server
add_library(rksec_stream STATIC
  stream/credentials.cpp
  stream/rtsp_server.cpp)
target_link_libraries(rksec_stream PUBLIC rksec_media rksec_core PRIVATE PkgConfig::GST rksec_options)

# ui：界面（LVGL）。页面导航（router）不依赖 LVGL；界面能在没有屏幕的环境里创建和测试
# 中文字体是 tools/fonts/gen_fonts.sh 生成的 C 文件，单独编译，不套用本项目的 C++ 警告选项
add_library(rksec_fonts STATIC
  ui/fonts/rksec_font_cn_20.c
  ui/fonts/rksec_font_cn_28.c
  ui/fonts/rksec_font_names_20.c)
target_link_libraries(rksec_fonts PRIVATE lvgl)

add_library(rksec_ui STATIC
  ui/glyphs.cpp
  ui/lvgl_port.cpp
  ui/router.cpp
  ui/screens.cpp)
target_link_libraries(rksec_ui PUBLIC rksec_fonts rksec_media rksec_core lvgl PRIVATE rksec_options)

# app：主程序和模拟器共用的部分（进程指标、界面模拟 ui_sim、预览 + 检测流水线）
add_library(rksec_app STATIC
  access/access_machine.cpp
  access/coordinator.cpp
  app/pass_history.cpp
  app/access_service.cpp
  stream/mqtt_publisher.cpp
  access/pass_store.cpp
  app/detect_pipeline.cpp
  app/encode_stage.cpp
  app/proc_stats.cpp
  app/recording.cpp
  app/ui_sim.cpp)
target_link_libraries(rksec_app PUBLIC rksec_ui rksec_record rksec_stream rksec_track rksec_face rksec_detect rksec_core PRIVATE rksec_options)

add_library(rksec_hw STATIC hw/pir.cpp)
target_link_libraries(rksec_hw PUBLIC rksec_core PRIVATE PkgConfig::GPIOD rksec_options)

target_link_libraries(rksec_app PRIVATE PkgConfig::SQLITE PkgConfig::MOSQUITTO rksec_hw)

# 主程序
add_executable(rk3588-security
  app/capture_run.cpp
  app/capture_test.cpp
  app/decode_bench.cpp
  app/detect_bench.cpp
  app/encode_run.cpp
  app/event_admin.cpp
  app/face_admin.cpp
  app/face_bench.cpp
  app/face_tool.cpp
  app/hw_pipeline.cpp
  app/hw_test.cpp
  app/main.cpp
  app/access_run.cpp
  app/pass_admin.cpp
  app/rga_bench.cpp
  app/record_run.cpp
  app/ui_run.cpp)
target_link_libraries(rk3588-security PRIVATE rksec_hw rksec_app rksec_ui rksec_detect rksec_display rksec_media rksec_capture rksec_core
                                              PkgConfig::TURBOJPEG rksec_options)

# 电脑上的界面模拟器（第 4c 步）：找到 SDL2 才编译（WSL / Linux 桌面：sudo apt install libsdl2-dev）。
# 板子上不需要，没有 SDL2 时跳过，不影响其他目标
pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
if(SDL2_FOUND)
  add_executable(rk3588-security-sim
    sim/main.cpp
    sim/sdl_backend.cpp)
  target_link_libraries(rk3588-security-sim PRIVATE rksec_app PkgConfig::SDL2 rksec_options)
  message(STATUS "SDL2 ${SDL2_VERSION}：编译界面模拟器 rk3588-security-sim")
else()
  message(STATUS "没有 SDL2：不编译界面模拟器（需要时 sudo apt install libsdl2-dev）")
endif()

install(TARGETS rk3588-security RUNTIME DESTINATION bin)
install(FILES ${PROJECT_SOURCE_DIR}/config/rk3588-security.conf DESTINATION /etc/rk3588-security)

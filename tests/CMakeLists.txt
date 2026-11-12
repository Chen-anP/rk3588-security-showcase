# GoogleTest 用系统包（Ubuntu 22.04: libgtest-dev 1.11），版本随系统锁定
find_package(GTest 1.11 REQUIRED)
include(GoogleTest)

add_executable(rksec_tests
  sanitizer_suppressions.cpp
  test_bounded_queue.cpp
  test_capture_service.cpp
  test_config.cpp
  test_pir.cpp
  test_access_machine.cpp
  test_access_coordinator.cpp
  test_pass_store.cpp
  test_detect_pipeline.cpp
  test_dma_buffer.cpp
  test_encode_stage.cpp
  test_face_align.cpp
  test_face_db.cpp
  test_drm_select.cpp
  test_frame_pool.cpp
  test_gallery.cpp
  test_h264_stream.cpp
  test_ini.cpp
  test_instance_lock.cpp
  test_jpeg_decoder.cpp
  test_latest_image.cpp
  test_letterbox.cpp
  test_log.cpp
  test_mpp_h264_encoder.cpp
  test_mpp_jpeg_decoder.cpp
  test_record.cpp
  test_reorder_buffer.cpp
  test_retinaface.cpp
  test_face_pick.cpp
  test_rga_converter.cpp
  test_rknn_model.cpp
  test_router.cpp
  test_stage_timer.cpp
  test_stream.cpp
  test_tracker.cpp
  test_ui.cpp
  test_ui_sim.cpp
  test_v4l2_camera.cpp
  test_version.cpp
  test_yolo_post.cpp
  test_yuv_frame.cpp)
target_link_libraries(rksec_tests PRIVATE rksec_hw rksec_app rksec_stream rksec_record rksec_ui rksec_track rksec_face rksec_detect rksec_display rksec_media rksec_capture rksec_core rksec_options
                                          PkgConfig::TURBOJPEG PkgConfig::SQLITE PkgConfig::AVFORMAT PkgConfig::GST GTest::gtest_main)
# 测试里要读仓库自带的配置文件
target_compile_definitions(rksec_tests PRIVATE RKSEC_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_include_directories(rksec_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})  # tests/data/

# 每个 TEST 注册成一个 ctest 用例，失败时能看到是哪一个。
# 多线程代码写错常见的表现是死锁，测试卡住不动；超过 60 秒判失败，不让 CI 一直挂着。
gtest_discover_tests(rksec_tests TEST_FILTER "-RtspServer.*" PROPERTIES TIMEOUT 60)
# RTSP 回环测试（第 8c 步）真的起服务、连客户端，对 CPU 敏感：和 NPU 测试等一起并行跑、再加上 ASan 时，
# 偶发客户端只收到几帧就卡住（板子上 5 遍里 1 次；原因在 GStreamer 内部，没查清，见未完成清单）。单独串行跑
gtest_discover_tests(rksec_tests TEST_FILTER "RtspServer.*" PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

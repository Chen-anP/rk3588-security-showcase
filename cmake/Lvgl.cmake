# LVGL（界面库），版本锁定。
#
# 源码放在 third_party/lvgl（不进 git，.gitignore 里忽略），所有构建目录（debug/release/asan/…）共用一份。
# 第一次配置时自动下载：浅克隆 + 稀疏检出，只取编译要用的 src/ 等目录（约 20 MB；完整仓库带文档和演示近 300 MB）。
# 下载后核对标签对应的提交号：标签可以被仓库维护者移动，提交号是内容的哈希，改不了。对不上就停止，不用来路不明的代码。
#
# 离线编译（比如没有外网的产线机器）：把有网机器上的 third_party/lvgl 整个拷过来即可；
# 或者用 -DRKSEC_LVGL_DIR=/path/to/lvgl 指向别处。两种情况都会核对提交号。
set(RKSEC_LVGL_TAG v9.2.2)
set(RKSEC_LVGL_COMMIT 7f07a129e8d77f4984fff8e623fd5be18ff42e74)
set(RKSEC_LVGL_URL https://github.com/lvgl/lvgl.git)
set(RKSEC_LVGL_DIR "${PROJECT_SOURCE_DIR}/third_party/lvgl" CACHE PATH "LVGL 源码目录")

find_package(Git REQUIRED)

# 几个构建目录同时配置时，只让一个去下载
file(LOCK "${PROJECT_SOURCE_DIR}/third_party/.lvgl.lock" GUARD FILE TIMEOUT 600)

if(NOT EXISTS "${RKSEC_LVGL_DIR}/CMakeLists.txt")
  # 网络偶尔出错（板子上遇到过 “curl 16 Error in the HTTP2 framing layer”），最多试 3 次。
  # 用 HTTP/1.1：这个错误出在 HTTP/2 的分帧层，换协议能避开
  foreach(_try 1 2 3)
    message(STATUS "下载 LVGL ${RKSEC_LVGL_TAG} 到 ${RKSEC_LVGL_DIR}（第 ${_try} 次）")
    file(REMOVE_RECURSE "${RKSEC_LVGL_DIR}")  # 上次下到一半失败留下的残骸
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -c advice.detachedHead=false -c http.version=HTTP/1.1 clone --quiet --depth 1
              --branch ${RKSEC_LVGL_TAG} --filter=blob:none --sparse ${RKSEC_LVGL_URL} "${RKSEC_LVGL_DIR}"
      RESULT_VARIABLE _rc)
    if(_rc EQUAL 0)
      # 只检出编译要用的：源码、CMake 脚本、顶层头文件和模板、许可证
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" -c http.version=HTTP/1.1 -C "${RKSEC_LVGL_DIR}" sparse-checkout set --no-cone
                /src/ /env_support/cmake/ /*.h /*.in /CMakeLists.txt /LICENCE.txt
        RESULT_VARIABLE _rc)
    endif()
    if(_rc EQUAL 0)
      break()
    endif()
  endforeach()
  if(NOT _rc EQUAL 0)
    file(REMOVE_RECURSE "${RKSEC_LVGL_DIR}")
    message(FATAL_ERROR "下载 LVGL 失败（试了 3 次，需要能访问 github.com）。离线编译见 cmake/Lvgl.cmake 开头的说明")
  endif()
endif()

execute_process(
  COMMAND "${GIT_EXECUTABLE}" -C "${RKSEC_LVGL_DIR}" rev-parse HEAD
  OUTPUT_VARIABLE _lvgl_commit
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0 OR NOT _lvgl_commit STREQUAL RKSEC_LVGL_COMMIT)
  message(FATAL_ERROR "LVGL 提交号不对：期望 ${RKSEC_LVGL_COMMIT}（${RKSEC_LVGL_TAG}），实际 '${_lvgl_commit}'（${RKSEC_LVGL_DIR}）。"
                      "删掉这个目录重新配置会重新下载")
endif()
file(LOCK "${PROJECT_SOURCE_DIR}/third_party/.lvgl.lock" RELEASE)
message(STATUS "LVGL ${RKSEC_LVGL_TAG}（${_lvgl_commit}）")

# LVGL 自己的 CMake 选项：用我们的配置文件，不编译示例、演示和矢量图形库
set(LV_CONF_PATH "${PROJECT_SOURCE_DIR}/src/ui/lv_conf.h" CACHE STRING "LVGL 配置文件" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)

add_subdirectory("${RKSEC_LVGL_DIR}" "${CMAKE_BINARY_DIR}/_deps/lvgl-build")

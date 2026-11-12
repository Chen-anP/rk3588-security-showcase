# 用法：cmake -DSRC_DIR=... -DOUT_FILE=... -DTEMPLATE=... -DPROJECT_VERSION=... -P WriteVersion.cmake
find_package(Git QUIET)
set(GIT_DESCRIBE "unknown")
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always
    WORKING_DIRECTORY ${SRC_DIR}
    OUTPUT_VARIABLE _out
    RESULT_VARIABLE _rc
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_rc EQUAL 0 AND _out)
    set(GIT_DESCRIBE "${_out}")
  endif()
endif()
configure_file(${TEMPLATE} ${OUT_FILE} @ONLY)

# 每次编译都重新生成版本信息（git describe 会随提交变化），
# 内容不变时不改文件，避免无谓的重新编译。
# 不写编译时间：同一份源码应编出同一个程序（可复现构建，NFR-M2）。
set(RKSEC_VERSION_CPP ${CMAKE_BINARY_DIR}/generated/version_info.cpp)

add_custom_target(rksec_version_info
  COMMAND ${CMAKE_COMMAND}
    -DSRC_DIR=${CMAKE_SOURCE_DIR}
    -DOUT_FILE=${RKSEC_VERSION_CPP}
    -DTEMPLATE=${CMAKE_SOURCE_DIR}/cmake/version_info.cpp.in
    -DPROJECT_VERSION=${PROJECT_VERSION}
    -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
    "-DCOMPILER=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    -P ${CMAKE_SOURCE_DIR}/cmake/WriteVersion.cmake
  BYPRODUCTS ${RKSEC_VERSION_CPP}
  COMMENT "生成版本信息"
  VERBATIM)

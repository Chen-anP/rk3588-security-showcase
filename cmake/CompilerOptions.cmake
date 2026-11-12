# 所有自己的目标都链接 rksec_options，统一警告和 Sanitizer 选项
add_library(rksec_options INTERFACE)

target_compile_options(rksec_options INTERFACE
  -Wall -Wextra -Wpedantic
  -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
  -Wnull-dereference -Wdouble-promotion -Wformat=2)

if(RKSEC_WERROR)
  target_compile_options(rksec_options INTERFACE -Werror)
endif()

if(RKSEC_ASAN)
  target_compile_options(rksec_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(rksec_options INTERFACE -fsanitize=address,undefined)
endif()

if(RKSEC_TSAN)
  target_compile_options(rksec_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(rksec_options INTERFACE -fsanitize=thread)
endif()

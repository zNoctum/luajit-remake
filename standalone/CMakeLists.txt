add_library(ljr_standalone OBJECT
  main.cpp
)

add_dependencies(ljr_standalone 
  deegen_fps_lib
)
set_target_properties(ljr_standalone PROPERTIES COMPILE_FLAGS " -DDEEGEN_POST_FUTAMURA_PROJECTION -target aarch64-linux-gnu ")

target_compile_options(ljr_standalone PRIVATE -target aarch64-linux-gnu)
target_link_options(ljr_standalone PRIVATE -target aarch64-linux-gnu)
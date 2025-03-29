add_library(runtime 
  runtime_utils.cpp
  vm.cpp
  init_global_object.cpp
  math_fast_pow.cpp
  lj_strscan.cpp
  lj_strfmt_num.cpp
  lj_strfmt.cpp
  lj_lex.cpp
  lj_parse.cpp
)

add_dependencies(runtime 
  deegen_fps_lib
)
set_target_properties(runtime PROPERTIES COMPILE_FLAGS " -DDEEGEN_POST_FUTAMURA_PROJECTION -target aarch64-unknown-linux-gnu")

# The legacy JSON bytecode dump parser, only used for unit tests as a debugging tool
#
add_library(legacy_json_bc_dump_parser
  legacy_json_bc_dump_parser.cpp
)
target_compile_options(legacy_json_bc_dump_parser PRIVATE -target aarch64-unknown-linux-gnu)

add_dependencies(legacy_json_bc_dump_parser 
  deegen_fps_lib
)
set_target_properties(legacy_json_bc_dump_parser PROPERTIES COMPILE_FLAGS " -DDEEGEN_POST_FUTAMURA_PROJECTION ")


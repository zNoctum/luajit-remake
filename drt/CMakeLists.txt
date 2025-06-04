SET(DEEGEN_RT_SRC
	bytecode_builder.cpp
	baseline_jit_codegen_helper.cpp
	jit_memory_allocator.cpp
	mmap_utils.cpp
	dfg_arena.cpp
	dfg_node.cpp
	dfg_control_flow_and_upvalue_analysis.cpp
	temp_arena_allocator.cpp
	dfg_basic_block_builder.cpp
	dfg_frontend.cpp
	dfg_ir_validator.cpp
	dfg_ir_dump.cpp
	dfg_speculative_inliner.cpp
	dfg_bytecode_liveness.cpp
	dfg_construct_block_local_ssa.cpp
	dfg_trivial_cfg_cleanup.cpp
	dfg_phantom_insertion.cpp
	memory_ptr.cpp
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
	list(APPEND DEEGEN_RT_SRC deegen_internal_enter_exit_vm_x64.s)
else()
	list(APPEND DEEGEN_RT_SRC deegen_internal_enter_exit_vm_arm64.s)
endif()

add_library(deegen_rt ${DEEGEN_RT_SRC})

add_dependencies(deegen_rt 
  deegen_fps_lib
)
set_target_properties(deegen_rt PROPERTIES COMPILE_FLAGS " -DDEEGEN_POST_FUTAMURA_PROJECTION ")


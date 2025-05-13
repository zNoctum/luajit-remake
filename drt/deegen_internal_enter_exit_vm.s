	.text
	.file	"deegen_internal_enter_exit_vm.c"
	
# deegen_enter_vm_from_c_impl
#   Enter the VM from C code.
#
	.globl	deegen_enter_vm_from_c_impl  
	.p2align	4, 0x90
	.type	deegen_enter_vm_from_c_impl,@function
deegen_enter_vm_from_c_impl:  
	# Input (Linux C calling convention):
	#   arg 0 (x0): CoroutineCtx
	#   arg 1 (x1): the GHC-calling-conv callee
	#   arg 2 (x2): stackBase
	#   arg 3 (x3): numArgs
	#   arg 4 (x4): cb
	#   arg 5 (x5): vmBasePointer
	#
	# The GHC calling convention callee expects:
	#   dst 0 (x19): CoroutineCtx
	#   dst 1 (x20): stackBase
	#   dst 2 (x21): numArgs
	#   dst 3 (x22): cb
	#   dst 4 (x23): tag register 1
	#   dst 5 (x24): (unused)disas
	#   dst 6 (x25): isMustTail64 (should be 0)
	#   dst 7 (x26): (unused)
	#   dst 8 (x27): vmBasePointer
	#   dst 9 (x28): tag register 2
	#
	# Push all the callee-saved registers in AAPCS64.
	# Note that we happen to push 96 bytes, and we are branching to the callee.
	# So the callee will see a 16-byte-aligned sp as expected. 
	#
	sub sp, sp,   #96
	stp x19, x20, [sp, #0]
	stp x21, x22, [sp, #16]
	stp x23, x24, [sp, #32]
	stp x25, x26, [sp, #48]
	stp x27, x28, [sp, #64]
	stp x29, x30, [sp, #80]
	
	# Set up the registers expected by the GHC calling convention callee
	#
	
	# Move CoroutineCtx (x19)
	#
	mov     x19, x0
	
	# Move stackBase (x20)
	#
	mov 	x20, x2
	
	# Move numArgs (x21)
	#
	mov     x21, x3
	
	# Move vmBasePointer (x22)
	#
	mov     x22, x5
	
	# set up tag register 1 (x23, x_int32Tag)
	#
	mov     x23, #281470681743360
	movk    x23, #65531, lsl #48

	# Move cb (x24)
	#
	mov     x24, x4

	# Set isMustTail64 (x25) to 0
	#
	mov     x25, xzr
	
	# Unused (x26)
	
	# Unused (x27)
	
	# set up tag register 2 (x28, x_mivTag)
	#
	mov     x28, #-1125899906842497
	movk    x28, #65535, lsl #32

	# Branch to callee (x1)
	# The stack is unbalanced yet, but it's fine because by design control must 
	# eventually transfer to deegen_internal_use_only_exit_vm_epilogue, 
	# which will restore the callee-saved registers, re-balance the stack,
	# and return control to C
	#
	br x1
	udf #0
	
.Lfunc_end_deegen_enter_vm_from_c_impl:
	.size	deegen_enter_vm_from_c_impl, .Lfunc_end_deegen_enter_vm_from_c_impl-deegen_enter_vm_from_c_impl
          
# deegen_internal_use_only_exit_vm_epilogue
#   Clean up the stack and return control to C.
#   Never called directly from C. 
#
	.globl	deegen_internal_use_only_exit_vm_epilogue  
	.p2align	4, 0x90
	.type	deegen_internal_use_only_exit_vm_epilogue,@function
deegen_internal_use_only_exit_vm_epilogue:  
	# Input (GHC calling convention):
	#   arg 0 (x19): CoroutineCtx (unused)
	#   arg 1 (x20): stackBase (unused)
	#   arg 2 (x21): (unused)
	#   arg 3 (x22): (unused)
	#   arg 4 (x23): tag register 1 (unused)
	#   arg 5 (x24): retStart
	#   arg 6 (x25): numRets
	#   arg 7 (x26) : (unused)
	#   arg 8 (x27) : (unused)
	#   arg 9 (x28): tag register 2 (unused)
	#
	# Returns (Linux C calling convention, 'DeegenInternalEnterVMFromCReturnResults')
	#   x0: retStart
	#   x1: numRets
	#

	# Set up the return values and return to C code
	#
	mov     x0, x24
	mov     x1, x25
	
	# Clean up the stack and restore the callee-saved registers of C calling convention
	#
	ldp x29, x30, [sp, #80]
	ldp x27, x28, [sp, #64]
	ldp x25, x26, [sp, #48]
	ldp x23, x24, [sp, #32]
	ldp x21, x22, [sp, #16]
	ldp x19, x20, [sp, #0]
	add sp, sp,   #96

	ret
	
.Lfunc_end_deegen_internal_use_only_exit_vm_epilogue:
	.size	deegen_internal_use_only_exit_vm_epilogue, .Lfunc_end_deegen_internal_use_only_exit_vm_epilogue-deegen_internal_use_only_exit_vm_epilogue

	.section	".note.GNU-stack","",@progbits
	 

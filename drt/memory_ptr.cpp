#define DEEGEN_DEF_BYTECODE
#include "memory_ptr.h"

__thread VM* activeVMForCurrentThread;

extern "C" void* __attribute__((__const__, __warn_unused_result__)) DeegenImpl_GetVMBasePointer() {
    return activeVMForCurrentThread;
}

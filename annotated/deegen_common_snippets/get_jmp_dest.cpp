#include "force_release_build.h"

#include "define_deegen_common_snippet.h"

#define MASK(x) ((1ULL<<x)-1)

static void* DeegenSnippet_GetJmpDest(uint32_t* jmpEndAddr)
{
    uint32_t *instrAddr = jmpEndAddr - 1;
    uint32_t instr = *instrAddr;
    uint8_t ident = instr>>24;
    int64_t diff = 0;

    // BL and B instructions
    // 
    if ((ident&0x7c) == 0x14) {
        diff = ((instr&MASK(26))<<38)>>38;
    // B.cc and BC.cc instructions
    //
    } else if (ident == 0x54) {
        diff = ((instr&MASK(19))<<45)>>45;
    } else {
        assert(false && "unexpected instruction for destination retrieval!");
    }
    return reinterpret_cast<void*>(instrAddr + diff);
}

DEFINE_DEEGEN_COMMON_SNIPPET("GetJmpDest", DeegenSnippet_GetJmpDest)

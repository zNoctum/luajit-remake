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
        diff = (static_cast<int64_t>(instr)<<38)>>36;
    // B.cc and BC.cc instructions
    //
    } else if (ident == 0x54) {
        diff = (static_cast<int64_t>(instr>>5)<<45)>>43;
    } else if ((instr & ~(MASK(16)<<5)) == 0xd2800010) {
        return reinterpret_cast<void*>(((instrAddr[0]>>5)&MASK(16)) + (((instrAddr[1]>>5)&MASK(16))<<16) + (((instrAddr[2]>>5)&MASK(16))<<32) + (((instrAddr[3]>>5)&MASK(16))<<48));
    } else {
        assert(false && "unexpected instruction for destination retrieval!");
    }
    return reinterpret_cast<void*>(reinterpret_cast<uint64_t>(instrAddr) + static_cast<uint64_t>(diff));
}

DEFINE_DEEGEN_COMMON_SNIPPET("GetJmpDest", DeegenSnippet_GetJmpDest)

#include "force_release_build.h"

#include "define_deegen_common_snippet.h"

#define MASK(x) ((1LL<<x)-1)

static void DeegenSnippet_SetJmpDest(uint32_t* jmpEndAddr, void* newDest)
{
    uint32_t *instrAddr = jmpEndAddr - 1;
    uint32_t instr = *instrAddr;
    uint8_t ident = instr>>24;
    int64_t diff = reinterpret_cast<int64_t>(newDest) - reinterpret_cast<int64_t>(instrAddr);

    diff >>= 2;

    // BL and B instructions
    // 
    if ((ident&0x7c) == 0x14) {
        // Assert that the offset is in the +-128MiB range of B and BL
        //
        assert((((diff&MASK(26))<<38)>>38) == diff);
        instr = static_cast<uint32_t>((instr&~MASK(26))|(static_cast<uint64_t>(diff)&MASK(26)));
    } else if (ident == 0x54) {
        assert((((diff&MASK(19))<<45)>>45) == diff);
        instr = static_cast<uint32_t>((instr&(~MASK(24)|MASK(5)))|((static_cast<uint64_t>(diff)&MASK(19))<<5));
    } else {
        assert(false && "Unexpected Instruction!");
    }
    
    // std::printf("%02X %02X %02X %02X\n", 0xFF&instr, (instr>>8)&0xFF, (instr>>16)&0xFF, (instr>>24)&0xFF);
    *instrAddr = instr;
    __builtin___clear_cache(reinterpret_cast<char*>(instrAddr), reinterpret_cast<char*>(instrAddr+1));
    return;
}

DEFINE_DEEGEN_COMMON_SNIPPET("SetJmpDest", DeegenSnippet_SetJmpDest)

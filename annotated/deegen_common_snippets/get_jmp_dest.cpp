#include "force_release_build.h"

#include "define_deegen_common_snippet.h"
#include "drt/platform.h"

#define MASK(x) ((1ULL<<x)-1)

static void* DeegenSnippet_GetJmpDest(uint32_t* instrAddr)
{
    if (x_targetX64)
    {
        int32_t tmp = *reinterpret_cast<int32_t*>(instrAddr);
        return reinterpret_cast<void*>(reinterpret_cast<int64_t>(instrAddr) + 4 + static_cast<int64_t>(tmp));
    }
    else
    {
        uint32_t instr = instrAddr[0];
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
        } else if ((ident&0x9f) == 0x90) {
            uint64_t addr = static_cast<uint64_t>((static_cast<int64_t>(((instr>>29)&MASK(2))+(((instr>>5)&MASK(19))<<2))<<43)>>43);
            addr += reinterpret_cast<uint64_t>(instrAddr)>>12;
            return reinterpret_cast<void*>((addr << 12) + ((instrAddr[1]>>10)&MASK(12)));
        } else {
            assert(false && "unexpected instruction for destination retrieval!");
        }
        return reinterpret_cast<void*>(reinterpret_cast<uint64_t>(instrAddr) + static_cast<uint64_t>(diff));
    }
}

DEFINE_DEEGEN_COMMON_SNIPPET("GetJmpDest", DeegenSnippet_GetJmpDest)

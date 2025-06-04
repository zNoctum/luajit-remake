#include "force_release_build.h"

#include "define_deegen_common_snippet.h"
#include "drt/platform.h"

#define MASK(x) ((1U<<x)-1)

static void DeegenSnippet_SetJmpDest(uint32_t* instrAddr, void* newDest)
{
    if (x_targetX64)
    {
        void *jmpEnd = reinterpret_cast<void*>(instrAddr + 1);
        int64_t tmp = reinterpret_cast<int64_t>(newDest) - reinterpret_cast<int64_t>(jmpEnd);
        assert(tmp == static_cast<int64_t>(static_cast<int32_t>(tmp)));
        *reinterpret_cast<int32_t*>(instrAddr) = static_cast<int32_t>(tmp);
    }
    else
    {
        uint32_t instr = instrAddr[0];
        uint8_t ident = instr>>24;
        uint64_t diff = reinterpret_cast<uint64_t>(newDest) - reinterpret_cast<uint64_t>(instrAddr);

        diff = static_cast<uint64_t>(static_cast<int64_t>(diff)>>2);

        // BL and B instructions
        //
        if ((ident&0x7c) == 0x14) {
            // Assert that the offset is in the +-128MiB range of B and BL
            //
            assert((((diff&MASK(26))<<38)>>38) == diff);
            instrAddr[0] = static_cast<uint32_t>((instr&~MASK(26))|(diff&MASK(26)));
            clear_cache(reinterpret_cast<char*>(instrAddr), reinterpret_cast<char*>(instrAddr) + 4);
        // b.cc and bc.cc instructions
        //
        } else if (ident == 0x54) {
            assert((((diff&MASK(19))<<45)>>45) == diff);
            instrAddr[0] = static_cast<uint32_t>((instr&(~MASK(24)|MASK(5)))|((diff&MASK(19))<<5));
            clear_cache(reinterpret_cast<char*>(instrAddr), reinterpret_cast<char*>(instrAddr) + 4);
        } else if ((ident&0x9f) == 0x90) {
            assert(static_cast<uint64_t>(static_cast<int64_t>((diff&MASK(31))<<33)>>33) == diff);
            uint32_t tmp = static_cast<uint32_t>((reinterpret_cast<uint64_t>(newDest) >> 12) - (reinterpret_cast<uint64_t>(instrAddr) >> 12));
            instrAddr[0] = ((tmp&MASK(2))<< 29) + (((tmp>>2)&MASK(19))<<5) + 0x90000010;                                // adrp x16, #0
            instrAddr[1] = static_cast<uint32_t>(((reinterpret_cast<uint64_t>(newDest))&MASK(12))<<10) + 0x91000210;    // add  x16, x16, #0
            instrAddr[2] = 0xd61f0200;                                                                                  // blr  x16
            clear_cache(reinterpret_cast<char*>(instrAddr), reinterpret_cast<char*>(instrAddr) + 12);
        } else {
            assert(false && "Unexpected Instruction!");
        }
    }
    return;
}

DEFINE_DEEGEN_COMMON_SNIPPET("SetJmpDest", DeegenSnippet_SetJmpDest)

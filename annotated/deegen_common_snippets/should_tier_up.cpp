#include "force_release_build.h"

#include "define_deegen_common_snippet.h"
#include "runtime_utils.h"

static bool DeegenSnippet_ShouldTierUp(CodeBlock* cb)
{
    int64_t counter = cb->m_interpreterTierUpCounter;
    return __builtin_expect(counter < 0, false);
}

DEFINE_DEEGEN_COMMON_SNIPPET("ShouldTierUp", DeegenSnippet_ShouldTierUp)

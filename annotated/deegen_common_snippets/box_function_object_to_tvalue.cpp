#include "force_release_build.h"

#include "define_deegen_common_snippet.h"
#include "runtime_utils.h"

static TValue DeegenSnippet_BoxFunctionObjectToTValue(FunctionObject* func)
{
    TValue tv = TValue::Create<tFunction>(func);
    assert(tv.Is<tFunction>());
    return tv;
}

DEFINE_DEEGEN_COMMON_SNIPPET("BoxFunctionObjectToTValue", DeegenSnippet_BoxFunctionObjectToTValue)

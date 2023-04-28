#include "gtest/gtest.h"

#include "misc_llvm_helper.h"

#include "read_file.h"
#include "runtime_utils.h"
#include "json_utils.h"
#include "deegen_bytecode_ir_components.h"
#include "deegen_baseline_jit_impl_creator.h"
#include "deegen_stencil_runtime_constant_insertion_pass.h"
#include "deegen_stencil_lowering_pass.h"
#include "deegen_stencil_creator.h"
#include "deegen_baseline_jit_codegen_logic_creator.h"
#include "deegen_ast_inline_cache.h"
#include "lj_parser_wrapper.h"
#include "drt/baseline_jit_codegen_helper.h"
#include "deegen_function_entry_logic_creator.h"
#include "llvm_override_option.h"
#include "test_vm_utils.h"
#include "deegen_ast_make_call.h"
#include "llvm/IR/DIBuilder.h"
#include "deegen_parse_asm_text.h"
#include "deegen_call_inline_cache.h"
#include "deegen_stencil_inline_cache_extraction_pass.h"

using namespace dast;
using namespace llvm;
#if 0
TEST(XX, XX)
{
    x_deegen_unit_test = true;

    using namespace llvm::object;
    std::unique_ptr<LLVMContext> llvmCtxHolder(new LLVMContext);
    LLVMContext& ctx = *llvmCtxHolder.get();

    nlohmann::json j = nlohmann::json::parse(ReadFileContentAsString("test_inputs/test_call.json"))["all-bytecode-info"][0];

    BytecodeIrInfo b(ctx, j);
    // b.m_slowPaths[0]->m_module->dump();

    b.m_bytecodeDef->ComputeBaselineJitSlowPathDataLayout();


    BaselineJitImplCreator mainJic(*b.m_jitMainComponent.get());
    mainJic.DoLowering();

    fprintf(stderr, "%s\n", mainJic.GetStencilPostTransformAsmFile().c_str());
}
#endif

#if 0
TEST(BaselineJit, Fib_Upvalue)
{
    VM* vm = VM::Create();
    Auto(vm->Destroy());
    vm->SetEngineStartingTier(VM::EngineStartingTier::BaselineJIT);

    ParseResult res = ParseLuaScript(vm->GetRootCoroutine(), ReadFileContentAsString("test_fib_upvalue.lua"));

    ScriptModule* sm = res.m_scriptModule.get();

    //std::ignore = sm;
    //ReleaseAssert(false);
    vm->LaunchScript(sm);
    ReleaseAssert(false);
}

TEST(XX, YY)
{
    std::unique_ptr<LLVMContext> llvmCtxHolder(new LLVMContext);
    LLVMContext& ctx = *llvmCtxHolder.get();
    DeegenFunctionEntryLogicCreator creator(ctx, DeegenEngineTier::BaselineJIT, false, static_cast<size_t>(-1));
    auto res = creator.GetBaselineJitResult();
    res.m_module->dump();

    fprintf(stderr, "%s\n", res.m_asmSourceForAudit.c_str());

}

TEST(BaselineJITGen, LoadBytecodeInfo)
{
    std::unique_ptr<LLVMContext> llvmCtxHolder(new LLVMContext);
    LLVMContext& ctx = *llvmCtxHolder.get();

    nlohmann::json j = nlohmann::json::parse(ReadFileContentAsString("test_expected_output/test_add_input.json"));
    BytecodeIrInfo b(ctx, j);
    ReleaseAssert(b.m_bytecodeDef->m_bytecodeName == "Add");
    ReleaseAssert(!b.m_bytecodeDef->m_hasConditionalBranchTarget);
    ReleaseAssert(b.m_bytecodeDef->m_hasOutputValue);
}

TEST(XX, XX)
{
    using namespace llvm::object;
    std::unique_ptr<LLVMContext> llvmCtxHolder(new LLVMContext);
    LLVMContext& ctx = *llvmCtxHolder.get();

    nlohmann::json j = nlohmann::json::parse(ReadFileContentAsString("test_inputs/test_call.json"))["all-bytecode-info"][0];

    BytecodeIrInfo b(ctx, j);
    // b.m_slowPaths[0]->m_module->dump();

    b.m_bytecodeDef->ComputeBaselineJitSlowPathDataLayout();

    DeegenBytecodeBaselineJitInfo bji = DeegenBytecodeBaselineJitInfo::Create(b, BytecodeOpcodeRawValueMap::ParseFromJSON("[\"Call_0\"]"_json));

    printf("%s", bji.m_disasmForAudit.c_str());

    bji.m_cgMod->dump();

    printf("%s", CompileLLVMModuleToAssemblyFile(bji.m_cgMod.get(), Reloc::Static, CodeModel::Small).c_str());
}

TEST(XX, YY)
{
    using namespace llvm::object;
    std::unique_ptr<LLVMContext> llvmCtxHolder(new LLVMContext);
    LLVMContext& ctx = *llvmCtxHolder.get();

    nlohmann::json j = nlohmann::json::parse(ReadFileContentAsString("test_inputs/test_table_get_by_id.json"))["all-bytecode-info"][0];
    BytecodeIrInfo b(ctx, j);
    // b.m_slowPaths[0]->m_module->dump();

    b.m_bytecodeDef->ComputeBaselineJitSlowPathDataLayout();

    AstInlineCache::TriviallyLowerAllInlineCaches(b.m_jitMainComponent->m_impl);

    DeegenBytecodeBaselineJitInfo bji = DeegenBytecodeBaselineJitInfo::Create(b, BytecodeOpcodeRawValueMap::ParseFromJSON("[\"TableGetById_0\"]"_json));

    printf("%s", bji.m_disasmForAudit.c_str());

    bji.m_cgMod->dump();

    printf("%s", CompileLLVMModuleToAssemblyFile(bji.m_cgMod.get(), Reloc::Static, CodeModel::Small).c_str());
}

TEST(XX, ZZ)
{
    using namespace llvm::object;
    std::unique_ptr<LLVMContext> llvmCtxHolder(new LLVMContext);
    LLVMContext& ctx = *llvmCtxHolder.get();

    nlohmann::json j = nlohmann::json::parse(ReadFileContentAsString("test_inputs/test_table_get_by_id.json"))["all-bytecode-info"][0];
    BytecodeIrInfo b(ctx, j);
    // b.m_slowPaths[0]->m_module->dump();

    b.m_bytecodeDef->ComputeBaselineJitSlowPathDataLayout();

    BaselineJitImplCreator mainJic(*b.m_slowPaths[1].get());
    mainJic.DoLowering();

    mainJic.GetModule()->dump();
}
#endif
TEST(StencilCreator, DataSectionHandling_1)
{
    using namespace llvm::object;
    std::unique_ptr<LLVMContext> llvmCtxHolder(new LLVMContext);
    LLVMContext& ctx = *llvmCtxHolder.get();

    DeegenStencil ds = DeegenStencil::ParseMainLogic(ctx, ReadFileContentAsString("test_inputs/test_stencil_parser_1.o"));

    ReleaseAssert(ds.m_privateDataObject.m_bytes.size() == 64);
    ReleaseAssert(ds.m_privateDataObject.m_relocations.size() == 8);
    for (size_t i = 0; i < 8; i++)
    {
        ReleaseAssert(ds.m_privateDataObject.m_relocations[i].m_offset == 8 * i);
    }

    ReleaseAssert(ds.m_sharedDataObjs.size() == 3);
}

#include "deegen_interpreter_bytecode_impl_creator.h"
#include "deegen_bytecode_operand.h"
#include "deegen_ast_make_call.h"
#include "deegen_ast_return.h"
#include "deegen_ast_return_value_accessor.h"
#include "deegen_interpreter_function_interface.h"
#include "deegen_ast_throw_error.h"
#include "deegen_options.h"
#include "tvalue_typecheck_optimization.h"
#include "deegen_ast_simple_lowering_utils.h"
#include "tag_register_optimization.h"
#include "llvm_fcmp_extra_optimizations.h"
#include "deegen_ast_inline_cache.h"
#include "deegen_ast_slow_path.h"

#include "llvm/Linker/Linker.h"

#include "runtime_utils.h"

namespace dast {

struct ReturnContinuationAndSlowPathFinder
{
    ReturnContinuationAndSlowPathFinder(llvm::Function* from, bool ignoreSlowPaths)
    {
        m_ignoreSlowPaths = ignoreSlowPaths;

        FindCalls(from);
        FindSlowPaths(from);

        // We disallow the entry function itself to also be a continuation,
        // since the entry function cannot access return values while the continuation function can.
        //
        ReleaseAssert(!m_returnContinuations.count(from));

        // The entry function should not be a slow path either: it just doesn't make sense
        //
        ReleaseAssert(!m_slowPaths.count(from));

        ReleaseAssertImp(m_ignoreSlowPaths, m_slowPaths.size() == 0);

        m_allResults = m_returnContinuations;
        for (llvm::Function* func : m_slowPaths)
        {
            ReleaseAssert(!m_allResults.count(func));
            m_allResults.insert(func);
        }
    }

    bool m_ignoreSlowPaths;

    std::unordered_set<llvm::Function*> m_returnContinuations;
    std::unordered_set<llvm::Function*> m_slowPaths;

    // This is simply m_slowPaths + m_returnContinuations
    //
    std::unordered_set<llvm::Function*> m_allResults;

private:
    void FindCalls(llvm::Function* from)
    {
        std::vector<AstMakeCall> list = AstMakeCall::GetAllUseInFunction(from);
        for (AstMakeCall& amc : list)
        {
            dfs(amc.m_continuation, true /*isCall*/);
        }
    }

    void FindSlowPaths(llvm::Function* from)
    {
        if (m_ignoreSlowPaths) { return; }
        std::vector<AstSlowPath> list = AstSlowPath::GetAllUseInFunction(from);
        for (AstSlowPath& sp : list)
        {
            dfs(sp.GetImplFunction(), false /*isCall*/);
        }
    }

    void dfs(llvm::Function* cur, bool isCall)
    {
        if (cur == nullptr)
        {
            return;
        }
        if (isCall)
        {
            ReleaseAssert(!m_slowPaths.count(cur));
            if (m_returnContinuations.count(cur))
            {
                return;
            }
            m_returnContinuations.insert(cur);
        }
        else
        {
            ReleaseAssert(!m_returnContinuations.count(cur));
            if (m_slowPaths.count(cur))
            {
                return;
            }
            m_slowPaths.insert(cur);
        }

        FindCalls(cur);
        FindSlowPaths(cur);
    }
};

std::string WARN_UNUSED InterpreterBytecodeImplCreator::GetInterpreterBytecodeFunctionCName(BytecodeVariantDefinition* bytecodeDef)
{
    return std::string("__deegen_interpreter_op_") + bytecodeDef->m_bytecodeName + "_" + std::to_string(bytecodeDef->m_variantOrd);
}

std::string WARN_UNUSED InterpreterBytecodeImplCreator::GetInterpreterBytecodeReturnContinuationFunctionCName(BytecodeVariantDefinition* bytecodeDef, size_t rcOrd)
{
    return GetInterpreterBytecodeFunctionCName(bytecodeDef) + "_retcont_" + std::to_string(rcOrd);
}

static std::string GetQuickeningSlowPathFunctionNameFromBytecodeMainFunctionName(const std::string& bytecodeFnName)
{
    return bytecodeFnName + "_quickening_slowpath";
}

bool WARN_UNUSED InterpreterBytecodeImplCreator::IsFunctionReturnContinuationOfBytecode(llvm::Function* func, const std::string& bytecodeVariantMainFuncName)
{
    std::string fnName = func->getName().str();
    std::string expectedPrefix = bytecodeVariantMainFuncName + "_retcont_";
    if (!fnName.starts_with(expectedPrefix))
    {
        return false;
    }
    fnName = fnName.substr(expectedPrefix.length());
    ReleaseAssert(fnName.length() > 0);
    for (size_t i = 0; i < fnName.length(); i++)
    {
        ReleaseAssert('0' <= fnName[i] && fnName[i] <= '9');
    }
    return true;
}

static std::unordered_map<uint64_t /*operandOrd*/, uint64_t /*argOrd*/> GetQuickeningSlowPathAdditionalArgs(BytecodeVariantDefinition* bytecodeDef)
{
    ReleaseAssert(bytecodeDef->HasQuickeningSlowPath());

    // We can only use the unused registers in InterpreterFunctionInterface, so unfortunately this is
    // currently tightly coupled with our value-passing convention of InterpreterFunctionInterface..
    //
    // The already-decoded args will be passed to the continuation using registers in the following order.
    //
    std::vector<uint64_t> gprList = InterpreterFunctionInterface::GetAvaiableGPRListForBytecodeSlowPath();
    std::vector<uint64_t> fprList = InterpreterFunctionInterface::GetAvaiableFPRListForBytecodeSlowPath();

    // We will use pop_back for simplicity, so reverse the vector now
    //
    std::reverse(gprList.begin(), gprList.end());
    std::reverse(fprList.begin(), fprList.end());

    std::unordered_map<uint64_t /*operandOrd*/, uint64_t /*argOrd*/> res;
    for (auto& it : bytecodeDef->m_quickening)
    {
        TypeSpeculationMask mask = it.m_speculatedMask;
        // This is the only case that we want to (and can) use FPR to hold the TValue directly
        //
        bool shouldUseFPR = (mask == x_typeSpeculationMaskFor<tDoubleNotNaN>);
        size_t operandOrd = it.m_operandOrd;

        if (shouldUseFPR)
        {
            if (fprList.empty())
            {
                continue;
            }
            size_t argOrd = fprList.back();
            fprList.pop_back();
            res[operandOrd] = argOrd;
        }
        else
        {
            if (gprList.empty())
            {
                continue;
            }
            size_t argOrd = gprList.back();
            gprList.pop_back();
            res[operandOrd] = argOrd;
        }
    }
    return res;
}

constexpr const char* x_get_bytecode_metadata_ptr_placeholder_fn_name = "__DeegenImpl_GetInterpreterBytecodeMetadataPtrPlaceholder";

static llvm::Function* CreateGetBytecodeMetadataPtrPlaceholderFnDeclaration(llvm::Module* module)
{
    using namespace llvm;
    std::string fnName = x_get_bytecode_metadata_ptr_placeholder_fn_name;
    Function* f = module->getFunction(fnName);
    if (f != nullptr)
    {
        ReleaseAssert(f->empty() && f->arg_size() == 0 && llvm_type_has_type<void*>(f->getReturnType()));
        return f;
    }
    ReleaseAssert(module->getNamedValue(fnName) == nullptr);
    FunctionType* fty = FunctionType::get(llvm_type_of<void*>(module->getContext()), { }, false /*isVarArg*/);
    f = Function::Create(fty, GlobalValue::ExternalLinkage, fnName, module);
    ReleaseAssert(f->getName() == fnName);
    return f;
}

BytecodeIrComponent::BytecodeIrComponent(BytecodeIrComponent::ProcessFusedInIcEffectTag,
                                         BytecodeVariantDefinition* bytecodeDef,
                                         llvm::Function* implTmp,
                                         size_t icEffectOrd)
    : BytecodeIrComponent(bytecodeDef, implTmp, BytecodeIrComponentKind::FusedInInlineCacheEffect)
{
    using namespace llvm;
    ReleaseAssert(m_module != nullptr && m_impl != nullptr);
    LLVMContext& ctx = m_module->getContext();

    ReleaseAssert(m_impl->getLinkage() == GlobalValue::ExternalLinkage);

    {
        std::string implFuncName = m_impl->getName().str();
        ReleaseAssert(implFuncName.ends_with("_impl"));
        m_resultFuncName = implFuncName.substr(0, implFuncName.length() - strlen("_impl")) + "_fused_ic_" + std::to_string(icEffectOrd);
    }

    // Populate the implementation of the inline cache adaption placeholder functions
    //
    {
        Function* func = m_module->getFunction(x_adapt_ic_hit_check_behavior_placeholder_fn);
        ReleaseAssert(func != nullptr);
        ReleaseAssert(func->empty());
        func->setLinkage(GlobalValue::InternalLinkage);
        func->addFnAttr(Attribute::AttrKind::AlwaysInline);
        BasicBlock* bb = BasicBlock::Create(ctx, "", func);
        ReleaseAssert(func->arg_size() == 1);
        Value* input = func->getArg(0);
        ReleaseAssert(llvm_value_has_type<bool>(input) && llvm_type_has_type<bool>(func->getReturnType()));
        ReturnInst::Create(ctx, input, bb);
        CopyFunctionAttributes(func /*dst*/, m_impl /*src*/);
        ValidateLLVMFunction(func);
    }

    {
        Function* func = m_module->getFunction(x_adapt_get_ic_effect_ord_behavior_placeholder_fn);
        ReleaseAssert(func != nullptr);
        ReleaseAssert(func->empty());
        func->setLinkage(GlobalValue::InternalLinkage);
        func->addFnAttr(Attribute::AttrKind::AlwaysInline);
        BasicBlock* bb = BasicBlock::Create(ctx, "", func);
        ReleaseAssert(func->arg_size() == 1);
        ReleaseAssert(llvm_value_has_type<uint8_t>(func->getArg(0)));
        ReleaseAssert(llvm_type_has_type<uint8_t>(func->getReturnType()));
        uint8_t icEffectOrdU8 = SafeIntegerCast<uint8_t>(icEffectOrd);
        ReturnInst::Create(ctx, CreateLLVMConstantInt<uint8_t>(ctx, icEffectOrdU8), bb);
        CopyFunctionAttributes(func /*dst*/, m_impl /*src*/);
        ValidateLLVMFunction(func);
    }

    DoOptimization();
}

BytecodeIrComponent::BytecodeIrComponent(BytecodeVariantDefinition* bytecodeDef, llvm::Function* implTmp, BytecodeIrComponentKind processKind)
    : m_processKind(processKind)
    , m_bytecodeDef(bytecodeDef)
    , m_module(nullptr)
    , m_impl(nullptr)
{
    using namespace llvm;
    m_module = llvm::CloneModule(*implTmp->getParent());
    m_impl = m_module->getFunction(implTmp->getName());
    ReleaseAssert(m_impl != nullptr);

    if (m_impl->getLinkage() != GlobalValue::InternalLinkage)
    {
        // We require the implementation function to be marked 'static', so they can be automatically dropped
        // after we finished the transformation and made them dead
        //
        fprintf(stderr, "The implementation function of the bytecode (or any of its return continuation) must be marked 'static'!\n");
        abort();
    }

    // Figure out the result function name
    //
    if (m_processKind == BytecodeIrComponentKind::SlowPath)
    {
        m_resultFuncName = AstSlowPath::GetPostProcessSlowPathFunctionNameForInterpreter(m_impl);
    }
    else
    {
        std::string implFuncName = m_impl->getName().str();
        ReleaseAssert(implFuncName.ends_with("_impl"));
        m_resultFuncName = implFuncName.substr(0, implFuncName.length() - strlen("_impl"));
    }

    // At this stage we can drop the bytecode definition global symbol, which will render all bytecode definitions dead.
    // We temporarily change 'm_impl' to external linkage to keep it alive, then run DCE to strip the dead symbols,
    // so that all bytecode defintions except 'm_impl' are stripped and later processing is faster
    //
    std::string implNameBak = m_impl->getName().str();
    BytecodeVariantDefinition::RemoveUsedAttributeOfBytecodeDefinitionGlobalSymbol(m_module.get());
    ReleaseAssert(m_impl->getLinkage() == GlobalValue::InternalLinkage);
    m_impl->setLinkage(GlobalValue::ExternalLinkage);

    RunLLVMDeadGlobalElimination(m_module.get());

    // Sanity check 'm_impl' is still there
    //
    ReleaseAssert(m_module->getFunction(implNameBak) == m_impl);

    // Run the general inlining pass to inline general (non-API) functions into 'm_impl' and canonicalize it
    // Note that we cannot change 'm_impl' back to internal linkage right now, as otherwise it will be dead and get removed
    //
    DesugarAndSimplifyLLVMModule(m_module.get(), DesugaringLevel::InlineGeneralFunctions);
    ReleaseAssert(m_module->getFunction(implNameBak) == m_impl);

    // Assert that the inline caching APIs only show up where they are allowed to
    //
    if (m_processKind == BytecodeIrComponentKind::ReturnContinuation || m_processKind == BytecodeIrComponentKind::SlowPath)
    {
        // Inline caching API is not allowed in return continuations or in manual slow paths
        //
        ReleaseAssert(AstInlineCache::GetAllUseInFunction(m_impl).size() == 0);
    }
    else if (m_processKind == BytecodeIrComponentKind::QuickeningSlowPath)
    {
        // TODO FIXME: for now we simply fail if inline caching API is used, but this is unreasonable.
        // Instead, we should lower them to a naive implementation (i.e., no inline caching actually happens)
        //
        ReleaseAssert(AstInlineCache::GetAllUseInFunction(m_impl).size() == 0);
    }

    // For FusedInInlineCacheEffect, our caller constructor needs do something further
    //
    if (m_processKind != BytecodeIrComponentKind::FusedInInlineCacheEffect)
    {
        DoOptimization();
    }
}

void BytecodeIrComponent::DoOptimization()
{
    using namespace llvm;
    ReleaseAssert(m_impl != nullptr);

    // Run TValue typecheck strength reduction
    //
    DesugarAndSimplifyLLVMModule(m_module.get(), DesugarUpToExcluding(DesugaringLevel::TypeSpecialization));
    if (m_processKind == BytecodeIrComponentKind::Main || m_processKind == BytecodeIrComponentKind::FusedInInlineCacheEffect)
    {
        if (m_bytecodeDef->HasQuickeningSlowPath())
        {
            // In this case, we are the fast path
            //
            TValueTypecheckOptimizationPass::DoOptimizationForBytecodeQuickeningFastPath(m_bytecodeDef, m_impl);
        }
        else
        {
            TValueTypecheckOptimizationPass::DoOptimizationForBytecode(m_bytecodeDef, m_impl);
        }
    }
    else if (m_processKind == BytecodeIrComponentKind::QuickeningSlowPath)
    {
        TValueTypecheckOptimizationPass::DoOptimizationForBytecodeQuickeningSlowPath(m_bytecodeDef, m_impl);
    }
    else if (m_processKind == BytecodeIrComponentKind::ReturnContinuation || m_processKind == BytecodeIrComponentKind::SlowPath)
    {
        TValueTypecheckOptimizationPass::DoOptimizationForBytecode(m_bytecodeDef, m_impl);
    }
    else
    {
        ReleaseAssert(false);
    }

    DesugarAndSimplifyLLVMModule(m_module.get(), DesugaringLevel::PerFunctionSimplifyOnlyAggresive);
}

BytecodeIrInfo WARN_UNUSED BytecodeIrInfo::Create(BytecodeVariantDefinition* bytecodeDef, llvm::Function* mainImplTmp)
{
    using namespace llvm;
    std::unique_ptr<Module> module = CloneModule(*mainImplTmp->getParent());
    Function* mainImplFn = module->getFunction(mainImplTmp->getName());
    ReleaseAssert(mainImplFn != nullptr);
    LLVMContext& ctx = module->getContext();

    BytecodeIrInfo r;
    r.m_bytecodeDef = bytecodeDef;

    std::string resultMainFnName = InterpreterBytecodeImplCreator::GetInterpreterBytecodeFunctionCName(bytecodeDef);
    std::string implFuncName = resultMainFnName + "_impl";
    ReleaseAssert(module->getNamedValue(implFuncName) == nullptr);
    mainImplFn->setName(implFuncName);

    // Parse out all the needed return continuations, in preparation of processing each of them later
    //
    {
        // Find all the return continuations and slow paths.
        // For return continuations, give each of them a unique name right now, and create the declarations.
        // This is necessary for us to later link them together.
        // For slow paths, we will rename them in the end since that's easier.
        //
        // Note that return continuations and slow paths may arbitrarily call each other, so the search needs
        // to be recursive.
        //
        ReturnContinuationAndSlowPathFinder finder(mainImplFn, false /*ignoreSlowPaths*/);

        // Sort the return continuations by their original name for determinism
        //
        std::map<std::string /*fnName*/, Function*> sortedReturnContFns;
        for (Function* fn : finder.m_returnContinuations)
        {
            std::string fnName = fn->getName().str();
            ReleaseAssert(!sortedReturnContFns.count(fnName));
            sortedReturnContFns[fnName] = fn;
        }

        std::vector<Function*> rcList;
        {
            size_t rcOrd = 0;
            for (auto& it : sortedReturnContFns)
            {
                Function* rc = it.second;
                rcList.push_back(rc);
                std::string rcFinalName = InterpreterBytecodeImplCreator::GetInterpreterBytecodeReturnContinuationFunctionCName(bytecodeDef, rcOrd);
                std::string rcImplName = rcFinalName + "_impl";
                ReleaseAssert(module->getNamedValue(rcFinalName) == nullptr);
                ReleaseAssert(module->getNamedValue(rcImplName) == nullptr);
                rc->setName(rcImplName);
                rcOrd++;
            }
        }

        // After all the renaming and function declarations, create the InterpreterBytecodeImplCreator class so we can process each of the return continuation later
        // Note that creating the InterpreterBytecodeImplCreator also clones the module, so we want to do it after all the renaming but before our own processing begins
        //
        for (Function* targetRc : rcList)
        {
            ReleaseAssert(targetRc != nullptr);
            r.m_allRetConts.push_back(std::make_unique<BytecodeIrComponent>(bytecodeDef, targetRc, BytecodeIrComponentKind::ReturnContinuation));
        }

        // Process the quickening slow path if needed
        //
        if (bytecodeDef->HasQuickeningSlowPath())
        {
            // Temporarily change 'm_impl' to the function name for the slowpath
            //
            std::string oldName = mainImplFn->getName().str();
            std::string desiredName = GetQuickeningSlowPathFunctionNameFromBytecodeMainFunctionName(resultMainFnName) + "_impl";
            mainImplFn->setName(desiredName);
            ReleaseAssert(mainImplFn->getName().str() == desiredName);

            // Construct the slowpath creator. This clones the module in the process, and the impl function in the cloned module has the new name, as desired.
            //
            r.m_quickeningSlowPath = std::make_unique<BytecodeIrComponent>(bytecodeDef, mainImplFn, BytecodeIrComponentKind::QuickeningSlowPath);

            // Revert the name
            //
            mainImplFn->setName(oldName);
            ReleaseAssert(mainImplFn->getName().str() == oldName);
        }

        // Sanity check that all the slow path calls have the right types
        //
        for (Function* fn : finder.m_allResults)
        {
            std::vector<AstSlowPath> slowPaths = AstSlowPath::GetAllUseInFunction(fn);
            for (AstSlowPath& sp : slowPaths)
            {
                sp.CheckWellFormedness(mainImplFn);
            }
        }

        // Process the slow paths if any.
        //
        if (finder.m_slowPaths.size() > 0)
        {
            // Sort all the functions by function name and process them in sorted order for determinism
            //
            std::map<std::string /*fnName*/, Function*> sortedFns;
            for (Function* fn : finder.m_slowPaths)
            {
                std::string fnName = fn->getName().str();
                ReleaseAssert(!sortedFns.count(fnName));
                sortedFns[fnName] = fn;
            }

            for (auto& it : sortedFns)
            {
                Function* implFn = it.second;
                r.m_slowPaths.push_back(std::make_unique<BytecodeIrComponent>(bytecodeDef, implFn, BytecodeIrComponentKind::SlowPath));
            }
        }
    }

    // Process all the inline caches
    //
    std::vector<AstInlineCache> allIcUses = AstInlineCache::GetAllUseInFunction(mainImplFn);

    bool hasICFusedIntoInterpreterOpcode = false;
    size_t numFusedIcEffects = static_cast<size_t>(-1);
    for (size_t icOrd = 0; icOrd < allIcUses.size(); icOrd++)
    {
        AstInlineCache& ic = allIcUses[icOrd];
        if (ic.m_shouldFuseICIntoInterpreterOpcode)
        {
            if (hasICFusedIntoInterpreterOpcode)
            {
                fprintf(stderr, "[ERROR] Function '%s' contained more than one IC with FuseICIntoInterpreterOpcode attribute!\n", resultMainFnName.c_str());
                abort();
            }
            hasICFusedIntoInterpreterOpcode = true;
            numFusedIcEffects = ic.m_totalEffectKinds;
        }
    }

    if (hasICFusedIntoInterpreterOpcode && bytecodeDef->HasQuickeningSlowPath())
    {
        fprintf(stderr, "[LOCKDOWN] FuseICIntoInterpreterOpcode() currently cannot be used together with EnableHotColdSplitting().\n");
        abort();
    }

    for (size_t icOrd = 0; icOrd < allIcUses.size(); icOrd++)
    {
        AstInlineCache& ic = allIcUses[icOrd];

        // Lower the inline caching APIs
        //
        ic.DoLoweringForInterpreter();

        // Append required data into the bytecode metadata struct
        //
        bytecodeDef->AddBytecodeMetadata(std::move(ic.m_icStruct));

        // Note that AstInlineCache::DoLoweringForInterpreter by design does not lower the GetIcPtr calls
        // (and we cannot lower them at this stage because the relavent interpreter context isn't available yet).
        // Therefore, here we only change the dummy function name to a special one, so we can locate them and lower them later.
        //
        CallInst* icPtrGetter = ic.m_icPtrOrigin;
        ReleaseAssert(icPtrGetter->arg_size() == 0 && llvm_value_has_type<void*>(icPtrGetter));
        ReleaseAssert(AstInlineCache::IsIcPtrGetterFunctionCall(icPtrGetter));
        Function* replacementFn = CreateGetBytecodeMetadataPtrPlaceholderFnDeclaration(module.get());
        icPtrGetter->setCalledFunction(replacementFn);

        // Rename and record the IC body name. If LLVM chooses to not inline it, we need to extract it as well.
        // Note that we don't need to do anything to the IC effect functions, since they have been marked always_inline
        //
        std::string icBodyNewName = resultMainFnName + "_icbody_" + std::to_string(icOrd);
        ReleaseAssert(module->getNamedValue(icBodyNewName) == nullptr);
        ic.m_bodyFn->setName(icBodyNewName);
        ReleaseAssert(ic.m_bodyFn->getName() == icBodyNewName);
        ReleaseAssert(ic.m_bodyFn->getLinkage() == GlobalValue::InternalLinkage);
        r.m_icBodyNames.push_back(icBodyNewName);

        // Change the calling convention of the IC body to PreserveMost, since it's the slow path and we want it to be less intrusive
        //
        ic.m_bodyFn->setCallingConv(CallingConv::PreserveMost);
        for (User* usr : ic.m_bodyFn->users())
        {
            CallInst* callInst = dyn_cast<CallInst>(usr);
            if (callInst != nullptr)
            {
                ReleaseAssert(callInst->getCalledFunction() == ic.m_bodyFn);
                callInst->setCallingConv(CallingConv::PreserveMost);
            }
        }

        if (hasICFusedIntoInterpreterOpcode)
        {
            // The body fn should never be inlined since it's going to be called by multiple bytecodes
            //
            ic.m_bodyFn->setLinkage(GlobalValue::ExternalLinkage);
            ic.m_bodyFn->addFnAttr(Attribute::AttrKind::NoInline);
        }
        else
        {
            // We need to give it one more chance to inline, because previously when the inline pass was run,
            // the call from the bytecode implementation to the IC body was not visible: it only becomes visible after the lowering.
            //
            LLVMRepeatedInliningInhibitor::GiveOneMoreChance(ic.m_bodyFn);
        }
    }

    if (hasICFusedIntoInterpreterOpcode)
    {
        // Create the processors for each of the specialized implementation
        //
        for (size_t i = 0; i < numFusedIcEffects; i++)
        {
            std::unique_ptr<BytecodeIrComponent> processor = std::make_unique<BytecodeIrComponent>(
                BytecodeIrComponent::ProcessFusedInIcEffectTag(), bytecodeDef, mainImplFn, i /*effectKindOrd*/);
            r.m_affliatedBytecodeFnNames.push_back(processor->m_resultFuncName);
            r.m_fusedICs.push_back(std::move(processor));
        }

        // Populate the implementation of the inline cache adaption placeholder functions
        // Since we are only called for the empty IC case, we know that the IC hit check will always fail
        //
        {
            Function* func = module->getFunction(x_adapt_ic_hit_check_behavior_placeholder_fn);
            ReleaseAssert(func != nullptr);
            ReleaseAssert(func->empty());
            func->setLinkage(GlobalValue::InternalLinkage);
            func->addFnAttr(Attribute::AttrKind::AlwaysInline);
            BasicBlock* bb = BasicBlock::Create(ctx, "", func);
            ReleaseAssert(func->arg_size() == 1);
            ReleaseAssert(llvm_value_has_type<bool>(func->getArg(0)));
            ReleaseAssert(llvm_type_has_type<bool>(func->getReturnType()));
            ReturnInst::Create(ctx, CreateLLVMConstantInt<bool>(ctx, false), bb);
            CopyFunctionAttributes(func /*dst*/, mainImplFn /*src*/);
            ValidateLLVMFunction(func);
        }

        {
            Function* func = module->getFunction(x_adapt_get_ic_effect_ord_behavior_placeholder_fn);
            ReleaseAssert(func != nullptr);
            ReleaseAssert(func->empty());
            func->setLinkage(GlobalValue::InternalLinkage);
            func->addFnAttr(Attribute::AttrKind::AlwaysInline);
            BasicBlock* bb = BasicBlock::Create(ctx, "", func);
            ReleaseAssert(func->arg_size() == 1);
            Value* input = func->getArg(0);
            ReleaseAssert(llvm_value_has_type<uint8_t>(input));
            ReleaseAssert(llvm_type_has_type<uint8_t>(func->getReturnType()));
            ReturnInst::Create(ctx, input, bb);
            CopyFunctionAttributes(func /*dst*/, mainImplFn /*src*/);
            ValidateLLVMFunction(func);
        }
    }

    r.m_mainComponent = std::make_unique<BytecodeIrComponent>(bytecodeDef, mainImplFn, BytecodeIrComponentKind::Main);

    ReleaseAssert(!bytecodeDef->IsBytecodeStructLengthTentativelyFinalized());

    // Figure out whether this bytecode has metadata.
    // This is a bit tricky. We relying on the fact that whether or not the bytecode metadata exists depends
    // solely on the logic of the Main processor.
    // We probably should refactor it so that it's less tricky. But let's stay with it for now.
    //
    {
        // Figure out if we need Call IC. We need it if there are any calls used in the main function.
        // Note that we must do this check here after all optimizations have happened, as optimizations could have
        // deduced that calls are unreachable and removed them.
        //
        bool needCallIc = false;
        if (AstMakeCall::GetAllUseInFunction(r.m_mainComponent->m_impl).size() > 0)
        {
            needCallIc = true;
        }
        if (bytecodeDef->m_isInterpreterCallIcExplicitlyDisabled)
        {
            needCallIc = false;
        }
        if (needCallIc)
        {
            bytecodeDef->AddInterpreterCallIc();
        }
    }

    // At this point we should have determined everything that needs to sit in the bytecode metadata (if any)
    //
    bytecodeDef->TentativelyFinalizeBytecodeStructLength();

    return r;
}

void InterpreterBytecodeImplCreator::CreateWrapperFunction()
{
    using namespace llvm;
    LLVMContext& ctx = m_module->getContext();

    ReleaseAssert(m_wrapper == nullptr);

    // Create the wrapper function
    // We can also change the linkage of 'm_impl' back to internal now, since the wrapper function will keep it alive
    //
    ReleaseAssert(m_impl->hasExternalLinkage());
    m_impl->setLinkage(GlobalValue::InternalLinkage);
    m_wrapper = InterpreterFunctionInterface::CreateFunction(m_module.get(), m_resultFuncName);
    ReleaseAssert(m_wrapper->arg_size() == 16);

    // Set up the function attributes
    // TODO: add alias attributes to parameters
    //
    m_wrapper->addFnAttr(Attribute::AttrKind::NoReturn);
    m_wrapper->addFnAttr(Attribute::AttrKind::NoUnwind);
    if (m_processKind == BytecodeIrComponentKind::QuickeningSlowPath || m_processKind == BytecodeIrComponentKind::SlowPath)
    {
        m_wrapper->addFnAttr(Attribute::AttrKind::NoInline);
    }
    CopyFunctionAttributes(m_wrapper /*dst*/, m_impl /*src*/);

    BasicBlock* entryBlock = BasicBlock::Create(ctx, "", m_wrapper);
    BasicBlock* currentBlock = entryBlock;

    if (m_processKind != BytecodeIrComponentKind::ReturnContinuation)
    {
        // Note that we also set parameter names here.
        // These are not required, but just to make dumps more readable
        //
        Value* coroCtx = m_wrapper->getArg(0);
        coroCtx->setName(x_coroutineCtx);
        m_valuePreserver.Preserve(x_coroutineCtx, coroCtx);

        Value* stackBase = m_wrapper->getArg(1);
        stackBase->setName(x_stackBase);
        m_valuePreserver.Preserve(x_stackBase, stackBase);

        Value* curBytecode = m_wrapper->getArg(2);
        curBytecode->setName(x_curBytecode);
        m_valuePreserver.Preserve(x_curBytecode, curBytecode);

        Value* codeBlock = m_wrapper->getArg(3);
        codeBlock->setName(x_codeBlock);
        m_valuePreserver.Preserve(x_codeBlock, codeBlock);
    }
    else
    {
        m_valuePreserver.Preserve(x_coroutineCtx, m_wrapper->getArg(0));

        UnreachableInst* tmpInst = new UnreachableInst(ctx, currentBlock);
        Value* calleeStackBase = m_wrapper->getArg(1);
        Value* stackBase = CallDeegenCommonSnippet("GetCallerStackBaseFromStackBase", { calleeStackBase }, tmpInst);
        m_valuePreserver.Preserve(x_stackBase, stackBase);
        tmpInst->eraseFromParent();

        Value* retStart = m_wrapper->getArg(5);
        retStart->setName(x_retStart);
        m_valuePreserver.Preserve(x_retStart, retStart);

        Value* numRet = m_wrapper->getArg(6);
        numRet->setName(x_numRet);
        m_valuePreserver.Preserve(x_numRet, numRet);

        // Note that the 'm_callerBytecodePtr' is stored in the callee's stack frame header, so we should pass 'calleeStackBase' here
        //
        Instruction* bytecodePtr = CreateCallToDeegenCommonSnippet(GetModule(), "GetBytecodePtrAfterReturnFromCall", { calleeStackBase }, currentBlock);
        ReleaseAssert(llvm_value_has_type<void*>(bytecodePtr));

        Instruction* codeblock = CreateCallToDeegenCommonSnippet(GetModule(), "GetCodeBlockFromStackBase", { GetStackBase() }, currentBlock);
        ReleaseAssert(llvm_value_has_type<void*>(codeblock));

        m_valuePreserver.Preserve(x_codeBlock, codeblock);
        m_valuePreserver.Preserve(x_curBytecode, bytecodePtr);
    }

    std::unordered_map<uint64_t /*operandOrd*/, uint64_t /*argOrd*/> alreadyDecodedArgs;
    if (m_processKind == BytecodeIrComponentKind::QuickeningSlowPath && m_bytecodeDef->HasQuickeningSlowPath())
    {
        alreadyDecodedArgs = GetQuickeningSlowPathAdditionalArgs(m_bytecodeDef);
    }

    std::vector<Value*> opcodeValues;

    for (auto& operand : m_bytecodeDef->m_list)
    {
        if (alreadyDecodedArgs.count(operand->OperandOrdinal()))
        {
            opcodeValues.push_back(nullptr);
        }
        else if (!operand->SupportsGetOperandValueFromBytecodeStruct())
        {
            opcodeValues.push_back(nullptr);
        }
        else
        {
            opcodeValues.push_back(operand->GetOperandValueFromBytecodeStruct(this, currentBlock));
        }
    }

    if (m_bytecodeDef->m_hasOutputValue)
    {
        Value* outputSlot = m_bytecodeDef->m_outputOperand->GetOperandValueFromBytecodeStruct(this, currentBlock);
        ReleaseAssert(llvm_value_has_type<uint64_t>(outputSlot));
        m_valuePreserver.Preserve(x_outputSlot, outputSlot);
        outputSlot->setName(x_outputSlot);
    }

    if (m_bytecodeDef->m_hasConditionalBranchTarget)
    {
        Value* condBrTarget = m_bytecodeDef->m_condBrTarget->GetOperandValueFromBytecodeStruct(this, currentBlock);
        ReleaseAssert(llvm_value_has_type<int32_t>(condBrTarget));
        m_valuePreserver.Preserve(x_condBrDest, condBrTarget);
        condBrTarget->setName(x_condBrDest);
    }

    ReleaseAssert(m_bytecodeDef->IsBytecodeStructLengthFinalized());
    if (m_bytecodeDef->HasBytecodeMetadata())
    {
        if (m_bytecodeDef->IsBytecodeMetadataInlined())
        {
            ReleaseAssert(!m_bytecodeDef->m_inlinedMetadata->SupportsGetOperandValueFromBytecodeStruct());
            Value* metadataPtr = m_bytecodeDef->m_inlinedMetadata->EmitUsageValueFromBytecodeValue(this, currentBlock, nullptr /*bytecodeValue*/);
            m_valuePreserver.Preserve(x_metadataPtr, metadataPtr);
            metadataPtr->setName(x_metadataPtr);
        }
        else
        {
            Value* metadataPtrOffset32 = m_bytecodeDef->m_metadataPtrOffset->GetOperandValueFromBytecodeStruct(this, currentBlock);
            ReleaseAssert(llvm_value_has_type<uint32_t>(metadataPtrOffset32));
            Value* offset64 = new ZExtInst(metadataPtrOffset32, llvm_type_of<uint64_t>(ctx), "", currentBlock);
            GetElementPtrInst* metadataPtr = GetElementPtrInst::CreateInBounds(llvm_type_of<uint8_t>(ctx), GetCodeBlock(), { offset64 }, "", currentBlock);
            m_valuePreserver.Preserve(x_metadataPtr, metadataPtr);
            metadataPtr->setName(x_metadataPtr);
        }
    }

    std::vector<Value*> usageValues;
    {
        size_t ord = 0;
        for (auto& operand : m_bytecodeDef->m_list)
        {
            ReleaseAssert(operand->OperandOrdinal() == ord);
            if (alreadyDecodedArgs.count(operand->OperandOrdinal()))
            {
                size_t argOrd = alreadyDecodedArgs[operand->OperandOrdinal()];
                ReleaseAssert(argOrd < m_wrapper->arg_size());
                Value* arg = m_wrapper->getArg(static_cast<uint32_t>(argOrd));
                if (llvm_value_has_type<double>(arg))
                {
                    Instruction* dblToI64 = new BitCastInst(arg, llvm_type_of<uint64_t>(ctx), "", currentBlock);
                    usageValues.push_back(dblToI64);
                }
                else if (llvm_value_has_type<void*>(arg))
                {
                    Instruction* ptrToI64 = new PtrToIntInst(arg, llvm_type_of<uint64_t>(ctx), "", currentBlock);
                    usageValues.push_back(ptrToI64);
                }
                else
                {
                    ReleaseAssert(llvm_value_has_type<uint64_t>(arg));
                    usageValues.push_back(arg);
                }
            }
            else
            {
                usageValues.push_back(operand->EmitUsageValueFromBytecodeValue(this, currentBlock, opcodeValues[ord]));
            }
            // Set name to make dump a bit more readable
            //
            usageValues.back()->setName(std::string("bc_operand_") + operand->OperandName());
            ord++;
        }
        ReleaseAssert(ord == opcodeValues.size() && ord == usageValues.size());
    }

    if (m_processKind == BytecodeIrComponentKind::SlowPath)
    {
        std::vector<Value*> extraArgs = AstSlowPath::CreateCallArgsInSlowPathWrapperFunction(static_cast<uint32_t>(usageValues.size()), m_impl, currentBlock);
        for (Value* val : extraArgs)
        {
            usageValues.push_back(val);
        }
    }

    {
        FunctionType* fty = m_impl->getFunctionType();
        ReleaseAssert(llvm_type_has_type<void>(fty->getReturnType()));
        ReleaseAssert(fty->getNumParams() == usageValues.size());
        for (size_t i = 0; i < usageValues.size(); i++)
        {
            ReleaseAssert(fty->getParamType(static_cast<uint32_t>(i)) == usageValues[i]->getType());
        }
    }

    if (m_processKind == BytecodeIrComponentKind::Main && m_bytecodeDef->HasQuickeningSlowPath())
    {
        // If we are the main function and we are a quickening bytecode, we need to check that the quickening condition holds.
        // We can only run 'm_impl' if the condition holds. If not, we must transfer control to the quickening slow path.
        //

        // First let's create the basic block that fallbacks to the slow path
        //
        BasicBlock* slowpathBB;
        {
            std::string slowpathName = GetQuickeningSlowPathFunctionNameFromBytecodeMainFunctionName(m_resultFuncName);
            Function* slowpathFn = InterpreterFunctionInterface::CreateFunction(m_module.get(), slowpathName);
            slowpathFn->addFnAttr(Attribute::AttrKind::NoReturn);
            slowpathFn->addFnAttr(Attribute::AttrKind::NoInline);
            ReleaseAssert(slowpathFn->getName() == slowpathName);
            slowpathBB = BasicBlock::Create(ctx, "slowpath", m_wrapper);
            Instruction* tmp = new UnreachableInst(ctx, slowpathBB);
            CallInst* callInst = InterpreterFunctionInterface::CreateDispatchToBytecodeSlowPath(slowpathFn, GetCoroutineCtx(), GetStackBase(), GetCurBytecode(), GetCodeBlock(), tmp /*insertBefore*/);
            tmp->eraseFromParent();
            std::unordered_map<uint64_t /*operandOrd*/, uint64_t /*argOrd*/> extraArgs = GetQuickeningSlowPathAdditionalArgs(m_bytecodeDef);
            for (auto& it : extraArgs)
            {
                uint64_t operandOrd = it.first;
                ReleaseAssert(operandOrd < usageValues.size());
                uint64_t argOrd = it.second;
                ReleaseAssert(argOrd < slowpathFn->arg_size());
                Type* desiredArgTy = slowpathFn->getArg(static_cast<uint32_t>(argOrd))->getType();
                Value* srcValue = usageValues[operandOrd];
                ReleaseAssert(llvm_value_has_type<uint64_t>(srcValue));
                Value* argValue;
                if (llvm_type_has_type<double>(desiredArgTy))
                {
                    argValue = new BitCastInst(srcValue, llvm_type_of<double>(ctx), "", callInst /*insertBefore*/);
                }
                else if (llvm_type_has_type<void*>(desiredArgTy))
                {
                    argValue = new IntToPtrInst(srcValue, llvm_type_of<void*>(ctx), "", callInst /*insertBefore*/);
                }
                else
                {
                    ReleaseAssert(llvm_type_has_type<uint64_t>(desiredArgTy));
                    argValue = srcValue;
                }
                ReleaseAssert(argValue->getType() == desiredArgTy);
                ReleaseAssert(dyn_cast<UndefValue>(callInst->getArgOperand(static_cast<uint32_t>(argOrd))) != nullptr);
                callInst->setArgOperand(static_cast<uint32_t>(argOrd), argValue);
            }
        }

        // Now, create each check of the quickening condition
        //
        Function* expectIntrin = Intrinsic::getDeclaration(m_module.get(), Intrinsic::expect, { Type::getInt1Ty(ctx) });
        TypeCheckFunctionSelector tcFnSelector(m_module.get());

        for (auto& it : m_bytecodeDef->m_quickening)
        {
            size_t operandOrd = it.m_operandOrd;
            ReleaseAssert(operandOrd < usageValues.size());
            TypeSpeculationMask mask = it.m_speculatedMask;
            TypeCheckFunctionSelector::QueryResult res = tcFnSelector.Query(mask, x_typeSpeculationMaskFor<tTop>);
            ReleaseAssert(res.m_opKind == TypeCheckFunctionSelector::QueryResult::CallFunction);
            Function* callee = res.m_func;
            ReleaseAssert(callee != nullptr && callee->arg_size() == 1 && llvm_value_has_type<uint64_t>(callee->getArg(0)) && llvm_type_has_type<bool>(callee->getReturnType()));
            CallInst* checkPassed = CallInst::Create(callee, { usageValues[operandOrd] }, "", currentBlock);
            ReleaseAssert(llvm_value_has_type<bool>(checkPassed));
            checkPassed = CallInst::Create(expectIntrin, { checkPassed, CreateLLVMConstantInt<bool>(ctx, true) }, "", currentBlock);

            BasicBlock* newBB = BasicBlock::Create(ctx, "", m_wrapper);
            BranchInst::Create(newBB /*ifTrue*/, slowpathBB /*ifFalse*/, checkPassed /*cond*/, currentBlock);
            currentBlock = newBB;
        }

        // We've passed all checks. Now we can call our specialized fastpath 'm_impl' which assumes these checks are true
        //
        CallInst::Create(m_impl, usageValues, "", currentBlock);
        new UnreachableInst(ctx, currentBlock);
    }
    else
    {
        // Otherwise, it's as simple as calling 'm_impl'
        //
        CallInst::Create(m_impl, usageValues, "", currentBlock);
        new UnreachableInst(ctx, currentBlock);
    }

    ValidateLLVMFunction(m_wrapper);
}

void InterpreterBytecodeImplCreator::LowerGetBytecodeMetadataPtrAPI()
{
    using namespace llvm;
    Function* fn = m_module->getFunction(x_get_bytecode_metadata_ptr_placeholder_fn_name);
    if (fn != nullptr)
    {
        std::unordered_set<CallInst*> instructionToReplace;
        for (Use& u : fn->uses())
        {
            User* usr = u.getUser();
            ReleaseAssert(isa<CallInst>(usr));
            CallInst* ci = cast<CallInst>(usr);
            ReleaseAssert(&u == &ci->getCalledOperandUse());
            ReleaseAssert(llvm_value_has_type<void*>(ci));
            ReleaseAssert(!instructionToReplace.count(ci));
            instructionToReplace.insert(ci);
        }
        if (!instructionToReplace.empty())
        {
            Value* mdPtr = m_valuePreserver.Get(x_metadataPtr);
            ReleaseAssert(llvm_value_has_type<void*>(mdPtr));
            for (CallInst* ci : instructionToReplace)
            {
                ReleaseAssert(ci->getParent() != nullptr);
                ReleaseAssert(ci->getParent()->getParent() == m_wrapper);
                ci->replaceAllUsesWith(mdPtr);
                ci->eraseFromParent();
            }
        }
        ReleaseAssert(fn->use_empty());
    }
}

InterpreterBytecodeImplCreator::InterpreterBytecodeImplCreator(BytecodeIrComponent& bic)
{
    using namespace llvm;
    m_bytecodeDef = bic.m_bytecodeDef;
    m_module = CloneModule(*bic.m_module.get());
    m_processKind = bic.m_processKind;
    m_impl = m_module->getFunction(bic.m_impl->getName());
    ReleaseAssert(m_impl != nullptr);
    ReleaseAssert(m_impl->getName() == bic.m_impl->getName());
    m_wrapper = nullptr;
    m_resultFuncName = bic.m_resultFuncName;
    m_generated = false;
}

std::unique_ptr<InterpreterBytecodeImplCreator> WARN_UNUSED InterpreterBytecodeImplCreator::LowerOneComponent(BytecodeIrComponent& bic)
{
    std::unique_ptr<InterpreterBytecodeImplCreator> ifi = std::make_unique<InterpreterBytecodeImplCreator>(bic);
    ifi->DoLowering();
    return ifi;
}

void InterpreterBytecodeImplCreator::DoLowering()
{
    using namespace llvm;

    ReleaseAssert(!m_generated);
    m_generated = true;

    // DoLowering() is alwasy called after the bytecode struct has been finalized.
    // Having decided the final bytecode metadata struct layout, we can lower all the metadata struct element getters.
    // Again, this relies on the fact that the metadata struct layout is solely determined by the Main processor.
    //
    ReleaseAssert(m_bytecodeDef->IsBytecodeStructLengthFinalized());
    if (m_bytecodeDef->HasBytecodeMetadata())
    {
        m_bytecodeDef->m_bytecodeMetadataMaybeNull->LowerAll(m_module.get());
    }

    // Create the wrapper function 'm_wrapper'
    // TODO: currently this happens after we decide whether the bytecode has metadata, because CreateWrapperFunction()
    // also decodes the metadata pointer. However, this is not ideal. In theory, the extra information in the wrapper
    // function (e.g., certain bytecode operands are constant) could eliminate calls in the bytecode, thus making
    // some of the metadata unnecessary, or even eliminate the metadata altogether.
    // This should be fixed by first create the wrapper and run desugaring, then decide the metadata by inspecting the
    // wrapper, and finally emit logic in the wrapper to decode the metadata pointer if it exists. However, we are
    // leaving it as is for now because we don't have such use cases.
    //
    CreateWrapperFunction();
    ReleaseAssert(m_wrapper != nullptr);

    // Inline 'm_impl' into 'm_wrapper'
    //
    if (m_impl->hasFnAttribute(Attribute::AttrKind::NoInline))
    {
        m_impl->removeFnAttr(Attribute::AttrKind::NoInline);
    }
    m_impl->addFnAttr(Attribute::AttrKind::AlwaysInline);
    m_impl->setLinkage(GlobalValue::InternalLinkage);

    DesugarAndSimplifyLLVMModule(m_module.get(), DesugaringLevel::PerFunctionSimplifyOnlyAggresive);
    RunLLVMDeadGlobalElimination(m_module.get());
    m_impl = nullptr;

    m_valuePreserver.RefreshAfterTransform();

    // Now we can do the lowerings
    //
    AstBytecodeReturn::LowerForInterpreter(this, m_wrapper);
    AstMakeCall::LowerForInterpreter(this, m_wrapper);
    AstReturnValueAccessor::LowerForInterpreter(this, m_wrapper);
    DeegenAllSimpleApiLoweringPasses::LowerAllForInterpreter(this, m_wrapper);
    LowerGetBytecodeMetadataPtrAPI();
    LowerInterpreterGetBytecodePtrInternalAPI(this, m_wrapper);
    AstSlowPath::LowerAllForInterpreter(this, m_wrapper);

    // All lowerings are complete.
    // Remove the NoReturn attribute since all pseudo no-return API calls have been replaced to dispatching tail calls
    //
    m_wrapper->removeFnAttr(Attribute::AttrKind::NoReturn);
    if (m_processKind == BytecodeIrComponentKind::Main && m_bytecodeDef->HasQuickeningSlowPath())
    {
        std::string slowPathFnName = GetQuickeningSlowPathFunctionNameFromBytecodeMainFunctionName(m_resultFuncName);
        Function* slowPathFn = m_module->getFunction(slowPathFnName);
        ReleaseAssert(slowPathFn != nullptr);
        ReleaseAssert(slowPathFn->hasFnAttribute(Attribute::AttrKind::NoReturn));
        slowPathFn->removeFnAttr(Attribute::AttrKind::NoReturn);
    }

    // Remove the value preserver annotations so optimizer can work fully
    //
    m_valuePreserver.Cleanup();

    // Now, having lowered everything, we can run the tag register optimization pass
    //
    // The tag register optimization pass is supposed to be run after all API calls have been inlined, so lower all the API calls
    //
    DesugarAndSimplifyLLVMModule(m_module.get(), DesugaringLevel::Top);

    // Now, run the tag register optimization pass
    //
    RunTagRegisterOptimizationPass(m_wrapper);

    // Run LLVM optimization pass
    //
    RunLLVMOptimizePass(m_module.get());

    // Run our homebrewed simple rewrite passes (targetting some insufficiencies of LLVM's optimizations of FCmp) after the main LLVM optimization pass
    //
    DeegenExtraLLVMOptPass_FuseTwoNaNChecksIntoOne(m_module.get());
    DeegenExtraLLVMOptPass_FuseNaNAndCmpCheckIntoOne(m_module.get());

    // After the optimization pass, change the linkage of everything to 'external' before extraction
    // This is fine: our caller will fix up the linkage for us.
    //
    for (Function& func : *m_module.get())
    {
        func.setLinkage(GlobalValue::ExternalLinkage);
        func.setComdat(nullptr);
    }

    // Sanity check that 'm_wrapper' is there
    //
    ReleaseAssert(m_module->getFunction(m_resultFuncName) == m_wrapper);
    ReleaseAssert(!m_wrapper->empty());

    // Extract the functions if m_processKind is not Main
    // For the main component, our caller will do the extraction.
    //
    if (m_processKind != BytecodeIrComponentKind::Main)
    {
        m_module = ExtractFunction(m_module.get(), m_resultFuncName);

        // After the extract, 'm_wrapper' is invalidated since a new module is returned. Refresh its value.
        //
        m_wrapper = m_module->getFunction(m_resultFuncName);
        ReleaseAssert(m_wrapper != nullptr);
    }
}

std::unique_ptr<llvm::Module> WARN_UNUSED InterpreterBytecodeImplCreator::DoLoweringForAll(BytecodeIrInfo& bi)
{
    using namespace llvm;

    BytecodeVariantDefinition* bytecodeDef = bi.m_bytecodeDef;

    size_t finalLength = bytecodeDef->GetTentativeBytecodeStructLength();
    for (BytecodeVariantDefinition* sameLengthConstraintVariant : bytecodeDef->m_sameLengthConstraintList)
    {
        size_t otherLength = sameLengthConstraintVariant->GetTentativeBytecodeStructLength();
        finalLength = std::max(finalLength, otherLength);
    }
    bytecodeDef->FinalizeBytecodeStructLength(finalLength);

    // Figure out the return continuations directly or transitively used by the main function.
    // These return continuations are considered hot, and will be put into the interpreter's hot code section
    // The other return continuations (which are used by calls that can only happen in the slow paths) will
    // be put into the cold code section.
    //
    std::unordered_set<std::string> fnNamesOfAllReturnContinuationsUsedByMainFn;
    ReturnContinuationAndSlowPathFinder rcFinder(bi.m_mainComponent->m_impl, true /*ignoreSlowPaths*/);
    for (Function* func : rcFinder.m_returnContinuations)
    {
        std::string rcImplName = func->getName().str();
        ReleaseAssert(rcImplName.ends_with("_impl"));
        std::string rcFinalName = rcImplName.substr(0, rcImplName.length() - strlen("_impl"));
        ReleaseAssert(!fnNamesOfAllReturnContinuationsUsedByMainFn.count(rcFinalName));
        fnNamesOfAllReturnContinuationsUsedByMainFn.insert(rcFinalName);
    }

    std::unique_ptr<Module> module;

    // Process the main component
    //
    {
        std::unique_ptr<InterpreterBytecodeImplCreator> mainComponent = InterpreterBytecodeImplCreator::LowerOneComponent(*bi.m_mainComponent.get());

        // Extract the necessary functions from the main component
        // We need to extract the result function and any IC body functions that didn't get inlined
        //
        std::vector<std::string> extractTargets;
        ReleaseAssert(mainComponent->m_module->getFunction(mainComponent->m_resultFuncName) != nullptr);
        extractTargets.push_back(mainComponent->m_resultFuncName);
        for (auto& icBodyFnName : bi.m_icBodyNames)
        {
            Function* icBodyFn = mainComponent->m_module->getFunction(icBodyFnName);
            if (icBodyFn != nullptr)
            {
                extractTargets.push_back(icBodyFnName);
                // I'm not certain if the IC body shall be put into the hot code section or the cold code section.
                // For now, let's not bother with this, and just put them in the normal code section like the other runtime functions.
                //
                ReleaseAssert(!icBodyFn->hasSection());
            }
            else
            {
                ReleaseAssert(mainComponent->m_module->getNamedValue(icBodyFnName) == nullptr);
            }
        }
        module = ExtractFunctions(mainComponent->m_module.get(), extractTargets);

        // Add section attribute to the main entry function
        //
        Function* mainEntryFn = module->getFunction(mainComponent->m_resultFuncName);
        ReleaseAssert(mainEntryFn != nullptr);

        ReleaseAssert(!mainEntryFn->hasSection());
        mainEntryFn->setSection(x_hot_code_section_name);
    }

    // Link in all the sub-components
    //
    // We also assign hot/cold section for each function.
    // The main function and any return continuations directly/transitively used by the main function are hot,
    // and everything else are cold.
    //
    // Link in the FuseICIntoInterpreterOpcode specializations, if any
    //
    ReleaseAssert(bi.m_affliatedBytecodeFnNames.size() == bi.m_fusedICs.size());
    for (size_t fusedInIcOrd = 0; fusedInIcOrd < bi.m_fusedICs.size(); fusedInIcOrd++)
    {
        std::unique_ptr<InterpreterBytecodeImplCreator> component = InterpreterBytecodeImplCreator::LowerOneComponent(*bi.m_fusedICs[fusedInIcOrd].get());

        std::unique_ptr<Module> spModule = std::move(component->m_module);
        std::string expectedFnName = bi.m_fusedICs[fusedInIcOrd]->m_resultFuncName;
        ReleaseAssert(spModule->getFunction(expectedFnName) != nullptr);
        ReleaseAssert(!spModule->getFunction(expectedFnName)->empty());
        ReleaseAssert(expectedFnName == bi.m_affliatedBytecodeFnNames[fusedInIcOrd]);

        Linker linker(*module.get());
        // linkInModule returns true on error
        //
        ReleaseAssert(linker.linkInModule(std::move(spModule)) == false);

        Function* linkedInFn = module->getFunction(expectedFnName);
        ReleaseAssert(linkedInFn != nullptr);
        ReleaseAssert(!linkedInFn->empty());
        ReleaseAssert(!linkedInFn->hasSection());
        linkedInFn->setSection(x_hot_code_section_name);
    }

    // Link in the quickening slow path if needed
    // We link in the quickening slow path before the return continuations so that related code stay closer to each other.
    // Though I guess the benefit of doing this is minimal, it doesn't hurt either, and it makes the assembly dump more readable..
    //
    ReleaseAssertIff(bi.m_bytecodeDef->HasQuickeningSlowPath(), bi.m_quickeningSlowPath.get() != nullptr);
    if (bi.m_bytecodeDef->HasQuickeningSlowPath())
    {
        std::unique_ptr<InterpreterBytecodeImplCreator> component = InterpreterBytecodeImplCreator::LowerOneComponent(*bi.m_quickeningSlowPath.get());

        std::unique_ptr<Module> spModule = std::move(component->m_module);
        std::string expectedSpName = bi.m_quickeningSlowPath->m_resultFuncName;
        ReleaseAssert(expectedSpName == GetQuickeningSlowPathFunctionNameFromBytecodeMainFunctionName(bi.m_mainComponent->m_resultFuncName));
        ReleaseAssert(spModule->getFunction(expectedSpName) != nullptr);
        ReleaseAssert(!spModule->getFunction(expectedSpName)->empty());

        Linker linker(*module.get());
        // linkInModule returns true on error
        //
        ReleaseAssert(linker.linkInModule(std::move(spModule)) == false);

        Function* linkedInFn = module->getFunction(expectedSpName);
        ReleaseAssert(linkedInFn != nullptr);
        ReleaseAssert(!linkedInFn->empty());
        ReleaseAssert(!linkedInFn->hasSection());
        linkedInFn->setSection(x_cold_code_section_name);
    }

    // Note that some of the return continuations could be dead (due to optimizations), however, since return continuations
    // may arbitrarily call each other, we cannot know a return continuation is dead until we have linked in all the return continuations.
    // So first we need to link in all return continuations.
    //
    for (size_t rcOrdinal = 0; rcOrdinal < bi.m_allRetConts.size(); rcOrdinal++)
    {
        std::unique_ptr<InterpreterBytecodeImplCreator> component = InterpreterBytecodeImplCreator::LowerOneComponent(*bi.m_allRetConts[rcOrdinal].get());

        std::unique_ptr<Module> rcModule = std::move(component->m_module);
        std::string expectedRcName = bi.m_allRetConts[rcOrdinal]->m_resultFuncName;
        ReleaseAssert(expectedRcName == GetInterpreterBytecodeReturnContinuationFunctionCName(bytecodeDef, rcOrdinal));
        ReleaseAssert(rcModule->getFunction(expectedRcName) != nullptr);
        ReleaseAssert(!rcModule->getFunction(expectedRcName)->empty());
        // Optimization pass may have stripped the return continuation function if it's not directly used by the main function
        // But if it exists, it should always be a declaration at this point
        //
        if (module->getFunction(expectedRcName) != nullptr)
        {
            ReleaseAssert(module->getFunction(expectedRcName)->empty());
        }

        Linker linker(*module.get());
        // linkInModule returns true on error
        //
        ReleaseAssert(linker.linkInModule(std::move(rcModule)) == false);

        Function* linkedInFn = module->getFunction(expectedRcName);
        ReleaseAssert(linkedInFn != nullptr);
        ReleaseAssert(!linkedInFn->empty());
        ReleaseAssert(!linkedInFn->hasSection());
        if (fnNamesOfAllReturnContinuationsUsedByMainFn.count(expectedRcName))
        {
            fnNamesOfAllReturnContinuationsUsedByMainFn.erase(expectedRcName);
            linkedInFn->setSection(x_hot_code_section_name);
        }
        else
        {
            linkedInFn->setSection(x_cold_code_section_name);
        }
    }
    ReleaseAssert(fnNamesOfAllReturnContinuationsUsedByMainFn.empty());

    // Similarly, some slow paths could be dead. But we need to link in everything first.
    //
    for (size_t slowPathOrd = 0; slowPathOrd < bi.m_slowPaths.size(); slowPathOrd++)
    {
        std::unique_ptr<InterpreterBytecodeImplCreator> component = InterpreterBytecodeImplCreator::LowerOneComponent(*bi.m_slowPaths[slowPathOrd].get());

        std::unique_ptr<Module> spModule = std::move(component->m_module);
        std::string expectedFnName = bi.m_slowPaths[slowPathOrd]->m_resultFuncName;
        ReleaseAssert(spModule->getFunction(expectedFnName) != nullptr);
        ReleaseAssert(!spModule->getFunction(expectedFnName)->empty());

        Linker linker(*module.get());
        // linkInModule returns true on error
        //
        ReleaseAssert(linker.linkInModule(std::move(spModule)) == false);

        Function* linkedInFn = module->getFunction(expectedFnName);
        ReleaseAssert(linkedInFn != nullptr);
        ReleaseAssert(!linkedInFn->empty());
        ReleaseAssert(!linkedInFn->hasSection());
        linkedInFn->setSection(x_cold_code_section_name);
    }

    // Now, having linked in everything, we can strip dead return continuations and slow paths by
    // changing the linkage of those functions to internal and run the DCE pass.
    //
    for (size_t rcOrdinal = 0; rcOrdinal < bi.m_allRetConts.size(); rcOrdinal++)
    {
        std::string rcName = bi.m_allRetConts[rcOrdinal]->m_resultFuncName;
        Function* func = module->getFunction(rcName);
        ReleaseAssert(func != nullptr);
        ReleaseAssert(!func->empty());
        ReleaseAssert(func->hasExternalLinkage());
        func->setLinkage(GlobalValue::InternalLinkage);
    }

    for (size_t slowPathOrd = 0; slowPathOrd < bi.m_slowPaths.size(); slowPathOrd++)
    {
        std::string spName = bi.m_slowPaths[slowPathOrd]->m_resultFuncName;
        Function* func = module->getFunction(spName);
        ReleaseAssert(func != nullptr);
        ReleaseAssert(!func->empty());
        ReleaseAssert(func->hasExternalLinkage());
        func->setLinkage(GlobalValue::InternalLinkage);
    }

    RunLLVMDeadGlobalElimination(module.get());

    // In theory it's fine to leave them with internal linkage now, since they are exclusively
    // used by our bytecode, but some of our callers expects them to have external linkage.
    // So we will change the surviving functions back to external linkage after DCE.
    //
    for (size_t rcOrdinal = 0; rcOrdinal < bi.m_allRetConts.size(); rcOrdinal++)
    {
        std::string rcName = bi.m_allRetConts[rcOrdinal]->m_resultFuncName;
        Function* func = module->getFunction(rcName);
        if (func != nullptr)
        {
            ReleaseAssert(func->hasInternalLinkage());
            func->setLinkage(GlobalValue::ExternalLinkage);
        }
        else
        {
            ReleaseAssert(module->getNamedValue(rcName) == nullptr);
        }
    }

    // Additionally, for slow paths, the function names are not unique across translational units.
    // So at this stage, we will rename them and give each of the survived slow path a unique name
    //
    std::vector<Function*> survivedSlowPathFns;
    for (size_t slowPathOrd = 0; slowPathOrd < bi.m_slowPaths.size(); slowPathOrd++)
    {
        std::string spName = bi.m_slowPaths[slowPathOrd]->m_resultFuncName;
        Function* func = module->getFunction(spName);
        if (func != nullptr)
        {
            ReleaseAssert(func->hasInternalLinkage());
            func->setLinkage(GlobalValue::ExternalLinkage);
            survivedSlowPathFns.push_back(func);
        }
        else
        {
            ReleaseAssert(module->getNamedValue(spName) == nullptr);
        }
    }

    for (size_t ord = 0; ord < survivedSlowPathFns.size(); ord++)
    {
        Function* func = survivedSlowPathFns[ord];
        std::string newFnName = bi.m_mainComponent->m_resultFuncName + "_slow_path_" + std::to_string(ord);
        ReleaseAssert(module->getNamedValue(newFnName) == nullptr);
        func->setName(newFnName);
        ReleaseAssert(func->getName() == newFnName);
    }

    return module;
}

}   // namespace dast

#include "deegen_interpreter_bytecode_impl_creator.h"
#include "deegen_bytecode_operand.h"
#include "deegen_ast_make_call.h"
#include "deegen_ast_return.h"
#include "deegen_ast_return_value_accessor.h"
#include "deegen_interpreter_function_interface.h"
#include "deegen_ast_throw_error.h"
#include "tvalue_typecheck_optimization.h"

#include "llvm/Linker/Linker.h"

// TODO: extract out needed logic
#include "bytecode.h"

namespace dast {

std::string WARN_UNUSED InterpreterBytecodeImplCreator::GetInterpreterBytecodeFunctionCName(BytecodeVariantDefinition* bytecodeDef)
{
    return std::string("__deegen_interpreter_op_") + bytecodeDef->m_bytecodeName + "_" + std::to_string(bytecodeDef->m_variantOrd);
}

InterpreterBytecodeImplCreator::InterpreterBytecodeImplCreator(BytecodeVariantDefinition* bytecodeDef, llvm::Function* implTmp, bool isReturnContinuation)
    : m_bytecodeDef(bytecodeDef)
    , m_module(nullptr)
    , m_isReturnContinuation(isReturnContinuation)
    , m_impl(nullptr)
    , m_wrapper(nullptr)
    , m_valuePreserver()
    , m_generated(false)
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

    LLVMContext& ctx = m_module->getContext();

    // For isReturnContinuation, our caller should have set up the desired function name for us
    // For !isReturnContinuation, we should rename by ourselves.
    //
    if (!isReturnContinuation)
    {
        std::string desiredFnName = GetInterpreterBytecodeFunctionCName(m_bytecodeDef) + "_impl";
        ReleaseAssert(m_module->getNamedValue(desiredFnName) == nullptr);
        m_impl->setName(desiredFnName);
    }
    else
    {
        ReleaseAssert(m_impl->getName().str().ends_with("_impl"));
    }

    {
        FunctionType* fty = InterpreterFunctionInterface::GetType(ctx);
        std::string finalFnName = m_impl->getName().str();
        ReleaseAssert(finalFnName.ends_with("_impl"));
        finalFnName = finalFnName.substr(0, finalFnName.length() - strlen("_impl"));
        ReleaseAssert(m_module->getNamedValue(finalFnName) == nullptr);
        m_wrapper = Function::Create(fty, GlobalValue::LinkageTypes::ExternalLinkage, finalFnName, m_module.get());
    }

    // Set parameter names just to make dumps more readable
    //
    ReleaseAssert(m_wrapper->arg_size() == 5);
    m_wrapper->getArg(0)->setName(x_coroutineCtx);
    m_wrapper->getArg(1)->setName(x_stackBase);
    if (m_isReturnContinuation)
    {
        m_wrapper->getArg(2)->setName(x_retStart);
        m_wrapper->getArg(3)->setName(x_numRet);
    }
    else
    {
        m_wrapper->getArg(2)->setName(x_curBytecode);
        m_wrapper->getArg(3)->setName(x_codeBlock);
    }

    // Set up the function attributes
    // TODO: add alias attributes to parameters
    //
    m_wrapper->addFnAttr(Attribute::AttrKind::NoReturn);
    m_wrapper->addFnAttr(Attribute::AttrKind::NoUnwind);
    CopyFunctionAttributes(m_wrapper /*dst*/, m_impl /*src*/);

    BasicBlock* entryBlock = BasicBlock::Create(ctx, "", m_wrapper);
    BasicBlock* currentBlock = entryBlock;

    if (!m_isReturnContinuation)
    {
        m_valuePreserver.Preserve(x_coroutineCtx, m_wrapper->getArg(0));
        m_valuePreserver.Preserve(x_stackBase, m_wrapper->getArg(1));
        m_valuePreserver.Preserve(x_curBytecode, m_wrapper->getArg(2));
        m_valuePreserver.Preserve(x_codeBlock, m_wrapper->getArg(3));
    }
    else
    {
        m_valuePreserver.Preserve(x_coroutineCtx, m_wrapper->getArg(0));
        m_valuePreserver.Preserve(x_stackBase, m_wrapper->getArg(1));
        m_valuePreserver.Preserve(x_retStart, m_wrapper->getArg(2));

        PtrToIntInst* numRet = new PtrToIntInst(m_wrapper->getArg(3), llvm_type_of<uint64_t>(ctx), "" /*name*/, currentBlock);
        m_valuePreserver.Preserve(x_numRet, numRet);

        Function* getCbFunc = LinkInDeegenCommonSnippet(m_module.get(), "GetCodeBlockFromStackFrameBase");
        ReleaseAssert(getCbFunc->arg_size() == 1 && llvm_type_has_type<void*>(getCbFunc->getFunctionType()->getParamType(0)));
        ReleaseAssert(llvm_type_has_type<void*>(getCbFunc->getFunctionType()->getReturnType()));
        Instruction* codeblock = CallInst::Create(getCbFunc, { GetStackBase() }, "" /*name*/, currentBlock);

        Function* getBytecodePtrFunc = LinkInDeegenCommonSnippet(m_module.get(), "GetBytecodePtrAfterReturnFromCall");
        ReleaseAssert(getBytecodePtrFunc->arg_size() == 2 &&
                      llvm_type_has_type<void*>(getBytecodePtrFunc->getFunctionType()->getParamType(0)) &&
                      llvm_type_has_type<void*>(getBytecodePtrFunc->getFunctionType()->getParamType(1)));
        ReleaseAssert(llvm_type_has_type<void*>(getBytecodePtrFunc->getFunctionType()->getReturnType()));
        Instruction* bytecodePtr = CallInst::Create(getBytecodePtrFunc, { GetStackBase(), codeblock }, "" /*name*/, currentBlock);

        m_valuePreserver.Preserve(x_codeBlock, codeblock);
        m_valuePreserver.Preserve(x_curBytecode, bytecodePtr);
    }

    std::vector<Value*> opcodeValues;
    for (auto& operand : m_bytecodeDef->m_list)
    {
        opcodeValues.push_back(operand->GetOperandValueFromBytecodeStruct(this, currentBlock));
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

    std::vector<Value*> usageValues;
    {
        size_t ord = 0;
        for (auto& operand : m_bytecodeDef->m_list)
        {
            usageValues.push_back(operand->EmitUsageValueFromBytecodeValue(this, currentBlock, opcodeValues[ord]));
            // Set name to make dump a bit more readable
            //
            usageValues.back()->setName(std::string("bc_operand_") + operand->OperandName());
            ord++;
        }
        ReleaseAssert(ord == opcodeValues.size() && ord == usageValues.size());
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

    CallInst::Create(m_impl, usageValues, "", currentBlock);
    new UnreachableInst(ctx, currentBlock);

    ValidateLLVMFunction(m_wrapper);
}

struct ReturnContinuationFinder
{
    ReturnContinuationFinder(llvm::Function* from)
    {
        m_count = 0;
        std::vector<AstMakeCall> list = AstMakeCall::GetAllUseInFunction(from);
        for (AstMakeCall& amc : list)
        {
            dfs(amc.m_continuation);
        }
        // We disallow the entry function itself to also be a continuation,
        // since the entry function cannot access return values while the continuation function can.
        //
        ReleaseAssert(!m_labelMap.count(from));
    }

    std::unordered_map<llvm::Function*, size_t> m_labelMap;
    size_t m_count;

private:
    void dfs(llvm::Function* cur)
    {
        if (cur == nullptr || m_labelMap.count(cur))
        {
            return;
        }
        m_labelMap[cur] = m_count;
        m_count++;

        std::vector<AstMakeCall> list = AstMakeCall::GetAllUseInFunction(cur);
        for (AstMakeCall& amc : list)
        {
            dfs(amc.m_continuation);
        }
    }
};

std::unique_ptr<llvm::Module> WARN_UNUSED InterpreterBytecodeImplCreator::ProcessReturnContinuation(llvm::Function* rc)
{
    ReleaseAssert(!m_isReturnContinuation);
    InterpreterBytecodeImplCreator ifi(m_bytecodeDef, rc, true /*isReturnContinuation*/);
    return ifi.DoOptimizationAndLowering();
}

std::unique_ptr<llvm::Module> WARN_UNUSED InterpreterBytecodeImplCreator::ProcessBytecode(BytecodeVariantDefinition* bytecodeDef, llvm::Function* impl)
{
    InterpreterBytecodeImplCreator ifi(bytecodeDef, impl, false /*isReturnContinuation*/);
    return ifi.DoOptimizationAndLowering();
}

void InterpreterBytecodeImplCreator::DoOptimization()
{
    using namespace llvm;
    ReleaseAssert(!m_generated);
    ReleaseAssert(m_impl != nullptr);
    TValueTypecheckOptimizationPass::DoOptimizationForBytecode(m_bytecodeDef, m_impl);
}

std::unique_ptr<llvm::Module> WARN_UNUSED InterpreterBytecodeImplCreator::DoLowering()
{
    using namespace llvm;
    ReleaseAssert(!m_generated);
    m_generated = true;

    std::string finalFnName = m_impl->getName().str();
    ReleaseAssert(finalFnName.ends_with("_impl"));
    finalFnName = finalFnName.substr(0, finalFnName.length() - strlen("_impl"));
    auto getRetContFinalNameForOrdinal = [this, finalFnName](size_t ord) -> std::string
    {
        ReleaseAssert(!m_isReturnContinuation);
        return finalFnName + "_retcont_" + std::to_string(ord);
    };

    // First step: if we are the main function (i.e., not return continuation), we shall parse out all the needed return
    // continuations, and process each of them
    //
    std::vector<std::unique_ptr<llvm::Module>> allRetConts;
    if (!m_isReturnContinuation)
    {
        // Find all the return continuations, give each of them a unique name, and create the declarations.
        // This is necessary for us to later link them together.
        //
        ReturnContinuationFinder rcFinder(m_impl);
        std::vector<Function*> rcList;
        rcList.resize(rcFinder.m_count, nullptr);
        ReleaseAssert(rcFinder.m_labelMap.size() == rcFinder.m_count);
        for (auto& it : rcFinder.m_labelMap)
        {
            ReleaseAssert(it.second < rcList.size());
            Function* rc = it.first;
            std::string rcFinalName = getRetContFinalNameForOrdinal(it.second);
            std::string rcImplName = rcFinalName + "_impl";
            ReleaseAssert(m_module->getNamedValue(rcImplName) == nullptr);
            rc->setName(rcImplName);
            ReleaseAssert(rcList[it.second] == nullptr);
            rcList[it.second] = rc;
            ReleaseAssert(m_module->getNamedValue(rcFinalName) == nullptr);
        }

        // After all the renaming and function declarations, process each of the return continuation
        //
        for (Function* targetRc : rcList)
        {
            ReleaseAssert(targetRc != nullptr);
            allRetConts.push_back(ProcessReturnContinuation(targetRc));
        }
    }

    // Now we can process our own function
    // Inline 'm_impl' into 'm_wrapper'
    //
    if (m_impl->hasFnAttribute(Attribute::AttrKind::NoInline))
    {
        m_impl->removeFnAttr(Attribute::AttrKind::NoInline);
    }
    m_impl->addFnAttr(Attribute::AttrKind::AlwaysInline);
    m_impl->setLinkage(GlobalValue::InternalLinkage);

    DesugarAndSimplifyLLVMModule(m_module.get(), DesugaringLevel::PerFunctionSimplifyOnly);
    m_impl = nullptr;

    m_valuePreserver.RefreshAfterTransform();

    // Now we can do the lowerings
    //
    AstBytecodeReturn::LowerForInterpreter(this, m_wrapper);
    AstMakeCall::LowerForInterpreter(this, m_wrapper);
    AstReturnValueAccessor::LowerForInterpreter(this, m_wrapper);
    DeegenLowerThrowErrorAPIForInterpreter(this, m_wrapper);

    // All lowerings are complete.
    // Remove the NoReturn attribute since all pseudo no-return API calls have been replaced to dispatching tail calls
    //
    m_wrapper->removeFnAttr(Attribute::AttrKind::NoReturn);

    // Remove the value preserver annotations so optimizer can work fully
    //
    m_valuePreserver.Cleanup();

    // Run optimization pass
    //
    RunLLVMOptimizePass(m_module.get());

    // After the optimization pass, change the linkage of everything to 'external' before extraction
    // This is fine: our caller will fix up the linkage for us.
    //
    for (Function& func : *m_module.get())
    {
        func.setLinkage(GlobalValue::ExternalLinkage);
        func.setComdat(nullptr);
    }
    for (GlobalVariable& gv : m_module->globals())
    {
        if (gv.hasLocalLinkage())
        {
            if (gv.isConstant() && gv.hasInitializer())
            {
                gv.setLinkage(GlobalValue::LinkOnceODRLinkage);
            }
            else
            {
                gv.setLinkage(GlobalValue::ExternalLinkage);
            }
        }
    }

    // Sanity check that the function we just created is there, and extract it
    //
    ReleaseAssert(m_module->getFunction(finalFnName) != nullptr);
    ReleaseAssert(!m_module->getFunction(finalFnName)->empty());
    m_module = ExtractFunction(m_module.get(), finalFnName);

    // If we are the main function, we also need to link in all the return continuations
    //
    ReleaseAssertImp(m_isReturnContinuation, allRetConts.size() == 0);
    for (size_t rcOrdinal = 0; rcOrdinal < allRetConts.size(); rcOrdinal++)
    {
        std::unique_ptr<Module> rcModule = std::move(allRetConts[rcOrdinal]);
        std::string expectedRcName = getRetContFinalNameForOrdinal(rcOrdinal);
        ReleaseAssert(rcModule->getFunction(expectedRcName) != nullptr);
        ReleaseAssert(!rcModule->getFunction(expectedRcName)->empty());
        // Optimization pass may have stripped the return continuation function if it's not directly used by the main function
        // But if it exists, it should always be a declaration at this point
        //
        if (m_module->getFunction(expectedRcName) != nullptr)
        {
            ReleaseAssert(m_module->getFunction(expectedRcName)->empty());
        }

        Linker linker(*m_module.get());
        // linkInModule returns true on error
        //
        ReleaseAssert(linker.linkInModule(std::move(rcModule)) == false);

        ReleaseAssert(m_module->getFunction(expectedRcName) != nullptr);
        ReleaseAssert(!m_module->getFunction(expectedRcName)->empty());
    }

    m_wrapper = nullptr;
    return std::move(m_module);
}

}   // namespace dast
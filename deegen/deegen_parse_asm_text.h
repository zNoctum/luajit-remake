#pragma once

#include "common.h"
#include "misc_llvm_helper.h"
#include "deegen_magic_asm_helper.h"

// A poorly-written and highly-inefficient makeshift tool to parse ASM text generated by LLVM
//
namespace dast {

enum class MagicAsmKind;
struct AsmMagicPayload;

struct X64AsmLine
{
    std::vector<std::string> m_components;
    std::vector<size_t> m_nonWhiteSpaceIdx;
    std::string m_trailingComments;
    std::string m_prefixingText;
    AsmMagicPayload* m_magicPayload;    // If it is a ASM magic, carries the details of the magic
    llvm::Instruction* m_originCertain;
    llvm::Instruction* m_originMaybe;
    uint32_t m_rawLocIdent;             // 0 if doesn't exist

    X64AsmLine()
        : m_magicPayload(nullptr)
        , m_originCertain(nullptr)
        , m_originMaybe(nullptr)
        , m_rawLocIdent(0)
    { }

    std::string WARN_UNUSED ToString()
    {
        std::string res = m_prefixingText;
        for (const std::string& comp : m_components) { res += comp; }
        res += m_trailingComments;
        res += "\n";
        return res;
    }

    size_t WARN_UNUSED NumWords() { return m_nonWhiteSpaceIdx.size(); }

    std::string& WARN_UNUSED GetWord(size_t ord)
    {
        ReleaseAssert(ord < m_nonWhiteSpaceIdx.size());
        return m_components[m_nonWhiteSpaceIdx[ord]];
    }

    // Note that function label will not pass this check because the label does not start with "."
    //
    bool WARN_UNUSED IsLocalLabel()
    {
        if (NumWords() != 1) { return false; }
        return GetWord(0).starts_with(".") && GetWord(0).ends_with(":");
    }

    std::string WARN_UNUSED ParseLabel()
    {
        ReleaseAssert(IsLocalLabel());
        return GetWord(0).substr(0, GetWord(0).length() - 1);
    }

    bool WARN_UNUSED IsCommentOrEmptyLine()
    {
        return NumWords() == 0;
    }

    // All the magic ASM sequence must start with a 'hlt' instruction and end with a 'hlt' instruction
    //
    bool WARN_UNUSED IsMagicInstruction()
    {
        return NumWords() == 1 && GetWord(0) == "hlt";
    }

    bool WARN_UNUSED IsMagicInstructionOfKind(MagicAsmKind kind);

    bool WARN_UNUSED IsInstruction()
    {
        if (NumWords() == 0) { return false; }
        if (GetWord(0).starts_with(".") || GetWord(0).starts_with("#") || GetWord(0).ends_with(":")) { return false; }
        return true;
    }

    bool WARN_UNUSED IsIndirectJumpInst()
    {
        if (!IsInstruction()) { return false; }
        return GetWord(0) == "jmpq";
    }

    bool WARN_UNUSED IsDirectUnconditionalJumpInst()
    {
        if (!IsInstruction()) { return false; }
        bool res = (GetWord(0) == "jmp");
        ReleaseAssertImp(res, NumWords() == 2);
        return res;
    }

    bool WARN_UNUSED IsConditionalJumpInst()
    {
        if (!IsInstruction()) { return false; }
        std::string& opcode = GetWord(0);
        if (opcode == "jmp" || opcode == "jmpq") { return false; }
        return opcode.starts_with("j");         // hopefully this is good enough..
    }

    void FlipConditionalJumpCondition()
    {
        ReleaseAssert(IsConditionalJumpInst());
        std::string& opcode = GetWord(0);
        ReleaseAssert(opcode.starts_with("j"));
        if (opcode.starts_with("jn"))
        {
            opcode = "j" + opcode.substr(2);
        }
        else
        {
            opcode = "jn" + opcode.substr(1);
        }
    }

    bool WARN_UNUSED IsDefinitelyBarrierInst()
    {
        if (!IsInstruction()) { return false; }
        std::string& opcode = GetWord(0);
        // For now... If this function return 'false' while the instruction is actually a barrier,
        // it is only a perf issue (we would emit an unnecessary jump), not a correctness issue.
        //
        return (opcode == "jmp" || opcode == "jmpq" || opcode == "ud2" || opcode == "ret");
    }

    bool WARN_UNUSED IsDirective()
    {
        if (NumWords() == 0) { return false; }
        return (GetWord(0).starts_with(".") && !IsLocalLabel());
    }

    bool WARN_UNUSED IsDiLocDirective()
    {
        if (!IsDirective()) { return false; }
        return GetWord(0) == ".loc";
    }

    uint32_t WARN_UNUSED ParseLineNumberFromDiLocDirective()
    {
        ReleaseAssert(IsDiLocDirective());
        ReleaseAssert(NumWords() >= 4);
        std::string str = GetWord(2);
        int res;
        try
        {
            res = std::stoi(str);
        }
        catch (...)
        {
            fprintf(stderr, "DILoc directive seems to contain a non-integer line number '%s'!\n", str.c_str());
            abort();
        }
        ReleaseAssert(res >= 0);
        return static_cast<uint32_t>(res);
    }

    bool WARN_UNUSED IsUnknown()
    {
        return !IsLocalLabel() && !IsCommentOrEmptyLine() && !IsInstruction() && !IsDirective();
    }

    static X64AsmLine WARN_UNUSED Parse(std::string line);
};

// Describes an ASM magic
//
// All ASM magic are required to have the following pattern:
//     hlt
//     int $XXX   # XXX := 100 + magicKind
//     <opaque instruction sequence>
//     int $XXX
//     hlt
//
// This allows us to reliably identify the pattern from the assembly code.
// The pattern is then transformed to a single 'hlt' AsmLine carrying a MagicPayload <magicKind, opaqueInstSequence>
//
struct AsmMagicPayload
{
    MagicAsmKind m_kind;
    std::vector<X64AsmLine> m_lines;
};

// The ASM output may have multiple labels coming in a row (mainly because we are taking BlockAddress..). These labels are all equal.
// This class normalize these equivalent labels to one of them
//
class X64LabelNameNormalizer
{
public:
    X64LabelNameNormalizer()
        : m_uniqueLabelOrd(0)
    { }

    std::string WARN_UNUSED GetNormalizedLabel(std::string label)
    {
        ReleaseAssert(m_map.count(label));
        return find(label);
    }

    void AddEquivalenceRelation(std::string l1, std::string l2)
    {
        ReleaseAssert(m_map.count(l1) && m_map.count(l2));
        l1 = find(l1);
        l2 = find(l2);
        ReleaseAssert(m_map.count(l1) && m_map[l1] == l1);
        ReleaseAssert(m_map.count(l2) && m_map[l2] == l2);
        m_map[l1] = l2;
    }

    void AddLabel(const std::string& s)
    {
        ReleaseAssert(!m_map.count(s));
        m_map[s] = s;
    }

    std::vector<std::string> WARN_UNUSED GetNormalizedLabelList()
    {
        std::vector<std::string> list;
        for (auto& it : m_map) { list.push_back(it.first); }

        std::vector<std::string> res;
        for (const std::string& s : list)
        {
            if (find(s) == s)
            {
                res.push_back(s);
            }
        }
        return res;
    }

    bool WARN_UNUSED QueryLabelExists(const std::string& label)
    {
        return m_map.count(label);
    }

    std::string WARN_UNUSED GetUniqueLabel()
    {
        std::string label = ".Ldeegen_tmp_unique_label_" + std::to_string(m_uniqueLabelOrd);
        m_uniqueLabelOrd++;
        AddLabel(label);
        return label;
    }

private:
    std::string WARN_UNUSED find(std::string s)
    {
        ReleaseAssert(m_map.count(s));
        if (s == m_map[s])
        {
            return s;
        }
        m_map[s] = find(m_map[s]);
        return m_map[s];
    }

    std::unordered_map<std::string, std::string> m_map;
    size_t m_uniqueLabelOrd;
};

struct X64AsmFile;

// A chunk of ASM code starting with a label, ending until reaching another label
//
struct X64AsmBlock
{
    X64AsmBlock() = default;

    // Split the block into part1 [0, line) and part2 [line, end)
    // part2 gets a unique label.
    // A branch instruction is inserted at the end of part1 to branch to part2
    //
    void SplitAtLine(X64AsmFile* owner, size_t line, X64AsmBlock*& part1 /*out*/, X64AsmBlock*& part2 /*out*/);

    X64AsmBlock* WARN_UNUSED Clone(X64AsmFile* owner);

    // Reorder blocks to maximize fallthroughs.
    //
    static std::vector<X64AsmBlock*> WARN_UNUSED ReorderBlocksToMaximizeFallthroughs(const std::vector<X64AsmBlock*>& input,
                                                                                     size_t entryOrd);

    std::string m_prefixText;

    // The label name for this block
    //
    std::string m_normalizedLabelName;

    // If 'm_endsWithJmpToLocalLabel' is true, the last instruction in the block is always an
    // unconditional jump to a local label, i.e., 'jmp m_terminalJmpTargetLabel'
    //
    // Otherwise the last instruction must be a barrier instruction that is either not a direct jump (e.g., an indirect jump),
    // or a direct jump to a label that is not local (e.g., a tail dispatch to another function)
    //
    // TODO: maintaining these fields are error prone. We should make them member functions that works by examining m_line...
    //
    bool m_endsWithJmpToLocalLabel;
    std::string m_terminalJmpTargetLabel;

    // The lines of ASM instructions. Each line must be IsInstruction()
    //
    std::vector<X64AsmLine> m_lines;
};

// Inject magic DILocation metadata, so we can reliably recover the CFG from the assembly after compilation
// Must be executed after all LLVM IR level transformation, that is, right before the module is compiled,
// since any IR transformation could potentially break debug info!
//
struct InjectedMagicDiLocationInfo
{
    static InjectedMagicDiLocationInfo WARN_UNUSED RunOnFunction(llvm::Function* func);

    // Return nullptr if it cannot be determined (this is possible because LLVM sometimes inject line=0, col=0
    // for assembly that does not correspond to an LLVM instruction, e.g., zeroing a register)
    //
    llvm::Instruction* WARN_UNUSED GetCertainInstructionOriginMaybeNullFromDILoc(uint32_t line);
    llvm::Instruction* WARN_UNUSED GetMaybeInstructionOriginMaybeNullFromDILoc(uint32_t line);
    llvm::Instruction* WARN_UNUSED GetIndirectBrSourceFromMagicAsmAnnotation(uint32_t ident);
    llvm::Function* GetFunc() { return m_func; }

    InjectedMagicDiLocationInfo()
        : m_func(nullptr)
        , m_hasUpdatedAfterCodegen(false)
    { }

    void UpdateAfterCodegen();

private:
    static constexpr uint32_t x_lineBase = 1000000000;
    static constexpr uint32_t x_lineInc = 10000;


    std::vector<uint32_t /*checkRem*/> m_list;
    llvm::Function* m_func;
    std::unordered_map<uint32_t, llvm::Instruction*> m_mappingCertain;
    std::unordered_map<uint32_t, llvm::Instruction*> m_mappingMaybe;
    std::unordered_map<uint32_t, llvm::Instruction*> m_markedIndirectBrMap;
    // It seems like LLVM codegen pass modifies module.. so even if we add debug metadata right before codegen pass,
    // it can still get somehow corrupted (e.g., an IR instruction may be sinked or duplicated).
    // For now, workaround by automatically removing those corrupted debug info
    //
    bool m_hasUpdatedAfterCodegen;
};

struct X64AsmFile
{
    // The list of X86AsmBlock in the order they show up in the file
    //
    std::vector<X64AsmBlock*> m_blocks;

    // Originally empty, may be set up by user
    //
    std::vector<X64AsmBlock*> m_slowpath;

    X64LabelNameNormalizer m_labelNormalizer;

    // Only the target function is parsed, everything else is stored raw here
    //
    std::string m_filePreheader;
    std::string m_fileFooter;

    // Memory owner
    //
    std::vector<std::unique_ptr<X64AsmBlock>> m_blockHolders;

    // Whether the terminator instruction of m_blocks[ord] is a jmp to next block (thus a no-op)
    //
    bool IsTerminatorInstructionFallthrough(size_t blockOrd)
    {
        ReleaseAssert(blockOrd < m_blocks.size());
        if (blockOrd == m_blocks.size() - 1)
        {
            return false;
        }
        return (m_blocks[blockOrd]->m_endsWithJmpToLocalLabel &&
                m_blocks[blockOrd]->m_terminalJmpTargetLabel == m_blocks[blockOrd + 1]->m_normalizedLabelName);
    }

    // Remove all asm magic of the specified kind, including in slow path
    //
    void RemoveAsmMagic(MagicAsmKind magicKind);

    void InsertBlocksAfter(const std::vector<X64AsmBlock*>& blocksToBeInserted, X64AsmBlock* insertAfter);
    void RemoveBlock(X64AsmBlock* blockToRemove);

    std::unique_ptr<X64AsmFile> WARN_UNUSED Clone();

    void Validate();

    static std::unique_ptr<X64AsmFile> WARN_UNUSED ParseFile(std::string fileContents, InjectedMagicDiLocationInfo diInfo);

    std::string WARN_UNUSED ToString();
    std::string WARN_UNUSED ToStringAndRemoveDebugInfo();
};

}   // namespace dast

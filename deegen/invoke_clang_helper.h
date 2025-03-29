#pragma once

#include "common.h"

namespace dast {

enum class Arch {
    X86,
    AArch64
};  // enum class Arch
// A simple helper function that compiles ASM (.s) file to object file (.o) using clang -O3
// Return the file contents of the object file as a string
//
std::string WARN_UNUSED CompileAssemblyFileToObjectFile(const std::string& asmFileContents, const std::string& extraCmdlineArgs, const Arch arch = Arch::AArch64);

// Compile a CPP file to object file or LLVM IR file using clang -O3
// If 'storePath' is provided, the file and compilation result will be stored there.
// Return the compilation result file contents as a string
//
std::string WARN_UNUSED CompileCppFileToObjectFile(const std::string& cppFileContents, const std::string& storePath = "", const Arch arch = Arch::AArch64);
std::string WARN_UNUSED CompileCppFileToLLVMBitcode(const std::string& cppFileContents, const std::string& storePath = "", const Arch arch = Arch::AArch64);

}   // namespace dast

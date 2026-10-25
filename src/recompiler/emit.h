#pragma once
// C++ generation: one function per guest function (src/runtime/ctx.h is the only header it needs).
#include "recompiler/analyze.h"

#include <map>
#include <string>
#include <vector>

namespace sa2rc {

struct OutputFile {
    std::string name;
    std::string text;
};

struct EmitStats {
    size_t functions = 0, instructions = 0, hle = 0, patchPoints = 0;
    std::map<std::string, size_t> unsupported;  // mnemonic/opcode -> count
    std::map<std::string, size_t> unresolvedTargets;  // "module:address" of direct branches to no function
};

// Splits main.dol into `dolChunks` files and each REL into files of about `relChunkInsns`
// instructions, plus headers and function tables (docs/PLAN.md).
std::vector<OutputFile> emitProgram(const Program& p, EmitStats& stats, int dolChunks = 24,
                                    size_t relChunkInsns = 60000);

}  // namespace sa2rc

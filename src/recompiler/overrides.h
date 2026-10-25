#pragma once
// Instruction overrides applied while generating C++ (sa2recompile --overrides <file>): systematic changes
// to the game's code compiled natively - loop bounds, array strides, arrays moved to new storage.
// docs/MORE_PLAYERS.md has the why; config/GKYE01/patches_8p.txt is the first user.
//
// File format (one directive per line, '#' starts a comment; numbers: 0x.. hex, 8 hex digits = an address,
// else decimal; an expression is terms joined by + and -, a term a number or a name from `set`):
//   set  NAME EXPR              a name for the expressions below
//   word ADDR EXPR              the instruction at ADDR becomes this word
//   imm  ADDR EXPR              ... keeps its opcode and registers, its 16-bit immediate becomes EXPR
//   was  ADDR EXPR              a check: the game's instruction at ADDR is this word (else the run fails:
//                               the file is for another program)
//   move ADDR FROM LEN TO       the address the instruction at ADDR computes - a load's or store's effective
//                               address, or the result of addi/addis/add/ori - is moved when it lies in
//                               [FROM, FROM+LEN): it becomes TO + (it - FROM). Several per instruction apply
//                               in turn (each to the result of the one before).
//   include FILE                another file of directives (relative to this one)
// Without --overrides the output is exactly what it was before any of this.
#include "recompiler/image.h"

#include <map>
#include <string>
#include <vector>

namespace sa2rc {

struct AddrMove {
    u32 from = 0, len = 0, to = 0;
};

struct CodeOverrides {
    std::map<u32, u32> words;                    // main.dol address -> replacement instruction word
    std::map<u32, std::vector<AddrMove>> moves;  // main.dol address -> moves of the address it computes
    // Memory-access tracing (sa2recompile --trace-mem LO:HI): every load/store whose address, and every
    // addi/addis/add/ori whose result, lies in [lo, hi) is reported to rt::memTrace (src/runtime/codeoverrides.cpp).
    bool trace = false;
    u32 traceLo = 0, traceHi = 0;
    bool any() const { return !words.empty() || !moves.empty() || trace; }
};

// Reads an override file (and its includes) into `out`; checks `was` lines against main.dol (`dol`).
bool loadOverrides(const std::string& path, const Module& dol, CodeOverrides& out, std::string& error);

}  // namespace sa2rc

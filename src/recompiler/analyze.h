#pragma once
// Function discovery for a module with no symbol map.
//
// Seeds: the entry point, REL prolog/epilog/unresolved, section starts, every call target (bl, and
// REL24 relocations from other modules). Each seed owns the region up to the next seed; a walk of
// its control flow inside that region gives the function's extent. A branch that leaves the region
// is a tail call, and its target becomes a seed; the walk is repeated until no seeds appear. Code
// nothing reaches (functions only called through pointers) is then seeded from the gaps.
// Code addresses found in data or materialised by lis/addi become seeds when they lie outside every
// function, and alias entries (a function running to the end of the containing one) when they point
// into one. Jump tables (cmplwi/lis/addi/lwzx/mtctr/bctr) stay inside their function as a switch.
#include "recompiler/image.h"
#include "recompiler/overrides.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace sa2rc {

struct Func {
    u32 start = 0;
    u32 end = 0;           // one past the last instruction
    bool alias = false;    // interior entry point of the containing function
    u32 body = 0;          // alias: start of the containing function, whose whole body it is emitted
                           // with (entered at `start`) so its branches back into it stay local
    bool hle = false;      // replaced by a host implementation (set from the names file)
    std::string name;      // SDK/library name when known
};

struct ModuleInfo {
    const Module* mod = nullptr;
    std::map<u32, Func> funcs;
    std::unordered_map<u32, std::vector<u32>> jumpTables;  // bctr address -> case targets
    std::set<u32> tableWords;                               // data words that are jump-table entries
    // statistics
    size_t gapSeeds = 0, pointerSeeds = 0, aliases = 0, tailSeeds = 0, unresolvedBctr = 0;
};

struct Program {
    std::vector<Module> modules;  // [0] = main.dol
    std::vector<ModuleInfo> info; // parallel to modules
    std::map<u32, int> patchPoints;  // main.dol address -> patch point slot (native mods hook in there)
    CodeOverrides overrides;         // sa2recompile --overrides / --trace-mem (recompiler/overrides.h)
    const Module* byId(u32 id) const {
        for (const auto& m : modules)
            if (m.id == id) return &m;
        return nullptr;
    }
};

// Discovers functions in every module (RELs first, so their calls into main.dol seed it).
void analyze(Program& p);

// Applies a names file: "<address> <name> [lib]" per line (main.dol addresses). Functions whose lib
// is one of the replaced SDK libraries are marked hle.
void applyNames(Program& p, const std::string& text, const std::set<std::string>& hleLibs);

std::string funcSymbol(const Module& m, u32 addr);  // C++ identifier of the function at addr

}  // namespace sa2rc

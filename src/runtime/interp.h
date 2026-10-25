#pragma once
// PowerPC interpreter for functions replaced at run time (src/runtime/interp.cpp).
#include "runtime/ctx.h"

#include <functional>
#include <vector>

namespace rt {

// Host code run in place of an instruction (an interpHookWord word in a patch or stub): returns
// the address to continue at, kInterpReturn to return from the function, or kInterpOriginal to
// run the instruction the game has there (after the hook's own work).
using InterpHook = std::function<u32(Ctx& c, u32 pc)>;
constexpr u32 kInterpReturn = 0xFFFFFFFFu;
constexpr u32 kInterpOriginal = 0xFFFFFFFEu;

void interpInit();                         // SA2_INTERP (checks against the translation)
void interpPatch(u32 addr, u32 word);      // the interpreter sees `word` at `addr`
void interpUnpatch(u32 addr);
u32 interpHookWord(InterpHook fn);         // an instruction word that runs `fn`
u32 interpStub(const std::vector<u32>& words);  // code in guest memory, interpreted where branched to
bool isStub(u32 addr);
int interpOverrideAt(u32 addr);            // functions containing `addr` run in the interpreter
void interpRun(Ctx& c, u32 entry, u32 body, u32 end);
// From a hook: runs one instruction `word` as if at `pc` in the function being interpreted;
// returns the next address (or kInterpReturn).
u32 interpStep(Ctx& c, u32 pc, u32 word);

// Whole functions: `fn` runs whenever the function is called (main.dol: module 0 and its address;
// a REL: its module id and section-relative address (section << 24 | offset), whether or not the
// module is linked now). It gets the address the function was entered at.
using InterpHandler = std::function<void(Ctx& c, u32 entry)>;
bool interpReplace(u32 module, u32 addr, InterpHandler fn);
void interpUnreplace(u32 module, u32 addr);  // its own code again (translated, unless patched)
// The function at `entry` runs its own code (in the interpreter: patched instructions apply, a
// replacement does not).
void interpOriginal(Ctx& c, u32 entry);
// Called after a REL module is linked or unlinked (the mod host re-applies its patches).
extern void (*g_relLinkHook)(u32 module, bool linked);

}  // namespace rt

#pragma once
// Tables the recompiler emits for the game module: main.dol's functions and, per REL, the
// functions at their section-relative addresses ((section << 24) | offset). OSLink registers a
// REL's functions at their real addresses once it knows where the game loaded the module.
#include "runtime/abi.h"
#include "runtime/ctx.h"

namespace rt {

struct FuncEntry {
    u32 addr;
    GuestFn fn;
    u32 body, end;  // the code it runs: [body, end) (body is before addr for an alias entry point)
};

struct NameEntry {
    u32 addr;
    const char* name;
};

struct HookEntry {
    const char* name;
    u32 addr;
    GuestFn orig;  // the translated body
};

struct RelModuleCode {
    u32 id;
    const char* file;
    const FuncEntry* funcs;
    u32 count;
    u32 slotBase;  // its functions' override slots: slotBase + index (main.dol's: the index)
};

// Native mod code at a patch point: true when it took over (the code after the point is skipped).
using PatchFn = bool (*)(Ctx&);

// What the game module (kirbyairride_game.dll) hands the runtime when it is loaded: its tables and the
// state its code owns. kGameAbi changes whenever this or anything in ctx.h the module relies on changes;
// the runtime refuses a module built for another (setup then builds it again).
constexpr u32 kGameAbi = KAR_GAME_ABI;
struct GameModule {
    u32 abi;
    const FuncEntry* dolFuncs;
    u32 dolFuncCount;
    const NameEntry* dolNames;
    u32 dolNameCount;
    const HookEntry* hookTable;
    u32 hookCount;
    HleFn* hooks;
    const RelModuleCode* relModules;
    u32 relModuleCount;
    u32 fnSlotCount;  // run-time replacement of any function: g_fnOverride[slot] set -> runOverride (interp.cpp)
    u8* fnOverride;
    PatchFn* patchPoints;
    const u32* patchPointAddrs;
    u32 patchPointCount;
    // the code overrides it was translated with (the 8-rider build's: config/GKYE01/patches_8p.txt; none: 0)
    const u32* overrideWords;
    u32 overrideWordCount;
    const u32* overrideMoves;
    u32 overrideMoveCount;
    u8** mem;
    volatile u32* irqPending;
    bool* traceOn;
    u32 (*relBase)[32];
};

#if defined(KAR_GAME_DLL)
extern const FuncEntry kDolFuncs[];
extern const u32 kDolFuncCount;
extern const NameEntry kDolNames[];
extern const u32 kDolNameCount;
extern const HookEntry kHookTable[];
extern const u32 kHookCount;
extern HleFn g_hooks[];
extern const RelModuleCode kRelModules[];
extern const u32 kRelModuleCount;
extern const u32 kFnSlotCount;
extern u8 g_fnOverride[];
extern PatchFn g_patchPoints[];
extern const u32 kPatchPointAddrs[];
extern const u32 kPatchPointCount;
extern const u32 kOverrideWords[];
extern const u32 kOverrideWordCount;
extern const u32 kOverrideMoves[];
extern const u32 kOverrideMoveCount;
#else
// the runtime's view: the loaded module's tables (src/runtime/game.cpp)
extern const GameModule* g_game;
#define kDolFuncs (::rt::g_game->dolFuncs)
#define kDolFuncCount (::rt::g_game->dolFuncCount)
#define kDolNames (::rt::g_game->dolNames)
#define kDolNameCount (::rt::g_game->dolNameCount)
#define kHookTable (::rt::g_game->hookTable)
#define kHookCount (::rt::g_game->hookCount)
#define g_hooks (::rt::g_game->hooks)
#define kRelModules (::rt::g_game->relModules)
#define kRelModuleCount (::rt::g_game->relModuleCount)
#define kFnSlotCount (::rt::g_game->fnSlotCount)
#define g_fnOverride (::rt::g_game->fnOverride)
#define g_patchPoints (::rt::g_game->patchPoints)
#define kPatchPointAddrs (::rt::g_game->patchPointAddrs)
#define kPatchPointCount (::rt::g_game->patchPointCount)
#endif

}  // namespace rt

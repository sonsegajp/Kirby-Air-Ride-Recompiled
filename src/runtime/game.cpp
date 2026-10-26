// The game module: kirbyairride_game.dll (the 8-rider build's: kirbyairride_game_8p.dll), the game's code
// translated by the recompiler from the player's own disc - built by first-run setup (src/setup) on the
// player's machine, never shipped, or for development by the CMake target kirbyairride_game. Loaded first
// thing; its tables and the state its code owns are reached through g_game and the pointers below
// (src/runtime/ctx.h, modules.h).
#include "runtime/modules.h"
#include "runtime/runtime.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <string>

namespace rt {

const GameModule* g_game = nullptr;
void registerCodeOverrides(const u32* words, u32 wordCount, const u32* moves, u32 moveCount);  // codeoverrides.cpp

namespace {
// stand-ins until the module is loaded (a flag set from the command line is carried over)
volatile u32 s_irqPending = 0;
bool s_traceOn = false;
u32 s_relBase[256][32];
}  // namespace
volatile u32* g_irqPendingPtr = &s_irqPending;
bool* g_traceOnPtr = &s_traceOn;
u32 (*g_relBasePtr)[32] = s_relBase;

// The module's file next to the program (the 8-rider build's or the game's own).
std::wstring gameModulePath(bool eightRiders) {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe, n);
    return path.substr(0, path.find_last_of(L"\\/") + 1) + (eightRiders ? L"kirbyairride_game_8p.dll" : L"kirbyairride_game.dll");
}

// Loads it; false with the reason in `error` when it is missing or was built for another version of the runtime.
bool loadGameModule(bool eightRiders, std::string& error) {
    const std::wstring path = gameModulePath(eightRiders);
    HMODULE m = LoadLibraryW(path.c_str());
    if (!m) {
        const DWORD e = GetLastError();
        error = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ? "The game has not been set up on this computer yet."
                                                                             : "The game module couldn't be loaded (error " + std::to_string(e) + ").";
        return false;
    }
    using Fn = const GameModule* (*)();
    const auto get = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, "kirbyairride_game_module")));
    const GameModule* g = get ? get() : nullptr;
    if (!g || g->abi != kGameAbi) {
        FreeLibrary(m);
        error = "The game was set up by another version of this program.";
        return false;
    }
    *g->irqPending = s_irqPending;
    *g->traceOn = s_traceOn;
    g_irqPendingPtr = g->irqPending;
    g_traceOnPtr = g->traceOn;
    g_relBasePtr = g->relBase;
    g_game = g;
    // the code overrides it was translated with (the 8-rider build's), before main.dol is loaded
    if (g->overrideWordCount || g->overrideMoveCount)
        registerCodeOverrides(g->overrideWords, g->overrideWordCount, g->overrideMoves, g->overrideMoveCount);
    return true;
}

// Once guest memory exists: the module's copy of its base.
void gameModuleMemory() {
    if (g_game) *g_game->mem = g_mem;
}

}  // namespace rt

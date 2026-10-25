#pragma once
// More than 4 riders (docs/MORE_PLAYERS.md): where host code finds the game's per-player arrays. In the 8-player
// build (generated with config/GKYE01/patches_8p.txt) they live in new, larger storage; in the normal build where
// the game has them.
#include "runtime/ctx.h"
#include "runtime/morepl_layout.h"

namespace rt {
bool codeMovesActive();  // src/runtime/codeoverrides.cpp
}

namespace rt::morepl {
inline bool active() { return codeMovesActive(); }
// Element i of a moved array.
inline u32 at(const Moved& m, u32 i) { return (active() ? m.to : m.addr) + i * m.stride; }
inline u32 count(const Moved& m) { return active() ? m.count : m.oldCount; }
// The 8-player build's slots: riders in 0-3 and 6-9; 4 stays King Dedede's (VS. King Dedede) and 5 stays empty,
// because the game uses 5 for "nobody" (a camera's target, a pad's player, ...).
inline int riders() { return active() ? 8 : 4; }  // human and CPU riders
inline int slots() { return active() ? 10 : 5; }  // with King Dedede's (and the empty one)
inline u32 riderSlot(int i) { return i < 4 ? u32(i) : u32(i + 2); }
inline bool isRiderSlot(u32 s) { return s < 4 || (active() && s >= 6 && s < 10); }
// The game's player setup for a match (gm +AC8: kind, rider, bike, machine, color, rumble, pad, ...; 0x30 each).
inline u32 plyDesc(u32 i) { return at(k_ply_desc, i); }
void frame();       // each frame: HUD layout for more than 4 riders (src/runtime/morepl.cpp)
void debugFrame();  // SA2_8P_DEBUG: the slots, logged (src/runtime/morepl.cpp)
void stallDump();   // (from the frontend's hang report) the game's file preload table, logged
void install();  // the 8-player build's function replacements (src/runtime/morepl.cpp)
}  // namespace rt::morepl

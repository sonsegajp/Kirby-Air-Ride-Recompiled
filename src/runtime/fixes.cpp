// Changes to the game's own code (widescreen, culling and the like), applied while the module they
// are in is linked (instructions run through the interpreter with host code in their place,
// src/runtime/interp.h). Kirby Air Ride has none yet.
#include "runtime/interp.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <vector>

namespace rt {

std::atomic<float> g_wideExtra{0.f};  // logical 640x480 units shown beyond each side of the 4:3 screen

namespace {
struct Patch {
    u32 module, section, offset, original;
    InterpHook hook;
};
std::vector<u32> s_applied[256];  // addresses patched per module

std::vector<Patch> patches() { return {}; }
}  // namespace

void gameFixesLinked(u32 module) {
    static const std::vector<Patch> s_patches = patches();
    for (const Patch& p : s_patches) {
        if (p.module != module) continue;
        const u32 a = g_relBase[module][p.section] + p.offset;
        if (rd32(a) != p.original) {
            log("[fixes] module %u +%X: %08X, not the expected %08X - left alone\n", module, p.offset, rd32(a), p.original);
            continue;
        }
        interpPatch(a, interpHookWord(p.hook));
        interpOverrideAt(a);
        s_applied[module & 255].push_back(a);
    }
}

void gameFixesUnlinked(u32 module) {
    for (u32 a : s_applied[module & 255]) interpUnpatch(a);
    s_applied[module & 255].clear();
}

void gameFixesInit() {
    // SA2_LOGIC_STEPS (debugging): how many logic steps the main loop runs per drawn frame (the
    // retraces since the last one, f_800628A0, checked at 80006BD8), logged every 600 frames.
    if (std::getenv("SA2_LOGIC_STEPS")) {
        interpPatch(0x80006BD8, interpHookWord([](Ctx& c, u32) -> u32 {
            static u32 s_hist[4], s_n;
            s_hist[std::min<u32>(c.r[3] & 0xFF, 3)]++;
            if (++s_n % 600 == 0) {
                log("[logic] steps per frame over 600 frames: 0:%u 1:%u 2:%u 3+:%u\n", s_hist[0], s_hist[1], s_hist[2], s_hist[3]);
                s_hist[0] = s_hist[1] = s_hist[2] = s_hist[3] = 0;
            }
            return kInterpOriginal;
        }));
        interpOverrideAt(0x80006BD8);
    }
}
void gameFixesFrame() {}

}  // namespace rt

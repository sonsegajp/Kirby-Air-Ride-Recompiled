// Indirect calls: guest code address -> native function, in O(1). Two-level table over the
// physical address (cached/uncached mirrors resolve alike); pages are allocated on demand.
#include "runtime/interp.h"
#include "runtime/modules.h"
#include "runtime/gx.h"
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace rt {

namespace {
constexpr u32 kPageBits = 12;
GuestFn* s_pages[kHostMemSize >> kPageBits];

GuestFn* slot(u32 addr, bool create) {
    const u32 p = phys(addr);
    GuestFn*& page = s_pages[p >> kPageBits];
    if (!page) {
        if (!create) return nullptr;
        page = new GuestFn[1u << (kPageBits - 2)]();
    }
    return &page[(p & ((1u << kPageBits) - 1)) >> 2];
}
}  // namespace

void dispatchRegister(u32 addr, GuestFn fn) { *slot(addr, true) = fn; }

void dispatchUnregister(u32 addr) {
    if (GuestFn* s = slot(addr, false)) *s = nullptr;
}

GuestFn dispatchLookup(u32 addr) {
    GuestFn* s = slot(addr, false);
    return s ? *s : nullptr;
}

void registerDol() {
    for (u32 i = 0; i < kDolFuncCount; i++) dispatchRegister(kDolFuncs[i].addr, kDolFuncs[i].fn);
}

static const RelModuleCode* relCode(u32 id) {
    for (u32 i = 0; i < kRelModuleCount; i++)
        if (kRelModules[i].id == id) return &kRelModules[i];
    return nullptr;
}

std::atomic<u64> g_relLinkFrame[256];      // game frame at which each module was last linked (+1; 0 = never)
std::atomic<bool> g_stopRequested{false};  // test runs: the frontend exits at its next frame
std::atomic<bool> g_relLinked[256];        // which modules are linked now

void registerRelModule(u32 id) {
    const RelModuleCode* m = relCode(id);
    if (!m) fatal("OSLink: no recompiled code for REL module %u", id);
    for (u32 i = 0; i < m->count; i++) {
        const u32 a = m->funcs[i].addr;
        dispatchRegister(g_relBase[id][a >> 24] + (a & 0xFFFFFF), m->funcs[i].fn);
    }
    struct Later {  // after the module counts as linked (below)
        u32 id;
        ~Later() {
            if (g_relLinkHook) g_relLinkHook(id, true);
        }
    } later{id};
    std::string secs;  // every placed section: index@address
    for (u32 s = 0; s < 20; s++)
        if (g_relBase[id][s]) {
            char b[24];
            std::snprintf(b, sizeof b, " %u@%08X", s, g_relBase[id][s]);
            secs += b;
        }
    log("[rel] linked module %u (%s): %u functions at frame %llu, sections%s\n", id, m->file, m->count,
        (unsigned long long)gxFrames(), secs.c_str());
    g_relLinkFrame[id & 255] = gxFrames() + 1;
    g_relLinked[id & 255] = true;
    gameFixesLinked(id);
}

void unregisterRelModule(u32 id) {
    gameFixesUnlinked(id);
    // Like the console, an unlinked module's code stays callable at its old addresses until
    // something else is linked there (the game calls into a module after OSUnlink, e.g. eventD's
    // callbacks); a module linked later at the same addresses simply overwrites these entries.
    const RelModuleCode* m = relCode(id);
    if (m) log("[rel] unlinked module %u (%s)\n", id, m->file);
    g_relLinked[id & 255] = false;
    if (g_relLinkHook) g_relLinkHook(id, false);
    // Test runs: SA2_STOP_ON_UNLINK=<id> ends the run when that module goes (e.g. an event ended).
    if (const char* s = std::getenv("SA2_STOP_ON_UNLINK"); s && u32(std::atoi(s)) == id) g_stopRequested = true;
    extern std::atomic<int> g_traceBurst;
    if (const char* n = std::getenv("SA2_TRACE_AFTER_UNLINK")) g_traceBurst = std::atoi(n);
}

void Ctx::call(u32 addr) {
    // Only the thread that owns the guest CPU runs guest code: two at once corrupt the game's
    // memory (its locks are "interrupts off", which only hold on one CPU). Reported a few times.
    if (u32 self = 0, owner = 0; !sched::ownsCpu(&self, &owner)) [[unlikely]] {
        static std::atomic<int> s_reported{0};
        if (s_reported++ < 5) log("[sched] guest thread %08X calls %08X (lr %08X) while %08X owns the CPU\n", self, addr, lr, owner);
    }
    if (GuestFn fn = dispatchLookup(addr)) [[likely]] {
        // The task system calling a task's function (it names it in kTaskFnVar first): what the
        // task draws is tagged with it (src/runtime/gx.cpp), for in-between frames.
        if (kTaskFnVar && addr == rd32(kTaskFnVar)) {
            gxTaskEnter(r[3], addr);
            fn(*this);
            gxTaskLeave();
            return;
        }
        // SA2_CHECK_ABI (debugging): a callee must give back r1 and r14-r31 as it found them;
        // the first few that do not are reported.
        static const bool s_checkAbi = std::getenv("SA2_CHECK_ABI") != nullptr;
        if (s_checkAbi) [[unlikely]] {
            const u32 from = lr;
            u32 saved[19];
            saved[0] = r[1];
            std::memcpy(saved + 1, &r[14], 18 * sizeof(u32));
            fn(*this);
            if (saved[0] != r[1] || std::memcmp(saved + 1, &r[14], 18 * sizeof(u32))) {
                static std::atomic<int> s_reported{0};
                if (s_reported++ < 10) {
                    log("[abi] %08X (called from %08X) returned with changed registers:", addr, from);
                    if (saved[0] != r[1]) log(" r1 %08X->%08X", saved[0], r[1]);
                    for (int i = 0; i < 18; i++)
                        if (saved[1 + i] != r[14 + i]) log(" r%d %08X->%08X", 14 + i, saved[1 + i], r[14 + i]);
                    log("\n");
                }
            }
            return;
        }
        fn(*this);
        return;
    }
    // The caller's registers: the object the bad pointer came from is usually in one of them.
    for (int i = 0; i < 32; i++) log("r%-2d %08X%s", i, r[i], i % 8 == 7 ? "\n" : "  ");
    // ... and the guest's calls that led here (back chain: each frame links to its caller's, LR saved at +4)
    log("guest stack:");
    for (u32 sp = r[1], i = 0; i < 20 && (sp >> 24) == 0x80 + ((sp >> 24) & 1); i++) {
        sp = rd32(sp);
        if (!sp) break;
        log(" %08X", rd32(sp + 4));
    }
    log("\n");
    if (addr == 0) fatal("call to null (lr=%08X r3=%08X r12=%08X)", lr, r[3], r[12]);
    fatal("call to %08X: no function starts there (lr=%08X)", addr, lr);
}

}  // namespace rt

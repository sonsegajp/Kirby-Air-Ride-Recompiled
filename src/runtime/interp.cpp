// PowerPC interpreter for functions replaced at run time (the mod host's translated patches).
//
// Every translated function checks its override flag first (g_fnOverride, see modules.h); a set
// flag runs the function here instead, instruction by instruction from guest memory, where some
// instruction words may be overridden (interpPatch) or be calls of host code (interpHook). The
// semantics are exactly those the recompiler emits (src/recompiler/emit.cpp), so an overridden
// function without patches behaves as its translation. Calls and branches out of the function go
// to the translated code (or other overrides) as they would from the translation.
//
// SA2_INTERP=<addr>[,<addr>...] or SA2_INTERP=all runs those functions (every function) here with
// no patches: a check of the interpreter against the translation.
#include "runtime/interp.h"

#include "runtime/modules.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rt {
bool codeMovesActive();       // src/runtime/codeoverrides.cpp
u32 codeMove(u32 pc, u32 x);

extern std::atomic<bool> g_relLinked[256];

namespace {

// The recompiler's address moves (src/runtime/codeoverrides.cpp; programs generated with --overrides): the
// interpreter moves what the translation moves, by the address of the instruction it runs.
thread_local u32 t_pc = 0;
u32 MVI(u32 x) { return codeMovesActive() ? codeMove(t_pc, x) : x; }

u32 bits(u32 w, int from, int to) { return (w >> (31 - to)) & ((1u << (to - from + 1)) - 1); }
s32 sext16(u32 v) { return s32(s16(u16(v))); }
u32 maskOf(u32 mb, u32 me) {
    const u32 m1 = 0xFFFFFFFFu >> mb;
    const u32 m2 = me == 31 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> (me + 1));
    return mb <= me ? (m1 & m2) : (m1 | m2);
}

// Patched instruction words, and host hooks (instruction words 0x04000000 | index: primary
// opcode 1 does not exist on the Gekko).
std::unordered_map<u32, u32> s_patches;
std::deque<InterpHook> s_hooks;  // deque: a hook may add hooks while it runs
std::unordered_map<u32, InterpHandler> s_handlers;  // by override slot
std::mutex s_lock;  // registration only: the game runs its code on one thread at a time

// Code the mod host writes (trampolines for inserted calls): interpreted, as part of whatever
// function branches to it.
std::vector<std::pair<u32, u32>> s_stubChunks;
u32 s_stubTop = 0, s_stubEnd = 0;

bool s_interpAll = false;
std::unordered_set<u32> s_forced;  // slots SA2_INTERP asked for: they stay interpreted

struct Fn {
    u32 entry, body, end;
};
bool slotFunction(u32 slot, Fn& out) {
    if (slot < kDolFuncCount) {
        const FuncEntry& e = kDolFuncs[slot];
        out = {e.addr, e.body, e.end};
        return true;
    }
    for (u32 i = 0; i < kRelModuleCount; i++) {
        const RelModuleCode& m = kRelModules[i];
        if (slot < m.slotBase || slot >= m.slotBase + m.count) continue;
        if (!g_relLinked[m.id]) return false;
        const FuncEntry& e = m.funcs[slot - m.slotBase];
        auto real = [&](u32 a) { return g_relBase[m.id][a >> 24] + (a & 0xFFFFFF); };
        out = {real(e.addr), real(e.body), real(e.end)};
        return true;
    }
    return false;
}

// Every slot whose function (as linked now) contains `addr`: main.dol's, then each linked module's.
template <typename F>
void forEachSlotContaining(u32 addr, F&& fn) {
    for (u32 i = 0; i < kDolFuncCount; i++)
        if (addr >= kDolFuncs[i].body && addr < kDolFuncs[i].end) fn(i);
    for (u32 i = 0; i < kRelModuleCount; i++) {
        const RelModuleCode& m = kRelModules[i];
        if (!g_relLinked[m.id]) continue;
        for (u32 k = 0; k < m.count; k++) {
            const FuncEntry& e = m.funcs[k];
            const u32 base = g_relBase[m.id][e.body >> 24];
            if (addr >= base + (e.body & 0xFFFFFF) && addr < base + (e.end & 0xFFFFFF)) fn(m.slotBase + k);
        }
    }
}

// The slot's function still has a patch or a replacement (else it can run its translation again).
bool stillInterpreted(u32 slot) {
    if (s_interpAll || s_forced.count(slot) || s_handlers.count(slot)) return true;
    Fn f;
    if (!slotFunction(slot, f)) return false;
    for (const auto& [a, w] : s_patches)
        if (a >= f.body && a < f.end) return true;
    return false;
}

u32 fetch(u32 pc) {
    if (!s_patches.empty()) {
        const auto it = s_patches.find(pc);
        if (it != s_patches.end()) return it->second;
    }
    return rd32(pc);
}

[[noreturn]] void unsupported(Ctx& c, u32 pc, u32 w) { c.unimplemented(pc, w); }

void trapIf(Ctx& c, u32 pc, u32 to, u32 a, u32 b) {
    const bool t = ((to & 16) && s32(a) < s32(b)) || ((to & 8) && s32(a) > s32(b)) || ((to & 4) && a == b) ||
                   ((to & 2) && a < b) || ((to & 1) && a > b);
    if (t) c.trap(pc);
}

// Branch condition (BO, BI); decrements CTR when BO says so.
bool cond(Ctx& c, u32 bo, u32 bi) {
    bool ok = true;
    if (!(bo & 0x04)) {
        c.ctr--;
        ok = (bo & 0x02) ? c.ctr == 0 : c.ctr != 0;
    }
    if (!(bo & 0x10)) ok = ok && (c.crbit(int(bi)) == bool(bo & 0x08));
    return ok;
}

void setPair(Ctx& c, u32 d, double t0, double t1, bool round, bool rc) {
    if (round) t0 = Ctx::rs(t0), t1 = Ctx::rs(t1);
    c.f[d] = t0;
    c.ps1[d] = t1;
    if (rc) c.rc1();
}

void op4(Ctx& c, u32 pc, u32 w) {
    const u32 d = bits(w, 6, 10), a = bits(w, 11, 15), b = bits(w, 16, 20), cc = bits(w, 21, 25);
    const u32 xo10 = bits(w, 21, 30), xo6 = bits(w, 25, 30), xo5 = bits(w, 26, 30);
    const bool rc = w & 1;
    const double A0 = c.f[a], A1 = c.ps1[a], B0 = c.f[b], B1 = c.ps1[b], C0 = c.f[cc], C1 = c.ps1[cc];
    auto neg = [](double v) { return dbl(bits64(v) ^ 0x8000000000000000ull); };
    switch (xo10) {
    case 0: case 32: c.fcmp(int(d >> 2), A0, B0); return;
    case 64: case 96: c.fcmp(int(d >> 2), A1, B1); return;
    case 40: setPair(c, d, neg(B0), neg(B1), false, rc); return;
    case 72: setPair(c, d, B0, B1, false, rc); return;
    case 136: setPair(c, d, dbl(bits64(B0) | 0x8000000000000000ull), dbl(bits64(B1) | 0x8000000000000000ull), false, rc); return;
    case 264: setPair(c, d, dbl(bits64(B0) & 0x7FFFFFFFFFFFFFFFull), dbl(bits64(B1) & 0x7FFFFFFFFFFFFFFFull), false, rc); return;
    case 528: setPair(c, d, A0, B0, false, rc); return;
    case 560: setPair(c, d, A0, B1, false, rc); return;
    case 592: setPair(c, d, A1, B0, false, rc); return;
    case 624: setPair(c, d, A1, B1, false, rc); return;
    case 1014: c.dcbz((a ? c.r[a] : 0) + c.r[b]); return;
    default: break;
    }
    if (xo6 == 6 || xo6 == 7 || xo6 == 38 || xo6 == 39) {
        const bool wbit = bits(w, 21, 21);
        const int i = int(bits(w, 22, 24));
        const u32 ea = MVI(xo6 >= 38 ? c.r[a] + c.r[b] : (a ? c.r[a] : 0) + c.r[b]);
        if (xo6 & 1) c.psqStore(int(d), ea, wbit, i);
        else c.psqLoad(int(d), ea, wbit, i);
        if (xo6 >= 38) c.r[a] = ea;
        return;
    }
    auto fma = [](double x, double y, double z) { return __builtin_fma(x, y, z); };
    switch (xo5) {
    case 10: setPair(c, d, A0 + B1, C1, true, rc); return;
    case 11: setPair(c, d, C0, A0 + B1, true, rc); return;
    case 12: setPair(c, d, A0 * C0, A1 * C0, true, rc); return;
    case 13: setPair(c, d, A0 * C1, A1 * C1, true, rc); return;
    case 14: setPair(c, d, fma(A0, C0, B0), fma(A1, C0, B1), true, rc); return;
    case 15: setPair(c, d, fma(A0, C1, B0), fma(A1, C1, B1), true, rc); return;
    case 18: setPair(c, d, A0 / B0, A1 / B1, true, rc); return;
    case 20: setPair(c, d, A0 - B0, A1 - B1, true, rc); return;
    case 21: setPair(c, d, A0 + B0, A1 + B1, true, rc); return;
    case 23: setPair(c, d, Ctx::fsel(A0, C0, B0), Ctx::fsel(A1, C1, B1), false, rc); return;
    case 24: setPair(c, d, Ctx::fres(B0), Ctx::fres(B1), true, rc); return;
    case 25: setPair(c, d, A0 * C0, A1 * C1, true, rc); return;
    case 26: setPair(c, d, Ctx::frsqrte(B0), Ctx::frsqrte(B1), true, rc); return;
    case 28: setPair(c, d, fma(A0, C0, -B0), fma(A1, C1, -B1), true, rc); return;
    case 29: setPair(c, d, fma(A0, C0, B0), fma(A1, C1, B1), true, rc); return;
    case 30: setPair(c, d, -fma(A0, C0, -B0), -fma(A1, C1, -B1), true, rc); return;
    case 31: setPair(c, d, -fma(A0, C0, B0), -fma(A1, C1, B1), true, rc); return;
    default: unsupported(c, pc, w);
    }
}

void op31(Ctx& c, u32 pc, u32 w) {
    const u32 d = bits(w, 6, 10), a = bits(w, 11, 15), b = bits(w, 16, 20);
    const u32 xo = bits(w, 21, 30), xo9 = bits(w, 22, 30);
    const bool rc = w & 1, oe = bits(w, 21, 21);
    u32& D = c.r[d];
    const u32 A = c.r[a], B = c.r[b], S = c.r[d];
    // XO-form arithmetic
    {
        bool hit = true;
        u32 v = 0;
        switch (xo9) {
        case 266: v = MVI(A + B); break;
        case 10: v = c.addCarry(A, B, 0); break;
        case 138: v = c.addCarry(A, B, c.ca); break;
        case 234: v = c.addCarry(A, 0xFFFFFFFFu, c.ca); break;
        case 202: v = c.addCarry(A, 0u, c.ca); break;
        case 40: v = B - A; break;
        case 8: v = c.addCarry(~A, B, 1); break;
        case 136: v = c.addCarry(~A, B, c.ca); break;
        case 232: v = c.addCarry(~A, 0xFFFFFFFFu, c.ca); break;
        case 200: v = c.addCarry(~A, 0u, c.ca); break;
        case 104: v = 0u - A; break;
        case 235: v = A * B; break;
        case 75: v = Ctx::mulhw(A, B); break;
        case 11: v = Ctx::mulhwu(A, B); break;
        case 491: v = Ctx::divw(A, B); break;
        case 459: v = Ctx::divwu(A, B); break;
        default: hit = false; break;
        }
        if (hit) {
            if (oe) unsupported(c, pc, w);
            D = v;
            if (rc) c.rc0(v);
            return;
        }
    }
    // X-form logical: rS = d, rA = a (destination)
    {
        bool hit = true;
        u32 v = 0;
        switch (xo) {
        case 28: v = S & B; break;
        case 60: v = S & ~B; break;
        case 444: v = S | B; break;
        case 412: v = S | ~B; break;
        case 316: v = S ^ B; break;
        case 476: v = ~(S & B); break;
        case 124: v = ~(S | B); break;
        case 284: v = ~(S ^ B); break;
        case 24: v = Ctx::slw(S, B); break;
        case 536: v = Ctx::srw(S, B); break;
        case 792: v = c.sraw(S, B); break;
        case 824: v = c.sraw(S, b); break;
        case 26: v = Ctx::cntlzw(S); break;
        case 922: v = u32(s32(s16(u16(S)))); break;
        case 954: v = u32(s32(s8(u8(S)))); break;
        default: hit = false; break;
        }
        if (hit) {
            c.r[a] = v;
            if (rc) c.rc0(v);
            return;
        }
    }
    const u32 ea = MVI((a ? A : 0) + B);
    const u32 eu = MVI(A + B);  // update forms
    switch (xo) {
    case 0: c.cmps(int(d >> 2), s32(A), s32(B)); return;
    case 32: c.cmpu(int(d >> 2), A, B); return;
    case 4: trapIf(c, pc, d, A, B); return;
    case 23: D = rd32(ea); return;
    case 55: c.r[a] = eu; D = rd32(eu); return;
    case 87: D = rd8(ea); return;
    case 119: c.r[a] = eu; D = rd8(eu); return;
    case 279: D = rd16(ea); return;
    case 311: c.r[a] = eu; D = rd16(eu); return;
    case 343: D = u32(s32(s16(u16(rd16(ea))))); return;
    case 375: c.r[a] = eu; D = u32(s32(s16(u16(rd16(eu))))); return;
    case 151: wr32(ea, S); return;
    case 183: wr32(eu, S); c.r[a] = eu; return;
    case 215: wr8(ea, S); return;
    case 247: wr8(eu, S); c.r[a] = eu; return;
    case 407: wr16(ea, S); return;
    case 439: wr16(eu, S); c.r[a] = eu; return;
    case 534: D = bswap32(rd32(ea)); return;
    case 662: wr32(ea, bswap32(S)); return;
    case 790: D = bswap16(u16(rd16(ea))); return;
    case 918: wr16(ea, bswap16(u16(S))); return;
    case 20: D = rd32(ea); return;
    case 150: wr32(ea, S); c.cr[0] = u8(2 | c.so); return;
    case 535: c.f[d] = c.ps1[d] = rdf32(ea); return;
    case 567: c.r[a] = eu; c.f[d] = c.ps1[d] = rdf32(eu); return;
    case 599: c.f[d] = rdf64(ea); return;
    case 631: c.r[a] = eu; c.f[d] = rdf64(eu); return;
    case 663: wrf32(ea, c.f[d]); return;
    case 695: wrf32(eu, c.f[d]); c.r[a] = eu; return;
    case 727: wrf64(ea, c.f[d]); return;
    case 759: wrf64(eu, c.f[d]); c.r[a] = eu; return;
    case 983: wr32(ea, u32(bits64(c.f[d]))); return;
    case 19: D = c.getCr(); return;
    case 144: c.mtcrf(bits(w, 12, 19), S); return;
    case 83: D = c.msr; return;
    case 146: c.mtmsr(S); return;
    case 339: case 467: case 371: {
        const u32 n = a | (b << 5);
        if (xo == 371) {
            D = n == 269 ? c.timebaseHi() : c.timebaseLo();
            return;
        }
        const bool to = xo == 467;
        u32* reg = n == 8 ? &c.lr : n == 9 ? &c.ctr : (n >= 912 && n <= 919) ? &c.gqr[n - 912] : nullptr;
        if (n == 1) {
            if (to) c.setXer(S);
            else D = c.getXer();
        } else if (reg) {
            if (to) *reg = S;
            else D = *reg;
        } else if (to) {
            c.mtspr(n, S);
        } else {
            D = c.mfspr(n);
        }
        return;
    }
    case 512:
        c.cr[d >> 2] = u8(c.so << 3 | c.ov << 2 | c.ca << 1);
        c.so = c.ov = c.ca = 0;
        return;
    case 86: case 54: case 278: case 246: case 470: case 982: case 598: case 854: case 306: case 566: return;
    case 1014: c.dcbz(ea); return;
    case 597: c.lswi(int(d), a ? A : 0, b ? b : 32); return;
    case 725: c.stswi(int(d), a ? A : 0, b ? b : 32); return;
    default: unsupported(c, pc, w);
    }
}

void op59(Ctx& c, u32 pc, u32 w) {
    const u32 d = bits(w, 6, 10), a = bits(w, 11, 15), b = bits(w, 16, 20), cc = bits(w, 21, 25);
    const double A = c.f[a], B = c.f[b], C = c.f[cc];
    double e;
    switch (bits(w, 26, 30)) {
    case 21: e = A + B; break;
    case 20: e = A - B; break;
    case 25: e = A * C; break;
    case 18: e = A / B; break;
    case 29: e = __builtin_fma(A, C, B); break;
    case 28: e = __builtin_fma(A, C, -B); break;
    case 31: e = -__builtin_fma(A, C, B); break;
    case 30: e = -__builtin_fma(A, C, -B); break;
    case 24: e = Ctx::fres(B); break;
    case 22: e = __builtin_sqrt(B); break;
    default: unsupported(c, pc, w);
    }
    const double t = Ctx::rs(e);
    c.f[d] = t;
    c.ps1[d] = t;
    if (w & 1) c.rc1();
}

void op63(Ctx& c, u32 pc, u32 w) {
    const u32 d = bits(w, 6, 10), a = bits(w, 11, 15), b = bits(w, 16, 20), cc = bits(w, 21, 25);
    const u32 xo5 = bits(w, 26, 30), xo = bits(w, 21, 30);
    const double A = c.f[a], B = c.f[b], C = c.f[cc];
    bool hit = true;
    double e = 0;
    switch (xo5) {
    case 18: e = A / B; break;
    case 20: e = A - B; break;
    case 21: e = A + B; break;
    case 22: e = __builtin_sqrt(B); break;
    case 23: e = Ctx::fsel(A, C, B); break;
    case 25: e = A * C; break;
    case 26: e = Ctx::frsqrte(B); break;
    case 28: e = __builtin_fma(A, C, -B); break;
    case 29: e = __builtin_fma(A, C, B); break;
    case 30: e = -__builtin_fma(A, C, -B); break;
    case 31: e = -__builtin_fma(A, C, B); break;
    default: hit = false; break;
    }
    if (hit) {
        c.f[d] = e;
        if (w & 1) c.rc1();
        return;
    }
    switch (xo) {
    case 0: case 32: c.fcmp(int(d >> 2), A, B); return;
    case 12: { const double t = Ctx::rs(B); c.f[d] = t; c.ps1[d] = t; break; }
    case 14: c.f[d] = dbl(0xFFF8000000000000ull | Ctx::fctiw(B, false)); break;
    case 15: c.f[d] = dbl(0xFFF8000000000000ull | Ctx::fctiw(B, true)); break;
    case 40: c.f[d] = dbl(bits64(B) ^ 0x8000000000000000ull); break;
    case 72: c.f[d] = B; break;
    case 136: c.f[d] = dbl(bits64(B) | 0x8000000000000000ull); break;
    case 264: c.f[d] = dbl(bits64(B) & 0x7FFFFFFFFFFFFFFFull); break;
    case 38: c.fpscr |= 0x80000000u >> d; break;
    case 70: c.fpscr &= ~(0x80000000u >> d); break;
    case 134: {
        const u32 sh = 28 - 4 * (d >> 2);
        c.fpscr = (c.fpscr & ~(0xFu << sh)) | (bits(w, 16, 19) << sh);
        break;
    }
    case 583: c.f[d] = dbl(0xFFF8000000000000ull | c.fpscr); break;
    case 711: {
        const u32 fm = bits(w, 7, 14);
        u32 m = 0;
        for (int i = 0; i < 8; i++)
            if (fm & (0x80 >> i)) m |= 0xFu << (28 - 4 * i);
        c.fpscr = (c.fpscr & ~m) | (u32(bits64(B)) & m);
        break;
    }
    case 64: c.cr[d >> 2] = u8((c.fpscr >> (28 - 4 * (a >> 2))) & 0xF); return;
    default: unsupported(c, pc, w);
    }
    if (w & 1) c.rc1();
}

void loadStore(Ctx& c, u32 op, u32 w) {
    const u32 d = bits(w, 6, 10), a = bits(w, 11, 15);
    const u32 simm = u32(sext16(w & 0xFFFF));
    if (op == 46 || op == 47) {  // lmw / stmw
        const u32 ea = MVI((a ? c.r[a] : 0) + simm);
        for (u32 i = d; i < 32; i++) {
            if (op == 46) c.r[i] = rd32(ea + (i - d) * 4);
            else wr32(ea + (i - d) * 4, c.r[i]);
        }
        return;
    }
    const bool upd = op & 1;
    const u32 ea = MVI(upd ? c.r[a] + simm : (a ? c.r[a] : 0) + simm);
    const bool isLoad = op <= 35 || (op >= 40 && op <= 43) || (op >= 48 && op <= 51);
    if (upd && isLoad) c.r[a] = ea;  // the loaded register wins (invalid rA == rD forms)
    switch (op) {
    case 32: case 33: c.r[d] = rd32(ea); break;
    case 34: case 35: c.r[d] = rd8(ea); break;
    case 36: case 37: wr32(ea, c.r[d]); break;
    case 38: case 39: wr8(ea, c.r[d]); break;
    case 40: case 41: c.r[d] = rd16(ea); break;
    case 42: case 43: c.r[d] = u32(s32(s16(u16(rd16(ea))))); break;
    case 44: case 45: wr16(ea, c.r[d]); break;
    case 48: case 49: c.f[d] = c.ps1[d] = rdf32(ea); break;
    case 50: case 51: c.f[d] = rdf64(ea); break;
    case 52: case 53: wrf32(ea, c.f[d]); break;
    case 54: case 55: wrf64(ea, c.f[d]); break;
    }
    if (upd && !isLoad) c.r[a] = ea;
}

}  // namespace

// ---- registration ----

void interpPatch(u32 addr, u32 word) {
    std::lock_guard<std::mutex> g(s_lock);
    s_patches[addr] = word;
}
void interpUnpatch(u32 addr) {
    std::lock_guard<std::mutex> g(s_lock);
    if (!s_patches.erase(addr)) return;
    // A function with nothing patched left runs its translation again (a mod taken back must not
    // leave the game interpreted).
    forEachSlotContaining(addr, [](u32 slot) {
        if (!stillInterpreted(slot)) g_fnOverride[slot] = 0;
    });
}
u32 interpHookWord(InterpHook fn) {
    std::lock_guard<std::mutex> g(s_lock);
    s_hooks.push_back(std::move(fn));
    return 0x04000000u | u32(s_hooks.size() - 1);
}
u32 interpStub(const std::vector<u32>& words) {
    std::lock_guard<std::mutex> g(s_lock);
    const u32 n = u32(words.size()) * 4;
    if (s_stubTop + n > s_stubEnd) {
        const u32 chunk = std::max<u32>(0x1000, n);
        s_stubTop = hleAlloc(chunk, 32);
        s_stubEnd = s_stubTop + chunk;
        s_stubChunks.push_back({s_stubTop, s_stubEnd});
    }
    const u32 a = s_stubTop;
    for (size_t i = 0; i < words.size(); i++) wr32(a + 4 * u32(i), words[i]);
    s_stubTop += n;
    return a;
}
bool isStub(u32 a) {
    for (const auto& [b, e] : s_stubChunks)
        if (a >= b && a < e) return true;
    return false;
}

// Every function (main.dol, linked RELs) whose code contains `addr`; their overrides are set.
int interpOverrideAt(u32 addr) {
    int n = 0;
    forEachSlotContaining(addr, [&](u32 slot) { g_fnOverride[slot] = 1, n++; });
    return n;
}

void interpInit() {
    const char* s = std::getenv("SA2_INTERP");
    if (!s) return;
    if (!std::strcmp(s, "all")) {
        s_interpAll = true;
        std::memset(g_fnOverride, 1, kFnSlotCount);
        log("[interp] every function runs in the interpreter\n");
        return;
    }
    for (const char* p = s; *p;) {  // <addr> (the functions containing it) or <from>-<to> (entry points)
        char* e;
        const u32 a = u32(std::strtoul(p, &e, 16));
        if (e == p) break;
        if (*e == '-') {
            const u32 to = u32(std::strtoul(e + 1, &e, 16));
            int n = 0;
            for (u32 slot = 0; slot < kFnSlotCount; slot++) {
                Fn f;
                if (slotFunction(slot, f) && f.entry >= a && f.entry < to) g_fnOverride[slot] = 1, s_forced.insert(slot), n++;
            }
            log("[interp] %08X-%08X: %d function(s) run in the interpreter\n", a, to, n);
        } else {
            int n = 0;
            forEachSlotContaining(a, [&](u32 slot) { g_fnOverride[slot] = 1, s_forced.insert(slot), n++; });
            log("[interp] %08X: %d function(s) run in the interpreter\n", a, n);
        }
        p = *e ? e + 1 : e;
    }
}

// ---- execution ----

namespace {

struct Bounds {
    u32 body, end;
    bool local(u32 t) const { return (t >= body && t < end) || isStub(t); }
};
thread_local const Bounds* t_fn = nullptr;  // the function being interpreted (for hooks)

// One instruction `w` at `pc` of the function `f`: returns the next address, or kInterpReturn
// when the function returns (or tail-calls out of itself).
u32 step(Ctx& c, u32 pc, u32 w, const Bounds& f) {
    t_pc = pc;
    const u32 op = w >> 26;
    const u32 d = bits(w, 6, 10), a = bits(w, 11, 15), b = bits(w, 16, 20);
    const u32 simm = u32(sext16(w & 0xFFFF)), uimm = w & 0xFFFF;
    switch (op) {
    case 1: {  // host hook
        const u32 r = s_hooks.at(w & 0x03FFFFFF)(c, pc);
        return r == kInterpOriginal ? step(c, pc, rd32(pc), f) : r;
    }
    case 18: {  // b / bl
        const s32 li = s32(w << 6) >> 6 & ~3;
        const u32 t = (w & 2) ? u32(li) : pc + u32(li);
        if (w & 1) {
            c.lr = pc + 4;
            if (t != pc + 4) c.call(t);
            return pc + 4;
        }
        if (f.local(t)) {
            if (t <= pc) c.poll();
            return t;
        }
        c.call(t);
        return kInterpReturn;
    }
    case 16: {  // bc
        const u32 t = (w & 2) ? simm & ~3u : pc + (simm & ~3u);
        if (!cond(c, d, a)) return pc + 4;
        if (w & 1) {
            c.lr = pc + 4;
            c.call(t);
            return pc + 4;
        }
        if (f.local(t)) {
            if (t <= pc) c.poll();
            return t;
        }
        c.call(t);
        return kInterpReturn;
    }
    case 19: {
        const u32 xo = bits(w, 21, 30);
        if (xo == 16) {  // bclr
            const bool ok = cond(c, d, a);
            if (w & 1) {
                const u32 t = c.lr;
                c.lr = pc + 4;
                if (ok) c.call(t);
                return pc + 4;
            }
            return ok ? kInterpReturn : pc + 4;
        }
        if (xo == 528) {  // bcctr
            const bool ok = cond(c, d | 4, a);
            if (w & 1) {
                c.lr = pc + 4;
                if (ok) c.call(c.ctr);
                return pc + 4;
            }
            if (!ok) return pc + 4;
            if (f.local(c.ctr)) return c.ctr;  // a jump table
            c.call(c.ctr);
            return kInterpReturn;
        }
        const bool A = c.crbit(int(a)), B = c.crbit(int(b));
        switch (xo) {
        case 257: c.setCrbit(int(d), A && B); break;
        case 449: c.setCrbit(int(d), A || B); break;
        case 193: c.setCrbit(int(d), A != B); break;
        case 225: c.setCrbit(int(d), !(A && B)); break;
        case 33: c.setCrbit(int(d), !(A || B)); break;
        case 289: c.setCrbit(int(d), A == B); break;
        case 129: c.setCrbit(int(d), A && !B); break;
        case 417: c.setCrbit(int(d), A || !B); break;
        case 0: c.cr[d >> 2] = c.cr[a >> 2]; break;
        case 150: break;  // isync
        default: unsupported(c, pc, w);
        }
        return pc + 4;
    }
    case 3: trapIf(c, pc, d, c.r[a], simm); break;
    case 7: c.r[d] = c.r[a] * simm; break;
    case 8: c.r[d] = c.addCarry(~c.r[a], simm, 1); break;
    case 10: c.cmpu(int(d >> 2), c.r[a], uimm); break;
    case 11: c.cmps(int(d >> 2), s32(c.r[a]), s32(simm)); break;
    case 12: c.r[d] = c.addCarry(c.r[a], simm, 0); break;
    case 13: c.r[d] = c.addCarry(c.r[a], simm, 0); c.rc0(c.r[d]); break;
    case 14: c.r[d] = MVI((a ? c.r[a] : 0) + simm); break;
    case 15: c.r[d] = MVI((a ? c.r[a] : 0) + (uimm << 16)); break;
    case 17: c.syscall(); break;
    case 20: case 21: case 23: {
        const u32 mask = maskOf(bits(w, 21, 25), bits(w, 26, 30));
        const u32 rot = Ctx::rotl(c.r[d], op == 23 ? c.r[b] : b);
        c.r[a] = op == 20 ? (rot & mask) | (c.r[a] & ~mask) : rot & mask;
        if (w & 1) c.rc0(c.r[a]);
        break;
    }
    case 24: c.r[a] = MVI(c.r[d] | uimm); break;
    case 25: c.r[a] = c.r[d] | (uimm << 16); break;
    case 26: c.r[a] = c.r[d] ^ uimm; break;
    case 27: c.r[a] = c.r[d] ^ (uimm << 16); break;
    case 28: c.r[a] = c.r[d] & uimm; c.rc0(c.r[a]); break;
    case 29: c.r[a] = c.r[d] & (uimm << 16); c.rc0(c.r[a]); break;
    case 31: op31(c, pc, w); break;
    case 59: op59(c, pc, w); break;
    case 63: op63(c, pc, w); break;
    case 4: op4(c, pc, w); break;
    case 56: case 57: case 60: case 61: {
        const bool wbit = bits(w, 16, 16), upd = op == 57 || op == 61;
        const int i = int(bits(w, 17, 19));
        const u32 off = u32(s32(w << 20) >> 20);
        const u32 ea = MVI((a ? c.r[a] : 0) + off);
        if (op < 60) c.psqLoad(int(d), ea, wbit, i);
        else c.psqStore(int(d), ea, wbit, i);
        if (upd) c.r[a] = ea;
        break;
    }
    default:
        if (op >= 32 && op <= 55) loadStore(c, op, w);
        else unsupported(c, pc, w);
        break;
    }
    return pc + 4;
}

}  // namespace

void interpRun(Ctx& c, u32 entry, u32 body, u32 end) {
    const Bounds f{body, end};
    const Bounds* outer = t_fn;
    t_fn = &f;
    u32 pc = entry;
    for (;;) {
        if (!f.local(pc)) {  // ran off the end: into the next function
            c.call(pc);
            break;
        }
        const u32 next = step(c, pc, fetch(pc), f);
        if (next == kInterpReturn) break;
        pc = next;
    }
    t_fn = outer;
}

u32 interpStep(Ctx& c, u32 pc, u32 word) {
    if (!t_fn) fatal("interpStep at %08X outside an interpreted function", pc);
    return step(c, pc, word, *t_fn);
}

void runOverride(Ctx& c, u32 slot) {
    Fn f;
    if (!slotFunction(slot, f)) fatal("override slot %u: no such function loaded", slot);
    if (!s_handlers.empty()) {
        const auto it = s_handlers.find(slot);
        if (it != s_handlers.end()) return it->second(c, f.entry);
    }
    interpRun(c, f.entry, f.body, f.end);
}

void (*g_relLinkHook)(u32 module, bool linked) = nullptr;

namespace {
u32 slotOf(u32 module, u32 addr) {
    if (!module) {
        for (u32 i = 0; i < kDolFuncCount; i++)
            if (kDolFuncs[i].addr == addr) return i;
        return 0xFFFFFFFF;
    }
    for (u32 i = 0; i < kRelModuleCount; i++) {
        const RelModuleCode& m = kRelModules[i];
        if (m.id != module) continue;
        for (u32 k = 0; k < m.count; k++)
            if (m.funcs[k].addr == addr) return m.slotBase + k;
    }
    return 0xFFFFFFFF;
}
}  // namespace

void interpUnreplace(u32 module, u32 addr) {
    const u32 slot = slotOf(module, addr);
    if (slot == 0xFFFFFFFF) return;
    std::lock_guard<std::mutex> g(s_lock);
    s_handlers.erase(slot);
    if (!stillInterpreted(slot)) g_fnOverride[slot] = 0;
}

bool interpReplace(u32 module, u32 addr, InterpHandler fn) {
    u32 slot = 0xFFFFFFFF;
    if (!module) {
        for (u32 i = 0; i < kDolFuncCount; i++)
            if (kDolFuncs[i].addr == addr) slot = i;
    } else {
        for (u32 i = 0; i < kRelModuleCount; i++) {
            const RelModuleCode& m = kRelModules[i];
            if (m.id != module) continue;
            for (u32 k = 0; k < m.count; k++)
                if (m.funcs[k].addr == addr) slot = m.slotBase + k;
        }
    }
    if (slot == 0xFFFFFFFF) return false;
    std::lock_guard<std::mutex> g(s_lock);
    s_handlers[slot] = std::move(fn);
    g_fnOverride[slot] = 1;
    return true;
}

void interpOriginal(Ctx& c, u32 entry) {
    // Called on every call of a function a mod watches: the slot is looked up once per entry
    // address (and again only if a module has moved since).
    static std::unordered_map<u32, u32> s_slotOf;
    Fn f;
    if (const auto it = s_slotOf.find(entry); it != s_slotOf.end() && slotFunction(it->second, f) && f.entry == entry)
        return interpRun(c, f.entry, f.body, f.end);
    for (u32 slot = 0; slot < kFnSlotCount; slot++)
        if (slotFunction(slot, f) && f.entry == entry) {
            s_slotOf[entry] = slot;
            return interpRun(c, f.entry, f.body, f.end);
        }
    fatal("interpOriginal: no function at %08X", entry);
}

}  // namespace rt

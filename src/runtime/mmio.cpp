// Hardware registers (0xCCxxxxxx) that recompiled SDK code still touches. The GX write-gather
// pipe feeds the GX bridge, the DSP block is the native DSP stand-in; other registers act as plain
// storage, and the first access of each is logged so remaining hardware use is visible.
#include "runtime/gx.h"
#include "runtime/runtime.h"

#include <atomic>

namespace rt {

u32 dspRead16(u32 off);
void dspWrite16(u32 off, u32 v);
u32 exiRead32(u32 off);  // src/runtime/exi.cpp
void exiWrite32(u32 off, u32 v);

namespace {
u8 s_regs[0x10000];  // 0xCC000000-0xCC00FFFF
std::atomic<u8> s_seen[0x10000 / 4];

void noteFirst(u32 a, bool write, u32 v) {
    const u8 bit = write ? 2 : 1;
    std::atomic<u8>& seen = s_seen[(a & 0xFFFF) / 4];
    if (seen.load(std::memory_order_relaxed) & bit) return;  // (a plain read: polled registers stay cheap)
    if (seen.fetch_or(bit, std::memory_order_relaxed) & bit) return;
    if (write) log("[mmio] first write %08X = %08X\n", a, v);
    else log("[mmio] first read  %08X\n", a);
}
}  // namespace

u32 mmioGet16(u32 off) { return u32(s_regs[off & 0xFFFF]) << 8 | s_regs[(off + 1) & 0xFFFF]; }
u32 mmioGet32(u32 off) { return mmioGet16(off) << 16 | mmioGet16(off + 2); }
void mmioSet16(u32 off, u32 v) {
    s_regs[off & 0xFFFF] = u8(v >> 8);
    s_regs[(off + 1) & 0xFFFF] = u8(v);
}

u32 mmio_rd(u32 a, int size) {
    const u32 o = a & 0xFFFF;
    if (o >= 0x6800 && o < 0x683C) {  // EXI bus (src/runtime/exi.cpp)
        const u32 v = exiRead32(o & ~3u);
        return size == 4 ? v : size == 2 ? (v >> (16 - 8 * (o & 2))) & 0xFFFF : (v >> (24 - 8 * (o & 3))) & 0xFF;
    }
    if (o >= 0x5000 && o < 0x500C) {  // DSP mailbox/control (src/runtime/dsp.cpp)
        if (size == 4) {
            const u32 hi = dspRead16(o);
            return hi << 16 | dspRead16(o + 2);
        }
        return dspRead16(o & ~1u);
    }
    noteFirst(a & ~3u, false, 0);
    // PI interrupt cause (0xCC003000) bit 16 is the console's reset switch, 1 while it is up. Read as
    // stored it was 0 - "held down" - and the game's soft-reset check (f_80022BC4, via
    // OSGetResetButtonState) saw it let go now and then and returned to the title screen.
    if ((o & ~3u) == 0x3000) {
        const u32 v = (u32(s_regs[0x3000]) << 24 | u32(s_regs[0x3001]) << 16 | u32(s_regs[0x3002]) << 8 | s_regs[0x3003]) | 0x10000u;
        return size == 4 ? v : size == 2 ? (v >> (16 - 8 * (o & 2))) & 0xFFFF : (v >> (24 - 8 * (o & 3))) & 0xFF;
    }
    switch (size) {
    case 1: return s_regs[o];
    case 2: return u32(s_regs[o]) << 8 | s_regs[(o + 1) & 0xFFFF];
    default: return u32(s_regs[o]) << 24 | u32(s_regs[o + 1]) << 16 | u32(s_regs[o + 2]) << 8 | s_regs[o + 3];
    }
}

void mmio_wr(u32 a, u32 v, int size) {
    const u32 o = a & 0xFFFF;
    if ((o & 0xFFF8) == 0x8000) {  // GX write-gather pipe
        gxWrite(v, size);
        return;
    }
    if (o >= 0x6800 && o < 0x683C) {
        if (size == 4) exiWrite32(o, v);
        else log("[exi] unexpected %d-byte write %08X = %X\n", size, a, v);
        return;
    }
    if (o >= 0x5000 && o < 0x500C) {
        if (size == 4) {
            dspWrite16(o, v >> 16);
            dspWrite16(o + 2, v & 0xFFFF);
        } else {
            dspWrite16(o & ~1u, v);
        }
        return;
    }
    noteFirst(a & ~3u, true, v);
    switch (size) {
    case 1: s_regs[o] = u8(v); break;
    case 2:
        s_regs[o] = u8(v >> 8);
        s_regs[(o + 1) & 0xFFFF] = u8(v);
        break;
    default:
        s_regs[o] = u8(v >> 24);
        s_regs[o + 1] = u8(v >> 16);
        s_regs[o + 2] = u8(v >> 8);
        s_regs[o + 3] = u8(v);
        break;
    }
    if (o == 0x0020 || o == 0x0022) gxNoteCpRegister(o, v & 0xFFFF);
}

}  // namespace rt

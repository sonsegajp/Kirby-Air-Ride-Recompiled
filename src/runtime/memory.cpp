// Guest memory, main.dol loading and the low-memory boot block.
#include "runtime/runtime.h"

#include <cstdlib>
#include <cstring>

namespace rt {

u8* g_mem = nullptr;
u32 g_relBase[256][32];

bool memInit() {
    g_mem = static_cast<u8*>(std::calloc(kHostMemSize, 1));
    return g_mem != nullptr;
}

bool loadDol(const std::vector<u8>& d, u32& entry) {
    if (d.size() < 0x100) return false;
    auto be = [&](size_t o) { return u32(d[o]) << 24 | u32(d[o + 1]) << 16 | u32(d[o + 2]) << 8 | d[o + 3]; };
    // BSS first: the DOL's bss range also covers the small-data sections, which are loaded on top.
    const u32 bssAddr = be(0xD8), bssLen = be(0xDC);
    std::memset(hostp(bssAddr), 0, bssLen);
    for (int i = 0; i < 18; i++) {
        const u32 off = be(i * 4), addr = be(0x48 + i * 4), size = be(0x90 + i * 4);
        if (!size) continue;
        if (off + size > d.size() || phys(addr) + size > kMem1Size) return false;
        std::memcpy(hostp(addr), &d[off], size);
    }
    entry = be(0xE0);
    return true;
}

static u32 s_hleNext = kHleRegion + 0x4000;  // past the locked cache, which shares the host bytes at its start

u32 hleAlloc(u32 size, u32 align) {
    s_hleNext = (s_hleNext + align - 1) & ~(align - 1);
    const u32 a = s_hleNext;
    s_hleNext += size;
    if (s_hleNext > kHleRegionEnd) fatal("hleAlloc: out of HLE memory (%u bytes)", size);
    std::memset(hostp(a), 0, size);
    return a;
}

}  // namespace rt

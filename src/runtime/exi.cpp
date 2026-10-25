// EXI bus (0xCC006800, three channels) and the memory card in slot A, at the hardware level: the
// SDK's EXI and CARD libraries run recompiled and drive these registers, so the game's own card
// code mounts, formats, reads and writes a raw card image (saves/MemoryCardA.raw, the same layout
// as a real card and Dolphin's .raw files; tools/mkcard.py builds one from .gci saves).
//   per channel (0x14 apart): CSR +0, MAR +4 (DMA address), LENGTH +8, CR +0xC, DATA +0x10
//   CSR: 0 EXI int mask, 1 EXI int, 2 TC int mask, 3 TC int, 4-6 clock, 7-9 device select,
//        10 EXT int mask, 11 EXT int, 12 device attached (read only); interrupt bits clear on write 1
//   CR:  0 start, 1 DMA, 2-3 direction (0 read, 1 write, 2 both), 4-5 immediate length - 1
// The card answers the flash command set (Dolphin's EXI_DeviceMemoryCard documents it): 0x00 ID,
// 0x83 status, 0x89 clear status, 0x81 interrupt enable, 0x52 read, 0xF1 erase sector, 0xF2 program
// page, 0xF4 erase chip. Erase and program finish when the card is deselected, raising its EXI
// interrupt. Transfers complete at once (transfer-complete interrupt when enabled).
#include "runtime/runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rt {

namespace {
// OS interrupts: EXI channel n: EXI 9+3n, TC 10+3n, EXT 11+3n (channel 2 has no EXT).
constexpr u32 kIrqExi = 9, kIrqTc = 10;
enum : u32 { CSR_EXIMASK = 1, CSR_EXIINT = 2, CSR_TCMASK = 4, CSR_TCINT = 8, CSR_EXTINT = 0x800, CSR_EXT = 0x1000 };

struct Device {
    virtual ~Device() = default;
    virtual void select() {}
    virtual void deselect() {}
    virtual void write(u8) {}
    virtual u8 read() { return 0xFF; }
    virtual void dmaRead(u32 addr, u32 len) { std::memset(hostp(addr), 0, len); }
    virtual void dmaWrite(u32, u32) {}
    virtual bool interruptPending() { return false; }
};

// ---- memory card ----
struct MemoryCard : Device {
    std::FILE* file = nullptr;
    std::vector<u8> image;
    u32 sizeMbit;
    u8 cmd = 0, status = 0x41;  // ready | unlocked
    u32 pos = 0, address = 0;
    bool intEnabled = false, exiPending = false;
    u8 page[128];
    u32 pageBytes = 0;

    MemoryCard(const std::string& path, u32 mbit) : sizeMbit(mbit) {
        image.assign(size_t(mbit) * 1024 * 1024 / 8, 0xFF);
        file = std::fopen(path.c_str(), "r+b");
        if (file) {
            std::fread(image.data(), 1, image.size(), file);
        } else {
            std::error_code ec;  // (its folder too: a first run has no saves/ yet)
            if (const auto dir = std::filesystem::path(path).parent_path(); !dir.empty()) std::filesystem::create_directories(dir, ec);
            file = std::fopen(path.c_str(), "w+b");  // a new, unformatted card: the game offers to format it
            if (file) std::fwrite(image.data(), 1, image.size(), file);
        }
        if (file) std::fflush(file);
        log("[card] slot A: %s (%u blocks)%s\n", path.c_str(), u32(image.size() / 0x2000 - 5), file ? "" : " - CANNOT OPEN");
    }
    void persist(u32 off, u32 len) {
        if (!file) return;
        std::fseek(file, long(off), SEEK_SET);
        std::fwrite(&image[off], 1, len, file);
        std::fflush(file);
    }
    u32 mask(u32 a) const { return a % u32(image.size()); }

    void select() override { pos = 0, pageBytes = 0; }
    void deselect() override {
        if (cmd == 0xF1 && pos >= 3) {  // sector erase
            const u32 base = mask(address & ~0x1FFFu);
            std::memset(&image[base], 0xFF, 0x2000);
            persist(base, 0x2000);
            finish();
        } else if (cmd == 0xF2 && pos >= 5) {  // page program
            const u32 base = mask(address & ~0x7Fu);
            for (u32 i = 0; i < pageBytes && i < 128; i++) image[base + ((address + i) & 0x7F)] = page[i];
            persist(base, 128);
            finish();
        } else if (cmd == 0xF4 && pos >= 1) {
            std::memset(image.data(), 0xFF, image.size());
            persist(0, u32(image.size()));
            finish();
        }
        cmd = 0;
    }
    void finish() {
        if (intEnabled) exiPending = true;
    }
    bool interruptPending() override {
        const bool p = exiPending;
        exiPending = false;
        return p;
    }
    void write(u8 b) override {
        if (pos == 0) {
            cmd = b;
            if (cmd == 0x89) status &= ~0x18;  // clear status: error bits
        } else {
            switch (cmd) {
            case 0x81:
                if (pos == 1) intEnabled = b & 1;
                break;
            case 0x52:
            case 0xF2:
                if (pos == 1) address = u32(b & 0x3F) << 17;
                else if (pos == 2) address |= u32(b) << 9;
                else if (pos == 3) address |= u32(b & 3) << 7;
                else if (pos == 4) address |= b & 0x7F;
                else if (cmd == 0xF2 && pageBytes < 128) page[pageBytes++] = b;
                break;
            case 0xF1:
                if (pos == 1) address = u32(b & 0x7F) << 17;
                else if (pos == 2) address |= u32(b) << 9;
                break;
            default: break;
            }
        }
        pos++;
    }
    u8 read() override {
        const u32 p = pos++;
        switch (cmd) {
        case 0x00: {  // ID after the two command bytes: the card size in Mbit (sector size 8 KiB)
            const u32 id = sizeMbit;
            return p >= 2 && p < 6 ? u8(id >> (8 * (5 - p))) : 0;
        }
        case 0x83: return status;
        case 0x52: return image[mask(address++)];
        default: return 0xFF;
        }
    }
    void dmaRead(u32 addr, u32 len) override {
        for (u32 i = 0; i < len; i++) wr8(addr + i, cmd == 0x52 ? image[mask(address++)] : 0xFF);
    }
    void dmaWrite(u32 addr, u32 len) override {
        if (cmd != 0xF2) return;
        for (u32 i = 0; i < len && pageBytes < 128; i++) page[pageBytes++] = u8(rd8(addr + i));
    }
};

// ---- the IPL/RTC chip (channel 0, device 1): only the clock is answered here (SRAM is replaced in
// src/runtime/hle_os.cpp) ----
struct Rtc : Device {
    u32 cmdWord = 0, pos = 0;
    void select() override { pos = 0, cmdWord = 0; }
    void write(u8 b) override {
        if (pos < 4) cmdWord = cmdWord << 8 | b;
        pos++;
    }
    u8 read() override {
        const u32 p = pos++ - 4;
        if ((cmdWord & 0x7FFFFF00) == 0x20000000 && p < 4) {  // RTC: seconds since 2000-01-01
            const u32 secs = u32(std::time(nullptr) - 946684800);
            return u8(secs >> (8 * (3 - p)));
        }
        return 0;
    }
};

struct Channel {
    u32 csr = 0, mar = 0, len = 0, cr = 0, data = 0;
    Device* dev[3] = {};
    int selected = -1;
};
std::recursive_mutex s_lock;
Channel s_ch[3];
std::unique_ptr<MemoryCard> s_cardA;
Rtc s_rtc;
bool s_init = false;

void init() {
    if (s_init) return;
    s_init = true;
    if (!std::getenv("SA2_NO_CARD")) {
        const char* path = std::getenv("SA2_CARD");
        s_cardA = std::make_unique<MemoryCard>(path ? path : "saves/MemoryCardA.raw", 16);
        s_ch[0].dev[0] = s_cardA.get();
    }
    s_ch[0].dev[1] = &s_rtc;
}

void raise(u32 chan, u32 base) {
    raiseInterrupt(base + chan * 3);
}

void transfer(u32 chan) {
    Channel& ch = s_ch[chan];
    Device* d = ch.selected >= 0 ? ch.dev[ch.selected] : nullptr;
    const u32 rw = (ch.cr >> 2) & 3;
    if (ch.cr & 2) {  // DMA
        const u32 addr = 0x80000000u | (ch.mar & 0x03FFFFE0), len = ch.len & 0x03FFFFE0;
        if (d) {
            if (rw == 0) d->dmaRead(addr, len);
            else d->dmaWrite(addr, len);
        } else if (rw == 0) {
            std::memset(hostp(addr), 0xFF, len);
        }
    } else {  // immediate: up to 4 bytes through DATA, most significant first
        const u32 n = ((ch.cr >> 4) & 3) + 1;
        u32 in = 0;
        for (u32 i = 0; i < n; i++) {
            const u8 out = u8(ch.data >> (24 - 8 * i));
            u8 got = 0xFF;
            if (d) {
                if (rw != 0) d->write(out);
                if (rw != 1) got = d->read();
            }
            in |= u32(got) << (24 - 8 * i);
        }
        if (rw != 1) ch.data = in;
    }
    ch.cr &= ~1u;
    ch.csr |= CSR_TCINT;
    if (ch.csr & CSR_TCMASK) raise(chan, kIrqTc);
}

void deviceInterrupt(u32 chan) {
    Channel& ch = s_ch[chan];
    for (Device* d : ch.dev)
        if (d && d->interruptPending()) {
            ch.csr |= CSR_EXIINT;
            if (ch.csr & CSR_EXIMASK) raise(chan, kIrqExi);
        }
}
}  // namespace

u32 exiRead32(u32 off) {
    std::lock_guard g(s_lock);
    init();
    const u32 chan = (off - 0x6800) / 0x14, reg = (off - 0x6800) % 0x14;
    if (chan > 2) return 0;
    Channel& ch = s_ch[chan];
    switch (reg) {
    case 0x00: return (ch.csr & ~u32(CSR_EXT)) | (chan < 2 && ch.dev[0] ? CSR_EXT : 0);
    case 0x04: return ch.mar;
    case 0x08: return ch.len;
    case 0x0C: return ch.cr;
    default: return ch.data;
    }
}

void exiWrite32(u32 off, u32 v) {
    std::lock_guard g(s_lock);
    init();
    const u32 chan = (off - 0x6800) / 0x14, reg = (off - 0x6800) % 0x14;
    if (chan > 2) return;
    Channel& ch = s_ch[chan];
    switch (reg) {
    case 0x00: {
        const u32 ints = CSR_EXIINT | CSR_TCINT | CSR_EXTINT;  // status bits: writing 1 clears
        const u32 old = ch.csr;
        ch.csr = (ch.csr & ints & ~(v & ints)) | (v & ~u32(ints | CSR_EXT));
        // The interrupt lines are levels: a device interrupt still pending when its mask is set is
        // delivered then. The card finishes an erase or program at once, while the SDK keeps its
        // channel locked with the EXI interrupt masked; it unmasks it on unlock (EXIUnlock).
        if ((ch.csr & ~old & CSR_EXIMASK) && (ch.csr & CSR_EXIINT)) raise(chan, kIrqExi);
        // Device select (one-hot, bits 7-9): the selected device sees select/deselect edges.
        const u32 cs = (v >> 7) & 7;
        const int sel = cs & 1 ? 0 : cs & 2 ? 1 : cs & 4 ? 2 : -1;
        if (sel != ch.selected) {
            if (ch.selected >= 0 && ch.dev[ch.selected]) ch.dev[ch.selected]->deselect();
            ch.selected = sel;
            if (sel >= 0 && ch.dev[sel]) ch.dev[sel]->select();
            deviceInterrupt(chan);
        }
        break;
    }
    case 0x04: ch.mar = v; break;
    case 0x08: ch.len = v; break;
    case 0x0C:
        ch.cr = v;
        if (v & 1) transfer(chan);
        break;
    default: ch.data = v; break;
    }
}

}  // namespace rt

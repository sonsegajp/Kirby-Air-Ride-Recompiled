// Video retrace, ARAM, audio DMA pacing and controllers. The SDK's own interrupt handlers stay
// native: a device "interrupt" sets the status bit the handler checks and calls the handler the
// game registered in the OS interrupt table (__OSSetInterruptHandler, table at 0x80003040).
#include "runtime/gx.h"
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <memory>
#include <thread>

namespace rt {

u32 mmioGet16(u32 off);
void mmioSet16(u32 off, u32 v);
void hostThreadTimeCritical();  // src/runtime/hosttime.cpp

namespace {
enum : u32 { IRQ_DSP_AI = 5, IRQ_DSP_ARAM = 6, IRQ_PI_VI = 24 };


// ---- VI: retrace at the NTSC field rate ----
std::atomic<bool> s_viRunning{false};
std::atomic<u32> s_xfb{0};
std::atomic<u64> s_frames{0};
std::atomic<double> s_lastRetrace{0};  // gxNow() time of the latest retrace

void viThread() {
    profileRegisterThread();
    hostThreadTimeCritical();
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::nanoseconds(1000000000ll * 1001 / 60000);  // 59.94 Hz
    auto next = clock::now() + period;
    double lateSum = 0, lateMax = 0;  // how late the retraces came, for the log
    for (;;) {
        sleepUntil(next, true);
        const double late = std::chrono::duration<double, std::milli>(clock::now() - next).count();
        lateSum += late, lateMax = std::max(lateMax, late);
        // Late by a whole retrace (the host stalled): carry on from now, not two retraces at once.
        next = late > 16.0 ? clock::now() + period : next + period;
        s_lastRetrace = gxNow();
        raiseInterrupt(IRQ_PI_VI, [] { mmioSet16(0x2030, mmioGet16(0x2030) | 0x8000); });
        if (++s_frames % 600 == 0) {
            log("[vi] %llu retraces, xfb %08X; late %.2f ms on average, %.2f at most\n", (unsigned long long)s_frames.load(),
                s_xfb.load(), lateSum / 600, lateMax);
            lateSum = lateMax = 0;
        }
    }
}

// ---- ARAM: 16 MiB of host memory; DMA completes at once, the interrupt follows ----
std::unique_ptr<u8[]> s_aram;
constexpr u32 kAramSize = 0x1000000;
}  // namespace

u8* aramPtr(u32 a) { return &s_aram[a & (kAramSize - 1)]; }
u64 viRetraces() { return s_frames.load(); }
double viLastRetrace() { return s_lastRetrace.load(); }

HLE(VIInit) {
    callOriginal(c, "VIInit");
    bool expected = false;
    if (s_viRunning.compare_exchange_strong(expected, true)) std::thread(viThread).detach();
}
HLE(VISetNextFrameBuffer) {
    s_xfb = c.r[3];
    callOriginal(c, "VISetNextFrameBuffer");
    // The frame's GX commands go to the renderer - unless the game flips in its retrace callback
    // (HAL's engine: Kirby Air Ride), wherever its drawing has got to: then a frame ends at its
    // display copy instead (src/runtime/gx.cpp), else frames were cut mid-draw (objects flickered).
    if (sched::inInterrupt()) gxFramesByDisplayCopy();
    else if (!gxFramesEndAtCopy()) gxEndFrame();
}

HLE(__ARChecksize) {
    if (!s_aram) s_aram.reset(new u8[kAramSize]());
    // __AR_Size is a small-data variable; where it is comes from ARGetSize (lwz r3, __AR_Size(r13)).
    const u32 get = rd32(guestSymbol("ARGetSize"));
    if ((get >> 16) != 0x806D) fatal("ARGetSize does not start with lwz r3, x(r13): %08X", get);
    wr32(c.r[13] + u32(s32(s16(get & 0xFFFF))), kAramSize);
}
HLE(ARStartDMA) {  // (type, main memory, aram, length): type 0 main->ARAM, 1 ARAM->main
    if (!s_aram) s_aram.reset(new u8[kAramSize]());
    const u32 type = c.r[3], mem = c.r[4], aram = c.r[5], len = c.r[6];
    for (u32 i = 0; i < len; i++) {
        if (type == 0) *aramPtr(aram + i) = *hostp(mem + i);
        else *hostp(mem + i) = *aramPtr(aram + i);
    }
    raiseInterrupt(IRQ_DSP_ARAM);
}

// ---- AI: the DMA "plays" in real time; each finished block raises the AI DMA interrupt, whose
// handler calls the game's AIRegisterDMACallback (the audio frame tick) and sets up the next ----
void audioPushAi(u32 addr, u32 bytes);  // src/runtime/audio.cpp
namespace {
std::atomic<bool> s_aiRunning{false};
void aiThread() {
    profileRegisterThread();
    log("[audio] AI DMA starts at %.3f s (frame %llu)\n", gxNow(), (unsigned long long)gxFrames());
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    while (s_aiRunning) {
        const u32 blocks = mmioGet16(0x5036) & 0x7FFF;
        const u32 bytes = (blocks ? blocks : 1) * 32;
        if (blocks && (mmioGet16(0x5036) & 0x8000)) {  // the block now playing goes to the host
            const u32 src = ((mmioGet16(0x5030) & 0x3FF) << 16 | mmioGet16(0x5032)) & ~31u;
            // The game swaps buffers in its AI interrupt; the same one again means it had not yet.
            static u32 s_last = 0, s_repeats = 0, s_blocks = 0, s_run = 0;
            s_repeats += src == s_last;
            s_run = src == s_last ? s_run + 1 : 0, s_last = src;
            // A game that stopped (hung, or stalled loading) leaves one buffer playing over and over: a
            // loud tone. After 20 ms of that, silence until it hands over a new one.
            audioPushAi(s_run >= 4 ? 0 : 0x80000000u | src, bytes);
            if (++s_blocks % 12000 == 0 && s_repeats) {
                log("[audio] %u of the last 12000 blocks replayed the one before (the game's buffer swap came late)\n", s_repeats);
                s_repeats = 0;
            }
        }
        next += std::chrono::nanoseconds(u64(bytes / 4) * 1000000000ull / 32000);
        sleepUntil(next, false);
        if (mmioGet16(0x5036) & 0x8000) {
            const u32 playing = mmioGet16(0x5030) << 16 | mmioGet16(0x5032);
            raiseInterrupt(IRQ_DSP_AI, [] { mmioSet16(0x500A, mmioGet16(0x500A) | 0x0008); });
            // The handler runs when the game next takes interrupts: give it a moment to hand over
            // the next buffer (host audio keeps a cushion, so this costs nothing audible).
            const auto until = clock::now() + std::chrono::milliseconds(2);
            while ((mmioGet16(0x5030) << 16 | mmioGet16(0x5032)) == playing && clock::now() < until)
                sleepUntil(clock::now() + std::chrono::microseconds(250), false);
        }
    }
}
}  // namespace

HLE(AIInit) {
    // Hook the DMA and streaming interrupts up the way AIInit does; skip its sample-counter
    // calibration (it busy-waits on hardware).
    guestCall(c, guestSymbol("__OSSetInterruptHandler"), {IRQ_DSP_AI, guestSymbol("__AIDHandler")});
    guestCall(c, guestSymbol("__OSSetInterruptHandler"), {8, guestSymbol("__AISHandler")});
}
// The same calibration, run again by AISetDSPSampleRate when a game switches the DSP to 32 kHz
// (Kirby Air Ride's HSD sound setup does): nothing to measure here.
HLE(__AI_SRC_INIT) {}
HLE(AIStartDMA) {
    callOriginal(c, "AIStartDMA");
    bool expected = false;
    if (s_aiRunning.compare_exchange_strong(expected, true)) std::thread(aiThread).detach();
}

// ---- PAD: port 1 is a standard controller (input comes from the host later), 2-4 are empty ----
// SA2_TAS=<file.dtm> (bug hunting): port 1 replays a Dolphin input movie instead - its recorded
// polls taken one per retrace since PADInit (SA2_TAS_MODE=read: one per PADRead), shifted by
// SA2_TAS_OFFSET polls. A movie entry is 8 bytes: buttons (bit 0 Start, A, B, X, Y, Z, Up, Down,
// Left, Right, L, R), triggers L/R, main stick X/Y, C-stick X/Y (128 = centre).
namespace {
std::atomic<u64> s_padInitVi{0}, s_padReads{0};
bool tasEntry(u8 out[12]) {
    static const std::vector<u8> s_movie = [] {
        std::vector<u8> v;
        if (const char* p = std::getenv("SA2_TAS"))
            if (std::FILE* f = std::fopen(p, "rb")) {
                std::fseek(f, 0, SEEK_END);
                v.resize(size_t(std::ftell(f)));
                std::fseek(f, 0, SEEK_SET);
                if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
                std::fclose(f);
                log("[tas] %s: %zu polls\n", p, v.size() > 256 ? (v.size() - 256) / 8 : 0);
            }
        return v;
    }();
    if (s_movie.size() <= 256) return false;
    static const bool s_byRead = std::getenv("SA2_TAS_MODE") && !std::strcmp(std::getenv("SA2_TAS_MODE"), "read");
    static const s64 s_offset = std::getenv("SA2_TAS_OFFSET") ? std::atoll(std::getenv("SA2_TAS_OFFSET")) : 0;
    const s64 i = (s_byRead ? s64(s_padReads.load()) : s64(viRetraces() - s_padInitVi.load())) + s_offset;
    const size_t at = 256 + size_t(std::max<s64>(i, 0)) * 8;
    if (at + 8 > s_movie.size()) return false;  // past the end: the player's own controller again
    const u8* e = &s_movie[at];
    const u32 b = e[0] | e[1] << 8;
    static const u16 kMap[12] = {0x1000, 0x0100, 0x0200, 0x0400, 0x0800, 0x0010, 0x0008, 0x0004, 0x0001, 0x0002, 0x0040, 0x0020};
    u16 btn = 0;
    for (int k = 0; k < 12; k++)
        if (b & (1u << k)) btn |= kMap[k];
    out[0] = u8(btn >> 8), out[1] = u8(btn);
    out[2] = u8(e[4] - 128), out[3] = u8(e[5] - 128);  // stick
    out[4] = u8(e[6] - 128), out[5] = u8(e[7] - 128);  // C-stick
    out[6] = e[2], out[7] = e[3];                       // triggers
    out[8] = out[9] = 0, out[10] = 0, out[11] = 0;       // analog A/B, no error
    static u64 s_logged = ~0ull;
    if (i / 3600 != s64(s_logged)) s_logged = u64(i / 3600), log("[tas] poll %lld\n", (long long)i);
    return true;
}
}  // namespace
HLE(PADInit) {
    s_padInitVi = viRetraces();
    c.r[3] = 1;
}
HLE(PADReset) { c.r[3] = 1; }
HLE(PADRecalibrate) { c.r[3] = 1; }
HLE(PADControlMotor) {}
void padSnapshot(u8 out[4][12]);  // src/runtime/frontend.cpp (host controllers via Aurora)
HLE(PADRead) {
    u8 pads[4][12];
    padSnapshot(pads);
    tasEntry(pads[0]);
    s_padReads++;
    u32 mask = 0;
    for (u32 i = 0; i < 4; i++) {
        std::memcpy(hostp(c.r[3] + i * 12), pads[i], 12);
        if (s8(pads[i][10]) == 0) mask |= 0x80000000u >> i;
    }
    static const bool s_log = std::getenv("SA2_PAD_LOG") != nullptr;  // buttons as the game gets them
    if (s_log) {
        static u16 s_last = 0;
        const u16 b = u16(pads[0][0] << 8 | pads[0][1]);
        if (b != s_last) log("[pad] read %llu (lr %08X): port 1 buttons %04X err %d\n", (unsigned long long)s_padReads, c.lr, b, s8(pads[0][10]));
        s_last = b;
    }
    c.r[3] = mask;
}

}  // namespace rt

// CPU helpers the generated code calls out of line: quantized paired-single loads/stores, SPRs,
// MSR, traps, time base, string loads; plus logging, fatal errors and call tracing.
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <xmmintrin.h>

namespace rt {


// ---- logging ----
static std::mutex s_logLock;
static std::FILE* s_logFile = nullptr;  // logToFile: a copy of everything logged, each line timed

void logToFile(const char* path) {
    std::lock_guard g(s_logLock);
    s_logFile = std::fopen(path, "w");
}
void vlog(const char* fmt, va_list ap) {
    std::lock_guard g(s_logLock);
    va_list copy;
    va_copy(copy, ap);
    std::vfprintf(stdout, fmt, ap);
    std::fflush(stdout);
    if (s_logFile) {
        static const auto s_start = std::chrono::steady_clock::now();
        static bool s_lineStart = true;
        char buf[2048];
        std::vsnprintf(buf, sizeof buf, fmt, copy);
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
        for (const char* p = buf; *p;) {
            if (s_lineStart) std::fprintf(s_logFile, "%02d:%06.3f ", int(t / 60), std::fmod(t, 60.0));
            const char* nl = std::strchr(p, '\n');
            const size_t n = nl ? size_t(nl - p + 1) : std::strlen(p);
            std::fwrite(p, 1, n, s_logFile);
            s_lineStart = nl != nullptr;
            p += n;
        }
        std::fflush(s_logFile);
    }
    va_end(copy);
}
void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

// ---- tracing: last functions entered, per host thread. Rings are registered so the watchdog can
// show what every thread is doing. ----
namespace {
struct Ring {
    u32 a[64];
    u32 pos;
    u64 calls;
};
std::mutex s_ringsLock;
std::vector<Ring*> s_rings;
thread_local Ring* t_ring = nullptr;
Ring* myRing() {
    if (!t_ring) {
        t_ring = new Ring{};
        std::lock_guard g(s_ringsLock);
        s_rings.push_back(t_ring);
    }
    return t_ring;
}
void dumpRing(const Ring* r, u32 n) {
    for (u32 i = 64 - n; i < 64; i++) {
        const u32 a = r->a[(r->pos + i) & 63];
        if (!a) continue;
        const char* nm = guestName(a);
        std::fprintf(stdout, "    %08X %s\n", a, nm ? nm : "");
    }
}
}  // namespace
std::atomic<int> g_traceBurst{0};  // print this many upcoming function entries (any thread)

// Code coverage (SA2_COVER=<file>, tests): every main.dol function entered, with the scenes it ran in (the
// game's major and minor scene, Kirby Air Ride's gm state +0x7D4 / +0x7D7). Written at exit and every 20 s:
// "<addr> <major mask> <minor mask>" per line.
namespace {
constexpr u32 kCovLo = 0x80003100, kCovHi = 0x80490000;
struct Cov {
    u32 major;
    u64 minor;
};
Cov* s_cov = nullptr;
std::string s_covFile;
void covDump() {
    if (!s_cov) return;
    static std::mutex m;
    std::lock_guard g(m);
    const std::string tmp = s_covFile + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    for (u32 i = 0; i < (kCovHi - kCovLo) / 4; i++)
        if (s_cov[i].major) std::fprintf(f, "%08X %08X %016llX\n", kCovLo + 4 * i, s_cov[i].major, (unsigned long long)s_cov[i].minor);
    std::fclose(f);
    std::remove(s_covFile.c_str());
    std::rename(tmp.c_str(), s_covFile.c_str());
}
const bool s_covInit = [] {
    const char* e = std::getenv("SA2_COVER");
    if (!e || !*e) return false;
    s_covFile = e;
    s_cov = static_cast<Cov*>(std::calloc((kCovHi - kCovLo) / 4, sizeof(Cov)));
    g_traceOn = true;
    std::atexit(covDump);
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(20));
            covDump();
        }
    }).detach();
    return true;
}();
}  // namespace
void trace_enter(u32 addr, Ctx& c) {
    if (s_cov && addr - kCovLo < kCovHi - kCovLo) {
        Cov& v = s_cov[(addr - kCovLo) / 4];
        v.major |= 1u << (rd8(0x805359D8 + 0x7D4) & 31);
        v.minor |= u64(1) << (rd8(0x805359D8 + 0x7D7) & 63);
        if (g_traceBurst.load(std::memory_order_relaxed) <= 0) return;
    }
    Ring* r = myRing();
    r->a[r->pos++ & 63] = addr;
    r->calls++;
    if (g_traceBurst.load(std::memory_order_relaxed) > 0 && g_traceBurst.fetch_sub(1) > 0) {
        const char* n = guestName(addr);
        log("[trace] %08X %s (lr %08X r3 %08X)\n", addr, n ? n : "", c.lr, c.r[3]);
    }
}
static void dumpTrace() {
    std::fprintf(stdout, "recent guest functions on this thread (oldest first):\n");
    if (t_ring) dumpRing(t_ring, 64);
}
void dumpAllThreads() {
    std::lock_guard g(s_ringsLock);
    std::fprintf(stdout, "[watch] %zu host threads ran guest code\n", s_rings.size());
    for (size_t i = 0; i < s_rings.size(); i++) {
        std::fprintf(stdout, "  thread %zu: %llu calls, last:\n", i, (unsigned long long)s_rings[i]->calls);
        dumpRing(s_rings[i], 12);
    }
    std::fflush(stdout);
}

void hostFloatMode() { _mm_setcsr(_mm_getcsr() | 0x8040); }  // FTZ | DAZ

[[noreturn]] void fatal(const char* fmt, ...) {
    {
        std::lock_guard g(s_logLock);
        std::fprintf(stdout, "\nFATAL: ");
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(stdout, fmt, ap);
        va_end(ap);
        std::fprintf(stdout, "\n");
        if (s_logFile) {
            std::fprintf(s_logFile, "FATAL: ");
            va_start(ap, fmt);
            std::vfprintf(s_logFile, fmt, ap);
            va_end(ap);
            std::fprintf(s_logFile, "\n");
            std::fflush(s_logFile);
        }
        if (g_traceOn) dumpTrace();
        std::fflush(stdout);
    }
    std::_Exit(1);
}

// ---- time base ----
static const auto s_t0 = std::chrono::steady_clock::now();
u64 timebase() {
    const u64 ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - s_t0).count());
    // Whole seconds and the rest apart: ns * 40.5 MHz overflows 64 bits after 455 s, and the guest's
    // clock then jumped back to 0 - 7 min 35 s into every session the sound sequencer (an interrupt
    // handler) saw a huge elapsed time and never caught up: the game froze with its last audio
    // buffer looping (the "freeze with a loud beep").
    return ns / 1000000000ull * kTimebaseHz + ns % 1000000000ull * kTimebaseHz / 1000000000ull;
}
u32 Ctx::timebaseLo() { return u32(timebase()); }
u32 Ctx::timebaseHi() { return u32(timebase() >> 32); }

double hostFma(double a, double b, double c) { return std::fma(a, b, c); }

// ---- paired-single quantization (GQR: load type 16-18, load scale 24-29, store type 0-2,
// store scale 8-13; types 0 float, 4 u8, 5 u16, 6 s8, 7 s16; scale is 6-bit signed) ----
namespace {
int scaleOf(u32 v) { return (v & 0x20) ? int(v) - 64 : int(v); }
// 2^-s for the 6-bit signed scales (std::ldexp per component was a measurable cost)
struct Pow2 {
    double v[64];
    Pow2() {
        for (int s = -32; s < 32; s++) v[s + 32] = std::ldexp(1.0, s);
    }
    double operator()(int s) const { return v[s + 32]; }
};
const Pow2 kPow2;
double dequant(u32 ea, u32 type, int scale, u32& size) {
    switch (type) {
    case 4: size = 1; return double(rd8(ea)) * kPow2(-scale);
    case 5: size = 2; return double(rd16(ea)) * kPow2(-scale);
    case 6: size = 1; return double(s8(rd8(ea))) * kPow2(-scale);
    case 7: size = 2; return double(s16(rd16(ea))) * kPow2(-scale);
    default: size = 4; return double(flt(rd32(ea)));
    }
}
void quant(u32 ea, u32 type, int scale, double v, u32& size) {
    auto clampTo = [](double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); };
    switch (type) {
    case 4: size = 1; wr8(ea, u32(clampTo(v * kPow2(scale), 0, 255))); return;
    case 5: size = 2; wr16(ea, u32(clampTo(v * kPow2(scale), 0, 65535))); return;
    case 6: size = 1; wr8(ea, u32(s32(clampTo(v * kPow2(scale), -128, 127)))); return;
    case 7: size = 2; wr16(ea, u32(s32(clampTo(v * kPow2(scale), -32768, 32767)))); return;
    default: size = 4; wr32(ea, bits32(float(v))); return;
    }
}
}  // namespace

void Ctx::psqLoad(int d, u32 ea, bool w, int i) {
    const u32 g = gqr[i];
    const u32 type = (g >> 16) & 7;
    if (type < 4) {  // floats (most loads): no conversion
        f[d] = double(flt(rd32(ea)));
        ps1[d] = w ? 1.0 : double(flt(rd32(ea + 4)));
        return;
    }
    const int scale = scaleOf((g >> 24) & 0x3F);
    u32 size;
    f[d] = dequant(ea, type, scale, size);
    ps1[d] = w ? 1.0 : dequant(ea + size, type, scale, size);
}

void Ctx::psqStore(int s, u32 ea, bool w, int i) {
    const u32 g = gqr[i];
    const u32 type = g & 7;
    if (type < 4) {
        wr32(ea, bits32(float(f[s])));
        if (!w) wr32(ea + 4, bits32(float(ps1[s])));
        return;
    }
    const int scale = scaleOf((g >> 8) & 0x3F);
    u32 size;
    quant(ea, type, scale, f[s], size);
    if (!w) quant(ea + size, type, scale, ps1[s], size);
}

// ---- special registers ----
void Ctx::mtspr(u32 n, u32 v) {
    spr[n & 1023] = v;
    // Locked-cache DMA: DMA_U (922) = memory address | length high, DMA_L (923) = cache address |
    // load bit (16) | length low (bits 2-3) | trigger (2). Done at once; LCQueueWait sees it idle.
    if (n == 923 && (v & 2)) {
        const u32 u = spr[922];
        u32 blocks = ((u & 0x1F) << 2) | ((v >> 2) & 3);
        if (!blocks) blocks = 128;
        const u32 mem = u & ~31u, lc = v & ~31u, len = blocks * 32;
        if (v & 0x10) __builtin_memcpy(hostp(lc), hostp(mem), len);
        else __builtin_memcpy(hostp(mem), hostp(lc), len);
        spr[923] = v & ~2u;
    }
}
u32 Ctx::mfspr(u32 n) {
    switch (n) {
    case 22: return u32(-1);           // DEC: never expires (alarms are host timers)
    case 287: return 0x00083214;       // PVR: Gekko
    default: return spr[n & 1023];
    }
}
void Ctx::mtmsr(u32 v) {
    const bool wasOff = !(msr & 0x8000);
    msr = v;
    if (wasOff && (msr & 0x8000)) poll();
}

void Ctx::syscall() {}  // sc: PPCSync in the SDK; nothing to synchronise natively
void Ctx::trap(u32 pc) { fatal("trap at %08X (lr=%08X)", pc, lr); }
void Ctx::unimplemented(u32 pc, u32 insn) { fatal("unimplemented instruction %08X at %08X (lr=%08X)", insn, pc, lr); }

void Ctx::lswi(int d, u32 ea, u32 n) {
    // Four bytes per register from rD on, big-endian; the last register is zero-padded.
    for (u32 i = 0; i < n; i++) {
        const u32 reg = (d + i / 4) & 31;
        if (i % 4 == 0) r[reg] = 0;
        r[reg] |= rd8(ea + i) << (24 - 8 * (i % 4));
    }
}
void Ctx::stswi(int s, u32 ea, u32 n) {
    for (u32 i = 0; i < n; i++) wr8(ea + i, (r[(s + i / 4) & 31] >> (24 - 8 * (i % 4))) & 0xFF);
}

// ---- interrupt safe points ----
extern std::atomic<u32> g_dvdGatePending;  // src/runtime/hle_dvd.cpp: a held read for the main thread's safe point
void dvdGateSafePoint(Ctx& c);
void service_irq(Ctx& c) {
    if (g_dvdGatePending) dvdGateSafePoint(c);
    sched::serviceInterrupts(c);
    if (g_dvdGatePending) g_irqPending = 1;  // (the main thread's safe points go on stopping here until it is told)
}

}  // namespace rt

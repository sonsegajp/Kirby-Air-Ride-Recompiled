// Developer entry point (kirbyairride): load main.dol into guest memory, set up the boot block the IPL
// would leave behind, and run the recompiled __start.
//   kirbyairride --iso <disc image> [--dol <main.dol>] [--headless] [--mute] [--volume 0-100] [--frames N] [--trace]
// (without --dol the program is read from the disc, as the console's apploader does)
#include "disc/discimage.h"
#include "runtime/interp.h"
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace rt {
void dvdOpenDisc(const std::string& path);  // src/runtime/hle_dvd.cpp
void dumpAllThreads();                       // src/runtime/cpu.cpp
void installCrashHandler();                  // src/runtime/crash.cpp
void installWatch(u32 guest, u32 len);       // src/runtime/crash.cpp
int runFrontend(int argc, char** argv, bool headless, u64 maxFrames, void (*startGuest)());  // frontend.cpp
void audioInit(bool enabled, float volume);  // src/runtime/audio.cpp
void applyCodeOverrides();                   // src/runtime/codeoverrides.cpp (programs generated with --overrides)
}

using namespace rt;

static bool readFile(const std::string& p, std::vector<u8>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// main.dol from the disc image (.iso/.gcm, .ciso or .rvz): the header's 0x420 is where it starts;
// its size is where its last section ends.
static bool readDiscDol(const std::string& iso, std::vector<u8>& out) {
    std::string err;
    const auto img = disc::open(std::filesystem::path(iso), err);
    if (!img) return false;
    auto be = [](const u8* p) { return u32(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; };
    u8 h[4], dh[0x100];
    if (!img->read(0x420, h, 4)) return false;
    const u32 at = be(h);
    if (!img->read(at, dh, sizeof dh)) return false;
    u32 size = 0x100;
    for (int i = 0; i < 18; i++) size = std::max(size, be(dh + i * 4) + be(dh + 0x90 + i * 4));
    out.resize(size);
    return img->read(at, out.data(), size);
}

// The boot block (OSBootInfo at 0x80000000 and friends) as the IPL leaves it for a retail disc.
static void setupLowMem(const std::vector<u8>& discHeader) {
    std::memcpy(hostp(0x80000000), discHeader.data(), 0x20);  // game id, maker, disc, version
    wr32(0x8000001C, 0xC2339F3D);  // DVD magic
    wr32(0x80000020, 0x0D15EA5E);  // normal boot
    wr32(0x80000024, 1);           // boot info version
    wr32(0x80000028, kMem1Size);   // physical memory size
    wr32(0x8000002C, 0x00000003);  // console type: retail
    wr32(0x80000030, 0);           // arena lo: linker default
    wr32(0x80000034, 0x80000000 + kMem1Size);  // arena hi: lowered to the FST by dvdOpenDisc
    wr32(0x80000038, 0);                        // FST location, size: set by dvdOpenDisc
    wr32(0x8000003C, 0);
    wr32(0x800000CC, 0);           // TV mode: NTSC
    wr32(0x800000F0, kMem1Size);   // simulated memory size
    wr32(0x800000F8, 162000000);   // bus clock
    wr32(0x800000FC, 486000000);   // CPU clock
    wr16(0x800030E6, 0x8003);      // device code: retail
}

namespace rt {
void hostTimingInit();               // src/runtime/hosttime.cpp
void logToFile(const char* path);    // src/runtime/cpu.cpp
}

int main(int argc, char** argv) {
    hostTimingInit();
    std::string dolPath, isoPath;
    int watchSecs = 0;
    bool headless = false, mute = std::getenv("SA2_MUTE") != nullptr;
    u64 maxFrames = 0;
    float volume = std::getenv("SA2_VOLUME") ? float(std::atof(std::getenv("SA2_VOLUME"))) / 100.f : 1.f;  // percent
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--dol" && i + 1 < argc) dolPath = argv[++i];
        else if (a == "--iso" && i + 1 < argc) isoPath = argv[++i];
        else if (a == "--trace") g_traceOn = true;
        else if (a == "--watch" && i + 1 < argc) watchSecs = std::atoi(argv[++i]);
        else if (a == "--headless") headless = true;
        else if (a == "--mute") mute = true;
        else if (a == "--volume" && i + 1 < argc) volume = float(std::atof(argv[++i])) / 100.f;
        else if (a == "--fast-disc") _putenv("SA2_FAST_DISC=1");  // read by the drive model (hle_dvd.cpp)
        else if (a == "--frames" && i + 1 < argc) maxFrames = std::strtoull(argv[++i], nullptr, 10);
    }
    // Played sessions keep their log (kirbyairride.log, replaced at each start); test runs (headless) capture
    // the console instead and leave it alone.
    if (!headless) logToFile("kirbyairride.log");
    installCrashHandler();
    audioInit(!headless && !mute, volume);  // headless runs are always silent
    if (!memInit()) return std::fprintf(stderr, "out of memory\n"), 1;
    std::vector<u8> dol;
    if (dolPath.empty() && !isoPath.empty()) {
        if (!readDiscDol(isoPath, dol)) return std::fprintf(stderr, "can't read the program from %s\n", isoPath.c_str()), 1;
        dolPath = "main.dol (disc)";
    } else {
        if (dolPath.empty()) dolPath = "orig/main.dol";
        if (!readFile(dolPath, dol)) return std::fprintf(stderr, "can't read %s\n", dolPath.c_str()), 1;
    }
    u32 entry = 0;
    if (!loadDol(dol, entry)) return std::fprintf(stderr, "bad DOL %s\n", dolPath.c_str()), 1;
    applyCodeOverrides();
    std::vector<u8> header(0x20, 0);
    if (!isoPath.empty()) {
        std::string err;
        if (const auto img = disc::open(std::filesystem::path(isoPath), err)) img->read(0, header.data(), 0x20);
    } else {
        std::memcpy(header.data(), "GKYE01", 6);
    }
    setupLowMem(header);
    if (!isoPath.empty()) dvdOpenDisc(isoPath);  // also places the FST and records it in the boot block
    registerDol();
    resolveHooks();
    interpInit();
    gameFixesInit();
    if (const char* w = std::getenv("SA2_WATCH")) {  // SA2_WATCH=<hex address>[:<length>]
        const char* colon = std::strchr(w, ':');
        installWatch(u32(std::strtoul(w, nullptr, 16)), colon ? u32(std::strtoul(colon + 1, nullptr, 0)) : 4);
    }
    log("[boot] %s entry %08X\n", dolPath.c_str(), entry);
    if (watchSecs > 0) {
        g_traceOn = true;
        std::thread([watchSecs] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(watchSecs));
                dumpAllThreads();
                sched::debugDump();
            }
        }).detach();
    }
    static u32 s_entry;
    s_entry = entry;
    // The game runs on its own host thread; this one owns the window.
    return runFrontend(argc, argv, headless, maxFrames, [] {
        hostFloatMode();
        static Ctx boot;
        boot.msr = 0;  // __start runs with interrupts off; OSInit enables them
        boot.call(s_entry);
        log("[boot] __start returned\n");
    });
}

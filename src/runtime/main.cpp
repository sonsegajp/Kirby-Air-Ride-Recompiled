// Developer entry point (kirbyairride): load main.dol into guest memory, set up the boot block the IPL
// would leave behind, and run the recompiled __start.
//   kirbyairride --iso <disc image> [--dol <main.dol>] [--headless] [--mute] [--volume 0-100] [--frames N] [--trace]
// (without --dol the program is read from the disc, as the console's apploader does)
#include "disc/discimage.h"
#include "runtime/interp.h"
#include "runtime/runtime.h"
#include "runtime/sched.h"
#include "runtime/setup.h"

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

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

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
    const auto img = disc::open(std::filesystem::u8path(iso), err);
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
bool loadGameModule(bool eightRiders, std::string& error);  // src/runtime/game.cpp
void gameModuleMemory();             // src/runtime/game.cpp
bool setupScreen(const std::filesystem::path& exeDir, std::string& disc, bool eightRiders, std::string error);  // setupscreen.cpp
extern bool (*g_beforeGame)();       // src/runtime/frontend.cpp
}

namespace {

std::string s_dolPath, s_isoPath;
u32 s_entry;
bool s_eightRiders = false;

std::filesystem::path exeDir() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(std::wstring(exe, n)).parent_path();
}

// The disc image the player chose (disc.txt beside the program).
std::string rememberedDisc() {
    std::ifstream f(exeDir() / "disc.txt");
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);  // (a byte-order mark)
    return line;
}
void rememberDisc(const std::string& path) { std::ofstream(exeDir() / "disc.txt") << path << "\n"; }

// Everything that needs the game's code, once it is there: guest memory, the program, the disc.
bool bootGame(std::string& error) {
    if (!loadGameModule(s_eightRiders, error)) return false;
    if (!memInit()) return error = "out of memory", false;
    gameModuleMemory();
    std::vector<u8> dol;
    if (s_dolPath.empty() && !s_isoPath.empty()) {
        if (!readDiscDol(s_isoPath, dol)) return error = "can't read the program from " + s_isoPath, false;
        s_dolPath = "main.dol (disc)";
    } else {
        if (s_dolPath.empty()) s_dolPath = "orig/main.dol";
        if (!readFile(s_dolPath, dol)) return error = "can't read " + s_dolPath, false;
    }
    u32 entry = 0;
    if (!loadDol(dol, entry)) return error = "bad DOL " + s_dolPath, false;
    applyCodeOverrides();
    std::vector<u8> header(0x20, 0);
    if (!s_isoPath.empty()) {
        std::string err;
        if (const auto img = disc::open(std::filesystem::u8path(s_isoPath), err)) img->read(0, header.data(), 0x20);
    } else {
        std::memcpy(header.data(), "GKYE01", 6);
    }
    setupLowMem(header);
    if (!s_isoPath.empty()) dvdOpenDisc(s_isoPath);  // also places the FST and records it in the boot block
    registerDol();
    resolveHooks();
    interpInit();
    gameFixesInit();
    if (const char* w = std::getenv("SA2_WATCH")) {  // SA2_WATCH=<hex address>[:<length>]
        const char* colon = std::strchr(w, ':');
        installWatch(u32(std::strtoul(w, nullptr, 16)), colon ? u32(std::strtoul(colon + 1, nullptr, 0)) : 4);
    }
    log("[boot] %s entry %08X\n", s_dolPath.c_str(), entry);
    s_entry = entry;
    return true;
}

// Headless (test runs): first-run setup without a screen, its progress on the console.
bool setupHeadless() {
    Setup setup;
    setup.start(exeDir(), std::filesystem::u8path(s_isoPath), s_eightRiders);
    int lastStep = -1;
    auto lastPrint = std::chrono::steady_clock::now();
    while (setup.running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const Setup::Status st = setup.status();
        if (st.step != lastStep || std::chrono::steady_clock::now() - lastPrint > std::chrono::seconds(10)) {
            std::printf("[setup] %s %3d%% (all: %3d%%) %s\n", Setup::stepName(st.step), int(st.stepFrac * 100), int(st.overall * 100), st.detail.c_str());
            std::fflush(stdout);
            lastStep = st.step, lastPrint = std::chrono::steady_clock::now();
        }
    }
    const Setup::Status st = setup.status();
    if (!st.done) return std::fprintf(stderr, "setup failed: %s\n", st.error.c_str()), false;
    std::printf("[setup] done in %.0f s\n", st.elapsed);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    hostTimingInit();
    int watchSecs = 0;
    bool headless = false, mute = std::getenv("SA2_MUTE") != nullptr;
    u64 maxFrames = 0;
    float volume = std::getenv("SA2_VOLUME") ? float(std::atof(std::getenv("SA2_VOLUME"))) / 100.f : 1.f;  // percent
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--dol" && i + 1 < argc) s_dolPath = argv[++i];
        else if (a == "--iso" && i + 1 < argc) s_isoPath = argv[++i];
        else if (a == "--trace") g_traceOn = true;
        else if (a == "--watch" && i + 1 < argc) watchSecs = std::atoi(argv[++i]);
        else if (a == "--headless") headless = true;
        else if (a == "--mute") mute = true;
        else if (a == "--volume" && i + 1 < argc) volume = float(std::atof(argv[++i])) / 100.f;
        else if (a == "--fast-disc") _putenv("SA2_FAST_DISC=1");  // read by the drive model (hle_dvd.cpp)
        else if (a == "--frames" && i + 1 < argc) maxFrames = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--8p") s_eightRiders = true;  // the 8-rider build (kirbyairride_game_8p.dll)
        else if (a == "--window" || a == "--hd-textures") i++;  // (the frontend's, with a value)
        else if (a[0] != '-' && s_isoPath.empty()) s_isoPath = a;  // a disc image dropped on the program
    }
    // Played sessions keep their log (kirbyairride.log, replaced at each start); test runs (headless) capture
    // the console instead and leave it alone.
    if (!headless) logToFile("kirbyairride.log");
    installCrashHandler();
    audioInit(!headless && !mute, volume);  // headless runs are always silent
    // The game's code: kirbyairride_game.dll, built from the player's disc by first-run setup (src/runtime/setup.h)
    // - on the setup screen, or here for test runs. A developer's run from main.dol (--dol) takes the module as it is.
    if (s_isoPath.empty() && s_dolPath.empty()) s_isoPath = rememberedDisc();
    std::error_code ec;
    const bool haveDisc = !s_isoPath.empty() && std::filesystem::exists(std::filesystem::u8path(s_isoPath), ec);
    const bool needSetup = s_dolPath.empty() && (!haveDisc || Setup::needed(exeDir(), std::filesystem::u8path(s_isoPath), s_eightRiders));
    if (headless) {
        if (s_dolPath.empty() && !haveDisc) return std::fprintf(stderr, "no disc image (--iso)\n"), 1;
        if (needSetup && !setupHeadless()) return 1;
        if (std::string err; !bootGame(err)) return std::fprintf(stderr, "%s\n", err.c_str()), 1;
    } else if (std::string err; needSetup || !bootGame(err)) {
        // the setup screen, then the game (built, or found at last), from the frontend's thread
        static std::string s_error;
        s_error = needSetup ? "" : err;
        g_beforeGame = [] {
            for (;;) {
                const std::string before = s_isoPath;
                if (!setupScreen(exeDir(), s_isoPath, s_eightRiders, s_error)) return false;
                if (s_isoPath != before || rememberedDisc().empty()) rememberDisc(s_isoPath);
                if (bootGame(s_error)) return true;
                log("[setup] the game didn't start: %s\n", s_error.c_str());
            }
        };
    } else if (haveDisc && rememberedDisc() != s_isoPath) {
        rememberDisc(s_isoPath);
    }
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
    // The game runs on its own host thread; this one owns the window.
    return runFrontend(argc, argv, headless, maxFrames, [] {
        hostFloatMode();
        static Ctx boot;
        boot.msr = 0;  // __start runs with interrupts off; OSInit enables them
        boot.call(s_entry);
        log("[boot] __start returned\n");
    });
}

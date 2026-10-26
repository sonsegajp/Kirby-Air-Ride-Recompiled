// Window, render loop and controllers (Aurora). The game runs on its own host threads; this thread
// owns the window: each frame it takes the game's finished GX command stream (src/runtime/gx.cpp),
// feeds it to Aurora's command processor and presents.
// Debug/automation: --headless renders with the window hidden; SA2_DUMP_DIR=<dir> writes every
// SA2_DUMP_EVERY-th frame (from SA2_DUMP_FROM on) as raw RGBA (tools/rgba2png.py converts it);
// --frames N exits after N.
#include "runtime/gx.h"
#include "runtime/menu.h"
#include "runtime/online_menu.h"
#include "runtime/results8.h"
#include "runtime/morepl.h"
#include "runtime/settings.h"
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <aurora/gfx.hpp>
#include <dolphin/gx.h>
#include <aurora/texture.hpp>  // (needs GXTexObj from dolphin/gx.h)
#include <dolphin/mtx.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_video.h>

#include "gx/fifo.hpp"
#include "window.hpp"
#include "webgpu/gpu.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

extern "C" void aurora_texture_memo_reset();  // (patches/aurora: texture hashes kept for one game frame)
extern "C" void aurora_pipeline_cache_clean_exit();
extern "C" void aurora_pipeline_cache_session_ok();  // (patches/aurora: past its first seconds, a session keeps the cache)  // (patches/aurora: a crashed session drops the pipeline cache)
namespace rt {

extern std::atomic<u64> g_relLinkFrame[256];  // src/runtime/dispatch.cpp
extern std::atomic<bool> g_stopRequested;     // src/runtime/dispatch.cpp
extern std::atomic<bool> g_relLinked[256];    // src/runtime/dispatch.cpp
std::string dvdPendingSummary();                // src/runtime/hle_dvd.cpp
void profileSnapshot(int samples, int ms);       // src/runtime/profile.cpp
void dumpAllThreads();                         // src/runtime/cpu.cpp
u64 hostGpuMemory();                           // src/runtime/hostgpu.cpp
void audioSetGain(float gain);                 // src/runtime/audio.cpp

namespace {
std::mutex s_padLock;
u8 s_padGuest[4][12];  // PADStatus as the game sees it (big-endian)

void auroraLog(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
    static const char* names[] = {"debug", "info", "warn", "error", "fatal"};
    static const int minLevel = std::getenv("SA2_AURORA_LOG") ? std::atoi(std::getenv("SA2_AURORA_LOG")) : LOG_WARNING;
    if (int(level) < minLevel) return;
    // The same message again and again (a warning a frame) is logged three times, then once every thousand, counted:
    // each line costs the render thread a console write and a flush.
    if (level < LOG_FATAL) {
        static std::mutex s_lock;
        static std::string s_last;
        static u64 s_repeats = 0;
        std::lock_guard g(s_lock);
        const std::string text = std::string(module) + ": " + std::string(message, len);
        if (text == s_last) {
            if (++s_repeats > 2 && s_repeats % 1000 != 0) return;
            if (s_repeats % 1000 == 0) {
                log("[aurora %s] %s (%llu times)\n", names[level < 5 ? level : 4], text.c_str(), (unsigned long long)s_repeats);
                return;
            }
        } else {
            s_last = text, s_repeats = 0;
        }
    }
    log("[aurora %s] %s: %.*s\n", names[level < 5 ? level : 4], module, int(len), message);
    if (level == LOG_FATAL) std::abort();
}

// Scripted input for automated runs: SA2_PRESS="300:A,420:START,..." holds the buttons on port 1
// for 8 frames from the given presented frame; "r600:START" counts from the SA2_DUMP_REL module's
// link (a stage) instead. SL/SR hold the stick left/right (steering).
u64 s_frameNo = 0;
constexpr u64 kRelPress = 1ull << 63;
constexpr u16 kStickLeft = 0x4000, kStickRight = 0x8000;  // bits no PAD button uses
u16 scriptedButtons() {
    static const int s_rel = std::getenv("SA2_DUMP_REL") ? std::atoi(std::getenv("SA2_DUMP_REL")) & 255 : -1;
    static const std::vector<std::pair<u64, u16>> script = [] {
        std::vector<std::pair<u64, u16>> v;
        const char* s = std::getenv("SA2_PRESS");
        while (s && *s) {
            const bool rel = *s == 'r';
            if (rel) s++;
            const u64 frame = std::strtoull(s, const_cast<char**>(&s), 10) | (rel ? kRelPress : 0);
            if (*s != ':') break;
            const char* name = ++s;
            while (*s && *s != ',') s++;
            const std::string b(name, size_t(s - name));
            const u16 mask = b == "A" ? PAD_BUTTON_A : b == "B" ? PAD_BUTTON_B : b == "X" ? PAD_BUTTON_X
                           : b == "Y" ? PAD_BUTTON_Y : b == "START" ? PAD_BUTTON_START : b == "UP" ? PAD_BUTTON_UP
                           : b == "DOWN" ? PAD_BUTTON_DOWN : b == "LEFT" ? PAD_BUTTON_LEFT : b == "RIGHT" ? PAD_BUTTON_RIGHT
                           : b == "SL" ? kStickLeft : b == "SR" ? kStickRight : 0;
            v.push_back({frame, mask});
            if (*s == ',') s++;
        }
        return v;
    }();
    u16 held = 0;
    // SA2_MENU_SEQ="START~mode select;DOWN@mode select;A@..." (test runs: menus driven by what they show - load times
    // vary): steps in turn. "B~S" presses B every 1.5 s until the screen recognised (gxMenuScreen) is S; "B@S" presses B
    // once S has shown for half a second, then waits a second. A bare "B" presses B and waits a second.
    if (const char* seq = std::getenv("SA2_MENU_SEQ")) {
        struct Step {
            u16 button;
            char mode;
            std::string screen;
        };
        static const std::vector<Step> steps = [&] {
            std::vector<Step> v;
            for (const char* p = seq; *p;) {
                const char* e = std::strchr(p, ';');
                const std::string t(p, e ? size_t(e - p) : std::strlen(p));
                const size_t k = t.find_first_of("~@");
                const std::string b = t.substr(0, k);
                const u16 mask = b == "A" ? PAD_BUTTON_A : b == "B" ? PAD_BUTTON_B : b == "START" ? PAD_BUTTON_START
                               : b == "UP" ? PAD_BUTTON_UP : b == "DOWN" ? PAD_BUTTON_DOWN : b == "LEFT" ? PAD_BUTTON_LEFT
                               : b == "RIGHT" ? PAD_BUTTON_RIGHT : b == "X" ? PAD_BUTTON_X : b == "Y" ? PAD_BUTTON_Y : 0;
                v.push_back({mask, k == std::string::npos ? ' ' : t[k], k == std::string::npos ? "" : t.substr(k + 1)});
                if (!e) break;
                p = e + 1;
            }
            return v;
        }();
        static size_t s_step = 0;
        static u64 s_since = 0, s_shown = 0, s_pressAt = ~0ull;
        if (s_step < steps.size()) {
            const Step& st = steps[s_step];
            const std::string now = gxMenuScreen();
            if (st.mode == '~') {
                if (now == st.screen) {
                    log("[seq] step %zu: on %s (frame %llu)\n", s_step, now.c_str(), (unsigned long long)s_frameNo);
                    s_step++, s_since = s_frameNo, s_shown = 0;
                } else if ((s_frameNo - s_since) % 90 < 8) {
                    held |= st.button;
                }
            } else {
                if (s_pressAt == ~0ull) {
                    if (st.mode == ' ' || now == st.screen) s_shown++;
                    else s_shown = 0;
                    if (s_shown >= 30) s_pressAt = s_frameNo;
                } else if (s_frameNo < s_pressAt + 8) {
                    held |= st.button;
                } else if (s_frameNo >= s_pressAt + 60) {
                    log("[seq] step %zu done (frame %llu, screen \"%s\")\n", s_step, (unsigned long long)s_frameNo, now.c_str());
                    s_step++, s_since = s_frameNo, s_shown = 0, s_pressAt = ~0ull;
                }
            }
        }
    }
    for (auto [f, m] : script) {
        if (f & kRelPress) {
            const u64 link = s_rel >= 0 ? g_relLinkFrame[s_rel].load() : 0;
            if (!link) continue;
            f = (f & ~kRelPress) + link;
        }
        if (s_frameNo >= f && s_frameNo < f + 8) held |= m;
    }
    return held;
}

void pollPads() {
    PADStatus st[4]{};
    PADRead(st);
    st[0].err = 0;  // port 1 is always connected: the keyboard stands in when there is no gamepad
    const u16 scripted = scriptedButtons();
    st[0].button |= scripted & ~(kStickLeft | kStickRight);
    if (scripted & kStickLeft) st[0].stickX = -100;
    if (scripted & kStickRight) st[0].stickX = 100;
    // (scripted directions move the stick too: some menus read only the stick)
    if (scripted & PAD_BUTTON_UP) st[0].stickY = 100;
    if (scripted & PAD_BUTTON_DOWN) st[0].stickY = -100;
    if (scripted & PAD_BUTTON_LEFT) st[0].stickX = -100;
    if (scripted & PAD_BUTTON_RIGHT) st[0].stickX = 100;
    // SA2_RUN (test runs): from 400 frames after the SA2_DUMP_REL module (a stage) linked, the stick
    // is held forward, so the player travels through the stage.
    static const bool s_run = std::getenv("SA2_RUN") != nullptr;
    // (SA2_RUN_REL=<module id> starts it from another module's link than the dumps'.)
    static const int s_runRel = std::getenv("SA2_RUN_REL")    ? std::atoi(std::getenv("SA2_RUN_REL"))
                                : std::getenv("SA2_DUMP_REL") ? std::atoi(std::getenv("SA2_DUMP_REL"))
                                                              : -1;
    if (s_run && s_runRel >= 0 && g_relLinked[s_runRel & 255] && s_frameNo > g_relLinkFrame[s_runRel & 255].load() + 400)
        st[0].stickY = 100;
    menu::takePads(st);  // the F1 menu reads them; the game gets none while it is open
    online::takePads(st);  // the Online menu likewise (and hands the game its LAN screen's Retry / Cancel)
    std::lock_guard g(s_padLock);
    for (int i = 0; i < 4; i++) {
        u8* p = s_padGuest[i];
        p[0] = u8(st[i].button >> 8);
        p[1] = u8(st[i].button);
        p[2] = u8(st[i].stickX);
        p[3] = u8(st[i].stickY);
        p[4] = u8(st[i].substickX);
        p[5] = u8(st[i].substickY);
        p[6] = st[i].triggerLeft;
        p[7] = st[i].triggerRight;
        p[8] = st[i].analogA;
        p[9] = st[i].analogB;
        p[10] = u8(st[i].err);
        p[11] = 0;
    }
}

// Frame dumps of the real framebuffer: after the frame's commands are drained, the EFB is snapshotted
// (resolve_pass) and an encoder task copies the snapshot into a buffer on Aurora's render worker;
// the file is written there after submit. Nothing touches the GPU queue from this thread.
struct EfbDump {
    wgpu::Texture tex;
    wgpu::Buffer buf;
    u32 w = 0, h = 0, bpr = 0;
    bool bgra = false;
    std::string path;
};
EfbDump s_efbDumps[4];
u32 s_efbDumpNext = 0;
aurora::gfx::EncoderTaskId s_efbTask = aurora::gfx::InvalidEncoderTask;

void efbCopyTask(const aurora::gfx::EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload, size_t,
                 void*) {
    EfbDump& d = s_efbDumps[*static_cast<const u32*>(payload) & 3];
    const wgpu::BufferDescriptor bd{.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead, .size = u64(d.bpr) * d.h};
    d.buf = ctx.device.CreateBuffer(&bd);
    const wgpu::TexelCopyTextureInfo from{.texture = d.tex};
    const wgpu::TexelCopyBufferInfo to{.layout = {.offset = 0, .bytesPerRow = d.bpr, .rowsPerImage = d.h}, .buffer = d.buf};
    const wgpu::Extent3D ext{d.w, d.h, 1};
    cmd.CopyTextureToBuffer(&from, &to, &ext);
}

void efbWriteTask(const aurora::gfx::EncoderTaskCompletionContext&, const void* payload, size_t, void*) {
    EfbDump& d = s_efbDumps[*static_cast<const u32*>(payload) & 3];
    bool ok = false;
    wgpu::Future fut = d.buf.MapAsync(wgpu::MapMode::Read, 0, u64(d.bpr) * d.h, wgpu::CallbackMode::WaitAnyOnly,
                                      [&](wgpu::MapAsyncStatus s, wgpu::StringView) { ok = s == wgpu::MapAsyncStatus::Success; });
    aurora::webgpu::g_instance.WaitAny(fut, UINT64_MAX);
    if (ok) {
        const u8* px = static_cast<const u8*>(d.buf.GetConstMappedRange(0, u64(d.bpr) * d.h));
        if (std::FILE* f = std::fopen(d.path.c_str(), "wb")) {
            const u32 fmt = d.bgra ? 1 : 0;
            std::fwrite("RGBA", 1, 4, f);
            std::fwrite(&d.w, 4, 1, f);
            std::fwrite(&d.h, 4, 1, f);
            std::fwrite(&fmt, 4, 1, f);
            for (u32 y = 0; y < d.h; y++) std::fwrite(px + size_t(y) * d.bpr, 1, size_t(d.w) * 4, f);
            std::fclose(f);
        }
        d.buf.Unmap();
    }
    d.buf = {};
    d.tex = {};
}

void dumpEfb(const std::string& path) {
    // At most SA2_DUMP_MAX (300) dumps a run: 3.6 MB each, a test asking for every frame must not
    // fill the disk.
    static const u32 s_max = std::getenv("SA2_DUMP_MAX") ? u32(std::atoi(std::getenv("SA2_DUMP_MAX"))) : 300;
    static u32 s_dumped = 0;
    if (s_dumped++ >= s_max) {
        if (s_dumped == s_max + 1) log("[frontend] %u frames dumped: no more this run (SA2_DUMP_MAX)\n", s_max);
        return;
    }
    if (s_efbTask == aurora::gfx::InvalidEncoderTask)
        s_efbTask = aurora::gfx::register_encoder_task_type({.label = "EFB dump", .callback = efbCopyTask, .afterSubmit = efbWriteTask});
    aurora::gx::fifo::drain();  // the frame's commands are recorded; the EFB pass is still open
    aurora::gfx::ResolvedTargets rt{};
    if (!aurora::gfx::resolve_pass({.color = true}, rt) || !rt.colorTexture) return;
    const u32 idx = s_efbDumpNext++ & 3;
    EfbDump& d = s_efbDumps[idx];
    d.tex = rt.colorTexture;
    d.w = rt.width;
    d.h = rt.height;
    d.bpr = (d.w * 4 + 255) & ~255u;
    d.bgra = rt.colorFormat == wgpu::TextureFormat::BGRA8Unorm || rt.colorFormat == wgpu::TextureFormat::BGRA8UnormSrgb;
    d.path = path;
    aurora::gfx::push_encoder_task(s_efbTask, &idx, sizeof idx);
}

// (Kept for EFB-copy-to-RAM work: readback of a GX texture copy, as the hardware would write it.)
constexpr u32 kEfbW = 640, kEfbH = 480;
AuroraReadbackStream s_rb = 0;
u8 s_rbKey;  // its address identifies our copy
std::vector<u8> s_rbData(size_t(kEfbW) * kEfbH * 4);
u64 s_rbPending = 0;
std::string s_rbDir;

void requestFrameDump(u64 frameNo) {
    std::vector<u8> cmd;
    auto p8 = [&](u32 v) { cmd.push_back(u8(v)); };
    auto p16 = [&](u32 v) { p8(v >> 8), p8(v); };
    auto p32 = [&](u32 v) { p16(v >> 16), p16(v); };
    auto p64 = [&](u64 v) { p32(u32(v >> 32)), p32(u32(v)); };
    auto pf = [&](float f) { p32(__builtin_bit_cast(u32, f)); };
    if (std::getenv("SA2_DUMP_QUAD")) {
        // Renderer self-test: a green quad in the middle of the screen with the simplest state.
        auto bp = [&](u32 v) { p8(GX_LOAD_BP_REG), p32(v); };
        auto cp = [&](u32 r, u32 v) { p8(GX_LOAD_CP_REG), p8(r), p32(v); };
        auto xf = [&](u32 addr, std::initializer_list<u32> vals) {
            p8(GX_LOAD_XF_REG), p16(u32(vals.size() - 1)), p16(addr);
            for (u32 v : vals) p32(v);
        };
        auto fb = [](float f) { return __builtin_bit_cast(u32, f); };
        bp(0x00000010);                                 // GENMODE: 1 color channel, 1 TEV stage, 0 texgens
        bp(0x20156156);                                 // scissor: full 640x480 (+342 bias)
        bp(0x213D5335);
        bp(0x5902ACAB);                                 // scissor offset 0
        xf(0x101A, {fb(320), fb(-240), fb(16777215), fb(662), fb(582), fb(16777215)});  // viewport 640x480
        bp(0x40000000);                                 // ZMODE: off
        bp(0x41000018);                                 // BLENDMODE: no blend, color + alpha update
        bp(0x28049000);                                 // TEV order: stage 0 = no texture, color channel 0
        bp(0xC008FFFA);                                 // TEV color 0: out = rasterized color
        bp(0xC108FFD0);                                 // TEV alpha 0: out = rasterized alpha
        bp(0xF33FFF00);                                 // alpha compare: always
        xf(0x1009, {1});                                // 1 color channel
        xf(0x100E, {1});                                // color0 from vertex color, no lighting
        xf(0x1010, {1});
        xf(0x103F, {0});                                // no texgens
        xf(0x0000, {fb(1), 0, 0, 0, 0, fb(1), 0, 0, 0, 0, fb(1), 0});  // position matrix 0 = identity
        xf(0x1020, {fb(1.0f / 320), fb(-1), fb(-1.0f / 240), fb(1), fb(-1), fb(-1), 1});  // ortho 640x480, z -> -z-1
        if (const char* st = std::getenv("SA2_QUAD_STATE")) {  // add one piece of the game's state at a time
            if (std::strchr(st, 'z')) bp(0x40000007);           // z compare LEQUAL, no update (game's ZMODE)
            if (std::strchr(st, 'b')) bp(0x410004AD);           // alpha blend (game's BLENDMODE)
            if (std::strchr(st, 'p')) bp(0x43000040);           // PE_CTRL RGB8_Z24, z before texture
            if (std::strchr(st, 'c')) cp(0x30, 0x3CF3C780);     // game's matrix indices
        }
        cp(0x30, 0x3CF3CF00);                           // matrix indices: position 0, textures identity
        xf(0x1018, {0x3CF3CF00});
        cp(0x50, 0x00002200);                           // VCD: position + color0 direct
        cp(0x60, 0);
        cp(0x70, 0x00014009);                           // VAT0: position XYZ f32, color0 RGBA8
        cp(0x80, 0x80000000);
        cp(0x90, 0);
        p8(0x80), p16(4);                               // QUADS fmt 0, 4 vertices
        for (auto [x, y] : {std::pair{160.f, 120.f}, {480.f, 120.f}, {480.f, 360.f}, {160.f, 360.f}}) {
            pf(x), pf(y), pf(-0.5f);
            p32(0x00FF00FF);                            // green, opaque
        }
        static bool s_saved = false;
        if (!s_saved) {
            s_saved = true;
            if (std::FILE* f = std::fopen("build/myquad.bin", "wb")) {
                std::fwrite(cmd.data(), 1, cmd.size(), f);
                std::fclose(f);
            }
        }
    }
    p8(GX_AURORA), p16(GX_AURORA_LOAD_COPY_SRC), p32(0), p32(0), p32(kEfbW), p32(kEfbH);
    p8(GX_AURORA), p16(GX_AURORA_LOAD_COPY_DST), p32(kEfbW), p32(kEfbH), p32(GX_TF_RGBA8), p8(0);
    p8(GX_AURORA), p16(GX_AURORA_LOAD_COPY_DEST), p64(u64(reinterpret_cast<uintptr_t>(&s_rbKey)));
    p8(GX_LOAD_BP_REG), p32(0x52000000);  // texture copy, no clear
    aurora::gx::fifo::write_data(cmd.data(), u32(cmd.size()));
    const AuroraReadbackCopySrc src{frameNo, &s_rbKey};
    if (AuroraReadbackCaptureCopies(s_rb, &src, 1)) s_rbPending = frameNo;
}

void collectFrameDump() {
    if (!s_rbPending) return;
    AuroraReadbackDest d{s_rbPending, s_rbData.data(), u32(s_rbData.size()), GX_FALSE};
    if (!AuroraReadbackGet(s_rb, &d, 1) || !d.found) return;
    char path[512];
    std::snprintf(path, sizeof path, "%s/frame_%05llu.rgba", s_rbDir.c_str(), (unsigned long long)s_rbPending);
    s_rbPending = 0;
    // GX RGBA8: 4x4 tiles of 64 bytes, the first 32 holding A/R pairs and the next 32 G/B pairs.
    std::vector<u8> lin(s_rbData.size());
    for (u32 ty = 0; ty < kEfbH / 4; ty++)
        for (u32 tx = 0; tx < kEfbW / 4; tx++) {
            const u8* t = &s_rbData[(size_t(ty) * (kEfbW / 4) + tx) * 64];
            for (u32 i = 0; i < 16; i++) {
                u8* o = &lin[((size_t(ty) * 4 + i / 4) * kEfbW + tx * 4 + i % 4) * 4];
                o[0] = t[i * 2 + 1], o[1] = t[32 + i * 2], o[2] = t[32 + i * 2 + 1], o[3] = t[i * 2];
            }
        }
    if (std::FILE* f = std::fopen(path, "wb")) {
        const u32 w = kEfbW, h = kEfbH, fmt = 0;
        std::fwrite("RGBA", 1, 4, f);
        std::fwrite(&w, 4, 1, f);
        std::fwrite(&h, 4, 1, f);
        std::fwrite(&fmt, 4, 1, f);
        std::fwrite(lin.data(), 1, lin.size(), f);
        std::fclose(f);
    }
}
}  // namespace

u64 viRetraces();  // src/runtime/hle_hw.cpp

void padSnapshot(u8 out[4][12]) {
    std::lock_guard g(s_padLock);
    std::memcpy(out, s_padGuest, sizeof s_padGuest);
}

bool (*g_beforeGame)() = nullptr;

int runFrontend(int argc, char** argv, bool headless, u64 maxFrames, void (*startGuest)()) {
    if (headless) _putenv("AURORA_HEADLESS=1");
    // Widescreen (default): the window's aspect, with the 3D views widened and the HUD kept at its
    // shape near the screen edges; --no-widescreen keeps the original 4:3 picture, pillarboxed.
    // Presentation without vsync by default (Aurora then picks Mailbox: no tearing, no waiting): with
    // vsync a present waits for the display's refresh, a frame that runs long backs the queue up,
    // and the game, held once the renderer is 4 frames behind, falls under 60 - the intro's effects
    // (frame-counted) then drift from its animation (music-timed). --vsync restores waiting.
    // (the window as the player left it, from the menu's settings, unless the command line says otherwise)
    settings::load();
    bool widescreen = settings::widescreen, fullscreen = settings::fullscreen, vsync = false;
    u32 winW = 0, winH = 0;  // --window WxH
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--no-widescreen")) widescreen = false;
        else if (!std::strcmp(argv[i], "--fullscreen")) fullscreen = true;
        else if (!std::strcmp(argv[i], "--vsync")) vsync = true;
        else if (!std::strcmp(argv[i], "--window") && i + 1 < argc) std::sscanf(argv[++i], "%ux%u", &winW, &winH);
    }
    settings::fullscreen = fullscreen, settings::widescreen = widescreen;
    if (!winW || !winH) winW = 1280, winH = widescreen ? 720 : 960;
    // The window's title is the project's name; Aurora's folders (shader cache, settings) get a name
    // a path can hold - Aurora derives them from the title otherwise, and its colon made no path
    // at all (a null cache path: a crash at start).
    // (SA2_CACHE_DIR: test runs keep their shader caches apart from the player's)
    static char* const s_prefPath = std::getenv("SA2_CACHE_DIR") ? std::getenv("SA2_CACHE_DIR")
                                                                  : SDL_GetPrefPath(nullptr, "Kirby Air Ride Recompiled");
    const AuroraConfig cfg{
        .appName = "Kirby Air Ride: Recompiled",
        .userPath = s_prefPath,
        .cachePath = s_prefPath,
        .vsync = vsync,
        .startFullscreen = fullscreen,
        .windowWidth = winW,
        .windowHeight = winH,
        .logCallback = &auroraLog,
    };
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");  // the window's icon: the program's
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    const AuroraInfo info = aurora_initialize(argc, argv, &cfg);
    menu::init();  // the F1 menu's fonts
    AuroraSetViewportPolicy(widescreen ? AURORA_VIEWPORT_STRETCH : AURORA_VIEWPORT_FIT);
    AuroraSetWidescreen(widescreen);
    // --hd-textures <folder>: the launcher's HD texture pack (tools/hd/build_pack.py). Aurora looks a
    // replacement up as each texture loads, by size, format and the hash of its texels (Dolphin's
    // tex1_ names), and streams it in.
    // The ones loaded stay cached up to a budget (least recently used dropped first): about a third
    // of the graphics card's memory (Aurora's own 4 GB would fill a 6 GB card with the game's
    // buffers besides), 0.5-4 GB.
    for (int i = 1; i + 1 < argc; i++)
        if (!std::strcmp(argv[i], "--hd-textures")) {
            const u64 vram = hostGpuMemory();
            const u64 budget = std::clamp<u64>(vram ? vram * 3 / 10 : 1ull << 30, 512ull << 20, 4ull << 30);
            aurora::texture::set_replacement_cache_budget(budget);
            const auto group = aurora::texture::load_replacement_directory(std::filesystem::path(argv[i + 1]));
            log("[frontend] HD textures: %zu from %s; up to %llu MB kept (card: %llu MB)\n", group.registrations.size(), argv[i + 1],
                (unsigned long long)(budget >> 20), (unsigned long long)(vram >> 20));
        }
    log("[frontend] Aurora up: backend %d, window %ux%u, framebuffer %ux%u\n", int(info.backend), info.windowSize.width,
        info.windowSize.height, info.windowSize.fb_width, info.windowSize.fb_height);
    // Aurora's own GX/VI state starts from its GXInit defaults and a 640x480 NTSC logical
    // framebuffer; everything after that comes from the game's command stream.
    static u8 s_auroraFifo[0x10000];
    if (aurora_begin_frame()) {
        GXInit(s_auroraFifo, sizeof s_auroraFifo);
        VIInit();
        VIConfigure(&GXNtsc480IntDf);
        aurora_end_frame();
    }
    // Keyboard stand-in for port 1 (a gamepad, when connected, works alongside it):
    // WASD stick, IJKL C-stick, Space A, Shift B, F X, C Y, Tab Z, Q/E L/R, Enter START, arrows D-pad.
    static const PADDefaultKeyBindings s_keys = {
        {{SDL_SCANCODE_SPACE, PAD_BUTTON_A},   {SDL_SCANCODE_LSHIFT, PAD_BUTTON_B},  {SDL_SCANCODE_F, PAD_BUTTON_X},
         {SDL_SCANCODE_C, PAD_BUTTON_Y},       {SDL_SCANCODE_RETURN, PAD_BUTTON_START}, {SDL_SCANCODE_TAB, PAD_TRIGGER_Z},
         {SDL_SCANCODE_Q, PAD_TRIGGER_L},      {SDL_SCANCODE_E, PAD_TRIGGER_R},     {SDL_SCANCODE_UP, PAD_BUTTON_UP},
         {SDL_SCANCODE_DOWN, PAD_BUTTON_DOWN}, {SDL_SCANCODE_LEFT, PAD_BUTTON_LEFT}, {SDL_SCANCODE_RIGHT, PAD_BUTTON_RIGHT}},
        {{SDL_SCANCODE_D, PAD_AXIS_LEFT_X_POS, 100},  {SDL_SCANCODE_A, PAD_AXIS_LEFT_X_NEG, 100},
         {SDL_SCANCODE_W, PAD_AXIS_LEFT_Y_POS, 100},  {SDL_SCANCODE_S, PAD_AXIS_LEFT_Y_NEG, 100},
         {SDL_SCANCODE_L, PAD_AXIS_RIGHT_X_POS, 100}, {SDL_SCANCODE_J, PAD_AXIS_RIGHT_X_NEG, 100},
         {SDL_SCANCODE_I, PAD_AXIS_RIGHT_Y_POS, 100}, {SDL_SCANCODE_K, PAD_AXIS_RIGHT_Y_NEG, 100},
         {SDL_SCANCODE_Q, PAD_AXIS_TRIGGER_L, 100},   {SDL_SCANCODE_E, PAD_AXIS_TRIGGER_R, 100}},
    };
    if (!PADSetDefaultKeyBindings(0, &s_keys)) log("[frontend] keyboard bindings rejected\n");
    PADInit();
    PADSetKeyboardActive(0, TRUE);
    pollPads();
    // First-run setup (the setup screen) and what needs the game's code, before the game starts.
    if (g_beforeGame && !g_beforeGame()) {
        log("[frontend] closed before the game started\n");
        aurora_pipeline_cache_clean_exit();
        std::fflush(stdout);
        std::_Exit(0);
    }
    log("[frontend] controllers ready; starting the game\n");
    std::thread(startGuest).detach();

    const char* dumpDir = std::getenv("SA2_DUMP_DIR");
    const u64 dumpEvery = std::getenv("SA2_DUMP_EVERY") ? std::strtoull(std::getenv("SA2_DUMP_EVERY"), nullptr, 10) : 60;
    // In-between frames (src/runtime/gxinterp.cpp): on when the display is faster than the game's
    // 60 Hz and the menu's Interpolation setting is on, then paced by the display (vsync); --interp /
    // --no-interp (or SA2_INTERP=1/0) force them. (The 2D over the 3D - the icons over riders, the
    // HUD - blends too: at 60 Hz under blended 3D it trailed.) SA2_INTERP_TEST=<t> (test runs) draws
    // every game frame once, at t between it and the one before, for checking the blending in dumps.
    int interpMode = -1;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--interp")) interpMode = 1;
        else if (!std::strcmp(argv[i], "--no-interp")) interpMode = 0;
    }
    if (const char* e = std::getenv("SA2_INTERP")) interpMode = std::atoi(e) ? 1 : 0;
    const double interpTest = std::getenv("SA2_INTERP_TEST") ? std::atof(std::getenv("SA2_INTERP_TEST")) : -1.0;
    float refresh = 0;
    if (SDL_Window* w = aurora::window::get_sdl_window())
        if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w))) refresh = m->refresh_rate;
    // (any display faster than the game's 60 Hz can have them - 120, 144 Hz)
    const bool canInterp = interpTest < 0 && !headless && interpMode < 0 && refresh > 65.f;
    bool interp = interpTest < 0 && (interpMode == 1 || (canInterp && settings::interpolation));
    gxSetRecording(interp || interpTest >= 0);
    if ((interp || canInterp) && !std::getenv("SA2_NO_VSYNC")) aurora_enable_vsync(true);  // (SA2_NO_VSYNC: measure the cost unpaced)
    log("[frontend] display %.0f Hz: in-between frames %s\n", refresh, interp ? "on" : interpTest >= 0 ? "test" : "off");

    std::vector<std::shared_ptr<GxFrame>> fresh;
    std::vector<u8> frame;                       // the commands drawn this time
    std::vector<std::unique_ptr<u8[]>> keep[2];  // blended vertex arrays of this draw and the last
    std::shared_ptr<GxFrame> held[2];            // the frames they draw (their snapshots)
    int kb = 0;
    u64 presented = 0, renders = 0;              // game frames taken; frames drawn
    double interval = 1.0 / 60, lastTime = -1;
    const GxFrame* lastShown = nullptr;
    double s_slot = 0, s_drawCost = 0.006;  // in-between frames' pacing: the next one's time, one's cost
    float lastT = -1;
    for (;;) {
        bool exiting = false;
        for (const AuroraEvent* ev = aurora_update(); ev && ev->type != AURORA_NONE; ++ev)
            if (ev->type == AURORA_EXIT) exiting = true;
            else if (ev->type == AURORA_SDL_EVENT) {
                menu::event(ev->sdl);    // F1, Escape
                online::event(ev->sdl);  // the Online menu: typing, Escape, F2
            }
        if (exiting) break;
        pollPads();
        {  // the menu's Master Volume
            static float s_gain = -1.f;
            const float gain = menu::volume();  // (silent in the background with Mute in Background)
            if (gain != s_gain) audioSetGain(gain), s_gain = gain;
        }
        // In-between frames as the menu's Interpolation setting asks, unless the command line forced them.
        if (canInterp && settings::interpolation != interp) {
            interp = settings::interpolation;
            log("[frontend] in-between frames %s\n", interp ? "on" : "off");
            gxSetRecording(interp);
            if (interp) gxInterpReset();  // (frames recorded with them off have nothing to blend)
        }
        {  // what widescreen shows beyond each side of the 4:3 screen, for the game-code changes
            const AuroraWindowSize ws = aurora::window::get_window_size();
            const float aspect = ws.fb_height ? float(ws.fb_width) / float(ws.fb_height) : 4.f / 3.f;
            if (settings::widescreen != widescreen) {  // (the menu's Widescreen)
                widescreen = settings::widescreen;
                AuroraSetViewportPolicy(widescreen ? AURORA_VIEWPORT_STRETCH : AURORA_VIEWPORT_FIT);
                AuroraSetWidescreen(widescreen);
            }
            g_wideExtra = widescreen ? std::max(0.f, 320.f * (aspect * 0.75f - 1.f)) : 0.f;
        }
        {  // F11 (or the menu): fullscreen on/off
            static bool s_f11 = false, s_full = fullscreen;
            const bool down = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_F11];
            if (down && !s_f11) settings::fullscreen = !settings::fullscreen, settings::save();
            s_f11 = down;
            if (settings::fullscreen != s_full) aurora::window::set_fullscreen(s_full = settings::fullscreen);
        }
        // The game's finished frames (waiting a little when there is nothing to draw).
        gxTakeFrames(fresh, interp && gxInterpNewest() ? 0 : 16);
        {  // A hang: no new game frame for 3 s - where every thread is, what the disc is doing (once per hang).
            static double s_lastFrameAt = gxNow();
            static bool s_reported = false;
            if (!fresh.empty()) s_lastFrameAt = gxNow(), s_reported = false;
            else if (presented > 0 && !s_reported && gxNow() - s_lastFrameAt > 3.0) {
                s_reported = true;
                log("[stall] no game frame for 3 s (last: frame %llu); disc: %s\n", (unsigned long long)presented,
                    dvdPendingSummary().c_str());
                sched::debugDump();
                morepl::stallDump();  // (the 8-player build: the game's file preload table)
                profileSnapshot(100, 500);
            }
        }
        bool stop = false;
        for (auto& f : fresh) {
            if (lastTime >= 0) interval = std::clamp(0.9 * interval + 0.1 * (f->time - lastTime), 1.0 / 75, 1.0 / 20);
            lastTime = f->time;
            presented++;
            if (presented <= 3 || presented % 600 == 0)
                log("[frontend] game frame %llu: %zu bytes of GX commands\n", (unsigned long long)presented, f->cmds.size());
            // Freeze evidence: the same command stream for 3 s (gameplay waiting on something) logs
            // what the game threads and the disc are doing. Static menus trip it too, hence the cap.
            {
                static u64 s_lastHash = 0, s_same = 0;
                static int s_reports = 0;
                u64 h = 1469598103934665603ull ^ f->cmds.size();
                for (size_t i = 0; i < f->cmds.size(); i += 61) h = (h ^ f->cmds[i]) * 1099511628211ull;
                s_same = h == s_lastHash ? s_same + 1 : 0;
                s_lastHash = h;
                if (s_same == 180 && s_reports++ < 6) {
                    log("[stall] frame %llu: the game has shown an identical frame for 3 s; disc: %s\n",
                        (unsigned long long)presented, dvdPendingSummary().c_str());
                    dumpAllThreads();
                    sched::debugDump();
                }
            }
            s_frameNo = presented;
            // SA2_GXDUMP_FRAME=<n>: that frame's commands to build/gxframe.bin; =<from>-<to>: each to build/gxframe_<n>.bin
            static u64 s_gxDumpFrom = 0, s_gxDumpTo = 0;
            if (const char* gxDump = std::getenv("SA2_GXDUMP_FRAME"); gxDump && !s_gxDumpTo) {
                char* end;
                s_gxDumpFrom = std::strtoull(gxDump, &end, 10);
                s_gxDumpTo = *end == '-' ? std::strtoull(end + 1, nullptr, 10) : s_gxDumpFrom;
            }
            if (s_gxDumpTo && presented >= s_gxDumpFrom && presented <= s_gxDumpTo) {
                char name[64];
                if (s_gxDumpTo == s_gxDumpFrom) std::snprintf(name, sizeof name, "build/gxframe.bin");
                else std::snprintf(name, sizeof name, "build/gxframe_%05llu.bin", (unsigned long long)presented);
                if (std::FILE* fo = std::fopen(name, "wb")) {  // decode with tools/gxdump.py
                    std::fwrite(f->preamble.data(), 1, f->preamble.size(), fo);
                    std::fwrite(f->cmds.data(), 1, f->cmds.size(), fo);
                    std::fclose(fo);
                }
            }
            gxInterpPush(f);
            if (maxFrames && presented >= maxFrames) stop = true;
            // SA2_STOP_AFTER=N with SA2_DUMP_REL: exit N frames after that module linked (test runs).
            static const u64 s_stopAfter = std::getenv("SA2_STOP_AFTER") ? std::strtoull(std::getenv("SA2_STOP_AFTER"), nullptr, 10) : 0;
            static const int s_stopRel = std::getenv("SA2_DUMP_REL") ? std::atoi(std::getenv("SA2_DUMP_REL")) : -1;
            if (s_stopAfter && s_stopRel >= 0) {
                const u64 link = g_relLinkFrame[s_stopRel & 255].load();
                if (link && presented >= link - 1 + s_stopAfter) stop = true;
            }
            if (g_stopRequested) stop = true;
        }
        if (stop) break;
        if (!gxInterpNewest()) continue;  // keep the window responsive while the game loads
        const bool newFrame = !fresh.empty();
        if (static bool s_cacheOk = false; !s_cacheOk && presented >= 1800) s_cacheOk = true, aurora_pipeline_cache_session_ok();
        if (newFrame) aurora_texture_memo_reset();  // (Aurora is idle here: the last frame end drained it)
        // What to draw: without in-between frames, each game frame once; with them, the frames due
        // one frame (and a little) after the game finished them, blended to the display's time.
        double when;
        if (interpTest >= 0) {
            if (!newFrame) continue;
            when = gxInterpNewestTime() - (1.0 - interpTest) * interval;
            // A dumped frame: drawn at t = 0, the test's t and 1, each dumped (_t0, _tN, _t1).
            static const u64 s_tdFrom = std::getenv("SA2_DUMP_FROM") ? std::strtoull(std::getenv("SA2_DUMP_FROM"), nullptr, 10) : 0;
            static const int s_tdRel = std::getenv("SA2_DUMP_REL") ? std::atoi(std::getenv("SA2_DUMP_REL")) : -1;
            const u64 link = s_tdRel >= 0 ? g_relLinkFrame[s_tdRel & 255].load() : 1;
            const u64 at = link ? link - 1 + s_tdFrom : ~0ull;
            if (dumpDir && dumpEvery && presented >= at && (presented - at) % dumpEvery == 0) {
                for (const double tt : {0.0, interpTest, 1.0}) {
                    keep[kb].clear();
                    float got = 1.f;
                    held[kb] = gxInterpBuild(gxInterpNewestTime() - (1.0 - tt) * interval, frame, keep[kb], &got);
                    if (!held[kb] || !aurora_begin_frame()) continue;
                    kb ^= 1;
                    aurora::gx::fifo::write_data(frame.data(), u32(frame.size()));
                    char path[512];
                    std::snprintf(path, sizeof path, "%s/frame_%05llu_t%02d.rgba", dumpDir, (unsigned long long)presented,
                                  int(tt * 10 + 0.5));
                    dumpEfb(path);
                    aurora_end_frame();
                    renders++;
                }
                continue;
            }
        } else if (!interp) {
            if (!newFrame) continue;
            when = 1e300;
        } else {
            // Even pacing: one in-between frame every k refreshes. Each is drawn for its own slot in that steady schedule,
            // so what it shows moves by equal steps (drawing as fast as possible put frames on the screen one or two
            // refreshes after they were timed - judder). k is 1 - every refresh - unless frames actually come late: more
            // than half of them in half a second takes it to 2, and after 1 s 1 is tried again. (A frame now and then
            // a refresh late shows less than every other refresh for seconds.)
            // (at most the menu's Frame Rate Limit)
            const int limit = settings::frameLimit;
            const float rate = limit > 0 ? std::min(refresh, float(limit)) : refresh;
            const double period = 1.0 / double(std::max(60.f, rate));
            static int s_k = 1, s_late = 0, s_count = 0;
            static double s_retryAt = 0;
            const double now0 = gxNow();
            if (s_k > 1 && now0 >= s_retryAt) s_k = 1, s_late = s_count = 0;  // try every refresh again
            s_slot += s_k * period;
            const bool late = s_slot < now0;
            if (late || s_slot > now0 + 4 * s_k * period) s_slot = now0 + (s_k == 1 ? period : s_drawCost);  // fell behind, or a stall
            if (s_k == 1) {
                s_late += late ? 1 : 0;
                if (++s_count >= int(0.5 / period)) {
                    if (s_late * 2 > s_count) s_k = 2, s_retryAt = now0 + 1.0;  // can't keep up: every other refresh for a while
                    s_late = s_count = 0;
                }
            }
            // (every refresh: the present waits for the display; every other, or below the display's rate: start in time
            // for the slot)
            if (const double start = s_slot - s_drawCost; (s_k > 1 || rate < refresh - 1.f) && start > now0)
                sleepUntil(std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                                    std::chrono::duration<double>(start - now0)),
                           true);
            when = gxInterpShowTime(s_slot);
        }
        keep[kb].clear();
        float t = 1.f;
        static double s_tBuild = 0, s_tWrite = 0, s_tEnd = 0;  // seconds spent, for the stats line
        const double c0 = gxNow();
        std::shared_ptr<GxFrame> shown = gxInterpBuild(when, frame, keep[kb], &t);
        if (!shown) continue;
        if (interp && shown.get() == lastShown && t >= 1.f && lastT >= 1.f) {
            // The newest frame is on screen and nothing newer has come: drawing it again shows nothing new.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        lastShown = shown.get();
        lastT = t;
        held[kb] = std::move(shown);
        const double c1 = gxNow();
        s_tBuild += c1 - c0;
        static u64 s_refused = 0;
        if (!aurora_begin_frame()) {
            if (++s_refused <= 3 || s_refused % 600 == 0)
                log("[frontend] aurora_begin_frame refused (%llu)\n", (unsigned long long)s_refused);
            continue;
        }
        kb ^= 1;
        static const char* s_isolate = std::getenv("SA2_ISOLATE");  // "api": Aurora API only, "quad": test quad only
        if (s_isolate && !std::strcmp(s_isolate, "api")) {
            GXSetCopyClear(GXColor{0, 200, 0, 255}, 0xFFFFFF);
            GXColor c{0, 200, 0, 255};
            (void)c;
            frame.clear();
        } else if (s_isolate && !std::strcmp(s_isolate, "quad")) {
            frame.clear();
        } else if (s_isolate && !std::strcmp(s_isolate, "apiraw")) {
            // Replays the captured bytes of Aurora's API quad through the raw command path.
            static std::vector<u8> s_raw = [] {
                std::vector<u8> v;
                const char* path = std::getenv("SA2_RAW_FILE") ? std::getenv("SA2_RAW_FILE") : "build/apiquad.bin";
                if (std::FILE* f = std::fopen(path, "rb")) {
                    u8 b[4096];
                    size_t n;
                    while ((n = std::fread(b, 1, sizeof b, f)) > 0) v.insert(v.end(), b, b + n);
                    std::fclose(f);
                }
                return v;
            }();
            frame = s_raw;
        } else if (s_isolate && !std::strcmp(s_isolate, "apiquad")) {
            // Aurora's own API draws a green quad; its FIFO bytes are saved for comparison.
            frame.clear();
            const u32 before = aurora::gx::fifo::get_buffer_size();
            Mtx44 ortho;
            C_MTXOrtho(ortho, 0, 480, 0, 640, 0, 1);
            GXSetProjection(ortho, GX_ORTHOGRAPHIC);
            Mtx ident;
            PSMTXIdentity(ident);
            GXLoadPosMtxImm(ident, GX_PNMTX0);
            GXSetCurrentMtx(GX_PNMTX0);
            GXSetViewport(0, 0, 640, 480, 0, 1);
            GXSetScissor(0, 0, 640, 480);
            GXClearVtxDesc();
            GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
            GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
            GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
            GXSetNumChans(1);
            GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
            GXSetNumTexGens(0);
            GXSetNumTevStages(1);
            GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
            GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
            GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
            GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
            GXBegin(GX_QUADS, GX_VTXFMT0, 4);
            for (auto [x, y] : {std::pair{160.f, 120.f}, {480.f, 120.f}, {480.f, 360.f}, {160.f, 360.f}}) {
                GXPosition3f32(x, y, -0.5f);
                GXColor4u8(0, 255, 0, 255);
            }
            GXEnd();
            static bool s_saved = false;
            if (!s_saved) {
                s_saved = true;
                const u32 after = aurora::gx::fifo::get_buffer_size();
                if (std::FILE* f = std::fopen("build/apiquad.bin", "wb")) {
                    std::fwrite(aurora::gx::fifo::get_buffer_data() + before, 1, after - before, f);
                    std::fclose(f);
                }
            }
        }
        if (std::getenv("SA2_DUMP_TEST")) {
            // Capture self-test: clear the EFB to red through a cleared copy before the game's commands.
            static const u8 red[] = {GX_LOAD_BP_REG, 0x4F, 0x00, 0xFF, 0xFF,  // clear A=FF R=FF
                                     GX_LOAD_BP_REG, 0x50, 0x00, 0x00, 0x00,  // clear G=0 B=0
                                     GX_LOAD_BP_REG, 0x52, 0x00, 0x08, 0x00}; // texture copy + clear
            aurora::gx::fifo::write_data(red, sizeof red);
        }
        // SA2_DUMP_FROM counts from the start, or with SA2_DUMP_REL=<module id> from that REL's link.
        static const u64 s_dumpFrom = std::getenv("SA2_DUMP_FROM") ? std::strtoull(std::getenv("SA2_DUMP_FROM"), nullptr, 10) : 0;
        static const int s_dumpRel = std::getenv("SA2_DUMP_REL") ? std::atoi(std::getenv("SA2_DUMP_REL")) : -1;
        const u64 relLink = s_dumpRel >= 0 ? g_relLinkFrame[s_dumpRel & 255].load() : 1;
        const u64 dumpAt = relLink ? relLink - 1 + s_dumpFrom : ~0ull;
        const bool dumpThis = newFrame && dumpDir && dumpEvery && presented >= dumpAt && (presented - dumpAt) % dumpEvery == 0;
        // SA2_NO_DRAW=1 (load tests: many copies on one PC): the game runs, its frames are not drawn - but those dumped.
        static const bool s_noDraw = std::getenv("SA2_NO_DRAW") != nullptr;
        if (!s_noDraw || dumpThis) aurora::gx::fifo::write_data(frame.data(), u32(frame.size()));
        const double c2 = gxNow();
        s_tWrite += c2 - c1;
        renders++;
        if (dumpThis) {
            if (std::getenv("SA2_DUMP_QUAD") || (s_isolate && !std::strcmp(s_isolate, "quad"))) requestFrameDump(presented);
            char path[512];
            std::snprintf(path, sizeof path, "%s/frame_%05llu.rgba", dumpDir, (unsigned long long)presented);
            dumpEfb(path);
            if (std::getenv("SA2_GXDUMP")) {  // the frame's command stream too (tools/gxdump.py)
                std::snprintf(path, sizeof path, "%s/frame_%05llu.gx", dumpDir, (unsigned long long)presented);
                if (std::FILE* f = std::fopen(path, "wb")) {
                    std::fwrite(frame.data(), 1, frame.size(), f);
                    std::fclose(f);
                }
            }
        }
        menu::overlay(headless);  // the F1 menu, the frame counter, the F1 hint: over the picture
        results8::overlay(headless);  // City Trial's results with more than 4 riders (the 8-player build)
        online::overlay(headless);  // the Online menu, in the game's Online mode (drawn behind the F1 menu)
        aurora_end_frame();
        s_tEnd += gxNow() - c2;
        // What drawing one costs (building, Aurora's processing and submission), for pacing; paced,
        // the frame end barely waits for the display.
        s_drawCost = std::clamp(0.9 * s_drawCost + 0.1 * (gxNow() - c0), 0.002, 0.1);
        if (newFrame && (presented % 300 == 0 || presented == 30)) {
            static auto s_last = std::chrono::steady_clock::now();
            static u64 s_lastFrames = 0, s_lastRetraces = 0, s_lastSubmits = 0, s_lastRenders = 0;
            const auto now = std::chrono::steady_clock::now();
            const double secs = std::chrono::duration<double>(now - s_last).count();
            const u64 retraces = viRetraces(), submits = gxFrames();
            const double fps = secs > 0 ? double(presented - s_lastFrames) / secs : 0;
            const double hz = secs > 0 ? double(retraces - s_lastRetraces) / secs : 0;
            const double gameFps = secs > 0 ? double(submits - s_lastSubmits) / secs : 0;
            const double drawFps = secs > 0 ? double(renders - s_lastRenders) / secs : 0;
            const double per = double(std::max<u64>(1, renders - s_lastRenders)) / 1000.0;  // ms per draw
            s_last = now, s_lastFrames = presented, s_lastRetraces = retraces, s_lastSubmits = submits, s_lastRenders = renders;
            const AuroraStats* st = aurora_get_stats();
            log("[frontend] %llu frames, %.1f fps (game submits %.1f/s, VI %.1f Hz), drawn %.1f/s (ms each: build %.2f, "
                "commands %.2f, end %.2f); %zu bytes GX; Aurora: %u draws (%u merged), %u pipelines\n",
                (unsigned long long)presented, fps, gameFps, hz, drawFps, s_tBuild / per, s_tWrite / per, s_tEnd / per,
                frame.size(), st->drawCallCount, st->mergedDrawCallCount, st->createdPipelines);
            s_tBuild = s_tWrite = s_tEnd = 0;
        }
    }
    log("[frontend] exiting after %llu frames\n", (unsigned long long)presented);
    online::shutdown();  // leaves the online server politely (rt::airnet::shutdown)
    aurora_pipeline_cache_clean_exit();
    std::fflush(stdout);
    std::_Exit(0);
}

}  // namespace rt

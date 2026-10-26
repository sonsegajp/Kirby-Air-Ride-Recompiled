// The setup screen: what the window shows before the game the first time - choosing the disc image and building
// the game from it (src/runtime/setup.h) - in the look of the port's menus (src/runtime/menustyle.h). It runs on
// the frontend's thread, between Aurora coming up and the game starting (src/runtime/frontend.cpp).
#include "runtime/menustyle.h"
#include "runtime/runtime.h"
#include "runtime/setup.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <imgui.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_misc.h>

#include "window.hpp"

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace rt {

using namespace menustyle;
namespace fs = std::filesystem;

namespace {

// The file dialog answers on a thread of its own.
std::mutex s_pickLock;
std::string s_picked;
bool s_picking = false;

void SDLCALL onPicked(void*, const char* const* files, int) {
    std::lock_guard g(s_pickLock);
    s_picking = false;
    if (files && files[0]) s_picked = files[0];
}

void pickDisc() {
    {
        std::lock_guard g(s_pickLock);
        if (s_picking) return;
        s_picking = true;
    }
    static const SDL_DialogFileFilter kFilters[] = {{"GameCube disc image", "iso;gcm;ciso;rvz;wia"}, {"All files", "*"}};
    SDL_ShowOpenFileDialog(onPicked, nullptr, aurora::window::get_sdl_window(), kFilters, 2, nullptr, false);
}

std::string fmtTime(float s) {
    char b[32];
    const int t = int(s + 0.5f);
    if (t >= 60) std::snprintf(b, sizeof b, "%d min %02d s", t / 60, t % 60);
    else std::snprintf(b, sizeof b, "%d s", t);
    return b;
}

// Lines of `s` that fit `width` in that font.
std::vector<std::string> wrap(ImFont* f, float size, const std::string& s, float width) {
    std::vector<std::string> lines;
    const char* p = s.c_str();
    const char* e = p + s.size();
    const float scale = size / F(f)->FontSize;
    while (p < e) {
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', size_t(e - p)));
        const char* end = nl ? nl : e;
        const char* q = F(f)->CalcWordWrapPositionA(scale, p, end, width);
        if (q <= p) q = p + 1;
        lines.emplace_back(p, q);
        p = q;
        while (p < e && *p == ' ') p++;
        if (p == nl) p++;
    }
    return lines;
}

// A button: a leaning capsule, orange when it is the one Enter presses. True when clicked.
bool button(ImDrawList* dl, float x, float cy, const char* label, bool primary, const Mouse& m, float& width) {
    const float size = U(19), h = U(44);
    width = measure(g_font.bold, size, label).x + U(56);
    const ImVec2 a(x, cy - h * 0.5f), b(x + width, cy + h * 0.5f);
    const bool hot = m.in(a, b);
    capsule(dl, a.x - U(3), a.y - U(3), b.x + U(3), b.y + U(3), 0.18f, true, col(kInk));
    capsule(dl, a.x, a.y, b.x, b.y, 0.18f, true, col(primary ? (hot ? kOrangeLit : kOrange) : (hot ? kButtonLit : kButton)));
    dl->AddCircleFilled(ImVec2(a.x + h * 0.42f, cy), h * 0.2f, col(primary ? kBallLit : kBall), 20);
    text(dl, g_font.bold, size, a.x + h * 0.42f + U(20), cy, col(kInk), label);
    return hot && m.clicked;
}

}  // namespace

// Shows the screen until the game can start: false if the player closes the window. `disc` is the disc image
// (empty: not chosen yet; the choice is written back), `error` why the game could not start (the module would not
// load) - the screen then offers to build it again.
bool setupScreen(const fs::path& exeDir, std::string& disc, bool eightRiders, std::string error) {
    enum class Page { Disc, Building, Failed } page = Page::Disc;
    Setup setup;
    std::string failure = error, note;
    auto begin = [&] {
        std::error_code ec;
        if (disc.empty() || !fs::exists(fs::u8path(disc), ec)) {
            page = Page::Disc;
            return;
        }
        if (!error.empty() || Setup::needed(exeDir, fs::u8path(disc), eightRiders)) {
            error.clear();
            setup.start(exeDir, fs::u8path(disc), eightRiders);
            page = Page::Building;
        } else {
            page = Page::Building;  // (nothing to build: the next frame finds it done)
        }
    };
    if (!error.empty()) page = Page::Failed;
    else begin();
    bool started = page == Page::Building && setup.running();
    for (;;) {
        bool enter = false, escape = false, quit = false;
        for (const AuroraEvent* ev = aurora_update(); ev && ev->type != AURORA_NONE; ++ev) {
            if (ev->type == AURORA_EXIT) {
                setup.cancel();
                return false;
            }
            if (ev->type != AURORA_SDL_EVENT) continue;
            const SDL_Event& e = ev->sdl;
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
                if (e.key.scancode == SDL_SCANCODE_RETURN || e.key.scancode == SDL_SCANCODE_KP_ENTER) enter = true;
                else if (e.key.scancode == SDL_SCANCODE_ESCAPE) escape = true;
            } else if (e.type == SDL_EVENT_DROP_FILE && e.drop.data && page != Page::Building) {
                std::lock_guard g(s_pickLock);
                s_picked = e.drop.data;
            }
        }
        {  // a disc image chosen in the dialog or dropped on the window
            std::string picked;
            {
                std::lock_guard g(s_pickLock);
                picked.swap(s_picked);
            }
            if (!picked.empty() && page != Page::Building) {
                disc = picked;
                failure.clear();
                log("[setup] disc image: %s\n", disc.c_str());
                begin();
                started = page == Page::Building;
            }
        }
        const Setup::Status st = setup.status();
        if (page == Page::Building && !setup.running()) {
            if (st.failed || st.cancelled) {
                page = Page::Failed;
                failure = st.failed ? st.error : "The setup was stopped.";
            } else {
                return true;  // built (or nothing to build): the game starts
            }
        }
        if (!aurora_begin_frame()) continue;
        ImGuiIO& io = ImGui::GetIO();
        const float W = io.DisplaySize.x, H = io.DisplaySize.y;
        g_s = std::max(0.5f, std::min(W / 1280.f, H / 720.f));
        g_alpha = 1.f;
        const float now = float(ImGui::GetTime());
        Mouse m{io.MousePos, false, io.MouseClicked[0], io.MouseDown[0], io.MouseReleased[0], 0.f};
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        std::vector<ImVec2> pts;

        // the frame, the bar along the top
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), col(kPlum));
        const float sx0 = std::floor((W - std::min(W - U(48), U(1200))) * 0.5f), sx1 = W - sx0;
        const float barCy = U(34);
        star(dl, ImVec2(sx0 + U(42), barCy), U(14), U(6.5f), -0.2f + std::sin(now * 1.5f) * 0.15f, col(0xFFD84A), col(kInk), U(2.5f));
        {
            float x = sx0 + U(64);
            x += text(dl, g_font.label, U(25), x, barCy - U(1), col(kCream), "Kirby Air Ride");
            text(dl, g_font.text, U(17), x + U(10), barCy + U(2), col(kOrange), "Recompiled");
        }
        // the field
        const ModeColour mc = page == Page::Failed ? kAirRide : kSelectMode;
        const ImVec2 fa(sx0 + U(16), U(70)), fb(sx1 - U(16), H - U(30));
        const float rTL = U(26), rTR = U(26), rBR = U(96), rBL = U(26);
        roundedRect(pts, fa, fb, rTL, rTR, rBR, rBL);
        dl->AddConvexPolyFilled(pts.data(), int(pts.size()), col(mc.field));
        dl->PushClipRect(fa, fb, true);
        {
            const u32 light = mixRgb(mc.field, kWhite, 0.35f);
            const float spin = now * 0.08f;
            const ImVec2 rc(fb.x - U(170), fb.y + U(40));
            dl->AddCircle(rc, U(250), col(light, 0.33f), 72, U(34));
            dl->AddCircle(rc, U(165), col(light, 0.22f), 64, U(12));
            for (int i = 0; i < 12; i++) {
                const float a = spin + float(i) * kPi / 6.f;
                dl->AddCircleFilled(ImVec2(rc.x + std::cos(a) * U(300), rc.y + std::sin(a) * U(300)), U(20), col(light, 0.24f), 20);
            }
            for (int i = 0; i < 22; i++) {
                const float x = fa.x + std::fmod(float(i) * U(173.3f) + U(40), fb.x - fa.x);
                const float y = fa.y + std::fmod(float(i * i) * U(41.7f) + U(30), fb.y - fa.y);
                const float r = U(5 + (i % 3) * 3);
                const ImVec2 q[4] = {ImVec2(x, y - r), ImVec2(x + r, y), ImVec2(x, y + r), ImVec2(x - r, y)};
                dl->AddConvexPolyFilled(q, 4, col(light, 0.28f));
            }
        }
        dl->PopClipRect();
        maskCorners(dl, fa, fb, rTL, rTR, rBR, rBL, col(kPlum));
        // the title on its line
        const char* title = page == Page::Disc ? "Your Disc" : page == Page::Building ? "Building the Game" : "Setup Stopped";
        {
            const float size = U(26), ty = fa.y + U(36);
            const float tw = text(dl, g_font.bold, size, fa.x + U(48), ty, col(kTitleInk), title);
            dl->AddLine(ImVec2(fa.x + U(4), ty + U(18)), ImVec2(fa.x + U(48) + tw + U(14), ty + U(18)), col(kTitleInk, 0.85f), U(3));
            dl->AddLine(ImVec2(fa.x + U(48) + tw + U(14), ty + U(18)), ImVec2(fa.x + U(48) + tw + U(30), ty), col(kTitleInk, 0.85f), U(3));
            dl->AddLine(ImVec2(fa.x + U(48) + tw + U(30), ty), ImVec2(fb.x - U(30), ty), col(kTitleInk, 0.85f), U(3));
        }
        const float x0 = fa.x + U(56), textW = std::min(fb.x - x0 - U(80), U(900));
        float y = fa.y + U(96);
        auto para = [&](const std::string& s, float size, u32 ink, ImFont* f) {
            for (const std::string& l : wrap(f, size, s, textW)) {
                text(dl, f, size, x0, y, col(ink), l.c_str());
                y += size * 1.35f;
            }
        };
        float bw = 0;
        const float by = fb.y - U(70);
        if (page == Page::Disc) {
            para("Kirby Air Ride: Recompiled plays from your own copy of the game. Choose your Kirby Air Ride (USA) disc image - "
                 ".iso, .gcm, .ciso, .rvz or .wia - or drop it on this window.",
                 U(22), kInk, g_font.text);
            y += U(10);
            para("The first time, the game is built from the program on your disc: this takes a few minutes, once. Nothing of the "
                 "game is downloaded.",
                 U(20), 0x23324A, g_font.text);
            bool picking;
            {
                std::lock_guard g(s_pickLock);
                picking = s_picking;
            }
            if (button(dl, x0, by, picking ? "Choosing..." : "Choose Disc Image", true, m, bw) || enter) pickDisc();
            float qw = 0;
            if (button(dl, x0 + bw + U(24), by, "Quit", false, m, qw) || escape) quit = true;
        } else if (page == Page::Building) {
            para(eightRiders ? "The 8-rider build is being made from the program on your disc. This happens once; the game starts when it is done."
                             : "The game is being made from the program on your disc. This happens once; the game starts when it is done.",
                 U(21), kInk, g_font.text);
            y += U(16);
            for (int s = 0; s < Setup::kSteps; s++) {  // the steps, the one under way lit
                const bool doneStep = s < st.step || st.done, cur = s == st.step && !st.done;
                const float h = U(38), cy = y + h * 0.5f;
                const float w = U(440);
                capsule(dl, x0 - U(3), cy - h * 0.5f - U(3), x0 + w + U(3), cy + h * 0.5f + U(3), 0.18f, true, col(kInk, cur ? 1.f : 0.5f));
                capsule(dl, x0, cy - h * 0.5f, x0 + w, cy + h * 0.5f, 0.18f, true, col(cur ? kOrange : doneStep ? kButtonLit : kButton, cur ? 1.f : 0.85f));
                dl->AddCircleFilled(ImVec2(x0 + h * 0.42f, cy), h * 0.2f, col(cur || doneStep ? kBallLit : kBall), 20);
                text(dl, g_font.bold, U(18), x0 + h * 0.42f + U(20), cy, col(kInk, doneStep || cur ? 1.f : 0.6f), Setup::stepName(s));
                if (doneStep) text(dl, g_font.bold, U(18), x0 + w - U(64), cy, col(0x1E5E2A), "Done");
                else if (cur && st.stepFrac > 0) {
                    char pc[16];
                    std::snprintf(pc, sizeof pc, "%d%%", int(st.stepFrac * 100.f));
                    text(dl, g_font.bold, U(18), x0 + w - U(64), cy, col(kInk), pc);
                }
                if (cur && !st.detail.empty()) text(dl, g_font.text, U(18), x0 + w + U(28), cy, col(kInk), st.detail.c_str());
                y += h + U(12);
            }
            // the whole build's bar
            y += U(16);
            const float bh = U(30), bx1 = x0 + std::min(textW, U(820));
            capsule(dl, x0 - U(3), y - U(3), bx1 + U(3), y + bh + U(3), 0.f, true, col(kInk));
            capsule(dl, x0, y, bx1, y + bh, 0.f, true, col(0x3A3A3A));
            const float fill = x0 + (bx1 - x0) * glide(&setup, st.overall, 6.f);
            if (fill > x0 + bh) capsule(dl, x0, y, fill, y + bh, 0.f, true, col(kOrange));
            y += bh + U(26);
            std::string when = fmtTime(st.elapsed);
            if (st.eta >= 0) when += "  -  about " + fmtTime(st.eta) + " left";
            text(dl, g_font.text, U(19), x0, y, col(kInk), when.c_str());
            if (button(dl, x0, by, "Cancel", false, m, bw) || escape) setup.cancel();
        } else {
            para(failure, U(21), kInk, g_font.text);
            y += U(10);
            if (started) para("What happened is in setup.log, beside the program.", U(19), 0x4A1E1E, g_font.text);
            const bool haveDisc = !disc.empty();
            float x = x0;
            if (haveDisc) {
                if (button(dl, x, by, "Try Again", true, m, bw) || enter) {
                    failure.clear();
                    setup.start(exeDir, fs::u8path(disc), eightRiders);
                    page = Page::Building;
                    started = true;
                }
                x += bw + U(24);
            }
            if (button(dl, x, by, "Choose Another Disc", !haveDisc, m, bw) || (!haveDisc && enter)) pickDisc();
            x += bw + U(24);
            float qw = 0;
            if (button(dl, x, by, "Quit", false, m, qw) || escape) quit = true;
        }
        aurora_end_frame();
        if (quit) return false;
    }
}

}  // namespace rt

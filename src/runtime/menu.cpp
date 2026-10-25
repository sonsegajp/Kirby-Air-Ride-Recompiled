// The F1 menu: the player's settings over the game, in Kirby Air Ride's colours.
//
// Laid out the way Dusklight's menus are (TwilitRealm/dusklight, CC0): tabs in a bar along the top, the
// open tab's sections down the left, the open section's settings as rows to the right. Its look is the
// game's own menus, redrawn in flat colours with Dear ImGui's draw lists:
//   - a dark plum frame around a field in the open tab's mode colour (Air Ride red, Top Ride gold, City
//     Trial green, Select Mode blue), with the big faint rings and light streaks of the game's backdrops
//     and its title over a line that steps up behind it ("Select Mode", "City Trial");
//   - rows as the slanted grey buttons of the mode menus, each with its ball at the left; the one in focus
//     orange on a maroon band, like "Start Game", with corner marks around it;
//   - sections as the outlined capsules of "Game Settings" / "Records";
//   - a rounded band along the bottom telling what the setting in focus does, as the game describes the
//     mode in focus ("Use Copy abilities as you race Air Ride machines!");
//   - a spinning disc at the right, as the game shows the chosen mode's emblem.
// It slides down from the top when F1 opens it (settling with a little overshoot) and dims the game; it
// slides back up when closed. Keyboard, mouse and controllers all steer it; the game sees no input while
// it is open (it keeps running: nothing here pauses it).
//
// Test runs (headless draws nothing over the game otherwise): SA2_MENU_OPEN=<tab>,<section>,<column>,<row>
// opens it on that page, SA2_MENU_AT=<n> frames in (1); SA2_MENU_TOUR=<frames> then visits every page in
// turn, and closes and reopens it at the end. SA2_MENU_SHOT=<dir> writes what the menu drew every
// SA2_MENU_SHOT_EVERY (30) frames from SA2_MENU_SHOT_FROM (SA2_MENU_AT) as a transparent .rgba, to lay over
// the SA2_DUMP_DIR dumps, which hold only the game's picture (ImGui is drawn after them, to the window).
#include "runtime/menu.h"
#include "runtime/menustyle.h"
#include "runtime/runtime.h"
#include "runtime/settings.h"

#include <aurora/aurora.h>
#include <dolphin/pad.h>
#include <imgui.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_video.h>

#include "window.hpp"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace rt::menu {

namespace {

using namespace rt::menustyle;  // the look: colours, shapes, fonts (src/runtime/menustyle.h)

// ---------------------------------------------------------------------------------------------------
// What the menu holds

enum class Kind : u8 {
    Toggle,  // On / Off
    Choice,  // < value >  (the frame rate limit)
    Slider,  // 0-100 %
    Keys,    // keycaps ("Q|E"), for reading only
    Text,    // a value, for reading only
};

struct Row {
    const char* label;
    const char* help;
    Kind kind;
    std::atomic<bool>* flag = nullptr;   // Toggle
    std::atomic<int>* number = nullptr;  // Choice
    std::atomic<float>* amount = nullptr;  // Slider
    const char* value = nullptr;         // Keys, Text
    const char* const* names = nullptr;  // Choice: its values' names, by number (none: Frame Rate Limit's)
    int nameCount = 0;
};

struct Section {
    const char* name;
    const char* help;
    std::vector<Row> rows;
};

struct Tab {
    const char* name;
    ModeColour colour;
    std::vector<Section> sections;
};

const int kLimits[] = {0, 60, 120, 144, 165, 240};  // Frame Rate Limit's choices (0: the display's rate)

const std::vector<Tab>& tabs() {
    using namespace settings;
    static const std::vector<Tab> s_tabs = {
        {"Graphics",
         kAirRide,
         {
             {"Display",
              "The window: how much of the screen it fills, and the picture's shape.",
              {
                  {"Fullscreen", "Fill the whole screen. F11 switches it at any time, menu or not.", Kind::Toggle, &fullscreen},
                  {"Widescreen", "Show more of each course at the sides of a wide window. Off: the original 4:3 picture.",
                   Kind::Toggle, &widescreen},
              }},
             {"Frame Rate",
              "How smoothly the picture moves. The game itself always runs at 60 frames a second.",
              {
                  {"Interpolation", "Draw in-between frames on displays faster than 60 Hz, for smoother motion.", Kind::Toggle,
                   &interpolation},
                  {"Frame Rate Limit", "The most frames drawn each second with Interpolation on. Display: your display's own rate.",
                   Kind::Choice, nullptr, &frameLimit},
              }},
             {"Overlay",
              "Small things drawn over the game.",
              {
                  {"Show FPS", "Count the frames drawn each second, in the top-left corner.", Kind::Toggle, &showFps},
                  {"F1 Hint", "Show the \"F1: Menu\" reminder for a few seconds when the game starts.", Kind::Toggle, &menuHint},
              }},
         }},
        {"Audio",
         kTopRide,
         {
             {"Volume",
              "How the game sounds.",
              {
                  {"Master Volume", "How loud everything plays. Left and right turn it down and up.", Kind::Slider, nullptr, nullptr,
                   &masterVolume},
                  {"Mute in Background", "Go silent while another window is in front of the game.", Kind::Toggle, &muteUnfocused},
              }},
         }},
        {"Controls",
         kCityTrial,
         {
             {"Keyboard",
              "The keyboard stands in for controller 1; a controller works alongside it.",
              {
                  {"A Button", "Boost-charge, inhale, choose.", Kind::Keys, nullptr, nullptr, nullptr, "Space"},
                  {"B Button", "Go back in the game's menus.", Kind::Keys, nullptr, nullptr, nullptr, "Shift"},
                  {"X / Y Buttons", "The X and Y buttons.", Kind::Keys, nullptr, nullptr, nullptr, "F|C"},
                  {"L / R Buttons", "The shoulder buttons.", Kind::Keys, nullptr, nullptr, nullptr, "Q|E"},
                  {"Z Button", "The Z button.", Kind::Keys, nullptr, nullptr, nullptr, "Tab"},
                  {"Start", "Pause, and start a race.", Kind::Keys, nullptr, nullptr, nullptr, "Enter"},
              }},
             {"Sticks",
              "The sticks and the + Control Pad on the keyboard.",
              {
                  {"Control Stick", "Steer your Air Ride machine.", Kind::Keys, nullptr, nullptr, nullptr, "W|A|S|D"},
                  {"C-Stick", "The C-Stick.", Kind::Keys, nullptr, nullptr, nullptr, "I|J|K|L"},
                  {"+ Control Pad", "The + Control Pad.", Kind::Keys, nullptr, nullptr, nullptr, "Arrows"},
              }},
             {"Controller",
              "A controller plugged in plays as controller 1 alongside the keyboard; more play as 2 to 4.",
              {
                  {"Control Stick", "Steer your Air Ride machine.", Kind::Text, nullptr, nullptr, nullptr, "Left stick"},
                  {"C-Stick", "The C-Stick.", Kind::Text, nullptr, nullptr, nullptr, "Right stick"},
                  {"L / R Buttons", "The GameCube controller's analog shoulder buttons.", Kind::Text, nullptr, nullptr, nullptr,
                   "Triggers"},
                  {"Z Button", "The Z button.", Kind::Text, nullptr, nullptr, nullptr, "Right shoulder"},
                  {"A B X Y", "Placed as your controller's own layout has them.", Kind::Text, nullptr, nullptr, nullptr,
                   "Face buttons"},
              }},
             {"This Menu",
              "Steering this menu: the game sees nothing you press while it is open.",
              {
                  {"Open / Close", "F1 opens and closes this menu at any time.", Kind::Keys, nullptr, nullptr, nullptr, "F1"},
                  {"Move", "Up and down move between settings; left and right change one.", Kind::Keys, nullptr, nullptr, nullptr,
                   "Arrows|WASD"},
                  {"Choose", "Choose, or switch a setting. A on a controller.", Kind::Keys, nullptr, nullptr, nullptr, "Space|Enter"},
                  {"Back", "Back to the sections, then out of the menu. B on a controller.", Kind::Keys, nullptr, nullptr, nullptr,
                   "Shift|Esc"},
                  {"Tabs", "The next and the previous tab. L and R on a controller.", Kind::Keys, nullptr, nullptr, nullptr, "Q|E"},
              }},
         }},
        {"About",
         kSelectMode,
         {
             {"Project",
              "Kirby Air Ride: Recompiled.",
              {
                  {"Kirby Air Ride", "A PC port made by recompiling the GameCube game.", Kind::Text, nullptr, nullptr, nullptr,
                   "Recompiled"},
                  {"Game Data", "Nothing of the game comes with the port: it is read from your own disc image.", Kind::Text, nullptr,
                   nullptr, nullptr, "Your own disc"},
                  {"Settings File", "Every change here is saved at once, beside the program.", Kind::Text, nullptr, nullptr, nullptr,
                   "kirbyairride-settings.ini"},
              }},
             {"Licence",
              "Under what terms the port comes.",
              {
                  {"This Port", "Free to use, change and share, but not for commercial purposes. See LICENSE.md.", Kind::Text,
                   nullptr, nullptr, nullptr, "PolyForm Noncommercial 1.0.0"},
                  {"The Game", "Kirby Air Ride is HAL Laboratory's and Nintendo's.", Kind::Text, nullptr, nullptr, nullptr,
                   "\xC2\xA9 Nintendo / HAL Laboratory"},  // (UTF-8 for the copyright sign)
                  {"Affiliation", "An unofficial fan project, not endorsed by Nintendo or HAL Laboratory.", Kind::Text, nullptr, nullptr,
                   nullptr, "None"},
              }},
             {"Thanks",
              "The open-source work the port is built on.",
              {
                  {"Aurora", "Draws the game's GameCube graphics with WebGPU.", Kind::Text, nullptr, nullptr, nullptr, "MIT"},
                  {"Dawn", "Google's WebGPU, under Aurora.", Kind::Text, nullptr, nullptr, nullptr, "BSD 3-Clause"},
                  {"SDL 3", "The window, sound and controllers.", Kind::Text, nullptr, nullptr, nullptr, "zlib"},
                  {"Dear ImGui", "Draws this menu.", Kind::Text, nullptr, nullptr, nullptr, "MIT"},
              }},
         }},
    };
    return s_tabs;
}

// ---------------------------------------------------------------------------------------------------
// The menu's state (all on the frontend's thread)

enum Nav : u32 {
    kUp = 1,
    kDown = 2,
    kLeft = 4,
    kRight = 8,
    kOk = 16,
    kBack = 32,
    kPrevTab = 64,
    kNextTab = 128,
};

struct Ui {
    bool open = false;
    double openedAt = -100, closedAt = -100;  // (ImGui time; -1 until the first frame: see overlay)
    bool openPending = false, closePending = false;
    int tab = 0;
    int section[8] = {};
    int column = 0;  // 0: the sections, 1: the rows
    int row = 0;
    int top = 0;     // the first row shown (a long section scrolls)
    u32 nav = 0;     // what the controllers pressed since the last frame
    u32 padMask = 0; // buttons held as the menu closed: kept from the game until let go
    double pageAt = -100;  // when the page shown last changed (it slides in)
    int pageKey = -1;
    ModeColour fieldFrom = kAirRide;  // the field's colour fades to a new tab's
    double tabAt = -100;
    float focusY = -1;  // the maroon band's place (it glides between rows)
    const void* drag = nullptr;  // the slider held by the mouse
};
Ui g_ui;

// ---------------------------------------------------------------------------------------------------
// Changing settings

double displayHz() {
    if (SDL_Window* w = aurora::window::get_sdl_window())
        if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w))) return m->refresh_rate;
    return 0;
}

int limitIndex() {
    const int v = settings::frameLimit;
    int best = 0;
    for (int i = 0; i < int(std::size(kLimits)); i++)
        if (std::abs(kLimits[i] - v) < std::abs(kLimits[best] - v)) best = i;
    return best;
}

// Steps a row's value (dir -1 / +1; 0: as choosing it does). Returns whether it changed.
bool step(const Row& r, int dir) {
    switch (r.kind) {
    case Kind::Toggle: r.flag->store(!r.flag->load()); break;
    case Kind::Choice: {
        if (r.names) {
            const int n = r.nameCount, i = std::clamp(r.number->load(), 0, n - 1);
            r.number->store(dir < 0 ? (i + n - 1) % n : (i + 1) % n);
            break;
        }
        const int n = int(std::size(kLimits));
        const int i = limitIndex();
        const int to = dir < 0 ? (i + n - 1) % n : (i + 1) % n;
        r.number->store(kLimits[to]);
        break;
    }
    case Kind::Slider: {
        if (!dir) return false;
        const float v = std::round(r.amount->load() * 20.f) / 20.f;
        const float to = std::clamp(v + 0.05f * float(dir), 0.f, 1.f);
        if (to == r.amount->load()) return false;
        r.amount->store(to);
        break;
    }
    default: return false;
    }
    settings::save();
    return true;
}

void openMenu() {
    if (g_ui.open) return;
    g_ui.openPending = true;
}
void closeMenu() {
    if (!g_ui.open) return;
    g_ui.closePending = true;
}

// ---------------------------------------------------------------------------------------------------
// Test runs

struct TourStop {
    int tab, section, column, row;
};
std::vector<TourStop> tourStops() {
    std::vector<TourStop> v;
    for (int t = 0; t < int(tabs().size()); t++)
        for (int s = 0; s < int(tabs()[t].sections.size()); s++) v.push_back({t, s, 1, s % 2});
    v.push_back({0, 1, 0, 0});
    return v;
}

void goTo(int tab, int section, int column, int row) {
    const auto& T = tabs();
    g_ui.tab = std::clamp(tab, 0, int(T.size()) - 1);
    g_ui.section[g_ui.tab] = std::clamp(section, 0, int(T[g_ui.tab].sections.size()) - 1);
    g_ui.column = column ? 1 : 0;
    g_ui.row = std::clamp(row, 0, int(T[g_ui.tab].sections[g_ui.section[g_ui.tab]].rows.size()) - 1);
}

// ---------------------------------------------------------------------------------------------------
// Drawing the menu

void drawMenu(ImDrawList* dl, float appear, bool live, Mouse m) {
    ImGuiIO& io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    const float now = float(ImGui::GetTime());
    const auto& T = tabs();
    const Tab& tab = T[g_ui.tab];
    const int secIdx = g_ui.section[g_ui.tab];
    const Section& sec = tab.sections[secIdx];
    u32 nav = live ? g_ui.nav : 0;

    // The game dimmed behind.
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), col(0x0E0B14, 0.58f * clamp01(appear)));

    // The sheet, hanging from the top of the screen.
    const float sheetW = std::min(W - U(48), U(1200)), sheetH = std::min(H - U(30), U(612));
    const float sx0 = std::floor((W - sheetW) * 0.5f), sx1 = sx0 + sheetW;
    const float dy = -(1.f - appear) * (sheetH + U(40));
    const float sy1 = sheetH + dy;
    std::vector<ImVec2> pts;
    // its drop shadow, then the frame
    roundedRect(pts, ImVec2(sx0 + U(6), -U(80) + dy), ImVec2(sx1 + U(6), sy1 + U(8)), 0, 0, U(34), U(34));
    dl->AddConvexPolyFilled(pts.data(), int(pts.size()), col(0x000000, 0.35f));
    roundedRect(pts, ImVec2(sx0 - U(3), -U(80) + dy), ImVec2(sx1 + U(3), sy1 + U(3)), 0, 0, U(33), U(33));
    dl->AddConvexPolyFilled(pts.data(), int(pts.size()), col(kPlumLight));
    roundedRect(pts, ImVec2(sx0, -U(80) + dy), ImVec2(sx1, sy1), 0, 0, U(30), U(30));
    dl->AddConvexPolyFilled(pts.data(), int(pts.size()), col(kPlum));

    // ---- the bar along the top: the name, the tabs, closing ----
    const float barCy = U(34) + dy;
    {
        star(dl, ImVec2(sx0 + U(42), barCy), U(14), U(6.5f), -0.2f, col(0xFFD84A), col(kInk), U(2.5f));
        float x = sx0 + U(64);
        x += text(dl, g_font.label, U(25), x, barCy - U(1), col(kCream), "Kirby Air Ride");
        text(dl, g_font.text, U(17), x + U(10), barCy + U(2), col(kOrange), "Recompiled");
    }
    float tx = sx0 + U(362);
    for (int i = 0; i < int(T.size()); i++) {
        const float size = U(19), w = gameMeasure(g_font.bold, size, T[i].name) + U(46);
        const ImVec2 a(tx, barCy - U(18)), b(tx + w, barCy + U(18));
        const bool on = i == g_ui.tab;
        const bool hot = live && m.in(ImVec2(a.x - U(10), a.y), b);
        if (hot && m.clicked && !on) {
            g_ui.tab = i, g_ui.row = 0, g_ui.top = 0;
        }
        const float lit = glide(&T[i], on ? 1.f : 0.f), warm = glide(&T[i].sections, hot && !on ? 1.f : 0.f);
        // the chosen one: in its mode's colour, outlined in white, over a deeper tail (the game's "Options" capsule)
        if (lit > 0.01f) {
            capsule(dl, a.x - U(14), a.y + U(4), b.x - U(8), b.y + U(4), 0.f, true, col(T[i].colour.deep, lit));
            capsule(dl, a.x - U(2.5f), a.y - U(2.5f), b.x + U(2.5f), b.y + U(2.5f), 0.f, true, col(kWhite, lit));
        }
        capsule(dl, a.x, a.y, b.x, b.y, 0.f, true, col(mixRgb(mixRgb(kSlate, 0x5E6878, warm), T[i].colour.field, lit)));
        gameText(dl, g_font.bold, size, a.x + U(23), barCy, col(mixRgb(mixRgb(kSlateText, kWhite, warm), 0x1E1E1E, lit)), T[i].name);
        tx = b.x + U(14);
    }
    {  // the way out, at the right: [F1] Close
        const float size = U(17);
        const float w = measure(g_font.bold, size, "Close").x;
        const float right = sx1 - U(28);
        const ImVec2 a(right - w - U(48), barCy - U(18)), b(right + U(6), barCy + U(18));
        const bool hot = live && m.in(a, b);
        if (hot && m.clicked) closeMenu();
        text(dl, g_font.bold, size, right - w, barCy, col(hot ? kWhite : kSlateText), "Close");
        keycap(dl, right - w - U(10), barCy, "F1", true);
    }

    // ---- the field, in the tab's mode colour ----
    const ImVec2 fa(sx0 + U(16), U(70) + dy), fb(sx1 - U(16), sy1 - U(66));
    const float rTL = U(26), rTR = U(26), rBR = U(96), rBL = U(26);
    const float fade = easeOut((now - float(g_ui.tabAt)) / 0.3f);
    const u32 field = mixRgb(g_ui.fieldFrom.field, tab.colour.field, fade);
    const u32 deep = mixRgb(g_ui.fieldFrom.deep, tab.colour.deep, fade);
    roundedRect(pts, fa, fb, rTL, rTR, rBR, rBL);
    dl->AddConvexPolyFilled(pts.data(), int(pts.size()), col(field));
    // the backdrop: big faint rings, light streaks from the top, a scatter of diamonds
    dl->PushClipRect(fa, fb, true);
    {
        const u32 light = mixRgb(field, kWhite, 0.35f);
        const ImU32 faint = col(light, 0.33f);
        const float spin = now * 0.08f;
        // the game's toothed ring, turning slowly (drawn in its place until the game's art has loaded)
        if (!artRing(dl, gameart::Art::Gear, ImVec2(fa.x + U(150), fb.y + U(30)), U(330), col(light, 0.3f), spin)) {
            dl->AddCircle(ImVec2(fa.x + U(150), fb.y + U(30)), U(250), faint, 72, U(34));
            dl->AddCircle(ImVec2(fa.x + U(150), fb.y + U(30)), U(165), col(light, 0.22f), 64, U(12));
            for (int i = 0; i < 12; i++) {  // the ring's teeth
                const float a = spin + float(i) * kPi / 6.f;
                const ImVec2 c(fa.x + U(150) + std::cos(a) * U(300), fb.y + U(30) + std::sin(a) * U(300));
                dl->AddCircleFilled(c, U(20), col(light, 0.24f), 20);
            }
        }
        dl->AddCircle(ImVec2(fb.x - U(260), fa.y + U(170)), U(330), col(light, 0.18f), 96, U(22));
        for (int i = 0; i < 3; i++) {  // streaks of light across the top
            const float x = fa.x + U(420) + float(i) * U(150);
            const ImVec2 q[4] = {ImVec2(x, fa.y), ImVec2(x + U(70 + 30 * i), fa.y), ImVec2(x - U(150 - 40 * i), fa.y + U(150)),
                                 ImVec2(x - U(190 - 20 * i), fa.y + U(150))};
            dl->AddConvexPolyFilled(q, 4, col(kWhite, 0.07f));
        }
        for (int i = 0; i < 26; i++) {  // diamonds, as on the game's backdrops
            const float x = fa.x + std::fmod(float(i) * U(173.3f) + U(40), fb.x - fa.x);
            const float y = fa.y + std::fmod(float(i * i) * U(41.7f) + U(30), fb.y - fa.y);
            const float r = U(5 + (i % 3) * 3);
            const ImVec2 q[4] = {ImVec2(x, y - r), ImVec2(x + r, y), ImVec2(x, y + r), ImVec2(x - r, y)};
            dl->AddConvexPolyFilled(q, 4, col(light, 0.28f));
        }
    }
    {  // the tab's name, huge and faint low on the field, as the game sets its modes' logos
        const float size = U(112);
        const u32 light = mixRgb(field, kWhite, 0.3f);
        const ImVec2 at(fa.x + U(30), fb.y - U(98));
        for (int i = 0; i < 2; i++)
            dl->AddText(F(g_font.label), size, ImVec2(at.x + U(i ? 0 : 5), at.y - size * 0.5f + U(i ? 0 : 5)),
                        i ? col(light, 0.3f) : col(deep, 0.18f), tab.name);
    }
    dl->PopClipRect();
    maskCorners(dl, fa, fb, rTL, rTR, rBR, rBL, col(kPlum));

    // ---- the title, on its line that steps up behind it ----
    {
        const float size = U(23), ty = fa.y + U(30);
        const float tw = gameMeasure(g_font.bold, size, tab.name);
        const float x = fa.x + U(48);
        const float base = ty + U(15), high = ty - U(3), w = U(3);
        // (the game's step in the line, once its art has loaded)
        if (artTitleStep(dl, x + tw + U(10), base + w * 0.5f, high - w * 0.5f, col(kTitleInk, 0.85f))) {
            const float stepW = (base - high + w) / (16.f / 24.f) * 32.f / 24.f;
            dl->AddLine(ImVec2(fa.x + U(4), base), ImVec2(x + tw + U(11), base), col(kTitleInk, 0.85f), w);
            dl->AddLine(ImVec2(x + tw + U(9) + stepW, high), ImVec2(fb.x - U(230), high), col(kTitleInk, 0.85f), w);
        } else {
            const ImVec2 line[4] = {ImVec2(fa.x + U(4), base), ImVec2(x + tw + U(14), base), ImVec2(x + tw + U(34), high),
                                    ImVec2(fb.x - U(230), high)};
            dl->AddPolyline(line, 4, col(kTitleInk, 0.85f), ImDrawFlags_None, w);
        }
        dl->AddLine(ImVec2(fb.x - U(200), high), ImVec2(fb.x - U(40), high), col(kTitleInk, 0.85f), w);
        gameText(dl, g_font.bold, size, x, ty, col(kTitleInk), tab.name);
    }

    // ---- the disc at the right, with the mode's star, turning ----
    {
        const ImVec2 c(fb.x - U(100), fa.y + (fb.y - fa.y) * 0.47f);
        const float r = U(78);
        dl->AddCircleFilled(c, r + U(14), col(kPlum), 64);
        dl->AddCircleFilled(c, r + U(4), col(0x9A9AA2), 64);
        dl->AddCircleFilled(c, r, col(mixRgb(0xD8DCE2, field, 0.3f)), 64);
        // its grooves: the game's (in their place until its art has loaded, rings)
        if (!artRing(dl, gameart::Art::Grooves, c, r * 0.98f, col(0x55555E, 0.9f), now * 0.05f))
            for (int i = 1; i <= 3; i++) dl->AddCircle(c, r * (0.45f + 0.17f * float(i)), col(0xFFFFFF, 0.35f), 64, U(1.5f));
        dl->AddCircle(c, r * 0.36f, col(0x8A8A92, 0.7f), 48, U(2));
        star(dl, c, r * 0.68f, r * 0.31f, now * 0.35f, col(deep), col(kInk), U(4));
        star(dl, ImVec2(c.x - r * 0.04f, c.y - r * 0.05f), r * 0.42f, r * 0.19f, now * 0.35f, col(mixRgb(field, kWhite, 0.45f), 0.8f),
             col(0, 0), 0.f);
    }

    // ---- the sections, down the left ----
    const float listTop = fa.y + U(92);
    const float sideX0 = fa.x + U(34), sideX1 = fa.x + U(262), pillH = U(42), sidePitch = U(56);
    for (int i = 0; i < int(tab.sections.size()); i++) {
        const float y0 = listTop + float(i) * sidePitch, y1 = y0 + pillH;
        const bool chosen = i == secIdx, focus = chosen && g_ui.column == 0;
        const bool hot = live && m.in(ImVec2(sideX0 - U(12), y0), ImVec2(sideX1, y1));
        if (hot && m.clicked) {
            if (!chosen) g_ui.section[g_ui.tab] = i, g_ui.row = 0, g_ui.top = 0;
            g_ui.column = 0;
        }
        const float lit = glide(&tab.sections[i], focus ? 1.f : 0.f);
        const float pick = glide(&tab.sections[i].rows, chosen ? 1.f : hot ? 0.4f : 0.f);
        capsule(dl, sideX0 - U(12), y0 - U(3), sideX1 + U(3), y1 + U(3), 0.f, true, col(lit > 0.01f ? mixRgb(kInk, kWhite, lit) : kInk));
        capsule(dl, sideX0 - U(10) + U(3), y0, sideX1, y1, 0.f, true, col(kInk));
        const u32 inner = mixRgb(mixRgb(kButton, 0xF4F4F4, pick), kOrange, lit);
        capsule(dl, sideX0 + U(4), y0 + U(4), sideX1 - U(4), y1 - U(4), 0.f, true, col(inner));
        capsule(dl, sideX0 + U(14), y0 + U(6), sideX1 - U(18), y0 + U(16), 0.f, true,
                col(mixRgb(mixRgb(kButtonLit, kWhite, pick), kOrangeLit, lit), 0.55f));
        const float size = U(21);
        const float w = measure(g_font.label, size, tab.sections[i].name).x;
        text(dl, g_font.label, size, (sideX0 + sideX1) * 0.5f - w * 0.5f, (y0 + y1) * 0.5f, col(kInk), tab.sections[i].name);
        if (focus) cornerMarks(dl, ImVec2(sideX0 - U(12), y0 - U(3)), ImVec2(sideX1 + U(3), y1 + U(3)), now);
    }

    // ---- the rows of the section ----
    const float rx0 = fa.x + U(318), rx1 = fb.x - U(206);
    const float rowH = U(44), pitch = U(56);
    const float areaTop = listTop, areaBottom = fb.y - U(34);
    const int fits = std::max(1, int((areaBottom - areaTop + (pitch - rowH)) / pitch));
    const int count = int(sec.rows.size());
    g_ui.row = std::clamp(g_ui.row, 0, count - 1);
    // a new page slides in from the right
    const int key = g_ui.tab * 16 + secIdx;
    if (key != g_ui.pageKey) {
        g_ui.pageKey = key, g_ui.pageAt = now, g_ui.top = 0;
    }
    const float pageIn = easeOut((now - float(g_ui.pageAt)) / 0.22f);
    const float slide = (1.f - pageIn) * U(46);
    if (g_ui.column == 1) g_ui.top = std::clamp(g_ui.top, g_ui.row - fits + 1, g_ui.row);
    if (live && m.wheel != 0 && m.in(ImVec2(rx0, areaTop), ImVec2(rx1, areaBottom)))
        g_ui.top -= m.wheel > 0 ? 1 : -1;
    g_ui.top = std::clamp(g_ui.top, 0, std::max(0, count - fits));
    const float hz = sec.rows.empty() ? 0.f : float(displayHz());
    const Row* focusRow = g_ui.column == 1 ? &sec.rows[g_ui.row] : nullptr;
    g_alpha = pageIn;
    dl->PushClipRect(ImVec2(fa.x, areaTop - U(14)), ImVec2(fb.x, areaBottom + U(10)), true);
    {  // the maroon band behind the row in focus, gliding between rows
        const float target = areaTop + float(g_ui.row - g_ui.top) * pitch;
        if (g_ui.focusY < 0 || pageIn < 1.f) g_ui.focusY = target;
        g_ui.focusY += (target - g_ui.focusY) * (1.f - std::exp(-io.DeltaTime * 22.f));
        if (focusRow) {
            const float y0 = g_ui.focusY - U(6), y1 = g_ui.focusY + rowH + U(6);
            if (!artBand(dl, fa.x + U(272) + slide, y0 - U(4), rx1 + U(30) + slide, y1 + U(4))) {  // (the game's band, once loaded)
                capsule(dl, fa.x + U(272) + slide, y0 - U(2), rx1 + U(18) + slide, y1 + U(2), 0.35f, false, col(kMaroonRim));
                capsule(dl, fa.x + U(272) + slide, y0, rx1 + U(14) + slide, y1, 0.35f, false, col(kMaroon));
            }
        }
    }
    for (int i = g_ui.top; i < count && i < g_ui.top + fits + 1; i++) {
        const Row& r = sec.rows[i];
        const float y0 = areaTop + float(i - g_ui.top) * pitch, y1 = y0 + rowH, cy = (y0 + y1) * 0.5f;
        const float x0 = rx0 + slide, x1 = rx1 + slide;
        const bool focus = &r == focusRow;
        const bool hot = live && y1 <= areaBottom + U(4) && m.in(ImVec2(x0, y0), ImVec2(x1, y1));
        if (hot && (m.moved || m.clicked) && !g_ui.drag) g_ui.column = 1, g_ui.row = i;
        const float lit = glide(&r, focus ? 1.f : 0.f, 20.f);
        const bool dim = r.kind == Kind::Toggle && r.flag == &settings::interpolation && hz > 0 && hz <= 65.f;
        // the game's button, grey, orange in focus (until its art has loaded: drawn - outline, button, sheen, ball)
        if (!artButton(dl, x0 - U(22), y0 - U(5), x1 + U(8), y1 + U(5), mixRgb(hot ? 0xCCCCCC : kButton, kOrange, lit), 1.f, lit > 0.5f)) {
            if (lit > 0.01f) capsule(dl, x0 - U(5), y0 - U(5), x1 + U(5), y1 + U(5), 0.35f, false, col(kWhite, lit));
            capsule(dl, x0 - U(3), y0 - U(3), x1 + U(3), y1 + U(3), 0.35f, false, col(kInk));
            capsule(dl, x0, y0, x1, y1, 0.35f, false, col(mixRgb(hot ? 0xCCCCCC : kButton, kOrange, lit)));
            capsule(dl, x0 + U(22), y0 + U(4), x1 - U(30), y0 + U(15), 0.35f, false, col(mixRgb(kButtonLit, kOrangeLit, lit), 0.6f));
            const ImVec2 ball(x0 + U(4), cy);
            dl->AddCircleFilled(ball, U(17), col(kInk), 28);
            dl->AddCircleFilled(ball, U(13.5f), col(mixRgb(kBall, kBallLit, lit)), 28);
            dl->AddCircleFilled(ImVec2(ball.x - U(4), ball.y - U(4)), U(4.5f), col(kWhite, 0.25f + 0.5f * lit), 16);
        }
        text(dl, g_font.label, U(23), x0 + U(30), cy, col(dim ? 0x5A5A5A : kInk), r.label);
        // its value, at the right
        const float vx1 = x1 - U(34);
        switch (r.kind) {
        case Kind::Toggle: {
            const bool on = r.flag->load();
            const float k = glide(r.flag, on ? 1.f : 0.f, 18.f);
            const float w = U(76), h = U(28);
            const ImVec2 a(vx1 - w, cy - h * 0.5f), b(vx1, cy + h * 0.5f);
            dl->AddRectFilled(ImVec2(a.x - U(2.5f), a.y - U(2.5f)), ImVec2(b.x + U(2.5f), b.y + U(2.5f)), col(kInk), h);
            dl->AddRectFilled(a, b, col(mixRgb(0xE9E9E9, kPlum, k)), h);
            const float kx = a.x + h * 0.5f + (w - h) * k;
            const char* word = on ? "ON" : "OFF";
            const float ws = U(14);
            const float ww = gameMeasure(g_font.bold, ws, word);
            gameText(dl, g_font.bold, ws, on ? a.x + U(12) : b.x - U(12) - ww, cy, col(on ? kWhite : 0x555555), word);
            dl->AddCircleFilled(ImVec2(kx, cy), h * 0.5f + U(1), col(kInk), 24);
            dl->AddCircleFilled(ImVec2(kx, cy), h * 0.5f - U(2), col(on ? kOrange : 0xFAFAFA), 24);
            if (focus && on) dl->AddCircleFilled(ImVec2(kx, cy), h * 0.5f - U(2), col(kWhite), 24);
            if (hot && m.clicked) step(r, 0);
            break;
        }
        case Kind::Choice: {
            const int v = r.number->load();
            char buf[32];
            if (r.names) std::snprintf(buf, sizeof buf, "%s", r.names[std::clamp(v, 0, r.nameCount - 1)]);
            else if (v <= 0) std::snprintf(buf, sizeof buf, "Display");
            else std::snprintf(buf, sizeof buf, "%d FPS", v);
            const float span = U(190), ax0 = vx1 - U(8) - span;
            const float ts = U(19), tw = gameMeasure(g_font.bold, ts, buf);
            gameText(dl, g_font.bold, ts, ax0 + (span - tw) * 0.5f, cy, col(kInk), buf);
            const float t = U(8);
            const bool hl = hot && m.pos.x < ax0 + span * 0.5f;
            const ImU32 al = col(hl ? kWhite : kInk), ar = col(hot && !hl ? kWhite : kInk);
            dl->AddTriangleFilled(ImVec2(ax0 + U(4), cy), ImVec2(ax0 + U(4) + t * 1.4f, cy - t), ImVec2(ax0 + U(4) + t * 1.4f, cy + t), al);
            const float ax1 = ax0 + span;
            dl->AddTriangleFilled(ImVec2(ax1, cy), ImVec2(ax1 - t * 1.4f, cy + t), ImVec2(ax1 - t * 1.4f, cy - t), ar);
            if (hot && m.clicked) step(r, m.pos.x < ax0 + span * 0.5f ? -1 : 1);
            break;
        }
        case Kind::Slider: {
            const float v = r.amount->load();
            char buf[16];
            std::snprintf(buf, sizeof buf, "%d%%", int(std::round(v * 100.f)));
            const float ts = U(19);
            const float numW = measure(g_font.bold, ts, "100%").x;
            gameText(dl, g_font.bold, ts, vx1 - gameMeasure(g_font.bold, ts, buf), cy, col(kInk), buf);
            const float t0 = vx1 - numW - U(222), t1 = vx1 - numW - U(18);
            const float h = U(12);
            dl->AddRectFilled(ImVec2(t0 - U(2.5f), cy - h * 0.5f - U(2.5f)), ImVec2(t1 + U(2.5f), cy + h * 0.5f + U(2.5f)), col(kInk), h);
            dl->AddRectFilled(ImVec2(t0, cy - h * 0.5f), ImVec2(t1, cy + h * 0.5f), col(kPlum), h);
            const float shown = glide(r.amount, v, 24.f);
            const float kx = t0 + (t1 - t0) * shown;
            if (shown > 0.005f) dl->AddRectFilled(ImVec2(t0, cy - h * 0.5f), ImVec2(kx, cy + h * 0.5f), col(focus ? kWhite : kOrange), h);
            dl->AddCircleFilled(ImVec2(kx, cy), U(12), col(kInk), 24);
            dl->AddCircleFilled(ImVec2(kx, cy), U(9), col(focus ? kOrange : 0xFAFAFA), 24);
            const bool onTrack = hot && m.pos.x >= t0 - U(14) && m.pos.x <= t1 + U(14);
            if (onTrack && m.clicked) g_ui.drag = &r;
            if (g_ui.drag == &r && m.down) {
                const float to = std::round(clamp01((m.pos.x - t0) / (t1 - t0)) * 100.f) / 100.f;
                if (to != r.amount->load()) r.amount->store(to), settings::save();
            }
            break;
        }
        case Kind::Keys: {
            // "Q|E": a key each, the last at the right
            std::vector<std::string> keys;
            for (const char* p = r.value; *p;) {
                const char* e = std::strchr(p, '|');
                keys.emplace_back(p, e ? size_t(e - p) : std::strlen(p));
                if (!e) break;
                p = e + 1;
            }
            float right = vx1;
            for (int k = int(keys.size()) - 1; k >= 0; k--) right -= keycap(dl, right, cy, keys[k].c_str(), true) + U(10);
            break;
        }
        case Kind::Text: {
            const float ts = U(18);
            gameText(dl, g_font.bold, ts, vx1 - gameMeasure(g_font.bold, ts, r.value), cy, col(0x262626), r.value);
            break;
        }
        }
        if (focus && lit > 0.5f) cornerMarks(dl, ImVec2(x0 - U(8), y0 - U(5)), ImVec2(x1 + U(8), y1 + U(5)), now);
    }
    dl->PopClipRect();
    g_alpha = 1.f;
    // more rows below or above: small arrows at the list's foot and head
    if (g_ui.top > 0)
        dl->AddTriangleFilled(ImVec2((rx0 + rx1) * 0.5f, areaTop - U(18)), ImVec2((rx0 + rx1) * 0.5f - U(10), areaTop - U(8)),
                              ImVec2((rx0 + rx1) * 0.5f + U(10), areaTop - U(8)), col(kInk, 0.7f));
    if (g_ui.top + fits < count)
        dl->AddTriangleFilled(ImVec2((rx0 + rx1) * 0.5f, areaBottom + U(10)), ImVec2((rx0 + rx1) * 0.5f - U(10), areaBottom),
                              ImVec2((rx0 + rx1) * 0.5f + U(10), areaBottom), col(kInk, 0.7f));
    if (!m.down) g_ui.drag = nullptr;

    // ---- the description band ----
    {
        std::string help = focusRow ? focusRow->help : sec.help;
        if (focusRow && focusRow->flag == &settings::interpolation && hz > 0 && hz <= 65.f) {
            char buf[96];
            std::snprintf(buf, sizeof buf, " (This display runs at %.0f Hz.)", double(hz));
            help += buf;
        }
        if (focusRow && focusRow->number == &settings::frameLimit && !settings::interpolation) help += " (Interpolation is off.)";
        const float bx0 = fa.x + U(70), bx1 = fb.x - U(300);
        const float bcy = fb.y + U(2), bh = U(46);
        dl->AddRectFilled(ImVec2(bx0, bcy - bh * 0.5f), ImVec2(bx1, bcy + bh * 0.5f), col(kBand, 0.97f), bh * 0.5f);
        float ts = U(21);
        const float room = bx1 - bx0 - U(48);
        const float tw = gameMeasure(g_font.text, ts, help.c_str());
        if (tw > room) ts *= room / tw;
        gameText(dl, g_font.text, ts, bx0 + U(26), bcy, col(kBandText), help.c_str());
        // what the buttons do, to the right of it
        float hx = bx1 + U(24);
        const float hs = U(15);
        auto hint = [&](const char* button, u32 face, const char* what) {
            dl->AddCircleFilled(ImVec2(hx + U(11), bcy), U(11), col(face), 20);
            const float bw = measure(g_font.bold, U(13), button).x;
            text(dl, g_font.bold, U(13), hx + U(11) - bw * 0.5f, bcy, col(kWhite), button);
            hx += U(26);
            hx += gameText(dl, g_font.text, hs, hx, bcy, col(kSlateText), what) + U(16);
        };
        hint("A", 0x2FA37A, "Choose");
        hint("B", 0xD0453B, "Back");
        dl->AddCircleFilled(ImVec2(hx + U(11), bcy), U(11), col(0x6B7280), 20);
        text(dl, g_font.bold, U(13), hx + U(11) - measure(g_font.bold, U(13), "L").x * 0.5f, bcy, col(kWhite), "L");
        hx += U(25);
        hint("R", 0x6B7280, "Tabs");
    }

    // ---- what the controllers and keys pressed ----
    if (!live) return;
    const int nSec = int(tab.sections.size());
    auto changeTab = [&](int d) {
        g_ui.tab = (g_ui.tab + d + int(T.size())) % int(T.size());
        g_ui.row = 0, g_ui.top = 0;
    };
    if (nav & kPrevTab) changeTab(-1);
    if (nav & kNextTab) changeTab(1);
    if (nav & (kPrevTab | kNextTab)) nav = 0;
    if (g_ui.column == 0) {
        int& s = g_ui.section[g_ui.tab];
        if (nav & kUp) s = (s + nSec - 1) % nSec, g_ui.row = 0, g_ui.top = 0;
        if (nav & kDown) s = (s + 1) % nSec, g_ui.row = 0, g_ui.top = 0;
        if (nav & (kRight | kOk)) g_ui.column = 1, g_ui.row = 0;
        if (nav & kBack) closeMenu();
    } else {
        if (nav & kUp) g_ui.row = (g_ui.row + count - 1) % count;
        if (nav & kDown) g_ui.row = (g_ui.row + 1) % count;
        const Row& r = sec.rows[g_ui.row];
        const bool readOnly = r.kind == Kind::Keys || r.kind == Kind::Text;
        if (nav & kLeft) {
            if (readOnly) g_ui.column = 0;
            else step(r, -1);
        }
        if (nav & kRight) step(r, 1);
        if (nav & kOk) step(r, 0);
        if (nav & kBack) g_ui.column = 0;
    }
}

// "F1: Menu", for a few seconds after the game first shows.
void drawHint(ImDrawList* dl, float age) {
    constexpr float kShow = 6.f;
    if (age < 0 || age > kShow) return;
    const float a = clamp01(age / 0.35f) * clamp01((kShow - age) / 0.6f);
    const float W = ImGui::GetIO().DisplaySize.x;
    const float size = U(19);
    const float tw = measure(g_font.text, size, "Menu").x;
    const float w = U(28) + U(40) + tw + U(26), h = U(44);
    const float x1 = W - U(24) + (1.f - easeOut(age / 0.35f)) * U(60), y0 = U(24);
    const ImVec2 p0(x1 - w, y0), p1(x1, y0 + h);
    g_alpha = a;
    dl->AddRectFilled(ImVec2(p0.x - U(2), p0.y - U(2)), ImVec2(p1.x + U(2), p1.y + U(2)), col(kPlum, 0.9f), h);
    dl->AddRectFilled(p0, p1, col(kBand, 0.95f), h * 0.5f);
    const float cy = y0 + h * 0.5f;
    keycap(dl, p0.x + U(22) + keycap(dl, 0, 0, "F1", false), cy, "F1", true);
    text(dl, g_font.text, size, p0.x + U(22) + keycap(dl, 0, 0, "F1", false) + U(14), cy, col(kBandText), "Menu");
    g_alpha = 1.f;
}

void drawFps(ImDrawList* dl, float fps) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f FPS", double(fps));
    const float size = U(17);
    const float w = measure(g_font.bold, size, buf).x + U(26), h = U(30);
    const ImVec2 a(U(14), U(14)), b(a.x + w, a.y + h);
    dl->AddRectFilled(a, b, col(kPlum, 0.72f), h * 0.5f);
    text(dl, g_font.bold, size, a.x + U(13), a.y + h * 0.5f, col(kCream), buf);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------

void init() {
    if (!ImGui::GetCurrentContext()) return;
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();  // ImGui's own stays the default
    static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2013, 0x2014, 0x2018, 0x201D, 0x2022, 0x2022, 0x2026, 0x2026, 0};
    std::string fonts;
#ifdef _WIN32
    if (const char* windir = std::getenv("WINDIR")) fonts = std::string(windir) + "/Fonts/";
#endif
    float density = 1.f;
    if (SDL_Window* w = aurora::window::get_sdl_window()) density = std::clamp(SDL_GetWindowDisplayScale(w), 1.f, 3.f);
    auto load = [&](std::initializer_list<const char*> names, float px) -> ImFont* {
        for (const char* n : names) {
            const std::string path = n[0] == '/' ? std::string(n) : fonts + n;
            if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
                std::fclose(f);
                ImFontConfig cfg;
                cfg.OversampleH = 2;
                cfg.OversampleV = 2;
                cfg.RasterizerDensity = density;  // (sharp on high-DPI screens)
                if (ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), px, &cfg, kRanges)) return font;
            }
        }
        return nullptr;
    };
    // (the player's own system fonts: nothing is shipped; ImGui's font when none is there)
    g_font.label = load({"georgiab.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf"}, 48.f);
    g_font.bold = load({"segoeuib.ttf", "arialbd.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"}, 40.f);
    g_font.text = load({"bahnschrift.ttf", "segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed.ttf"}, 40.f);
    log("[menu] fonts: %s, %s, %s\n", g_font.label ? "serif" : "default", g_font.bold ? "bold" : "default",
        g_font.text ? "text" : "default");
}

void event(const SDL_Event& e) {
    if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat) return;
    if (e.key.scancode == SDL_SCANCODE_F1) g_ui.open ? closeMenu() : openMenu();
    else if (e.key.scancode == SDL_SCANCODE_ESCAPE) closeMenu();
}

void takePads(PADStatus pads[4]) {
    // What is held on every controller plugged in (port 1 is the keyboard too), as the menu's moves.
    u32 held = 0;
    u16 buttons = 0;
    for (int i = 0; i < 4; i++) {
        const PADStatus& p = pads[i];
        if (p.err != PAD_ERR_NONE) continue;
        buttons |= p.button;
        if (p.button & PAD_BUTTON_UP || p.stickY > 50) held |= kUp;
        if (p.button & PAD_BUTTON_DOWN || p.stickY < -50) held |= kDown;
        if (p.button & PAD_BUTTON_LEFT || p.stickX < -50) held |= kLeft;
        if (p.button & PAD_BUTTON_RIGHT || p.stickX > 50) held |= kRight;
        if (p.button & (PAD_BUTTON_A | PAD_BUTTON_START)) held |= kOk;
        if (p.button & PAD_BUTTON_B) held |= kBack;
        if (p.button & PAD_TRIGGER_L || p.triggerLeft > 150) held |= kPrevTab;
        if (p.button & PAD_TRIGGER_R || p.triggerRight > 150) held |= kNextTab;
    }
    // A move pressed counts once; a direction held repeats after a moment.
    static u32 s_prev = 0;
    static double s_heldSince = 0, s_lastRepeat = 0;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    constexpr u32 kDirs = kUp | kDown | kLeft | kRight;
    u32 pressed = held & ~s_prev;
    if (pressed & kDirs) s_heldSince = now, s_lastRepeat = now;
    else if (held & kDirs && now - s_heldSince > 0.38 && now - s_lastRepeat > 0.075) pressed |= held & kDirs, s_lastRepeat = now;
    s_prev = held;
    if (g_ui.open) g_ui.nav |= pressed;
    // The game sees nothing while the menu is open, nor the buttons that were held as it closed until let go.
    if (g_ui.open) {
        g_ui.padMask = buttons;
        for (int i = 0; i < 4; i++) {
            const s8 err = pads[i].err;
            pads[i] = PADStatus{};
            pads[i].err = err;
        }
    } else if (g_ui.padMask) {
        g_ui.padMask &= buttons;
        for (int i = 0; i < 4; i++) pads[i].button &= u16(~g_ui.padMask);
    }
}

float volume() {
    if (settings::muteUnfocused && !SDL_GetKeyboardFocus()) return 0.f;
    return settings::masterVolume;
}

bool isOpen() { return g_ui.open; }

void overlay(bool headless) {
    if (!ImGui::GetCurrentContext()) return;
    gameart::load();  // the game's own menu art (src/runtime/gameart.h), once it can be read
    static const char* s_test = std::getenv("SA2_MENU_OPEN");
    if (headless && !s_test) return;
    ImGuiIO& io = ImGui::GetIO();
    const double now = ImGui::GetTime();
    g_s = std::max(0.5f, std::min(io.DisplaySize.x / 1280.f, io.DisplaySize.y / 720.f));
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    static int s_frames = 0;
    s_frames++;
    static double s_firstAt = now;
    // (SA2_MENU_AT=<frames>: opened that many frames in, over whatever the game shows by then)
    static const int s_at = std::getenv("SA2_MENU_AT") ? std::max(1, std::atoi(std::getenv("SA2_MENU_AT"))) : 1;
    if (s_test && s_frames == s_at) {
        int t = 0, s = 0, c = 1, r = 0;
        std::sscanf(s_test, "%d,%d,%d,%d", &t, &s, &c, &r);
        goTo(t, s, c, r);
        g_ui.fieldFrom = tabs()[g_ui.tab].colour;
        openMenu();
    }
    if (static const int s_tour = std::getenv("SA2_MENU_TOUR") ? std::atoi(std::getenv("SA2_MENU_TOUR")) : 0; s_tour > 0) {
        static const std::vector<TourStop> stops = tourStops();
        const int f = s_frames - s_at, k = f / s_tour;
        if (f > 0 && f % s_tour == 0 && k < int(stops.size())) goTo(stops[k].tab, stops[k].section, stops[k].column, stops[k].row);
        // after the tour: closed, then opened again (its slide up and down)
        if (k == int(stops.size()) && f % s_tour == 0) closeMenu();
        if (k == int(stops.size()) && f % s_tour == s_tour / 2) openMenu();
    }

    // Opening and closing (the moves pressed meanwhile are dropped).
    if (g_ui.openPending) {
        g_ui.openPending = false;
        if (!g_ui.open) {
            g_ui.open = true;
            // (closing part way: it comes back down from where it is)
            const float back = clamp01(1.f - float(now - g_ui.closedAt) / 0.24f);
            g_ui.openedAt = now - 0.42 * double(back) * 0.6;
            g_ui.nav = 0;
            g_ui.fieldFrom = tabs()[g_ui.tab].colour;
        }
    }
    if (g_ui.closePending) {
        g_ui.closePending = false;
        if (g_ui.open) g_ui.open = false, g_ui.closedAt = now, g_ui.drag = nullptr;
    }
    float appear = 0.f;
    if (g_ui.open) appear = easeOutBack(float(now - g_ui.openedAt) / 0.42f);
    else if (now - g_ui.closedAt < 0.24) {
        const float t = clamp01(float(now - g_ui.closedAt) / 0.24f);
        appear = 1.f - t * t * t;
    }
    io.MouseDrawCursor = g_ui.open;

    // The frame counter: frames drawn in the last half second.
    if (settings::showFps) {
        static double s_from = now;
        static int s_count = 0;
        static float s_fps = 0;
        s_count++;
        if (now - s_from >= 0.5) s_fps = float(s_count / (now - s_from)), s_from = now, s_count = 0;
        drawFps(dl, s_fps);
    }
    // (hidden while the menu is out)
    static bool s_menuSeen = false;
    s_menuSeen |= g_ui.open;
    if (settings::menuHint && !s_menuSeen) drawHint(dl, float(now - s_firstAt) - 1.f);

    if (appear > 0.f) {
        // a tab chosen: the field's colour fades from the last one
        static int s_lastTab = g_ui.tab;
        if (s_lastTab != g_ui.tab) {
            const float fade = easeOut(float(now - g_ui.tabAt) / 0.3f);
            const ModeColour was = tabs()[s_lastTab].colour;
            g_ui.fieldFrom = {mixRgb(g_ui.fieldFrom.field, was.field, fade), mixRgb(g_ui.fieldFrom.deep, was.deep, fade)};
            g_ui.tabAt = now, s_lastTab = g_ui.tab;
        }
        Mouse m{io.MousePos, io.MouseDelta.x != 0 || io.MouseDelta.y != 0, io.MouseClicked[0], io.MouseDown[0], io.MouseReleased[0],
                io.MouseWheel};
        const bool live = g_ui.open && now - g_ui.openedAt > 0.12;
        // a click on the dimmed game closes the menu
        drawMenu(dl, appear, live, m);
        if (live && m.clicked && !g_ui.drag) {
            const float sheetW = std::min(io.DisplaySize.x - U(48), U(1200)), sheetH = std::min(io.DisplaySize.y - U(30), U(612));
            const float sx0 = (io.DisplaySize.x - sheetW) * 0.5f;
            if (m.pos.y > sheetH || m.pos.x < sx0 || m.pos.x > sx0 + sheetW) closeMenu();
        }
    }
    g_ui.nav = 0;

    if (static const char* s_shot = std::getenv("SA2_MENU_SHOT"); s_shot) {
        static const int s_every = std::getenv("SA2_MENU_SHOT_EVERY") ? std::max(1, std::atoi(std::getenv("SA2_MENU_SHOT_EVERY"))) : 30;
        static const int s_from = std::getenv("SA2_MENU_SHOT_FROM") ? std::atoi(std::getenv("SA2_MENU_SHOT_FROM")) : s_at;
        if (s_frames >= s_from && (s_frames - s_from) % s_every == s_every - 1) writeShot(s_shot, "menu", s_frames);  // (a tour stop's last frame)
    }
}

}  // namespace rt::menu

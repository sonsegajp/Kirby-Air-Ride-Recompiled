// The Online menu: Kirby Air Ride online, through the Air Ride master server (src/runtime/airnet.h,
// docs/ONLINE.md), as a screen of the game.
//
// Choosing ONLINE in Select Mode opens it at once, full screen: the game goes on to its LAN mode's first
// screen ("How many Nintendo GameCube systems...?") and waits there, never seen - this menu covers it from
// its first frame and takes the controllers. Its screens:
//   - Sign In / Create Account (a remembered login signs in by itself);
//   - the Online menu: Browse Servers, Host Server, Profile, Settings, Log Out, Back;
//   - the server browser and the Host Server form;
//   - a server: one session. Joining (or hosting) puts you in it: the players and how this console
//     reaches each (direct on the LAN, direct, or through the relay) with their round trip, the session's
//     state (gathering, the countdown, the match on, its results), the game settings (the mode and its
//     rules: the host changes them, everyone sees them) and chat. The host starts the match now, cancels
//     the countdown, kicks;
//   - profiles (Kirby's colour, a favourite machine, matches and wins), one's own and others'.
// When the match starts (airnet's match start), the menu hands the game the match - the console count,
// this console's number, the settings - and the game goes straight into it (src/runtime/online_game.h ->
// lansession). When the game reports the match over, the results go to the server and the menu comes
// back to the session for the next match. Players beyond the match's consoles (or who join during one)
// wait for the next. Back leaves the server, then Online: the game goes back to Select Mode.
//
// Its look is the game's mode menus (a title on a stepped line, leaning buttons with a ball at their left,
// the one in focus orange on a maroon band, a disc with the mode's emblem - the ONLINE globe - and the
// rounded band describing what is in focus), drawn with the game's own art and font where they are
// (src/runtime/gameart.h, menustyle.h). Controllers (d-pad / stick, A, B, X, Y, L / R), the keyboard
// (arrows, Enter, Escape, typing) and the mouse all steer it; text is typed on the keyboard or on an
// on-screen keyboard (A on a text field).
//
// Test runs (headless draws nothing unless asked): SA2_ONLINE_TEST="<frames>:<step>;..." runs steps that
// many drawn frames apart: open, close, lanflow (as if ONLINE were chosen), up, down, left, right, a, b, x,
// y, l, r, start, enter, esc, bs, type=<text>, results=<json> (as if the game reported the match over),
// shot (SA2_ONLINE_SHOT=<dir> receives online_<n>.rgba: what the menu drew, as the F1 menu's SA2_MENU_SHOT).
// SA2_ONLINE=0 turns the menu off; with SA2_LAN_LOOPBACK (developer LAN tests) it does not open by itself.
#include "runtime/online_menu.h"
#include "runtime/morepl.h"
#include "runtime/airnet.h"
#include "runtime/kirbymodel.h"
#include "runtime/lanport.h"
#include "runtime/mapfiles.h"
#include "runtime/menu.h"
#include "runtime/menustyle.h"
#include "runtime/online_game.h"
#include "runtime/runtime.h"
#include "runtime/settings.h"

#include <aurora/aurora.h>
#include <dolphin/pad.h>
#include <imgui.h>
#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_video.h>

#include "window.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "runtime/gx.h"
#include <ctime>
#include <deque>
#include <functional>
#include <future>
#include <string>
#include <vector>

namespace rt::online {

namespace {

using namespace rt::menustyle;
namespace game = rt::online::game;
namespace an = rt::airnet;

// The Online mode's colours: the ONLINE badge's sky blue.
constexpr ModeColour kOnline{0x79A6C8, 0x4D7AA0};
constexpr u32 kDeepPlum = 0x18151E;  // the strip along the bottom
constexpr u32 kGreen = 0x58C76B, kYellow = 0xF2C14E, kRed = 0xD9483B, kGrey = 0x8A8A92;

// Kirby's colours (airnet's kColours ids).
u32 kirbyRgb(const std::string& id) {
    struct C {
        const char* id;
        u32 rgb;
    };
    static const C k[] = {{"pink", 0xF5A0C2},   {"yellow", 0xF6D24A}, {"red", 0xE5483F},   {"blue", 0x4F86E0},
                          {"green", 0x5DBB5A},  {"purple", 0x9C6ADE}, {"brown", 0x9C6B40}, {"white", 0xF2F2F2}};
    for (const C& c : k)
        if (id == c.id) return c.rgb;
    return 0xF5A0C2;
}

// ---------------------------------------------------------------------------------------------------
// Input

enum Nav : u32 {
    kUp = 1,
    kDown = 2,
    kLeft = 4,
    kRight = 8,
    kOk = 16,      // A (Space, Enter on a button)
    kBack = 32,    // B (Shift, Escape)
    kPrevTab = 64, // L (Q)
    kNextTab = 128,// R (E)
    kX = 256,      // X (F)
    kY = 512,      // Y (C)
    kStart = 1024, // Start (Enter)
    kSubmit = 2048,// Enter typed on a text field
};

// A line of text being typed.
enum class Allow : u8 { Username, Password, Text, Address };
struct Field {
    std::string text;
    int max = 32;  // bytes
    Allow allow = Allow::Text;
    bool secret = false;
    const char* placeholder = "";
    bool takes(u32 cp) const {
        if (cp < 0x20 || cp == 0x7F) return false;
        switch (allow) {
        case Allow::Username: return cp < 0x80 && (std::isalnum(int(cp)) || cp == '_' || cp == '.' || cp == '-');
        case Allow::Password: return cp < 0x7F;
        case Allow::Address: return cp < 0x80 && (std::isalnum(int(cp)) || cp == '.' || cp == ':' || cp == '-' || cp == '[' || cp == ']' || cp == '_');
        default: return true;
        }
    }
    // Types UTF-8 text, keeping what the field takes.
    void type(const char* s) {
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p;) {
            int n = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 1;
            u32 cp = n == 1 ? *p : n == 2 ? *p & 0x1F : n == 3 ? *p & 0x0F : *p & 0x07;
            bool ok = true;
            for (int i = 1; i < n; i++) {
                if ((p[i] & 0xC0) != 0x80) {
                    ok = false, n = i;
                    break;
                }
                cp = cp << 6 | (p[i] & 0x3F);
            }
            if (ok && takes(cp) && int(text.size()) + n <= max) text.append(reinterpret_cast<const char*>(p), size_t(n));
            p += n;
        }
    }
    void erase() {
        while (!text.empty() && (u8(text.back()) & 0xC0) == 0x80) text.pop_back();
        if (!text.empty()) text.pop_back();
    }
};

// Something on screen the controllers can move to: a button, a value, a text field, a list row.
struct Item {
    ImVec2 a, b;    // its box (the mouse)
    int col = 0;    // left / right move between columns
    bool enabled = true;
    std::function<void()> ok;        // A, a click
    std::function<void(int)> step;   // left / right change it (a click: by the side it lands)
    float split = -1;                // ... the click's dividing line (default: the box's middle)
    Field* field = nullptr;          // typing goes here while it is in focus; A opens the on-screen keyboard
    std::function<void()> submit;    // Enter typed in it, Done on the on-screen keyboard
    std::string help;                // the band's text while it is in focus
};

enum class Screen : u8 { Connecting, SignIn, Register, Main, Browse, Host, Lobby, Profile, ViewProfile, Settings, Update, Count };

struct Button {
    std::string label;
    std::function<void()> act;
};
struct Modal {
    bool open = false;
    std::string title, text;
    Field* field = nullptr;
    std::string fieldLabel;
    std::vector<Button> buttons;
    std::function<void()> cancel;  // B
    int focus = 0;
    double at = -100;
};

struct Toast {
    std::string text;
    double at;
    bool error;
};

// A press handed to the game (its LAN screen): held `hold` seconds after waiting `wait`, counted in the game's own
// frames (60 a second) - a stall of the window does not shorten it.
struct Press {
    u16 buttons;
    double wait, hold;
    u64 startedAt = ~0ull;
};

struct Ui {
    bool enabled = true, autoOpen = true, headless = false;
    bool open = false;
    double openedAt = -100, closedAt = -100;
    bool openPending = false, closePending = false;
    bool everOpened = false;  // (airnet's client is made on first use)
    Screen screen = Screen::SignIn;
    Screen settingsFrom = Screen::SignIn, profileFrom = Screen::Main;
    double screenAt = -100;
    int focus[int(Screen::Count)] = {};
    u32 nav = 0;     // what the controllers pressed since the last frame
    u32 keyNav = 0;  // ... and the keyboard (on text fields; Escape)
    u32 padMask = 0; // buttons held as the menu closed: kept from the game until let go
    std::vector<Item> items;
    int click = -1;
    ImVec2 clickPos;
    bool inModal = false;  // the modal's items are being made
    Modal modal;
    // the on-screen keyboard
    bool osk = false;
    Field* oskField = nullptr;
    std::string oskLabel;
    std::function<void()> oskDone;
    int oskRow = 1, oskCol = 0;
    bool oskShift = false;
    double oskAt = -100;
    // typing on the keyboard: the field it goes to (as of the last frame)
    Field* typing = nullptr;
    bool textInput = false;
    // toasts
    std::vector<Toast> toasts;
    u32 errorSerial = 0;
    // lists
    int browseTop = 0, membersTop = 0, chatBack = 0;
    int settingsTop = 0;  // the lobby's game settings: the first row shown (the list scrolls)
    double refreshedAt = -100;
    int filter = 0;  // the browser's: Any, Air Ride, City Trial
    std::string viewing;  // the profile asked for
    // the game's side
    bool lanFlow = false;     // opened on the game's Online mode (it waits on its LAN screen underneath)
    bool cover = false;       // the game is covered, whatever the menu is doing (its LAN screen, a match starting)
    bool leaving = false;     // Back: covered until the game is back on Select Mode
    double leavingAt = -100;
    bool wanted = false;
    double wantedAt = -100, unwantedAt = -100;
    std::deque<Press> presses;
    // the match
    bool matchSeenInit = false;
    uint32_t matchSeen = 0;   // the last match start acted on (airnet's matchSerial)
    bool inMatch = false;     // the game is playing the session's match
    double matchAt = -100;
    bool matchFailed = false; // the game side could not start it: the menu stays
    // what the last frame saw
    bool wasInLobby = false, wasLoggedIn = false;
    an::SessionState session = an::SessionState::Lobby;
} g;

struct Forms {
    Field user{"", an::kUsernameMax, Allow::Username, false, "your name"};
    Field pass{"", 64, Allow::Password, true, "at least 6 characters"};
    Field pass2{"", 64, Allow::Password, true, "the same again"};
    bool remember = true;
    Field master{"", 63, Allow::Address, false, "host:port"};  // a dedicated server's (never the official service's)
    bool dedicated = false;                                     // Online Settings: a dedicated server, not the official service
    Field newPass{"", 64, Allow::Password, true, "at least 6 characters"};
    Field hostName{"", 32, Allow::Text, false, ""};
    Field hostPass{"", 64, Allow::Password, true, "none: open to all"};
    int hostMax = 8, hostPlayers = an::kMatchDefault, hostMode = game::kCityTrial;
    bool hostAuto = true;
    Field chat{"", an::kChatMax, Allow::Text, false, "Say something..."};
    Field joinPass{"", 64, Allow::Password, true, ""};
} f;

an::Snapshot s;  // airnet's state, read once a frame
// The most consoles a match can have here: 8 in the 8-player build (src/runtime/morepl.h), else the game's 4.
int maxMatchPlayers() { return morepl::active() ? an::kMatchMax : 4; }

// The map mods on this PC, for the host's Stage choice (src/runtime/mapfiles.h): looked through on a worker - each
// one's hash read from all its files - as a server opens, and when a map has come.
struct MapList {
    std::vector<mapfiles::Map> maps;
    std::future<std::vector<mapfiles::Map>> scanning;
    bool scanned = false;
    bool wasInLobby = false;
    an::MapState wasState = an::MapState::None;
} g_maps;

void scanMaps() {
    if (g_maps.scanning.valid()) return;  // (one at a time)
    g_maps.scanning = std::async(std::launch::async, [] { return mapfiles::scan(mapfiles::defaultRoot(), true); });
}
void mapsFrame() {
    if ((s.inLobby && !g_maps.wasInLobby) || (s.mapState == an::MapState::Ready && g_maps.wasState != an::MapState::Ready)) scanMaps();
    g_maps.wasInLobby = s.inLobby;
    g_maps.wasState = s.mapState;
    if (g_maps.scanning.valid() && g_maps.scanning.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        g_maps.maps = g_maps.scanning.get();
        g_maps.scanned = true;
    }
}

// The lobby's stage (its settings' map; folder "": the city), worked out again only when the settings change.
const game::Stage& lobbyStage() {
    static std::string s_json;
    static game::Stage s_stage;
    if (s_json != s.lobby.settingsJson) {
        s_json = s.lobby.settingsJson;
        const game::Settings st = game::Settings::fromJson(s_json);
        s_stage = st.mode == game::kCityTrial ? st.stage : game::Stage{};
    }
    return s_stage;
}
// A player of the next match without the stage's map (the match waits for them).
bool lacksMap(const an::Member& m) { return m.player && !lobbyStage().folder.empty() && m.mapOk != lobbyStage().hash; }
int playersWithoutMap() {
    int n = 0;
    for (const an::Member& m : s.lobby.members) n += lacksMap(m) ? 1 : 0;
    return n;
}

an::Client& net() { return an::client(); }
double nowT() { return ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0; }

void toast(const std::string& text, bool error = true) {
    g.toasts.push_back({text, nowT(), error});
    if (g.toasts.size() > 3) g.toasts.erase(g.toasts.begin());
    log("[onlinemenu] %s\n", text.c_str());
}

// ---------------------------------------------------------------------------------------------------
// Layout: design units (a 1280x720 window), the design centred in the window

float g_ox = 0, g_oy = 0;
float g_ext = 0;  // design units the window is taller than 16:9, on each side (4:3: 120): the layout stretches
float X(float v) { return g_ox + U(v); }
float Y(float v) { return g_oy + U(v); }
// (the content sits a little above the middle of the extra height, the header at the top, the foot at the bottom)
float YT(float v) { return Y(v) - U(g_ext * 0.5f); }
float YB(float v) { return Y(v) + U(g_ext * 1.5f); }
Mouse g_m;
bool g_live = false;  // taking input this frame

// Lettering cut to w with an ellipsis (the serif labels' font, or the game's).
std::string fit(ImFont* font, float size, const std::string& s, float w) {
    auto width = [&](const std::string& x) { return font == g_font.label ? measure(font, size, x.c_str()).x : gameMeasure(font, size, x.c_str()); };
    if (width(s) <= w) return s;
    std::string t = s;
    while (!t.empty() && width(t + "\xE2\x80\xA6") > w) {
        t.pop_back();
        while (!t.empty() && (u8(t.back()) & 0xC0) == 0x80) t.pop_back();
    }
    return t + "\xE2\x80\xA6";
}

std::string dateOf(int64_t t) {
    if (t <= 0) return "-";
    const std::time_t tt = std::time_t(t);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%d %b %Y", &tm);
    return b;
}

// ---------------------------------------------------------------------------------------------------
// Items: made while drawing (immediate mode), acted on after

int& curFocus() { return g.inModal ? g.modal.focus : g.focus[int(g.screen)]; }
// A key for an item's eased values (glide): the screen (or the box) and its place, never dereferenced.
const void* itemKey(int idx, int salt = 0) {
    const uintptr_t where = g.inModal ? 15u : uintptr_t(g.screen);
    return reinterpret_cast<const void*>(uintptr_t(0x7F000000u) + (where << 14) + uintptr_t(idx) * 4u + uintptr_t(salt));
}
bool interactive() { return g_live && !g.osk && (!g.modal.open || g.inModal); }
bool isFocus(int idx) { return interactive() && idx == curFocus(); }
int nextIdx() { return int(g.items.size()); }
bool hover(ImVec2 a, ImVec2 b) { return interactive() && g_m.in(a, b); }

int add(Item it) {
    const int idx = nextIdx();
    if (it.enabled && hover(it.a, it.b)) {
        if (g_m.moved) curFocus() = idx;
        if (g_m.clicked) curFocus() = idx, g.click = idx, g.clickPos = g_m.pos;
    }
    g.items.push_back(std::move(it));
    return idx;
}

// ---------------------------------------------------------------------------------------------------
// Shapes of this menu

void spinner(ImDrawList* dl, ImVec2 c, float r, float t, u32 rgb = kWhite) {
    for (int i = 0; i < 8; i++) {
        const float a = t * 7.f + float(i) * kPi / 4.f;
        const float k = float((i + int(t * 7.f / (kPi / 4.f))) % 8) / 8.f;
        dl->AddCircleFilled(ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r), r * 0.22f, col(rgb, 0.25f + 0.75f * k), 10);
    }
}

// A padlock (a locked server).
void padlock(ImDrawList* dl, ImVec2 c, float s, ImU32 fill) {
    dl->AddCircle(ImVec2(c.x, c.y - s * 0.25f), s * 0.38f, fill, 16, s * 0.16f);
    dl->AddRectFilled(ImVec2(c.x - s * 0.55f, c.y - s * 0.15f), ImVec2(c.x + s * 0.55f, c.y + s * 0.6f), fill, s * 0.12f);
}

// A crown (the host).
void crown(ImDrawList* dl, ImVec2 c, float s) {
    const ImVec2 p[7] = {ImVec2(c.x - s, c.y + s * 0.6f), ImVec2(c.x - s, c.y - s * 0.4f), ImVec2(c.x - s * 0.45f, c.y + s * 0.05f),
                         ImVec2(c.x, c.y - s * 0.7f),     ImVec2(c.x + s * 0.45f, c.y + s * 0.05f), ImVec2(c.x + s, c.y - s * 0.4f),
                         ImVec2(c.x + s, c.y + s * 0.6f)};
    const ImU32 gold = col(0xF6C945);
    dl->AddRectFilled(ImVec2(c.x - s, c.y + s * 0.05f), ImVec2(c.x + s, c.y + s * 0.6f), gold);
    dl->AddTriangleFilled(p[1], p[2], ImVec2(c.x - s, c.y + s * 0.05f), gold);
    dl->AddTriangleFilled(p[2], p[3], p[4], gold);
    dl->AddTriangleFilled(p[4], p[5], ImVec2(c.x + s, c.y + s * 0.05f), gold);
    dl->AddPolyline(p, 7, col(kInk), ImDrawFlags_Closed, U(1.5f));
}

// Text broken into lines: the first `firstW` wide, the rest `restW` (a chat line after its sender's name).
std::vector<std::string> wrapLines(ImFont* font, float size, const std::string& s, float firstW, float restW) {
    std::vector<std::string> lines;
    const char* p = s.c_str();
    const char* end = p + s.size();
    float w = firstW;
    while (p < end) {
        const char* e = gameWrap(font, size, p, end, std::max(w, size));
        if (e <= p) e = p + 1;  // (a word wider than the line)
        lines.emplace_back(p, e);
        p = e;
        while (p < end && (*p == ' ' || *p == '\n')) p++;
        w = restW;
    }
    if (lines.empty()) lines.emplace_back();
    return lines;
}

// Text in the game's lettering, wrapped to w, its first line's top at y; returns its height.
float wrapped(ImDrawList* dl, ImFont* font, float size, float x, float y, float w, ImU32 c, const std::string& s) {
    const float lineH = size * 1.3f;
    float h = 0;
    size_t from = 0;
    while (from <= s.size()) {  // (paragraphs at line breaks)
        size_t to = s.find('\n', from);
        if (to == std::string::npos) to = s.size();
        for (const std::string& line : wrapLines(font, size, s.substr(from, to - from), w, w)) {
            if (dl) gameText(dl, font, size, x, y + h + lineH * 0.5f, c, line.c_str());
            h += lineH;
        }
        from = to + 1;
    }
    return h;
}

// The ball in front of a member: their Kirby colour.
void colourBall(ImDrawList* dl, ImVec2 c, float r, u32 rgb) {
    dl->AddCircleFilled(c, r + U(2), col(kInk), 20);
    dl->AddCircleFilled(c, r, col(rgb), 20);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.35f, c.y - r * 0.35f), r * 0.32f, col(kWhite, 0.45f), 12);
}

// A colour's image in Kirby's model (src/runtime/kirbymodel.h).
int kirbyFrame(const std::string& id) {
    static const std::pair<const char*, kirbymodel::Colour> kFrames[] = {
        {"pink", kirbymodel::Pink},   {"yellow", kirbymodel::Yellow}, {"red", kirbymodel::Red},     {"blue", kirbymodel::Blue},
        {"green", kirbymodel::Green}, {"purple", kirbymodel::Purple}, {"brown", kirbymodel::Brown}, {"white", kirbymodel::White}};
    for (const auto& [n, f] : kFrames)
        if (id == n) return f;
    return kirbymodel::Pink;
}

// Kirby, in a colour (the Profile's emblem when his model is not there).
void puffball(ImDrawList* dl, ImVec2 c, float r, u32 rgb, float t) {
    const u32 foot = mixRgb(rgb, 0xB0103A, rgb == 0xF5A0C2 ? 0.75f : 0.45f);
    const float bob = std::sin(t * 2.2f) * r * 0.03f;
    c.y += bob;
    for (int side = -1; side <= 1; side += 2) {  // feet, arms
        dl->AddEllipseFilled(ImVec2(c.x + side * r * 0.5f, c.y + r * 0.86f - bob), ImVec2(r * 0.46f, r * 0.24f), col(kInk), side * 0.25f, 24);
        dl->AddEllipseFilled(ImVec2(c.x + side * r * 0.5f, c.y + r * 0.86f - bob), ImVec2(r * 0.42f, r * 0.2f), col(foot), side * 0.25f, 24);
        dl->AddCircleFilled(ImVec2(c.x + side * r * 0.92f, c.y + r * 0.12f), r * 0.27f, col(kInk), 20);
        dl->AddCircleFilled(ImVec2(c.x + side * r * 0.92f, c.y + r * 0.12f), r * 0.23f, col(rgb), 20);
    }
    dl->AddCircleFilled(c, r + U(3), col(kInk), 48);
    dl->AddCircleFilled(c, r, col(rgb), 48);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.35f, c.y - r * 0.4f), r * 0.28f, col(kWhite, 0.35f), 24);
    const bool blink = std::fmod(t, 3.7f) < 0.12f;
    for (int side = -1; side <= 1; side += 2) {  // eyes, cheeks
        const ImVec2 e(c.x + side * r * 0.2f, c.y - r * 0.12f);
        if (blink) {
            dl->AddLine(ImVec2(e.x - r * 0.08f, e.y), ImVec2(e.x + r * 0.08f, e.y), col(kInk), U(3));
        } else {
            dl->AddEllipseFilled(e, ImVec2(r * 0.085f, r * 0.2f), col(0x101030), 0, 20);
            dl->AddEllipseFilled(ImVec2(e.x, e.y - r * 0.08f), ImVec2(r * 0.05f, r * 0.08f), col(kWhite), 0, 12);
            dl->AddEllipseFilled(ImVec2(e.x, e.y + r * 0.1f), ImVec2(r * 0.05f, r * 0.06f), col(0x3050C0), 0, 12);
        }
        dl->AddEllipseFilled(ImVec2(c.x + side * r * 0.5f, c.y + r * 0.12f), ImVec2(r * 0.14f, r * 0.07f), col(0xF06A8E, 0.7f), 0, 16);
    }
    dl->PathArcTo(ImVec2(c.x, c.y + r * 0.1f), r * 0.1f, 0.3f, kPi - 0.3f, 10);
    dl->PathStroke(col(kInk), ImDrawFlags_None, U(2.5f));
}

// The turning globe with a star circling it: the Online mode's emblem.
void globe(ImDrawList* dl, ImVec2 c, float r, float t) {
    dl->AddCircleFilled(c, r + U(5), col(kInk), 64);
    dl->AddCircleFilled(c, r, col(0xEAF1F7), 64);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.3f, c.y - r * 0.32f), r * 0.45f, col(kWhite, 0.55f), 40);
    const ImU32 line = col(kOnline.deep, 0.95f);
    const float w = U(3.2f);
    for (int k = 0; k < 6; k++) {  // meridians, turning
        const float ph = std::fmod(t * 0.45f + float(k) * kPi / 6.f, kPi);
        const float rx = r * std::fabs(std::cos(ph));
        if (rx > U(1.5f)) dl->AddEllipse(c, ImVec2(rx, r), line, 0.f, 48, w);
        else dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), line, w);
    }
    for (float lat : {-0.55f, 0.f, 0.55f}) {  // parallels
        const float y = c.y + lat * r, hw = r * std::sqrt(1.f - lat * lat);
        dl->AddEllipse(ImVec2(c.x, y), ImVec2(hw, r * 0.07f), line, 0.f, 40, w);
    }
    dl->AddCircle(c, r, col(kInk), 64, U(5));
    // the star's orbit, tilted, and the star on it
    const float tilt = -0.38f, ox = r * 1.32f, oy = r * 0.36f;
    auto onOrbit = [&](float a) {
        const float x = std::cos(a) * ox, y = std::sin(a) * oy;
        return ImVec2(c.x + x * std::cos(tilt) - y * std::sin(tilt), c.y + x * std::sin(tilt) + y * std::cos(tilt));
    };
    std::vector<ImVec2> pts;
    for (int i = 0; i <= 64; i++) pts.push_back(onOrbit(float(i) * 2.f * kPi / 64.f));
    dl->AddPolyline(pts.data(), int(pts.size()), col(kOrange, 0.9f), ImDrawFlags_Closed, U(4));
    const float a = t * 1.3f;
    star(dl, onOrbit(a), r * 0.24f, r * 0.11f, t * 2.f, col(0xFFD84A), col(kInk), U(2.5f));
}

// The disc at the right that holds the mode's emblem (as the City Trial menu's).
void disc(ImDrawList* dl, ImVec2 c, float r, float t) {
    dl->AddCircleFilled(c, r + U(18), col(0x24202C), 96);
    for (int i = 0; i < 90; i++) {  // the ticks around its rim, turning slowly
        const float a = t * 0.05f + float(i) * 2.f * kPi / 90.f;
        const ImVec2 d(std::cos(a), std::sin(a));
        dl->AddLine(ImVec2(c.x + d.x * (r + U(5)), c.y + d.y * (r + U(5))), ImVec2(c.x + d.x * (r + U(13)), c.y + d.y * (r + U(13))),
                    col(0x8C8794, 0.8f), U(2.5f));
    }
    dl->AddCircleFilled(c, r + U(3), col(0x3A3542), 96);
    dl->AddCircleFilled(c, r, col(0x9C9AA4), 96);
    dl->AddCircleFilled(c, r * 0.94f, col(0xB4B2BB), 96);
    // its grooves: the game's (rings in their place until its art has loaded)
    if (!artRing(dl, gameart::Art::Grooves, c, r * 0.97f, col(0x55555E, 0.9f), t * 0.04f))
        for (int i = 1; i <= 3; i++) dl->AddCircle(c, r * (0.38f + 0.18f * float(i)), col(kWhite, 0.18f), 96, U(2));
}

// The ONLINE emblem on the disc: the globe and lettering of the mode select's ONLINE badge (the drawn
// globe until they are there).
void outlined(ImDrawList* dl, ImFont* font, float size, ImVec2 at, u32 fill, const char* s);
void onlineEmblem(ImDrawList* dl, ImVec2 c, float r, float t) {
    const gameart::Texture& globeArt = gameart::get(gameart::Art::Emblem);
    const gameart::Texture& word = gameart::get(gameart::Art::Lettering);
    if (globeArt && word) {
        const float g = r * 1.18f;  // (the art's globe fills about two thirds of it)
        const ImVec2 gc(c.x + r * 0.02f, c.y - r * 0.16f + std::sin(t * 1.6f) * r * 0.015f);
        dl->AddImage(globeArt.id, ImVec2(gc.x - g * 0.5f + U(3), gc.y - g * 0.5f + U(4)), ImVec2(gc.x + g * 0.5f + U(3), gc.y + g * 0.5f + U(4)),
                     ImVec2(0, 0), ImVec2(1, 1), col(0x24202C, 0.35f));
        dl->AddImage(globeArt.id, ImVec2(gc.x - g * 0.5f, gc.y - g * 0.5f), ImVec2(gc.x + g * 0.5f, gc.y + g * 0.5f), ImVec2(0, 0), ImVec2(1, 1),
                     col(0x2A2733));
        const float ww = r * 0.9f, wh = ww * float(word.h) / float(word.w);
        const ImVec2 wc(c.x, c.y + r * 0.56f);
        dl->AddImage(word.id, ImVec2(wc.x - ww * 0.5f, wc.y - wh * 0.5f), ImVec2(wc.x + ww * 0.5f, wc.y + wh * 0.5f), ImVec2(0, 0), ImVec2(1, 1),
                     col(kWhite));
        return;
    }
    globe(dl, ImVec2(c.x, c.y - r * 0.1f), r * 0.55f, t);
    const float size = r * 0.3f, w = gameMeasure(g_font.bold, size, "ONLINE");
    outlined(dl, g_font.bold, size, ImVec2(c.x - w * 0.5f, c.y + r * 0.5f - size * 0.5f), kWhite, "ONLINE");
}
// Lettering with an ink outline (the game's chunky logos).
void outlined(ImDrawList* dl, ImFont* font, float size, ImVec2 at, u32 fill, const char* s) {
    const float o = std::max(1.f, size * 0.07f);
    for (int i = 0; i < 8; i++) {
        const float a = float(i) * kPi / 4.f;
        dl->AddText(F(font), size, ImVec2(at.x + std::cos(a) * o, at.y + std::sin(a) * o), col(kInk), s);
    }
    dl->AddText(F(font), size, at, col(fill), s);
}

// ---------------------------------------------------------------------------------------------------
// Buttons and rows

// The mode menus' main button ("Start Game", "Stadium"): a capsule leaning forward, a ball at its left;
// in focus orange, on a maroon band reaching in from the left edge of the screen.
void ballButton(ImDrawList* dl, float x0, float y0, float x1, float y1, const char* label, float lit, bool hot, bool enabled) {
    const float h = y1 - y0, cy = (y0 + y1) * 0.5f, lean = 0.42f;
    const u32 fill = mixRgb(hot ? 0xD2D2D2 : kButton, kOrange, lit);
    // the game's band and button (drawn in their place until its art has loaded)
    if (lit > 0.01f && !artBand(dl, -U(80), y0 - U(12), x1 + U(30), y1 + U(12), lit)) {
        capsule(dl, -U(80), y0 - U(10), x1 + U(14), y1 + U(10), lean, false, col(kMaroonRim, lit));
        capsule(dl, -U(80), y0 - U(8), x1 + U(9), y1 + U(8), lean, false, col(kMaroon, lit));
    }
    if (!artButton(dl, x0 - h * 0.12f, y0 - U(4), x1 + U(10), y1 + U(4), fill, 1.f, lit > 0.5f)) {
        capsule(dl, x0 - U(4), y0 - U(4), x1 + U(4), y1 + U(4), lean, false, col(kInk));
        capsule(dl, x0, y0, x1, y1, lean, false, col(fill));
        capsule(dl, x0 + U(34), y0 + U(5), x1 - U(36), y0 + h * 0.3f, lean, false, col(mixRgb(kButtonLit, kOrangeLit, lit), 0.6f));
        const ImVec2 ball(x0 + U(10), cy);
        dl->AddCircleFilled(ball, h * 0.44f, col(kInk), 28);
        dl->AddCircleFilled(ball, h * 0.36f, col(mixRgb(kBall, kBallLit, lit)), 28);
        dl->AddCircleFilled(ImVec2(ball.x - h * 0.1f, ball.y - h * 0.1f), h * 0.1f, col(kWhite, 0.25f + 0.5f * lit), 16);
    }
    text(dl, g_font.label, h * 0.7f, x0 + h * 1.3f, cy, col(enabled ? kInk : 0x6A6A6A), label);
}
// The mode menus' other buttons ("Game Settings", "Records"): a grey pill in a heavy black capsule that
// sweeps back at its left.
void pillButton(ImDrawList* dl, float x0, float y0, float x1, float y1, const char* label, float lit, bool hot) {
    const float h = y1 - y0, cy = (y0 + y1) * 0.5f;
    capsule(dl, x0 - U(26), y0 + U(7), x1 - U(40), y1 + U(3), 0.f, true, col(kInk));
    capsule(dl, x0, y0, x1, y1, 0.f, true, col(kInk));
    capsule(dl, x0 + U(8), y0 + U(7), x1 - U(10), y1 - U(7), 0.f, true, col(mixRgb(hot ? 0xD8D8D8 : 0xC6C6C6, kOrange, lit)));
    capsule(dl, x0 + U(20), y0 + U(10), x1 - U(26), y0 + h * 0.38f, 0.f, true, col(mixRgb(0xE4E4E4, kOrangeLit, lit), 0.7f));
    const float size = h * 0.62f, w = measure(g_font.label, size, label).x;
    text(dl, g_font.label, size, (x0 + x1) * 0.5f - w * 0.5f, cy, col(kInk), label);
    if (lit > 0.5f) cornerMarks(dl, ImVec2(x0 - U(4), y0 - U(2)), ImVec2(x1 + U(4), y1 + U(2)), float(nowT()));
}

// A form's row, as the F1 menu's: a slanted grey button with its ball, the label at the left; returns
// where its value may end at the right.
float formRow(ImDrawList* dl, float x0, float y0, float x1, float y1, const char* label, float lit, bool hot, bool dim = false) {
    const float cy = (y0 + y1) * 0.5f;
    const u32 fill = mixRgb(hot ? 0xCCCCCC : kButton, kOrange, lit);
    // the game's band and button (drawn in their place until its art has loaded)
    if (lit > 0.01f && !artBand(dl, x0 - U(42), y0 - U(9), x1 + U(30), y1 + U(9), lit)) {
        capsule(dl, x0 - U(42), y0 - U(8), x1 + U(20), y1 + U(8), 0.35f, false, col(kMaroonRim, lit));
        capsule(dl, x0 - U(42), y0 - U(6), x1 + U(16), y1 + U(6), 0.35f, false, col(kMaroon, lit));
        capsule(dl, x0 - U(5), y0 - U(5), x1 + U(5), y1 + U(5), 0.35f, false, col(kWhite, lit));
    }
    if (!artButton(dl, x0 - U(22), y0 - U(5), x1 + U(8), y1 + U(5), fill, 1.f, lit > 0.5f)) {
        capsule(dl, x0 - U(3), y0 - U(3), x1 + U(3), y1 + U(3), 0.35f, false, col(kInk));
        capsule(dl, x0, y0, x1, y1, 0.35f, false, col(fill));
        capsule(dl, x0 + U(22), y0 + U(4), x1 - U(30), y0 + U(15), 0.35f, false, col(mixRgb(kButtonLit, kOrangeLit, lit), 0.6f));
        const ImVec2 ball(x0 + U(4), cy);
        dl->AddCircleFilled(ball, U(17), col(kInk), 28);
        dl->AddCircleFilled(ball, U(13.5f), col(mixRgb(kBall, kBallLit, lit)), 28);
        dl->AddCircleFilled(ImVec2(ball.x - U(4), ball.y - U(4)), U(4.5f), col(kWhite, 0.25f + 0.5f * lit), 16);
    }
    text(dl, g_font.label, U(23), x0 + U(30), cy, col(dim ? 0x5A5A5A : kInk), label);
    return x1 - U(34);
}
// A text field's box: what was typed (dots for a password), a caret while it takes the keyboard.
void fieldBox(ImDrawList* dl, ImVec2 a, ImVec2 b, const Field& fd, bool typing) {
    const float h = b.y - a.y, cy = (a.y + b.y) * 0.5f;
    dl->AddRectFilled(ImVec2(a.x - U(2.5f), a.y - U(2.5f)), ImVec2(b.x + U(2.5f), b.y + U(2.5f)), col(kInk), h * 0.5f);
    dl->AddRectFilled(a, b, col(typing ? 0xFFFFFF : 0xF1EDE4), h * 0.5f);
    std::string shown;
    if (fd.secret)
        for (size_t i = 0; i < fd.text.size(); i++) shown += "\xE2\x80\xA2";
    else shown = fd.text;
    const float size = U(18), pad = U(14);
    const float room = b.x - a.x - pad * 2;
    dl->PushClipRect(ImVec2(a.x + pad * 0.5f, a.y), ImVec2(b.x - pad * 0.5f, b.y), true);
    if (shown.empty() && !typing) {
        gameText(dl, g_font.text, size, a.x + pad, cy, col(0x8A8478), fd.placeholder);
    } else {
        const float tw = gameMeasure(g_font.bold, size, shown.c_str());
        const float x = a.x + pad - std::max(0.f, tw - room + U(6));
        gameText(dl, g_font.bold, size, x, cy, col(0x1E1E1E), shown.c_str());
        if (typing && std::fmod(nowT(), 1.0) < 0.6)
            dl->AddRectFilled(ImVec2(x + tw + U(2), cy - size * 0.55f), ImVec2(x + tw + U(4.5f), cy + size * 0.55f), col(kOrange));
    }
    dl->PopClipRect();
}

// "< value >" at the right of a row.
void choiceValue(ImDrawList* dl, float right, float cy, const char* value, bool hot, u32 dot = 0) {
    const float span = U(220), ax0 = right - U(8) - span;
    const float ts = U(19), tw = gameMeasure(g_font.bold, ts, value) + (dot ? U(26) : 0.f);
    float x = ax0 + (span - tw) * 0.5f;
    if (dot) colourBall(dl, ImVec2(x + U(9), cy), U(8), dot), x += U(26);
    gameText(dl, g_font.bold, ts, x, cy, col(kInk), value);
    const float t = U(8);
    const bool hl = hot && g_m.pos.x < ax0 + span * 0.5f;
    dl->AddTriangleFilled(ImVec2(ax0 + U(4), cy), ImVec2(ax0 + U(4) + t * 1.4f, cy - t), ImVec2(ax0 + U(4) + t * 1.4f, cy + t),
                          col(hl ? kWhite : kInk));
    const float ax1 = ax0 + span;
    dl->AddTriangleFilled(ImVec2(ax1, cy), ImVec2(ax1 - t * 1.4f, cy + t), ImVec2(ax1 - t * 1.4f, cy - t), col(hot && !hl ? kWhite : kInk));
}

void toggleValue(ImDrawList* dl, float right, float cy, bool on, bool focus, const void* key) {
    const float k = glide(key, on ? 1.f : 0.f, 18.f);
    const float w = U(76), h = U(28);
    const ImVec2 a(right - w, cy - h * 0.5f), b(right, cy + h * 0.5f);
    dl->AddRectFilled(ImVec2(a.x - U(2.5f), a.y - U(2.5f)), ImVec2(b.x + U(2.5f), b.y + U(2.5f)), col(kInk), h);
    dl->AddRectFilled(a, b, col(mixRgb(0xE9E9E9, kPlum, k)), h);
    const float kx = a.x + h * 0.5f + (w - h) * k;
    const char* word = on ? "ON" : "OFF";
    const float ws = U(14), ww = gameMeasure(g_font.bold, ws, word);
    gameText(dl, g_font.bold, ws, on ? a.x + U(12) : b.x - U(12) - ww, cy, col(on ? kWhite : 0x555555), word);
    dl->AddCircleFilled(ImVec2(kx, cy), h * 0.5f + U(1), col(kInk), 24);
    dl->AddCircleFilled(ImVec2(kx, cy), h * 0.5f - U(2), col(focus && on ? kWhite : on ? kOrange : 0xFAFAFA), 24);
}

// ---------------------------------------------------------------------------------------------------
// Form rows (the forms are columns of these)

struct Form {
    ImDrawList* dl;
    float x0, x1, y, rowH = U(44), pitch = U(56);
    float slide;
};

void rowField(Form& fm, const char* label, Field& fd, const char* help, std::function<void()> submit) {
    const int idx = nextIdx();
    const bool focus = isFocus(idx);
    const ImVec2 a(fm.x0 + fm.slide, fm.y), b(fm.x1 + fm.slide, fm.y + fm.rowH);
    const float lit = glide(itemKey(idx), focus ? 1.f : 0.f, 20.f);
    const float right = formRow(fm.dl, a.x, a.y, b.x, b.y, label, lit, hover(a, b));
    fieldBox(fm.dl, ImVec2(right - U(330), a.y + U(7)), ImVec2(right, b.y - U(7)), fd, focus && !g.osk);
    Item it{a, b};
    it.field = &fd, it.submit = std::move(submit), it.help = help;
    add(std::move(it));
    fm.y += fm.pitch;
}

void rowButton(Form& fm, const char* label, const char* help, std::function<void()> ok, bool enabled = true, const char* value = nullptr,
               bool busy = false) {
    const int idx = nextIdx();
    const bool focus = isFocus(idx);
    const ImVec2 a(fm.x0 + fm.slide, fm.y), b(fm.x1 + fm.slide, fm.y + fm.rowH);
    const float lit = glide(itemKey(idx), focus ? 1.f : 0.f, 20.f);
    const float right = formRow(fm.dl, a.x, a.y, b.x, b.y, label, lit, enabled && hover(a, b), !enabled);
    const float cy = (a.y + b.y) * 0.5f;
    if (busy) spinner(fm.dl, ImVec2(right - U(12), cy), U(10), float(nowT()), kInk);
    else if (value) gameText(fm.dl, g_font.bold, U(18), right - gameMeasure(g_font.bold, U(18), value), cy, col(0x262626), value);
    else if (enabled) {  // an arrow: this does something
        const float t = U(8);
        fm.dl->AddTriangleFilled(ImVec2(right, cy), ImVec2(right - t * 1.4f, cy + t), ImVec2(right - t * 1.4f, cy - t), col(kInk, 0.75f));
    }
    Item it{a, b};
    it.enabled = enabled, it.ok = std::move(ok), it.help = help;
    add(std::move(it));
    fm.y += fm.pitch;
}

void rowChoice(Form& fm, const char* label, const char* value, const char* help, std::function<void(int)> step, u32 dot = 0) {
    const int idx = nextIdx();
    const bool focus = isFocus(idx);
    const ImVec2 a(fm.x0 + fm.slide, fm.y), b(fm.x1 + fm.slide, fm.y + fm.rowH);
    const float lit = glide(itemKey(idx), focus ? 1.f : 0.f, 20.f);
    const bool hot = hover(a, b);
    const float right = formRow(fm.dl, a.x, a.y, b.x, b.y, label, lit, hot);
    choiceValue(fm.dl, right, (a.y + b.y) * 0.5f, value, hot, dot);
    Item it{a, b};
    it.step = std::move(step), it.help = help, it.split = right - U(8) - U(110);
    add(std::move(it));
    fm.y += fm.pitch;
}

void rowToggle(Form& fm, const char* label, bool& v, const char* help) {
    const int idx = nextIdx();
    const bool focus = isFocus(idx);
    const ImVec2 a(fm.x0 + fm.slide, fm.y), b(fm.x1 + fm.slide, fm.y + fm.rowH);
    const float lit = glide(itemKey(idx), focus ? 1.f : 0.f, 20.f);
    const float right = formRow(fm.dl, a.x, a.y, b.x, b.y, label, lit, hover(a, b));
    toggleValue(fm.dl, right, (a.y + b.y) * 0.5f, v, focus, itemKey(idx, 1));
    Item it{a, b};
    bool* p = &v;
    it.ok = [p] { *p = !*p; }, it.step = [p](int) { *p = !*p; }, it.help = help;
    add(std::move(it));
    fm.y += fm.pitch;
}

void rowText(Form& fm, const char* label, const std::string& value, const char* help, u32 dot = 0) {
    const int idx = nextIdx();
    const bool focus = isFocus(idx);
    const ImVec2 a(fm.x0 + fm.slide, fm.y), b(fm.x1 + fm.slide, fm.y + fm.rowH);
    const float lit = glide(itemKey(idx), focus ? 1.f : 0.f, 20.f);
    const float right = formRow(fm.dl, a.x, a.y, b.x, b.y, label, lit, hover(a, b));
    const float cy = (a.y + b.y) * 0.5f, ts = U(18);
    const std::string v = fit(g_font.bold, ts, value, U(330));
    const float w = gameMeasure(g_font.bold, ts, v.c_str());
    if (dot) colourBall(fm.dl, ImVec2(right - w - U(18), cy), U(8), dot);
    gameText(fm.dl, g_font.bold, ts, right - w, cy, col(0x262626), v.c_str());
    Item it{a, b};
    it.help = help;
    add(std::move(it));
    fm.y += fm.pitch;
}

// ---------------------------------------------------------------------------------------------------
// Moving between screens

void go(Screen to) {
    if (g.screen == to) return;
    g.screen = to;
    g.screenAt = nowT();
    g.focus[int(to)] = 0;
    if (to == Screen::Browse) g.browseTop = 0;
    if (to == Screen::Lobby) g.membersTop = 0, g.chatBack = 0, g.settingsTop = 0;
}

void openModal(std::string title, std::string body, std::vector<Button> buttons, std::function<void()> cancel = nullptr,
               Field* field = nullptr, std::string fieldLabel = {}) {
    g.modal = Modal{};
    g.modal.open = true;
    g.modal.title = std::move(title);
    g.modal.text = std::move(body);
    g.modal.buttons = std::move(buttons);
    g.modal.cancel = cancel ? std::move(cancel) : [] { g.modal.open = false; };
    g.modal.field = field;
    g.modal.fieldLabel = std::move(fieldLabel);
    g.modal.at = nowT();
}
void closeModal() { g.modal.open = false; }

void openOsk(Field* fd, const std::string& label, std::function<void()> done) {
    g.osk = true;
    g.oskField = fd;
    g.oskLabel = label;
    g.oskDone = std::move(done);
    g.oskAt = nowT();
    g.oskShift = false;
}

void openMenu() {
    if (!g.open) g.openPending = true;
}
void closeMenu() {
    if (g.open) g.closePending = true;
}

// Presses for the game's LAN screen (after the menu closes and everything held is let go).
void press(u16 buttons, double wait, double hold = 0.15) { g.presses.push_back({buttons, wait, hold}); }

// Back out of Online: the game leaves its LAN screen for Select Mode (B there), covered until it has.
void leaveOnline() {
    closeMenu();
    if (!g.lanFlow) return;
    g.lanFlow = false;
    g.leaving = true, g.leavingAt = nowT();
    const game::Where w = game::where();
    log("[onlinemenu] back to Select Mode (the game: %s)\n", game::whereName(w));
    if (w == game::Where::LanSystems || w == game::Where::ChoosingOnline) press(PAD_BUTTON_B, w == game::Where::ChoosingOnline ? 0.5 : 0.2);
    else game::leave();  // (a session parked between matches: lansession takes the game back to Select Mode)
}

// ---------------------------------------------------------------------------------------------------
// Checking forms

bool validUsername(const std::string& u) {
    if (int(u.size()) < an::kUsernameMin || int(u.size()) > an::kUsernameMax) return false;
    if (!std::isalnum(u8(u[0]))) return false;
    for (char c : u)
        if (!(std::isalnum(u8(c)) || c == '_' || c == '.' || c == '-')) return false;
    return true;
}

void doLogin() {
    if (!validUsername(f.user.text)) return toast("Usernames are 3-16 letters, digits, _ . - (starting with a letter or digit).");
    if (f.pass.text.empty()) return toast("Type your password.");
    net().login(f.user.text, f.pass.text, f.remember);
}
void doRegister() {
    if (!validUsername(f.user.text)) return toast("Usernames are 3-16 letters, digits, _ . - (starting with a letter or digit).");
    if (int(f.pass.text.size()) < an::kPasswordMin) return toast("Passwords are at least 6 characters.");
    if (f.pass.text != f.pass2.text) return toast("The two passwords differ.");
    net().registerAccount(f.user.text, f.pass.text, f.remember);
}
// (The official service's address is never shown or typed in the menus: it comes from the settings file. A dedicated
// server - the same server software, server/, run by anyone - is typed in Online Settings.)
void saveMaster() {
    if (f.dedicated) {
        if (f.master.text.empty()) return toast("Type the dedicated server's address: host or host:port.");
        settings::onlineServer.set(f.master.text);
    } else {
        settings::onlineServer.set("");
    }
    settings::save();
    toast(f.dedicated ? "Connecting to the dedicated server..." : "Connecting to the online service...", false);
    net().connect(an::serviceAddress());
}
bool savedLoginForService() {
    if (settings::onlineToken.get().empty()) return false;
    const std::string kept = settings::onlineTokenServer.get();
    return (kept.empty() ? settings::onlineMaster.get() : kept) == an::serviceAddress();
}

const an::Member* member(const std::string& name) {
    for (const an::Member& m : s.lobby.members)
        if (m.name == name) return &m;
    return nullptr;
}
int playersIn() {
    int n = 0;
    for (const an::Member& m : s.lobby.members) n += m.player ? 1 : 0;
    return n;
}

// The host's "start now": the countdown if none, else the match at once.
void startNow() {
    if (!s.isHost) return toast("The host starts the match.");
    if (playersIn() < 2) return toast("A match needs 2 players.");
    if (s.lobby.session == an::SessionState::Lobby || s.lobby.session == an::SessionState::Countdown) net().startMatch(true);
}

// The match the master started: the game plays it (src/runtime/online_game.h).
void beginMatch() {
    const an::MatchStart& m = s.match;
    g.matchFailed = false;
    if (m.myConsole < 0) return;
    game::Start st;
    st.consoles = int(m.consoles.size());
    st.me = m.myConsole;
    st.seed = m.seed;
    st.startAtMs = m.startAtMs;
    st.settingsJson = m.settingsJson;
    log("[onlinemenu] match %u: console %d of %d\n", m.id, st.me, st.consoles);
    if (!game::begin(st)) {
        g.matchFailed = true;
        toast("Online matches cannot start in this build yet.");
        return;
    }
    g.inMatch = true, g.matchAt = nowT();
    g.cover = false;
    closeMenu();
}

// The game's match is over (its results), or the session moved on without it: back to the session.
void endMatch(const std::string* results) {
    if (results) net().endMatch(*results);
    else game::abort();
    g.inMatch = false;
    g.cover = true;
    g.screen = Screen::Lobby, g.screenAt = nowT();
    openMenu();
}

void viewProfile(const std::string& name) {
    g.viewing = name;
    if (name == s.me.username) {
        g.profileFrom = g.screen;
        go(Screen::Profile);
        return;
    }
    net().viewProfile(name);
    g.profileFrom = g.screen;
    go(Screen::ViewProfile);
}

void memberMenu(const an::Member& m) {
    const std::string name = m.name;
    const bool self = name == s.me.username;
    std::vector<Button> b;
    b.push_back({self ? "Edit Profile" : "View Profile", [name] { closeModal(), viewProfile(name); }});
    if (s.isHost && !self)
        b.push_back({"Kick", [name] {
                         openModal("Kick " + name + "?", "They leave this server and cannot come back to it.",
                                   {{"Kick", [name] { net().kick(name), closeModal(); }}, {"Cancel", [] { closeModal(); }}});
                     }});
    b.push_back({"Cancel", [] { closeModal(); }});
    std::string body = std::string(an::colourName(m.colour)) + " Kirby";
    if (!m.machine.empty()) body += std::string(", ") + an::machineName(m.machine);
    body += ".";
    if (m.host) body += " Hosts this server.";
    openModal(name, body, std::move(b));
}

// ---------------------------------------------------------------------------------------------------
// The frame every screen shares: the field, the title, the disc, the band

struct Frame {
    ImDrawList* dl;
    float W, H, now, appear, slide;  // slide: the page coming in from the right
    std::string help;                // the band's text
    std::vector<std::pair<const char*, const char*>> hints;  // (button, what) at the foot
};

void backdrop(Frame& fr) {
    ImDrawList* dl = fr.dl;
    const float a = fr.appear;
    const float back = g.cover ? 1.f : std::min(1.f, a * 1.6f);  // (covering the game: opaque from the first frame)
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(fr.W, fr.H), col(kPlum, back));
    const float in = easeOut(a);
    const float sx = -(1.f - in) * fr.W * 0.6f;
    const ImVec2 fa(-U(80) + sx, -U(80)), fb(X(1150) + sx, YB(578));
    std::vector<ImVec2> pts;
    roundedRect(pts, fa, fb, 0, U(130), U(250), 0);
    dl->AddConvexPolyFilled(pts.data(), int(pts.size()), col(kOnline.field));
    dl->PushClipRect(ImVec2(std::max(0.f, fa.x), 0), fb, true);
    {
        const u32 light = mixRgb(kOnline.field, kWhite, 0.35f);
        const float spin = fr.now * 0.07f;
        const ImVec2 rc(X(520) + sx, Y(330));
        // the game's toothed rings, turning slowly (drawn in their place until its art has loaded)
        if (!artRing(dl, gameart::Art::Gear, rc, U(420), col(light, 0.3f), spin)) {
            dl->AddCircle(rc, U(330), col(light, 0.3f), 96, U(40));
            dl->AddCircle(rc, U(240), col(light, 0.2f), 80, U(14));
            for (int i = 0; i < 14; i++) {
                const float an_ = spin + float(i) * kPi / 7.f;
                dl->AddCircleFilled(ImVec2(rc.x + std::cos(an_) * U(392), rc.y + std::sin(an_) * U(392)), U(22), col(light, 0.22f), 20);
            }
        }
        if (!artRing(dl, gameart::Art::Gear, ImVec2(X(60) + sx, YB(600)), U(300), col(light, 0.22f), -spin * 1.3f))
            dl->AddCircle(ImVec2(X(60) + sx, YB(600)), U(260), col(light, 0.2f), 80, U(26));
        // meridians of a faint big globe behind everything
        for (int k = 0; k < 5; k++) {
            const float ph = std::fmod(fr.now * 0.05f + float(k) * kPi / 5.f, kPi);
            dl->AddEllipse(rc, ImVec2(U(330) * std::fabs(std::cos(ph)) + U(1), U(330)), col(light, 0.12f), 0.f, 64, U(6));
        }
        for (int i = 0; i < 3; i++) {
            const float x = X(380) + sx + float(i) * U(160);
            const ImVec2 q[4] = {ImVec2(x, fa.y), ImVec2(x + U(70 + 30 * i), fa.y), ImVec2(x - U(170 - 40 * i), Y(170)),
                                 ImVec2(x - U(210 - 20 * i), Y(170))};
            dl->AddConvexPolyFilled(q, 4, col(kWhite, 0.07f));
        }
        for (int i = 0; i < 30; i++) {
            const float x = X(0) + sx + std::fmod(float(i) * U(173.3f) + U(40), U(1150));
            const float y = YT(0) + std::fmod(float(i * i) * U(41.7f) + U(30), U(560 + 2 * g_ext));  // (over the whole field)
            const float r = U(5 + (i % 3) * 3);
            const ImVec2 q[4] = {ImVec2(x, y - r), ImVec2(x + r, y), ImVec2(x, y + r), ImVec2(x - r, y)};
            dl->AddConvexPolyFilled(q, 4, col(light, 0.28f));
        }
    }
    dl->PopClipRect();
    maskCorners(dl, fa, fb, 0, U(130), U(250), 0, col(kPlum, back));
    // the strip along the bottom
    dl->AddRectFilled(ImVec2(0, YB(684)), ImVec2(fr.W, fr.H), col(kDeepPlum, back));
}

void title(Frame& fr, const std::string& name) {
    ImDrawList* dl = fr.dl;
    const float size = U(26), ty = YT(93);
    const float x = X(205) - (1.f - easeOut(fr.appear)) * U(300);
    const float tw = gameMeasure(g_font.bold, size, name.c_str());
    const float base = ty + U(17), high = ty - U(3), w = U(3.5f);
    // (the game's step in the line, once its art has loaded)
    if (artTitleStep(dl, x + tw + U(12), base + w * 0.5f, high - w * 0.5f, col(kTitleInk, 0.85f))) {
        const float stepW = (base - high + w) / (16.f / 24.f) * 32.f / 24.f;
        dl->AddLine(ImVec2(X(-40), base), ImVec2(x + tw + U(13), base), col(kTitleInk, 0.85f), w);
        dl->AddLine(ImVec2(x + tw + U(11) + stepW, high), ImVec2(X(795), high), col(kTitleInk, 0.85f), w);
    } else {
        const ImVec2 line[4] = {ImVec2(X(-40), base), ImVec2(x + tw + U(16), base), ImVec2(x + tw + U(38), high), ImVec2(X(795), high)};
        dl->AddPolyline(line, 4, col(kTitleInk, 0.85f), ImDrawFlags_None, w);
    }
    dl->AddLine(ImVec2(X(955), high), ImVec2(X(1250), high), col(kTitleInk, 0.85f), w);
    gameText(dl, g_font.bold, size, x, ty, col(kTitleInk), name.c_str());
}

// The status at the top right: who is signed in, how many are online, the relay.
void statusChip(Frame& fr) {
    ImDrawList* dl = fr.dl;
    u32 dot = kGrey;
    std::string t;
    switch (s.state) {
    case an::State::Online:
        dot = kGreen;
        t = s.me.username;
        if (s.usersOnline > 0) t += "  \xC2\xB7  " + std::to_string(s.usersOnline) + " online";
        break;
    case an::State::Connecting: dot = kYellow, t = "Connecting"; break;
    case an::State::LoggingIn: dot = kYellow, t = "Signing in"; break;
    case an::State::Reconnecting: dot = kYellow, t = "Reconnecting"; break;
    case an::State::Connected: dot = 0x7FB3E0, t = "Not signed in"; break;
    default: t = "Offline"; break;
    }
    if (s.inLobby && s.player) {
        char b[48];
        if (s.linkUp) std::snprintf(b, sizeof b, "  \xC2\xB7  relay %d ms", std::max(0, s.pingMs));
        else std::snprintf(b, sizeof b, "  \xC2\xB7  relay...");
        t += b;
    }
    const float size = U(17), tw = gameMeasure(g_font.bold, size, t.c_str());
    const float h = U(34), right = X(1248), w = tw + U(46) + (s.busy ? U(24) : 0.f);
    const ImVec2 a(right - w, YT(18) - (1.f - easeOut(fr.appear)) * U(80)), b(right, a.y + h);
    dl->AddRectFilled(ImVec2(a.x - U(2), a.y - U(2)), ImVec2(b.x + U(2), b.y + U(2)), col(kPlumLight), h);
    dl->AddRectFilled(a, b, col(0x221E2A), h);
    const float cy = a.y + h * 0.5f;
    dl->AddCircleFilled(ImVec2(a.x + U(18), cy), U(6.5f), col(dot), 16);
    gameText(dl, g_font.bold, size, a.x + U(32), cy, col(kCream), t.c_str());
    if (s.busy) spinner(dl, ImVec2(b.x - U(20), cy), U(8), fr.now);
}

void band(Frame& fr) {
    ImDrawList* dl = fr.dl;
    const float bx0 = X(275), bx1 = X(1075), bcy = YB(626), bh = U(54);
    const float dy = (1.f - easeOut(fr.appear)) * U(120);
    dl->AddRectFilled(ImVec2(bx0, bcy - bh * 0.5f + dy), ImVec2(bx1, bcy + bh * 0.5f + dy), col(kBand, 0.97f), bh * 0.5f);
    float ts = U(23);
    const float room = bx1 - bx0 - U(56);
    const float tw = gameMeasure(g_font.text, ts, fr.help.c_str());
    if (tw > room) ts *= room / tw;
    gameText(dl, g_font.text, ts, bx0 + U(30), bcy + dy, col(kBandText), fr.help.c_str());
    // the buttons, along the strip at the foot
    float hx = X(1250);
    const float hy = YB(703), hs = U(15);
    for (int i = int(fr.hints.size()) - 1; i >= 0; i--) {
        const auto [button, what] = fr.hints[i];
        hx -= gameMeasure(g_font.text, hs, what);
        gameText(dl, g_font.text, hs, hx, hy, col(kSlateText), what);
        hx -= what[0] ? U(28) : U(22);
        const u32 face = !std::strcmp(button, "A") ? 0x2FA37A : !std::strcmp(button, "B") ? 0xD0453B : 0x6B7280;
        dl->AddCircleFilled(ImVec2(hx + U(11), hy), U(11), col(face), 20);
        gameText(dl, g_font.bold, U(13), hx + U(11) - gameMeasure(g_font.bold, U(13), button) * 0.5f, hy, col(kWhite), button);
        hx -= U(18);
    }
    gameText(dl, g_font.text, hs, X(30), hy, col(0x8E8796), "F1  Settings");
}

// ---------------------------------------------------------------------------------------------------
// The screens

void screenConnecting(Frame& fr) {
    ImDrawList* dl = fr.dl;
    const ImVec2 c(X(500) + fr.slide, Y(300));
    spinner(dl, c, U(26), fr.now, kInk);
    const char* what = s.state == an::State::LoggingIn ? "Signing in..." : "Connecting to the online service...";
    const float size = U(30);
    text(dl, g_font.label, size, c.x - measure(g_font.label, size, what).x * 0.5f, c.y + U(64), col(kInk), what);
    Form fm{dl, X(330), X(670), Y(470), U(44), U(56), fr.slide};
    rowButton(fm, "Cancel", "Stop trying and sign in another way.", [] {
        net().disconnect();
        go(Screen::SignIn);
    });
    fr.help = "Signing in to play Kirby Air Ride online.";
}

void screenSignIn(Frame& fr) {
    Form fm{fr.dl, X(150), X(860), Y(140), U(44), U(56), fr.slide};
    const std::string saved = settings::onlineUser.get();
    if (s.savedLogin && !saved.empty()) {
        rowButton(fm, ("Continue as " + saved).c_str(), "Sign in with the login remembered on this PC.", [] { net().resumeSaved(); }, true,
                  nullptr, s.state == an::State::LoggingIn);
    }
    rowField(fm, "Username", f.user, "Your account's name: 3-16 letters, digits, _ . -", [] { g.focus[int(g.screen)]++; });
    rowField(fm, "Password", f.pass, "Your account's password.", doLogin);
    rowToggle(fm, "Remember Me", f.remember, "Stay signed in on this PC: the next time, Online signs in by itself.");
    rowButton(fm, "Log In", "Sign in to the online service.", doLogin, true, nullptr, s.state == an::State::LoggingIn);
    rowButton(fm, "Create Account", "New here? Make an account: a name and a password are all it takes.", [] {
        f.pass2.text.clear();
        go(Screen::Register);
    });
    rowButton(fm, "Settings", "Your connection to the online service.", [] {
        g.settingsFrom = Screen::SignIn;
        f.master.text = settings::onlineServer.get();
        f.dedicated = !f.master.text.empty();
        go(Screen::Settings);
    });
    rowButton(fm, "Back", "Leave Online.", leaveOnline);
    fr.help = settings::onlineServer.get().empty() ? "Sign in to play online."
                                                   : "Sign in to the dedicated server " + settings::onlineServer.get() + " (Settings: the official service).";
}

void screenRegister(Frame& fr) {
    Form fm{fr.dl, X(150), X(860), Y(140), U(44), U(56), fr.slide};
    rowField(fm, "Username", f.user, "Choose a name: 3-16 letters, digits, _ . - (others see it).", [] { g.focus[int(g.screen)]++; });
    rowField(fm, "Password", f.pass, "At least 6 characters. It never leaves this PC: the service keeps only a check of it.",
             [] { g.focus[int(g.screen)]++; });
    rowField(fm, "Confirm Password", f.pass2, "The same password again.", doRegister);
    rowToggle(fm, "Remember Me", f.remember, "Stay signed in on this PC.");
    rowButton(fm, "Create Account", "Make the account and sign in.", doRegister, true, nullptr, s.busy);
    rowButton(fm, "Back", "Back to signing in.", [] { go(Screen::SignIn); });
    fr.help = "Create an account.";
}

void screenMain(Frame& fr) {
    ImDrawList* dl = fr.dl;
    struct B {
        const char* label;
        const char* help;
        float x, w;
        bool pill;
        std::function<void()> act;
    };
    const B bs[] = {
        {"Browse Servers", "Find a server to join, and the players in it.", 265, 520, false, [] {
             go(Screen::Browse);
             net().refreshServers(an::Mode(g.filter));
             g.refreshedAt = nowT();
         }},
        {"Host Server", "Open your own server: up to 16 people, up to 4 playing each match.", 190, 500, false, [] {
             if (f.hostName.text.empty()) f.hostName.text = s.me.username + "'s server";
             go(Screen::Host);
         }},
        {"Profile", "Your Kirby's colour, your favourite machine, your matches.", 300, 420, false, [] {
             g.profileFrom = Screen::Main;
             go(Screen::Profile);
         }},
        {"Settings", "Your connection to the online service; your password.", 245, 380, true, [] {
             g.settingsFrom = Screen::Main;
             f.master.text = settings::onlineServer.get();
             f.dedicated = !f.master.text.empty();
             go(Screen::Settings);
         }},
        {"Log Out", "Sign out, and forget the login on this PC.", 285, 360, true, [] {
             openModal("Log out?", "You will sign in again the next time you play online.",
                       {{"Log Out", [] { net().logout(), closeModal(); }}, {"Cancel", [] { closeModal(); }}});
         }},
        {"Back", "Leave Online (you stay signed in).", 225, 300, true, leaveOnline},
    };
    for (int i = 0; i < 6; i++) {
        const B& b = bs[i];
        const float y0 = Y(138 + 72 * i), y1 = y0 + U(b.pill ? 56 : 58);
        const float x0 = X(b.x) + fr.slide * (1.f + 0.15f * float(i)), x1 = x0 + U(b.w);
        const int idx = nextIdx();
        const bool focus = isFocus(idx), hot = hover(ImVec2(x0, y0), ImVec2(x1, y1));
        const float lit = glide(itemKey(idx), focus ? 1.f : 0.f, 18.f);
        if (b.pill) pillButton(dl, x0, y0, x1, y1, b.label, lit, hot);
        else ballButton(dl, x0, y0, x1, y1, b.label, lit, hot, true);
        Item it{ImVec2(x0, y0), ImVec2(x1, y1)};
        it.ok = b.act, it.help = b.help;
        add(std::move(it));
    }
    fr.help = "Play Kirby Air Ride online!";
    static std::string s_motdShown;  // (the message of the day, once)
    if (!s.motd.empty() && s.motd != s_motdShown) s_motdShown = s.motd, toast(s.motd, false);
}

void screenBrowse(Frame& fr) {
    ImDrawList* dl = fr.dl;
    const float now = fr.now;
    if (now - g.refreshedAt > 10.0 && !s.busy) net().refreshServers(an::Mode(g.filter)), g.refreshedAt = now;
    // the filter, as tabs (L / R)
    static const char* kTabs[] = {"All", "Air Ride", "City Trial"};
    static const ModeColour kTabColours[] = {kOnline, kAirRide, kCityTrial};
    float tx = X(140) + fr.slide;
    const float tcy = Y(152);
    for (int i = 0; i < 3; i++) {
        const float size = U(18), w = gameMeasure(g_font.bold, size, kTabs[i]) + U(42);
        const ImVec2 a(tx, tcy - U(17)), b(tx + w, tcy + U(17));
        const bool on = i == g.filter, hot = hover(a, b);
        if (hot && g_m.clicked && !on) g.filter = i, net().refreshServers(an::Mode(i)), g.refreshedAt = now;
        const float lit = glide(&kTabs[i], on ? 1.f : 0.f);
        if (lit > 0.01f) capsule(dl, a.x - U(2.5f), a.y - U(2.5f), b.x + U(2.5f), b.y + U(2.5f), 0.f, true, col(kWhite, lit));
        capsule(dl, a.x, a.y, b.x, b.y, 0.f, true, col(mixRgb(hot ? 0x5E6878 : kSlate, kTabColours[i].field, lit)));
        gameText(dl, g_font.bold, size, a.x + U(21), tcy, col(mixRgb(kSlateText, 0x1E1E1E, lit)), kTabs[i]);
        tx = b.x + U(12);
    }
    {  // Refresh (X), by the mouse too
        const char* label = "Refresh";
        const float size = U(17), w = gameMeasure(g_font.bold, size, label) + U(56);
        const ImVec2 a(X(880) - w + fr.slide, tcy - U(17)), b(X(880) + fr.slide, tcy + U(17));
        const bool hot = hover(a, b);
        if (hot && g_m.clicked) net().refreshServers(an::Mode(g.filter)), g.refreshedAt = now;
        capsule(dl, a.x, a.y, b.x, b.y, 0.f, true, col(hot ? 0x5E6878 : kSlate));
        dl->AddCircleFilled(ImVec2(a.x + U(19), tcy), U(11), col(0x6B7280), 20);
        dl->AddCircle(ImVec2(a.x + U(19), tcy), U(11), col(kWhite, 0.6f), 20, U(1.5f));
        gameText(dl, g_font.bold, U(13), a.x + U(19) - gameMeasure(g_font.bold, U(13), "X") * 0.5f, tcy, col(kWhite), "X");
        gameText(dl, g_font.bold, size, a.x + U(38), tcy, col(kSlateText), label);
        if (s.busy) spinner(dl, ImVec2(a.x - U(22), tcy), U(9), now, kInk);
    }
    // the servers
    const float x0 = X(130) + fr.slide, x1 = X(880) + fr.slide, top = Y(190), rowH = U(52), pitch = U(60);
    const int fits = 6, n = int(s.servers.size());
    if (n == 0) {
        const char* none = s.busy ? "Looking for servers..." : "No servers are open right now.";
        text(dl, g_font.label, U(26), x0 + U(40), top + U(50), col(kInk), none);
        Form fm{dl, X(150), X(620), top + U(110), U(44), U(56), fr.slide};
        rowButton(fm, "Host a Server", "Open your own server for others to join.", [] {
            if (f.hostName.text.empty()) f.hostName.text = s.me.username + "'s server";
            go(Screen::Host);
        });
    }
    const int first = nextIdx();
    int fRow = curFocus() - first;
    if (fRow >= 0 && fRow < n) g.browseTop = std::clamp(g.browseTop, fRow - fits + 1, fRow);
    if (g_live && g_m.wheel != 0 && g_m.in(ImVec2(x0, top), ImVec2(x1, top + pitch * fits))) g.browseTop -= g_m.wheel > 0 ? 1 : -1;
    g.browseTop = std::clamp(g.browseTop, 0, std::max(0, n - fits));
    dl->PushClipRect(ImVec2(X(0), top - U(12)), ImVec2(X(1150), top + pitch * fits), true);
    for (int i = 0; i < n; i++) {
        const an::ServerInfo sv = s.servers[size_t(i)];
        const bool shown = i >= g.browseTop && i < g.browseTop + fits;
        const float y0 = top + float(i - g.browseTop) * pitch, y1 = y0 + rowH, cy = (y0 + y1) * 0.5f;
        const ImVec2 a(x0, y0), b(x1, y1);
        const int idx = nextIdx();
        const bool focus = isFocus(idx);
        const bool full = sv.members >= sv.max;
        if (shown) {
            const float lit = glide(itemKey(idx), focus ? 1.f : 0.f, 20.f);
            const bool hot = hover(a, b);
            float right = formRow(dl, x0, y0, x1, y1, "", lit, hot, full);
            // name and host
            const std::string name = fit(g_font.label, U(22), sv.name, U(330));
            text(dl, g_font.label, U(22), x0 + U(30), cy - U(7), col(full ? 0x5A5A5A : kInk), name.c_str());
            gameText(dl, g_font.text, U(15), x0 + U(32), cy + U(13), col(0x3C3C3C), ("hosted by " + sv.host + (sv.region.empty() ? "" : "  \xC2\xB7  " + sv.region)).c_str());
            // at the right: ping, mode, players, lock
            char buf[48];
            if (sv.pingMs >= 0) std::snprintf(buf, sizeof buf, "%d ms", sv.pingMs);
            else std::snprintf(buf, sizeof buf, "-");
            const float ps = U(16);
            right -= gameMeasure(g_font.bold, ps, buf);
            gameText(dl, g_font.bold, ps, right, cy, col(sv.pingMs < 0 ? 0x666666 : sv.pingMs < 80 ? 0x1F7A35 : sv.pingMs < 160 ? 0x8A6A10 : 0x9A2A20), buf);
            right -= U(22);
            const char* mode = an::modeName(sv.mode);
            const float mw = gameMeasure(g_font.bold, U(14), mode) + U(20);
            const u32 mc = sv.mode == an::Mode::AirRide ? kAirRide.field : sv.mode == an::Mode::CityTrial ? kCityTrial.field : 0x9AA3B0;
            dl->AddRectFilled(ImVec2(right - mw, cy - U(12)), ImVec2(right, cy + U(12)), col(mc), U(12));
            gameText(dl, g_font.bold, U(14), right - mw + U(10), cy, col(0x1E1E1E), mode);
            right -= mw + U(20);
            std::snprintf(buf, sizeof buf, "%d/%d", sv.members, sv.max);
            right -= gameMeasure(g_font.bold, U(19), buf);
            gameText(dl, g_font.bold, U(19), right, cy, col(full ? 0x9A2A20 : kInk), buf);
            if (sv.session != an::SessionState::Lobby) {
                std::snprintf(buf, sizeof buf, "%s", sv.session == an::SessionState::InMatch ? "In a match"
                                                     : sv.session == an::SessionState::Countdown ? "Starting" : "Results");
                right -= gameMeasure(g_font.text, U(14), buf) + U(14);
                gameText(dl, g_font.text, U(14), right, cy, col(0x3C3C3C), buf);
            }
            if (sv.locked) padlock(dl, ImVec2(right - U(22), cy), U(10), col(kInk));
        }
        Item it{shown ? a : ImVec2(0, 0), shown ? b : ImVec2(0, 0)};
        it.help = sv.locked ? "A password is needed to join. " : "";
        it.help += "Join " + sv.name + " (" + an::modeName(sv.mode) + ").";
        it.ok = [sv] {
            if (sv.members >= sv.max) return toast("That server is full.");
            if (sv.locked) {
                f.joinPass.text.clear();
                const uint32_t id = sv.id;
                openModal("Join " + sv.name, "This server is locked: type its password.",
                          {{"Join", [id] { net().join(id, f.joinPass.text), closeModal(); }}, {"Cancel", [] { closeModal(); }}}, nullptr,
                          &f.joinPass, "Password");
                return;
            }
            net().join(sv.id);
        };
        add(std::move(it));
    }
    dl->PopClipRect();
    const float mid = (x0 + x1) * 0.5f;
    if (g.browseTop > 0)
        dl->AddTriangleFilled(ImVec2(mid, top - U(18)), ImVec2(mid - U(10), top - U(8)), ImVec2(mid + U(10), top - U(8)), col(kInk, 0.7f));
    if (g.browseTop + fits < n) {
        const float yb = top + pitch * fits;
        dl->AddTriangleFilled(ImVec2(mid, yb + U(8)), ImVec2(mid - U(10), yb - U(2)), ImVec2(mid + U(10), yb - U(2)), col(kInk, 0.7f));
    }
    char h[96];
    std::snprintf(h, sizeof h, "%d server%s open, %d player%s online.", n, n == 1 ? "" : "s", s.usersOnline, s.usersOnline == 1 ? "" : "s");
    fr.help = h;
    fr.hints = {{"A", "Join"}, {"B", "Back"}, {"X", "Refresh"}, {"L", ""}, {"R", "Mode"}};
}

void screenHost(Frame& fr) {
    Form fm{fr.dl, X(150), X(860), Y(126), U(42), U(51), fr.slide};
    rowField(fm, "Server Name", f.hostName, "Its name in the server list.", [] { g.focus[int(g.screen)]++; });
    char maxv[16];
    std::snprintf(maxv, sizeof maxv, "%d people", f.hostMax);
    rowChoice(fm, "Server Size", maxv, "How many may be in the server (2-16). Those past a match's players wait for the next match.",
              [](int d) { f.hostMax = std::clamp(f.hostMax + d, 2, an::kLobbyMax); });
    char playv[16];
    std::snprintf(playv, sizeof playv, "%d players", f.hostPlayers);
    rowChoice(fm, "Match Players", playv,
              morepl::active() ? "How many play in each match (2-8; more than 4: City Trial)." : "How many play in each match (2-4).",
              [](int d) {
                  f.hostPlayers = std::clamp(f.hostPlayers + d, 2, maxMatchPlayers());
                  if (f.hostPlayers > 4) f.hostMode = game::kCityTrial;  // (Air Ride: 4 riders still)
              });
    rowField(fm, "Password", f.hostPass, "Optional: only those who know it can join. Empty: open to everyone.", [] { g.focus[int(g.screen)]++; });
    rowChoice(fm, "Mode", game::modeName(f.hostMode), "Air Ride or City Trial (its rules are set in the server, and can change between matches).",
              [](int) {
                  if (f.hostPlayers > 4) return toast("Matches of more than 4 are City Trial.");
                  f.hostMode = f.hostMode == game::kAirRide ? game::kCityTrial : game::kAirRide;
              });
    rowToggle(fm, "Start by Itself", f.hostAuto, "On: a match counts down by itself once 2 are in. Off: you start each match.");
    rowButton(fm, "Create Server", "Open the server: you are in it at once.", [] {
        an::HostOptions o;
        o.name = f.hostName.text;
        o.maxMembers = f.hostMax;
        o.maxPlayers = f.hostPlayers;
        o.password = f.hostPass.text;
        o.mode = f.hostMode == game::kAirRide ? an::Mode::AirRide : an::Mode::CityTrial;
        o.autoStart = f.hostAuto;
        o.settingsJson = game::Settings::defaults(f.hostMode).toJson();
        net().host(o);
    }, true, nullptr, s.busy);
    rowButton(fm, "Back", "Back to the Online menu.", [] { go(Screen::Main); });
    fr.help = "Host a server.";
}

// What the session is doing, in words, for the band.
std::string sessionHelp() {
    const an::Lobby& L = s.lobby;
    switch (L.session) {
    case an::SessionState::Countdown: return s.player ? "The match is about to start!" : "A match is starting: you play in the next one.";
    case an::SessionState::InMatch:
        if (g.matchFailed) return "The match could not start on this PC - you join the next one.";
        return s.player ? "The match is on." : "A match is on: you play in the next one.";
    case an::SessionState::Results: return "The match is over. The next one comes up shortly.";
    default: break;
    }
    if (playersIn() < 2) return "Waiting for another player to join.";
    if (playersWithoutMap()) return "The match waits until every player has the stage's map.";
    if (L.hold || !L.autoStart) return s.isHost ? "Start the match when everyone is ready (X)." : "The host starts the match.";
    return "The match starts shortly.";
}

// How this console reaches a player's, in words.
std::string pathText(const an::Member& m) {
    if (m.name == s.me.username) return "you";
    if (m.away) return "away";
    if (lacksMap(m)) return "getting map";
    if (!m.player || !s.player) return m.pingMs >= 0 ? std::to_string(m.pingMs) + " ms" : "";
    char b[48];
    const char* how = m.path == an::Path::DirectLan ? "LAN" : m.path == an::Path::DirectInternet ? "Direct" : m.path == an::Path::Relay ? "Relay" : "...";
    if (m.peerRttMs >= 0) std::snprintf(b, sizeof b, "%s %d ms", how, m.peerRttMs);
    else std::snprintf(b, sizeof b, "%s", how);
    return b;
}
u32 pathColour(const an::Member& m) {
    if (m.away) return 0x8A8494;
    if (lacksMap(m) && m.name != s.me.username) return kYellow;
    if (!m.player || !s.player || m.name == s.me.username) return 0x9A93A6;
    if (m.path == an::Path::DirectLan || m.path == an::Path::DirectInternet) return 0x7FD08A;
    return 0xF2C14E;
}

// A row of the game settings: the label at the left, the value at the right (with arrows for the host).
void settingRow(ImDrawList* dl, float x0, float x1, float y, const char* label, const std::string& value, bool editable, const char* help,
                std::function<void(int)> step, bool shown = true) {
    const float h = U(32), cy = y + h * 0.5f;
    const ImVec2 a(x0, y), b(x1, y + h);
    const int idx = nextIdx();
    if (!shown) {  // scrolled out of sight: still in the focus order, not drawn (and not under the mouse)
        Item it{ImVec2(-1e6f, -1e6f), ImVec2(-1e6f, -1e6f)};
        it.col = 1, it.help = help;
        if (editable) it.step = std::move(step), it.split = -1e6f;
        add(std::move(it));
        return;
    }
    const bool focus = isFocus(idx), hot = editable && hover(a, b);
    const float lit = glide(itemKey(idx), focus ? 1.f : 0.f, 20.f);
    const u32 fill = mixRgb(editable ? (hot ? 0xD6D6D6 : 0xC4C4C4) : 0x9E9AA6, kOrange, lit);
    if (!artButton(dl, x0 - U(14), y - U(3), x1 + U(6), y + h + U(3), fill, 1.f, lit > 0.5f)) {
        capsule(dl, x0 - U(3), y - U(3), x1 + U(3), y + h + U(3), 0.3f, false, col(kInk));
        capsule(dl, x0, y, x1, y + h, 0.3f, false, col(fill));
    }
    gameText(dl, g_font.bold, U(15), x0 + U(22), cy, col(kInk), label);
    const float vs = U(15), right = x1 - U(26);
    const float vw = gameMeasure(g_font.bold, vs, value.c_str());
    gameText(dl, g_font.bold, vs, right - vw - (editable ? U(16) : 0.f), cy, col(0x1E1E1E), value.c_str());
    if (editable) {
        const float t = U(6), ax1 = right, ax0 = right - vw - U(32);
        dl->AddTriangleFilled(ImVec2(ax0, cy), ImVec2(ax0 + t * 1.3f, cy - t), ImVec2(ax0 + t * 1.3f, cy + t), col(kInk));
        dl->AddTriangleFilled(ImVec2(ax1, cy), ImVec2(ax1 - t * 1.3f, cy + t), ImVec2(ax1 - t * 1.3f, cy - t), col(kInk));
    }
    if (focus && lit > 0.5f) cornerMarks(dl, ImVec2(x0 - U(6), y - U(4)), ImVec2(x1 + U(6), y + h + U(4)), float(nowT()));
    Item it{a, b};
    it.col = 1, it.help = help;
    if (editable) it.step = std::move(step), it.split = (x0 + x1) * 0.5f;
    add(std::move(it));
}

void screenLobby(Frame& fr) {
    ImDrawList* dl = fr.dl;
    const float now = fr.now;
    const an::Lobby& L = s.lobby;
    {  // what the server is
        char b[160];
        std::snprintf(b, sizeof b, "hosted by %s  \xC2\xB7  %d/%d in the server  \xC2\xB7  %d players a match", L.host.c_str(), int(L.members.size()),
                      L.max, L.maxPlayers);
        float x = X(130) + fr.slide;
        if (L.locked) padlock(dl, ImVec2(x + U(8), Y(134)), U(9), col(kInk)), x += U(24);
        gameText(dl, g_font.bold, U(17), x, Y(134), col(0x1E2A36), b);
    }
    auto panel = [&](ImVec2 a, ImVec2 b, const char* head) {
        dl->AddRectFilled(ImVec2(a.x + U(5), a.y + U(6)), ImVec2(b.x + U(5), b.y + U(6)), col(0x000000, 0.18f), U(18));
        dl->AddRectFilled(a, b, col(0x2A2633, 0.86f), U(18));
        dl->AddRectFilled(a, ImVec2(b.x, a.y + U(32)), col(0x3A3446, 0.95f), U(18), ImDrawFlags_RoundCornersTop);
        gameText(dl, g_font.bold, U(17), a.x + U(16), a.y + U(16), col(kCream), head);
    };
    // ---- the players (column 0): this match's (in console order during one), then those waiting ----
    {
        const ImVec2 a(X(110) + fr.slide, Y(156)), b(X(430) + fr.slide, Y(566 + g_ext));  // (taller in 4:3)
        char head[48];
        std::snprintf(head, sizeof head, "Players  %d/%d", playersIn(), L.maxPlayers);
        panel(a, b, head);
        std::vector<an::Member> order;
        if (L.session == an::SessionState::InMatch || L.session == an::SessionState::Results)
            for (const std::string& n : L.matchPlayers)
                if (const an::Member* m = member(n)) order.push_back(*m);
        for (const an::Member& m : L.members)
            if (m.player && std::find_if(order.begin(), order.end(), [&](const an::Member& o) { return o.name == m.name; }) == order.end())
                order.push_back(m);
        const int nPlayers = int(order.size());
        for (const an::Member& m : L.members)
            if (std::find_if(order.begin(), order.end(), [&](const an::Member& o) { return o.name == m.name; }) == order.end()) order.push_back(m);
        const float top = a.y + U(42), pitch = U(30), rowH = U(26);
        const int fits = 11, n = int(order.size());
        const int first = nextIdx();
        const int fRow = curFocus() - first;
        if (fRow >= 0 && fRow < n) g.membersTop = std::clamp(g.membersTop, fRow - fits + 1, fRow);
        g.membersTop = std::clamp(g.membersTop, 0, std::max(0, n - fits));
        float y = top;
        for (int i = 0; i < n; i++) {
            const an::Member m = order[size_t(i)];
            const bool shown = i >= g.membersTop && i < g.membersTop + fits;
            if (shown && i == nPlayers && i > 0) {  // the line between this match's players and the rest
                gameText(dl, g_font.text, U(14), a.x + U(16), y + U(9), col(0x9A93A6), "Next match");
                y += U(22);
            }
            const float y0 = y, cy = y0 + rowH * 0.5f;
            const ImVec2 ra(a.x + U(10), y0), rb(b.x - U(10), y0 + rowH);
            const int idx = nextIdx();
            const bool focus = isFocus(idx);
            if (shown) {
                const bool hot = hover(ra, rb), me = m.name == s.me.username;
                const float lit = glide(itemKey(idx), focus ? 1.f : 0.f, 20.f);
                const u32 base = me ? 0x4A4458 : hot ? 0x433E50 : 0x35303F;
                dl->AddRectFilled(ra, rb, col(mixRgb(base, kOrange, lit)), rowH * 0.5f);
                if (i < nPlayers) {  // its console number
                    char nb[4];
                    std::snprintf(nb, sizeof nb, "%d", i + 1);
                    dl->AddCircleFilled(ImVec2(ra.x + U(13), cy), U(10), col(lit > 0.5f ? kInk : 0x5B6577), 16);
                    gameText(dl, g_font.bold, U(13), ra.x + U(13) - gameMeasure(g_font.bold, U(13), nb) * 0.5f, cy, col(kWhite), nb);
                }
                colourBall(dl, ImVec2(ra.x + U(36), cy), U(7.5f), kirbyRgb(m.colour));
                const u32 ink = lit > 0.5f ? kInk : m.away ? 0x8A8494 : kCream;
                const std::string nm = fit(g_font.bold, U(16), m.name, U(120));
                const float nw = gameText(dl, g_font.bold, U(16), ra.x + U(50), cy, col(ink), nm.c_str());
                if (m.host) crown(dl, ImVec2(ra.x + U(50) + nw + U(12), cy), U(6.5f));
                const std::string tag = pathText(m);
                const float tw = gameMeasure(g_font.text, U(14), tag.c_str());
                gameText(dl, g_font.text, U(14), rb.x - U(12) - tw, cy, col(lit > 0.5f ? kInk : pathColour(m)), tag.c_str());
                if (focus && lit > 0.5f) cornerMarks(dl, ImVec2(ra.x - U(3), ra.y - U(3)), ImVec2(rb.x + U(3), rb.y + U(3)), now);
                y += pitch;
            }
            Item it{shown ? ra : ImVec2(0, 0), shown ? rb : ImVec2(0, 0)};
            it.col = 0;
            std::string help = m.name + ": " + an::colourName(m.colour) + " Kirby";
            if (!m.machine.empty()) help += std::string(", ") + an::machineName(m.machine);
            if (m.player && s.player && m.name != s.me.username)
                help += m.path == an::Path::Relay ? ". Through the relay." : m.path == an::Path::None ? "." : ". Connected directly.";
            else help += m.player ? "." : ". Plays in the next match.";
            it.help = help;
            it.ok = [m] { memberMenu(m); };
            add(std::move(it));
        }
    }
    // ---- the session and the game settings (column 1) ----
    {
        const ImVec2 a(X(450) + fr.slide, Y(156)), b(X(860) + fr.slide, Y(566 + g_ext));  // (taller in 4:3)
        panel(a, b, "Next Match");
        const float ax0 = a.x + U(16), ax1 = b.x - U(16);
        float y = a.y + U(42);
        // the state: a headline, a line under it
        std::string head, sub;
        u32 headCol = kCream;
        const int64_t nowMs = an::clockMs();
        switch (L.session) {
        case an::SessionState::Countdown: {
            const int secs = int(std::max<int64_t>(0, (L.countdownEndsMs - nowMs + 999) / 1000));
            head = "Starting in " + std::to_string(secs);
            sub = s.player ? "Get ready!" : "You play in the next match.";
            headCol = kOrangeLit;
            break;
        }
        case an::SessionState::InMatch:
            head = "Match in progress";
            sub = !s.player ? "You play in the next match." : g.matchFailed ? "Could not join this match: you play the next one." : "Back here once it is over.";
            headCol = 0x7FD08A;
            break;
        case an::SessionState::Results: {
            head = "Results", headCol = kOrangeLit;
            const int secs = int(std::max<int64_t>(0, (L.resultsEndsMs - nowMs + 999) / 1000));
            if (L.resultsEndsMs > 0) sub = "The next match in " + std::to_string(secs) + " s.";
            break;
        }
        default:
            head = playersIn() < 2 ? "Waiting for players" : (L.hold || !L.autoStart) ? "Ready" : "Getting ready";
            sub = playersIn() < 2 ? "A match needs 2 players." : (L.hold || !L.autoStart) ? "The host starts the match." : "The countdown starts any moment.";
            if (const int n = playersWithoutMap(); n && playersIn() >= 2) {
                head = "Getting the map";
                sub = (n == 1 ? std::string("A player is") : std::to_string(n) + " players are") + " still getting " + lobbyStage().title + ".";
            }
            break;
        }
        gameText(dl, g_font.bold, U(26), ax0 + U(4), y + U(14), col(headCol), head.c_str());
        if (L.session == an::SessionState::Countdown) {  // the seconds running out, as a bar
            const float left = clamp01(float(L.countdownEndsMs - nowMs) / 5000.f);
            dl->AddRectFilled(ImVec2(ax0, y + U(32)), ImVec2(ax1, y + U(38)), col(0x4A4458), U(3));
            dl->AddRectFilled(ImVec2(ax0, y + U(32)), ImVec2(ax0 + (ax1 - ax0) * left, y + U(38)), col(kOrange), U(3));
        }
        if (!sub.empty()) gameText(dl, g_font.text, U(16), ax0 + U(4), y + U(50), col(0xB8B0C4), sub.c_str());
        y += U(66);
        // the stage's map, while this PC gets it from the host's (airnet)
        if (s.mapState == an::MapState::Checking || s.mapState == an::MapState::Downloading || s.mapState == an::MapState::Failed) {
            std::string line;
            u32 ink = 0xB8B0C4;
            if (s.mapState == an::MapState::Checking) {
                line = "Looking for " + s.mapTitle + " on this PC...";
            } else if (s.mapState == an::MapState::Downloading) {
                line = "Downloading " + s.mapTitle + "  " + std::to_string(int(s.mapProgress * 100.f)) + "%";
            } else {
                line = "Could not get " + s.mapTitle + ": " + s.mapError, ink = kRed;
            }
            gameText(dl, g_font.text, U(15), ax0 + U(4), y + U(6), col(ink), fit(g_font.text, U(15), line, ax1 - ax0 - U(8)).c_str());
            if (s.mapState == an::MapState::Downloading) {  // how far, as a bar (as the countdown's)
                dl->AddRectFilled(ImVec2(ax0, y + U(18)), ImVec2(ax1, y + U(23)), col(0x4A4458), U(3));
                dl->AddRectFilled(ImVec2(ax0, y + U(18)), ImVec2(ax0 + (ax1 - ax0) * clamp01(s.mapProgress), y + U(23)), col(kOrange), U(3));
            }
            y += U(30);
        }
        if (L.session == an::SessionState::Results) {  // each player's report
            game::Json res = game::Json::parse(L.resultsJson);
            std::stable_sort(res.members.begin(), res.members.end(), [](const auto& x, const auto& y) {  // (by place, when they say)
                const game::Json* a = x.second.get("place");
                const game::Json* b = y.second.get("place");
                return (a ? a->n : 99.0) < (b ? b->n : 99.0);
            });
            int shown = 0;
            for (const auto& [name, rep] : res.members) {
                if (shown++ >= 4) break;
                std::string what;
                if (const game::Json* place = rep.get("place")) {
                    const int p = int(place->n);
                    what = std::to_string(p) + (p == 1 ? "st" : p == 2 ? "nd" : p == 3 ? "rd" : "th");
                }
                for (const auto& [k, v] : rep.members)
                    if (k != "place" && what.size() < 40) what += (what.empty() ? "" : "  \xC2\xB7  ") + k + " " + v.text();
                const an::Member* m = member(name);
                colourBall(dl, ImVec2(ax0 + U(10), y + U(10)), U(6.5f), kirbyRgb(m ? m->colour : ""));
                gameText(dl, g_font.bold, U(15), ax0 + U(24), y + U(10), col(kCream), name.c_str());
                gameText(dl, g_font.text, U(15), ax1 - gameMeasure(g_font.text, U(15), what.c_str()), y + U(10), col(0xD8D2E2), what.c_str());
                y += U(22);
            }
            if (res.members.empty()) gameText(dl, g_font.text, U(15), ax0 + U(4), y + U(10), col(0x9A93A6), "No results came in."), y += U(22);
            y += U(6);
        }
        // the host's buttons
        auto action = [&](float x0, float x1, const char* label, bool primary, bool enabled, const char* help, std::function<void()> act) {
            const ImVec2 ra(x0, y), rb(x1, y + U(36));
            const int idx = nextIdx();
            const bool focus = isFocus(idx), hot = enabled && hover(ra, rb);
            const float lit = glide(itemKey(idx), focus ? 1.f : 0.f, 20.f);
            const u32 base = !enabled ? 0x6A6670 : primary ? 0xF3B04A : hot ? 0xD2D2D2 : kButton;
            capsule(dl, ra.x - U(3), ra.y - U(3), rb.x + U(3), rb.y + U(3), 0.3f, true, col(lit > 0.5f ? kWhite : kInk));
            capsule(dl, ra.x, ra.y, rb.x, rb.y, 0.3f, true, col(mixRgb(base, kOrange, lit)));
            capsule(dl, ra.x + U(14), ra.y + U(4), rb.x - U(14), ra.y + U(13), 0.3f, true, col(kWhite, 0.35f));
            const float size = U(primary ? 21 : 18), w = measure(g_font.label, size, label).x;
            text(dl, g_font.label, size, (ra.x + rb.x) * 0.5f - w * 0.5f, ra.y + U(18), col(enabled ? kInk : 0x3A3A3A), label);
            if (focus && lit > 0.5f) cornerMarks(dl, ImVec2(ra.x - U(4), ra.y - U(4)), ImVec2(rb.x + U(4), rb.y + U(4)), now);
            Item it{ra, rb};
            it.col = 1, it.enabled = enabled, it.ok = std::move(act), it.help = help;
            add(std::move(it));
        };
        if (s.isHost) {
            const float mid = ax0 + (ax1 - ax0) * 0.56f;
            switch (L.session) {
            case an::SessionState::Lobby:
                action(ax0, ax1, "Start Now", true, playersIn() >= 2, "Start the match now (X).", startNow);
                break;
            case an::SessionState::Countdown:
                action(ax0, mid, "Start Now", true, true, "Skip the countdown (X).", startNow);
                action(mid + U(10), ax1, "Hold", false, true, "Stop the countdown: you start the match when everyone is ready.",
                       [] { net().cancelMatch(); });
                break;
            case an::SessionState::InMatch:
                action(ax0, ax1, "Abandon Match", false, true, "End the match for everyone and go back to the lobby.", [] {
                    openModal("Abandon the match?", "It ends for everyone in it.",
                              {{"Abandon", [] { net().cancelMatch(), closeModal(); }}, {"Cancel", [] { closeModal(); }}});
                });
                break;
            case an::SessionState::Results:
                action(ax0, ax1, "Continue", true, true, "On to the next match.", [] { net().continueSession(); });
                break;
            }
            y += U(48);
        }
        // the game settings: the mode and its rules (the host changes them between matches)
        const game::Settings st = game::Settings::fromJson(L.settingsJson);
        const bool editable = s.isHost && L.session != an::SessionState::InMatch;
        gameText(dl, g_font.bold, U(15), ax0 + U(4), y + U(8), col(0xB8B0C4), editable ? "Game Settings" : "Game Settings (the host sets them)");
        y += U(22);
        auto change = [](game::Settings ns) { net().setGameSettings(ns.toJson()); };
        // The rows (the mode, then its rules) scroll within the panel: the focused row is kept in sight, the
        // mouse wheel scrolls, and arrows at the top / bottom show there is more.
        const std::vector<game::Rule>& rules = game::rules(st.mode);
        const bool city = st.mode == game::kCityTrial;  // (City Trial: the Stage row after the mode)
        const int rulesFrom = city ? 2 : 1, n = rulesFrom + int(rules.size());
        const float pitch = U(38), listTop = y, listBottom = b.y - U(8);
        const int fits = std::max(1, int((listBottom - listTop + U(6)) / pitch));
        const int first = nextIdx();
        const int fRow = curFocus() - first;
        if (fRow >= 0 && fRow < n) g.settingsTop = std::clamp(g.settingsTop, fRow - fits + 1, fRow);
        if (g_live && g_m.wheel != 0 && g_m.in(ImVec2(ax0, listTop), ImVec2(ax1, listBottom))) g.settingsTop -= g_m.wheel > 0 ? 1 : -1;
        g.settingsTop = std::clamp(g.settingsTop, 0, std::max(0, n - fits));
        auto rowY = [&](int i) { return listTop + float(i - g.settingsTop) * pitch; };
        auto shown = [&](int i) { return i >= g.settingsTop && i < g.settingsTop + fits; };
        settingRow(dl, ax0 + U(10), ax1, rowY(0), "Mode", game::modeName(st.mode), editable, "Air Ride or City Trial.", [st, change](int) {
            if (s.lobby.maxPlayers > 4 && st.mode == game::kCityTrial) return toast("Matches of more than 4 are City Trial.");
            change(game::Settings::defaults(st.mode == game::kAirRide ? game::kCityTrial : game::kAirRide));
        }, shown(0));
        if (city) {
            // The stage: the city, or a map mod on the host's PC (the others get it from there: airnet). The host's
            // choices are its map mods (looked through as the server opened); everyone sees the host's.
            const std::string stage = st.stage.folder.empty() ? "Original" : st.stage.title.empty() ? st.stage.folder : st.stage.title;
            settingRow(dl, ax0 + U(10), ax1, rowY(1), "Stage", stage, editable,
                       editable ? "Where City Trial is played: the city, or a map mod on this PC (players without it get it from yours)."
                                : "Where City Trial is played (players without the host's map get it from the host's PC).",
                       [st, change](int d) {
                           if (!g_maps.scanned) return toast("Looking for map mods on this PC...", false);
                           const int count = 1 + int(g_maps.maps.size());
                           if (count < 2) return toast("No map mods on this PC (mods/maps).", false);
                           int cur = 0;
                           for (size_t k = 0; k < g_maps.maps.size(); k++)
                               if (!st.stage.folder.empty() && g_maps.maps[k].hash == st.stage.hash) cur = int(k) + 1;
                           const int next = ((cur + d) % count + count) % count;
                           game::Settings ns = st;
                           ns.stage = game::Stage{};
                           if (next) {
                               const mapfiles::Map& m = g_maps.maps[size_t(next - 1)];
                               ns.stage = {m.folder, m.title, m.hash, m.size};
                           }
                           change(ns);
                       },
                       shown(1));
        }
        for (int i = rulesFrom; i < n; i++) {
            const game::Rule& r = rules[size_t(i - rulesFrom)];
            const int ci = st.index(r);
            settingRow(dl, ax0 + U(10), ax1, rowY(i), r.name.c_str(), r.names[size_t(ci)], editable, r.help.c_str(), [st, change, r, ci](int d) {
                game::Settings ns = st;
                ns.set(r, ci + d);
                change(ns);
            }, shown(i));
        }
        // more above / below
        const float cx = (ax0 + ax1) * 0.5f, t = U(7);
        if (g.settingsTop > 0)
            dl->AddTriangleFilled(ImVec2(cx, listTop - U(13)), ImVec2(cx + t, listTop - U(5)), ImVec2(cx - t, listTop - U(5)), col(kCream));
        if (g.settingsTop + fits < n) {
            const float yb = rowY(g.settingsTop + fits) - U(4);
            dl->AddTriangleFilled(ImVec2(cx - t, yb), ImVec2(cx + t, yb), ImVec2(cx, yb + U(8)), col(kCream));
        }
    }
    // ---- chat (column 2) ----
    {
        const ImVec2 a(X(880) + fr.slide, Y(156)), b(X(1250) + fr.slide, Y(566 + g_ext));  // (taller in 4:3)
        panel(a, b, "Chat");
        const float lx0 = a.x + U(14), lx1 = b.x - U(14), ly0 = a.y + U(40), ly1 = b.y - U(56);
        const float size = U(16), lineH = size * 1.2f;
        // the lines, newest at the bottom, wrapped
        struct Line {
            std::string from, text;
            u32 colour;
            bool system;
        };
        std::vector<Line> lines;
        for (const an::ChatLine& c : s.chat) {
            const an::Member* m = member(c.from);
            lines.push_back({c.from, c.text, m ? kirbyRgb(m->colour) : 0xC9CED6, c.system});
        }
        if (g_live && g_m.wheel != 0 && g_m.in(a, b)) g.chatBack = std::max(0, g.chatBack + (g_m.wheel > 0 ? 1 : -1));
        g.chatBack = std::min(g.chatBack, std::max(0, int(lines.size()) - 1));
        dl->PushClipRect(ImVec2(lx0, ly0), ImVec2(lx1, ly1), true);
        float yb = ly1;
        for (int i = int(lines.size()) - 1 - g.chatBack; i >= 0 && yb > ly0; i--) {
            const Line& l = lines[size_t(i)];
            // the sender's name in their colour, the text after it, wrapping back to the left
            const std::string prefix = l.system ? "" : l.from + ": ";
            const float pw = l.system ? 0.f : gameMeasure(g_font.text, size, prefix.c_str());
            const std::vector<std::string> wrapped = wrapLines(g_font.text, size, l.text, lx1 - lx0 - pw, lx1 - lx0);
            yb -= float(wrapped.size()) * lineH + U(5);
            if (!l.system) gameText(dl, g_font.text, size, lx0, yb + lineH * 0.5f, col(l.colour), prefix.c_str());
            for (size_t k = 0; k < wrapped.size(); k++)
                gameText(dl, g_font.text, size, lx0 + (k ? 0.f : pw), yb + float(k) * lineH + lineH * 0.5f, col(l.system ? 0xA9A2B4 : kCream),
                         wrapped[k].c_str());
        }
        dl->PopClipRect();
        if (lines.empty()) gameText(dl, g_font.text, U(16), lx0, ly0 + U(12), col(0x9A93A6), "No messages yet.");
        // what is being typed
        const ImVec2 fa(lx0, b.y - U(46)), fb(lx1, b.y - U(12));
        const int idx = nextIdx();
        const bool focus = isFocus(idx);
        if (focus) dl->AddRectFilled(ImVec2(fa.x - U(4), fa.y - U(4)), ImVec2(fb.x + U(4), fb.y + U(4)), col(kOrange), U(20));
        fieldBox(dl, fa, fb, f.chat, focus && !g.osk);
        Item it{fa, fb};
        it.col = 2, it.field = &f.chat, it.help = "Type a message (Enter sends it). A: the on-screen keyboard.";
        it.submit = [] {
            if (!f.chat.text.empty()) net().chat(f.chat.text), f.chat.text.clear(), g.chatBack = 0;
        };
        add(std::move(it));
    }
    fr.help = sessionHelp();
    const bool canX = s.isHost && s.lobby.session != an::SessionState::InMatch;
    if (canX) fr.hints = {{"A", "Choose"}, {"B", "Leave"}, {"X", s.lobby.session == an::SessionState::Results ? "Continue" : "Start Now"}, {"Y", "Chat"}, {"L", ""}, {"R", "Panels"}};
    else fr.hints = {{"A", "Choose"}, {"B", "Leave"}, {"Y", "Chat"}, {"L", ""}, {"R", "Panels"}};
}

void screenProfile(Frame& fr) {
    Form fm{fr.dl, X(150), X(860), Y(140), U(44), U(54), fr.slide};
    const an::Profile& p = s.me;
    rowText(fm, "Username", p.username, "Your account's name.");
    auto colourIdx = [&] {
        for (int i = 0; i < an::kColourCount; i++)
            if (p.colour == an::kColours[i].id) return i;
        return 0;
    };
    auto machineIdx = [&] {
        for (int i = 0; i < an::kMachineCount; i++)
            if (p.machine == an::kMachines[i].id) return i;
        return 0;
    };
    const int ci = colourIdx(), mi = machineIdx();
    rowChoice(fm, "Colour", an::kColours[ci].name, "Your Kirby's colour, as others see you in lobbies.",
              [ci, mi](int d) {
                  const int n = an::kColourCount, to = (ci + d + n) % n;
                  net().setProfile(an::kColours[to].id, an::kMachines[mi].id);
              },
              kirbyRgb(an::kColours[ci].id));
    rowChoice(fm, "Favourite Machine", an::kMachines[mi].name, "The Air Ride machine you like best, on your profile.", [ci, mi](int d) {
        const int n = an::kMachineCount, to = (mi + d + n) % n;
        net().setProfile(an::kColours[ci].id, an::kMachines[to].id);
    });
    rowText(fm, "Matches Played", std::to_string(p.played), "Matches played online.");
    char rate[32];
    if (p.played > 0) std::snprintf(rate, sizeof rate, "%d  (%d%%)", p.wins, p.wins * 100 / std::max(1, p.played));
    else std::snprintf(rate, sizeof rate, "%d", p.wins);
    rowText(fm, "Wins", rate, "Matches won, and how many of those played.");
    rowText(fm, "Member Since", dateOf(p.created), "When this account was made.");
    rowButton(fm, "Back", "Back.", [] { go(g.profileFrom == Screen::Lobby && s.inLobby ? Screen::Lobby : Screen::Main); });
    fr.help = "Your profile: others see it in lobbies.";
}

void screenViewProfile(Frame& fr) {
    Form fm{fr.dl, X(150), X(860), Y(140), U(44), U(54), fr.slide};
    const bool ok = s.viewedValid && s.viewed.username == g.viewing;
    const an::Profile& p = s.viewed;
    if (!ok) {
        spinner(fr.dl, ImVec2(X(500) + fr.slide, Y(300)), U(22), fr.now, kInk);
    } else {
        rowText(fm, "Status", p.online ? (p.lobby ? "Online, in a server" : "Online") : "Offline", "Whether they are playing now.",
                p.online ? kGreen : kGrey);
        rowText(fm, "Colour", an::colourName(p.colour), "Their Kirby's colour.", kirbyRgb(p.colour));
        rowText(fm, "Favourite Machine", an::machineName(p.machine), "The Air Ride machine they like best.");
        rowText(fm, "Matches Played", std::to_string(p.played), "Matches played online.");
        char rate[32];
        if (p.played > 0) std::snprintf(rate, sizeof rate, "%d  (%d%%)", p.wins, p.wins * 100 / std::max(1, p.played));
        else std::snprintf(rate, sizeof rate, "%d", p.wins);
        rowText(fm, "Wins", rate, "Matches won.");
        rowText(fm, "Member Since", dateOf(p.created), "When their account was made.");
    }
    rowButton(fm, "Back", "Back.", [] { go(g.profileFrom == Screen::Lobby && s.inLobby ? Screen::Lobby : Screen::Main); });
    fr.help = ok ? p.username + "'s profile." : "Fetching the profile...";
}

void screenSettings(Frame& fr) {
    Form fm{fr.dl, X(150), X(860), Y(140), U(44), U(56), fr.slide};
    rowChoice(fm, "Service", f.dedicated ? "Dedicated Server" : "Official",
              "Official: every server, one account. Dedicated: a server someone runs (its own accounts).",
              [](int) { f.dedicated = !f.dedicated; });
    if (f.dedicated)
        rowField(fm, "Server Address", f.master, "The dedicated server: host or host:port (27600 if none).",
                 saveMaster);
    rowButton(fm, s.state == an::State::Offline ? "Connect" : "Reconnect",
              "Connect to the chosen service (a login remembered for it signs in by itself).", saveMaster);
    if (s.loggedIn) {
        rowField(fm, "New Password", f.newPass, "A new password for your account: at least 6 characters.", [] { g.focus[int(g.screen)]++; });
        rowButton(fm, "Change Password", "Set the new password (you stay signed in here; other PCs sign in again).", [] {
            if (int(f.newPass.text.size()) < an::kPasswordMin) return toast("Passwords are at least 6 characters.");
            net().changePassword(f.newPass.text);
            f.newPass.text.clear();
            toast("Password changed.", false);
        });
    }
    char st[96];
    std::snprintf(st, sizeof st, "%s%s", an::stateName(s.state), s.serverVersion.empty() ? "" : ("  \xC2\xB7  " + s.serverVersion).c_str());
    rowText(fm, "Status", st, "The connection to the online service.");
    rowButton(fm, "Back", "Back.", [] { go(g.settingsFrom); });
    fr.help = "Online settings.";
}

void screenUpdate(Frame& fr) {
    ImDrawList* dl = fr.dl;
    const float x = X(170) + fr.slide;
    text(dl, g_font.label, U(34), x, Y(200), col(kInk), "An update is needed");
    const char* body = "The online service needs a newer version of Kirby Air Ride: Recompiled.\nUpdate the game (the launcher does it "
                       "for you) to play online.";
    wrapped(dl, g_font.text, U(22), x, Y(240), U(640), col(0x1E2A36), body);
    if (!s.serverVersion.empty()) gameText(dl, g_font.text, U(18), x, Y(330), col(0x2A3A4A), ("Service: " + s.serverVersion).c_str());
    Form fm{dl, X(170), X(640), Y(400), U(44), U(56), fr.slide};
    rowButton(fm, "Back", "Leave Online.", leaveOnline);
    fr.help = "This version is too old for online play.";
}

// ---------------------------------------------------------------------------------------------------
// Over the screen: the modal box, the on-screen keyboard, toasts

void drawModal(Frame& fr) {
    ImDrawList* dl = fr.dl;
    Modal& md = g.modal;
    const float in = easeOutBack(float(fr.now - md.at) / 0.3f);
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(fr.W, fr.H), col(0x0E0B14, 0.5f * clamp01(float(fr.now - md.at) / 0.15f)));
    const int nb = int(md.buttons.size());
    const int perRow = 4, rows = (nb + perRow - 1) / perRow;
    const float w = U(640);
    const float bodyH = md.text.empty() ? 0.f : wrapped(nullptr, g_font.text, U(20), 0, 0, w - U(60), 0, md.text);
    const float h = U(80) + bodyH + (md.field ? U(64) : 0.f) + float(rows) * U(56) + U(20);
    const ImVec2 c(fr.W * 0.5f, fr.H * 0.5f - (1.f - in) * U(40));
    const ImVec2 a(c.x - w * 0.5f, c.y - h * 0.5f), b(c.x + w * 0.5f, c.y + h * 0.5f);
    g_alpha = clamp01(float(fr.now - md.at) / 0.12f);
    dl->AddRectFilled(ImVec2(a.x + U(6), a.y + U(8)), ImVec2(b.x + U(6), b.y + U(8)), col(0x000000, 0.35f), U(26));
    dl->AddRectFilled(ImVec2(a.x - U(3), a.y - U(3)), ImVec2(b.x + U(3), b.y + U(3)), col(kPlumLight), U(28));
    dl->AddRectFilled(a, b, col(kPlum), U(26));
    dl->AddRectFilled(ImVec2(a.x + U(12), a.y + U(12)), ImVec2(b.x - U(12), a.y + U(58)), col(kOnline.field), U(20));
    text(dl, g_font.label, U(26), a.x + U(30), a.y + U(35), col(kInk), fit(g_font.label, U(26), md.title, w - U(70)).c_str());
    float y = a.y + U(72);
    if (!md.text.empty()) {
        wrapped(dl, g_font.text, U(20), a.x + U(30), y, w - U(60), col(kBandText), md.text);
        y += bodyH + U(12);
    }
    g.inModal = true;
    if (md.field) {
        const ImVec2 fa(a.x + U(30), y), fb(b.x - U(30), y + U(42));
        const int idx = nextIdx();
        const bool focus = isFocus(idx);
        if (focus) dl->AddRectFilled(ImVec2(fa.x - U(4), fa.y - U(4)), ImVec2(fb.x + U(4), fb.y + U(4)), col(kOrange), U(24));
        fieldBox(dl, fa, fb, *md.field, focus && !g.osk);
        Item it{fa, fb};
        it.field = md.field;
        it.submit = md.buttons.empty() ? std::function<void()>() : md.buttons[0].act;
        add(std::move(it));
        y += U(60);
    }
    const float bw = (w - U(60) - U(12) * float(std::min(nb, perRow) - 1)) / float(std::min(nb, perRow));
    for (int i = 0; i < nb; i++) {
        const int r = i / perRow, k = i % perRow;
        const ImVec2 ba(a.x + U(30) + float(k) * (bw + U(12)), y + float(r) * U(56)), bb(ba.x + bw, ba.y + U(42));
        const int idx = nextIdx();
        const bool focus = isFocus(idx), hot = hover(ba, bb);
        const float lit = glide(itemKey(idx), focus ? 1.f : 0.f, 22.f);
        capsule(dl, ba.x - U(3), ba.y - U(3), bb.x + U(3), bb.y + U(3), 0.f, true, col(lit > 0.5f ? kWhite : kInk));
        capsule(dl, ba.x, ba.y, bb.x, bb.y, 0.f, true, col(mixRgb(hot ? 0xD6D6D6 : kButton, kOrange, lit)));
        float size = U(19), tw = measure(g_font.label, size, md.buttons[size_t(i)].label.c_str()).x;
        if (tw > bw - U(20)) size *= (bw - U(20)) / tw, tw = bw - U(20);  // (a long label in a narrow button)
        text(dl, g_font.label, size, (ba.x + bb.x) * 0.5f - tw * 0.5f, (ba.y + bb.y) * 0.5f, col(kInk), md.buttons[size_t(i)].label.c_str());
        Item it{ba, bb};
        it.col = 0;
        it.ok = md.buttons[size_t(i)].act;
        add(std::move(it));
    }
    g.inModal = false;
    g_alpha = 1.f;
}

// The on-screen keyboard (controllers): a key for each character the fields take, Shift, Space, Delete, Done.
const char* const kOskRows[2][4] = {{"1234567890-", "qwertyuiop_", "asdfghjkl.:", "zxcvbnm,!?'"},
                                     {"!@#$%^&*()+", "QWERTYUIOP=", "ASDFGHJKL;/", "ZXCVBNM<>~\""}};
constexpr int kOskCols = 11;
constexpr int kSegStart[4] = {0, 2, 7, 9}, kSegEnd[4] = {1, 6, 8, 10};  // the bottom row: Shift, Space, Delete, Done
int oskSeg(int col_) {
    for (int i = 3; i >= 0; i--)
        if (col_ >= kSegStart[i]) return i;
    return 0;
}

void oskKey(int row, int c) {
    Field* fd = g.oskField;
    if (!fd) return;
    if (row < 4) {
        const char ch[2] = {kOskRows[g.oskShift ? 1 : 0][row][c], 0};
        fd->type(ch);
        return;
    }
    switch (oskSeg(c)) {
    case 0: g.oskShift = !g.oskShift; break;
    case 1: fd->type(" "); break;
    case 2: fd->erase(); break;
    default: {
        g.osk = false;
        auto done = g.oskDone;
        if (done) done();
    }
    }
}

void drawOsk(Frame& fr, u32 nav) {
    ImDrawList* dl = fr.dl;
    Field* fd = g.oskField;
    if (!fd) {
        g.osk = false;
        return;
    }
    const float in = easeOut(float(fr.now - g.oskAt) / 0.22f);
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(fr.W, fr.H), col(0x0E0B14, 0.45f * in));
    const float kw = U(58), kh = U(50), gap = U(6);
    const float w = kOskCols * (kw + gap) - gap + U(48), h = U(84) + 5 * (kh + gap) + U(20);
    const ImVec2 a(fr.W * 0.5f - w * 0.5f, YB(700) - h + (1.f - in) * U(300)), b(a.x + w, a.y + h);
    dl->AddRectFilled(ImVec2(a.x - U(3), a.y - U(3)), ImVec2(b.x + U(3), b.y + U(3)), col(kPlumLight), U(26));
    dl->AddRectFilled(a, b, col(kPlum), U(24));
    // the field, at the top
    gameText(dl, g_font.bold, U(17), a.x + U(28), a.y + U(24), col(kOrange), g.oskLabel.c_str());
    fieldBox(dl, ImVec2(a.x + U(24), a.y + U(40)), ImVec2(b.x - U(24), a.y + U(76)), *fd, true);
    // moves
    if (nav & kUp) g.oskRow = (g.oskRow + 4) % 5;
    if (nav & kDown) g.oskRow = (g.oskRow + 1) % 5;
    if (g.oskRow == 4) g.oskCol = kSegStart[oskSeg(g.oskCol)];
    if (nav & kLeft) g.oskCol = g.oskRow == 4 ? kSegStart[(oskSeg(g.oskCol) + 3) % 4] : (g.oskCol + kOskCols - 1) % kOskCols;
    if (nav & kRight) g.oskCol = g.oskRow == 4 ? kSegStart[(oskSeg(g.oskCol) + 1) % 4] : (g.oskCol + 1) % kOskCols;
    const float kx0 = a.x + U(24), ky0 = a.y + U(88);
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < kOskCols; c++) {
            if (r == 4 && c != kSegStart[oskSeg(c)]) continue;
            const int span = r == 4 ? kSegEnd[oskSeg(c)] - c + 1 : 1;
            const ImVec2 ka(kx0 + float(c) * (kw + gap), ky0 + float(r) * (kh + gap)), kb(ka.x + float(span) * (kw + gap) - gap, ka.y + kh);
            const bool hot = g_live && g_m.in(ka, kb);
            if (hot && g_m.moved) g.oskRow = r, g.oskCol = c;
            const bool focus = r == g.oskRow && (r == 4 ? oskSeg(g.oskCol) == oskSeg(c) : c == g.oskCol);
            const float lit = glide(reinterpret_cast<const void*>(uintptr_t(0x7E000000u + r * 64 + c * 4)), focus ? 1.f : 0.f, 24.f);
            std::string label;
            u32 face = 0xF4F1EA;
            if (r < 4) label = std::string(1, kOskRows[g.oskShift ? 1 : 0][r][c]);
            else {
                const int sg = oskSeg(c);
                label = sg == 0 ? "Shift" : sg == 1 ? "Space" : sg == 2 ? "Delete" : "Done";
                face = sg == 3 ? 0xF3B04A : sg == 0 && g.oskShift ? 0xBFD7EA : 0xC9C5BE;
            }
            dl->AddRectFilled(ImVec2(ka.x - U(2), ka.y - U(2)), ImVec2(kb.x + U(2), kb.y + U(4)), col(kInk), U(10));
            dl->AddRectFilled(ImVec2(ka.x, ka.y + U(2)), ImVec2(kb.x, kb.y + U(2)), col(0x8A8680), U(8));
            dl->AddRectFilled(ka, kb, col(mixRgb(hot ? 0xFFFFFF : face, kOrange, lit)), U(8));
            const float size = r < 4 ? U(22) : U(17);
            const float tw = gameMeasure(g_font.bold, size, label.c_str());
            gameText(dl, g_font.bold, size, (ka.x + kb.x) * 0.5f - tw * 0.5f, (ka.y + kb.y) * 0.5f, col(kInk), label.c_str());
            // the button that does it, in the key's corner
            const char* glyph = r == 4 ? (oskSeg(c) == 0 ? "X" : oskSeg(c) == 1 ? "Y" : oskSeg(c) == 2 ? "B" : "S") : nullptr;
            if (glyph) {
                const ImVec2 gc(kb.x - U(12), ka.y + U(12));
                dl->AddCircleFilled(gc, U(8), col(!std::strcmp(glyph, "B") ? 0xD0453B : 0x6B7280), 16);
                gameText(dl, g_font.bold, U(10), gc.x - gameMeasure(g_font.bold, U(10), glyph) * 0.5f, gc.y, col(kWhite), glyph);
            }
            if (hot && g_m.clicked) g.oskRow = r, g.oskCol = c, oskKey(r, c);
        }
    }
    // the buttons' own jobs
    if (nav & kOk) oskKey(g.oskRow, g.oskCol);
    if (nav & kBack) {
        if (fd->text.empty()) g.osk = false;
        else fd->erase();
    }
    if (nav & kX) g.oskShift = !g.oskShift;
    if (nav & kY) fd->type(" ");
    if (nav & (kStart | kSubmit)) oskKey(4, 9);
}

void drawToasts(Frame& fr) {
    ImDrawList* dl = fr.dl;
    float y = YT(14);
    for (size_t i = 0; i < g.toasts.size();) {
        const Toast& t = g.toasts[i];
        const float age = float(fr.now - t.at), life = 5.f;
        if (age > life) {
            g.toasts.erase(g.toasts.begin() + long(i));
            continue;
        }
        const float a = clamp01(age / 0.2f) * clamp01((life - age) / 0.5f);
        g_alpha = a;
        const float size = U(19);
        const std::string msg = fit(g_font.text, size, t.text, U(660));
        const float tw = gameMeasure(g_font.text, size, msg.c_str()), h = U(40), w = tw + U(70);
        const ImVec2 ta(X(470) - w * 0.5f, y - (1.f - easeOut(age / 0.25f)) * U(20)), tb(ta.x + w, ta.y + h);
        dl->AddRectFilled(ImVec2(ta.x - U(2), ta.y - U(2)), ImVec2(tb.x + U(2), tb.y + U(2)), col(kInk), h);
        dl->AddRectFilled(ta, tb, col(t.error ? 0x8E2A1E : 0x2F5D7A), h * 0.5f);
        dl->AddCircleFilled(ImVec2(ta.x + U(22), ta.y + h * 0.5f), U(10), col(kWhite), 16);
        gameText(dl, g_font.bold, U(15), ta.x + U(22) - gameMeasure(g_font.bold, U(15), t.error ? "!" : "i") * 0.5f, ta.y + h * 0.5f,
             col(t.error ? 0x8E2A1E : 0x2F5D7A), t.error ? "!" : "i");
        gameText(dl, g_font.text, size, ta.x + U(44), ta.y + h * 0.5f, col(kWhite), msg.c_str());
        g_alpha = 1.f;
        y += h + U(10);
        i++;
    }
}

// ---------------------------------------------------------------------------------------------------
// One frame of the menu

void syncScreen() {
    const bool inLobby = s.inLobby, loggedIn = s.loggedIn;
    if (s.updateRequired) {
        go(Screen::Update);
    } else if (loggedIn) {
        if (g.screen == Screen::Connecting || g.screen == Screen::SignIn || g.screen == Screen::Register || g.screen == Screen::Update)
            go(inLobby ? Screen::Lobby : Screen::Main);
        if (inLobby && !g.wasInLobby) go(Screen::Lobby);
        if (!inLobby && g.screen == Screen::Lobby) go(Screen::Main);
    } else {
        const Screen sc = g.screen;
        const bool needsLogin = sc == Screen::Main || sc == Screen::Browse || sc == Screen::Host || sc == Screen::Lobby ||
                                sc == Screen::Profile || sc == Screen::ViewProfile || sc == Screen::Update;
        const bool trying = s.state == an::State::Connecting || s.state == an::State::LoggingIn || s.state == an::State::Reconnecting;
        if (needsLogin) go(trying && s.savedLogin ? Screen::Connecting : Screen::SignIn);
        if (sc == Screen::Connecting && !trying && nowT() - g.screenAt > 1.5) go(Screen::SignIn);  // (a moment for the connection to start)
        if (g.wasLoggedIn && !trying) toast("Signed out.", false);
    }
    g.wasInLobby = inLobby;
    g.wasLoggedIn = loggedIn;
}

// What the keys and controllers asked of the screen's items.
void navigate(u32 nav) {
    Screen sc = g.screen;
    auto back = [&] {
        switch (sc) {
        case Screen::Connecting: net().disconnect(), go(Screen::SignIn); break;
        case Screen::SignIn: leaveOnline(); break;
        case Screen::Register: go(Screen::SignIn); break;
        case Screen::Main: leaveOnline(); break;
        case Screen::Browse:
        case Screen::Host: go(Screen::Main); break;
        case Screen::Lobby:
            openModal("Leave " + s.lobby.name + "?", s.isHost ? "You host it: someone else will (or it closes)." : "You can come back while it is open.",
                      {{"Leave", [] { net().leave(), closeModal(); }}, {"Stay", [] { closeModal(); }}});
            break;
        case Screen::Profile:
        case Screen::ViewProfile: go(g.profileFrom == Screen::Lobby && s.inLobby ? Screen::Lobby : Screen::Main); break;
        case Screen::Settings: go(g.settingsFrom); break;
        case Screen::Update: leaveOnline(); break;
        default: break;
        }
    };
    const bool modal = g.modal.open;
    std::vector<Item>& items = g.items;
    const int n = int(items.size());
    // a click
    if (g.click >= 0 && g.click < n) {
        Item& it = items[size_t(g.click)];
        if (it.field) {
        } else if (it.step && !it.ok) {
            const float split = it.split >= 0 ? it.split : (it.a.x + it.b.x) * 0.5f;
            it.step(g.clickPos.x < split ? -1 : 1);
        } else if (it.ok) {
            it.ok();
        }
        g.click = -1;
        return;
    }
    g.click = -1;
    if (!nav) return;
    if (modal && (nav & kBack)) {
        auto c = g.modal.cancel;
        if (c) c();
        return;
    }
    if (!modal) {
        if (nav & kBack) return back();
        if (sc == Screen::Browse) {
            if (nav & (kPrevTab | kNextTab)) {
                g.filter = (g.filter + (nav & kPrevTab ? 2 : 1)) % 3;
                net().refreshServers(an::Mode(g.filter));
                g.refreshedAt = nowT();
                return;
            }
            if (nav & kX) {
                net().refreshServers(an::Mode(g.filter));
                g.refreshedAt = nowT();
                return;
            }
        }
        if (sc == Screen::Lobby) {
            if (nav & kX) {
                if (s.isHost && s.lobby.session == an::SessionState::Results) net().continueSession();
                else startNow();
                return;
            }
            if (nav & kY) {
                for (int i = 0; i < n; i++)
                    if (items[size_t(i)].field == &f.chat) curFocus() = i;
                openOsk(&f.chat, "Chat", [] {
                    if (!f.chat.text.empty()) net().chat(f.chat.text), f.chat.text.clear(), g.chatBack = 0;
                });
                return;
            }
        }
    }
    if (n == 0) return;
    int& fo = curFocus();
    fo = std::clamp(fo, 0, n - 1);
    auto enabledAt = [&](int i) { return items[size_t(i)].enabled; };
    if (!items[size_t(fo)].enabled)
        for (int k = 1; k < n; k++)
            if (enabledAt((fo + k) % n)) {
                fo = (fo + k) % n;
                break;
            }
    auto moveV = [&](int d) {
        const int c = items[size_t(fo)].col;
        int j = fo;
        for (int k = 0; k < n; k++) {
            j = (j + d + n) % n;
            if (items[size_t(j)].col == c && items[size_t(j)].enabled) {
                fo = j;
                return;
            }
        }
    };
    auto moveH = [&](int d) {
        const int c = items[size_t(fo)].col + d;
        const float cy = (items[size_t(fo)].a.y + items[size_t(fo)].b.y) * 0.5f;
        int best = -1;
        float bestD = 1e30f;
        for (int k = 0; k < n; k++) {
            const Item& it = items[size_t(k)];
            if (it.col != c || !it.enabled) continue;
            const float dd = it.b.y > it.a.y ? std::fabs((it.a.y + it.b.y) * 0.5f - cy) : 1e20f + float(k);
            if (dd < bestD) bestD = dd, best = k;
        }
        if (best >= 0) fo = best;
    };
    Item& it = items[size_t(fo)];
    if (nav & kUp) moveV(-1);
    else if (nav & kDown) moveV(1);
    else if (modal && (nav & (kLeft | kRight))) moveV(nav & kLeft ? -1 : 1);  // (the box's buttons sit side by side)
    else if (nav & kLeft) {
        if (it.step) it.step(-1);
        else moveH(-1);
    } else if (nav & kRight) {
        if (it.step) it.step(1);
        else moveH(1);
    } else if (nav & (kPrevTab | kNextTab)) {
        moveH(nav & kPrevTab ? -1 : 1);
    } else if (nav & kSubmit) {
        if (it.submit) it.submit();
        else if (it.ok) it.ok();
    } else if (nav & (kOk | kStart)) {
        if (it.field) {
            Field* fd = it.field;
            auto submit = it.submit;
            const char* label = "Type";
            for (const auto& [ff, name] : std::initializer_list<std::pair<Field*, const char*>>{
                     {&f.user, "Username"}, {&f.pass, "Password"}, {&f.pass2, "Confirm Password"}, {&f.master, "Server Address"},
                     {&f.newPass, "New Password"}, {&f.hostName, "Server Name"}, {&f.hostPass, "Password"}, {&f.chat, "Chat"},
                     {&f.joinPass, "Password"}})
                if (ff == fd) label = name;
            openOsk(fd, label, submit);
        } else if (it.ok) {
            it.ok();
        } else if (it.step) {
            it.step(1);
        }
    }
}

// The screen's backdrop and emblem alone: covering the game while it gets back to Select Mode, or fading.
void drawCover(ImDrawList* dl, float a) {
    ImGuiIO& io = ImGui::GetIO();
    Frame fr{dl, io.DisplaySize.x, io.DisplaySize.y, float(nowT()), 1.f, 0.f, {}, {}};
    g_alpha = a;
    const bool was = g.cover;
    g.cover = true;
    backdrop(fr);
    g.cover = was;
    const ImVec2 c(X(1104), Y(430));
    disc(dl, c, U(150), fr.now);
    onlineEmblem(dl, c, U(150), fr.now);
    g_alpha = 1.f;
}

void drawFrame(ImDrawList* dl, float appear, bool live) {
    ImGuiIO& io = ImGui::GetIO();
    Frame fr{dl, io.DisplaySize.x, io.DisplaySize.y, float(nowT()), appear, 0.f, {}, {}};
    fr.hints = {{"A", "Choose"}, {"B", "Back"}};
    const float pageIn = easeOut(float(fr.now - g.screenAt) / 0.24f);
    fr.slide = (1.f - pageIn) * U(60) + (1.f - easeOut(appear)) * U(260);
    g_live = live;
    g.items.clear();
    backdrop(fr);
    // the emblem: a disc with the globe (Kirby on the profile); none in the lobby (its chat is there)
    if (g.screen != Screen::Lobby) {
        const ImVec2 c(X(1104) + (1.f - easeOut(appear)) * U(420), Y(430));
        const float r = U(150);
        disc(dl, c, r, fr.now);
        if (g.screen == Screen::Profile) {
            if (!kirbymodel::draw(dl, ImVec2(c.x, c.y - U(2)), r * 1.62f, kirbyFrame(s.me.colour), fr.now))
                puffball(dl, ImVec2(c.x, c.y - U(4)), r * 0.52f, kirbyRgb(s.me.colour), fr.now);
        } else {
            onlineEmblem(dl, c, r, fr.now);
        }
    }
    static const char* kNames[] = {"Online", "Online", "Create Account", "Online", "Servers", "Host Server", "Lobby", "Profile", "Profile",
                                   "Online Settings", "Online"};
    std::string name = kNames[int(g.screen)];
    if (g.screen == Screen::Lobby) name = s.lobby.name;
    if (g.screen == Screen::ViewProfile) name = "Profile: " + g.viewing;
    title(fr, name);
    statusChip(fr);
    g_alpha = pageIn * clamp01(appear * 1.5f);
    switch (g.screen) {
    case Screen::Connecting: screenConnecting(fr); break;
    case Screen::SignIn: screenSignIn(fr); break;
    case Screen::Register: screenRegister(fr); break;
    case Screen::Main: screenMain(fr); break;
    case Screen::Browse: screenBrowse(fr); break;
    case Screen::Host: screenHost(fr); break;
    case Screen::Lobby: screenLobby(fr); break;
    case Screen::Profile: screenProfile(fr); break;
    case Screen::ViewProfile: screenViewProfile(fr); break;
    case Screen::Settings: screenSettings(fr); break;
    case Screen::Update: screenUpdate(fr); break;
    default: break;
    }
    g_alpha = 1.f;
    // the band tells what is in focus
    {
        const int fo = curFocus();
        if (!g.modal.open && fo >= 0 && fo < int(g.items.size()) && !g.items[size_t(fo)].help.empty()) fr.help = g.items[size_t(fo)].help;
    }
    if (g.osk) fr.hints = {{"A", "Type"}, {"B", "Delete"}, {"X", "Shift"}, {"Y", "Space"}, {"S", "Done"}};
    band(fr);
    // the keyboard's field: the one in focus, or the box's, or the on-screen keyboard's
    Field* typing = nullptr;
    if (g.modal.open) {
        g.items.clear();
        drawModal(fr);
        const int fo = g.modal.focus;
        if (fo >= 0 && fo < int(g.items.size())) typing = g.items[size_t(fo)].field;
    } else {
        const int fo = curFocus();
        if (fo >= 0 && fo < int(g.items.size())) typing = g.items[size_t(fo)].field;
    }
    const u32 nav = live ? (g.nav | g.keyNav) : 0;
    if (g.osk) {
        typing = g.oskField;
        drawOsk(fr, nav);
    } else if (live) {
        g.inModal = g.modal.open;
        navigate(nav);
        g.inModal = false;
    }
    g.typing = live ? typing : nullptr;
    drawToasts(fr);
}

// ---------------------------------------------------------------------------------------------------
// Test runs: SA2_ONLINE_TEST

int g_shotAt = -1;
bool g_testWanted = false;  // (SA2_ONLINE_TEST's lanflow: as if the game asked for the link)
bool g_testJoin = false;    // (its join: into the first server listed, until in its lobby)

struct TestStep {
    int delay;
    std::string what;
};
std::vector<TestStep> testSteps() {
    std::vector<TestStep> v;
    const char* e = std::getenv("SA2_ONLINE_TEST");
    if (!e) return v;
    std::string t = e;
    size_t p = 0;
    while (p < t.size()) {
        size_t q = t.find(';', p);
        if (q == std::string::npos) q = t.size();
        const std::string step = t.substr(p, q - p);
        const size_t c = step.find(':');
        if (c != std::string::npos) v.push_back({std::atoi(step.substr(0, c).c_str()), step.substr(c + 1)});
        p = q + 1;
    }
    return v;
}

void runTest(int frame) {
    static const std::vector<TestStep> steps = testSteps();
    static size_t next = 0;
    static int at = 0;
    while (next < steps.size() && frame >= at + steps[next].delay) {
        const std::string& w = steps[next].what;
        if (w.rfind("waitfor=", 0) == 0 && int(s.lobby.members.size()) < std::atoi(w.c_str() + 8)) break;  // (until that many in the lobby)
        at = w.rfind("waitfor=", 0) == 0 ? frame : at + steps[next].delay;
        log("[onlinemenu] test step %zu at %d: %s\n", next, frame, w.c_str());
        next++;
        static const std::pair<const char*, u32> kNames[] = {{"up", kUp},     {"down", kDown},   {"left", kLeft}, {"right", kRight},
                                                              {"a", kOk},      {"b", kBack},      {"x", kX},       {"y", kY},
                                                              {"l", kPrevTab}, {"r", kNextTab},   {"start", kStart}, {"enter", kSubmit},
                                                              {"esc", kBack}};
        bool done = false;
        for (const auto& [n, bit] : kNames)
            if (w == n) g.nav |= bit, done = true;
        if (done) continue;
        if (w == "open") {
            openMenu();
        } else if (w == "lanflow") {  // (as if the game had asked for the link now: its Online mode)
            g_testWanted = true;
        } else if (w == "close") {
            closeMenu();
        } else if (w == "bs") {
            if (Field* fd = g.osk ? g.oskField : g.typing) fd->erase();
        } else if (w.rfind("results=", 0) == 0) {  // (as if the game reported its match over)
            if (g.inMatch || s.lobby.session == an::SessionState::InMatch) {
                const std::string r = w.substr(8);
                endMatch(&r);
            }
        } else if (w.rfind("type=", 0) == 0) {
            if (Field* fd = g.osk ? g.oskField : g.typing) fd->type(w.c_str() + 5);
        } else if (w == "shot") {
            g_shotAt = frame;  // (written once this frame is drawn)
        } else if (w == "join") {  // (to the server list, then into the first server listed - until in its lobby)
            g_testJoin = true;
        }
    }
    if (g_testJoin) {
        if (s.inLobby) {
            g_testJoin = false;
            log("[onlinemenu] test: in the lobby\n");
        } else if (g.screen == Screen::Main && !g.inModal) {
            go(Screen::Browse);
        } else if (g.screen == Screen::Browse && !g.inModal && !s.servers.empty() && frame % 60 == 0) {
            net().join(s.servers[0].id);
        }
    }
}

// ---------------------------------------------------------------------------------------------------
// Opening on the game's Online mode

void watchGame(double now) {
    const game::Where where = game::where();
    // ONLINE chosen in Select Mode (the LAN screen comes next frame), or the game on its LAN screen: the menu.
    const bool w = g_testWanted || where == game::Where::ChoosingOnline || where == game::Where::LanSystems;
    if (w != g.wanted) {
        g.wanted = w;
        if (w) g.wantedAt = now;
        else g.unwantedAt = now;
        log("[onlinemenu] the game: %s\n", game::whereName(where));
    }
    if (w && !g.open && !g.openPending && g.autoOpen && !g.leaving && !g.inMatch) {
        g.lanFlow = true;
        g.cover = true;  // (from this frame on: the LAN screen is never seen)
        openMenu();
    }
    // Back: covered until the game is on Select Mode again (or it is taking long).
    if (g.leaving && !g.open && ((where != game::Where::LanSystems && where != game::Where::ChoosingOnline && g.presses.empty()) ||
                                 now - g.leavingAt > 4.0)) {
        g.leaving = false;
        g.cover = false;
        g.closedAt = now;  // (the cover fades from here)
        log("[onlinemenu] back on %s\n", game::whereName(where));
    }
}

// The session's matches: the game plays this console's part of each, then the menu comes back.
void watchMatch(double now) {
    if (!g.matchSeenInit) g.matchSeenInit = true, g.matchSeen = s.matchSerial;
    if (s.matchSerial != g.matchSeen) {
        g.matchSeen = s.matchSerial;
        if (s.inLobby && s.match.myConsole >= 0) beginMatch();
    }
    if (g.inMatch) {
        std::string results;
        if (game::takeResults(results)) {
            log("[onlinemenu] the match is over: %s\n", results.c_str());
            endMatch(&results);
        } else if ((!s.inLobby || s.lobby.session == an::SessionState::Lobby ||
                    (s.lobby.session == an::SessionState::Results && now - g.matchAt > 3.0)) &&
                   now - g.matchAt > 2.0) {
            log("[onlinemenu] the session moved on without this console's match: back to the server\n");
            endMatch(nullptr);
        }
    }
    if (s.lobby.session != an::SessionState::InMatch) g.matchFailed = false;
    g.session = s.lobby.session;
}

void setTextInput(bool on) {
    if (on == g.textInput) return;
    g.textInput = on;
    if (SDL_Window* w = aurora::window::get_sdl_window()) {
        if (on) SDL_StartTextInput(w);
        else SDL_StopTextInput(w);
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------

void event(const SDL_Event& e) {
    if (!g.enabled || menu::isOpen()) return;
    if (e.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Scancode sc = e.key.scancode;
        if (!g.open) return;
        Field* fd = g.osk ? g.oskField : g.typing;
        if (sc == SDL_SCANCODE_ESCAPE && !e.key.repeat) {
            if (g.osk) g.osk = false;
            else g.keyNav |= kBack;
            return;
        }
        if (!fd) {
            if (sc == SDL_SCANCODE_BACKSPACE && !e.key.repeat) g.keyNav |= kBack;
            return;
        }
        // typing: the keys the controller's stand-in uses are letters now
        switch (sc) {
        case SDL_SCANCODE_BACKSPACE: fd->erase(); break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            if (!e.key.repeat) g.keyNav |= kSubmit;
            break;
        case SDL_SCANCODE_UP: g.keyNav |= kUp; break;
        case SDL_SCANCODE_DOWN: g.keyNav |= kDown; break;
        case SDL_SCANCODE_TAB: g.keyNav |= (e.key.mod & SDL_KMOD_SHIFT) ? kUp : kDown; break;
        case SDL_SCANCODE_V:
            if (e.key.mod & SDL_KMOD_CTRL) {
                if (char* clip = SDL_GetClipboardText()) {
                    fd->type(clip);
                    SDL_free(clip);
                }
            }
            break;
        default: break;
        }
    } else if (e.type == SDL_EVENT_TEXT_INPUT && g.open) {
        if (Field* fd = g.osk ? g.oskField : g.typing)
            if (e.text.text && !(SDL_GetModState() & SDL_KMOD_CTRL)) fd->type(e.text.text);
    }
}

void takePads(PADStatus pads[4]) {
    if (!g.enabled) return;
    // The controllers' moves: port 1 apart (the keyboard stands in for it - while a text field takes the
    // keyboard, its keys type rather than steer).
    auto navOf = [](const PADStatus& p) {
        u32 held = 0;
        if (p.err != PAD_ERR_NONE) return held;
        if (p.button & PAD_BUTTON_UP || p.stickY > 50) held |= kUp;
        if (p.button & PAD_BUTTON_DOWN || p.stickY < -50) held |= kDown;
        if (p.button & PAD_BUTTON_LEFT || p.stickX < -50) held |= kLeft;
        if (p.button & PAD_BUTTON_RIGHT || p.stickX > 50) held |= kRight;
        if (p.button & PAD_BUTTON_A) held |= kOk;
        if (p.button & PAD_BUTTON_START) held |= kStart;
        if (p.button & PAD_BUTTON_B) held |= kBack;
        if (p.button & PAD_BUTTON_X) held |= kX;
        if (p.button & PAD_BUTTON_Y) held |= kY;
        if (p.button & PAD_TRIGGER_L || p.triggerLeft > 150) held |= kPrevTab;
        if (p.button & PAD_TRIGGER_R || p.triggerRight > 150) held |= kNextTab;
        return held;
    };
    struct Repeat {
        u32 prev = 0;
        double heldSince = 0, lastRepeat = 0;
        u32 pressed(u32 held, double now) {
            constexpr u32 kDirs = kUp | kDown | kLeft | kRight;
            u32 p = held & ~prev;
            if (p & kDirs) heldSince = now, lastRepeat = now;
            else if (held & kDirs && now - heldSince > 0.38 && now - lastRepeat > 0.075) p |= held & kDirs, lastRepeat = now;
            prev = held;
            return p;
        }
    };
    static Repeat s_port1, s_others;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    u32 others = 0;
    u16 buttons = 0;
    for (int i = 1; i < 4; i++) others |= navOf(pads[i]);
    for (int i = 0; i < 4; i++)
        if (pads[i].err == PAD_ERR_NONE) buttons |= pads[i].button;
    u32 pressed = s_others.pressed(others, now);
    const u32 p1 = s_port1.pressed(navOf(pads[0]), now);
    bool typingKeys = false;
    if (g.open && (g.typing || g.osk)) {
        int n = 0;
        const bool* keys = SDL_GetKeyboardState(&n);
        for (int k = 0; k < n && !typingKeys; k++) typingKeys = keys[k];
    }
    if (!typingKeys) pressed |= p1;
    if (g.open && !menu::isOpen()) g.nav |= pressed;
    // The game sees nothing while the menu is open, nor the buttons held as it closed until let go; then
    // the presses the menu hands it.
    if (g.open) {
        g.padMask = buttons;
        for (int i = 0; i < 4; i++) {
            const s8 err = pads[i].err;
            pads[i] = PADStatus{};
            pads[i].err = err;
        }
        return;
    }
    if (g.padMask) {
        g.padMask &= buttons;
        for (int i = 0; i < 4; i++) pads[i].button &= u16(~g.padMask);
    }
    if (g.leaving)  // (on the way back to Select Mode: the game gets only the menu's presses)
        for (int i = 0; i < 4; i++) {
            const s8 err = pads[i].err;
            pads[i] = PADStatus{};
            pads[i].err = err;
        }
    if (!g.presses.empty() && !g.padMask && !menu::isOpen()) {
        Press& p = g.presses.front();
        const u64 frame = gxFrames();
        if (p.startedAt == ~0ull) p.startedAt = frame, log("[onlinemenu] press %04X in %.2f s for %.2f s\n", p.buttons, p.wait, p.hold);
        const double t = double(frame - p.startedAt) / 60.0;
        if (t >= p.wait) {
            pads[0].err = PAD_ERR_NONE;
            pads[0].button |= p.buttons;
            if (p.buttons & PAD_BUTTON_UP) pads[0].stickY = 100;
            if (p.buttons & PAD_BUTTON_DOWN) pads[0].stickY = -100;
            if (p.buttons & PAD_BUTTON_LEFT) pads[0].stickX = -100;
            if (p.buttons & PAD_BUTTON_RIGHT) pads[0].stickX = 100;
            if (t >= p.wait + p.hold) g.presses.pop_front();
        }
    }
}

void overlay(bool headless) {
    if (!ImGui::GetCurrentContext()) return;
    static const bool s_init = [&] {
        const char* e = std::getenv("SA2_ONLINE");
        g.enabled = !(e && !std::strcmp(e, "0"));
        g.headless = headless;
        // (the developer loopback LAN, and headless runs, play without it unless a test asks)
        g.autoOpen = !std::getenv("SA2_LAN_LOOPBACK") && (!headless || std::getenv("SA2_ONLINE_TEST"));
        return true;
    }();
    (void)s_init;
    if (!g.enabled) return;
    if (headless && !std::getenv("SA2_ONLINE_TEST")) return;
    ImGuiIO& io = ImGui::GetIO();
    const double now = ImGui::GetTime();
    gameart::load();  // the game's menu art (src/runtime/gameart.h), once it can be read
    static int s_frames = 0;
    s_frames++;
    runTest(s_frames);
    watchGame(now);

    if (g.openPending) {
        g.openPending = false;
        if (!g.open) {
            g.open = true;
            const float back = clamp01(1.f - float(now - g.closedAt) / 0.25f);
            g.openedAt = now - 0.45 * double(back) * 0.6;
            g.nav = g.keyNav = 0;
            g.presses.clear();
            g.everOpened = true;
            net().snapshot(s);
            if (s.state == an::State::Offline) {
                net().connect();  // (resumes a remembered login)
                if (savedLoginForService()) go(Screen::Connecting);
            }
            if (f.user.text.empty()) f.user.text = settings::onlineUser.get();
            if (s.loggedIn) go(s.inLobby ? Screen::Lobby : Screen::Main);
            g.screenAt = now;
            log("[onlinemenu] open (%s)\n", g.lanFlow ? "the game's Online mode" : "a test");
        }
    }
    if (g.closePending) {
        g.closePending = false;
        if (g.open) g.open = false, g.closedAt = now, g.osk = false, g.modal.open = false, log("[onlinemenu] closed\n");
    }
    if (g.everOpened) {
        net().snapshot(s);
        if (s.errorSerial != g.errorSerial) {
            if (!s.error.empty()) toast(s.error);
            g.errorSerial = s.errorSerial;
        }
        mapsFrame();
        watchMatch(now);
        if (g.open) syncScreen();
    }
    float appear = 0.f;
    if (g.open) appear = easeOut(float(now - g.openedAt) / 0.45f);
    else if (now - g.closedAt < 0.3) appear = 1.f - easeOut(float(now - g.closedAt) / 0.3f);

    g_s = std::max(0.5f, std::min(io.DisplaySize.x / 1280.f, io.DisplaySize.y / 720.f));
    g_ox = std::floor((io.DisplaySize.x - 1280.f * g_s) * 0.5f);
    g_oy = std::floor((io.DisplaySize.y - 720.f * g_s) * 0.5f);
    g_ext = std::max(0.f, g_oy / g_s);
    g_oy -= std::floor(U(g_ext * 0.5f));
    g_m = Mouse{io.MousePos, io.MouseDelta.x != 0 || io.MouseDelta.y != 0, io.MouseClicked[0], io.MouseDown[0], io.MouseReleased[0],
                io.MouseWheel};
    // (drawn behind the F1 menu: its list comes first)
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (g.open && appear > 0.f) {
        const bool live = !menu::isOpen() && now - g.openedAt > 0.15;
        drawFrame(dl, appear, live);
        io.MouseDrawCursor = io.MouseDrawCursor || g.open;
    } else {
        g.typing = nullptr;
        // closed: the cover alone while the game is on its way back (Select Mode), then fading
        if (g.cover || g.leaving || (g.inMatch && game::covered())) drawCover(dl, 1.f);  // (also while the game connects, hidden)
        else if (appear > 0.f) drawCover(dl, appear);
    }
    g.nav = g.keyNav = 0;
    setTextInput(g.open && (g.typing || g.osk) && !menu::isOpen());

    if (static const char* s_shot = std::getenv("SA2_ONLINE_SHOT"); s_shot) {
        static const int s_every = std::getenv("SA2_ONLINE_SHOT_EVERY") ? std::max(1, std::atoi(std::getenv("SA2_ONLINE_SHOT_EVERY"))) : 0;
        if ((s_every && s_frames % s_every == 0) || g_shotAt == s_frames) writeShot(s_shot, "online", s_frames);
    }
}

bool isOpen() { return g.open; }

void shutdown() {
    if (g.everOpened) an::shutdown();
}

}  // namespace rt::online

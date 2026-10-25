// City Trial's results with more than 4 riders (src/runtime/results8.h).
//
// The game's results (scene 0xD after a Stadium) show a winner's card and a row for each of 4 players, from the
// Stadium's results at gm +BC8 - which it copies, for 4, from the Stadium's own per-rider results: its ranking (+848,
// from 0), finishing times (+8B8, in frames), points (+A38) and distances (+A4C, in feet), all four Stadium kinds
// alike (in the 8-player build moved to 10 slots, src/runtime/morepl_layout.h; its ranking there is right only for
// the first 4 slots, so the places here are the port's, as the game ranks: see rank). Its layout has 4 rows and the code
// filling it loops over 4, so with 8 riders the port draws the whole screen over it: the Stadium's name on its plate, the
// winner's card with their Kirby (src/runtime/kirbymodel.h), and 8 rows by place - each the rider (P1.. or CPU),
// their Kirby colour, their score as the Stadium counts it and their place.
#include "runtime/results8.h"
#include "runtime/kirbymodel.h"
#include "runtime/menustyle.h"
#include "runtime/morepl.h"
#include "runtime/runtime.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace rt::results8 {

using namespace menustyle;

namespace {

constexpr u32 kGm = 0x805359D8;
constexpr u32 kScene = kGm + 0x7D7;         // the major scene (0xD: City Trial's results)
constexpr u32 kStadiumKind = kGm + 0x5AD;   // the Stadium played (StadiumKind)
constexpr u32 kGroupOfKind = 0x804957C8;    // StadiumKind -> its group (CityTrial_GetCurrentStadiumGroup)
constexpr u8 kResultsScene = 0x0D;

enum Group : int { Drag, Glider, Target, HighJump, Melee, Derby, Single, Dedede, GroupCount };
const char* const kGroupNames[GroupCount] = {"DRAG RACE",  "AIR GLIDER",        "TARGET FLIGHT", "HIGH JUMP",
                                             "KIRBY MELEE", "DESTRUCTION DERBY", "SINGLE RACE",   "VS. KING DEDEDE"};
// Kirby's colours by the game's number for them (a player's setup +4), as its results' rows show them.
constexpr u32 kKirby[8] = {0xF4A0BE, 0xF5D945, 0x5CB9F2, 0x93D96D, 0xF4F4F4, 0xC468E8, 0x7C5634, 0xEC3E46};
// ... and which of the model's colours that is (src/runtime/kirbymodel.h).
constexpr int kModel[8] = {kirbymodel::Pink, kirbymodel::Yellow, kirbymodel::Blue,  kirbymodel::Green,
                           kirbymodel::White, kirbymodel::Purple, kirbymodel::Brown, kirbymodel::Red};

constexpr u32 kGreen = 0xA9BF8F, kGreenDeep = 0x6E8456, kGreenDark = 0x3F4D35, kPlate = 0x3B403A, kRowGrey = 0x8A8A8A;

struct Row {
    u32 slot;
    bool cpu;
    int number;  // P<n> for a person
    int colour;
    int place;
    s32 time, points;
    float distance;
    float progress = 0;
};

int groupNow() {
    const s8 kind = s8(rd8(kStadiumKind));
    const int g = kind >= 0 && kind < 24 ? s8(rd8(kGroupOfKind + u32(kind))) : -1;
    return g >= 0 && g < GroupCount ? g : -1;
}

// The places, as the game ranks a Stadium (its own ranking is right only for the first 4 slots with 8 riders): times
// from the fastest, those who did not finish after them by how far they got; distances and points from the most.
// Equal scores share a place (1, 2, 2, 4).
void rank(std::vector<Row>& rows, int group) {
    const bool timed = group == Drag || group == Single || group == Dedede;
    const bool far = group == Glider || group == HighJump;
    auto key = [&](const Row& r) {
        if (timed) return r.time > 0 ? double(r.time) : 1e9 - double(r.progress);
        return far ? -double(r.distance) : -double(r.points);
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) { return key(a) < key(b); });
    for (size_t i = 0; i < rows.size(); i++) rows[i].place = i && key(rows[i]) == key(rows[i - 1]) ? rows[i - 1].place : int(i) + 1;
}

std::vector<Row> readRows() {
    std::vector<Row> rows;
    int people = 0;
    for (int i = 0; i < morepl::riders(); i++) {
        const u32 slot = morepl::riderSlot(i), d = morepl::plyDesc(slot);
        const u8 kind = rd8(d);  // (0 a person, 1 the console)
        if (kind > 1) continue;
        // (the Stadium's own per-rider results, as the game copies them into its results for 4: finishing time +8B8
        // in frames, points +A38, distance +A4C in feet - in a race how far along the course)
        Row r{slot, kind == 1, kind == 0 ? ++people : 0, int(rd8(d + 4) & 7), 0,
              s32(rd32(morepl::at(morepl::k_race_8B8, slot))), s32(rd32(morepl::at(morepl::k_race_A38, slot))),
              __builtin_bit_cast(float, rd32(morepl::at(morepl::k_race_A4C, slot)))};
        r.progress = r.distance;  // (in the races, +A4C is how far along the course)
        rows.push_back(r);
    }
    rank(rows, groupNow());
    return rows;
}

// A score as the game letters it: times as minutes:seconds:hundredths, distances in feet, the rest as a count.
std::string scoreText(const Row& r, int group) {
    char b[32];
    switch (group) {
    case Drag:
    case Single:
    case Dedede: {  // (VS. King Dedede: the time he was beaten in)
        if (r.time <= 0) return "--:--:--";
        const s32 cs = (r.time * 100 + 59) / 60;  // (frames to hundredths, up: as the game letters them)
        std::snprintf(b, sizeof b, "%02d:%02d:%02d", cs / 6000, cs / 100 % 60, cs % 100);
        break;
    }
    case Glider:
    case HighJump: std::snprintf(b, sizeof b, "%.2fft", double(r.distance)); break;
    case Target:
    case Derby: std::snprintf(b, sizeof b, "%d Pts.", r.points); break;
    default: std::snprintf(b, sizeof b, "%d", r.points); break;  // (Kirby Melee: the count as it is)
    }
    return b;
}

std::string riderName(const Row& r) {
    if (r.cpu) return "CPU";
    char b[8];
    std::snprintf(b, sizeof b, "P%d", r.number);
    return b;
}

// The place on its disc (as the game's: a red numeral on a dark disc; gold, silver, bronze for the first three).
void placeDisc(ImDrawList* dl, ImVec2 c, float r, int place, float a) {
    const u32 ring = 0x2E332C;
    dl->AddCircleFilled(c, r + U(2.5f), col(kInk, a), 32);
    dl->AddCircleFilled(c, r, col(ring, a), 32);
    dl->AddCircleFilled(c, r * 0.78f, col(0x2E332C, a), 32);
    char b[4];
    std::snprintf(b, sizeof b, "%d", place);
    const float size = r * 1.35f;
    const float w = gameMeasure(g_font.bold, size, b);
    const u32 ink = place == 1 ? 0xF0342C : place == 2 ? 0xF4D23A : place == 3 ? 0x5DB04A : 0x9A9A9A;
    gameText(dl, g_font.bold, size, c.x - w * 0.5f, c.y, col(ink, a), b);
}

void colourBall(ImDrawList* dl, ImVec2 c, float r, u32 rgb, float a) {
    dl->AddCircleFilled(c, r + U(2), col(kInk, a), 24);
    dl->AddCircleFilled(c, r, col(rgb, a), 24);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.35f, c.y - r * 0.35f), r * 0.32f, col(kWhite, a * 0.5f), 12);
    for (int s = -1; s <= 1; s += 2)  // (eyes)
        dl->AddEllipseFilled(ImVec2(c.x + s * r * 0.22f, c.y - r * 0.1f), ImVec2(r * 0.09f, r * 0.22f), col(0x14143A, a), 0.f, 12);
}

void draw(const std::vector<Row>& rows, int group, float t, float a) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    g_s = std::max(0.5f, std::min(W / 1280.f, H / 720.f));
    const float ox = (W - U(1280)) * 0.5f, oy = (H - U(720)) * 0.5f;
    auto X = [&](float v) { return ox + U(v); };
    auto Y = [&](float v) { return oy + U(v); };
    const float in = easeOut(clamp01(t / 0.35f));
    // the backdrop: the game's results green across the whole screen, its pale hexagons, its darker bands
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), col(kGreen, a));
    const float hr = U(54);
    for (int row = -1; row * hr * 1.5f < H + hr; row++)
        for (float x = (row & 1) * hr * 0.866f - hr; x < W + hr; x += hr * 1.732f) {
            ImVec2 pts[6];
            for (int k = 0; k < 6; k++) pts[k] = ImVec2(x + hr * 0.92f * std::cos(kPi / 6 + k * kPi / 3), row * hr * 1.5f + hr * 0.92f * std::sin(kPi / 6 + k * kPi / 3));
            dl->AddPolyline(pts, 6, col(kWhite, a * 0.12f), ImDrawFlags_Closed, U(2));
        }
    dl->AddRectFilled(ImVec2(0, Y(640)), ImVec2(W, H), col(kGreenDeep, a * 0.85f));
    dl->AddRectFilled(ImVec2(0, Y(652)), ImVec2(W, H), col(kGreenDark, a * 0.5f));

    // the Stadium's plate (top right): a dark capsule with its name
    const char* title = group >= 0 ? kGroupNames[group] : "RESULTS";
    const float px = X(560) + (1.f - in) * U(500);
    capsule(dl, px - U(8), Y(36), X(1244) + U(8), Y(128), 0.f, true, col(kGreenDeep, a));
    capsule(dl, px, Y(44), X(1244), Y(120), 0.f, true, col(kPlate, a));
    gameText(dl, g_font.bold, U(52), px + U(40), Y(82), col(0xD8DCD2, a), title);

    // the winner's card (left): the place, the rider, their score, their Kirby
    if (!rows.empty()) {
        const Row& w = rows.front();
        const float cx0 = X(40) - (1.f - in) * U(500), cy0 = Y(150);
        const u32 tint = mixRgb(kKirby[w.colour], 0xB03060, 0.35f);
        std::vector<ImVec2> pts;
        roundedRect(pts, ImVec2(cx0, cy0), ImVec2(cx0 + U(470), cy0 + U(430)), U(26), U(26), U(26), U(26));
        dl->AddConvexPolyFilled(pts.data(), int(pts.size()), col(tint, a));
        pts.clear();
        roundedRect(pts, ImVec2(cx0 + U(20), cy0 + U(64)), ImVec2(cx0 + U(450), cy0 + U(410)), U(18), U(18), U(18), U(18));
        dl->AddConvexPolyFilled(pts.data(), int(pts.size()), col(0x3C433A, a));
        gameText(dl, g_font.bold, U(58), cx0 + U(300), cy0 + U(34), col(kWhite, a * 0.9f), riderName(w).c_str());
        placeDisc(dl, ImVec2(cx0 + U(70), cy0 + U(30)), U(48), w.place, a);
        // their Kirby, in their colour, standing on the card
        kirbymodel::draw(dl, ImVec2(cx0 + U(235), cy0 + U(200)), U(210), kModel[w.colour], t, a);
        // the score plate
        const float sy = cy0 + U(352);
        capsule(dl, cx0 + U(40), sy - U(34), cx0 + U(430), sy + U(34), 0.f, true, col(mixRgb(kKirby[w.colour], kWhite, 0.55f), a));
        gameText(dl, g_font.bold, U(22), cx0 + U(62), sy, col(0x505050, a), "SCORE");
        const std::string s = scoreText(w, group);
        // (smaller when long - a time - so it stays clear of SCORE)
        const float room = U(410) - U(62) - gameMeasure(g_font.bold, U(22), "SCORE") - U(14);
        float size = U(46), sw = gameMeasure(g_font.bold, size, s.c_str());
        if (sw > room) size *= room / sw, sw = room;
        gameText(dl, g_font.bold, size, cx0 + U(410) - sw, sy, col(kInk, a), s.c_str());
        // nobody finished (King Dedede not beaten, nobody over the line): the game's RETIRE stamp across the card
        if ((group == Drag || group == Single || group == Dedede) && w.time <= 0) {
            const float rx = cx0 + U(235), ry = cy0 + U(200), rot = -0.35f;
            const float c = std::cos(rot), sn = std::sin(rot), hw = U(190), hh = U(46);
            auto P = [&](float x, float y) { return ImVec2(rx + x * c - y * sn, ry + x * sn + y * c); };
            dl->AddQuadFilled(P(-hw, -hh), P(hw, -hh), P(hw, hh), P(-hw, hh), col(0x4A4F47, a * 0.9f));
            dl->AddQuadFilled(P(-hw + U(8), -hh + U(8)), P(hw - U(8), -hh + U(8)), P(hw - U(8), hh - U(8)), P(-hw + U(8), hh - U(8)), col(0xA9BF8F, a * 0.9f));
            const float tw = gameMeasure(g_font.bold, U(64), "RETIRE");
            gameText(dl, g_font.bold, U(64), rx - tw * 0.5f, ry, col(0x4A4F47, a), "RETIRE");
        }
    }

    // the rows (right): one per rider by place, in from the right one after another
    const float rowH = U(52), gap = U(9);
    const float top = Y(160);
    for (size_t i = 0; i < rows.size(); i++) {
        const Row& r = rows[i];
        const float k = easeOut(clamp01((t - 0.15f - 0.06f * float(i)) / 0.3f));
        const float x0 = X(560) + (1.f - k) * U(760), y0 = top + float(i) * (rowH + gap);
        const float x1 = W + U(20), cy = y0 + rowH * 0.5f;
        const float ra = a * k;
        // the grey bar, the rider's colour band on it
        capsule(dl, x0, y0, x1, y0 + rowH, 0.18f, true, col(kRowGrey, ra));
        capsule(dl, x0 + U(170), y0 + U(6), x0 + U(560), y0 + rowH - U(6), 0.18f, true, col(r.cpu ? 0xBDBDBD : 0xE06FA2, ra));  // (a person's pink)
        const std::string name = riderName(r);
        gameText(dl, g_font.bold, U(30), x0 + U(26), cy, col(r.cpu ? 0x3A3A3A : 0xC8285A, ra), name.c_str());
        colourBall(dl, ImVec2(x0 + U(140), cy), U(17), kKirby[r.colour], ra);
        const std::string s = scoreText(r, group);
        const float sw = gameMeasure(g_font.bold, U(32), s.c_str());
        gameText(dl, g_font.bold, U(32), x0 + U(530) - sw, cy, col(kInk, ra), s.c_str());
        placeDisc(dl, ImVec2(x0 + U(600), cy), U(21), r.place, ra);
    }
}

// ---- the machine stats after the city (scene 0x10) ----
// Each rider's 9 stats (gm +3A0, 9 per slot - in the 8-player build moved to 10 slots), as the game's own screen
// shows them (Patch_GetPlySavedValue 8000AB48): in the order Boost, Top Speed, Turn, Charge, Glide, Weight, Offense,
// Defense, HP - the array's 1 2 3 4 5 0 6 7 8 - each less the lowest a stat goes (-2; HP 0). Its screen is the four
// views' quarters (the code reaching the stats, through a function returning their address, reads them where the
// game had them: with 8 riders, a 2 on every row); this one has 8 boards, two rows of four.
constexpr u8 kStatsScene = 0x10;
struct StatLine {
    const char* name;
    u32 colour;
    int index, low;
};
const StatLine kStats[9] = {{"Boost", 0xE048E0, 1, -2},  {"Top Speed", 0x38D0F0, 2, -2}, {"Turn", 0x58E058, 3, -2},
                            {"Charge", 0xF0E040, 4, -2}, {"Glide", 0xF2EED6, 5, -2},     {"Weight", 0xB8864A, 0, -2},
                            {"Offense", 0xF07028, 6, -2}, {"Defense", 0x6C8CF4, 7, -2},  {"HP", 0xF04858, 8, 0}};

void drawStats(const std::vector<Row>& rows, float t, float a) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    g_s = std::max(0.5f, std::min(W / 1280.f, H / 720.f));
    // the backdrop: the game's dark green with its grid
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), col(0x0E2410, a));
    for (float x = std::fmod(W * 0.5f, U(48)); x < W; x += U(48)) dl->AddLine(ImVec2(x, 0), ImVec2(x, H), col(0x2E5A2E, a * 0.6f), U(2));
    for (float y = std::fmod(H * 0.5f, U(48)); y < H; y += U(48)) dl->AddLine(ImVec2(0, y), ImVec2(W, y), col(0x2E5A2E, a * 0.6f), U(2));
    // 8 boards: two rows of four, a band between them as the game's
    const float bandH = U(44), bw = W / 4.f, bh = (H - bandH) / 2.f;
    dl->AddRectFilled(ImVec2(0, bh), ImVec2(W, bh + bandH), col(0x000000, a));
    if (t > 1.2f) {
        const char* press = "PRESS A";
        const float pw = gameMeasure(g_font.bold, U(30), press);
        gameText(dl, g_font.bold, U(30), W * 0.5f - pw * 0.5f, bh + bandH * 0.5f, col(0x8ED08A, a * (0.65f + 0.35f * std::sin(t * 4.f))), press);
    }
    for (size_t i = 0; i < rows.size() && i < 8; i++) {
        const Row& r = rows[i];
        const float x0 = bw * float(i % 4), y0 = (i < 4) ? 0.f : bh + bandH;
        const float k = easeOut(clamp01((t - 0.05f * float(i)) / 0.3f)), ba = a * k;
        dl->AddRect(ImVec2(x0 + U(2), y0 + U(2)), ImVec2(x0 + bw - U(2), y0 + bh - U(2)), col(0x000000, ba), 0.f, 0, U(4));
        // the rider: P1.. or CPU, their Kirby's colour
        const std::string name = riderName(r);
        colourBall(dl, ImVec2(x0 + U(30), y0 + bh - U(30)), U(14), kKirby[r.colour], ba);
        gameText(dl, g_font.bold, U(30), x0 + U(50), y0 + bh - U(30), col(r.cpu ? 0xB8B8B8 : 0xF07AB0, ba), name.c_str());
        const u32 st = morepl::at(morepl::k_ct_ply_stats, r.slot);
        const float rowH = (bh - U(66)) / 9.f;
        for (int n = 0; n < 9; n++) {
            const StatLine& L = kStats[n];
            const int v = int(std::lround(__builtin_bit_cast(float, rd32(st + u32(L.index) * 4)))) - L.low;
            const float cy = y0 + U(18) + rowH * (float(n) + 0.5f);
            gameText(dl, g_font.bold, std::min(U(24), rowH * 0.95f), x0 + U(28), cy, col(L.colour, ba), L.name);
            char b[8];
            std::snprintf(b, sizeof b, "%d", v);
            const float vx = x0 + bw * 0.72f;
            const float vw = gameMeasure(g_font.bold, std::min(U(24), rowH * 0.95f), b);
            gameText(dl, g_font.bold, std::min(U(24), rowH * 0.95f), vx - vw, cy, col(0xE8E8E8, ba), b);
            if (v > 0) {
                const float len = std::min(float(v), 12.f) * U(9);
                dl->AddRectFilled(ImVec2(vx + U(6), cy - rowH * 0.3f), ImVec2(vx + U(6) + len, cy + rowH * 0.3f), col(0xF03030, ba));
                dl->AddRect(ImVec2(vx + U(6), cy - rowH * 0.3f), ImVec2(vx + U(6) + len, cy + rowH * 0.3f), col(0x200000, ba), 0.f, 0, U(1.5f));
            }
        }
    }
}

}  // namespace

void overlay(bool headless) {
    if (!morepl::active() || !ImGui::GetCurrentContext()) return;
    static double s_since = -1;
    const double now = ImGui::GetTime();
    static u8 s_scene = 0;
    const u8 scene = rd8(kScene);
    const bool results = scene == kResultsScene, stats = scene == kStatsScene;
    if ((!results && !stats) || scene != s_scene) s_since = -1;
    s_scene = scene;
    if (!results && !stats) return;
    std::vector<Row> rows = readRows();
    if (rows.size() <= 4) return;  // (the game's own screen has room)
    if (stats) {  // the machine stats after the city: by slot, as the game's
        std::stable_sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return x.slot < y.slot; });
        if (s_since < 0) s_since = now;
        gameart::load();
        const float t = float(now - s_since);
        drawStats(rows, t, easeOut(clamp01(t / 0.25f)));
        if (static const char* s_shot = std::getenv("SA2_RESULTS_SHOT"); s_shot) {
            std::error_code ec;
            std::filesystem::create_directories(s_shot, ec);
            static int s_n = 0, s_last = -1;
            if (int(t) != s_last && t > 1.5f) s_last = int(t), writeShot(s_shot, "stats", ++s_n);
        }
        return;
    }
    if (s_since < 0) {
        s_since = now;
        std::string line;
        for (const Row& r : rows) {
            char b[96];
            std::snprintf(b, sizeof b, " [slot %u %s place %d time %d points %d distance %.2f]", r.slot, riderName(r).c_str(), r.place, r.time,
                          r.points, double(r.distance));
            line += b;
        }
        log("[results8] %zu riders, stadium group %d:%s\n", rows.size(), groupNow(), line.c_str());
    }
    gameart::load();
    const float t = float(now - s_since);
    draw(rows, groupNow(), t, easeOut(clamp01(t / 0.25f)));
    // SA2_RESULTS_SHOT=<dir> (tests): what it drew, every second it shows (as the menus' shots)
    if (static const char* s_shot = std::getenv("SA2_RESULTS_SHOT"); s_shot) {
        static const bool s_made = [] {
            std::error_code ec;
            std::filesystem::create_directories(std::getenv("SA2_RESULTS_SHOT"), ec);
            return true;
        }();
        (void)s_made;
        static int s_n = 0, s_last = -1;
        const int sec = int(t);
        if (sec != s_last && t > 0.6f) s_last = sec, writeShot(s_shot, "results", ++s_n);
    }
    (void)headless;
}

}  // namespace rt::results8

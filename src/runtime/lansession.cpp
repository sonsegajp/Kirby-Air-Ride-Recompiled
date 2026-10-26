// Kirby Air Ride's LAN mode started by host code, none of its LAN screens shown (src/runtime/lansession.h has
// the interface).
//
// How the game's LAN mode works (main.dol, GKYE01; HAL's gmlanmenu.c over lbnet.c over its net library):
//   The game runs "modes" (gm global 805359D8 = f_80006C14(): +7D4 mode, +7D5 next mode, +7D6 mode over),
//   each a chain of "major scenes" (+7D7 scene, +7D8 next scene, +7E0 scene over, +81C how it ended).
//   Mode table 80495058 (12 bytes: id, next mode, first scene, -, enter, after-each-scene), scene table
//   80495154 (0x24 bytes: id, next scene, -, enter, exit, frame, ...). Select Mode is mode 3 (scene 2, frame
//   800189E4); choosing LAN ends it with +81C = 0x10 and its step goes to mode 0x13: the LAN menu, scene 0x28
//   (enter 8004F77C, exit 8004F83C, frame 80051028).
//   The LAN menu's state is at 80537430 (+0 consoles, +14 screen, +18 screen to return to, +20.. cursors). Its
//   frame (80051028) shows the screen of +14 and runs it:
//     1 (8004F85C) a Yes/No entry dialog          2 (8004FA38) "How many Nintendo GameCube systems?" 2/3/4
//     3 (8004FC48) "Is it OK to connect via LAN?" - Yes starts lbnet (8007B7B0)
//     4 (8004FE28) "Connecting...": the IP stack's link and address (8007B800); then the session for +0 consoles
//        (8007B8D4: the net library's connect thread 804876A0 finds the others by SSDP, sorts their addresses -
//        console 0, the master, lowest - and hands round the count and the list over UDP 26502)
//     5 (800500AC) waiting for the others; done: 40 frames, then the rules reset to their LAN defaults
//        (80007CE4), each console's unlocks into the circulation (80007AF0/8007CAB0) and the lockstep starts
//        (from here on every console's game steps on the same input sets, src/runtime/lansync.cpp)
//     6 (800502F0) LAN mode select: Air Ride / City Trial / Quit - all pads of all consoles drive it, in step;
//        A first agrees the random seed (8007CCAC) and merges the unlocks (80007C10)
//     7 (80050514) Air Ride: Start (mode 4, 8003D5B8(0) Air Ride, 8003D614(pad)) / Rules (scene 3)
//     8 (800507EC) City Trial: City Trial / Stadium / Free Run (mode 6, 8003F694(0/1/2)) / Rules (scene 5)
//     9 (80050A40) "Quit?"  10 everyone quitting -> disconnect   11 (80050C50) an error: Retry / Cancel
//    12 disconnecting (8007B820 .. 8007B860 .. 8007B880), then back to screen +18
//   The matches are the game's own Air Ride and City Trial modes (4 and 6) with lbnet on: Air Ride runs course
//   select (scene 6), Player Entry with machine choice (8), the race (0x12), results (0xB); City Trial Player
//   Entry (0xA), the city (0x12), the machine stats (0x10), the Stadium (0x11, 0x12), results (0xD) - and then
//   Player Entry again, still connected. Rules live at gm +354 (Air Ride, 0x20 bytes) and +394 (City Trial,
//   0xF0), with menu copies at +5C / +BC (8001A3C0 / 8001F950 fill them; Air Ride's mode copies the menu back).
//
// What this does instead (every change below happens either before the lockstep starts or in a game step that
// is the same on every console - so the consoles stay in step):
//   - begin() from Select Mode ends it as if LAN were chosen; on the LAN menu's first frame it sets the count,
//     starts lbnet and jumps to "Connecting..." (screen 4). Screens 4 and 5 run as the game's, their pictures
//     hidden (the 9 LAN menu screens and its texts switched off after each frame) - with local DHCP answered
//     at once (lan::setLocalDhcp) this takes about a second; the console order follows the lobby's
//     (lan::setLocalDhcpPlace: the game orders consoles by address).
//   - On the first step of screen 6 (the lockstep's first) it agrees the seed and merges the unlocks as A would,
//     writes the rules and starts the mode - the LAN menu's own frame does not run then.
//   - In the match: the course select's cursor is set and A pressed; in Player Entry every console's player is
//     entered and Start pressed when ready (in-game input, in step); Air Ride's machine choice is the
//     players' (or Quick: each console presses A for its own player, through its pads like a person); the
//     machine stats and results go on by themselves after a few seconds.
//   - When the results end, the game goes back to the LAN menu (mode 0x13): its screen 6, connected, in step,
//     the LAN menu's frame not run (nothing shown, the pads ignored): Parked. A rematch: each console that has
//     the next begin() holds a sign on its own pad (all four d-pad directions - no real pad can - and 6 bits
//     of the settings' hash as A B X Y Z Start); in the step where every console's pad shows the same sign,
//     every console starts it.
//   - abort(): the game's own cancel (screen 12: disconnect, then back to "How many", hidden).
// SA2_LAN_SESSION_TEST="consoles=2;index=0;game=citytrial;ct.minutes=3;..." (tests): begin() once the game
// reaches Select Mode (settings as in JSON, key=value); "matches=<n>": begin() again after each match, n in all;
// "quick=1": Air Ride machines Quick. SA2_LAN_SESSION_TRACE=1 logs scenes and LAN menu screens.
#include "runtime/lansession.h"

#include "runtime/interp.h"
#include "runtime/lan.h"
#include "runtime/morepl.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace rt::online {
bool isOpen();  // src/runtime/online_menu.h
}

namespace rt::lansession {

// ---------------------------------------------------------------------------------------------------
// The catalogue

namespace {
const char* const kCourseIds[] = {"fantasy_meadows", "celestial_valley", "sky_sands", "frozen_hillside", "magma_flows",
                                  "beanstalk_park", "machine_passage", "checker_knights", "nebula_belt", "random"};
const char* const kCourseNames[] = {"Fantasy Meadows", "Celestial Valley", "Sky Sands", "Frozen Hillside", "Magma Flows",
                                    "Beanstalk Park", "Machine Passage", "Checker Knights", "Nebula Belt", "Random"};
const char* const kRuleIds[] = {"laps", "time"};
const char* const kRuleNames[] = {"Laps", "Time"};
const char* const kNoneOnIds[] = {"none", "on"};
const char* const kNoneOnNames[] = {"None", "On"};
const char* const kHelpIds[] = {"none", "weak", "strong"};
const char* const kHelpNames[] = {"None", "Weak", "Strong"};
const char* const kOnOffIds[] = {"on", "off"};
const char* const kOnOffNames[] = {"On", "Off"};
const char* const kTempoIds[] = {"normal", "slow"};
const char* const kTempoNames[] = {"Normal", "Slow"};
const char* const kMachineIds[] = {"choose", "quick"};
const char* const kMachineNames[] = {"Players choose", "Quick"};
const char* const kStadiumIds[] = {"shuffle", "drag_race", "air_glider", "target_flight", "high_jump", "kirby_melee",
                                   "destruction_derby", "single_race", "vs_king_dedede"};
const char* const kStadiumNames[] = {"Shuffle", "Drag Race", "Air Glider", "Target Flight", "High Jump", "Kirby Melee",
                                     "Destruction Derby", "Single Race", "VS. King Dedede"};

const SettingInfo kInfo[kSettingCount] = {
    {"ar.course", "Course", "The course raced (Random: a different one each time).", Game::AirRide, false, 0, 9, 0, kCourseIds,
     kCourseNames, nullptr, nullptr},
    {"ar.rule", "Rules", "Race a number of laps, or for a time.", Game::AirRide, false, 0, 1, 0, kRuleIds, kRuleNames, nullptr, nullptr},
    {"ar.laps", "Laps", "Laps per race (Rules: Laps).", Game::AirRide, false, 0, 99, 0, nullptr, nullptr, "laps", "Course default"},
    {"ar.minutes", "Time", "Length of the race (Rules: Time).", Game::AirRide, false, 1, 99, 2, nullptr, nullptr, "min", nullptr},
    {"ar.damage", "Damage", "Whether riders take damage.", Game::AirRide, false, 0, 1, 0, kNoneOnIds, kNoneOnNames, nullptr, nullptr},
    {"ar.speed_help", "Speed Help", "Catch-up speed for riders behind.", Game::AirRide, false, 0, 2, 1, kHelpIds, kHelpNames, nullptr,
     nullptr},
    {"ar.enemies", "Enemies", "Enemies on the course.", Game::AirRide, false, 0, 1, 0, kOnOffIds, kOnOffNames, nullptr, nullptr},
    {"ar.tempo", "Game Tempo", "The game's speed.", Game::AirRide, false, 0, 1, 0, kTempoIds, kTempoNames, nullptr, nullptr},
    {"ar.machines", "Machines", "Players choose their machine, or Quick: the highlighted one, at once.", Game::AirRide, false, 0, 1,
     0, kMachineIds, kMachineNames, nullptr, nullptr},
    {"ct.minutes", "Time", "The city's time limit before the Stadium.", Game::CityTrial, false, 3, 7, 5, nullptr, nullptr, "min",
     nullptr},
    {"ct.tempo", "Game Tempo", "The game's speed.", Game::CityTrial, false, 0, 1, 0, kTempoIds, kTempoNames, nullptr, nullptr},
    {"ct.stadium", "Stadium", "The Stadium event after the city (Shuffle: picked at random).", Game::CityTrial, false, 0, 8, 0,
     kStadiumIds, kStadiumNames, nullptr, nullptr},
    {"ct.events", "Events", "Events in the city (meteors, the Dedede ambush, ...).", Game::CityTrial, false, 0, 1, 0, kOnOffIds,
     kOnOffNames, nullptr, nullptr},
    {"results_seconds", "Results", "How long the results stay before the lobby (Start: at once).", Game::CityTrial, true, 3, 60, 12,
     nullptr, nullptr, "s", nullptr},
};
}  // namespace

const SettingInfo& info(Setting s) { return kInfo[std::clamp<int>(s, 0, kSettingCount - 1)]; }

Settings::Settings() {
    for (int i = 0; i < kSettingCount; i++) value[i] = kInfo[i].def;
}
void Settings::set(Setting s, int v) {
    if (s < 0 || s >= kSettingCount) return;
    value[s] = std::clamp(v, kInfo[s].min, kInfo[s].max);
}
bool Settings::operator==(const Settings& o) const { return game == o.game && std::memcmp(value, o.value, sizeof value) == 0; }

std::string toJson(const Settings& s) {
    std::string j = std::string("{\"game\":\"") + (s.game == Game::AirRide ? "airride" : "citytrial") + "\"";
    for (int i = 0; i < kSettingCount; i++) {
        const SettingInfo& in = kInfo[i];
        j += ",\"" + std::string(in.key) + "\":";
        if (in.choiceIds) j += "\"" + std::string(in.choiceIds[s.value[i] - in.min]) + "\"";
        else j += std::to_string(s.value[i]);
    }
    return j + "}";
}

bool fromJson(std::string_view j, Settings& out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    Settings s;
    size_t p = 0;
    auto ws = [&] {
        while (p < j.size() && (j[p] == ' ' || j[p] == '\t' || j[p] == '\n' || j[p] == '\r')) p++;
    };
    auto str = [&](std::string& v) {  // a JSON string (escapes: the simple ones; settings use plain ASCII)
        if (p >= j.size() || j[p] != '"') return false;
        for (p++; p < j.size() && j[p] != '"'; p++) {
            if (j[p] == '\\' && p + 1 < j.size()) p++;
            v += j[p];
        }
        if (p >= j.size()) return false;
        p++;
        return true;
    };
    ws();
    if (p >= j.size() || j[p] != '{') return fail("not a JSON object");
    p++;
    for (;;) {
        ws();
        if (p < j.size() && j[p] == '}') break;
        std::string key;
        if (!str(key)) return fail("expected a key");
        ws();
        if (p >= j.size() || j[p] != ':') return fail("expected ':'");
        p++;
        ws();
        std::string sv;
        long long nv = 0;
        bool isStr = false, isNum = false;
        if (p < j.size() && j[p] == '"') {
            if (!str(sv)) return fail("bad string");
            isStr = true;
        } else if (p < j.size() && (j[p] == '-' || (j[p] >= '0' && j[p] <= '9'))) {
            const size_t b = p;
            if (j[p] == '-') p++;
            while (p < j.size() && j[p] >= '0' && j[p] <= '9') p++;
            if (p < j.size() && (j[p] == '.' || j[p] == 'e' || j[p] == 'E')) {  // (a fraction: its whole part)
                while (p < j.size() && (j[p] == '.' || j[p] == 'e' || j[p] == 'E' || j[p] == '+' || j[p] == '-' || (j[p] >= '0' && j[p] <= '9'))) p++;
            }
            nv = std::atoll(std::string(j.substr(b, p - b)).c_str());
            isNum = true;
        } else {  // true / false / null / nested: skipped
            int depth = 0;
            while (p < j.size()) {
                const char ch = j[p];
                if (ch == '"') {
                    std::string skip;
                    if (!str(skip)) return fail("bad string");
                    continue;
                }
                if (ch == '{' || ch == '[') depth++;
                else if (ch == '}' || ch == ']') {
                    if (depth == 0) break;
                    depth--;
                } else if (ch == ',' && depth == 0) break;
                p++;
            }
        }
        if (key == "game") {
            if (isStr && (sv == "airride" || sv == "air_ride")) s.game = Game::AirRide;
            else if (isStr && (sv == "citytrial" || sv == "city_trial")) s.game = Game::CityTrial;
            else if (isNum) s.game = nv == 0 ? Game::AirRide : Game::CityTrial;
            else return fail("unknown game");
        } else {
            for (int i = 0; i < kSettingCount; i++) {
                const SettingInfo& in = kInfo[i];
                if (key != in.key) continue;
                if (isNum) s.set(Setting(i), int(std::clamp<long long>(nv, -1000000, 1000000)));
                else if (isStr && in.choiceIds) {
                    for (int k = 0; k <= in.max - in.min; k++)
                        if (sv == in.choiceIds[k]) s.set(Setting(i), in.min + k);
                }
            }
        }
        ws();
        if (p < j.size() && j[p] == ',') {
            p++;
            continue;
        }
        ws();
        if (p < j.size() && j[p] == '}') break;
        return fail("expected ',' or '}'");
    }
    out = s;
    return true;
}

uint32_t hash(const Settings& s) {
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) {
        for (int k = 0; k < 4; k++) h = (h ^ ((v >> (8 * k)) & 0xFF)) * 16777619u;
    };
    mix(uint32_t(s.game));
    for (int i = 0; i < kSettingCount; i++) mix(uint32_t(s.value[i]));
    return h;
}

const char* phaseName(Phase p) {
    switch (p) {
    case Phase::Idle: return "idle";
    case Phase::Waiting: return "waiting";
    case Phase::Connecting: return "connecting";
    case Phase::InMatch: return "in match";
    case Phase::Parked: return "parked";
    case Phase::Leaving: return "leaving";
    case Phase::Failed: return "failed";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------------
// The game

namespace {
// ---- main.dol (GKYE01) ----
constexpr u32 kGm = 0x805359D8;  // f_80006C14(): the game's state
constexpr u32 kGmMode = kGm + 0x7D4, kGmNextMode = kGm + 0x7D5, kGmMajor = kGm + 0x7D7, kGmSceneOver = kGm + 0x7E0,
              kGmExitCode = kGm + 0x81C;
constexpr u32 kAr = kGm + 0x354, kCt = kGm + 0x394;  // the rules (Air Ride, City Trial)
constexpr u32 kCourseSel = kGm + 0xD4;               // the course select's state: +2 cursor, +0xC.. the slots' courses
constexpr u32 kLm = 0x80537430;                      // the LAN menu's state (+0 consoles, +14 screen, +18 return screen)
constexpr u32 kNet = 0x805C3C40;                     // the net library's (+4C consoles, +4D our place)
constexpr u32 kPads = 0x8058B634, kPadSize = 0x44;   // HSD's pads: 0-3, then console c's port p is 4 + 4c + p
constexpr u32 kMenuPads = 0x80537000, kMenuPadSize = 0x30;  // the menus' table of the same (20: every pad)
constexpr int kModeSelect = 3, kModeAirRide = 4, kModeCityTrial = 6, kModeLan = 0x13;
constexpr int kSceneSelectMode = 2, kSceneLanMenu = 0x28;
// functions
constexpr u32 kSceneStart = 0x8000891C, kLanMenuFrame = 0x80051028, kSelectModeFrame = 0x800189E4, kPadRead = 0x80411D40;
constexpr u32 kCourseFrame = 0x8003C734, kArEntryFrame = 0x8002A3E0, kCtEntryFrame = 0x8003B4C8, kStatsFrame = 0x800461C4,
              kArResultsFrame = 0x80043B10, kCtResultsFrame = 0x80045A6C;
constexpr u32 kSetNextMode = 0x800082A0, kEndMode = 0x80008220, kEndScene = 0x800064F0;
constexpr u32 kLbNetStart = 0x8007B7B0, kLbNetError = 0x8007B6AC, kLbNetDisconnect = 0x8007B820, kLbNetActive = 0x8007B650;
constexpr u32 kAgreeSeed = 0x8007CCAC, kMergeUnlocks = 0x80007C10;
constexpr u32 kSetAirRideKind = 0x8003D5B8, kSetAirRidePad = 0x8003D614, kSetCityKind = 0x8003F694, kSetCourseSelection = 0x8003D668;
constexpr u32 kArRulesToMenu = 0x8001A3C0, kCtRulesToMenu = 0x8001F950;
constexpr u32 kHideScreens[] = {0x801832C4, 0x80183844, 0x8018433C, 0x80184828, 0x80184C0C, 0x80184E90, 0x801850A8, 0x80185358, 0x80185654};
constexpr u32 kHideTexts[] = {0x8017C90C, 0x8017C990, 0x8017CA38, 0x8017C9C8, 0x8017CA00};
// An instruction of each, to be sure this is the program they were found in.
constexpr struct {
    u32 addr, word;
} kSignature[] = {
    {0x80051044, 0x83DF0014},  // LAN menu frame: lwz r30, 0x14(r31) (the screen)
    {0x80051848, 0x38600005},  // LAN menu frame: li r3, 5 (before running the screen)
    {0x800500AC, 0x9421FFF0},  // screen 5
    {0x80050044, 0x981F0000},  // screen 4: stb r0, 0(r31) (the console count)
    {0x8004FDB0, 0x4802BA01},  // screen 3: bl lbnet start
    {0x80015CA0, 0x38600013},  // Select Mode's step: li r3, 0x13 (LAN)
    {0x80411D68, 0x4BFCAB81},  // pad read: bl PADRead
    {0x8003BB24, 0x88630016},  // course select: lbz r3, 0x16(r3)
    {0x80007E04, 0x480127B5},  // LAN rules reset: bl Air Ride rules to menu
    {0x8001A3E0, 0x7C800775},  // Air Ride rules to menu
    {0x8001F978, 0xA81F0394},  // City Trial rules to menu: lha r0, 0x394(r31)
    {0x800064FC, 0x48000719},  // end scene
};

// Buttons (the game's PADStatus / HSD bits)
constexpr u32 kA = 0x100, kB = 0x200, kX = 0x400, kY = 0x800, kZ = 0x10, kStart = 0x1000, kDpad = 0xF;
constexpr u32 kSignBits[6] = {kA, kB, kX, kY, kZ, kStart};

// ---- the session (game thread, but for the request and the status: s_lock) ----
std::mutex s_lock;
struct Request {
    bool pending = false;
    int consoles = 0, index = 0;  // the PCs (2-8: the game's consoles are the first 4 - src/runtime/lan.h)
    uint32_t id = 0;
    Settings settings;
} s_req;
bool s_abortReq = false, s_abortToSelect = false, s_hideLanMenu = false;
Status s_status;
std::function<void(const MatchResult&)> s_onMatchOver;

// game thread only
bool s_installed = false, s_trace = false;
Phase s_phase = Phase::Idle;
Request s_cur;            // the session in progress
bool s_rematchSign = false;  // Parked: this console shows the sign for s_cur
bool s_toSelectAfterLeave = false;
int s_matchNumber = 0;
u32 s_sceneSteps = 0, s_sceneMajor = ~0u, s_sceneSerial = 0, s_lastLanScreen = ~0u, s_phaseSteps = 0;
bool s_matchStarted = false;  // InMatch: the match's play scene ran (a results scene now ends it)
u32 s_matchSerial = 0;  // InMatch: s_sceneSerial when it began (the LAN menu's frame may run again before the scene ends)
std::chrono::steady_clock::time_point s_beginAt{};
u32 s_localButtons = 0, s_localCalls = 0;
// SA2_SOLO_TEST="riders=8;citysecs=60;matches=1;ct.stadium=kirby_melee;..." (tests): City Trial on this console
// alone, started from Select Mode: player 1 human (entered, Start pressed), the other riders CPUs (riders: 1-8);
// the machine stats and results go on by themselves; the program exits after the matches.
struct Solo {
    bool on = false, begun = false, inMatch = false;
    int riders = 4, matches = 1, done = 0;
    bool idle = false;
    bool allCpu = false;  // (allcpu=1: player 1 a CPU too - every Stadium ends as it should, nobody idle)
    Settings settings;
} s_solo;
constexpr u32 kCtSetup = 0x8002DDD8;   // City Trial's Player Entry over: the players into the match (gm +AC8 .. +BB8)
constexpr u32 kSetRiderCount = 0x8000922C;  // (its count of riders, human and CPU)
int s_testCitySeconds = 0;  // (SA2_LAN_SESSION_TEST "citysecs": a city shorter than the menu allows)  // pressed on this console's own pad (port 1) for the next calls

void setPhase(Phase p, const char* why = nullptr) {
    if (p == s_phase) return;
    log("[lansession] %s -> %s%s%s\n", phaseName(s_phase), phaseName(p), why ? ": " : "", why ? why : "");
    s_phase = p;
    s_phaseSteps = 0;
    if (p == Phase::Idle || p == Phase::Failed) lan::setSession(0, 0, 0);  // (the game's own LAN menus again)
    std::lock_guard g(s_lock);
    s_status.phase = p;
    s_status.cover = p == Phase::Connecting || p == Phase::Parked || p == Phase::Leaving || p == Phase::Waiting;
    if (p != Phase::Failed) s_status.error.clear();
}
void fail(const std::string& why) {
    {
        std::lock_guard g(s_lock);
        s_status.error = why;
    }
    setPhase(Phase::Failed, why.c_str());
}
long long msSinceBegin() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - s_beginAt).count();
}

// In-game input, in this step (on every console alike: the same step everywhere).
void pressInGame(u32 pad, u32 buttons) {
    const u32 p = kPads + pad * kPadSize;
    for (u32 o : {0u, 8u, 0xCu}) wr32(p + o, rd32(p + o) | buttons);  // held, pressed, repeated
    for (u32 i : {pad, 20u}) {
        const u32 m = kMenuPads + i * kMenuPadSize;
        for (u32 o : {4u, 0xCu, 0x14u}) wr32(m + o, rd32(m + o) | buttons);
    }
}
// Input on this console's own pad: it reaches every console through the lockstep, as a player's would.
void pressLocal(u32 buttons, u32 calls = 3) {
    s_localButtons = buttons;
    s_localCalls = calls;
}
u32 heldInGame(u32 pad) { return rd32(kPads + pad * kPadSize); }

int consoles() { return std::clamp<int>(rd8(kNet + 0x4C), 1, 4); }

u32 sceneStep() {  // steps of the scene in progress (the same on every console in the lockstep)
    if (s_sceneSerial != s_sceneMajor) s_sceneMajor = s_sceneSerial, s_sceneSteps = 0;
    return ++s_sceneSteps;
}

// ---- the rules ----
void applyRules(Ctx& c, const Settings& s) {
    const auto v = [&](Setting k) { return s.get(k); };
    // Air Ride (gm +354: +0 rule, +2 laps, +4 seconds, +6 flags: 20 damage, C0 speed help, 10 enemies off,
    // 0C tempo 1/2; +0xE course selection)
    wr8(kAr + 0, u32(v(ArRule)));
    wr16(kAr + 2, u32(v(ArLaps)));
    wr16(kAr + 4, u32(v(ArMinutes) * 60));
    u32 f = rd8(kAr + 6) & 0x03;
    f |= u32(v(ArDamage)) << 5;
    f |= u32(v(ArSpeedHelp)) << 6;
    f |= (v(ArEnemies) == 0 ? 1u : 0u) << 4;
    f |= (v(ArTempo) == 0 ? 1u : 2u) << 2;
    wr8(kAr + 6, f);
    guestCall(c, kSetCourseSelection, {0});  // (course selection on: the course is ours to pick)
    guestCall(c, kArRulesToMenu);            // (the Air Ride mode copies the menu's back over these)
    // City Trial (gm +394: +0 seconds, +2 stadium, +3 flags: C0 tempo 1/2, 20 events)
    wr16(kCt + 0, u32(v(CtMinutes) * 60));
    wr8(kCt + 2, u32(v(CtStadium)));
    u32 g = rd8(kCt + 3) & 0x1F;
    g |= (v(CtTempo) == 0 ? 1u : 2u) << 6;
    g |= (v(CtEvents) == 0 ? 1u : 0u) << 5;
    wr8(kCt + 3, g);
    guestCall(c, kCtRulesToMenu);
}

// The solo test: City Trial from Select Mode, no LAN.
void soloStart(Ctx& c) {
    applyRules(c, s_solo.settings);
    if (s_testCitySeconds) wr16(kCt, u32(s_testCitySeconds));
    guestCall(c, kSetNextMode, {kModeCityTrial});
    guestCall(c, kSetCityKind, {0});
    guestCall(c, kEndScene);
    guestCall(c, kEndMode);
    s_solo.inMatch = true;
    log("[solo] City Trial, %d riders\n", s_solo.riders);
}
// Player Entry is over: riders 2.. are CPUs (copies of player 1's entry: another color, a CPU level).
void ctSetup(Ctx& c, u32 entry) {
    interpOriginal(c, entry);
    if (s_trace) {  // (SA2_LAN_SESSION_TRACE: the players the game entered, and their views)
        for (u32 slot = 0; slot < morepl::count(morepl::k_ply_desc); slot++) {
            char b[200];
            int k = std::snprintf(b, sizeof b, "[lansession] ply_desc %u:", slot);
            for (u32 o = 0; o < 0x30; o++) k += std::snprintf(b + k, sizeof b - size_t(k), "%s%02X", o % 4 ? "" : " ", rd8(morepl::plyDesc(slot) + o));
            const u32 v = morepl::at(morepl::k_view_desc, slot);
            log("%s | view %02X %02X %02X\n", b, rd8(v), rd8(v + 1), rd8(v + 2));
        }
    }
    if (morepl::active() && lan::sessionPcs() > 0 && s_phase == Phase::InMatch) {
        // A session of ours in the 8-player build. Riders 6-9: the game's Player Entry (its loops raised to 10)
        // leaves copies there (a CPU, a "human" on pad 8) - none; then, with more than 4 PCs, PCs 4-7's riders,
        // humans on their seats' pads (src/runtime/lan.h), only this PC's own with a screen.
        const int pcs = lan::sessionPcs(), me = lan::sessionMe();
        for (u32 slot = 6; slot < 10; slot++) wr8(morepl::plyDesc(slot), 4);
        u32 used = 0;
        for (int i = 0; i < 4; i++)
            if (rd8(morepl::plyDesc(u32(i))) != 4) used |= 1u << (rd8(morepl::plyDesc(u32(i)) + 4) & 7);
        for (int pc = 4; pc < pcs; pc++) {
            const u32 slot = u32(lan::riderOf(pc)), d = morepl::plyDesc(slot), from = morepl::plyDesc(0);
            for (u32 k = 0; k < 0x30; k += 4) wr32(d + k, rd32(from + k));  // (as player 1's entry: a human)
            u32 colour = 0;
            while (colour < 7 && (used >> colour & 1)) colour++;
            used |= 1u << colour;
            wr8(d + 4, colour);
            wr8(d + 6, u32(lan::padOf(pc)));  // its controller: its seat's pad
            const u32 v = morepl::at(morepl::k_view_desc, slot);
            wr8(v + 0, 0xFF);
            wr8(v + 1, pc == me ? 1 : 0);
            wr8(v + 2, 0xFF);
        }
        int riders = 0;
        for (int i = 0; i < morepl::riders(); i++) riders += rd8(morepl::plyDesc(morepl::riderSlot(i))) != 4 ? 1 : 0;
        guestCall(c, kSetRiderCount, {u32(riders)});
        log("[lansession] %d riders for %d PCs (this one's: slot %d)\n", riders, pcs, pcs > 4 ? lan::riderOf(me) : -1);
        return;
    }
    if (!s_solo.inMatch || s_solo.riders < 2) return;
    const int riders = std::min(s_solo.riders, morepl::riders());
    for (int i = 1; i < riders; i++) {
        const u32 slot = morepl::riderSlot(i);
        const u32 d = morepl::plyDesc(slot), desc = morepl::plyDesc(0);
        for (u32 k = 0; k < 0x30; k += 4) wr32(d + k, rd32(desc + k));
        wr8(d + 0, s_solo.idle && slot >= 6 ? 0 : 1);  // CPU (or, idle=1, riders 6-9 human with no pad input)
        wr8(d + 4, u32(i % 8));     // color
        wr8(d + 6, slot);           // player
        wr8(d + 8, 0);              // handicap
        wr8(d + 9, 4);              // CPU level
        // its view entry as the game has a CPU's: the rider, no screen (and rider 1's third byte)
        const u32 v = morepl::at(morepl::k_view_desc, slot);
        wr8(v + 0, slot);
        wr8(v + 1, 0);
        wr8(v + 2, rd8(morepl::at(morepl::k_view_desc, 1) + 2));
    }
    if (s_solo.allCpu) {
        wr8(morepl::plyDesc(0) + 0, 1);
        wr8(morepl::plyDesc(0) + 9, 4);
    }
    guestCall(c, kSetRiderCount, {u32(riders)});
    log("[solo] %d riders entered%s\n", riders, morepl::active() ? " (8-player build)" : "");
}

// The LAN game's screens (f_80020188, at the end of Player Entry): only this console's riders keep theirs. Its
// loops are unrolled for 4 slots - and with 2 consoles, in the 8-player build, write past the views (the
// relocation split that loop) - so in a session of ours it is done here, for every slot: a rider keeps its screen
// when it reads this PC's controller (2 consoles share the 4 pads two each, 3 and 4 one each; past 4 PCs, each
// PC its seat's pad - src/runtime/lan.h). This PC's screens differ from the others' as in the game's own LAN.
constexpr u32 kLanViews = 0x80020188;
void lanViews(Ctx& c, u32 entry) {
    const int pcs = lan::sessionPcs();
    if (pcs <= 0 || !morepl::active()) return interpOriginal(c, entry);
    const int me = lan::sessionMe(), perConsole = pcs == 2 ? 2 : 1;
    for (u32 slot = 0; slot < morepl::count(morepl::k_view_desc); slot++) {
        const u32 v = morepl::at(morepl::k_view_desc, slot) + 1;
        if (!rd8(v)) continue;
        const int pad = rd8(morepl::plyDesc(slot) + 6);
        const bool mine = pcs > 4 ? pad == lan::padOf(me) : pad < 4 && pad / perConsole == me;
        if (!mine) wr8(v, 0);
    }
}

// From the LAN menu's screen 6 (in step on every console): the match starts.
void startMatch(Ctx& c) {
    const Settings& s = s_cur.settings;
    if (!rd8(kLm + 5)) {  // as the first A on this screen: the seed every console agreed, the unlocks merged
        guestCall(c, kAgreeSeed);
        guestCall(c, kMergeUnlocks);
        wr8(kLm + 5, 1);
    }
    applyRules(c, s);
    if (s_testCitySeconds) wr16(kCt, u32(s_testCitySeconds));
    if (s.game == Game::AirRide) {
        guestCall(c, kSetNextMode, {kModeAirRide});
        guestCall(c, kSetAirRideKind, {0});
        guestCall(c, kSetAirRidePad, {4});
    } else {
        guestCall(c, kSetNextMode, {kModeCityTrial});
        guestCall(c, kSetCityKind, {0});
    }
    guestCall(c, kEndScene);
    guestCall(c, kEndMode);
    s_matchNumber++;
    s_matchStarted = false;
    s_matchSerial = s_sceneSerial;
    s_rematchSign = false;
    {
        std::lock_guard g(s_lock);
        s_status.settingsHash = hash(s);
    }
    setPhase(Phase::InMatch);
    log("[lansession] match %d: %s, console %u of %u, %lld ms after begin()\n", s_matchNumber, s.game == Game::AirRide ? "Air Ride" : "City Trial",
        rd8(kNet + 0x4D), rd8(kNet + 0x4C), msSinceBegin());
}

void matchOver(bool completed) {
    MatchResult r{s_cur.settings.game, s_cur.consoles, int(rd8(kNet + 0x4D)), s_matchNumber, completed};
    std::function<void(const MatchResult&)> cb;
    {
        std::lock_guard g(s_lock);
        s_status.matchesPlayed++;
        s_status.matchSerial++;
        cb = s_onMatchOver;
    }
    log("[lansession] match %d over%s\n", s_matchNumber, completed ? "" : " (left early)");
    if (cb) cb(r);
}

void hideLanScreens(Ctx& c) {
    for (u32 f : kHideScreens) guestCall(c, f);
    for (u32 f : kHideTexts) guestCall(c, f, {7});
}

// The game's own cancel: disconnect (screen 12), then back to "How many" (hidden).
void disconnectFromLanMenu(Ctx& c) {
    wr32(kLm + 0x18, 2);
    wr32(kLm + 0x14, 12);
    wr8(kLm + 4, 0);
    guestCall(c, kLbNetDisconnect);
}

// Takes the request (on the game thread): what begin() / abort() asked for.
bool takeRequest(Request& r) {
    std::lock_guard g(s_lock);
    if (!s_req.pending) return false;
    r = s_req;
    s_req.pending = false;
    return true;
}
bool takeAbort(bool& toSelect) {
    std::lock_guard g(s_lock);
    if (!s_abortReq) return false;
    s_abortReq = false;
    toSelect = s_abortToSelect;
    return true;
}
void startConnecting(Ctx& c, const Request& r) {
    s_cur = r;
    lan::setLocalDhcp(true);
    lan::setLocalDhcpPlace(r.index);
    lan::setSession(r.consoles, r.index, r.id);  // (the connection filled in at once, the lockstep for all the PCs)
    const int consoles = std::min(r.consoles, 4);
    wr8(kLm + 0, u32(consoles));
    wr32(kLm + 0x20, u32(consoles - 2));  // ("How many": the cursor, 2/3/4)
    guestCall(c, kLbNetStart);               // ("Is it OK to connect?" Yes)
    wr32(kLm + 0x14, 4);                     // "Connecting..."
    {
        std::lock_guard g(s_lock);
        s_status.consoles = r.consoles;
        s_status.myConsole = -1;
        s_status.matchesPlayed = 0;
    }
    s_matchNumber = 0;
    setPhase(Phase::Connecting);
    log("[lansession] connecting: %d consoles, place %d\n", r.consoles, r.index);
}

// ---- the hooks ----

// The LAN menu's frame (scene 0x28).
void lanMenuFrame(Ctx& c, u32 entry) {
    const u32 screen = rd32(kLm + 0x14);
    if (s_trace && screen != s_lastLanScreen) log("[lansession] LAN menu screen %u -> %u\n", s_lastLanScreen, screen);
    s_lastLanScreen = screen;
    s_phaseSteps++;
    const bool connected = screen >= 6 && screen <= 10;  // (in the lockstep)
    bool toSelect = false;
    if (takeAbort(toSelect)) {
        s_toSelectAfterLeave = toSelect;
        if (screen >= 4 && screen <= 11) {
            disconnectFromLanMenu(c);
            setPhase(Phase::Leaving, "abort()");
        } else {
            setPhase(Phase::Idle, "abort()");
        }
    }
    Request r;
    if (s_phase != Phase::Connecting && s_phase != Phase::Leaving && takeRequest(r)) {
        if (connected && s_phase == Phase::Parked && r.consoles == s_cur.consoles && r.index == s_cur.index) {
            s_cur.settings = r.settings;  // a rematch: shown on this console's pad until everyone shows it
            s_rematchSign = true;
            log("[lansession] rematch asked (settings %08X): signing\n", hash(r.settings));
        } else if (connected) {
            {  // another group: leave this session, then connect anew
                std::lock_guard g(s_lock);
                s_req = r;
                s_req.pending = true;
            }
            s_toSelectAfterLeave = false;
            disconnectFromLanMenu(c);
            setPhase(Phase::Leaving, "another group");
        } else if (screen == 1 || screen == 2 || screen == 3 || screen == 11 || screen == 0) {
            if (screen == 11) {  // (an error showing: cancel it first, then connect)
                std::lock_guard g(s_lock);
                s_req = r;
                s_req.pending = true;
                disconnectFromLanMenu(c);
                setPhase(Phase::Leaving, "clearing an error");
            } else {
                s_beginAt = std::chrono::steady_clock::now();
                startConnecting(c, r);
            }
        } else {
            std::lock_guard g(s_lock);  // (screens 4/5/12: the game's own, from before - later)
            s_req = r;
            s_req.pending = true;
        }
    }
    const u32 now = rd32(kLm + 0x14);
    switch (s_phase) {
    case Phase::Connecting:
        if (now == 6) {
            {
                std::lock_guard g(s_lock);
                s_status.myConsole = rd8(kNet + 0x4D);
            }
            if (rd8(kNet + 0x4D) != s_cur.index)
                log("[lansession] the game's console order differs from the lobby's: place %d is console %u\n", s_cur.index, rd8(kNet + 0x4D));
            startMatch(c);
            return;  // (the LAN menu's own frame does not run: nothing of it shows)
        }
        if (now == 11) {  // an error (the others not found, a timeout): cancel, report
            disconnectFromLanMenu(c);
            fail("Could not connect to the other players.");
        } else if (s_phaseSteps > 60 * 45) {
            disconnectFromLanMenu(c);
            fail("Connecting took too long.");
        }
        break;
    case Phase::InMatch:  // (back in the LAN menu during a match: someone ended it from the pause menu)
        if (connected && s_sceneSerial != s_matchSerial) {
            setPhase(Phase::Parked);
            matchOver(false);
        }
        break;
    case Phase::Parked:
        if (!connected) {
            fail("The connection to the other players was lost.");
            break;
        }
        if (s_rematchSign) {  // every console's pad shows the same sign: everyone starts, in this step
            // (every PC's pad: past 4 PCs, those on spare ports too - each its seat's, src/runtime/lan.h)
            const int n = lan::sessionPcs() > 0 ? lan::sessionPcs() : consoles();
            auto padOfPc = [](int i) { return 4 + 4 * u32(lan::seatOf(i).console) + u32(lan::seatOf(i).port); };
            const u32 sign = heldInGame(4) & (kDpad | kA | kB | kX | kY | kZ | kStart);
            bool all = (sign & kDpad) == kDpad;
            for (int i = 1; i < n && all; i++) all = (heldInGame(padOfPc(i)) & (kDpad | kA | kB | kX | kY | kZ | kStart)) == sign;
            u32 mine = kDpad;
            for (int b = 0; b < 6; b++)
                if (hash(s_cur.settings) >> b & 1) mine |= kSignBits[b];
            if (all && sign == mine) {
                s_beginAt = std::chrono::steady_clock::now();
                startMatch(c);
                return;
            }
            if (all) log("[lansession] rematch: the others sign other settings - waiting\n");
        }
        if (guestCall(c, kLbNetError) != 0) fail("The connection to the other players was lost.");
        return;  // (parked: the LAN menu's frame does not run - nothing shows, the pads do nothing)
    case Phase::Leaving:
        if (now == 2 || now == 1) {
            Request again;
            bool connectAgain = false;
            {
                std::lock_guard g(s_lock);
                if (s_req.pending) again = s_req, s_req.pending = false, connectAgain = true;
            }
            lan::setLocalDhcpPlace(-1);
            if (connectAgain) {
                startConnecting(c, again);
            } else if (s_toSelectAfterLeave) {
                guestCall(c, kSetNextMode, {kModeSelect});
                guestCall(c, kEndScene);
                guestCall(c, kEndMode);
                setPhase(Phase::Idle);
            } else {
                setPhase(Phase::Idle);
            }
        }
        break;
    case Phase::Failed:
        if (now == 11) disconnectFromLanMenu(c);  // (the game's error dialog: cancelled, hidden)
        break;
    default:
        if (now == 11 && s_status.matchesPlayed > 0) disconnectFromLanMenu(c);
        break;
    }
    interpOriginal(c, entry);
    if (s_phase == Phase::Connecting || s_phase == Phase::Leaving || s_phase == Phase::Failed || s_phase == Phase::Waiting ||
        s_hideLanMenu)
        hideLanScreens(c);
}

void testFrame();
// Select Mode's frame: begin() leaves it as if LAN were chosen.
void selectModeFrame(Ctx& c, u32 entry) {
    // The Online menu open over it (src/runtime/online_menu.cpp), no match asked for yet: it stands still - left
    // to run, with no input (the menu takes it) it would go back to the title after a while, and a match then
    // starting would find no Select Mode to leave from.
    if (online::isOpen() && s_phase == Phase::Idle) {
        std::lock_guard g(s_lock);
        if (!s_req.pending) return;
    }
    interpOriginal(c, entry);
    testFrame();
    if (s_solo.on && !s_solo.begun && sceneStep() > 30 && !rd8(kGmSceneOver)) {
        s_solo.begun = true;
        soloStart(c);
        return;
    }
    if (s_phase == Phase::Idle || s_phase == Phase::Failed) {
        bool pending;
        {
            std::lock_guard g(s_lock);
            pending = s_req.pending;
        }
        if (pending && !rd8(kGmSceneOver)) {
            log("[lansession] Select Mode: to the LAN mode\n");
            s_beginAt = std::chrono::steady_clock::now();
            wr32(kGmExitCode, 0x10);  // (its step goes to the LAN mode)
            guestCall(c, kEndScene);
            setPhase(Phase::Waiting);
        }
    }
}

void sceneStart(Ctx& c, u32 entry) {
    s_sceneSerial++;
    if (s_trace) log("[lansession] scene: mode %u scene %u\n", rd8(kGmMode), rd8(kGmMajor));
    if (s_phase == Phase::InMatch && rd8(kGmMajor) == 0x12) s_matchStarted = true;
    interpOriginal(c, entry);
}

// The course select (Air Ride): the course's slot, A.
void courseFrame(Ctx& c, u32 entry) {
    const u32 k = sceneStep();
    if (s_phase == Phase::InMatch && k >= 15 && k % 10 == 5) {
        int slot = s_cur.settings.get(ArCourse);
        if (slot < 9 && rd8(kCourseSel + 0xC + u32(slot)) == 0x0A) slot = 9;  // (locked on every console: Random)
        wr8(kCourseSel + 2, u32(slot));
        pressInGame(4, kA);
    }
    interpOriginal(c, entry);
}

// Player Entry (Air Ride: with the machine choice; City Trial): everyone entered; Start when ready.
void entryFrame(Ctx& c, u32 entry, bool airRide) {
    const u32 k = sceneStep();
    if (s_solo.inMatch) {
        if (k == 20) pressInGame(0, kA);
        if (k >= 40 && k % 15 == 0) pressInGame(0, kStart);
    }
    if (s_phase == Phase::InMatch) {
        // Each PC's player joins as a person would: A on its own controller (in LAN games Player Entry reads the
        // consoles' input as it comes, not the pads as they are afterwards; the lockstep brings it to every PC in
        // the same step), a few times over (the screen takes it once it is ready).
        if ((k == 20 || k == 35 || k == 50) && lan::sessionMe() < 4) pressLocal(kA);  // (PCs past 4: riders 6-9, entered after)
        if (airRide && s_cur.settings.get(ArMachines) == 1 && k >= 50 && k % 30 == 20) pressLocal(kA);  // Quick
        if (k >= 70 && k % 20 == 0 && lan::sessionMe() == 0) pressLocal(kStart);  // (PC 0: taken once everyone is ready)
    }
    interpOriginal(c, entry);
}
void arEntryFrame(Ctx& c, u32 entry) { entryFrame(c, entry, true); }
void ctEntryFrame(Ctx& c, u32 entry) { entryFrame(c, entry, false); }

// City Trial's machine stats ("Press A"): on after 8 s.
void statsFrame(Ctx& c, u32 entry) {
    const u32 k = sceneStep();
    if (s_solo.inMatch && k >= 240 && k % 30 == 0) pressInGame(0, kA);
    if (s_phase == Phase::InMatch && k >= 480 && k % 30 == 0 && lan::sessionMe() < 4) pressLocal(kA);  // (each console, on its controller)
    interpOriginal(c, entry);
}

// The results: Start after a while; when they end, the game goes back to the LAN menu (parked).
void resultsFrame(Ctx& c, u32 entry) {
    const u32 k = sceneStep();
    if (s_solo.inMatch) {
        if (k >= 300 && k % 20 == 0) pressInGame(0, kStart);
        interpOriginal(c, entry);
        if (rd8(kGmSceneOver)) {
            log("[solo] match %d over\n", s_solo.done + 1);
            if (++s_solo.done >= s_solo.matches) {
                log("[solo] done: exiting\n");
                std::exit(0);
            }
        }
        return;
    }
    if (s_phase == Phase::InMatch && k >= u32(s_cur.settings.get(ResultsSeconds)) * 60 && k % 20 == 0) pressLocal(kStart);
    interpOriginal(c, entry);
    if (s_phase == Phase::InMatch && rd8(kGmSceneOver)) {
        guestCall(c, kSetNextMode, {kModeLan});
        guestCall(c, kEndMode);
        setPhase(Phase::Parked);  // (first: the handler may begin() the next match)
        matchOver(true);
    }
}

// SA2_TEST_ABILITIES=1 (tests: the HUD's copy ability pieces in widescreen): player 1's rider is given each copy
// ability in turn, 8 s into a stage and every 5 s after (Rider_GiveAbility(RiderData*, CopyKind) on
// Ply_GetRiderGObj(0)'s data).
void testAbilities(Ctx& c) {
    static const bool s_on = std::getenv("SA2_TEST_ABILITIES") != nullptr;
    if (!s_on) return;
    constexpr u32 kGetRiderGObj = 0x8022CB74, kGiveAbility = 0x801A81A4;
    static u32 s_gobj = 0, s_frames = 0, s_next = 0;
    const u32 gobj = guestCall(c, kGetRiderGObj, {0});
    if (gobj < 0x80000000u || gobj >= 0x81800000u) {  // (MEM1)
        s_gobj = 0;
        return;
    }
    if (gobj != s_gobj) s_gobj = gobj, s_frames = 0;
    const u32 rd = rd32(gobj + 0x2C);
    if (rd < 0x80000000u || rd >= 0x81800000u || rd8(rd + 8) != 0) return;
    if (++s_frames >= 480 && (s_frames - 480) % 300 == 0) {
        const u32 kind = s_next++ % 11;
        log("[test] player 1 given copy ability %u\n", kind);
        guestCall(c, kGiveAbility, {rd, kind});
    }
}

// The pads' read (HSD's, f_80411D40(PADStatus[4])): this console's own presses (port 1), and the rematch sign.
void padRead(Ctx& c, u32 entry) {
    const u32 out = c.r[3];
    interpOriginal(c, entry);
    morepl::frame();
    morepl::debugFrame();
    testAbilities(c);
    u32 add = 0;
    if (s_localCalls) add |= s_localButtons, s_localCalls--;
    if ((s_solo.inMatch || (s_phase == Phase::InMatch && lan::sessionMe() < 4)) && rd8(kGmMajor) == 0x11) {  // (the Stadium's title card waits for A)
        static u32 s_n = 0;
        if (++s_n % 40 < 3) add |= kA;
    }
    if (s_phase == Phase::Parked && s_rematchSign) {
        add |= kDpad;
        for (int b = 0; b < 6; b++)
            if (hash(s_cur.settings) >> b & 1) add |= kSignBits[b];
    }
    // A PC past 4 (on a spare port of a console, src/runtime/lan.h): outside a stage, no controller - in the menus a
    // controller on another port of a console is another player for them to wait for (its rider is entered for it).
    if (out && lan::sessionPcs() > 4 && lan::sessionMe() >= 4 && rd8(kGmMajor) != 0x12) {
        for (u32 o = 0; o < 10; o++) wr8(out + o, 0);
        wr8(out + 0xA, 0xFF);  // (PAD_ERR_NO_CONTROLLER)
    }
    if (add && out) {
        wr16(out, rd16(out) | add);
        wr8(out + 0xA, 0);  // (connected)
    }
}

// The match's other scenes (pause menu "End Game" etc. bring the game back to the LAN menu: handled there).

void install() {
    if (s_installed) return;
    // SA2_TEST_CITYSECS=<n> (tests: online matches through the Online menu): a city shorter than the menus allow.
    if (const char* e = std::getenv("SA2_TEST_CITYSECS")) s_testCitySeconds = std::atoi(e);
    if (const char* e = std::getenv("SA2_LAN_SESSION"); e && std::strcmp(e, "0") == 0) return;
    for (const auto& s : kSignature)
        if (rd32(s.addr) != s.word) return log("[lansession] %08X is %08X, not %08X: not the expected game - no LAN sessions\n", s.addr, rd32(s.addr), s.word);
    s_trace = std::getenv("SA2_LAN_SESSION_TRACE") != nullptr;
    bool ok = interpReplace(0, kLanMenuFrame, lanMenuFrame) && interpReplace(0, kSelectModeFrame, selectModeFrame) &&
              interpReplace(0, kSceneStart, sceneStart) && interpReplace(0, kCourseFrame, courseFrame) &&
              interpReplace(0, kArEntryFrame, arEntryFrame) && interpReplace(0, kCtEntryFrame, ctEntryFrame) &&
              interpReplace(0, kStatsFrame, statsFrame) && interpReplace(0, kArResultsFrame, resultsFrame) &&
              interpReplace(0, kCtResultsFrame, resultsFrame) && interpReplace(0, kPadRead, padRead) &&
              interpReplace(0, kCtSetup, ctSetup);
    if (!ok) return log("[lansession] the game's functions are not where expected - no LAN sessions\n");
    if (rd32(kLanViews) == 0x9421FFF0) interpReplace(0, kLanViews, lanViews);
    s_installed = true;
    log("[lansession] ready\n");
}

// ---- SA2_LAN_SESSION_TEST ----
struct Test {
    int consoles = 0, index = 0, matches = 1, done = 0, citySeconds = 0;
    Settings settings;
    bool begun = false;
} s_test;

Settings parseTestSettings(const char* t, std::function<void(const std::string&, const std::string&)> other);
void soloInit() {
    const char* t = std::getenv("SA2_SOLO_TEST");
    if (!t) return;
    s_solo.on = true;
    s_solo.settings = parseTestSettings(t, [](const std::string& k, const std::string& v) {
        if (k == "riders") s_solo.riders = std::clamp(std::atoi(v.c_str()), 1, 8);
        else if (k == "allcpu") s_solo.allCpu = std::atoi(v.c_str()) != 0;
        else if (k == "matches") s_solo.matches = std::max(1, std::atoi(v.c_str()));
        else if (k == "idle") s_solo.idle = std::atoi(v.c_str()) != 0;
        else if (k == "citysecs") s_testCitySeconds = std::atoi(v.c_str());
    });
    s_solo.settings.game = Game::CityTrial;
    log("[solo] test: %d riders, %d matches, settings %s\n", s_solo.riders, s_solo.matches, toJson(s_solo.settings).c_str());
}
// "k=v;k=v": the settings among them (as in JSON); the others to `other`.
Settings parseTestSettings(const char* t, std::function<void(const std::string&, const std::string&)> other) {
    std::string json = "{";
    for (const char* p = t; *p;) {
        const char* e = std::strchr(p, ';');
        const std::string kv(p, e ? size_t(e - p) : std::strlen(p));
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == "riders" || k == "matches" || k == "idle" || k == "citysecs" || k == "consoles" || k == "index") other(k, v);
            else {
                if (json.size() > 1) json += ",";
                const bool num = !v.empty() && (std::isdigit(u8(v[0])) || v[0] == '-');
                json += "\"" + k + "\":" + (num ? v : "\"" + v + "\"");
            }
        }
        if (!e) break;
        p = e + 1;
    }
    json += "}";
    Settings out;
    std::string err;
    if (!fromJson(json, out, &err)) log("[lansession] test settings %s: %s\n", json.c_str(), err.c_str());
    return out;
}
void testInit() {
    static bool s_done = false;
    soloInit();
    const char* t = std::getenv("SA2_LAN_SESSION_TEST");
    if (!t || s_done) return;
    s_done = true;
    std::string json = "{";
    for (const char* p = t; *p;) {
        const char* e = std::strchr(p, ';');
        const std::string kv(p, e ? size_t(e - p) : std::strlen(p));
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == "consoles") s_test.consoles = std::atoi(v.c_str());
            else if (k == "index") s_test.index = std::atoi(v.c_str());
            else if (k == "matches") s_test.matches = std::max(1, std::atoi(v.c_str()));
            else if (k == "citysecs") s_testCitySeconds = std::atoi(v.c_str());
            else {
                if (json.size() > 1) json += ",";
                const bool num = !v.empty() && (std::isdigit(u8(v[0])) || v[0] == '-');
                json += "\"" + k + "\":" + (num ? v : "\"" + v + "\"");
            }
        }
        if (!e) break;
        p = e + 1;
    }
    json += "}";
    std::string err;
    if (!fromJson(json, s_test.settings, &err)) log("[lansession] test settings %s: %s\n", json.c_str(), err.c_str());
    log("[lansession] test: %d consoles, place %d, %d matches, settings %s\n", s_test.consoles, s_test.index, s_test.matches,
        toJson(s_test.settings).c_str());
    setMatchOverHandler([](const MatchResult& r) {
        log("[lansession] test: match %d over (%s, console %d of %d)\n", r.matchNumber, r.completed ? "completed" : "left", r.myConsole, r.consoles);
        if (++s_test.done < s_test.matches) begin(s_test.consoles, s_test.index, s_test.settings);
    });
}
void testFrame() {  // (Select Mode reached: begin())
    // (a second on Select Mode first: a test's menu script - SA2_MENU_SEQ "START~mode select" - sees it and stops)
    if (s_test.consoles && !s_test.begun && rd8(kGmMode) == kModeSelect && rd8(kGmMajor) == kSceneSelectMode && sceneStep() > 60) {
        s_test.begun = true;
        begin(s_test.consoles, s_test.index, s_test.settings);
    }
}
}  // namespace

// ---------------------------------------------------------------------------------------------------

Status status() {
    std::lock_guard g(s_lock);
    return s_status;
}

bool begin(int n, int index, const Settings& settings, uint32_t id) {
    if (n < 2 || n > (morepl::active() ? 8 : 4) || index < 0 || index >= n) return false;
    if (n > 4 && settings.game != Game::CityTrial) return false;  // (Air Ride: 4 riders still)
    std::lock_guard g(s_lock);
    if (s_status.phase == Phase::Connecting || s_status.phase == Phase::InMatch) return false;
    s_req.pending = true;
    s_req.consoles = n;
    s_req.index = index;
    s_req.settings = settings;
    s_req.id = id ? id : hash(settings) ^ u32(n) * 0x9E3779B9u;
    s_abortReq = false;
    if (s_status.phase == Phase::Idle || s_status.phase == Phase::Failed) {
        s_status.phase = Phase::Waiting;
        s_status.cover = true;
        s_status.error.clear();
    }
    return true;
}

void abort(bool toSelectMode) {
    std::lock_guard g(s_lock);
    s_req.pending = false;
    s_abortReq = true;
    s_abortToSelect = toSelectMode;
}

void setMatchOverHandler(std::function<void(const MatchResult&)> handler) {
    std::lock_guard g(s_lock);
    s_onMatchOver = std::move(handler);
}

void setHideLanMenu(bool hide) {
    std::lock_guard g(s_lock);
    s_hideLanMenu = hide;
}

}  // namespace rt::lansession

namespace rt {
// The memory card library starts at boot, once the program is loaded: the hooks go in then.
HLE(CARDInit) {
    callOriginal(c, "CARDInit");
    lansession::install();
    lansession::testInit();
    morepl::install();
}
}  // namespace rt

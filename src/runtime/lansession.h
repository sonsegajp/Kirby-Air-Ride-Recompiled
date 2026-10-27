#pragma once
// A LAN session of Kirby Air Ride started by host code, with none of the game's LAN screens shown
// (src/runtime/lansession.cpp). The Online menu (src/runtime/online_menu.cpp) calls begin() when its match
// group starts a match: the game goes from wherever it waits (Select Mode, its Online mode, or the end of
// the last match) straight into the match - the game's own LAN connection and handshake run hidden
// underneath, the mode and rules are set from Settings, the course is picked and every console's player is
// entered - and after the results it parks, hidden, still connected, until the next begin() (a rematch
// starts in the same step on every console) or abort().
//
// What the player sees: the game's Player Entry screen for a moment (everyone already entered; City Trial
// starts by itself), in Air Ride the machine choice (Settings: machine choice "Players choose"; "Quick" takes
// the highlighted machine at once), then the match; the results until someone presses Start (or 12 s); then
// whatever the Online menu draws over the parked game (status().cover: the game shows nothing of its own).
//
// Thread safety: every function may be called from any thread. The match-over handler runs on the game's
// thread (keep it short; copy what it gets).
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace rt::lansession {

// ---- the settings catalogue (what the game's own LAN rules menus offer) ----
enum class Game : uint8_t { AirRide = 0, CityTrial = 1 };

enum Setting : int {
    // Air Ride
    ArCourse,       // choice: the 9 courses or Random (a course no console has unlocked: Random)
    ArRule,         // choice: Laps / Time
    ArLaps,         // 0-99 (0: the course's own count)
    ArMinutes,      // 1-99 (with Rule = Time)
    ArDamage,       // choice: None / On
    ArSpeedHelp,    // choice: None / Weak / Strong (catch-up help)
    ArEnemies,      // choice: On / Off
    ArTempo,        // choice: Normal / Slow
    ArMachines,     // choice: Players choose / Quick (session option: Quick takes each player's highlighted machine)
    // City Trial
    CtMinutes,      // 3-7 (the city's time before the Stadium)
    CtTempo,        // choice: Normal / Slow
    CtStadium,      // choice: Shuffle / Drag Race / Air Glider / Target Flight / High Jump / Kirby Melee /
                    //         Destruction Derby / Single Race / VS. King Dedede
    CtEvents,       // choice: On / Off
    // Both
    ResultsSeconds, // 3-60: the results stay this long unless someone presses Start (session option)
    kSettingCount
};

struct SettingInfo {
    const char* key;               // stable JSON key ("ar.course")
    const char* name;              // display name, as the game's menu calls it
    const char* help;              // one line for the UI
    Game game;                     // which game it belongs to (ResultsSeconds: listed under both, game ignored)
    bool both;                     // applies to both games
    int min, max, def;             // value range (choices: 0..count-1)
    const char* const* choiceIds;  // choices: stable ids for JSON ("fantasy_meadows"); nullptr: a number
    const char* const* choiceNames;  // choices: display names
    const char* unit;              // numbers: "min", "laps", "s"
    const char* zeroName;          // numbers: how 0 shows (nullptr: as 0) - Laps 0 is "Course default"
};
const SettingInfo& info(Setting s);
inline constexpr int settingCount() { return kSettingCount; }

// City Trial's stage: a map mod played in the city's place (src/runtime/maps.h), the same on every console - named by
// its folder (as the host has it) and found by its content's hash (src/runtime/mapfiles.h). Folder "": the city.
struct Map {
    std::string folder, title, hash;
    uint64_t size = 0;  // its files' bytes
    bool operator==(const Map&) const = default;
};

struct Settings {
    Game game = Game::CityTrial;
    int value[kSettingCount];
    Map map;                                // (City Trial)
    Settings();                             // every setting at its default
    int get(Setting s) const { return value[s]; }
    void set(Setting s, int v);             // clamped to the setting's range
    bool operator==(const Settings& o) const;
};
// JSON: {"game":"citytrial","ct.minutes":5,"ct.stadium":"shuffle",...} - every key is optional (missing:
// default); choices by id (or index); unknown keys are ignored. The master server keeps it as an opaque object.
// The map: "map":{"folder":...,"title":...,"hash":"<SHA-256, hex>","size":<bytes>} (missing, or folder "": the city).
std::string toJson(const Settings& s);
bool fromJson(std::string_view json, Settings& out, std::string* error = nullptr);
uint32_t hash(const Settings& s);  // the same on every console for the same settings

// ---- the session ----
enum class Phase : uint8_t {
    Idle,        // no session
    Waiting,     // begin() called: waiting for the game to reach a point a session can start from
    Connecting,  // the game's LAN connection and handshake (hidden)
    InMatch,     // the match (from the Player Entry screen to the results)
    Parked,      // a match ended: still connected, hidden, waiting for the next begin() (a rematch)
    Leaving,     // abort(), or the group changed: disconnecting (hidden)
    Failed,      // gave up (error says why); the game is back where a new begin() can start
};
const char* phaseName(Phase p);

struct MatchResult {
    Game game;
    int consoles;
    int myConsole;
    int matchNumber;       // 1, 2, ... in this session
    bool completed;        // false: the match was left early (a player quit from the pause menu)
};

struct Status {
    Phase phase = Phase::Idle;
    int consoles = 0;        // of the session in progress
    int myConsole = -1;      // the game's place for this console (0: the master) once connected
    int matchesPlayed = 0;   // in this session
    uint32_t matchSerial = 0;  // grows by one at each match over (a UI polls this instead of the handler)
    uint32_t settingsHash = 0;
    bool cover = false;      // the game is showing nothing of its own now (connecting, parked, leaving):
                             // the Online menu should draw its own screen ("Starting match...")
    std::string error;       // Failed: a sentence for the player
};
Status status();

// Starts a session: `consoles` (2-4) in the match group, this console's place in it (`myIndex`, 0..consoles-1, the
// order the master server gave - the game's console order follows it), the settings (the same on every console).
// From Select Mode, the game's Online mode (any of its screens), or Parked (same consoles: a rematch at once; else
// it leaves the old session and connects anew). Returns false (nothing changes) if the arguments are invalid or a
// session is connecting/in a match.
// consoles: the PCs, 2-4 (2-8 in the 8-player build: the game sees 4 consoles, PCs 4-7 on their spare ports -
// src/runtime/lan.h); id: the same on all of them, different for each session (the match's seed).
bool begin(int consoles, int myIndex, const Settings& settings, uint32_t id = 0);
// Leaves (connecting: cancels; in a match or parked: disconnects - the others' sessions fail). The game ends in
// its Online mode, hidden while the Online menu covers it; `toSelectMode`: back to Select Mode instead.
void abort(bool toSelectMode = false);
// Called at each match over (on the game's thread).
void setMatchOverHandler(std::function<void(const MatchResult&)> handler);
// The game's own LAN screens hidden even outside a session (when the Online menu covers them).
void setHideLanMenu(bool hide);

}  // namespace rt::lansession

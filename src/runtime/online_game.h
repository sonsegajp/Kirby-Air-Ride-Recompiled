#pragma once
// The game's side of an online session, as the Online menu (src/runtime/online_menu.cpp) needs it - a
// thin adapter, so the menu does not depend on how it is done:
//   - where the game is: Select Mode, choosing ONLINE there, its LAN screens (the menu covers them);
//   - starting a match of the server's session with the lobby's settings, its end, abandoning it:
//     src/runtime/lansession.h;
//   - the game settings' catalogue (the mode, and each mode's rules: names, choices, defaults) and their
//     JSON, as the master server carries them (airnet's settingsJson): lansession's.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace rt::online::game {

// ---- where the game is ----
enum class Where : uint8_t {
    Other,           // anywhere else (a match, other menus)
    SelectMode,      // the mode select
    ChoosingOnline,  // ... ONLINE chosen: the LAN screens come next frame
    LanSystems,      // its LAN mode's first screen ("How many Nintendo GameCube systems...?")
    LanOther,        // its LAN mode, past that screen
};
Where where();
const char* whereName(Where w);

// ---- a match ----
struct Start {
    int consoles = 0;   // 2..4 (2..8 in the 8-player build)
    int me = -1;        // this console's number, 0..consoles-1
    uint32_t seed = 0;
    int64_t startAtMs = 0;  // (airnet's clock)
    std::string settingsJson;
};
bool sessionAvailable();  // the game side can start matches (lansession is in)
bool begin(const Start& start);  // false: it could not
bool running();
bool takeResults(std::string& json);  // the match is over: what this console's game reports (once)
void abort();
bool covered();  // the game shows nothing of its own now (connecting, parked between matches): cover it
void leave();    // out of Online: the game back to Select Mode

// ---- the game settings ----
struct Rule {
    std::string id, name, help;
    std::vector<std::string> ids, names;  // the choices: as stored, as shown
    int def = 0;
};
inline constexpr int kAirRide = 0, kCityTrial = 1;
const char* modeId(int mode);    // "air_ride", "city_trial"
const char* modeName(int mode);  // "Air Ride", "City Trial"
const std::vector<Rule>& rules(int mode);

// City Trial's stage: a map mod in the city's place (src/runtime/mapfiles.h), named by its folder, found by its hash.
struct Stage {
    std::string folder, title, hash;  // folder "": the city
    uint64_t size = 0;
};

struct Settings {
    int mode = kCityTrial;
    std::map<std::string, std::string> values;  // rule id -> choice id (missing: the rule's default)
    Stage stage;                                // (City Trial)
    int index(const Rule& r) const;             // the choice in use
    void set(const Rule& r, int choice);
    std::string toJson() const;
    static Settings fromJson(const std::string& json);
    static Settings defaults(int mode);
};

// ---- JSON, as small as the menu needs (settings, results) ----
struct Json {
    enum Type : uint8_t { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> items;                             // Array
    std::vector<std::pair<std::string, Json>> members;   // Object, in order
    const Json* get(const std::string& key) const;
    std::string text() const;  // a value as words ("3", "true", "Fantasy Meadows")
    static Json parse(const std::string& in);
};

}  // namespace rt::online::game

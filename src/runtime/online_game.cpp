// The game's side of an online session for the Online menu (src/runtime/online_game.h).
#include "runtime/online_game.h"
#include "runtime/lansession.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rt::online::game {

// ---------------------------------------------------------------------------------------------------
// Where the game is: its scene manager's state (found for src/runtime/lansession.cpp): the current mode
// (3: Select Mode, 19: the LAN mode), the exit code Select Mode leaves for the next step (0x10: ONLINE
// chosen), and the LAN menu's own state (2: "How many systems?").

namespace {
constexpr u32 kGm = 0x805359D8;
constexpr u32 kGmMode = kGm + 0x7D4, kGmExitCode = kGm + 0x81C;
constexpr u32 kLanMenuState = 0x80537430 + 0x14;
constexpr u32 kModeSelect = 3, kModeLan = 19;
}  // namespace

Where where() {
    const u32 mode = rd8(kGmMode);
    if (mode == kModeSelect) return rd32(kGmExitCode) == 0x10 ? Where::ChoosingOnline : Where::SelectMode;
    if (mode == kModeLan) {
        const u32 st = rd32(kLanMenuState);
        return st == 2 || st == 3 ? Where::LanSystems : Where::LanOther;
    }
    return Where::Other;
}

const char* whereName(Where w) {
    switch (w) {
    case Where::SelectMode: return "Select Mode";
    case Where::ChoosingOnline: return "choosing ONLINE";
    case Where::LanSystems: return "LAN: how many systems";
    case Where::LanOther: return "LAN";
    default: return "elsewhere";
    }
}

// ---------------------------------------------------------------------------------------------------
// A match: src/runtime/lansession.h takes the game straight into it.

namespace {
uint32_t g_overSeen = 0;
bool g_began = false;
lansession::Settings toSession(const Settings& st);
}  // namespace

bool sessionAvailable() { return true; }

bool begin(const Start& st) {
    lansession::setHideLanMenu(true);
    const lansession::Settings ls = toSession(Settings::fromJson(st.settingsJson));
    g_overSeen = lansession::status().matchSerial;
    g_began = lansession::begin(st.consoles, st.me, ls, st.seed ^ u32(st.consoles) * 0x9E3779B9u);
    log("[onlinegame] match: %d consoles, this one %d: %s\n", st.consoles, st.me, g_began ? "the game starts it" : "the game could not start it");
    return g_began;
}

bool running() {
    const lansession::Phase p = lansession::status().phase;
    return g_began && (p == lansession::Phase::Waiting || p == lansession::Phase::Connecting || p == lansession::Phase::InMatch);
}

bool takeResults(std::string& json) {
    if (!g_began) return false;
    const lansession::Status st = lansession::status();
    if (st.matchSerial != g_overSeen) {
        g_overSeen = st.matchSerial;
        g_began = false;
        json = "{\"completed\":true,\"console\":" + std::to_string(st.myConsole) + ",\"match\":" + std::to_string(st.matchesPlayed) + "}";
        return true;
    }
    if (st.phase == lansession::Phase::Failed) {
        g_began = false;
        json = "{\"completed\":false}";
        log("[onlinegame] the match failed: %s\n", st.error.c_str());
        return true;
    }
    return false;
}

void abort() {
    if (g_began) lansession::abort();
    g_began = false;
}

bool covered() { return lansession::status().cover; }
void leave() { lansession::abort(true); }

// ---------------------------------------------------------------------------------------------------
// The settings' catalogue: lansession's

const char* modeId(int mode) { return mode == kAirRide ? "air_ride" : "city_trial"; }
const char* modeName(int mode) { return mode == kAirRide ? "Air Ride" : "City Trial"; }

namespace {
bool inMode(const lansession::SettingInfo& in, int mode) {
    return in.both || int(in.game) == mode;
}
}  // namespace

const std::vector<Rule>& rules(int mode) {
    static std::vector<Rule> s_rules[2];
    std::vector<Rule>& v = s_rules[mode == kAirRide ? 0 : 1];
    if (!v.empty()) return v;
    for (int i = 0; i < lansession::settingCount(); i++) {
        const lansession::SettingInfo& in = lansession::info(lansession::Setting(i));
        if (!inMode(in, mode)) continue;
        Rule r{in.key, in.name, in.help, {}, {}, 0};
        for (int x = in.min; x <= in.max; x++) {
            r.ids.push_back(in.choiceIds ? in.choiceIds[x] : std::to_string(x));
            if (in.choiceNames) r.names.push_back(in.choiceNames[x]);
            else if (x == 0 && in.zeroName) r.names.push_back(in.zeroName);
            else r.names.push_back(std::to_string(x) + (in.unit && *in.unit ? std::string(" ") + in.unit : std::string()));
            if (x == in.def) r.def = int(r.ids.size()) - 1;
        }
        v.push_back(std::move(r));
    }
    return v;
}

int Settings::index(const Rule& r) const {
    const auto it = values.find(r.id);
    if (it != values.end())
        for (size_t i = 0; i < r.ids.size(); i++)
            if (r.ids[i] == it->second) return int(i);
    return r.def;
}

void Settings::set(const Rule& r, int choice) {
    const int n = int(r.ids.size());
    if (n) values[r.id] = r.ids[size_t(((choice % n) + n) % n)];
}

Settings Settings::defaults(int mode) {
    Settings st;
    st.mode = mode;
    for (const Rule& r : rules(mode)) st.values[r.id] = r.ids[size_t(r.def)];
    return st;
}

namespace {
lansession::Settings toSession(const Settings& st) {
    lansession::Settings ls;
    ls.game = st.mode == kAirRide ? lansession::Game::AirRide : lansession::Game::CityTrial;
    for (int i = 0; i < lansession::settingCount(); i++) {
        const lansession::SettingInfo& in = lansession::info(lansession::Setting(i));
        if (!inMode(in, st.mode)) continue;
        for (const Rule& r : rules(st.mode))
            if (r.id == in.key) ls.set(lansession::Setting(i), in.min + st.index(r));
    }
    return ls;
}
}  // namespace

std::string Settings::toJson() const { return lansession::toJson(toSession(*this)); }

Settings Settings::fromJson(const std::string& json) {
    lansession::Settings ls;
    lansession::fromJson(json, ls);
    Settings st;
    st.mode = ls.game == lansession::Game::AirRide ? kAirRide : kCityTrial;
    for (int i = 0; i < lansession::settingCount(); i++) {
        const lansession::SettingInfo& in = lansession::info(lansession::Setting(i));
        if (!inMode(in, st.mode)) continue;
        for (const Rule& r : rules(st.mode))
            if (r.id == in.key) {
                const int k = ls.get(lansession::Setting(i)) - in.min;
                if (k >= 0 && k < int(r.ids.size())) st.values[r.id] = r.ids[size_t(k)];
            }
    }
    return st;
}
// ---------------------------------------------------------------------------------------------------
// JSON

const Json* Json::get(const std::string& key) const {
    for (const auto& [k, v] : members)
        if (k == key) return &v;
    return nullptr;
}

std::string Json::text() const {
    switch (type) {
    case Bool: return b ? "true" : "false";
    case Number: {
        char buf[32];
        if (std::fabs(n - std::round(n)) < 1e-9) std::snprintf(buf, sizeof buf, "%lld", (long long)std::llround(n));
        else std::snprintf(buf, sizeof buf, "%.2f", n);
        return buf;
    }
    case String: return s;
    case Array: return "[" + std::to_string(items.size()) + "]";
    case Object: return "{" + std::to_string(members.size()) + "}";
    default: return "";
    }
}

namespace {
struct Parser {
    const char* p;
    const char* e;
    int depth = 0;
    void ws() {
        while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    }
    bool str(std::string& out) {
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                p++;
                const char c = *p++;
                if (c == 'n') out += '\n';
                else if (c == 't') out += '\t';
                else if (c == 'u' && p + 4 <= e) {
                    const unsigned cp = unsigned(std::strtoul(std::string(p, 4).c_str(), nullptr, 16));
                    p += 4;
                    if (cp < 0x80) out += char(cp);
                    else if (cp < 0x800) out += char(0xC0 | cp >> 6), out += char(0x80 | (cp & 63));
                    else out += char(0xE0 | cp >> 12), out += char(0x80 | (cp >> 6 & 63)), out += char(0x80 | (cp & 63));
                } else out += c;
            } else out += *p++;
        }
        if (p >= e) return false;
        p++;
        return true;
    }
    bool value(Json& v) {
        ws();
        if (p >= e || ++depth > 16) return false;
        bool ok = true;
        if (*p == '{') {
            v.type = Json::Object;
            p++;
            ws();
            if (p < e && *p == '}') p++;
            else
                for (;;) {
                    ws();
                    std::string k;
                    Json x;
                    if (!str(k)) return false;
                    ws();
                    if (p >= e || *p++ != ':' || !value(x)) return false;
                    v.members.emplace_back(std::move(k), std::move(x));
                    ws();
                    if (p < e && *p == ',') {
                        p++;
                        continue;
                    }
                    if (p < e && *p == '}') {
                        p++;
                        break;
                    }
                    return false;
                }
        } else if (*p == '[') {
            v.type = Json::Array;
            p++;
            ws();
            if (p < e && *p == ']') p++;
            else
                for (;;) {
                    Json x;
                    if (!value(x)) return false;
                    v.items.push_back(std::move(x));
                    ws();
                    if (p < e && *p == ',') {
                        p++;
                        continue;
                    }
                    if (p < e && *p == ']') {
                        p++;
                        break;
                    }
                    return false;
                }
        } else if (*p == '"') {
            v.type = Json::String;
            ok = str(v.s);
        } else if (e - p >= 4 && !std::strncmp(p, "true", 4)) {
            v.type = Json::Bool, v.b = true, p += 4;
        } else if (e - p >= 5 && !std::strncmp(p, "false", 5)) {
            v.type = Json::Bool, p += 5;
        } else if (e - p >= 4 && !std::strncmp(p, "null", 4)) {
            p += 4;
        } else {
            char* end = nullptr;
            const std::string rest(p, size_t(std::min<ptrdiff_t>(e - p, 64)));
            v.n = std::strtod(rest.c_str(), &end);
            if (end == rest.c_str()) return false;
            v.type = Json::Number;
            p += end - rest.c_str();
        }
        depth--;
        return ok;
    }
};
}  // namespace

Json Json::parse(const std::string& in) {
    Json v;
    Parser ps{in.data(), in.data() + in.size()};
    if (!ps.value(v)) return Json{};
    return v;
}

}  // namespace rt::online::game

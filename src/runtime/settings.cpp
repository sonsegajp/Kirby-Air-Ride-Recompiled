// The player's settings: kirbyairride-settings.ini beside the program, one "name = value" a line.
#include "runtime/settings.h"
#include "runtime/runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace rt::settings {

std::atomic<bool> interpolation{true};
std::atomic<int> frameLimit{0};
std::atomic<bool> showFps{false};
std::atomic<bool> fullscreen{false};
std::atomic<bool> widescreen{true};
std::atomic<float> masterVolume{1.f};
std::atomic<bool> muteUnfocused{false};
std::atomic<bool> menuHint{true};
#ifndef KAR_ONLINE_MASTER
#define KAR_ONLINE_MASTER ""
#endif
Text onlineMaster{KAR_ONLINE_MASTER};  // the official service: set when building (CMake's KAR_ONLINE_MASTER), never in the source
Text onlineServer{""};
Text onlineUser{""};
Text onlineToken{""};
Text onlineTokenServer{""};
std::atomic<bool> onlineRelayTcp{false};
std::atomic<bool> onlineP2p{true};
Text onlineStun{"stun.l.google.com:19302,stun.cloudflare.com:3478"};

namespace {
constexpr const char* kFile = "kirbyairride-settings.ini";

struct Entry {
    const char* name;
    std::atomic<bool>* b;
    std::atomic<int>* i;
    std::atomic<float>* f;
    Text* t = nullptr;
};
const Entry kEntries[] = {
    {"interpolation", &interpolation, nullptr, nullptr},
    {"frame_limit", nullptr, &frameLimit, nullptr},
    {"show_fps", &showFps, nullptr, nullptr},
    {"fullscreen", &fullscreen, nullptr, nullptr},
    {"widescreen", &widescreen, nullptr, nullptr},
    {"master_volume", nullptr, nullptr, &masterVolume},
    {"mute_unfocused", &muteUnfocused, nullptr, nullptr},
    {"menu_hint", &menuHint, nullptr, nullptr},
    {"online_master", nullptr, nullptr, nullptr, &onlineMaster},
    {"online_user", nullptr, nullptr, nullptr, &onlineUser},
    {"online_server", nullptr, nullptr, nullptr, &onlineServer},
    {"online_token", nullptr, nullptr, nullptr, &onlineToken},
    {"online_token_server", nullptr, nullptr, nullptr, &onlineTokenServer},
    {"online_relay_tcp", &onlineRelayTcp, nullptr, nullptr},
    {"online_p2p", &onlineP2p, nullptr, nullptr},
    {"online_stun", nullptr, nullptr, nullptr, &onlineStun},
};
}  // namespace

void load() {
    std::FILE* f = std::fopen(kFile, "r");
    if (!f) return;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        char name[64];
        char value[64];
        if (std::sscanf(line, " %63[a-z_] = %63s", name, value) != 2) continue;
        for (const Entry& e : kEntries) {
            if (std::strcmp(e.name, name)) continue;
            if (e.b) *e.b = std::atoi(value) != 0;
            if (e.i) *e.i = std::atoi(value);
            if (e.f) *e.f = float(std::atof(value));
            if (e.t) e.t->set(value);
        }
    }
    std::fclose(f);
    log("[settings] loaded %s\n", kFile);
}

void save() {
    std::FILE* f = std::fopen(kFile, "w");
    if (!f) return;
    std::fprintf(f, "# Kirby Air Ride: Recompiled - settings (F1 in the game changes them)\n");
    for (const Entry& e : kEntries) {
        if (e.b) std::fprintf(f, "%s = %d\n", e.name, e.b->load() ? 1 : 0);
        if (e.i) std::fprintf(f, "%s = %d\n", e.name, e.i->load());
        if (e.f) std::fprintf(f, "%s = %.3f\n", e.name, double(e.f->load()));
        if (e.t) std::fprintf(f, "%s = %s\n", e.name, e.t->get().c_str());
    }
    std::fclose(f);
}

}  // namespace rt::settings

// City Trial maps: a map made with the map editor (tools/mapeditor) is played in the city's place - its stage files
// are served for the city's (src/runtime/hle_dvd.cpp, dvdOverlay). What the game does for the city alone, by its own
// code rather than from the stage files, is left out on such a map.
#include "runtime/maps.h"
#include "runtime/interp.h"
#include "runtime/morepl.h"
#include "runtime/runtime.h"
#include "runtime/settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace rt {

std::atomic<bool> g_customCity{false};  // a custom map is in the city's place (hle_dvd.cpp sets it)
u32 dvdOverlayLoadedAt(const std::string& path, u32* size);  // (hle_dvd.cpp)
void dvdServeDir(const std::string& dir, std::vector<std::string>& served, const std::vector<std::string>& skip);
std::vector<u8> dvdDiscFile(const std::string& path);
void dvdWatch(const std::string& path);
u32 dvdLoadedAt(const std::string& path);
u32 dvdEntryNum(const std::string& path);
void dvdUnserve(const std::vector<std::string>& paths);

namespace {
// The city's gimmicks (the wheel, its doors, the gates... 31 kinds), made one by one by the city's own setup: each takes
// its joints in the city's model and its part of the city's collision, by number - none of which a custom map has.
constexpr u32 kCityGimmicks = 0x8010F268;
// The stage's marker joints ("null positions": grnullpos.c), matched by name to joints of the city's model as the stage
// loads; it answers the first one missing (-1: none), and a missing one stops the game. A custom map's model has none.
constexpr u32 kNullPosMatch = 0x800E3CB8;
// The city's setup for the mode played (City Trial, the Stadiums...): shows the city's parts that mode uses - joints of the
// city's model, by numbers in the stage's parameters, each with its collision - after animating the city's parts
// (800DBE48) and before its end (800DC1B4). A custom map: the end alone.
constexpr u32 kCityModeSetup = 0x8010F114;

// ---- areas and warps (the map's mapinfo.txt, written by the map builder: tools/mapeditor/KarMapTool/MapExtras.cs) ----
// A rider is checked against them each frame, as City Trial's minimap moves its mark (8011DCF8: per rider, r3 its
// mark's object, +2C its data, +20 the rider's slot). A warp puts the machine where it leads by the game's own respawn
// (801E6718(machine): asks for a spot, then puts the machine on it - 801CB50C(machine, pos, forward, up) - stopped and
// set right), the spot replaced by the warp's.
constexpr u32 kMapMark = 0x8011DCF8;
constexpr u32 kRespawn = 0x801E6718;
constexpr u32 kPutMachine = 0x801CB50C;

struct Box {
    std::string name;
    float c[3], h[3], yaw;
    bool contains(const float p[3]) const {
        const float r = yaw * 3.14159265f / 180.f, cs = std::cos(r), sn = std::sin(r);
        const float d[3] = {p[0] - c[0], p[1] - c[1], p[2] - c[2]};
        const float lx = d[0] * cs - d[2] * sn, lz = d[0] * sn + d[2] * cs;  // (along its right, and its back)
        return std::fabs(lx) <= h[0] && std::fabs(d[1]) <= h[1] && std::fabs(lz) <= h[2];
    }
};
struct AreaInfo : Box {
    std::string track;  // its music: the game's track (audio/jp/<track>.hps) the map's music is in; "-": none
};
struct WarpInfo : Box {
    float to[3], toYaw;
};
std::vector<AreaInfo> s_areas;
std::vector<WarpInfo> s_warps;
u32 s_warpReady[16];     // per slot: the frame a rider can warp again (not straight back)
u32 s_frame[16];         // per slot: its frames (its mark's updates)
float s_prev[16][3];     // per slot: where it was the frame before (a warp is ridden through, not only stood in)
bool s_hasPrev[16];
bool s_inWarp[16];       // per slot: still in a warp's box since its last warp (it warps again only once out of them all)
struct Pending {
    u32 machine = 0;
    float pos[3], fwd[3];
} s_pending;             // the warp under way: the respawn's spot replaced by it
int s_inArea = -1;       // the area the first rider is in (-1: none): its music plays

void loadMapInfo(const std::string& dir) {
    s_areas.clear(), s_warps.clear();
    std::FILE* f = std::fopen((dir + "/mapinfo.txt").c_str(), "r");
    if (!f) return;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        char kind[16], name[128], track[64];
        float v[11];
        if (line[0] == '#') continue;
        if (std::sscanf(line, "%15s", kind) != 1) continue;
        if (!std::strcmp(kind, "area") &&
            std::sscanf(line, "%*s %127s %f %f %f %f %f %f %f %63s", name, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], track) == 9) {
            AreaInfo a;
            a.name = name, a.track = track;
            for (int i = 0; i < 3; i++) a.c[i] = v[i], a.h[i] = v[3 + i];
            a.yaw = v[6];
            s_areas.push_back(a);
        } else if (!std::strcmp(kind, "warp") &&
                   std::sscanf(line, "%*s %127s %f %f %f %f %f %f %f %f %f %f %f", name, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                               &v[8], &v[9], &v[10]) == 12) {
            WarpInfo w;
            w.name = name;
            for (int i = 0; i < 3; i++) w.c[i] = v[i], w.h[i] = v[3 + i], w.to[i] = v[7 + i];
            w.yaw = v[6], w.toYaw = v[10];
            s_warps.push_back(w);
        }
    }
    std::fclose(f);
    log("[maps] map info: %zu areas, %zu warps\n", s_areas.size(), s_warps.size());
}

// ---- an area's music: played as City Trial's events play theirs (grEventGenerator: announce 800EE778, start
// 800EE328, end 800EE660/800EE50C) - the map's music (stream 1) faded out and paused, the area's on stream 2, then
// stream 1 resumed and faded back in. The area's track is one City Trial never plays (Top Ride's, 2d_*: ids 3-0x11),
// its file the map's (audio/jp/<track>.hps). Left alone while a City Trial event has the music, or Dragoon's.
constexpr u32 kMusicFadeOut = 0x80061DF0;  // (frames): stream 1 to silence
constexpr u32 kMusicEventPlay = 0x80061E7C;  // (track): on stream 2, stream 1 paused
constexpr u32 kMusicEventEnd = 0x80062004;  // (frames): stream 1 back, stream 2 faded out
constexpr u32 kMusicEventStop = 0x800620E8;  // (): stream 2 stopped, its volume set back
constexpr u32 kAudioFlags = 0x805380CA, kEventState = 0x805DD6F8;
constexpr int kFade = 40;

int trackId(const std::string& name) {
    static const char* const k2d[] = {"2d_desert", "2d_desert_ura", "2d_kazan", "2d_kazan_ura", "2d_kekka", "2d_light", "2d_light_ura",
                                      "2d_machine", "2d_machine_ura", "2d_plants", "2d_plants_ura", "2d_sky", "2d_sky_ura", "2d_water", "2d_water_ura"};
    for (int i = 0; i < 15; i++)
        if (name == k2d[i]) return 3 + i;
    return -1;
}
bool eventHasMusic() {
    if (rd8(kAudioFlags) & 0x08) return true;  // (Dragoon's music: the game's own lock)
    const u32 s = rd32(kEventState);
    if ((s & 0xFE000000u) != 0x80000000u) return false;
    const u32 e = rd32(s + 0x2C);
    return (e & 0xFE000000u) == 0x80000000u && rd32(e + 4) != 0;
}
void gcall(Ctx& c, u32 fn, u32 a0) {
    const u32 lr = c.lr;
    c.r[3] = a0;
    c.call(fn);
    c.lr = lr;
}
struct Music {
    int want = -1;     // the track the area wants (-1: the map's own)
    int playing = -1;  // on stream 2 now (-1: none)
    int wait = 0;      // frames until the next step (a fade under way)
    bool ending = false;
    bool faded = false;  // the map's faded out, for the area's
} s_music;

// Each frame (the first rider's): towards the music the area wants, a step at a time.
// (The main music, stream 1, changed - 8005E1A8(track): City Trial's own way of starting its music - rather than the
// events' second stream: starting that again after it had played and stopped froze the game.)
constexpr u32 kMusicPlay = 0x8005E1A8;
constexpr int kCityTrack = 0x26;  // city.hps: the map's own music
void musicFrame(Ctx& c) {
    if (eventHasMusic()) return;  // (an event has the music: it gives it back when it ends)
    const int want = s_music.want >= 0 ? s_music.want : kCityTrack;
    if (s_music.playing < 0) s_music.playing = kCityTrack;  // (the race starts with the map's)
    if (want == s_music.playing) return;
    gcall(c, kMusicPlay, u32(want));
    s_music.playing = want;
    log("[maps] music: track %d\n", want);
}

void playAreaMusic(Ctx& c, const char* track) {
    (void)c;
    s_music.want = track ? trackId(track) : -1;
    log("[maps] music: %s\n", track ? track : "the map's");
}

// ---- a minimap per part of the map (minimaps.txt, written by the map builder: Minimap.cs) ----
// One scale for them all; each its own picture (the HUD's 8-bit one and its palette, swapped in the HUD's files where the
// game loaded them - IfAll1c.dat, IfAll1cs.dat - found by the first one's bytes, as the map's files hold it) and its
// own offset (the stage node's +64..+6C: where a rider's mark goes). The first rider's part decides which is shown.
struct MinimapInfo {
    std::string name;
    std::vector<u8> image, palette;
    float offset[3], lo[2], hi[2];
};
std::vector<MinimapInfo> s_minimaps;
int s_minimapShown = 0;
struct HudCopy {
    u32 base = 0, image = 0, palette = 0;  // (guest addresses: the file, its picture, its palette)
};
HudCopy s_hud[2];

void loadMinimaps(const std::string& dir) {
    s_minimaps.clear();
    std::FILE* f = std::fopen((dir + "/minimaps.txt").c_str(), "r");
    if (!f) return;
    char line[512];
    while (std::fgets(line, sizeof line, f)) {
        char name[128], file[128];
        float v[7];
        unsigned imageBytes = 0, paletteBytes = 0;
        if (std::sscanf(line, "minimap %127s %127s %f %f %f %f %f %f %f %u %u", name, file, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6],
                        &imageBytes, &paletteBytes) != 11)
            continue;
        MinimapInfo m;
        m.name = name;
        for (int i = 0; i < 3; i++) m.offset[i] = v[i];
        m.lo[0] = v[3], m.lo[1] = v[4], m.hi[0] = v[5], m.hi[1] = v[6];
        if (std::FILE* b = std::fopen((dir + "/" + file).c_str(), "rb")) {
            std::vector<u8> all(imageBytes + paletteBytes);
            if (std::fread(all.data(), 1, all.size(), b) == all.size()) {
                m.image.assign(all.begin(), all.begin() + imageBytes);
                m.palette.assign(all.begin() + imageBytes, all.end());
            }
            std::fclose(b);
        }
        if (!m.image.empty()) s_minimaps.push_back(std::move(m));
    }
    std::fclose(f);
    log("[maps] %zu minimaps\n", s_minimaps.size());
}

// The HUD's files (the minimap's picture in them) are loaded once, as the game starts, and kept: not served like the
// stage's files (a map chosen later would never be read), but changed where the game holds them - the minimap's picture
// and palette found in the disc's file (an image descriptor: CI8 of the map's picture's size; the texture that takes it,
// +4C, its palette's descriptor at +50), the map's written there, the disc's own written back for the city.
const char* const kHudFiles[2] = {"IfAll1c.dat", "IfAll1cs.dat"};
struct HudPatch {
    u32 image = 0, palette = 0;  // their offsets in the file (0: not found)
    std::vector<u8> discImage, discPalette;
    u32 appliedAt = 0;  // where the map's were last written (the file's place then); 0: not written
} s_hudPatch[2];

void hudPatchMake(int k) {
    HudPatch& p = s_hudPatch[k];
    p = {};
    if (s_minimaps.empty()) return;
    const std::vector<u8> d = dvdDiscFile(kHudFiles[k]);
    auto be32 = [&](size_t o) { return o + 4 <= d.size() ? u32(d[o]) << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3] : 0u; };
    auto be16 = [&](size_t o) { return o + 2 <= d.size() ? u32(d[o]) << 8 | d[o + 1] : 0u; };
    if (d.size() < 0x20) return;
    const size_t imageBytes = s_minimaps[0].image.size(), paletteBytes = s_minimaps[0].palette.size();
    const u32 dataSize = std::min<u32>(be32(4), u32(d.size() - 0x20));
    u32 desc = 0, image = 0, palette = 0;
    for (u32 o = 0; o + 12 <= dataSize && !desc; o += 4) {
        const u32 ptr = be32(0x20 + o), w = be16(0x20 + o + 4), h = be16(0x20 + o + 6), fmt = be32(0x20 + o + 8);
        if (fmt == 9 && w * h == imageBytes && ptr % 32 == 0 && ptr + imageBytes <= dataSize) desc = o, image = ptr;
    }
    for (u32 o = 0; o + 8 <= dataSize && desc && !palette; o += 4)
        if (be32(0x20 + o) == desc) {
            const u32 tlut = be32(0x20 + o + 4), at = tlut + 4 <= dataSize ? be32(0x20 + tlut) : 0;
            if (at && at + paletteBytes <= dataSize) palette = at;
        }
    if (!image || !palette) {
        log("[maps] %s: the minimap's picture not found (%08X, %08X): the city's left\n", kHudFiles[k], image, palette);
        return;
    }
    p.image = 0x20 + image, p.palette = 0x20 + palette;
    p.discImage.assign(d.begin() + p.image, d.begin() + p.image + imageBytes);
    p.discPalette.assign(d.begin() + p.palette, d.begin() + p.palette + paletteBytes);
}

// The map's minimap (or the disc's: `mod` false) where the game holds the HUD file now.
void hudPatchApply(int k, bool mod) {
    HudPatch& p = s_hudPatch[k];
    const u32 base = dvdLoadedAt(kHudFiles[k]);
    s_hud[k] = {};
    if (!base || !p.image) return;
    const std::vector<u8>& image = mod ? s_minimaps[0].image : p.discImage;
    const std::vector<u8>& palette = mod ? s_minimaps[0].palette : p.discPalette;
    std::memcpy(hostp(base + p.image), image.data(), image.size());
    std::memcpy(hostp(base + p.palette), palette.data(), palette.size());
    p.appliedAt = mod ? base : 0;
    if (mod) s_hud[k] = {base, base + p.image, base + p.palette};
}

// The HUD's copy of the minimap's picture and palette in guest memory.
bool findHud(int k) {
    const u32 base = dvdLoadedAt(kHudFiles[k]);
    HudCopy& h = s_hud[k];
    if (!base || !s_hudPatch[k].image) return false;
    if (s_hudPatch[k].appliedAt != base) hudPatchApply(k, true);  // (loaded again since: the map's minimap into it again)
    else if (!h.image) h = {base, base + s_hudPatch[k].image, base + s_hudPatch[k].palette};
    return h.image != 0;
}

void showMinimap(Ctx& c, int i) {
    const MinimapInfo& m = s_minimaps[size_t(i)];
    // the offset: in the stage node
    const u32 gp = rd32(c.r[13] + 0x5EC);
    if ((gp & 0xFE000000u) == 0x80000000u) {
        const u32 st = rd32(rd32(gp + 8) + 4);
        if ((st & 0xFE000000u) == 0x80000000u)
            for (int k = 0; k < 3; k++) wr32(st + 0x64 + 4 * u32(k), bits32(m.offset[k]));
    }
    // the picture: in each HUD file loaded
    for (int k = 0; k < 2; k++)
        if (findHud(k)) {
            std::memcpy(hostp(s_hud[k].image), m.image.data(), m.image.size());
            if (s_hud[k].palette) std::memcpy(hostp(s_hud[k].palette), m.palette.data(), m.palette.size());
        }
    s_minimapShown = i;
    log("[maps] minimap: %s\n", m.name.c_str());
}

void minimapFrame(Ctx& c, const float pos[3]) {
    if (s_minimaps.size() < 2) return;
    for (size_t i = 0; i < s_minimaps.size(); i++) {
        const MinimapInfo& m = s_minimaps[i];
        if (pos[0] >= m.lo[0] && pos[0] <= m.hi[0] && pos[2] >= m.lo[1] && pos[2] <= m.hi[1]) {
            if (int(i) != s_minimapShown) showMinimap(c, int(i));
            return;
        }
    }
}

// ---- a map in parts: only the first rider's part drawn ----
// The stage's culling (800E6074) shows the meshes of each view region (bounding data: gp +E4 -> its data, +0 the
// regions, 0x20 each - +0 the mesh list, +4 its count, +8/+14 the box - +4 their count) whose box the camera sees, so a
// part far off but in sight is drawn. The map builder gives each part its own region: the part the first rider is in
// keeps its meshes (its box made all-covering: always seen), the others' counts set to 0 - the game's own culling, with
// nothing replaced in it (each frame, natively).
struct RegionSave {
    u32 data = 0;
    std::vector<u16> counts;
    std::vector<std::array<u32, 6>> boxes;
    int active = -2;
} s_regions;

void partsFrame(Ctx& c, const float pos[3]) {
    const u32 gp = rd32(c.r[13] + 0x5EC);
    if ((gp & 0xFE000000u) != 0x80000000u) return;
    const u32 data = rd32(gp + 0xE4);
    if ((data & 0xFE000000u) != 0x80000000u) return;
    const u32 base = rd32(data), n = rd16(data + 4);
    if ((base & 0xFE000000u) != 0x80000000u || n < 2 || n > 64) return;
    if (s_regions.data != data) {  // (a stage loaded: its regions as the file has them)
        s_regions = {};
        s_regions.data = data;
        for (u32 k = 0; k < n; k++) {
            s_regions.counts.push_back(u16(rd16(base + k * 0x20 + 4)));
            std::array<u32, 6> b;
            for (u32 i = 0; i < 6; i++) b[i] = rd32(base + k * 0x20 + 8 + 4 * i);
            s_regions.boxes.push_back(b);
        }
    }
    const u32 st = rd32(rd32(gp + 8) + 4);
    float scale = (st & 0xFE000000u) == 0x80000000u ? flt(rd32(st + 8)) : 1.f;
    if (!(scale > 0.01f)) scale = 1.f;
    const float x = pos[0] / scale, z = pos[2] / scale;  // (the rider in the world; the boxes in the file's units)
    int active = -1;
    for (u32 k = 0; k < n && active < 0; k++) {
        const auto& b = s_regions.boxes[k];
        if (x >= flt(b[0]) && x <= flt(b[3]) && z >= flt(b[2]) && z <= flt(b[5])) active = int(k);
    }
    if (active < 0 || active == s_regions.active) return;  // (between parts: as it was)
    s_regions.active = active;
    for (u32 k = 0; k < n; k++) {
        const u32 e = base + k * 0x20;
        const bool on = int(k) == active;
        wr16(e + 4, on ? s_regions.counts[k] : 0);
        for (u32 i = 0; i < 6; i++) wr32(e + 8 + 4 * i, on ? bits32(i < 3 ? -1e7f : 1e7f) : s_regions.boxes[k][i]);
    }
    log("[maps] drawn: part %d of %u\n", active, n);
}

// What a match leaves behind, cleared for the next (a new stage loaded, or another map chosen).
void resetMatch() {
    std::memset(s_warpReady, 0, sizeof s_warpReady), std::memset(s_frame, 0, sizeof s_frame);
    std::memset(s_hasPrev, 0, sizeof s_hasPrev), std::memset(s_inWarp, 0, sizeof s_inWarp);
    s_pending = {};
    s_inArea = -1;
    s_music = {};
    s_minimapShown = 0;
    s_hud[0] = s_hud[1] = {};
    s_regions = {};
    for (int k = 0; k < 2; k++)  // (the HUD loaded again since the map was chosen: its minimap into it again)
        if (s_hudPatch[k].appliedAt != dvdLoadedAt(kHudFiles[k])) hudPatchApply(k, true);
}

void riderFrame(Ctx& c, u32 slot) {
    if (!morepl::isRiderSlot(slot)) return;
    {
        static u32 s_stage = 0;  // (the stage's bounding data: another one, another match)
        const u32 gp = rd32(c.r[13] + 0x5EC);
        const u32 stage = (gp & 0xFE000000u) == 0x80000000u ? rd32(gp + 0xE4) : 0;
        if (stage != s_stage) {
            s_stage = stage, resetMatch();
            if (std::getenv("SA2_MAP_TREELOG")) {
                const u32 g = rd32(c.r[13] + 0x5EC), t = (g & 0xFE000000u) == 0x80000000u ? rd32(g + 0x700) : 0;
                log("[maps] tree %08X: +54 %08X +58 %04X (iterator %08X)\n", t, t ? rd32(t + 0x54) : 0, t ? rd16(t + 0x58) : 0, g ? rd32(g + 0x704) : 0);
            }
            if (std::getenv("SA2_MAP_PRELOADSCAN")) {  // (debugging: where the game keeps the stage files' places)
                const u32 model = dvdOverlayLoadedAt("GrCity1Model.dat", nullptr);
                log("[maps] scan: GrCity1Model.dat at %08X\n", model);
                for (u32 a = 0x80003100; model && a + 4 <= 0x81800000; a += 4)
                    if (rd32(a) == model) {
                        log("[maps] scan: %08X:", a);
                        for (u32 k = 0; k < 16; k++) log(" %08X", rd32(a - 0x20 + 4 * k));
                        log("\n");
                    }
            }
        }
    }
    const u32 p = morepl::at(morepl::k_ptab, slot);
    if (rd32(p) == 4) return;  // (no rider in this slot)
    const u32 mg = rd32(p + 0x40);
    if ((mg & 0xFE000000u) != 0x80000000u) return;
    const u32 md = rd32(mg + 0x2C);
    if ((md & 0xFE000000u) != 0x80000000u) return;
    const float pos[3] = {flt(rd32(md + 0x3E8)), flt(rd32(md + 0x3EC)), flt(rd32(md + 0x3F0))};
    const u32 frame = ++s_frame[slot];
    if (slot == 0 && (frame == 1 || frame % 600 == 0))
        log("[maps] rider 0 (kind %u) at %.1f %.1f %.1f, frame %u\n", rd32(p), pos[0], pos[1], pos[2], frame);
    // warps: any point of the way from last frame's place to this one's in a warp's box
    auto through = [&](const Box& b) {
        if (!s_hasPrev[slot]) return b.contains(pos);
        for (int k = 0; k <= 8; k++) {
            float q[3];
            for (int i = 0; i < 3; i++) q[i] = s_prev[slot][i] + (pos[i] - s_prev[slot][i]) * float(k) / 8.f;
            if (b.contains(q)) return true;
        }
        return false;
    };
    const bool moved = s_hasPrev[slot];
    for (int i = 0; i < 3; i++) s_prev[slot][i] = pos[i];
    s_hasPrev[slot] = true;
    // (put down inside a warp's box - OoT's spots stand by its exits - it must ride out of them before one takes it)
    if (s_inWarp[slot]) {
        bool still = false;
        for (const WarpInfo& w : s_warps) still = still || w.contains(pos);
        if (!still) s_inWarp[slot] = false;
    }
    if (moved && !s_inWarp[slot] && frame >= s_warpReady[slot])
        for (const WarpInfo& w : s_warps)
            if (through(w)) {
                const float r = w.toYaw * 3.14159265f / 180.f;
                s_pending.machine = md;
                for (int i = 0; i < 3; i++) s_pending.pos[i] = w.to[i];
                s_pending.fwd[0] = -std::sin(r), s_pending.fwd[1] = 0, s_pending.fwd[2] = -std::cos(r);
                // (the respawn puts a machine back by the zone it fell into, +1B48, when it has one - a warp's has none:
                // the start-spot way taken, the spot replaced in 801CB50C)
                const u32 lr = c.lr, zone = rd32(md + 0x1B48);
                wr32(md + 0x1B48, 0xFFFFFFFFu);
                c.r[3] = md;
                c.call(kRespawn);
                c.lr = lr;
                wr32(md + 0x1B48, zone);
                s_pending.machine = 0;
                s_warpReady[slot] = frame + 45;
                s_hasPrev[slot] = false;  // (no way ridden from before the warp to after it)
                s_inWarp[slot] = true;
                log("[maps] rider %u warped by %s to %.0f %.0f %.0f\n", slot, w.name.c_str(), w.to[0], w.to[1], w.to[2]);
                break;
            }
    if (slot == 0) minimapFrame(c, pos), partsFrame(c, pos);
    if (slot == 0) {  // (City Trial's events, as they change: finding which one a custom map can't take)
        static u32 s_lastState = ~0u;
        const u32 s = rd32(kEventState);
        if ((s & 0xFE000000u) == 0x80000000u) {
            const u32 e = rd32(s + 0x2C);
            if ((e & 0xFE000000u) == 0x80000000u && rd32(e + 4) != s_lastState) {
                s_lastState = rd32(e + 4);
                log("[maps] event state %u: %08X %08X %08X %08X %08X %08X %08X %08X\n", s_lastState, rd32(e), rd32(e + 4), rd32(e + 8), rd32(e + 12),
                    rd32(e + 16), rd32(e + 20), rd32(e + 24), rd32(e + 28));
            }
        }
    }
    // areas: the first rider's decides the music
    if (slot == 0 && !s_areas.empty()) {
        int in = -1;
        for (size_t i = 0; i < s_areas.size(); i++)
            if (s_areas[i].track != "-" && s_areas[i].contains(pos)) {
                in = int(i);
                break;
            }
        if (in != s_inArea) {
            s_inArea = in;
            playAreaMusic(c, in >= 0 ? s_areas[size_t(in)].track.c_str() : nullptr);
        }
        musicFrame(c);
    }
}
}  // namespace

// ---- map mods: mods/maps/<folder>/ beside the program, each a map as the map editor builds it (its stage files, and
// mod.json: {"name": ...}) - the one chosen served in the city's place (the disc's files again for the city) ----
namespace {
struct MapMod {
    std::string folder, title, dir;
};
std::vector<MapMod> s_mods;       // (s_mods[0]: SA2_MAP_DIR's, when set - developing a map)
int s_selected = 0;               // 0: the city; i: s_mods[i - 1]
std::vector<std::string> s_served;

std::string modTitle(const std::filesystem::path& dir, const std::string& fallback) {
    std::FILE* f = _wfopen((dir / "mod.json").c_str(), L"rb");
    if (!f) return fallback;
    std::string text(4096, ' ');
    text.resize(std::fread(text.data(), 1, text.size(), f));
    std::fclose(f);
    const size_t key = text.find("\"name\"");
    if (key == std::string::npos) return fallback;
    const size_t a = text.find('"', text.find(':', key));
    const size_t b = a == std::string::npos ? a : text.find('"', a + 1);
    return b == std::string::npos || b == a + 1 ? fallback : text.substr(a + 1, b - a - 1);
}

void scanMods() {
    s_mods.clear();
    if (const char* dev = std::getenv("SA2_MAP_DIR")) s_mods.push_back({"", modTitle(std::filesystem::u8path(dev), "Test Map"), dev});
    std::error_code ec;
    std::vector<MapMod> found;
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path("mods") / "maps", ec)) {
        if (!e.is_directory() || !std::filesystem::exists(e.path() / "GrCity1.dat", ec)) continue;
        const std::u8string folder = e.path().filename().u8string(), full = e.path().u8string();
        const std::string f(folder.begin(), folder.end());
        found.push_back({f, modTitle(e.path(), f), std::string(full.begin(), full.end())});
    }
    std::sort(found.begin(), found.end(), [](const MapMod& a, const MapMod& b) { return a.title < b.title; });
    s_mods.insert(s_mods.end(), found.begin(), found.end());
}

// The game's preload cache (80550F68: 100 entries of 0x28 from +104 - +0 in use, +6 the file's entry number; looked up
// by entry number, 80073550...) keeps the stage's files from when Start Game was chosen - and from the last match - so
// a map chosen after that would not be read: the stage files' entries renumbered (-2: no file's), the next lookup finding
// none and reading the file again (the old entry's memory let go as the cache makes room, as for any file not wanted).
void forgetPreloads(const std::vector<std::string>& paths) {
    constexpr u32 kCache = 0x80550F68 + 0x104, kEntries = 100;
    for (const std::string& path : paths) {
        const u32 num = dvdEntryNum(path);
        if (!num) continue;
        for (u32 k = 0; k < kEntries; k++) {
            const u32 e = kCache + k * 0x28;
            if (rd8(e) && rd16(e + 6) == num) {
                wr16(e + 6, 0xFFFE);
                log("[maps] %s: the game's preloaded copy forgotten\n", path.c_str());
            }
        }
    }
}

void serve(int i) {
    {
        std::vector<std::string> stage = {"GrCity1.dat", "GrCity1Model.dat", "GrCity1Event.dat"};
        stage.insert(stage.end(), s_served.begin(), s_served.end());
        forgetPreloads(stage);
    }
    dvdUnserve(s_served);
    s_served.clear();
    for (int k = 0; k < 2; k++) hudPatchApply(k, false), s_hudPatch[k] = {};  // (the city's minimap back)
    resetMatch();
    s_areas.clear(), s_warps.clear(), s_minimaps.clear();
    s_selected = i > 0 && i <= int(s_mods.size()) ? i : 0;
    if (!s_selected) {
        log("[maps] City Trial: the city\n");
        return;
    }
    const MapMod& m = s_mods[size_t(s_selected - 1)];
    dvdServeDir(m.dir, s_served, {kHudFiles[0], kHudFiles[1]});
    loadMapInfo(m.dir), loadMinimaps(m.dir);
    for (int k = 0; k < 2; k++) hudPatchMake(k), hudPatchApply(k, true);
    log("[maps] City Trial: %s (%s, %zu files)\n", m.title.c_str(), m.dir.c_str(), s_served.size());
}
}  // namespace

namespace maps {
int count() { return 1 + int(s_mods.size()); }
std::string name(int i) { return i <= 0 || i > int(s_mods.size()) ? std::string("Original") : s_mods[size_t(i - 1)].title; }
int selected() { return s_selected; }
void select(int i) {
    if (i == s_selected) return;
    serve(i);
    if (s_selected == 0 || !s_mods[size_t(s_selected - 1)].folder.empty()) {  // (SA2_MAP_DIR's is never the one kept)
        settings::cityMap.set(s_selected ? s_mods[size_t(s_selected - 1)].folder : std::string());
        settings::save();
    }
}
void rescan() {
    const std::string dir = s_selected ? s_mods[size_t(s_selected - 1)].dir : std::string();
    scanMods();
    int keep = 0;
    for (size_t k = 0; k < s_mods.size(); k++)
        if (s_mods[k].dir == dir) keep = int(k) + 1;
    if (keep != s_selected || (keep && dir != s_mods[size_t(keep - 1)].dir)) serve(keep);
    else s_selected = keep;
}
}  // namespace maps

// (SA2_MAP_SKIP=gimmicks,nullpos,mode: only those left out - finding what a custom map needs)
bool skipping(const char* what) {
    const char* s = std::getenv("SA2_MAP_SKIP");
    return s ? std::strstr(s, what) != nullptr : std::strcmp(what, "nullpos") != 0;  // (the markers are kept: the map builder keeps the city's joints)
}

void cityMenuInit();  // (citymenu.cpp: the map chosen in City Trial's settings)

void mapsInit() {
    cityMenuInit();
    // The stage's files (Gr*.dat: the stage preloader's heap 4) in RAM of their own above the console's (ctx.h,
    // kExtRamBase: 32 MB), not the console's fixed 8.36 MB - a custom map's are not held to the city's sizes. The heaps
    // are made from a table (0x80497ED0: id, type, placed after, size): heap 4's size there made small (its place in
    // MEM1 left to the HSD heap, which takes what the others leave), its pool made in the extra RAM instead each time
    // the scene's heaps are made (80058564 -> 800577B8(start, end), r26 the heap's slot: 80537F50 + id x 1C).
    constexpr u32 kHeapTable4Size = 0x80497EFC, kHeap4Slot = 0x80537F50 + 4 * 0x1C, kMakePool = 0x800577B8;
    if (rd32(kHeapTable4Size) == 0x0085C000 && rd32(kHeapTable4Size - 0xC) == 4 && rd32(kMakePool) == 0x9421FFE0) {
        wr32(kHeapTable4Size, 0x8000);
        interpReplace(0, kMakePool, [](Ctx& c, u32 entry) {
            if (c.r[26] == kHeap4Slot && c.lr == 0x800587FCu) {
                // (the pool alone: the slot keeps its place in MEM1 - the next heaps are laid out after it, by its start
                // and size, and one laid after the extra RAM would wrap round onto the program)
                c.r[3] = 0x80000000u + kExtRamBase, c.r[4] = 0x80000000u + kExtRamBase + kExtRamSize;
                static bool s_said = false;
                if (!s_said) s_said = true, log("[maps] the stage's heap: %u MB, above the console's RAM\n", kExtRamSize >> 20);
            }
            interpOriginal(c, entry);
        });
    } else
        log("[maps] the heap table is not as expected (%08X): the stage's heap left as it is\n", rd32(kHeapTable4Size));
    for (const char* f : kHudFiles) dvdWatch(f);  // (where the HUD files are held: their minimap is changed there)
    if (std::getenv("SA2_MAP_PRELOADSCAN")) dvdWatch("GrCity1Model.dat");
    // the map chosen: SA2_MAP_DIR's when set, else the one the settings keep
    scanMods();
    int pick = std::getenv("SA2_MAP_DIR") ? 1 : 0;
    if (!pick) settings::load();  // (before the frontend reads them: this is earlier)
    if (!pick)
        for (size_t k = 0; k < s_mods.size(); k++)
            if (!s_mods[k].folder.empty() && s_mods[k].folder == settings::cityMap.get()) pick = int(k) + 1;
    log("[maps] %zu map mods\n", s_mods.size() - (std::getenv("SA2_MAP_DIR") ? 1 : 0));
    if (pick) serve(pick);
    // (SA2_MAP_EVENTLOG: City Trial's event choice, logged - finding which a custom map can't take)
    if (std::getenv("SA2_MAP_EVENTLOG") && rd32(0x800EDCF8) == 0x9421FF50)
        interpReplace(0, 0x800EDCF8, [](Ctx& c, u32 entry) {
            log("[maps] event decide: in r3 %08X r4 %08X\n", c.r[3], c.r[4]);
            interpOriginal(c, entry);
            log("[maps] event decided: r3 %08X\n", c.r[3]);
        });
    if (rd32(kMapMark) != 0x9421FFA0) log("[maps] the minimap's marks (%08X) are not as expected: no areas or warps\n", rd32(kMapMark));
    else if (interpReplace(0, kMapMark, [](Ctx& c, u32 entry) {
            const u32 mark = c.r[3];
            interpOriginal(c, entry);
            if (static int s_n = 0; std::getenv("SA2_MAP_TREELOG") && s_n++ % 3000 == 0) {
                const u32 g = rd32(c.r[13] + 0x5EC), t = (g & 0xFE000000u) == 0x80000000u ? rd32(g + 0x700) : 0;
                log("[maps] tree %08X: +54 %08X +58 %04X (custom %d)\n", t, t ? rd32(t + 0x54) : 0, t ? rd16(t + 0x58) : 0, int(g_customCity.load()));
            }
            if (!g_customCity.load() || (s_warps.empty() && s_areas.empty() && s_minimaps.size() < 2)) return;
            const u32 d = rd32(mark + 0x2C);
            static bool s_said = false;
            if (!s_said) s_said = true, log("[maps] the riders' minimap marks: areas and warps checked from here (mark data %08X, slot %u)\n", d, d ? rd32(d + 0x20) : 0);
            if ((d & 0xFE000000u) == 0x80000000u) riderFrame(c, rd32(d + 0x20));
        }))
        log("[maps] areas and warps: checked as the minimap's marks move\n");
    if (rd32(kPutMachine) == 0x9421FFB0)
        interpReplace(0, kPutMachine, [](Ctx& c, u32 entry) {
            if (s_pending.machine && c.r[3] == s_pending.machine) {  // (a warp: its spot, standing up)
                for (int i = 0; i < 3; i++) {
                    wr32(c.r[4] + 4 * u32(i), bits32(s_pending.pos[i]));
                    wr32(c.r[5] + 4 * u32(i), bits32(s_pending.fwd[i]));
                    wr32(c.r[6] + 4 * u32(i), bits32(i == 1 ? 1.f : 0.f));
                }
            }
            interpOriginal(c, entry);
        });
    if (rd32(kCityGimmicks) == 0x9421FFF0)  // (stwu r1,-16(r1): the function as the program has it)
        interpReplace(0, kCityGimmicks, [](Ctx& c, u32 entry) {
            if (g_customCity.load() && skipping("gimmicks")) {
                log("[maps] custom map: the city's gimmicks left out\n");
                return;
            }
            interpOriginal(c, entry);
        });
    if (rd32(kNullPosMatch) == 0x9421FFA0)  // (stwu r1,-96(r1))
        interpReplace(0, kNullPosMatch, [](Ctx& c, u32 entry) {
            if (g_customCity.load() && skipping("nullpos")) {
                c.r[3] = 0xFFFFFFFFu;  // (none missing)
                return;
            }
            interpOriginal(c, entry);
        });
    if (std::getenv("SA2_MAP_CAMLOG"))  // (debugging a custom map: the camera's parameters as each frame sets them)
        interpReplace(0, 0x800B31D4, [](Ctx& c, u32 entry) {
            static int s_n = 0;
            if (g_customCity.load() && s_n++ < 6) {
                float v[15];
                for (int i = 0; i < 15; i++) v[i] = __builtin_bit_cast(float, rd32(c.r[4] + i * 4));
                log("[maps] camera %08X: %.1f %.1f %.1f | %.1f %.1f %.1f | %.2f %.2f %.2f | %.2f %.2f %.2f | %.2f %.2f %.2f\n", c.r[3], v[0], v[1], v[2], v[3], v[4],
                    v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14]);
            }
            interpOriginal(c, entry);
        });
    if (rd32(kCityModeSetup) == 0x9421FFF0)
        interpReplace(0, kCityModeSetup, [](Ctx& c, u32 entry) {
            if (!g_customCity.load() || !skipping("mode")) {
                interpOriginal(c, entry);
                return;
            }
            // (800DBE48 animates the city's parts, from the stage's parameters: none on a custom map)
            const u32 stage = rd32(c.r[3] + 0x2C), r30 = c.r[30], r31 = c.r[31], lr = c.lr;
            c.r[3] = stage;
            c.call(0x800DC1B4);
            c.r[30] = r30, c.r[31] = r31, c.lr = lr;
            log("[maps] custom map: the city's parts for the mode left out\n");
        });
}

}  // namespace rt

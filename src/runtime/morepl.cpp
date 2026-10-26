// More than 4 riders (docs/MORE_PLAYERS.md): host-side support for the 8-player build (the game generated with
// config/GKYE01/patches_8p.txt). Nothing here does anything in the normal build.
#include "runtime/morepl.h"
#include "runtime/interp.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <tuple>

namespace rt {

namespace {
// The span of the old places of the moved arrays (a quick test before the list).
struct Span {
    u32 lo = ~0u, hi = 0;
};
const Span s_span = [] {
    Span s;
    for (const morepl::Moved& m : morepl::kAll) s.lo = std::min(s.lo, m.addr), s.hi = std::max(s.hi, m.addr + m.oldCount * m.stride);
    return s;
}();
}  // namespace

// The game clears blocks of its state with memset (Gm_InitData, GameManagerGObj_Create, 3D_InitHUD, ...): the
// arrays moved out of a block are cleared along with it, whole.
HLE(memset) {
    static const GuestFn s_original = originalOf("memset");
    const u32 dst = c.r[3], value = c.r[4] & 0xFF, n = c.r[5];
    s_original(c);
    if (!n || dst >= s_span.hi || dst + n <= s_span.lo || !morepl::active()) return;
    for (const morepl::Moved& m : morepl::kAll) {
        const u32 end = m.addr + m.oldCount * m.stride;
        if (m.addr >= dst + n || dst >= end) continue;
        // the part cleared at the old place, at the new one; a clear of the whole array clears its new slots too
        const u32 lo = std::max(dst, m.addr) - m.addr;
        const u32 hi = dst <= m.addr && end <= dst + n ? m.count * m.stride : std::min(dst + n, end) - m.addr;
        for (u32 a = m.to + lo; a < m.to + hi; a++) wr8(a, value);
    }
}

// ---- The game's functions unrolled for 5 slots, redone for all (installed in the 8-player build) ----
namespace {
// The preload list's reset (8023239C, unrolled x5): each slot's rider and machine to load (none: -1), its color,
// two flags off.
void preloadReset(Ctx&, u32) {
    for (u32 i = 0; i < morepl::k_preload.count; i++) {
        const u32 e = morepl::at(morepl::k_preload, i);
        wr32(e + 0, 0xFFFFFFFFu);
        wr32(e + 4, 0xFFFFFFFFu);
        wr8(e + 8, 0x13);
        wr8(e + 9, rd8(e + 9) & 0x3F);
    }
}
constexpr u32 kGm = 0x805359D8;
// Gm_GetUnkFloat (8000A370, unrolled x4): (riders in the match - 1 - the riders before `ply`) x a constant; used
// to place riders and views.
void gmRiderSpacing(Ctx& c, u32) {
    const s32 ply = s32(c.r[3]);
    int before = 0;
    for (int i = 0; i < morepl::riders() && s32(morepl::riderSlot(i)) < ply; i++)
        if (rd8(morepl::plyDesc(morepl::riderSlot(i))) != 4) before++;
    const s32 v = s32(s8(rd8(kGm + 0xA95))) - 1 - before;
    c.f[1] = double(flt(rd32(c.r[2] - 0x7FB0)) * float(v));
}
// The camera's look-at check (800B31D4: asserts its up vector is not zero): which camera, logged; then on with
// straight up (tests, while riders 6-9 are brought up).
u32 s_camByPly[16] = {};  // (debugging: a camera seen per rider)
void dumpCamDiff(u32 bad, u32 good) {
    // the camera data (0x2D0 bytes) and its player camera data (+17C, 0x74 bytes), word by word where they differ
    for (const auto& [off, len, ptr] : {std::tuple<u32, u32, bool>{0, 0x2D0, false}, {0x17C, 0x74, true}}) {
        const u32 b = ptr ? rd32(bad + off) : bad, g = ptr ? rd32(good + off) : good;
        if (!b || !g) continue;
        for (u32 o = 0; o < len; o += 4) {
            const u32 wb = rd32(b + o), wg = rd32(g + o);
            const float fb = flt(wb);
            if (wb != wg && (std::isnan(fb) || wb == 0 || std::isnan(flt(wg)) || wg == 0))
                log("[8p]   %s+%03X: bad %08X (%g)  good %08X (%g)\n", ptr ? "pcd" : "cam", o, wb, fb, wg, flt(wg));
        }
    }
}
void cameraLookAt(Ctx& c, u32 entry) {
    const u32 cam = c.r[3], param = c.r[4];
    if (const u32 t = rd32(cam + 0x94)) {
        const s32 ply = s16(rd16(t + 0xA6));
        if (ply >= 0 && ply < 16) s_camByPly[ply] = cam;
    }
    const float x = flt(rd32(param + 0x18)), y = flt(rd32(param + 0x1C)), z = flt(rd32(param + 0x20));
    const float eps = flt(rd32(c.r[2] - 0x7500));  // (the game's Epsilon)
    if (!(std::sqrt(x * x + y * y + z * z) > eps)) {
        static int s_n = 0;
        if (s_n++ < 20) {
            const u32 target = rd32(cam + 0x94), pcd = rd32(cam + 0x17C);
            log("[8p] camera %08X kind %u: up is zero; target ply %d, its player %u view %u; eye %.1f %.1f %.1f at %.1f %.1f %.1f\n",
                cam, rd32(cam), target ? s32(s16(rd16(target + 0xA6))) : -1, pcd ? rd16(pcd + 0x68) : 99u,
                pcd ? rd16(pcd + 0x6A) : 99u, flt(rd32(param)), flt(rd32(param + 4)), flt(rd32(param + 8)),
                flt(rd32(param + 0xC)), flt(rd32(param + 0x10)), flt(rd32(param + 0x14)));
            static bool s_dumped = false;
            if (!s_dumped && s_camByPly[1] && s_camByPly[1] != cam) {
                s_dumped = true;
                log("[8p] camera fields that are NaN/zero here but not in rider 1's camera:\n");
                dumpCamDiff(cam, s_camByPly[1]);
            }
        }
        wr32(param + 0x18, bits32(0.f));
        wr32(param + 0x1C, bits32(1.f));
        wr32(param + 0x20, bits32(0.f));
    }
    interpOriginal(c, entry);
}
// The cameras' setup (PlayerCamLookup_Init 800B00D8, unrolled x4): the follow distances of riders 6-9 as rider 0's.
void camLookupInit(Ctx& c, u32 entry) {
    interpOriginal(c, entry);
    const u32 d0 = morepl::at(morepl::k_cam_dist, 0);
    for (u32 s = 4; s < morepl::k_cam_dist.count; s++) {
        wr32(morepl::at(morepl::k_cam_dist, s), rd32(d0));
        wr32(morepl::at(morepl::k_cam_dist, s) + 4, rd32(d0 + 4));
    }
}

// Start spots: a stage has 4 or 5. Riders 6-9 take those of riders 0-3 (Player_Create 800135EC reads the spot of its
// slot), moved off it (grGetStartPosition 800D0B7C, while their Player_Create runs; spotAside).
u32 s_creating = ~0u;
void playerCreate(Ctx& c, u32 entry) {
    const u32 slot = c.r[3] & 0xFF;
    if (slot >= 6 && slot < 10) {
        for (const morepl::Moved* m : {&morepl::k_start_pos, &morepl::k_start_pos_city})
            wr8(morepl::at(*m, slot), rd8(morepl::at(*m, slot - 6)));
        s_creating = slot;
    }
    interpOriginal(c, entry);
    s_creating = ~0u;
}
// A spot (position, forward, up: 3 floats each) for rider slot 6-9, from the spot of rider slot-6. In a Stadium the
// 4 spots stand side by side about 5 units apart (one start line): riders 6-9 make a second row, 12 units behind.
// In the city the spots are far apart: they stand 12 units to the side (forward x up), on alternate sides.
constexpr u32 kIsInStadium = 0x8000AD48;  // CityTrial_IsInStadium
void spotAside(Ctx& c, u32 pos, u32 fwd, u32 up, u32 slot) {
    if (!pos || !fwd || !up) return;
    auto v = [](u32 a, int k) { return flt(rd32(a + 4 * u32(k))); };
    float r[3];
    float by;
    const u32 save3 = c.r[3];
    const bool stadium = s8(u8(guestCall(c, kIsInStadium))) == 1;
    c.r[3] = save3;
    if (stadium) {
        for (int k = 0; k < 3; k++) r[k] = v(fwd, k);
        by = -12.f;
    } else {
        r[0] = v(fwd, 1) * v(up, 2) - v(fwd, 2) * v(up, 1), r[1] = v(fwd, 2) * v(up, 0) - v(fwd, 0) * v(up, 2);
        r[2] = v(fwd, 0) * v(up, 1) - v(fwd, 1) * v(up, 0);
        by = (slot & 1) ? -12.f : 12.f;
    }
    const float len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (!(len > 1e-4f)) return;
    for (int k = 0; k < 3; k++) wr32(pos + 4 * u32(k), bits32(v(pos, k) + r[k] / len * by));
}
void startPosition(Ctx& c, u32 entry) {
    const u32 pos = c.r[4], fwd = c.r[5], up = c.r[6];
    interpOriginal(c, entry);
    if (s_creating != ~0u) spotAside(c, pos, fwd, up, s_creating);
}
// A machine put back on a start spot (f_800D4FB8(stage, rider slot, pos, fwd, up), from the machine's respawn
// 801E6718; asserts "0 <= num && num < grGetStartposNum()" outside the city): riders 6-9 take riders 0-3's, aside.
void respawnSpot(Ctx& c, u32 entry) {
    const u32 slot = c.r[4];
    if (slot < 6 || slot >= 10) return interpOriginal(c, entry);
    const u32 pos = c.r[5], fwd = c.r[6], up = c.r[7];
    c.r[4] = slot - 6;
    interpOriginal(c, entry);
    spotAside(c, pos, fwd, up, slot);
}
// A ground surface's lookup by a 0-1 position along it (800D416C, asserts the range; machines' ground contact):
// out-of-range positions logged and clamped (tests, while riders 6-9 are brought up).
void surfaceAt(Ctx& c, u32 entry) {
    const double p = c.f[1];
    if (!(p >= 0.0 && p <= 1.0)) {
        static int s_n = 0;
        if (s_n++ < 20) log("[8p] surface %d: position %f out of 0..1 (lr %08X)\n", s32(c.r[3]), p, c.lr);
        c.f[1] = p > 1.0 ? 1.0 : 0.0;
    }
    interpOriginal(c, entry);
}
// A sound track's release (AudioTrack_Free 8005D64C, asserts it is an automatic one, 0x22-0x17F): one outside
// is logged and skipped (an item's track gone bad with riders 6-9; to find).
void audioTrackFree(Ctx& c, u32 entry) {
    const u32 t = c.r[3];
    if (t && (t < 0x22 || t > 0x17F)) {
        static int s_n = 0;
        if (s_n++ < 10) log("[8p] sound track %u freed: not an automatic one (lr %08X) - skipped\n", t, c.lr);
        return;
    }
    interpOriginal(c, entry);
}
// An item's removal (f_8024F3B0, r3 = its object): one that is not an object in the heap (the item pool's recycling
// read an empty list with 8 riders' items) is logged and skipped. To find: why the list is empty.
void itemRemove(Ctx& c, u32 entry) {
    const u32 g = c.r[3];
    if (g < 0x80600000u || g >= 0x81800000u || (g & 3)) {
        static int s_n = 0;
        if (s_n++ < 10) log("[8p] item removal of %08X (not an object; lr %08X) - skipped\n", g, c.lr);
        return;
    }
    interpOriginal(c, entry);
}
// The item manager (Item_InitObj 8024E918 makes it on the heap, *805DD8CC): its item lists are kept per collecting
// slot, 5 groups (+8, 0x54 each). Moved to 81900000 with room for 10 (the fields after the 5 groups, +1AC..+1C8,
// live at 81900400: the patches move the instructions that reach them). Its own list pointers move with it.
constexpr u32 kItemMgr = 0x81900000, kItemMgrTrail = 0x81900400, kItemGroups = 10;
void itemInit(Ctx& c, u32 entry) {
    interpOriginal(c, entry);
    const u32 slot = c.r[13] + 0x7EC, heap = rd32(slot);
    if (!heap) return;
    auto reloc = [&](u32 v) { return v - heap < 0x1C8 ? v - heap + kItemMgr : v; };
    for (u32 o = 0; o < 8 + kItemGroups * 0x54 + 0x10; o += 4) wr32(kItemMgr + o, 0);
    for (u32 o = 0; o < 0x1AC; o += 4) wr32(kItemMgr + o, reloc(rd32(heap + o)));
    for (u32 o = 0x1AC; o < 0x1C8; o += 4) wr32(kItemMgrTrail + o - 0x1AC, reloc(rd32(heap + o)));
    for (u32 g = 5; g < kItemGroups; g++) {  // as the game sets up its 5
        const u32 p = kItemMgr + 8 + g * 0x54;
        wr32(p + 0x04, 0), wr32(p + 0x08, p + 0x10), wr32(p + 0x14, p), wr32(p + 0x18, p + 0x20), wr32(p + 0x24, p + 0x10);
        wr32(p + 0x28, p + 0x30), wr32(p + 0x34, p + 0x20), wr32(p + 0x38, 0), wr32(p + 0x40, p), wr32(p + 0x44, 0);
        wr32(p + 0x4C, 0), wr32(p + 0x50, 0), wr32(p + 0x48, 0);
    }
    wr32(slot, kItemMgr);
}
// The game manager's data (GameManagerGObj_Create 800130A8 takes 0xE4 bytes from its pool at 80537400 with
// HSD_ObjAlloc 804180E4; its pointer at r13+0x490): per slot it keeps bytes at +15 and words at +6C .. +D0, 5 each.
// That allocation is answered with a fixed address, so the patches move those arrays as static ones (to 10 slots).
// The pool's books stay right: counted in use here; at the GObj's end HSD_ObjFree puts it on the free list, taken
// off again the next time.
// (A hook by name - HSD_ObjAlloc runs for every HSD object: the others go straight to its translated body.)
constexpr u32 kGameMgrPool = 0x80537400, kGameMgr = 0x81900800, kGameMgrFrom = 0x80013118;
}  // namespace
HLE(HSD_ObjAlloc) {
    static const GuestFn s_original = originalOf("HSD_ObjAlloc");
    if (!morepl::active() || c.r[3] != kGameMgrPool || c.lr != kGameMgrFrom) return s_original(c);
    const u32 pool = c.r[3];
    if (rd32(pool + 4) == kGameMgr) {  // (on the free list: off it)
        wr32(pool + 4, rd32(kGameMgr));
        wr32(pool + 0xC, rd32(pool + 0xC) - 1);
    }
    wr32(pool + 8, rd32(pool + 8) + 1);
    for (u32 o = 0; o < 0x100; o += 4) wr32(kGameMgr + o, 0);
    c.r[3] = kGameMgr;
}
namespace {
// The rider in place `n` (f_8000A6C4, unrolled for riders 0-3; the minimap's dots are made in this order): in the
// Stadium groups 4, 5, 7 slot n itself; else the riders' race ranks (gm +848), ties broken in slot order, and the
// rider whose rank is n. 5 (nobody) when none.
void riderInPlace(Ctx& c, u32) {
    const s32 n = s32(c.r[3]);
    const u32 slots = morepl::k_ply_desc.count;
    auto present = [](u32 s) { return rd8(morepl::plyDesc(s)) != 4; };
    if (guestCall(c, 0x8000AD48) & 0xFF) {  // CityTrial_IsInStadium
        const s32 g = s8(guestCall(c, 0x8000AE74) & 0xFF);  // its group
        if (g == 4 || g == 5 || g == 7) {
            c.r[3] = n >= 0 && u32(n) < slots && present(u32(n)) ? u32(n) : 5;
            return;
        }
    }
    s32 rank[16];
    for (u32 s = 0; s < slots; s++) rank[s] = s8(rd8(morepl::at(morepl::k_race_rank, s)));
    for (u32 pass = 1; pass < slots; pass++)  // (the game's: a later rider with the same rank moves down one)
        for (u32 i = 0; i < slots; i++)
            if (present(i))
                for (u32 j = i + 1; j < slots; j++)
                    if (present(j) && rank[i] == rank[j]) rank[j]++;
    for (u32 s = 0; s < slots; s++)
        if (present(s) && rank[s] == n) {
            c.r[3] = s;
            return;
        }
    c.r[3] = 5;
}
// Machines (f_801DC80C, f_801CE778; r3 = the machine's data): a machine whose ground contact (+1B4C) is not a
// number is logged (its rider, kind, position) - finding why, while riders 6-9 are brought up.
void machineCheck(Ctx& c, u32 entry) {
    const u32 md = c.r[3];
    const float p = flt(rd32(md + 0x1B4C));
    if (std::isnan(p)) {
        static int s_n = 0;
        if (s_n++ < 12) {
            const u32 rider = rd32(md + 4);
            const u32 rd = rider ? rd32(rider + 0x2C) : 0;
            log("[8p] machine %08X kind %u rider %d: ground contact %d/NaN; pos %.1f %.1f %.1f status %u\n", md, rd8(md + 0x24),
                rd ? int(rd8(rd + 8)) : -1, s32(rd32(md + 0x1B48)), flt(rd32(md + 0x3E8)), flt(rd32(md + 0x3EC)),
                flt(rd32(md + 0x3F0)), rd32(md + 0x30));
        }
    }
    interpOriginal(c, entry);
}
}  // namespace

namespace morepl {
// SA2_8P_DEBUG (tests): every 2 s, each slot's kind, position and up vector (the player table).
// SA2_8P_DEBUG: the per-slot race and Stadium arrays as the results scene (0xD) begins - in the 8-player build both
// the moved storage and the game's own (what code not yet moved still writes there).
void resultsDump() {
    const Moved* arrays[] = {&k_race_rank, &k_race_84D, &k_race_852, &k_race_857, &k_race_868, &k_race_87C, &k_race_890,
                             &k_race_speed, &k_race_8B8, &k_race_finished, &k_race_A38, &k_race_A4C, &k_race_A60, &k_race_A74,
                             &k_st_placement, &k_st_BCC, &k_st_time, &k_st_BE0, &k_st_BF0, &k_st_C00, &k_st_points, &k_st_distance,
                             &k_st_CC8, &k_st_CCC, &k_st_CD0, &k_res_2C8, &k_res_2CC, &k_res_2D0, &k_res_2DB, &k_res_2E8, &k_res_2EC,
                             &k_res_2F0, &k_res_2F4, &k_res_2F8, &k_res_308, &k_res_318, &k_res_328, &k_res_32C};
    for (const Moved* m : arrays) {
        for (int moved = 0; moved < (active() ? 2 : 1); moved++) {
            char b[400];
            int k = std::snprintf(b, sizeof b, "[8p] results %08X x%X %s:", m->addr, m->stride, moved || !active() ? "game" : "moved");
            const u32 base = moved || !active() ? m->addr : m->to, n = moved || !active() ? m->oldCount : m->count;
            for (u32 i = 0; i < n; i++) {
                const u32 a = base + i * m->stride;
                if (m->stride == 1) k += std::snprintf(b + k, sizeof b - size_t(k), " %d", s8(rd8(a)));
                else if (m->stride == 2) k += std::snprintf(b + k, sizeof b - size_t(k), " %d", s16(rd16(a)));
                else k += std::snprintf(b + k, sizeof b - size_t(k), " %d(%g)", s32(rd32(a)), double(flt(rd32(a))));
            }
            log("%s\n", b);
        }
    }
}

// The game's file preload table (80550F68: 120 entries of 0x28 at +104 - state +0 (1 queued, 2 loading, 3 loaded,
// ...), heap +2, file +6, +8; the heap loading now at +13C4): what a load waits on, when the game hangs.
void stallDump() {
    if (!active()) return;
    constexpr u32 kPre = 0x80550F68;
    log("[8p] preload: heap now %d\n", s32(rd32(kPre + 0x13C4)));
    for (u32 i = 0; i < 0x78; i++) {
        const u32 e = kPre + 0x104 + i * 0x28;
        if (!rd8(e)) continue;
        char b[300];
        int k = std::snprintf(b, sizeof b, "[8p]   %3u: state %d heap %d file %d n %d |", i, s8(rd8(e)), s8(rd8(e + 2)), s16(rd16(e + 6)),
                              s16(rd16(e + 8)));
        for (u32 o = 0xC; o < 0x28; o += 4) k += std::snprintf(b + k, sizeof b - size_t(k), " %08X", rd32(e + o));
        log("%s\n", b);
    }
}

void frame() {
    // High Jump / Air Glider: the opponents' list beside the view (3DHud_CreateHighJumpOpponentHeightElement) puts
    // row n at -6 - 5n (one view; sdata2 805E002C, read only there): with 7 opponents the last rows fall off the
    // bottom. With more than 4 riders the list goes up from the first row instead, 3 apart, into the space above it.
    const int inRace = rd8(0x805359D8 + 0xA95);
    const bool many = inRace > 4;
    const u32 want = many ? 0xC0400000u : 0x40A00000u;  // -3.0 / 5.0
    if (rd32(0x805E002C) != want) wr32(0x805E002C, want);
    static u8 s_out[10] = {};
    if (!many || rd8(0x805359D8 + 0x7D7) != 0x12) {
        std::memset(s_out, 0, sizeof s_out);
        return;
    }
    // The list's rows (3D +C88 by slot; f_8012B9BC numbers them) reach only some opponents with more than 4: the
    // others stay on row 0, under another. Each rider without a view gets its own, in slot order.
    int row = 0;
    for (int i = 0; i < riders(); i++) {
        const u32 slot = riderSlot(i);
        if (rd8(at(k_view_desc, slot) + 1) == 1) continue;  // (has a view: the list is the others)
        if (rd8(at(k_hud_C88, slot)) != u32(row)) wr8(at(k_hud_C88, slot), u8(row));
        row++;
    }
    // A rider out of the race (gm +A30 set: fallen, or out of time) is placed from the bottom: Game_Think counts those
    // out before them in slots 0-3 only (unrolled), so one after a rider of slots 6-9, or the fifth, repeats a place.
    // Each new one is placed again here, counting all of them.
    int outBefore = 0;
    for (int i = 0; i < riders(); i++) outBefore += s_out[riderSlot(i)] ? 1 : 0;
    for (int i = 0; i < riders(); i++) {
        const u32 slot = riderSlot(i);
        const bool out = rd8(at(k_race_finished, slot)) != 0;
        if (out && !s_out[slot]) wr8(at(k_race_rank, slot), u8(std::max(0, inRace - outBefore - 1)));
        if (out && !s_out[slot]) outBefore++;
        s_out[slot] = out ? 1 : 0;
    }
}

void debugFrame() {
    static const bool s_on = std::getenv("SA2_8P_DEBUG") != nullptr;
    static u32 s_n = 0;
    if (s_on) {  // (the results scene's arrays, once, half a second in; the machine stats' at their screen)
        static u32 s_inResults = 0, s_inStats = 0;
        s_inResults = rd8(0x805359D8 + 0x7D7) == 0x0D ? s_inResults + 1 : 0;
        if (s_inResults == 30) resultsDump();
        s_inStats = rd8(0x805359D8 + 0x7D7) == 0x10 ? s_inStats + 1 : 0;
        // SA2_8P_MEMDUMP=<file>: MEM1 (24 MiB) as the stats screen shows, for finding what it shows (tests)
        if (const char* f = std::getenv("SA2_8P_MEMDUMP"); f && s_inStats == 90)
            if (std::FILE* o = std::fopen(f, "wb")) {
                for (u32 a = 0x80000000u; a < 0x81800000u; a += 4) {
                    const u32 w = rd32(a);
                    const u8 b[4] = {u8(w >> 24), u8(w >> 16), u8(w >> 8), u8(w)};
                    std::fwrite(b, 1, 4, o);
                }
                std::fclose(o);
                log("[8p] memory written to %s\n", f);
            }
        if (s_inStats == 30)
            for (u32 i = 0; i < count(k_ct_ply_stats); i++) {
                char b[300];
                int k = std::snprintf(b, sizeof b, "[8p] stats slot %u:", i);
                for (u32 o = 0; o < 0x24; o += 4) k += std::snprintf(b + k, sizeof b - size_t(k), " %g", double(flt(rd32(at(k_ct_ply_stats, i) + o))));
                k += std::snprintf(b + k, sizeof b - size_t(k), " | machine %u bike %u", rd8(at(k_ct_machine, i)), rd8(at(k_ct_is_bike, i)));
                log("%s\n", b);
                if (i < count(k_ply_D20)) {  // (gm +D20: 9 bytes a player)
                    char c3[120];
                    int k3 = std::snprintf(c3, sizeof c3, "[8p] D20 slot %u:", i);
                    for (u32 n = 0; n < 9; n++) k3 += std::snprintf(c3 + k3, sizeof c3 - size_t(k3), " %d", s8(rd8(at(k_ply_D20, i) + n)));
                    log("%s\n", c3);
                }
                if (i < count(k_ptab)) {  // (items collected: the player stats' counts at +4C8)
                    char c2[600];
                    int k2 = std::snprintf(c2, sizeof c2, "[8p] items slot %u:", i);
                    for (u32 n = 0; n < 0x40; n++)
                        if (const s32 v = s32(rd32(at(k_ptab, i) + 0x4C8 + 4 * n))) k2 += std::snprintf(c2 + k2, sizeof c2 - size_t(k2), " %u=%d", n, v);
                    log("%s\n", c2);
                }
            }
    }
    if (!s_on || ++s_n % 120) return;
    for (u32 pad : {1u, 6u, 7u, 8u}) {  // HSD's pads (8058B634, 0x44 each): the riders' input
        const u32 a = 0x8058B634 + pad * 0x44;
        char b[300];
        int k = std::snprintf(b, sizeof b, "[8p]   pad %u:", pad);
        for (u32 o = 0; o < 0x44; o += 4) k += std::snprintf(b + k, sizeof b - size_t(k), " %08X", rd32(a + o));
        log("%s\n", b);
    }
    {  // the views (gm +BB8: view, has one; 3 a slot) and the 3D HUD's view per slot (3D +1)
        char b[300];
        int k = std::snprintf(b, sizeof b, "[8p] view_desc / hud view:");
        for (u32 i = 0; i < count(k_view_desc); i++)
            k += std::snprintf(b + k, sizeof b - size_t(k), " %u:%d,%d/%d", i, s8(rd8(at(k_view_desc, i))), s8(rd8(at(k_view_desc, i) + 1)),
                               s8(rd8(at(k_hud_001, i))));
        log("%s\n", b);
    }
    for (u32 i = 0; i < count(k_ptab); i++) {
        const u32 p = at(k_ptab, i);
        log("[8p] slot %u kind %u ctrl %d pos %.1f %.1f %.1f fwd %.2f %.2f %.2f up %.2f %.2f %.2f\n", i, rd32(p), s8(rd8(p + 0x8D)),
            flt(rd32(p + 8)), flt(rd32(p + 0xC)), flt(rd32(p + 0x10)), flt(rd32(p + 0x14)), flt(rd32(p + 0x18)), flt(rd32(p + 0x1C)),
            flt(rd32(p + 0x20)), flt(rd32(p + 0x24)), flt(rd32(p + 0x28)));
        if (const u32 mg = rd32(p + 0x40); rd32(p) != 4 && mg && (mg >> 24) == 0x81) {  // its machine (gobj +2C: data, +3E8 pos)
            const u32 md = rd32(mg + 0x2C);
            if ((md >> 24) == 0x81)
                log("[8p]   machine %08X pos %.1f %.1f %.1f\n", md, flt(rd32(md + 0x3E8)), flt(rd32(md + 0x3EC)), flt(rd32(md + 0x3F0)));
        }
        if ((i == 1 || i == 6) && rd32(p) != 4 && false) {  // riders 1 and 6: stats (+44, 9 floats; +68, 9 more), setup's (+C)
            char b[400];
            int k = std::snprintf(b, sizeof b, "[8p]   stats");
            for (u32 o = 0x44; o < 0x8C; o += 4) k += std::snprintf(b + k, sizeof b - size_t(k), " %g", flt(rd32(p + o)));
            k += std::snprintf(b + k, sizeof b - size_t(k), " | desc");
            const u32 d = at(k_ply_desc, i);
            for (u32 o = 0xC; o < 0x30; o += 4) k += std::snprintf(b + k, sizeof b - size_t(k), " %08X", rd32(d + o));
            log("%s\n", b);
        }
    }
    {
        char b[300];
        int k = std::snprintf(b, sizeof b, "[8p] finished/placement/rank:");
        for (u32 i = 0; i < k_race_finished.count; i++)
            k += std::snprintf(b + k, sizeof b - size_t(k), " %u:%u/%u/%u", i, rd8(at(k_race_finished, i)), rd8(at(k_st_placement, i)),
                               rd8(at(k_race_rank, i)));
        log("%s (scene %02X, stadium kind %u, riders %u)\n", b, rd8(0x805359D8 + 0x7D7), rd8(0x805359D8 + 0x5AD), rd8(0x805359D8 + 0xA95));
        k = std::snprintf(b, sizeof b, "[8p] stadium time/points/distance:");
        for (u32 i = 0; i < k_st_time.count; i++)
            k += std::snprintf(b + k, sizeof b - size_t(k), " %u:%d/%d/%.1f", i, s32(rd32(at(k_st_time, i))), s32(rd32(at(k_st_points, i))),
                               flt(rd32(at(k_st_distance, i))));
        log("%s\n", b);
    }
}

void install() {
    static bool s_done = false;
    if (s_done || !active()) return;
    s_done = true;
    struct R {
        u32 addr, word;  // (a check: the game's instruction there)
        InterpHandler fn;
    };
    const R repl[] = {
        {0x8023239C, 0x9421FFE0, preloadReset},
        {0x8000A370, 0x9421FFE0, gmRiderSpacing},
        {0x800B31D4, 0x9421FFB0, cameraLookAt},
        {0x800B00D8, 0x9421FFE0, camLookupInit},
        {0x800135EC, 0x9421FF90, playerCreate},
        {0x800D0B7C, 0x9421FFE0, startPosition},
        {0x800D4FB8, 0x9421FFE0, respawnSpot},
        {0x800D416C, 0x9421FFD0, surfaceAt},
        {0x8005D64C, 0x9421FFF0, audioTrackFree},
        {0x8024F3B0, 0x9421FFF0, itemRemove},
        {0x8024E918, 0x9421FFF0, itemInit},
        {0x8000A6C4, 0x9421FFD0, riderInPlace},
        {0x801DC80C, 0x9421FFD0, machineCheck},
        {0x801CE778, 0x9421FFE0, machineCheck},
    };
    int n = 0;
    for (const R& r : repl) {
        if (rd32(r.addr) != r.word) {
            log("[morepl] %08X is %08X, not %08X: not replaced\n", r.addr, rd32(r.addr), r.word);
            continue;
        }
        n += interpReplace(0, r.addr, r.fn) ? 1 : 0;
    }
    log("[morepl] 8-player build: %d of %zu functions replaced\n", n, sizeof repl / sizeof repl[0]);
    // SA2_8P_DEBUG: each High Jump / Air Glider list row made (3DHud_CreateHighJumpOpponentHeightElement(view, slot))
    // The labels over the other riders (3DHud_CreateAllPlIcons 801210A8(viewer, view): one for each of slots 4..0 but
    // the viewer's own): room for 4. A viewer in slots 6-9 (online, PCs 5-8) has no own slot among them - a fifth label,
    // its model never loaded (jobj.c: "jobj->child" asserted, the game stopped in Kirby Melee): none for them.
    if (rd32(0x801210A8) == 0x9421FFE0)
        interpReplace(0, 0x801210A8, [](Ctx& c, u32 entry) {
            if (s32(c.r[3]) >= 6) return;
            interpOriginal(c, entry);
        });
    if (std::getenv("SA2_8P_DEBUG") && rd32(0x8012BAE0) == 0x9421FFC0)
        interpReplace(0, 0x8012BAE0, [](Ctx& c, u32 entry) {
            log("[8p] list row: view %d slot %u (from %08X; rows", s32(c.r[3]), c.r[4], c.lr);
            for (u32 i = 0; i < 10; i++) log(" %d", s8(rd8(at(k_hud_C88, i))));
            log(")\n");
            interpOriginal(c, entry);
        });
    if (std::getenv("SA2_8P_DEBUG") && rd32(0x8012C4FC) == 0x9421FFE0)
        interpReplace(0, 0x8012C4FC, [](Ctx& c, u32 entry) {
            log("[8p] list cleared (from %08X)\n", c.lr);
            interpOriginal(c, entry);
        });
    if (std::getenv("SA2_8P_DEBUG") && rd32(0x8012B9BC) == 0x9421FFE0)
        interpReplace(0, 0x8012B9BC, [](Ctx& c, u32 entry) {
            log("[8p] list rows numbered (from %08X)\n", c.lr);
            interpOriginal(c, entry);
        });
}
}  // namespace morepl

}  // namespace rt

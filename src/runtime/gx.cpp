// GX bridge: the game's own (recompiled) GX library writes the hardware command stream to the
// write-gather pipe (0xCC008000). This file parses that stream command by command and hands it to
// Aurora's command processor, rewriting the commands that carry console addresses into Aurora's
// pointer-carrying GX_AURORA forms:
//   CP 0xA0-0xAF array bases      -> GX_AURORA_LOAD_ARRAYBASE (host pointer into guest memory)
//   BP image3 (texture address)   -> GX_AURORA_LOAD_TEXOBJ (content-hashed by Aurora)
//   BP 0x64/0x65 TLUT loads       -> GX_AURORA_LOAD_TLUT
//   BP 0x49/0x4A/0x4B/0x4D + 0x52 -> GX_AURORA_LOAD_COPY_SRC/DST/DEST + the copy
//   CALL_DL                       -> the display list's commands, inline
// Draw-done (BP 0x45) and token (BP 0x47/0x48) raise the PE interrupts as soon as they are written.
// While the CPU FIFO is redirected away from the GP FIFO (GXBeginDisplayList), writes go to guest
// memory at the PI write pointer instead, like the hardware.
// Frames are handed to the main (render) thread at VISetNextFrameBuffer, each standing alone: it
// starts with the whole GX state as it began (a preamble), and records the values that move from
// frame to frame, named by the game object that drew them, so the renderer can draw in-between
// frames (src/runtime/gxinterp.cpp).
#include "runtime/gx.h"
#include "runtime/morepl.h"
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <unordered_map>
#include <string>
#include <thread>
#include <vector>

namespace rt {

extern std::atomic<u64> g_relLinkFrame[256];  // src/runtime/dispatch.cpp
u32 mmioGet16(u32 off);
u32 mmioGet32(u32 off);
u64 profileGuestCycles();  // src/runtime/profile.cpp
void profileLogThreads(double hz);
double viLastRetrace();  // src/runtime/hle_hw.cpp
double s_frameStartVi = 0;  // the retrace that started the frame being built (gxFrameStarts)
int s_staticVertices = 0;   // gxStaticVerticesEnter depth
int s_heightField = 0;      // gxHeightFieldEnter depth
u64 viRetraces();
inline u64 viRetraceCount() { return viRetraces(); }
void mmioSet16(u32 off, u32 v);

namespace {
enum : u8 {
    CMD_NOP = 0x00,
    CMD_LOAD_CP = 0x08,
    CMD_LOAD_XF = 0x10,
    CMD_INDX_A = 0x20,
    CMD_INDX_B = 0x28,
    CMD_INDX_C = 0x30,
    CMD_INDX_D = 0x38,
    CMD_CALL_DL = 0x40,
    CMD_METRICS = 0x44,
    CMD_INVAL_VC = 0x48,
    CMD_AURORA = 0x50,
    CMD_LOAD_BP = 0x61,
};
enum : u16 {
    AUR_ARRAYBASE = 0x0010,
    AUR_LOAD_TEXOBJ = 0x0030,
    AUR_LOAD_TLUT = 0x0031,
    AUR_COPY_SRC = 0x0035,
    AUR_COPY_DST = 0x0036,
    AUR_COPY_DEST = 0x0037,
};
enum : u32 { IRQ_PE_TOKEN = 18, IRQ_PE_FINISH = 19 };

// ---- output stream (Aurora format, big-endian) ----
std::vector<u8> s_out;
void put8(u32 v) { s_out.push_back(u8(v)); }
void put16(u32 v) {
    s_out.push_back(u8(v >> 8));
    s_out.push_back(u8(v));
}
void put32(u32 v) {
    s_out.push_back(u8(v >> 24));
    s_out.push_back(u8(v >> 16));
    s_out.push_back(u8(v >> 8));
    s_out.push_back(u8(v));
}
void put64(u64 v) {
    put32(u32(v >> 32));
    put32(u32(v));
}
void putBytes(const u8* p, size_t n) { s_out.insert(s_out.end(), p, p + n); }
void aurora(u16 sub) {
    put8(CMD_AURORA);
    put16(sub);
}
u64 hostAddr(u32 physAddr) { return u64(reinterpret_cast<uintptr_t>(hostp(0x80000000u | (physAddr & 0x01FFFFFF)))); }

// ---- CP vertex format state (for command lengths) ----
u32 s_vcdLo = 0, s_vcdHi = 0, s_vatA[8], s_vatB[8], s_vatC[8];
const u8 kComp[8] = {1, 1, 2, 2, 4, 0, 0, 0};
const u8 kClr[8] = {2, 3, 4, 2, 3, 4, 0, 0};
u32 vertexSize(u32 fmt) {
    const u32 lo = s_vcdLo, hi = s_vcdHi, a = s_vatA[fmt], b = s_vatB[fmt], c = s_vatC[fmt];
    u32 size = 0;
    for (u32 i = 0; i < 9; i++) size += (lo >> i) & 1;  // position/normal + 8 texture matrix indices
    auto attr = [&](u32 type, u32 direct) { size += type == 1 ? direct : type == 2 ? 1 : type == 3 ? 2 : 0; };
    attr((lo >> 9) & 3, ((a & 1) ? 3 : 2) * kComp[(a >> 1) & 7]);
    if (u32 t = (lo >> 11) & 3) {
        const bool nbt = (a >> 9) & 1;
        if (t == 1) size += (nbt ? 9 : 3) * kComp[(a >> 10) & 7];
        else size += (t == 2 ? 1 : 2) * ((nbt && (a >> 31)) ? 3 : 1);
    }
    attr((lo >> 13) & 3, kClr[(a >> 14) & 7]);
    attr((lo >> 15) & 3, kClr[(a >> 18) & 7]);
    const u32 tex[8][2] = {{(a >> 21) & 1, (a >> 22) & 7}, {b & 1, (b >> 1) & 7},          {(b >> 9) & 1, (b >> 10) & 7},
                           {(b >> 18) & 1, (b >> 19) & 7}, {(b >> 27) & 1, (b >> 28) & 7}, {(c >> 5) & 1, (c >> 6) & 7},
                           {(c >> 14) & 1, (c >> 15) & 7}, {(c >> 23) & 1, (c >> 24) & 7}};
    for (u32 i = 0; i < 8; i++) attr((hi >> (2 * i)) & 3, (tex[i][0] ? 2 : 1) * kComp[tex[i][1]]);
    return size;
}

u32 s_arraysSet = 0;  // CP arrays whose base has been loaded (bit = array index)

// Debugging: SA2_SKIP_DRAWS=i,j,... drops those draws (counted from 0 in every frame).
u32 s_drawIndex = 0;
const std::set<u32> s_skipDraws = [] {
    std::set<u32> s;
    for (const char* p = std::getenv("SA2_SKIP_DRAWS"); p && *p;) {
        char* end;
        s.insert(u32(std::strtoul(p, &end, 10)));
        p = *end == ',' ? end + 1 : end;
        if (end == p && *p) break;
    }
    return s;
}();

// ---- snapshots ----
// Aurora reads array data when the render thread processes the frame, after the game has moved on
// (and, since draw-done is signalled at once, possibly rewritten its per-frame vertex and matrix
// buffers). So the arrays a draw uses are copied when the draw is issued, into memory that lives
// until the frame has been rendered.
struct Arena {
    std::vector<std::unique_ptr<u8[]>> chunks;
    size_t used = 0, cap = 0;
    u8* alloc(size_t n) {
        n = (n + 31) & ~size_t(31);
        if (used + n > cap) {
            cap = std::max<size_t>(n, size_t(1) << 20);
            chunks.emplace_back(new u8[cap]);
            used = 0;
        }
        u8* p = chunks.back().get() + used;
        used += n;
        return p;
    }
};
Arena s_arena;  // snapshots of the frame being built
// What in-between frames need (patches) is recorded only when they are on (gxSetRecording);
// SA2_NO_RECORD: never (measuring what recording costs).
bool s_noRecord = std::getenv("SA2_NO_RECORD") != nullptr;

u32 s_xf[0x1058];  // XF registers and matrix memory, as the game wrote them
u32 s_bp[256];     // BP registers, as the game wrote them

// ---- tags: what drew each moving value ----
// The task system's display loop (f_80071998, children f_80071900) calls each task's display
// function after storing it at r13-0x7878; Ctx::call sees a call of that function and reports the
// task (r3) here. A value is named by the task, its display function, which time this frame the
// task is displayed, the viewport (split screen draws the world twice), and what it is used to draw:
// the model part of the draw that follows it (its display list, or its position array and first
// vertex), and which time in the task that part is drawn. Model parts the CPU builds every frame
// (sprites, skinned meshes) have no lasting address: those values are named by their place in
// what the task drew instead (also every value's fallback name). Values drawn outside any task
// count as one task of their own.
struct TaskScope {
    u32 task, fn, occurrence;
    u32 seq[3];  // per kind group: XF loads, vertices sent with draws, vertex arrays
    std::unordered_map<u64, u32> parts;  // model part -> times drawn so far
};
std::vector<TaskScope> s_tasks;  // innermost last
TaskScope s_noTask{0, 0, 0, {0, 0, 0}, {}};
u64 viewKey() {  // the viewport and scissor box (split screen views differ in at least one)
    u64 h = 0;
    for (u32 a = 0x101A; a < 0x1020; a++) h = h * 0x100000001B3ull ^ s_xf[a];
    for (u32 r : {0x20u, 0x21u, 0x59u}) h = h * 0x100000001B3ull ^ s_bp[r];
    return h;
}
std::unordered_map<u64, u32> s_occurrences;  // task << 32 | fn -> times displayed this frame
u64 mix(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ (x >> 33);
}
u64 scopeKey(const TaskScope& t) { return mix(mix(u64(t.task) << 32 | t.fn) ^ u64(t.occurrence) << 40 ^ viewKey()); }
u64 tag(int group, u32 detail) {
    TaskScope& t = s_tasks.empty() ? s_noTask : s_tasks.back();
    const u32 seq = t.seq[group]++;
    return mix(scopeKey(t) ^ (u64(group) << 32 | seq) ^ u64(detail) << 36);
}

std::shared_ptr<GxFrame> s_frame = std::make_shared<GxFrame>();  // the frame being built
std::vector<size_t> s_pendingXf;  // XF values loaded since the last draw (named by it)
std::vector<size_t> s_drawXf;     // those of the draw being translated
bool s_draw2D = false;            // the draw being translated is 2D (place2D)
bool s_hideDraw = false;          // ... and lies wholly off the 4:3 screen, where the game keeps it hidden
bool s_clipDraw = false;          // ... is cut at the 4:3 screen's sides, as the game's screen cut it
float s_drawBox[4];               // ... where it lands on the 4:3 screen (x0, x1, y0, y1; place2D)
u64 s_drawPart = 0;               // the draw being translated: its model part's name (0: none)
u64 s_drawGroup = 0;              // ... without the copy number
u32 s_drawOcc = 0;                // ... the copy number
u32 s_curDl = 0;                  // the display list being inlined (0: none)
// Display lists recorded under a scope (gxHeightFieldEnter...): played under it.
struct ScopedDl {
    u32 lo, hi;
    bool height, staticV;
};
std::vector<ScopedDl> s_scopedDls;  // lists recorded under a scope (the latest at each address)

// ---- vertex/matrix arrays ----
// The hardware has no array sizes, but Aurora copies each array a draw reads into a per-frame
// storage buffer, so a base is sent only once a draw uses it, as a snapshot of the extent the draws
// have reached so far; it is resent larger (doubling) when a later draw indexes further. Every frame
// starts over (s_arraySent = 0). A base the game sets again in the same frame (a model's arrays, set
// again for each of its parts) gets the snapshot it already has: copying them again for each part
// cost the game thread and Aurora (which copies each new snapshot to the GPU) milliseconds a frame in
// busy scenes. (The hardware reads arrays when it runs the frame, after the CPU has written them.)
u32 s_arrayBase[16], s_arrayStride[16], s_arraySent[16];
struct ArraySnap {
    u8* copy;
    u32 size;
};
std::unordered_map<u64, ArraySnap> s_arraySnaps;  // (slot, stride, base) -> this frame's snapshot
u8* s_arrayCopy[16];   // the current base's snapshot (none yet: null)
u8* s_arrayShown[16];  // the snapshot Aurora has for the slot

u64 arraySnapKey(u32 i) { return u64(i) << 56 | u64(s_arrayStride[i]) << 32 | (s_arrayBase[i] & 0x01FFFFFF); }

// The slot's base or stride changed: its snapshot this frame, if it has one.
void arrayBaseChanged(u32 i) {
    const auto it = s_arraySnaps.find(arraySnapKey(i));
    s_arrayCopy[i] = it != s_arraySnaps.end() ? it->second.copy : nullptr;
    s_arraySent[i] = it != s_arraySnaps.end() ? it->second.size : 0;
}

// Points Aurora's slot at a snapshot.
void sendArray(u32 i, u8* copy, u32 size, u32 vat) {
    aurora(u16(AUR_ARRAYBASE | i));
    if (i == 0 && s_arrayStride[0] && !s_noRecord) {  // positions: the CPU rewrites some every frame (skinned models)
        GxPatch p{};
        p.kind = GxPatch::Array, p.at = u32(s_out.size()), p.count = size, p.stride = s_arrayStride[0], p.data = copy;
        p.fmt = u8((vat >> 1) & 7), p.comps = u8((vat & 1) ? 3 : 2), p.shift = u8((vat >> 4) & 31);
        p.key2 = tag(2, p.stride << 8 | p.fmt);
        p.key = s_drawPart ? mix(s_drawPart ^ 0xA77A7) : p.key2;
        p.group = s_drawGroup, p.occ = s_drawOcc;
        p.flat = s_staticVertices > 0 && !s_heightField;
        p.height = s_heightField > 0;
        s_frame->patches.push_back(p);
    }
    put64(u64(reinterpret_cast<uintptr_t>(copy)));
    put32(size);
    put8(0);  // big-endian
    s_arrayShown[i] = copy;
}

// `vat`: the draw's VAT A word (array 0, positions: what the renderer needs to blend them).
void needArray(u32 i, u32 bytes, u32 vat = 0) {
    if (bytes <= s_arraySent[i]) {
        if (s_arrayShown[i] != s_arrayCopy[i]) sendArray(i, s_arrayCopy[i], s_arraySent[i], vat);  // (back to it)
        return;
    }
    const u32 limit = kHostMemSize - (s_arrayBase[i] & 0x01FFFFFF);
    const u32 size = std::min((std::max(bytes, s_arraySent[i] * 2) + 63) & ~63u, limit);
    s_arraySent[i] = size;
    u8* copy = s_arena.alloc(size);
    std::memcpy(copy, hostp(0x80000000u | (s_arrayBase[i] & 0x01FFFFFF)), size);
    s_arrayCopy[i] = copy;
    s_arraySnaps[arraySnapKey(i)] = {copy, size};
    sendArray(i, copy, size, vat);
}

// Reads the indices of a draw's vertices and makes sure every array they index is sent big enough.
void sizeArrays(const u8* p, u32 count, u32 fmt) {
    const u32 lo = s_vcdLo, hi = s_vcdHi, a = s_vatA[fmt], b = s_vatB[fmt], c = s_vatC[fmt];
    struct Op {
        u8 skip, idxSize, array, elem;
    };
    Op ops[24];
    u32 nops = 0, mtx = 0;
    for (u32 i = 0; i < 9; i++) mtx += (lo >> i) & 1;
    if (mtx) ops[nops++] = {u8(mtx), 0, 0, 0};
    auto attr = [&](u32 type, u32 array, u32 elem, u32 indices = 1) {
        if (type == 1) ops[nops++] = {u8(elem), 0, 0, 0};
        else if (type >= 2)
            for (u32 k = 0; k < indices; k++) ops[nops++] = {0, u8(type == 2 ? 1 : 2), u8(array), u8(elem)};
    };
    attr((lo >> 9) & 3, 0, ((a & 1) ? 3 : 2) * kComp[(a >> 1) & 7]);
    const bool nbt = (a >> 9) & 1, nbt3 = nbt && (a >> 31);
    const u32 nrmType = (lo >> 11) & 3;
    if (nrmType == 1) attr(1, 1, (nbt ? 9 : 3) * kComp[(a >> 10) & 7]);
    else attr(nrmType, 1, (nbt && !nbt3 ? 9 : 3) * kComp[(a >> 10) & 7], nbt3 ? 3 : 1);
    attr((lo >> 13) & 3, 2, kClr[(a >> 14) & 7]);
    attr((lo >> 15) & 3, 3, kClr[(a >> 18) & 7]);
    const u32 tex[8][2] = {{(a >> 21) & 1, (a >> 22) & 7}, {b & 1, (b >> 1) & 7},          {(b >> 9) & 1, (b >> 10) & 7},
                           {(b >> 18) & 1, (b >> 19) & 7}, {(b >> 27) & 1, (b >> 28) & 7}, {(c >> 5) & 1, (c >> 6) & 7},
                           {(c >> 14) & 1, (c >> 15) & 7}, {(c >> 23) & 1, (c >> 24) & 7}};
    for (u32 i = 0; i < 8; i++) attr((hi >> (2 * i)) & 3, 4 + i, (tex[i][0] ? 2 : 1) * kComp[tex[i][1]]);

    static const bool s_check = std::getenv("SA2_GX_CHECK") != nullptr;
    if (s_check) {
        u32 total = 0;
        for (u32 k = 0; k < nops; k++) total += ops[k].skip ? ops[k].skip : ops[k].idxSize;
        static int s_bad = 0;
        if (total != vertexSize(fmt) && s_bad++ < 10)
            log("[gx] layout mismatch: ops %u bytes, vertexSize %u (fmt %u VCD %08X/%08X VAT %08X %08X %08X)\n", total,
                vertexSize(fmt), fmt, lo, hi, a, b, c);
    }
    s32 maxIdx[12];
    u32 elemOf[12] = {};
    for (s32& m : maxIdx) m = -1;
    for (u32 v = 0; v < count; v++) {
        for (u32 k = 0; k < nops; k++) {
            const Op& o = ops[k];
            if (o.skip) {
                p += o.skip;
                continue;
            }
            const u32 idx = o.idxSize == 1 ? p[0] : u32(p[0]) << 8 | p[1];
            p += o.idxSize;
            // An all-ones position index makes the hardware skip the vertex; it reads nothing.
            if (o.array == 0 && idx == (o.idxSize == 1 ? 0xFFu : 0xFFFFu)) continue;
            if (s32(idx) > maxIdx[o.array]) maxIdx[o.array] = s32(idx);
            elemOf[o.array] = o.elem;
        }
    }
    for (u32 i = 0; i < 12; i++)
        if (maxIdx[i] >= 0) needArray(i, u32(maxIdx[i]) * s_arrayStride[i] + elemOf[i], a);
}

// Indexed attributes in the current vertex descriptor whose array was never given a base.
u32 missingArrays() {
    u32 need = 0;
    for (u32 i = 0; i < 4; i++)  // position, normal, color0, color1
        if (((s_vcdLo >> (9 + 2 * i)) & 3) >= 2) need |= 1u << i;
    for (u32 i = 0; i < 8; i++)
        if (((s_vcdHi >> (2 * i)) & 3) >= 2) need |= 1u << (4 + i);
    return need & ~s_arraysSet;
}

// ---- BP state the translation needs ----
u32 s_texImage0[8], s_texMode1[8], s_tlutRegion[8];
u32 s_tlutSrc = 0;
u32 s_copySrcTL = 0, s_copySrcWH = 0, s_copyDest = 0, s_copyStride = 0;
u64 s_frames = 0;
u32 s_displayCopies = 0;  // display copies since the last frame handoff
bool s_framesByCopy = false;  // frames end at their display copy (gxFramesByDisplayCopy)
bool s_endAtDraw = false;     // ... a display copy was made: the next draw starts the next frame

int texMapOf(u32 reg, u32 base) {  // maps 0-3 at base 0x80.., 4-7 at 0xA0..
    if (reg >= 0x80 + base && reg < 0x84 + base) return int(reg - 0x80 - base);
    if (reg >= 0xA0 + base && reg < 0xA4 + base) return int(reg - 0xA0 - base + 4);
    return -1;
}

u32 texBytes(u32 fmt, u32 w, u32 h) {
    // Block sizes (width x height of a 32-byte tile) per GX texture format.
    u32 bw = 8, bh = 8, bytes = 32;
    switch (fmt) {
    case 0x0: case 0x8: case 0xE: bw = 8; bh = 8; break;          // I4, C4, CMPR
    case 0x1: case 0x2: case 0x9: bw = 8; bh = 4; break;          // I8, IA4, C8
    case 0x3: case 0x4: case 0x5: case 0xA: bw = 4; bh = 4; break; // IA8, RGB565, RGB5A3, C14X2
    case 0x6: bw = 4; bh = 4; bytes = 64; break;                   // RGBA8
    default: break;
    }
    return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
}

// TLUT loads, by where they went in TMEM (512-byte units past 0x80000, as the TLUT registers name
// it). Games place their palettes anywhere - not only in the SDK's default regions (GX_TLUT0-15 at
// 0xC0000 + 0x2000*n): Billy Hatcher's menus pack them a unit apart, and a fixed slot per default
// region gave many textures another's palette. A texture's palette is the load that last covered
// its TLUT address; Aurora gets it in the texture map's own TLUT slot as the texture is bound.
// The palette is copied when it is loaded, as the hardware copies it into TMEM: games reuse the
// buffer they load palettes from (Billy Hatcher's menus load one palette after another from the
// same place), and the renderer reads a frame late - later still with in-between frames - so read
// in place every palette would come out as the last one written there. A palette stays in TMEM
// across frames: each frame that uses it gets its own copy (snapshot arena).
struct TlutLoad {
    std::vector<u8> data;  // the entries as loaded (big-endian 16-bit)
    u32 count = 0;         // entries
};
TlutLoad s_tlutAt[0x400];
u32 tlutUnits(u32 count) { return (count * 2 + 511) / 512; }

void tlutLoaded(u32 at, u32 src, u32 count) {
    // a load overwrites whatever started inside the TMEM it covers
    for (u32 a = at; a < std::min<u32>(at + tlutUnits(count), 0x400); a++) s_tlutAt[a].count = 0;
    TlutLoad& l = s_tlutAt[at];
    const u8* p = reinterpret_cast<const u8*>(uintptr_t(hostAddr(src)));
    l.data.assign(p, p + count * 2);
    l.count = count;
}

void emitTlut(u32 slot, u32 at, u32 format) {
    for (u32 a = at + 1, back = 0; a-- > 0 && back < 64; back++) {
        const TlutLoad& l = s_tlutAt[a];
        if (!l.count || a + tlutUnits(l.count) <= at) continue;
        const u32 skip = (at - a) * 256;  // entries before this address in a larger load
        u8* copy = s_arena.alloc((l.count - skip) * 2);
        std::memcpy(copy, l.data.data() + skip * 2, (l.count - skip) * 2);
        aurora(AUR_LOAD_TLUT);
        put8(slot);
        put64(u64(reinterpret_cast<uintptr_t>(copy)));
        put32(format);
        put16(l.count - skip);
        put32(0);  // object id 0: content-hashed
        put32(0);
        return;
    }
}

// The mode select's LAN logo, redrawn for online play (src/runtime/online_logo.inc: its emblem and
// lettering, swapped for the originals wherever they load, recognised by their content).
#include "runtime/online_logo.inc"
u64 fnv1a(const u8* p, size_t n) {
    u64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
// The race's place badge with more than 4 riders: its art has 1-4 (32x40 CI8 each, with its own palette), so for
// 5th-8th the "4" (recognised by its content) is drawn with src/runtime/place_digits.inc's digits, made in its style
// for its palette. The place is the one rider with a view's (gm +BB8: view, 1 when it has one; 3 a slot), in a stage.
#include "runtime/place_digits.inc"
constexpr u64 kPlace4Hash = 0xbe3dfaa006666f3eull;
int localPlace() {
    if (!morepl::active() || rd8(0x805359D8 + 0x7D7) != 0x12) return 0;
    int place = 0, views = 0;
    for (int i = 0; i < morepl::riders(); i++) {
        const u32 slot = morepl::riderSlot(i);
        if (rd8(morepl::at(morepl::k_view_desc, slot) + 1) != 1) continue;  // (has a view: Ply_CheckIfViewOn)
        views++;
        place = rd8(morepl::at(morepl::k_race_rank, slot)) + 1;
    }
    return views == 1 ? std::min(place, int(rd8(0x805359D8 + 0xA95))) : 0;
}

u64 textureSource(u32 image3, u32 fmt, u32 w, u32 h) {
    const u32 addr = 0x80000000u | ((image3 & 0x00FFFFFF) << 5);
    if (fmt == 0x9 && w == 32 && h == 40)
        if (const int place = localPlace(); place >= 5 && place <= 8 && fnv1a(hostp(addr), 32 * 40) == kPlace4Hash)
            return u64(reinterpret_cast<uintptr_t>(kPlaceDigits[place - 5]));
    if ((fmt == 0x0 && w == 64 && h == 64) || (fmt == 0x2 && w == 56 && h == 24)) {
        const u64 hash = fnv1a(hostp(addr), texBytes(fmt, w, h));
        if (hash == kLanEmblemHash) return u64(reinterpret_cast<uintptr_t>(kOnlineEmblem));
        if (hash == kLanLetteringHash) return u64(reinterpret_cast<uintptr_t>(kOnlineLettering));
    }
    return hostAddr(addr & 0x01FFFFFF);
}

void loadTexture(int map, u32 image3) {
    const u32 img0 = s_texImage0[map];
    const u32 w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 0xF;
    const u32 maxLod = (s_texMode1[map] >> 8) & 0xFF;
    // CI textures: their palette, in the format their TLUT register names, into this map's slot
    if (fmt == 0x8 || fmt == 0x9 || fmt == 0xA) emitTlut(u32(map), s_tlutRegion[map] & 0x3FF, (s_tlutRegion[map] >> 10) & 3);
    // SA2_TEX_DUMP=<hex address>,... (finding the game's art): each texture at those addresses, once, to
    // tex_<address>_<w>x<h>_f<format>.bin (its raw GX data); SA2_TEX_DUMP=all: every texture, once
    if (static const char* s_texDump = std::getenv("SA2_TEX_DUMP"); s_texDump) {
        static std::set<u32> s_dumped;
        const u32 addr = 0x80000000u | ((image3 & 0x00FFFFFF) << 5);
        char key[16];
        std::snprintf(key, sizeof key, "%08X", addr & 0x01FFFFFF);
        if ((!std::strcmp(s_texDump, "all") || std::strstr(s_texDump, key + 2)) && s_dumped.insert(addr).second) {
            char name[96];
            std::snprintf(name, sizeof name, "tex_%s_%ux%u_f%X.bin", key + 2, w, h, fmt);
            if (std::FILE* f = std::fopen(name, "wb")) {
                std::fwrite(hostp(addr), 1, texBytes(fmt, w, h), f);
                std::fclose(f);
            }
            if (fmt == 0x8 || fmt == 0x9 || fmt == 0xA) {  // (and its palette: <name>.tlut, big-endian entries)
                const u32 at = s_tlutRegion[map] & 0x3FF;
                for (u32 a = at + 1, back = 0; a-- > 0 && back < 64; back++) {
                    const TlutLoad& l = s_tlutAt[a];
                    if (!l.count || a + tlutUnits(l.count) <= at) continue;
                    const u32 skip = (at - a) * 256;
                    std::snprintf(name, sizeof name, "tex_%s_%ux%u_f%X_t%u.tlut", key + 2, w, h, fmt, (s_tlutRegion[map] >> 10) & 3);
                    if (std::FILE* f = std::fopen(name, "wb")) {
                        std::fwrite(l.data.data() + skip * 2, 1, (l.count - skip) * 2, f);
                        std::fclose(f);
                    }
                    break;
                }
            }
        }
    }
    aurora(AUR_LOAD_TEXOBJ);
    put8(u32(map));
    put64(textureSource(image3, fmt, w, h));
    put32(w);
    put32(h);
    put32(fmt);
    put32(u32(map));
    put8(maxLod > 0 ? 1 : 0);
    put32(0);  // object id 0: Aurora caches by content
    put32(0);
    (void)texBytes;
}

// ---- widescreen: which commands draw to the screen, and where 2D draws sit ----
// Aurora (patches/aurora/0004) widens perspective views spanning the screen and keeps everything
// else at its aspect, moved toward the edge it is near. The bridge marks each stretch of commands
// between EFB copies as drawing to the screen (1) or into a texture (0: decided when the copy that
// ends it turns out to be a small texture copy - shadow and reflection passes), and classifies each
// 2D (orthographic) draw by where it lands on the 640-wide logical screen.
enum : u16 { AUR_WIDE_SEGMENT = 0x0060, AUR_WIDE_2D = 0x0061 };
size_t s_segmentAt = SIZE_MAX;  // offset in s_out of the current segment's mode byte
int s_last2D = -1;
u32 s_cpMtxA = 0;           // CP matrix index A: the default position matrix
u32 s_genMode = 0;          // BP GENMODE (number of textures in bits 0-3)
u32 s_tex0 = 0;             // address of the texture in map 0
std::set<u32> s_copyDests;  // texture copy destinations this frame (screen effects draw from them)

void beginSegment() {
    aurora(AUR_WIDE_SEGMENT);
    s_segmentAt = s_out.size();
    put8(1);
    s_last2D = -1;
}

float xff(u32 a) { return __builtin_bit_cast(float, s_xf[a]); }

// Where a draw lands, for widescreen placement: its screen extent (logical units) and eye-space
// depth range. SA2 draws its 2D (HUD, menus, title art) with Ninja's sprite routines under the
// game's perspective projection: flat quads in screen pixels, at a depth given by the sprite's
// priority, through Ninja's 2D matrix [-a 0 320a 0 | 0 a -240a 0 | 0 0 1 0], which takes pixels at
// any depth to the eye space that projects them back onto those pixels. Such sprites and
// orthographic draws are 2D: full-screen art and backdrop tiles cover the screen, fades stretch,
// the HUD goes to the edges, the rest stays centred. Everything else is 3D, placed by Aurora from
// its viewport.
enum : int { WIDE_CENTRE, WIDE_LEFT, WIDE_RIGHT, WIDE_STRETCH, WIDE_3D, WIDE_COVER };
std::set<float> s_backdropDepths[2];  // depths of backdrop tiles in the previous and this frame
std::vector<std::pair<u32, float>> s_bleed;  // the draw's vertices to move out: offset in its vertex data, new x
// Menus in widescreen, by rule (no screen placed by hand): everything stays centred at its size, and a piece that reaches
// a side of the 4:3 screen with a straight edge there - a band, a panel, a bar, a background layer - goes on to the wide
// screen's side: a strip from its edge out, drawn with its outermost column (copies of its edge vertices, moved out:
// colours and texture coordinates as at the edge, so a plain colour continues, a band's pattern runs on, and a shape
// whose edge is transparent adds nothing). The strips' vertex data (triangles), drawn after the draw.
std::vector<u8> s_smear;
u32 s_smearCount = 0;
std::vector<u8> s_smearPos;  // (indexed positions: the strips' own position array, s_arrayStride[0] apart)
u32 s_zmode = 0;  // BP ZMODE (bit 0: depth test)
u64 s_wideLogFrame = ~0ull, s_wideLogTo = 0;  // one frame, or 2D draws over a range of frames
// SA2_WIDE_AUDIT=1 (test runs): every distinct drawing path whose draw is sized to the 4:3 screen - it reaches one of its
// sides, and not far past - but is drawn at 4:3 in widescreen (centred, or 3D seen through the widened view) is logged
// once: the effects that would show the 4:3 screen inside the wide one.
const bool s_wideAudit = std::getenv("SA2_WIDE_AUDIT") != nullptr;

// The game's HUD drawing routines, by the return addresses they leave on the stack (their 2D
// moves to the screen edges in widescreen; everything else 2D stays centred), the routines that
// display a running/paused stage, and HUD pieces that belong to one edge as a whole.
// Kirby Air Ride's race HUD (the render callbacks of its GObjs): 8011502C the timer and the map,
// 80114FD0 the speed meter and charge gauge (and, through the small orthographic camera, the
// tags over the riders - those stay with the 3D), 80120E50 the arrows to riders off the screen.
constexpr u32 kHudReturns[] = {0x8011502C, 0x80114FD0, 0x80120E50};
constexpr u32 kTagReturns[] = {0x80114FD0};  // ... these through the orthographic camera are tags
constexpr u32 kStageDisplayReturns[] = {0xFFFFFFFF};
struct AnchoredRoutine {
    u32 lo, hi;
    int place;
};
// The map's rider dots and its finish marker: with the map (hudPlace).
constexpr AnchoredRoutine kAnchoredRoutines[] = {{0x8011DFDC, 0x8011DFE0, -1}, {0x8011E6E4, 0x8011E6E8, -1}};
// The map itself: a small 3D view (Aurora places it by its viewport), drawn from here.
constexpr u32 kMapCodeLo = 0x8011D300, kMapCodeHi = 0x8011D800;
bool drawingMap() {
    const Ctx* c = sched::currentCtx();
    if (!c) return false;
    if (c->lr >= kMapCodeLo && c->lr < kMapCodeHi) return true;
    u32 sp = c->r[1];
    for (int n = 0; n < 4 && (sp >> 25) == 0x40; n++) {
        if (!(sp = rd32(sp))) break;
        const u32 ret = rd32(sp + 4);
        if (ret >= kMapCodeLo && ret < kMapCodeHi) return true;
    }
    return false;
}

// The heights of the thin strips (lines in pieces) on the 4:3 screen, this frame's and the last's: a strip wholly beside
// the screen at one of them continues a line; others are pieces the game keeps out of sight.
std::vector<std::pair<float, float>> s_lineStrips[2];
u64 s_lineFrame = ~0ull;
void lineFrame() {
    if (s_lineFrame == s_frames) return;
    s_lineStrips[1].swap(s_lineStrips[0]);
    if (s_lineFrame + 1 != s_frames) s_lineStrips[1].clear();
    s_lineStrips[0].clear();
    s_lineFrame = s_frames;
}
void lineStrip(float y0, float y1) {
    lineFrame();
    s_lineStrips[0].push_back({y0, y1});
}
bool lineContinues(float y0, float y1) {
    lineFrame();
    for (const auto& lines : s_lineStrips)
        for (auto [a, b] : lines)
            if (std::fabs(a - y0) <= 3.f && std::fabs(b - y1) <= 3.f) return true;
    return false;
}

// Whether the textures the draw uses are one colour down their first and last columns (each channel within a few
// steps): a movie's plain backdrop. Read from guest memory (I8 and IA8: the movies' luma and chroma); other formats
// count as not plain.
bool edgesPlain() {
    const u32 maps = std::min<u32>(s_genMode & 0xF, 4);
    if (!maps) return false;
    for (u32 m = 0; m < maps; m++) {
        const u32 img0 = s_bp[0x88 + m], img3 = s_bp[0x94 + m];
        const u32 w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 0xF;
        const u32 base = (img3 & 0xFFFFFF) << 5;
        if (fmt != 1 && fmt != 3) return false;
        const u32 bytes = fmt == 1 ? ((w + 7) / 8) * ((h + 3) / 4) * 32 : ((w + 3) / 4) * ((h + 3) / 4) * 32;
        if (!base || base + bytes > kMem1Size) return false;
        const u8* t = hostp(0x80000000u | base);
        for (u32 x : {0u, w - 1}) {
            int lo[2] = {255, 255}, hi[2] = {0, 0};
            for (u32 y = 0; y < h; y += 2) {
                u8 c[2];
                if (fmt == 1) c[0] = c[1] = t[((y / 4) * ((w + 7) / 8) + x / 8) * 32 + (y % 4) * 8 + x % 8];
                else {
                    const u8* q = t + ((y / 4) * ((w + 3) / 4) + x / 4) * 32 + ((y % 4) * 4 + x % 4) * 2;
                    c[0] = q[0], c[1] = q[1];
                }
                for (int k = 0; k < 2; k++) lo[k] = std::min<int>(lo[k], c[k]), hi[k] = std::max<int>(hi[k], c[k]);
            }
            if (hi[0] - lo[0] > 8 || hi[1] - lo[1] > 8) return false;
        }
    }
    return true;
}

bool s_stageHal2D = false;  // the draw being placed: through HAL's 2D camera, while a stage runs
constexpr u32 kGmScene = 0x805359D8 + 0x7D7;  // Kirby Air Ride's major scene (gm +7D7; 0x12: a stage running)
// The HUD's placement of a 2D draw: 0 = not HUD (stays centred), -1 = by where it is, else that.
int drawingHud(float minx, float maxx, bool ortho) {
    const Ctx* c = sched::currentCtx();
    if (!c) return 0;
    u32 sp = c->r[1], chain[12] = {};
    int n = 0;
    bool inStage = false;
    for (; n < 12 && (sp >> 25) == 0x40; n++) {  // the back chain's LR save words
        if (!(sp = rd32(sp))) break;
        const u32 ret = chain[n] = rd32(sp + 4);
        for (u32 r : kHudReturns)
            if (ret == r) {
                for (u32 t : kTagReturns)
                    if (ortho && ret == t) return 0;
                return -1;
            }
        for (const AnchoredRoutine& a : kAnchoredRoutines)
            if (ret >= a.lo && ret < a.hi) return a.place;
        for (u32 r : kStageDisplayReturns) inStage = inStage || ret == r;
    }
    // Kirby Air Ride: everything its 2D camera draws while a stage runs (scene 0x12: a race, the city, a Stadium) is
    // HUD - placed in groups by where it is (hudPlace).
    if (s_stageHal2D) return -1;
    // Other 2D in a stage (trick ratings, prompts, boss gauges...) stays centred; each distinct
    // drawing path is logged once, so the ones that belong at the edges can be found and added.
    if (inStage && n >= 2) {
        static std::set<u64> s_seen;
        if (s_seen.size() < 64 && s_seen.insert(u64(chain[0]) << 32 | chain[1]).second)
            log("[wide] in-stage 2D at x %.0f..%.0f drawn from %08X %08X %08X %08X\n", minx, maxx, chain[0], chain[1],
                n > 2 ? chain[2] : 0, n > 3 ? chain[3] : 0);
    }
    return 0;
}

bool ninja2DMatrix(u32 m) {
    const float a = xff(m + 5);
    if (a == 0.f || std::fabs(xff(m) + a) > 1e-4f * std::fabs(a) || xff(m + 10) != 1.f) return false;
    for (u32 z : {1u, 3u, 4u, 7u, 8u, 9u, 11u})
        if (xff(m + z) != 0.f) return false;
    return std::fabs(xff(m + 2) - 320.f * a) <= 1e-3f * std::fabs(320.f * a) &&
           std::fabs(xff(m + 6) + 240.f * a) <= 1e-3f * std::fabs(240.f * a);
}

// The 2D-camera backgrounds covering the screen in this frame, by depth and texture (Kirby Air Ride's
// title builds its background from a few pieces of one texture at one depth: they cover it
// together; the menu's own pieces sit at the depth of its full-width bars, with other textures).
std::vector<std::pair<float, u32>> s_panels;
u64 s_panelFrame = ~0ull;
bool coveredPanelAt(float z, u32 tex) {
    if (s_panelFrame != s_frames) return false;
    for (auto [d, t] : s_panels)
        if (t == tex && std::fabs(d - z) <= 1.f) return true;
    return false;
}
void coveredPanel(float z, u32 tex) {
    if (s_panelFrame != s_frames) s_panels.clear(), s_panelFrame = s_frames;
    if (!coveredPanelAt(z, tex)) s_panels.push_back({z, tex});
}

// Kirby Air Ride's menu screens, recognised by one of their pieces (its box on the 4:3 screen and vertex count), seen
// this frame or the last. (A layout's parts - boxes a piece lies within, or a piece's own box - would place pieces by
// hand; none do: menus follow the general rules.)
struct Box {
    float x0, x1, y0, y1;
    bool within(float a0, float a1, float b0, float b1) const { return a0 >= x0 && a1 <= x1 && b0 >= y0 && b1 <= y1; }
    bool same(float a0, float a1, float b0, float b1) const {
        return std::fabs(a0 - x0) <= 3.f && std::fabs(a1 - x1) <= 3.f && std::fabs(b0 - y0) <= 3.f && std::fabs(b1 - y1) <= 3.f;
    }
};
constexpr int MENU_HIDE = 100;  // a menu part not drawn at all
constexpr int MENU_CLIP = 101;  // ... drawn centred, cut at the 4:3 screen's sides
constexpr int MENU_BLEED_L = 102, MENU_BLEED_R = 103;  // ... centred, its end past that side moved out to the wide screen's
struct MenuPart {
    Box box;
    bool exact;  // the piece's own box (else: any piece within it ...
    int place;
    float minWidth = 0;  // ... at least this wide
    float maxWidth = 1e9f, minHeight = 0, maxHeight = 1e9f;  // ... and of this size: a pattern's tile)
};
struct MenuLayout {
    const char* name;
    Box sign;
    u32 signCount;
    std::vector<MenuPart> parts;
    bool strips = false;  // (its thin strips are parts too: the results' score capsules; else they stay as they are)
};
// The screens recognised, for the test runs' menu driving (SA2_MENU_SEQ) and the Online menu (gxMenuScreen). Their
// pieces are placed by the general rules (place2D), not by hand: no parts.
const std::vector<MenuLayout> kMenuLayouts = {
    {"mode select", {392, 593, 235, 384}, 15, {}},  // (its movie preview's frame)
    // The first of the LAN (Online) mode's screens, "How many Nintendo GameCube systems will you use to play?" (its third
    // choice, "4"): the Online menu opens on it (src/runtime/online_menu.cpp).
    {"lan systems", {310, 544, 314, 358}, 9, {}},
    {"stadium results", {287, 646, 43, 127}, 7, {}},  // (its name's plate)
    {"city trial menu", {-39, 677, 227, 483}, 93, {}},  // (its bottom band; the Air Ride and Top Ride menus have it too)
};
u64 s_layoutFrame = ~0ull;
const MenuLayout* s_layoutNow = nullptr;   // recognised this frame
const MenuLayout* s_layoutLast = nullptr;  // ... the last
int menuPart(float x0, float x1, float y0, float y1, u32 count, bool strip = false) {
    if (s_layoutFrame != s_frames) {
        s_layoutLast = s_layoutFrame + 1 == s_frames ? s_layoutNow : nullptr;
        s_layoutNow = nullptr;
        s_layoutFrame = s_frames;
    }
    for (const MenuLayout& l : kMenuLayouts)
        if (count == l.signCount && l.sign.same(x0, x1, y0, y1)) s_layoutNow = &l;
    const MenuLayout* l = s_layoutNow ? s_layoutNow : s_layoutLast;
    if (!l || (strip && !l->strips)) return 0;
    for (const MenuPart& part : l->parts)
        if (part.exact ? part.box.same(x0, x1, y0, y1)
                       : part.box.within(x0, x1, y0, y1) && x1 - x0 >= part.minWidth && x1 - x0 <= part.maxWidth &&
                             y1 - y0 >= part.minHeight && y1 - y0 <= part.maxHeight)
            return part.place;
    return 0;
}

}  // namespace
// The menu screen recognised (kMenuLayouts) in the last frame or the one before; "" for none.
const char* gxMenuScreen() {
    if (s_layoutFrame + 2 < s_frames) return "";
    const MenuLayout* l = s_layoutNow ? s_layoutNow : s_layoutLast;
    return l ? l->name : "";
}
namespace {

// The HUD's pieces, placed in groups. Kirby Air Ride draws its HUD piece by piece - a meter, its needle and digits,
// a copy ability's flourishes around it, a lap record's words - so pieces that touch or overlap are one group, placed
// as a whole: a group reaching into the outer eighth of the 4:3 screen goes to that edge, the rest stays centred.
// Pieces are placed as they are drawn, so the groups are the last frame's; a piece in none of them joins this
// frame's pieces it touches. A piece wider than half the screen (a pause or results panel) joins no group (it would
// tie the corners together). A map - a small 3D view, placed by Aurora from its viewport - takes the pieces inside it
// (its rider dots, wherever the course puts the map) to its edge.
struct HudBox {
    float x0, x1, y0, y1;
    int place;  // (pieces: a place they must have - a map's, a routine's - or 0)
};
constexpr float kHudGap = 10.f;      // (pieces closer than this touch: a word's letters, a meter's parts)
std::vector<HudBox> s_hudBoxes[2];   // map viewports at an edge: this frame's, the last
std::vector<HudBox> s_hudPieces;     // this frame's pieces
std::vector<HudBox> s_hudGroups;     // the last frame's groups
u64 s_hudBoxFrame = ~0ull;
bool hudTouch(const HudBox& a, float x0, float x1, float y0, float y1) {
    return x0 <= a.x1 + kHudGap && x1 >= a.x0 - kHudGap && y0 <= a.y1 + kHudGap && y1 >= a.y0 - kHudGap;
}
bool hudJoins(const HudBox& b) { return b.x1 - b.x0 <= 320.f; }
int hudEdge(float x0, float x1) {
    const float cx = (x0 + x1) * 0.5f;
    return x1 >= 560.f && cx > 320.f ? WIDE_RIGHT : x0 <= 80.f && cx < 320.f ? WIDE_LEFT : WIDE_CENTRE;
}
// The groups of a frame's pieces (each with its place: a member's required one, else by its box).
void hudGroups(const std::vector<HudBox>& pieces, std::vector<HudBox>& out) {
    out.clear();
    const size_t n = pieces.size();
    std::vector<size_t> up(n);
    for (size_t i = 0; i < n; i++) up[i] = i;
    auto root = [&](size_t i) {
        while (up[i] != i) i = up[i] = up[up[i]];
        return i;
    };
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (hudJoins(pieces[i]) && hudJoins(pieces[j]) && hudTouch(pieces[i], pieces[j].x0, pieces[j].x1, pieces[j].y0, pieces[j].y1))
                up[root(i)] = root(j);
    std::vector<int> slot(n, -1);
    std::vector<int> need;
    for (size_t i = 0; i < n; i++) {
        const size_t r = root(i);
        const HudBox& p = pieces[i];
        if (slot[r] < 0) {
            slot[r] = int(out.size());
            out.push_back({p.x0, p.x1, p.y0, p.y1, 0});
            need.push_back(0);
        }
        HudBox& g = out[size_t(slot[r])];
        g.x0 = std::min(g.x0, p.x0), g.x1 = std::max(g.x1, p.x1), g.y0 = std::min(g.y0, p.y0), g.y1 = std::max(g.y1, p.y1);
        if (p.place) need[size_t(slot[r])] = p.place;
    }
    for (size_t g = 0; g < out.size(); g++) out[g].place = need[g] ? need[g] : hudEdge(out[g].x0, out[g].x1);
}
void hudBoxesFrame() {
    if (s_hudBoxFrame != s_frames) {
        const bool next = s_hudBoxFrame + 1 == s_frames;
        s_hudBoxes[1].swap(s_hudBoxes[0]);
        if (!next) s_hudBoxes[1].clear();
        s_hudBoxes[0].clear();
        if (next) hudGroups(s_hudPieces, s_hudGroups);
        else s_hudGroups.clear();
        s_hudPieces.clear();
        s_hudBoxFrame = s_frames;
    }
}
void hudMapBox(float x0, float x1, float y0, float y1) {
    hudBoxesFrame();
    const float cx = xff(0x101D) - 342.f;  // the viewport's centre (Aurora places a small 3D view by it)
    const int place = cx < 0.3f * 640.f ? WIDE_LEFT : cx > 0.7f * 640.f ? WIDE_RIGHT : WIDE_CENTRE;
    if (place != WIDE_CENTRE) s_hudBoxes[0].push_back({x0, x1, y0, y1, place});
}
int hudPlace(float x0, float x1, float y0, float y1, int routine) {
    hudBoxesFrame();
    int need = routine > 0 ? routine : 0;
    const float area = (x1 - x0) * (y1 - y0);
    if (!need)
        for (const auto& boxes : s_hudBoxes)
            for (const HudBox& b : boxes)
                if (x0 >= b.x0 - 2.f && x1 <= b.x1 + 2.f && y0 >= b.y0 - 2.f && y1 <= b.y1 + 2.f && area * 1.5f < (b.x1 - b.x0) * (b.y1 - b.y0))
                    need = b.place;
    const HudBox self{x0, x1, y0, y1, need};
    s_hudPieces.push_back(self);
    if (need) return need;
    if (!hudJoins(self)) return hudEdge(x0, x1);
    // the last frame's group it lies in (the one it overlaps most)
    const HudBox* best = nullptr;
    float bestArea = 0.f;
    for (const HudBox& g : s_hudGroups) {
        if (!hudTouch(g, x0, x1, y0, y1)) continue;
        const float ox = std::min(x1, g.x1 + kHudGap) - std::max(x0, g.x0 - kHudGap), oy = std::min(y1, g.y1 + kHudGap) - std::max(y0, g.y0 - kHudGap);
        if (ox * oy > bestArea || !best) best = &g, bestArea = ox * oy;
    }
    if (best) return best->place;
    // else this frame's pieces it touches, and theirs
    HudBox box = self;
    int found = 0;
    for (bool grew = true; grew;) {
        grew = false;
        for (const HudBox& p : s_hudPieces) {
            if (!hudJoins(p) || !hudTouch(box, p.x0, p.x1, p.y0, p.y1)) continue;
            if (p.place) found = p.place;
            if (p.x0 < box.x0 || p.x1 > box.x1 || p.y0 < box.y0 || p.y1 > box.y1) {
                box.x0 = std::min(box.x0, p.x0), box.x1 = std::max(box.x1, p.x1), box.y0 = std::min(box.y0, p.y0), box.y1 = std::max(box.y1, p.y1);
                grew = true;
            }
        }
    }
    return found ? found : hudEdge(box.x0, box.x1);
}

void place2D(const u8* p, u32 count, u32 fmt) {
    s_drawBox[0] = s_drawBox[1] = s_drawBox[2] = s_drawBox[3] = 1e9f;  // (set below when known)
    const u32 lo = s_vcdLo, a = s_vatA[fmt];
    const u32 posType = (lo >> 9) & 3;
    if (!posType) return;
    const bool ortho = s_xf[0x1026] == 1;
    // Under a perspective projection, only Ninja's 2D matrix draws 2D: a draw whose first vertex
    // uses another matrix is 3D, without looking at its vertices - unless it is a single quad, which
    // may be a backdrop or screen effect filling the screen (below).
    const bool logging = s_frames == s_wideLogFrame || (s_frames >= s_wideLogFrame && s_frames <= s_wideLogTo) || s_wideAudit;
    // Kirby Air Ride (HAL's engine) draws its menus, title and race HUD in perspective through one
    // 2D camera with a narrow view (projection scales 3.593 and 4.511): flat, they are 2D.
    const bool hal2D = !ortho && std::fabs(xff(0x1020) - 3.593f) < 0.01f && std::fabs(xff(0x1022) - 4.511f) < 0.01f;
    s_stageHal2D = hal2D && rd8(kGmScene) == 0x12;
    if (!ortho && !logging && count > 6 && !hal2D) {
        const u32 m0 = ((lo & 1) ? p[0] : (s_cpMtxA & 0x3F)) * 4;
        if (m0 + 11 < 0x100 && !ninja2DMatrix(m0)) {
            if (s_last2D != WIDE_3D) {
                aurora(AUR_WIDE_2D);
                put8(u8(WIDE_3D));
                s_last2D = WIDE_3D;
            }
            return;
        }
    }
    u32 pre = 0;  // matrix index bytes before the position
    for (u32 i = 0; i < 9; i++) pre += (lo >> i) & 1;
    const u32 vsize = vertexSize(fmt);
    const u32 comps = (a & 1) ? 3 : 2, cfmt = (a >> 1) & 7, shift = (a >> 4) & 31;
    const float scale = 1.f / float(1u << shift);
    auto comp = [&](const u8* q, u32 i) -> float {
        switch (cfmt) {
        case 0: return q[i] * scale;
        case 1: return s8(q[i]) * scale;
        case 2: return u16(q[i * 2] << 8 | q[i * 2 + 1]) * scale;
        case 3: return s16(q[i * 2] << 8 | q[i * 2 + 1]) * scale;
        default: return __builtin_bit_cast(float, u32(q[i * 4]) << 24 | u32(q[i * 4 + 1]) << 16 | u32(q[i * 4 + 2]) << 8 | q[i * 4 + 3]);
        }
    };
    const float p0 = xff(0x1020), p1 = xff(0x1021), p2 = xff(0x1022), p3 = xff(0x1023);
    const float sx = xff(0x101A), ox = xff(0x101D) - 342.f, sy = xff(0x101B), oy = xff(0x101E) - 342.f;
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f, minz = 1e9f, maxz = -1e9f;
    bool ninja2D = true;
    s_bleed.clear();
    s_smear.clear(), s_smearCount = 0, s_smearPos.clear();
    float vxScreen[256], vxPerUnit[256], vyScreen[256];  // each vertex's screen x, y, and screen units per unit of its x
    s32 vxIndex[256];                     // ... and its index in the position array (indexed positions; -1: none)
    for (u32 v = 0; v < count && v < 256; v++) {
        vxScreen[v] = vyScreen[v] = 1e9f, vxPerUnit[v] = 0.f, vxIndex[v] = -1;
        const u8* vp = p + v * vsize;
        const u32 m = ((lo & 1) ? vp[0] : (s_cpMtxA & 0x3F)) * 4;  // matrix rows are 4 words
        if (m + 11 >= 0x100) continue;
        const u8* pos = vp + pre;
        if (posType >= 2) {
            const u32 idx = posType == 2 ? pos[0] : u32(pos[0]) << 8 | pos[1];
            pos = hostp(0x80000000u | ((s_arrayBase[0] + idx * s_arrayStride[0]) & 0x01FFFFFF));
            vxIndex[v] = s32(idx);
        }
        ninja2D = ninja2D && ninja2DMatrix(m);
        const float x = comp(pos, 0), y = comp(pos, 1), z = comps == 3 ? comp(pos, 2) : 0.f;
        const float ex = xff(m) * x + xff(m + 1) * y + xff(m + 2) * z + xff(m + 3);
        const float ey = xff(m + 4) * x + xff(m + 5) * y + xff(m + 6) * z + xff(m + 7);
        const float ez = xff(m + 8) * x + xff(m + 9) * y + xff(m + 10) * z + xff(m + 11);
        float ndcX, ndcY;
        if (ortho) ndcX = p0 * ex + p1, ndcY = p2 * ey + p3;
        else if (ez < -1e-6f) ndcX = (p0 * ex + p1 * ez) / -ez, ndcY = (p2 * ey + p3 * ez) / -ez;
        else continue;  // behind the eye
        const float screenX = ndcX * sx + ox, screenY = ndcY * sy + oy;
        static const int s_vtxLogCount = std::getenv("SA2_VTX_LOG") ? std::atoi(std::getenv("SA2_VTX_LOG")) : 0;  // (debugging)
        if (s_vtxLogCount && u32(s_vtxLogCount) == count && s_frames == s_wideLogFrame)
            log("[vtx] %u: obj %.2f %.2f %.2f screen %.1f %.1f (pos type %u fmt %u)\n", v, x, y, z, screenX, screenY, posType, cfmt);
        vxScreen[v] = screenX, vyScreen[v] = screenY;
        vxPerUnit[v] = (ortho ? p0 : p0 / -ez) * xff(m) * sx;
        minx = std::min(minx, screenX);
        maxx = std::max(maxx, screenX);
        miny = std::min(miny, screenY);
        maxy = std::max(maxy, screenY);
        minz = std::min(minz, ez);
        maxz = std::max(maxz, ez);
    }
    int place = WIDE_3D;
    s_draw2D = ortho;
    if (minx <= maxx && !hal2D && !ortho && drawingMap()) hudMapBox(minx, maxx, miny, maxy);
    if (minx <= maxx) {
        // (HAL's 2D layouts tilt some panels a little: the title's background spans 0.3% of its depth.)
        const bool flat = maxz - minz <= (hal2D ? 1e-2f : 1e-3f) * std::max(1.f, -minz);
        // (In a stage, all the 2D camera draws is HUD, flat or not: a copy ability's feathers and flames turn in depth
        // around the speed meter.)
        const bool sprite = ortho || (flat && (ninja2D || hal2D)) || s_stageHal2D;
        // A flat quad in perspective that fills the 4:3 screen exactly is a backdrop or screen effect
        // sized to the 4:3 view (Kirby Air Ride's skies at the far plane): seen through the widened
        // view it covered only the middle, the 3D's sky beside it. A picture covers the wide screen
        // at its own aspect (scaled, cropped top and bottom - never stretched); a plain colour
        // (fades, flashes) and a copy of the screen (afterimages: the copy is of the wide screen)
        // span it.
        const bool textured = (s_genMode & 0xF) && !s_copyDests.count(s_tex0);
        const bool fills43 = flat && count <= 6 && minx >= -2.f && minx <= 2.f && maxx >= 638.f && maxx <= 642.f && miny <= 2.f &&
                             maxy >= 478.f;
        if (!sprite && fills43) place = textured ? WIDE_COVER : WIDE_STRETCH;
        s_draw2D = sprite || fills43;
        const float w = maxx - minx, h = maxy - miny, cx = (minx + maxx) * 0.5f;
        // (also the 3D models a menu shows through its 2D camera: the Air Ride menu's Kirby beside its emblem)
        // A thin strip (a menu's title line, in pieces that run on past the screen) is not a part: it stays centred, its
        // pieces past the 4:3 screen shown - the line runs on to the wide screen's edge (moved with the panel, one piece
        // left a gap in it).
        // (Only one past the 4:3 screen or very wide: a button's label - Options in focus, 150 wide - is part of its button.)
        const bool strip = hal2D && h <= 30.f && w >= 150.f && (minx < -2.f || maxx > 642.f || w >= 300.f);
        // A piece of a bar that runs off the 4:3 screen: kept at its size, the end(s) past the screen moved out to the
        // wide screen's edge (below) - stretched, a shaped bar's notches and caps moved off the pieces beside them.
        int bleedSide = 0;  // -1 the left end, 1 the right, 2 both
        int overhang = 0;   // a plain shape's part past this side (-1 left, 1 right) drawn out to the wide screen's
        // (a screen placed by hand places the pieces its HUD routines draw too, and its lettering through the
        // orthographic camera: the results' times and icons)
        const int part0 = hal2D || ortho ? menuPart(minx, maxx, miny, maxy, count, strip) : 0;
        if (const int part = part0 == MENU_BLEED_L || part0 == MENU_BLEED_R ? 0 : part0) {
            place = part == MENU_HIDE || part == MENU_CLIP ? WIDE_CENTRE : part;  // placed by hand (kMenuLayouts)
            s_hideDraw = part == MENU_HIDE;
            s_clipDraw = part == MENU_CLIP;
        } else if (sprite) {
            // Nothing with a picture on it is stretched. Across the whole screen: a background covers
            // the wide screen at its own aspect (scaled, cropped top and bottom); a picture drawn
            // orthographically - Kirby Air Ride's movies - stays a 4:3 picture in the middle; a
            // plain colour (fades) and a copy of the screen span it; a full-width band with a
            // picture (not a background) stays as it is, centred.
            if (minx <= 16.f && maxx >= 624.f) {
                // a plain rectangle; shapes keep theirs (HAL's stretch: its transition shutters, bands) - but a shaped
                // band running past both sides (the title's, with its notch for Press Start) keeps its shape, its ends
                // moved out
                if (!textured && hal2D && count > 6 && h < 300.f && minx < -1.f && maxx > 641.f) place = WIDE_CENTRE, bleedSide = 2;
                else if (!textured) place = count <= 6 || hal2D ? WIDE_STRETCH : WIDE_CENTRE;
                else if (ortho) place = WIDE_CENTRE;
                else if (h >= 240.f) {
                    place = WIDE_COVER;
                    if (hal2D) coveredPanel(minz, s_tex0);
                } else place = WIDE_CENTRE;
            }
            // Kirby Air Ride's backgrounds built from pieces (the title's: pieces of one texture at
            // one depth) cover the screen together.
            else if (hal2D && textured && coveredPanelAt(minz, s_tex0))
                place = WIDE_COVER;
            // Its menus: centred, by the rules below and the strips (s_smear). A piece wholly beside the 4:3 screen is
            // one the game keeps out of sight there (the mode select's wings waiting off the left edge, Player Entry's
            // panels for players not in): the wide screen would show it, so it is not drawn - unless it is a piece of a
            // line running on past the screen (a title line, in pieces: one at the same height is on the screen).
            else if (hal2D && !drawingHud(minx, maxx, ortho)) {
                place = WIDE_CENTRE;
                if (strip && maxx > 0.f && minx < 640.f) lineStrip(miny, maxy);
                if ((maxx <= 0.f || minx >= 640.f) && !(strip && lineContinues(miny, maxy))) s_hideDraw = true;
                // A piece crossing one side of the 4:3 screen is shown past it, as the game drew it (the wide screen
                // shows more of the menu). A plain shape (a background layer, a panel, a shutter sweeping across) has
                // its part past the side drawn out to reach the wide screen's (remapOverhang, below: what is on the 4:3
                // screen stays as it is); a picture with a straight edge there runs on in a strip (s_smear); a thin
                // textured line running off one side (the title's Press Start capsule's edge lines) runs off the wide
                // screen's.
                const bool crossesL = minx < -2.f && maxx > 2.f && maxx < 638.f, crossesR = maxx > 642.f && minx < 638.f && minx > 2.f;
                if (crossesL || crossesR) {
                    if (!textured) overhang = crossesL ? -1 : 1;
                    else if (h <= 12.f) bleedSide = crossesL ? -1 : 1;
                }
            }
            // Backdrops: the farthest layers (sprite priority, as depth, 5000 or more). Nearer layers
            // carry things placed on them (the stage select's map and its stage icons) and stay
            // centred with the rest.
            else if (minz <= -5000.f && h >= 120.f && minx >= -1.f && maxx <= 641.f && (minx <= 1.f || maxx >= 639.f)) {
                place = WIDE_COVER;  // a tall piece against an edge: a tile of a backdrop
                s_backdropDepths[1].insert(minz);
            } else if (minz <= -5000.f && w >= 64.f && h >= 64.f && (s_backdropDepths[0].count(minz) || s_backdropDepths[1].count(minz)))
                place = WIDE_COVER;  // a tile inside a backdrop: Ninja draws them all at one priority (depth)
            else if (const int hud = drawingHud(minx, maxx, ortho); hud == 0) place = WIDE_CENTRE;
            else place = hudPlace(minx, maxx, miny, maxy, hud);
            // A full-height panel that runs off the 4:3 screen's side (the stage title card's band)
            // runs off the wide screen's: its outer edge moves out by the width added there.
            const float extra = g_wideExtra.load();
            const bool offLeft = minx < -1.f && maxx < 320.f, offRight = maxx > 641.f && minx > 320.f;
            // (Positions sent with the draw are changed in its copy of the commands; indexed ones in this frame's copy
            // of the position array, each once.)
            std::vector<s32> moved;
            auto setX = [&](u8* q, float x) {  // a position's x, in its format
                if (cfmt == 4) {
                    const u32 b = __builtin_bit_cast(u32, x);
                    q[0] = u8(b >> 24), q[1] = u8(b >> 16), q[2] = u8(b >> 8), q[3] = u8(b);
                    return;
                }
                const float f = std::round(x / scale);  // fixed point (clamped)
                if (cfmt == 0) q[0] = u8(std::clamp(f, 0.f, 255.f));
                else if (cfmt == 1) q[0] = u8(s8(std::clamp(f, -128.f, 127.f)));
                else {
                    const u32 b = cfmt == 2 ? u32(std::clamp(f, 0.f, 65535.f)) : u32(u16(s16(std::clamp(f, -32768.f, 32767.f))));
                    q[0] = u8(b >> 8), q[1] = u8(b);
                }
            };
            auto moveVertex = [&](u32 v, float by) {  // a vertex moved by `by` on the screen, sideways
                if (std::fabs(vxPerUnit[v]) <= 1e-6f) return;
                if (posType == 1) {
                    if (cfmt != 4) return;  // (moved as floats)
                    const float x = comp(p + v * vsize + pre, 0);
                    s_bleed.push_back({v * vsize + pre, x + by / vxPerUnit[v]});
                } else if (vxIndex[v] >= 0 && s_arrayCopy[0] && u32(vxIndex[v]) * s_arrayStride[0] + 4 <= s_arraySent[0] &&
                           std::find(moved.begin(), moved.end(), vxIndex[v]) == moved.end()) {
                    moved.push_back(vxIndex[v]);
                    u8* q = s_arrayCopy[0] + u32(vxIndex[v]) * s_arrayStride[0];
                    setX(q, comp(q, 0) + by / vxPerUnit[v]);
                }
            };
            auto bleed = [&](float edge, float by) {
                for (u32 v = 0; v < count && v < 256; v++)
                    if (std::fabs(vxScreen[v] - edge) < 0.5f) moveVertex(v, by);
            };
            // The part of a plain shape past a side of the 4:3 screen, drawn out to reach the wide screen's side (each
            // vertex past it moved out in proportion; what is on the 4:3 screen stays).
            auto remapOverhang = [&](int side) {
                const float over = side < 0 ? -minx : maxx - 640.f;
                if (over <= 0.5f || over >= extra + 2.f) return;
                const float k = (extra + 2.f) / over - 1.f;
                for (u32 v = 0; v < count && v < 256; v++) {
                    const float past = side < 0 ? std::min(vxScreen[v], 0.f) : std::max(vxScreen[v] - 640.f, 0.f);
                    if (past != 0.f && vxScreen[v] < 1e8f) moveVertex(v, past * k);
                }
            };
            if (overhang && place == WIDE_CENTRE && extra > 0.f && count <= 256) remapOverhang(overhang);
            if (bleedSide && place == WIDE_CENTRE && extra > 0.f && count <= 256) {
                if (bleedSide != 1) bleed(minx, -(extra + 2.f));
                if (bleedSide != -1) bleed(maxx, extra + 2.f);
            } else if (place == WIDE_CENTRE && extra > 0.f && miny <= 1.f && maxy >= 479.f && (offLeft || offRight) && posType == 1 &&
                       cfmt == 4 && count <= 256)
                bleed(offLeft ? minx : maxx, offLeft ? -(extra + 2.f) : extra + 2.f);
            // A band across a HAL menu that ends a little past both sides of the 4:3 screen (Player Entry's bar at the
            // bottom) ended inside the wide one: its ends move out to the wide screen's.
            else if (place == WIDE_CENTRE && hal2D && extra > 0.f && minx < -1.f && maxx > 641.f && minx > -extra && maxx < 640.f + extra &&
                     h >= 20.f && count <= 256) {
                bleed(minx, -(extra + 2.f));
                bleed(maxx, extra + 2.f);
            }
            // Menus: a piece reaching a side of the 4:3 screen with a straight edge runs on to the wide screen's side
            // (s_smear).
            const bool menu = (hal2D || ortho) && rd8(kGmScene) != 0x12;
            // A picture covering the whole 4:3 screen drawn orthographically (Kirby Air Ride's movies: the logos, the
            // intro, How to Play) runs on to the wide screen's sides only where its edges are one colour (the logos'
            // plain backdrop); a picture with detail at its edges would streak, so it stays a 4:3 picture.
            const bool fullPicture = ortho && textured && minx <= 2.f && maxx >= 638.f && miny <= 2.f && maxy >= 478.f;
            const u32 stride0 = s_arrayStride[0];
            const bool indexedOk = posType >= 2 && s_arrayCopy[0] && stride0 && stride0 <= 64;
            if (menu && place == WIDE_CENTRE && !s_hideDraw && extra > 0.f && (posType == 1 || indexedOk) && count >= 3 && count <= 256 &&
                s_bleed.empty() && moved.empty() && h >= 2.f && w >= 2.f && (!fullPicture || edgesPlain())) {
                const u32 cull = (s_genMode >> 14) & 3;
                // the draw's facing on the screen: its first triangle's winding (0: none to go by)
                const float face = (vxScreen[1] - vxScreen[0]) * (vyScreen[2] - vyScreen[0]) - (vxScreen[2] - vxScreen[0]) * (vyScreen[1] - vyScreen[0]);
                auto strip = [&](float edge, float to) {
                    int top = -1, bot = -1;  // the edge's highest and lowest vertices
                    for (u32 v = 0; v < count && v < 256; v++) {
                        if (std::fabs(vxScreen[v] - edge) >= 0.5f || std::fabs(vxPerUnit[v]) <= 1e-6f) continue;
                        if (posType >= 2 && (vxIndex[v] < 0 || u32(vxIndex[v]) * stride0 + stride0 > s_arraySent[0])) continue;
                        if (top < 0 || vyScreen[v] < vyScreen[top]) top = int(v);
                        if (bot < 0 || vyScreen[v] > vyScreen[bot]) bot = int(v);
                    }
                    // (straight: the edge's vertices span most of the piece's height - not a rounded or pointed end)
                    if (top < 0 || vyScreen[bot] - vyScreen[top] < 0.6f * h) return;
                    // A strip vertex: a copy of the edge vertex, moved out or not. Indexed positions go in the strips' own
                    // little position array (s_smearPos), each vertex its own entry.
                    auto vertex = [&](int v, bool out) {
                        const size_t at = s_smear.size();
                        s_smear.insert(s_smear.end(), p + size_t(v) * vsize, p + size_t(v + 1) * vsize);
                        const float x = (to - vxScreen[v]) / vxPerUnit[v];
                        if (posType == 1) {
                            if (out) setX(s_smear.data() + at + pre, comp(p + size_t(v) * vsize + pre, 0) + x);
                            return;
                        }
                        const u32 local = u32(s_smearPos.size() / stride0);
                        const u8* src = s_arrayCopy[0] + u32(vxIndex[v]) * stride0;
                        s_smearPos.insert(s_smearPos.end(), src, src + stride0);
                        if (out) setX(s_smearPos.data() + local * stride0, comp(src, 0) + x);
                        u8* idx = s_smear.data() + at + pre;
                        if (posType == 2) idx[0] = u8(local);
                        else idx[0] = u8(local >> 8), idx[1] = u8(local);
                    };
                    // (out-top, top, bottom) and (out-top, bottom, out-bottom), wound as the draw is if faces are culled
                    const float mine = (edge - to) * (vyScreen[bot] - vyScreen[top]);
                    const bool flip = cull != 0 && face != 0.f && (mine > 0.f) != (face > 0.f);
                    for (int pass = 0; pass < (cull != 0 && face == 0.f ? 2 : 1); pass++) {
                        const bool f = flip != (pass == 1);
                        vertex(top, true), vertex(f ? bot : top, false), vertex(f ? top : bot, false);
                        vertex(top, true), vertex(bot, f), vertex(bot, !f);
                        s_smearCount += 6;
                    }
                };
                if (minx <= 2.f && minx > -extra && maxx > 8.f) strip(minx, -(extra + 2.f));
                if (maxx >= 638.f && maxx < 640.f + extra && minx < 632.f) strip(maxx, 640.f + extra + 2.f);
            }
        }
        if (s_frames == s_wideLogFrame || (s_frames >= s_wideLogFrame && s_frames <= s_wideLogTo && flat)) {
            const u32 m = ((lo & 1) ? p[0] : (s_cpMtxA & 0x3F)) * 4;
            std::string chain;  // the guest code drawing it: LR, then the back chain's LR save words
            if (const Ctx* c = sched::currentCtx()) {
                char b[16];
                std::snprintf(b, sizeof b, " %08X", c->lr);
                chain += b;
                u32 sp = c->r[1];
                for (int i = 0; i < 12 && (sp >> 25) == 0x40; i++) {
                    if (!(sp = rd32(sp))) break;
                    std::snprintf(b, sizeof b, " %08X", rd32(sp + 4));
                    chain += b;
                }
            }
            log("[wide] %llu %s draw: x %.0f..%.0f y %.0f..%.0f z %.2f..%.2f, %u verts (pos %u/%u%s), mtx %u [%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g], zmode %x, tex %u @%08X%s -> %d%s%s%s; proj %.4g %.4g %.4g %.4g; stack%s\n",
                (unsigned long long)s_frames, ortho ? "ortho" : "persp", minx, maxx, miny, maxy, minz, maxz, count, posType, cfmt,
                s_smearCount ? " STRIPS" : "", m / 4,
                xff(m), xff(m + 1), xff(m + 2), xff(m + 3), xff(m + 4), xff(m + 5), xff(m + 6), xff(m + 7), xff(m + 8),
                xff(m + 9), xff(m + 10), xff(m + 11), s_zmode, s_genMode & 0xF, s_tex0, s_copyDests.count(s_tex0) ? " (copy)" : "", place,
                s_hideDraw ? " HIDE" : "", s_clipDraw ? " CLIP" : "", s_bleed.empty() ? "" : " BLEED", p0, p1, p2, p3, chain.c_str());
        }
    }
    if (s_wideAudit && minx <= maxx && !s_hideDraw && s_segmentAt != SIZE_MAX && s_out[s_segmentAt] == 1 &&
        (place == WIDE_CENTRE || place == WIDE_3D) && (minx <= 2.f || maxx >= 638.f) && minx > -60.f && maxx < 700.f &&
        maxy - miny >= 40.f) {
        u32 chain[4] = {};
        if (const Ctx* c = sched::currentCtx()) {
            chain[0] = c->lr;
            u32 sp = c->r[1];
            for (int i = 1; i < 4 && (sp >> 25) == 0x40; i++) {
                if (!(sp = rd32(sp))) break;
                chain[i] = rd32(sp + 4);
            }
        }
        const bool both = minx <= 2.f && maxx >= 638.f;
        static std::set<u64> s_auditSeen;
        const u64 key = (u64(chain[1]) << 32 | chain[2]) ^ (u64(chain[0]) * 0x9E3779B97F4A7C15ull) ^ u64(place) << 1 ^ u64(both);
        if (s_auditSeen.size() < 400 && s_auditSeen.insert(key).second) {
            const u32 m = ((lo & 1) ? p[0] : (s_cpMtxA & 0x3F)) * 4;
            log("[wideaudit] frame %llu scene %02X %s %s: x %.0f..%.0f y %.0f..%.0f z %.1f..%.1f, %u verts, tex %u%s, mtx [%.3g %.3g %.3g | %.3g %.3g %.3g | %.3g %.3g %.3g], %s; from %08X %08X %08X %08X\n",
                (unsigned long long)s_frames, rd8(kGmScene), place == WIDE_3D ? "3D" : "centred", both ? "BOTH sides" : "one side", minx, maxx,
                miny, maxy, minz, maxz, count, s_genMode & 0xF, s_copyDests.count(s_tex0) ? " (copy)" : "", xff(m), xff(m + 1), xff(m + 2),
                xff(m + 4), xff(m + 5), xff(m + 6), xff(m + 8), xff(m + 9), xff(m + 10), ortho ? "ortho" : hal2D ? "hal2D" : "persp", chain[0],
                chain[1], chain[2], chain[3]);
        }
    }
    s_drawBox[0] = minx, s_drawBox[1] = maxx, s_drawBox[2] = miny, s_drawBox[3] = maxy;
    if (place != s_last2D) {
        aurora(AUR_WIDE_2D);
        put8(u8(place));
        s_last2D = place;
    }
}

// ---- state shadows (for each frame's preamble) ----
u32 s_bpMask = 0xFFFFFF;
std::bitset<256> s_bpSet;
u32 s_cp[256];
std::bitset<256> s_cpSet;
std::bitset<0x1058> s_xfSet;

void translateBp(u32 v) {
    const u32 reg = v >> 24, val = v & 0xFFFFFF;
    if (reg == 0xFE) s_bpMask = val;
    else {
        s_bp[reg] = (s_bp[reg] & ~s_bpMask) | (val & s_bpMask);
        s_bpSet.set(reg);
        s_bpMask = 0xFFFFFF;
    }
    int map;
    if (reg == 0x00) s_genMode = val;
    if (reg == 0x40) s_zmode = val;
    if (texMapOf(reg, 0x14) == 0) s_tex0 = (val & 0xFFFFFF) << 5;
    if ((map = texMapOf(reg, 0x08)) >= 0) s_texImage0[map] = val;
    if ((map = texMapOf(reg, 0x04)) >= 0) s_texMode1[map] = val;
    if ((map = texMapOf(reg, 0x18)) >= 0) {
        s_tlutRegion[map] = val;
        // The SDK writes a CI texture's TLUT register after its address (GXLoadTexObj): sent at the
        // address, the texture had the palette the previous one named (Kirby Air Ride's roads came
        // out magenta and green) - it goes again with this one.
        const u32 fmt = (s_texImage0[map] >> 20) & 0xF;
        const u32 image3 = map < 4 ? 0x94 + u32(map) : 0xB4 + u32(map - 4);
        if ((fmt == 0x8 || fmt == 0x9 || fmt == 0xA) && s_bpSet[image3]) {
            put8(CMD_LOAD_BP);
            put32(v);
            loadTexture(map, s_bp[image3] & 0xFFFFFF);
            return;
        }
    }
    if ((map = texMapOf(reg, 0x14)) >= 0) {  // image3: the texture's address -> load it
        put8(CMD_LOAD_BP);
        put32(v);
        loadTexture(map, val);
        return;
    }
    switch (reg) {
    case 0x45:  // PE_DONE: everything before it has been "drawn"
        raiseInterrupt(IRQ_PE_FINISH, [] { mmioSet16(0x100A, mmioGet16(0x100A) | 0x8); });
        break;
    case 0x47:  // PE token (no interrupt)
        mmioSet16(0x100E, val & 0xFFFF);
        break;
    case 0x48:  // PE token with interrupt
        mmioSet16(0x100E, val & 0xFFFF);
        raiseInterrupt(IRQ_PE_TOKEN, [] { mmioSet16(0x100A, mmioGet16(0x100A) | 0x4); });
        break;
    case 0x49: s_copySrcTL = val; break;
    case 0x4A: s_copySrcWH = val; break;
    case 0x4B: s_copyDest = val; break;
    case 0x4D: s_copyStride = val; break;
    case 0x64: s_tlutSrc = val; break;
    case 0x65:  // TLUT load: from s_tlutSrc in main memory into TMEM (sent with the texture using it)
        tlutLoaded(val & 0x3FF, (s_tlutSrc & 0x1FFFFF) << 5, ((val >> 10) & 0x7FF) * 16);
        break;
    case 0x52: {  // EFB copy
        if ((val >> 14) & 1) {
            s_displayCopies++;
            if (s_framesByCopy) s_endAtDraw = true;  // (a copy in pieces is several copies in a row)
        }
        if (!((val >> 14) & 1)) {  // texture copy (display copies just present the EFB)
            aurora(AUR_COPY_SRC);
            put32(s_copySrcTL & 0x3FF);
            put32((s_copySrcTL >> 10) & 0x3FF);
            put32((s_copySrcWH & 0x3FF) + 1);
            put32(((s_copySrcWH >> 10) & 0x3FF) + 1);
            const bool half = (val >> 9) & 1;
            const u32 fmt = ((val >> 4) & 7) | (((val >> 3) & 1) << 3);
            aurora(AUR_COPY_DST);
            put32(((s_copySrcWH & 0x3FF) + 1) >> (half ? 1 : 0));
            put32((((s_copySrcWH >> 10) & 0x3FF) + 1) >> (half ? 1 : 0));
            put32(fmt);
            put8(half ? 1 : 0);
            aurora(AUR_COPY_DEST);
            put64(hostAddr((s_copyDest & 0x1FFFFF) << 5));
            s_copyDests.insert((s_copyDest & 0x1FFFFF) << 5);
            // A copy of less than the screen ends a render-to-texture pass (shadow textures:
            // 128 and 256 square in stages, 480 square in Chao World); its segment keeps plain
            // mapping. Copies of the whole screen (640 wide) capture the scene for screen effects.
            const u32 w = (s_copySrcWH & 0x3FF) + 1, h = ((s_copySrcWH >> 10) & 0x3FF) + 1;
            if ((w < 576 || h < 360) && s_segmentAt < s_out.size()) s_out[s_segmentAt] = 0;
        }
        put8(CMD_LOAD_BP);
        put32(v);
        beginSegment();
        return;
    }
    default: break;
    }
    put8(CMD_LOAD_BP);
    put32(v);
}

void translateCp(u32 reg, u32 v) {
    s_cp[reg & 0xFF] = v;
    s_cpSet.set(reg & 0xFF);
    if (reg == 0x50) s_vcdLo = v;
    else if (reg == 0x60) s_vcdHi = v;
    else if (reg >= 0x70 && reg < 0x78) s_vatA[reg & 7] = v;
    else if (reg >= 0x80 && reg < 0x88) s_vatB[reg & 7] = v;
    else if (reg >= 0x90 && reg < 0x98) s_vatC[reg & 7] = v;
    else if (reg == 0x30) s_cpMtxA = v;
    if (reg >= 0xA0 && reg < 0xB0) {  // array base: physical address, sent when a draw uses it
        s_arraysSet |= 1u << (reg & 0xF);
        s_arrayBase[reg & 0xF] = v;
        arrayBaseChanged(reg & 0xF);
        return;
    }
    if (reg >= 0xB0 && reg < 0xC0) {  // array stride
        s_arrayStride[reg & 0xF] = v & 0xFF;
        arrayBaseChanged(reg & 0xF);
    }
    put8(CMD_LOAD_CP);
    put8(reg);
    put32(v);
}

// The game's camera: the view matrix HAL's camera code last computed (gxViewMatrixAt), as an index
// into the frame's views (added when it changes).
std::array<float, 12> s_viewNow;
bool s_haveView = false;
s16 currentView() {
    if (!s_haveView) return -1;
    auto& v = s_frame->views;
    if (!v.empty() && v.back() == s_viewNow) return s16(v.size() - 1);
    for (size_t i = 0; i < v.size(); i++)
        if (v[i] == s_viewNow) return s16(i);
    if (v.size() >= 32000) return -1;
    v.push_back(s_viewNow);
    return s16(v.size() - 1);
}

// An XF load of [addr, addr+count) whose words start at `at` in the frame: the moving ones.
void recordXf(u32 addr, u32 count, u32 at) {
    if (s_noRecord) return;
    GxPatch::Kind kind;
    if (addr < 120) kind = GxPatch::PosMtx;             // position matrices (GX_PNMTX0-9)
    else if (addr < 0x100) kind = GxPatch::TexMtx;      // texture matrices
    else if (addr >= 0x400 && addr < 0x460) kind = GxPatch::NrmMtx;
    else if (addr >= 0x500 && addr < 0x600) kind = GxPatch::TexMtx;  // post-transform
    else if (addr >= 0x600 && addr < 0x680) kind = GxPatch::Light;
    else if (addr >= 0x1020 && addr <= 0x1026) kind = GxPatch::Proj;
    else return;
    GxPatch p{};
    p.kind = kind, p.at = at, p.count = count, p.stride = addr, p.key = p.key2 = tag(0, addr << 8 | count);
    if (kind == GxPatch::PosMtx) p.view = currentView();
    s_pendingXf.push_back(s_frame->patches.size());
    s_frame->patches.push_back(p);
}

// Whether a draw's texture coordinates 0 (sent with it, as floats) stay within one copy of the texture.
bool spriteWithinTexture(const u8* p, u32 count, u32 fmt) {
    const u32 lo = s_vcdLo, a = s_vatA[fmt];
    if ((s_vcdHi & 3) != 1 || ((a >> 22) & 7) != 4) return false;  // texcoord 0 direct, f32
    u32 off = 0;
    for (u32 i = 0; i < 9; i++) off += (lo >> i) & 1;
    auto direct = [&](u32 type, u32 bytes) { off += type == 1 ? bytes : type == 2 ? 1 : type == 3 ? 2 : 0; };
    direct((lo >> 9) & 3, ((a & 1) ? 3 : 2) * kComp[(a >> 1) & 7]);
    if (const u32 t = (lo >> 11) & 3) direct(t, (((a >> 9) & 1) ? 9 : 3) * kComp[(a >> 10) & 7]);
    direct((lo >> 13) & 3, kClr[(a >> 14) & 7]);
    direct((lo >> 15) & 3, kClr[(a >> 18) & 7]);
    const u32 comps = ((a >> 21) & 1) ? 2 : 1, vsize = vertexSize(fmt);
    for (u32 v = 0; v < count; v++)
        for (u32 c = 0; c < comps; c++) {
            const u8* q = p + v * vsize + off + 4 * c;
            const float x = __builtin_bit_cast(float, u32(q[0]) << 24 | u32(q[1]) << 16 | u32(q[2]) << 8 | q[3]);
            if (!(x >= -0.002f && x <= 1.002f)) return false;
        }
    return true;
}

// A draw: its model part (a display list's address; or its position array and first vertex), and
// the values loaded for it are named by that.
void nameDraw(const u8* p, u32 fmt) {
    if (s_noRecord) return;
    u64 part = 0;
    if (s_curDl) part = u64(s_curDl & 0x01FFFFFF) << 1 | 1;
    else if (const u32 posType = (s_vcdLo >> 9) & 3; posType >= 2) {
        u32 pre = 0;
        for (u32 i = 0; i < 9; i++) pre += (s_vcdLo >> i) & 1;
        const u32 idx = posType == 2 ? p[pre] : u32(p[pre]) << 8 | p[pre + 1];
        part = (u64(s_arrayBase[0] & 0x01FFFFFF) << 24 | idx) << 1;
    } else {
        // Positions sent with the draw (particles, effects): no lasting part, so draws of one
        // vertex format in the object count as copies of one part - paired by position.
        part = 1ull << 62 | u64(fmt) << 20 | (s_vcdLo & 0x1FFFF);
    }
    s_drawPart = s_drawGroup = 0;
    s_drawOcc = 0;
    if (part) {
        TaskScope& t = s_tasks.empty() ? s_noTask : s_tasks.back();
        const u32 n = t.parts[part]++;
        s_drawPart = mix(scopeKey(t) ^ mix(part ^ u64(n) << 56));
        s_drawGroup = mix(scopeKey(t) ^ mix(part)) | 1;
        s_drawOcc = n;
    }
    for (size_t n = 0; n < s_pendingXf.size(); n++) {
        GxPatch& x = s_frame->patches[s_pendingXf[n]];
        // A load that a later one overwrote before this draw (a reset to identity, then the real
        // matrix) never reached a draw: left alone, so it cannot take the real one's name.
        bool overwritten = false;
        for (size_t m = n + 1; m < s_pendingXf.size() && !overwritten; m++) {
            const GxPatch& y = s_frame->patches[s_pendingXf[m]];
            overwritten = y.stride <= x.stride && y.stride + y.count >= x.stride + x.count;
        }
        if (overwritten) {
            x.flat = true;
            continue;
        }
        if (s_drawPart) x.key = mix(s_drawPart ^ (u64(x.stride) << 8 | x.count)), x.group = s_drawGroup, x.occ = s_drawOcc;
    }
    s_drawXf.swap(s_pendingXf);
    s_pendingXf.clear();
}

// The whole GX state as it is now, as commands (the start of the next frame).
void buildPreamble(std::vector<u8>& out) {
    out.clear();
    // (Always, in-between frames or not: the renderer draws only the newest of the frames that came since it last drew,
    // and a frame without its state drawn after skipped ones had the wrong vertex formats - its commands went out of
    // step in Aurora: crashes after a stall, entering City Trial.)
    s_out.swap(out);
    for (u32 r = 0; r < 256; r++)
        if (s_cpSet[r] && !(r >= 0xA0 && r < 0xB0)) {  // array bases: each frame sends its own
            put8(CMD_LOAD_CP);
            put8(r);
            put32(s_cp[r]);
        }
    for (u32 a = 0; a < 0x1058;) {
        if (!s_xfSet[a]) {
            a++;
            continue;
        }
        u32 n = 0;
        while (a + n < 0x1058 && s_xfSet[a + n] && n < 64) n++;
        put8(CMD_LOAD_XF);
        put16(n - 1);
        put16(a);
        for (u32 i = 0; i < n; i++) put32(s_xf[a + i]);
        a += n;
    }
    for (u32 r = 0; r < 256; r++) {
        if (!s_bpSet[r]) continue;
        // Writes that do something rather than set state: draw done, tokens, copies, TLUT loads,
        // texture cache invalidation, the mask; and texture addresses (loaded below).
        if (r == 0x45 || r == 0x46 || r == 0x47 || r == 0x48 || r == 0x52 || r == 0x65 || r == 0x66 || r == 0x67 ||
            r == 0xFE || texMapOf(r, 0x14) >= 0)
            continue;
        put8(CMD_LOAD_BP);
        put32(r << 24 | (s_bp[r] & 0xFFFFFF));
    }
    for (int map = 0; map < 8; map++) {
        const u32 reg = map < 4 ? 0x94 + u32(map) : 0xB4 + u32(map - 4);
        if (!s_bpSet[reg]) continue;
        put8(CMD_LOAD_BP);
        put32(reg << 24 | (s_bp[reg] & 0xFFFFFF));
        loadTexture(map, s_bp[reg] & 0xFFFFFF);
    }
    s_out.swap(out);
}

u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
u32 be16(const u8* p) { return u32(p[0]) << 8 | p[1]; }

// Translates whole commands from [p, p+n); returns the bytes consumed (a partial command stays).
size_t parse(const u8* p, size_t n, int depth);

// A command longer than what has arrived (a draw's vertices, an XF load): the FIFO is parsed
// again only once it has all of it, not on every write.
size_t s_needBytes = 0;
// Kirby Air Ride's title: its background's right side is two strips that leave one triangle between
// them uncovered - (674,147) (586,186) (675,209) on the 4:3 screen, a black arrow at the 4:3
// screen's right edge. Filled with the background's own picture: a triangle of those three
// vertices (from the two strips' vertex data, with their texture coordinates) after the second.
size_t command(const u8* p, size_t n, int depth);
std::vector<u8> s_notchLow;  // the lower strip's tip and edge vertices, this frame
u64 s_notchFrame = ~0ull;
void fillTitleNotch(const u8* p, u32 count, u8 op, int depth) {
    if (count != 17 || (op & 0xF8) != 0x98) return;  // the strips: 17-vertex triangle strips
    auto near = [](float a, float b) { return std::fabs(a - b) <= 3.f; };
    const float* b = s_drawBox;
    const u32 vsize = vertexSize(op & 7);
    if (near(b[0], 362) && near(b[1], 675) && near(b[2], 186) && near(b[3], 485)) {  // the lower strip
        s_notchLow.assign(p + 3, p + 3 + 2 * vsize);  // its vertices 0 (the tip) and 1
        s_notchFrame = s_frames;
    } else if (near(b[0], 371) && near(b[1], 675) && near(b[2], -4) && near(b[3], 351) && s_notchFrame == s_frames &&
               s_notchLow.size() == 2 * vsize) {  // the upper strip: its vertex 5 is the corner above
        std::vector<u8> d{u8(0x90 | (op & 7)), 0, 6};  // triangles: both windings (one is culled)
        const u8* above = p + 3 + 5 * vsize;
        const u8* tip = s_notchLow.data();
        const u8* below = s_notchLow.data() + vsize;
        for (const u8* v : {above, tip, below, above, below, tip}) d.insert(d.end(), v, v + vsize);
        command(d.data(), d.size(), depth + 1);
    }
}

size_t needMore(size_t len, int depth) {
    if (depth == 0) s_needBytes = len;
    return 0;
}

size_t command(const u8* p, size_t n, int depth) {
    if (n < 1) return 0;
    const u8 op = p[0];
    switch (op) {
    case CMD_NOP: {  // padding (display lists are padded to 32 bytes): nothing to pass on
        size_t k = 1;
        while (k < n && p[k] == CMD_NOP) k++;
        return k;
    }
    case CMD_INVAL_VC:
    case CMD_METRICS:
        put8(op);
        return 1;
    case CMD_LOAD_CP:
        if (n < 6) return 0;
        translateCp(p[1], be32(p + 2));
        return 6;
    case CMD_LOAD_XF: {
        if (n < 5) return 0;
        const u32 count = (be16(p + 1)) + 1;
        const size_t len = 5 + 4 * size_t(count);
        if (n < len) return needMore(len, depth);
        const u32 addr = be16(p + 3);
        for (u32 i = 0; i < count; i++)
            if (addr + i < 0x1058) s_xf[addr + i] = be32(p + 5 + i * 4), s_xfSet.set(addr + i);
        recordXf(addr, count, u32(s_out.size()) + 5);
        putBytes(p, len);
        return len;
    }
    case CMD_INDX_A: case CMD_INDX_B: case CMD_INDX_C: case CMD_INDX_D: {
        // Indexed XF load: (index, length-1 << 12 | XF address) from arrays 12-15.
        if (n < 5) return 0;
        // Sent as the immediate load it amounts to, so the values are in the frame (to blend).
        const u32 array = 12 + (op - CMD_INDX_A) / 8;
        const u32 src = s_arrayBase[array] + be16(p + 1) * s_arrayStride[array], xa = be16(p + 3) & 0xFFF;
        const u32 count = (be16(p + 3) >> 12) + 1;
        put8(CMD_LOAD_XF);
        put16(count - 1);
        put16(xa);
        recordXf(xa, count, u32(s_out.size()));
        for (u32 i = 0; i < count; i++) {
            const u32 w = rd32(0x80000000u | ((src + i * 4) & 0x01FFFFFF));
            if (xa + i < 0x1058) s_xf[xa + i] = w, s_xfSet.set(xa + i);
            put32(w);
        }
        return 5;
    }
    case CMD_CALL_DL: {
        if (n < 9) return 0;
        const u32 addr = be32(p + 1), size = be32(p + 5);
        const u32 outer = s_curDl;
        s_curDl = addr;
        // A list recorded under a scope (gxHeightFieldEnter...) plays under it.
        const u32 a = 0x80000000u | (addr & 0x01FFFFFF);
        int height = 0, staticV = 0;
        for (const ScopedDl& d : s_scopedDls)
            if (a >= d.lo && a < d.hi) height += d.height, staticV += d.staticV || d.height;
        s_heightField += height, s_staticVertices += staticV;
        if (depth < 4) parse(hostp(0x80000000u | (addr & 0x01FFFFFF)), size, depth + 1);
        s_heightField -= height, s_staticVertices -= staticV;
        s_curDl = outer;
        return 9;
    }
    case CMD_LOAD_BP:
        if (n < 5) return 0;
        translateBp(be32(p + 1));
        return 5;
    default:
        if (op >= 0x80 && op < 0xC0) {
            if (n < 3) return 0;
            if (const u32 miss = missingArrays()) {
                static int s_warned = 0;
                if (s_warned++ < 5) log("[gx] draw %02X uses arrays %03X with no base (VCD %08X/%08X)\n", op, miss, s_vcdLo, s_vcdHi);
            }
            const u32 count = be16(p + 1);
            const size_t len = 3 + size_t(count) * vertexSize(op & 7);
            if (n < len) return needMore(len, depth);
            if (s_endAtDraw) {  // the first draw after the last frame's display copy
                s_endAtDraw = false;
                gxEndFrame();
            }
            if (s_skipDraws.count(s_drawIndex++)) return len;  // SA2_SKIP_DRAWS (debugging)
            // Too few vertices for one primitive draws nothing on the hardware. The game's model
            // code emits such draws (e.g. empty triangle strips); Aurora would turn them into a
            // bogus strip that the next draw merges into, drawing its triangles as a strip.
            const u32 prim = op & 0xF8;
            const u32 minCount = prim == 0x80 || prim == 0x88 ? 4 : prim >= 0xA8 && prim < 0xB8 ? 2 : prim == 0xB8 ? 1 : 3;
            if (count < minCount) return len;
            s_drawXf.clear();
            nameDraw(p + 3, op & 7);
            const size_t firstPatch = s_frame->patches.size();
            sizeArrays(p + 3, be16(p + 1), op & 7);
            s_draw2D = false;
            s_hideDraw = s_clipDraw = false;
            place2D(p + 3, count, op & 7);
            if (s_hideDraw) return len;
            // Cut at the 4:3 screen's sides: the game's scissor, a unit in from each side (Aurora
            // keeps a scissor of the whole screen covering the wide one), around this draw.
            const bool clip = s_clipDraw;
            if (clip) {
                const u32 tl = s_bp[0x20] & 0xFFFFFF, br = s_bp[0x21] & 0xFFFFFF;
                put8(CMD_LOAD_BP);
                put32(0x20u << 24 | (tl & ~0x7FF000u) | ((((tl >> 12) & 0x7FF) + 1) << 12));
                put8(CMD_LOAD_BP);
                put32(0x21u << 24 | (br & ~0x7FF000u) | ((((br >> 12) & 0x7FF) - 1) << 12));
            }
            if (s_draw2D) {  // 2D: what it uses is not blended - but its position matrices (GxPatch::ui)
                for (size_t i : s_drawXf) {
                    GxPatch& x = s_frame->patches[i];
                    x.flat = true;
                    x.ui = x.kind == GxPatch::PosMtx && x.count == 12;
                }
                for (size_t i = firstPatch; i < s_frame->patches.size(); i++) s_frame->patches[i].flat = true;
            }
            if (!s_noRecord && ((s_vcdLo >> 9) & 3) == 1) {  // positions sent with the draw (sprites, skinned meshes)
                const u32 a = s_vatA[op & 7];
                GxPatch v{};
                v.kind = GxPatch::Vertex, v.at = u32(s_out.size()) + 3, v.count = count, v.stride = vertexSize(op & 7);
                for (u32 i = 0; i < 9; i++) v.posOff += (s_vcdLo >> i) & 1;
                v.fmt = u8((a >> 1) & 7), v.comps = u8((a & 1) ? 3 : 2), v.shift = u8((a >> 4) & 31);
                v.key2 = tag(1, count << 8 | v.stride);
                v.key = s_drawPart ? mix(s_drawPart ^ 0x7E47) : v.key2;
                v.group = s_drawGroup, v.occ = s_drawOcc;
                v.flat = s_draw2D || (s_staticVertices > 0 && !s_heightField);
                v.height = s_heightField > 0;
                v.view = currentView();
                {  // the draw's position matrix: identity -> its positions are in the camera's space
                    const u32 m = ((s_vcdLo & 1) ? p[3] : (s_cpMtxA & 0x3F)) * 4;
                    bool id = m + 11 < 0x100;
                    for (u32 q = 0; id && q < 12; q++) id = xff(m + q) == ((q == 0 || q == 5 || q == 10) ? 1.f : 0.f);
                    v.viewSpace = id;
                }
                s_frame->patches.push_back(v);
            }
            // A 2D sprite drawing one copy of its texture (coordinates within 0..1) samples it clamped
            // at the edges: with the texture set to repeat, filtering at more than 640x480 would blend
            // in the opposite edge - lines between the tiles of backdrops and maps.
            const bool clampSprite = s_draw2D && spriteWithinTexture(p + 3, count, op & 7) && (s_bp[0x80] & 0xF);
            if (clampSprite) {
                put8(CMD_LOAD_BP);
                put32(0x80u << 24 | (s_bp[0x80] & 0xFFFFF0));
            }
            const size_t drawAt = s_out.size();
            putBytes(p, len);
            for (const auto& [off, x] : s_bleed) {  // (the copy: a display list in guest memory stays as it is)
                const u32 b = __builtin_bit_cast(u32, x);
                u8* q = s_out.data() + drawAt + 3 + off;
                q[0] = u8(b >> 24), q[1] = u8(b >> 16), q[2] = u8(b >> 8), q[3] = u8(b);
            }
            s_bleed.clear();
            if (clampSprite) {
                put8(CMD_LOAD_BP);
                put32(0x80u << 24 | (s_bp[0x80] & 0xFFFFFF));
            }
            fillTitleNotch(p, count, op, depth);
            if (clip) {  // the game's scissor again
                put8(CMD_LOAD_BP);
                put32(0x20u << 24 | (s_bp[0x20] & 0xFFFFFF));
                put8(CMD_LOAD_BP);
                put32(0x21u << 24 | (s_bp[0x21] & 0xFFFFFF));
            }
            if (s_smearCount) {  // the strips running it on to the wide screen's sides (place2D), their texture clamped
                const bool tex = s_genMode & 0xF;
                if (tex) {
                    put8(CMD_LOAD_BP);
                    put32(0x80u << 24 | (s_bp[0x80] & 0xFFFFF0));
                }
                const bool ownPos = !s_smearPos.empty();
                if (ownPos) {  // the strips' positions, for them alone (then the array again)
                    u8* copy = s_arena.alloc(u32(s_smearPos.size()));
                    std::memcpy(copy, s_smearPos.data(), s_smearPos.size());
                    aurora(u16(AUR_ARRAYBASE | 0));
                    put64(u64(reinterpret_cast<uintptr_t>(copy)));
                    put32(u32(s_smearPos.size()));
                    put8(0);
                }
                put8(u8(0x90 | (op & 7)));
                put16(s_smearCount);
                putBytes(s_smear.data(), s_smear.size());
                if (ownPos) {
                    aurora(u16(AUR_ARRAYBASE | 0));
                    put64(u64(reinterpret_cast<uintptr_t>(s_arrayCopy[0])));
                    put32(s_arraySent[0]);
                    put8(0);
                }
                if (tex) {
                    put8(CMD_LOAD_BP);
                    put32(0x80u << 24 | (s_bp[0x80] & 0xFFFFFF));
                }
                s_smear.clear(), s_smearCount = 0, s_smearPos.clear();
            }
            return len;
        }
        static int s_bad = 0;
        if (s_bad++ < 8) log("[gx] unknown command %02X (depth %d)\n", op, depth);
        return n;  // resync impossible: drop the rest
    }
}

size_t parse(const u8* p, size_t n, int depth) {
    size_t done = 0;
    while (done < n) {
        const size_t c = command(p + done, n - done, depth);
        if (!c) break;
        done += c;
    }
    return done;
}

// ---- write-gather pipe input ----
std::vector<u8> s_in;
size_t s_inPos = 0;
u64 s_gxCycles = 0;  // time stamp counter cycles spent translating (SA2_GX_TIMING)
const bool s_gxTiming = std::getenv("SA2_GX_TIMING") != nullptr;
u32 s_gpFifoBase = 0;  // CP FIFO base: writes go to the GPU while the CPU FIFO is the same one

bool redirectedToMemory() {
    const u32 piBase = mmioGet32(0x300C) & 0x03FFFFFF;  // (straight from the register store: every GX write asks)
    return s_gpFifoBase && piBase != s_gpFifoBase;
}

// ---- frame handoff to the render thread ----
std::mutex s_frameLock;
std::condition_variable s_frameCv;
std::deque<std::shared_ptr<GxFrame>> s_ready;
}  // namespace

void gxTaskEnter(u32 task, u32 fn) {
    const u32 occ = s_occurrences[u64(task) << 32 | fn]++;
    s_tasks.push_back({task, fn, occ, {0, 0, 0}, {}});
}
void gxTaskLeave() {
    if (!s_tasks.empty()) s_tasks.pop_back();
}

void gxWrite(u32 v, int size) {
    if (redirectedToMemory()) {  // display list recording: the CPU FIFO points into RAM
        // Past the FIFO's end (its last word) the write pointer wraps to the base and sets the
        // wrap flag, as the hardware's does: GXEndDisplayList reports a list that did not fit, and
        // nothing is written beyond the buffer (writing on ran into the next objects in memory).
        const u32 raw = rd32(0xCC003014);
        u32 wp = raw & 0x03FFFFFF, wrapped = raw & 0x04000000;
        const u32 base = rd32(0xCC00300C) & 0x03FFFFFF, end = rd32(0xCC003010) & 0x03FFFFFF;
        for (int i = size - 1; i >= 0; i--) {
            *hostp(0x80000000u | (wp++ & 0x01FFFFFF)) = u8(v >> (8 * i));
            if (end && wp > end) {
                wp = base;
                static int s_logged = 0;
                if (!wrapped && s_logged++ < 5) log("[gx] display list overflowed its buffer %08X-%08X\n", 0x80000000u | base, 0x80000000u | (end + 4));
                wrapped = 0x04000000;
            }
        }
        wr32(0xCC003014, wp | wrapped);
        return;
    }
    for (int i = size - 1; i >= 0; i--) s_in.push_back(u8(v >> (8 * i)));
    if (s_in.size() - s_inPos < s_needBytes) return;
    s_needBytes = 0;
    const u64 c0 = s_gxTiming ? __rdtsc() : 0;
    s_inPos += parse(s_in.data() + s_inPos, s_in.size() - s_inPos, 0);
    if (s_gxTiming) s_gxCycles += __rdtsc() - c0;
    if (s_inPos > (1u << 16)) {
        s_in.erase(s_in.begin(), s_in.begin() + ptrdiff_t(s_inPos));
        s_inPos = 0;
    }
}

void gxNoteCpRegister(u32 off, u32 v) {
    // CP FIFO base lo/hi (0xCC000020/22): remember which buffer the GP reads.
    if (off == 0x20) s_gpFifoBase = (s_gpFifoBase & 0xFFFF0000) | (v & 0xFFFF);
    else if (off == 0x22) s_gpFifoBase = (s_gpFifoBase & 0xFFFF) | ((v & 0x3FF) << 16);
}

void gxSetRecording(bool on) {
    static const bool s_never = std::getenv("SA2_NO_RECORD") != nullptr;
    s_noRecord = !on || s_never;
}

void gxFrameStarts() { s_frameStartVi = viLastRetrace(); }
void gxViewMatrixAt(u32 addr) {
    for (u32 i = 0; i < 12; i++) s_viewNow[i] = __builtin_bit_cast(float, rd32(addr + 4 * i));
    s_haveView = true;
}
// A scope that records a display list (the CPU FIFO pointed into memory): the scope's flags go with
// the list, to its playback.
namespace {
std::vector<u32> s_scopeDlStart;  // per open scope: where the CPU FIFO was writing (0: not recording)
u32 fifoWritePtr() { return 0x80000000u | (rd32(0xCC003014) & 0x01FFFFFF); }
void scopeEnter() { s_scopeDlStart.push_back(redirectedToMemory() ? fifoWritePtr() : 0); }
void scopeLeave(bool height) {
    const u32 start = s_scopeDlStart.empty() ? 0 : s_scopeDlStart.back();
    if (!s_scopeDlStart.empty()) s_scopeDlStart.pop_back();
    if (!start || !redirectedToMemory()) return;
    const u32 end = fifoWritePtr();
    if (end <= start) return;
    std::erase_if(s_scopedDls, [&](const ScopedDl& d) { return d.lo < end && start < d.hi; });  // rewritten
    s_scopedDls.push_back({start, end, height, !height});
    if (s_scopedDls.size() > 64) s_scopedDls.erase(s_scopedDls.begin());
}
}  // namespace
void gxHeightFieldEnter() { s_heightField++, scopeEnter(); }
void gxHeightFieldLeave() { s_heightField--, scopeLeave(true); }
void gxStaticVerticesEnter() { s_staticVertices++, scopeEnter(); }
void gxStaticVerticesLeave() { s_staticVertices--, scopeLeave(false); }

void gxFramesByDisplayCopy() {
    if (s_framesByCopy) return;
    s_framesByCopy = true;
    log("[gx] the game flips frames in an interrupt: frames end at their display copy\n");
}
bool gxFramesEndAtCopy() { return s_framesByCopy; }

void gxEndFrame() {
    gameFixesFrame();
    s_frames++;
    // SA2_SLOW=<from>:<to>:<ms> (timing experiments): the game takes that much longer per frame there.
    static unsigned s_slow[3] = {0, 0, 0};
    static const bool s_slowSet = std::getenv("SA2_SLOW") && std::sscanf(std::getenv("SA2_SLOW"), "%u:%u:%u", &s_slow[0], &s_slow[1], &s_slow[2]) == 3;
    if (s_slowSet && s_frames >= s_slow[0] && s_frames <= s_slow[1]) std::this_thread::sleep_for(std::chrono::milliseconds(s_slow[2]));
    // SA2_PACE_FILE=<file> SA2_PACE_REL=<module> SA2_PACE_FROM=<frames after link> (timing
    // experiments): from then on, frame k is held until it has taken the k-th number of retraces in
    // the file (one per line - e.g. a reference run's frame durations).
    if (const char* pf = std::getenv("SA2_PACE_FILE")) {
        static std::vector<u32> pace;
        static bool loaded = false;
        static u64 lastVi = 0;
        if (!loaded) {
            loaded = true;
            if (std::FILE* f = std::fopen(pf, "r")) {
                unsigned v;
                while (std::fscanf(f, "%u", &v) == 1) pace.push_back(v);
                std::fclose(f);
            }
            log("[pace] %zu frame durations from %s\n", pace.size(), pf);
        }
        static const int rel = std::getenv("SA2_PACE_REL") ? std::atoi(std::getenv("SA2_PACE_REL")) : 80;
        static const u64 from = std::getenv("SA2_PACE_FROM") ? std::strtoull(std::getenv("SA2_PACE_FROM"), nullptr, 10) : 0;
        const u64 link = g_relLinkFrame[rel & 255].load();
        if (link && s_frames >= link - 1 + from && s_frames - (link - 1 + from) < pace.size()) {
            const u32 want = pace[s_frames - (link - 1 + from)];
            sched::withCpuReleased([&] {
                while (viRetraces() < lastVi + want) std::this_thread::sleep_for(std::chrono::microseconds(500));
            });
        }
        lastVi = viRetraces();
    }
    // SA2_PEEK=<hex addr>,...[@<every>] (timing experiments): those words (as ints and floats) every
    // <every> frames (default 1), with the time and the VI retrace count.
    if (const char* pk = std::getenv("SA2_PEEK")) {
        static std::vector<u32> addrs;
        static u32 every = 1;
        if (addrs.empty()) {
            for (const char* p = pk; *p && *p != '@';) {
                char* end;
                addrs.push_back(u32(std::strtoul(p, &end, 16)));
                p = *end == ',' ? end + 1 : end;
            }
            if (const char* at = std::strchr(pk, '@')) every = std::max(1u, u32(std::atoi(at + 1)));
        }
        if (s_frames % every == 0) {
            std::string s;
            char b[64];
            for (u32 a : addrs) {
                const u32 v = rd32(a);
                std::snprintf(b, sizeof b, " %08X=%d/%.3f", a, s32(v), __builtin_bit_cast(float, v));
                s += b;
            }
            log("[peek] frame %llu t %.3f vi %llu:%s\n", (unsigned long long)s_frames, gxNow(), (unsigned long long)viRetraceCount(), s.c_str());
        }
    }
    // SA2_SCAN=<f1>:<f2>:<f3> (timing experiments): MEM1 words that grow steadily over those frames -
    // counters (ints) and clocks (floats) - with how much they grew in each interval, and the time.
    if (const char* sc = std::getenv("SA2_SCAN")) {
        static unsigned f[3] = {0, 0, 0};
        static const bool ok = std::sscanf(sc, "%u:%u:%u", &f[0], &f[1], &f[2]) == 3;
        static std::vector<u32> snap[3];
        static double when[3];
        for (int k = 0; ok && k < 3; k++)
            if (s_frames == f[k]) {
                snap[k].resize(0x01800000 / 4);
                for (u32 i = 0; i < snap[k].size(); i++) snap[k][i] = rd32(0x80000000u + i * 4);
                when[k] = gxNow();
                if (k == 2) {
                    log("[scan] frames %u, %u, %u at %.3f s apart, %.3f s\n", f[0], f[1], f[2], when[1] - when[0], when[2] - when[1]);
                    u32 shown = 0;
                    for (u32 i = 0; i < snap[0].size() && shown < 6000; i++) {
                        const u32 a = snap[0][i], b = snap[1][i], c = snap[2][i];
                        if (a == b || b == c) continue;
                        const s64 di = s64(s32(b)) - s32(a), dj = s64(s32(c)) - s32(b);
                        const float fa = __builtin_bit_cast(float, a), fb = __builtin_bit_cast(float, b), fc = __builtin_bit_cast(float, c);
                        const bool intLike = di > 0 && dj > 0 && di < 100000 && dj < 100000;
                        const bool floatLike = std::isfinite(fa) && std::isfinite(fc) && fb > fa && fc > fb && fb - fa < 1e6f &&
                                               std::fabs(fa) > 1e-3f && std::fabs(fa) < 1e7f;
                        if (!intLike && !floatLike) continue;
                        log("[scan] %08X: int +%lld +%lld | float %.4f +%.4f +%.4f\n", 0x80000000u + i * 4, (long long)di, (long long)dj, fa,
                            fb - fa, fc - fb);
                        shown++;
                    }
                    for (auto& s : snap) std::vector<u32>().swap(s);
                }
            }
    }
    // SA2_GX_TIMING: the game thread's time translating GX commands, and the frame handoff.
    static const bool s_timing = std::getenv("SA2_GX_TIMING") != nullptr;
    static u64 s_tc0 = __rdtsc();
    static double s_wall0 = gxNow();
    static u64 s_endCycles = 0;
    const u64 e0 = __rdtsc();
    struct EndTimer {
        u64 e0;
        ~EndTimer() { s_endCycles += __rdtsc() - e0; }
    } endTimer{e0};
    // The game's own threads' CPU time per frame: how close the game runs to its 16.7 ms.
    static u64 s_busy0 = 0, s_busyMax = 0, s_busySum = 0;
    static u32 s_busyOver = 0;
    if (s_timing) {
        const u64 b = profileGuestCycles();
        if (s_busy0) s_busySum += b - s_busy0, s_busyMax = std::max(s_busyMax, b - s_busy0), s_busyOver += (b - s_busy0) > 35000000;
        s_busy0 = b;
    }
    if (s_timing && s_frames % 300 == 0) {
        const double wall = gxNow() - s_wall0;
        const double hz = double(__rdtsc() - s_tc0) / std::max(wall, 1e-6);
        log("[gx] last 300 frames: translating %.2f ms/frame, handoff %.2f ms/frame (%.0f fps); game CPU %.2f ms/frame, max %.2f, "
            "%u over 14 ms\n",
            double(s_gxCycles) / hz * 1000 / 300, double(s_endCycles) / hz * 1000 / 300, 300 / std::max(wall, 1e-6),
            double(s_busySum) / hz * 1000 / 300, double(s_busyMax) / hz * 1000, s_busyOver);
        s_gxCycles = s_endCycles = 0;
        s_busySum = s_busyMax = 0, s_busyOver = 0;
        profileLogThreads(hz);
        s_tc0 = __rdtsc();
        s_wall0 = gxNow();
    }
    if (s_out.size() > (2u << 20) || s_displayCopies > 1)
        log("[gx] frame %llu: %zu bytes, %u display copies\n", (unsigned long long)s_frames, s_out.size(), s_displayCopies);
    s_displayCopies = 0;
    std::shared_ptr<GxFrame> f = std::move(s_frame);
    f->cmds.swap(s_out);
    s_out.clear();
    for (auto& chunk : s_arena.chunks) f->snapshots.push_back(std::move(chunk));
    s_arena = {};
    f->index = s_frames;
    {  // the main camera: the one most 3D position matrices were loaded under
        std::vector<u32> n(f->views.size(), 0);
        for (const GxPatch& p : f->patches)
            if (p.kind == GxPatch::PosMtx && p.view >= 0 && !p.flat) n[size_t(p.view)]++;
        if (!n.empty()) f->mainView = int(std::max_element(n.begin(), n.end()) - n.begin());
    }
    f->time = gxNow();
    f->vi = s_frameStartVi > 0 ? s_frameStartVi : viLastRetrace();
    std::unique_lock lk(s_frameLock);
    // The game never waits for the display: it runs at the console's 60 Hz whatever the monitor
    // does (some of its timing follows the music clock, some the frame count; holding frames back
    // pulled them apart in cutscenes). Only when the renderer has fallen several frames behind
    // (a stall, a minimised window) is the game held, briefly, with the guest CPU released.
    if (s_ready.size() >= 6) {
        lk.unlock();
        sched::withCpuReleased([&] {
            std::unique_lock w(s_frameLock);
            s_frameCv.wait_for(w, std::chrono::milliseconds(50), [] { return s_ready.size() < 6; });
        });
        lk.lock();
    }
    s_ready.push_back(std::move(f));
    lk.unlock();
    // The next frame: the state it starts from, then its commands.
    s_frame = std::make_shared<GxFrame>();
    buildPreamble(s_frame->preamble);
    s_tasks.clear();
    s_noTask = {0, 0, 0, {0, 0, 0}, {}};
    s_occurrences.clear();
    s_pendingXf.clear();
    std::fill(std::begin(s_arraySent), std::end(s_arraySent), 0u);  // next frame snapshots afresh
    std::fill(std::begin(s_arrayCopy), std::end(s_arrayCopy), nullptr);
    std::fill(std::begin(s_arrayShown), std::end(s_arrayShown), nullptr);
    s_arraySnaps.clear();
    s_drawIndex = 0;
    s_copyDests.clear();
    s_backdropDepths[0].swap(s_backdropDepths[1]);
    s_backdropDepths[1].clear();
    beginSegment();  // the next frame starts drawing to the screen
    // SA2_WIDE_LOG=<game frame> or r<module>:<frames after it linked>: log that frame's placements;
    // <from>-<to>: those frames' flat draws (2D candidates).
    if (const char* wl = std::getenv("SA2_WIDE_LOG")) {
        if (wl[0] == 'r') {
            const u32 mod = u32(std::atoi(wl + 1));
            const char* c = std::strchr(wl, ':');
            const u64 link = g_relLinkFrame[mod & 255].load();
            if (link && c) s_wideLogFrame = link - 1 + std::strtoull(c + 1, nullptr, 10);
            if (const char* d = c ? std::strchr(c, '-') : nullptr; link && d) s_wideLogTo = link - 1 + std::strtoull(d + 1, nullptr, 10);
        } else if (wl[0] == 's') {  // s<scene>:<frames>: that many frames into the game's major scene (hex; 0xD: results)
            static u64 s_sceneSince = 0;
            const u32 scene = u32(std::strtoul(wl + 1, nullptr, 16));
            const char* c = std::strchr(wl, ':');
            if (rd8(kGmScene) == scene) {
                if (!s_sceneSince) s_sceneSince = s_frames;
                if (c && s_wideLogFrame == ~0ull) s_wideLogFrame = s_sceneSince + std::strtoull(c + 1, nullptr, 10);
            } else s_sceneSince = 0;
        } else if (wl[0] == 'e') {  // e<n>: every n-th game frame
            const u64 n = std::max<u64>(1, std::strtoull(wl + 1, nullptr, 10));
            s_wideLogFrame = (s_frames + n) / n * n;
            if (s_frames % n == 0) s_wideLogFrame = s_frames;
        } else {
            s_wideLogFrame = std::strtoull(wl, nullptr, 10);
            if (const char* d = std::strchr(wl, '-')) s_wideLogTo = std::strtoull(d + 1, nullptr, 10);
        }
    }
    s_frameCv.notify_all();
}

bool gxTakeFrames(std::vector<std::shared_ptr<GxFrame>>& out, int waitMs) {
    out.clear();
    std::unique_lock lk(s_frameLock);
    if (!s_frameCv.wait_for(lk, std::chrono::milliseconds(waitMs), [] { return !s_ready.empty(); })) return false;
    for (auto& f : s_ready) out.push_back(std::move(f));
    s_ready.clear();
    s_frameCv.notify_all();
    return true;
}

u64 gxFrames() { return s_frames; }

double gxNow() {
    static const auto s_t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - s_t0).count();
}

}  // namespace rt

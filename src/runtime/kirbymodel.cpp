// Kirby's model, from RdKirby.dat on the player's disc (src/runtime/kirbymodel.h).
//
// RdKirby.dat is a HAL archive (header, data, relocations, then the root symbols); its root rdDataKirby points
// at the model: {joint tree, material animation, ..., parts count +C, detail tables +10 (highest first)}. A
// detail table lists, per part, its options: lists of the model's meshes (DObjs, numbered in the joint tree's
// order). Part 0's first option is Kirby's round body with his arms (the others: squashed for riding, the
// inhale), part 1 his feet; the rest are ability pieces, not drawn here.
//
// The meshes are HSD's: joint (JObj) -> mesh (DObj) -> material (MObj, its textures TObj) + shapes (PObj: vertex
// format, display list, skin). Skinned vertices with one weight are in their joint's space; with more, in the
// model's (each joint's world matrix times its inverse bind).
//
// Pose: the first motion of RdKirbyMotionWalk.dat - Kirby standing (the game's wait), looped at 60 frames a second -
// with its script's eye looks (its blink). Its key values are little-endian (see decodeTrack).
//
// Textures go through HSD's texture matrix (TObj scale, translation and repeat): the eyes are drawn from half
// their picture's width.
//
// Colours: the material animation (a tree beside the joints, one entry per mesh) swaps the textures: texture 0
// of the body, the arms and the feet has an image per colour (the game's first eight); texture 1 of the eyes,
// their looks (open, half, shut, ...) - the blink.
//
// The picture is drawn by a small rasteriser (2x2 samples a pixel, depth-buffered, perspective-correct) at up to
// 60 frames a second while it is shown, into one texture updated in place.
#include "runtime/kirbymodel.h"
#include "runtime/menustyle.h"
#include "runtime/runtime.h"

#include <aurora/imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace rt {
bool dvdReadFile(const std::string& path, std::vector<u8>& out);  // src/runtime/hle_dvd.cpp
}

namespace rt::kirbymodel {

namespace {

// ---- the archive ----
struct Dat {
    const u8* d = nullptr;
    u32 size = 0;
    u32 u32at(u32 o) const { return o + 4 <= size ? u32(d[o]) << 24 | u32(d[o + 1]) << 16 | u32(d[o + 2]) << 8 | d[o + 3] : 0; }
    u16 u16at(u32 o) const { return o + 2 <= size ? u16(d[o] << 8 | d[o + 1]) : 0; }
    u8 u8at(u32 o) const { return o < size ? d[o] : 0; }
    float f32(u32 o) const {
        const u32 v = u32at(o);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    bool ok(u32 o, u32 n) const { return o && o + n <= size && o + n >= o; }
};

// ---- textures ----
struct Image {
    int w = 0, h = 0;
    std::vector<u32> px;  // RGBA8 (r in the low byte)
};

u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | g << 8 | b << 16 | a << 24; }
u32 c565(u16 v) { return rgba((v >> 11) * 255 / 31, (v >> 5 & 63) * 255 / 63, (v & 31) * 255 / 31, 255); }
u32 colour16(u16 v, u32 palFmt) {
    if (palFmt == 0) return rgba(v & 255, v & 255, v & 255, v >> 8);  // IA8
    if (palFmt == 1) return c565(v);
    if (v & 0x8000) return rgba((v >> 10 & 31) * 255 / 31, (v >> 5 & 31) * 255 / 31, (v & 31) * 255 / 31, 255);  // RGB5A3
    return rgba((v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17, (v >> 12 & 7) * 255 / 7);
}
u32 mix(u32 a, u32 b, int wa, int wb, int div) {
    u32 o = 0;
    for (int s = 0; s < 32; s += 8) o |= ((((a >> s) & 255) * wa + ((b >> s) & 255) * wb) / div) << s;
    return o;
}

// An image descriptor {texels, u16 w, u16 h, u32 format} with its palette descriptor {colours, u32 format, u32
// name, u16 count} (0 if none): CMPR, C4, C8, I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8.
bool decode(const Dat& f, u32 img, u32 tlut, Image& out) {
    if (!f.ok(img, 12)) return false;
    const u32 data = f.u32at(img), fmt = f.u32at(img + 8);
    const int w = f.u16at(img + 4), h = f.u16at(img + 6);
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return false;
    std::vector<u32> pal;
    if (fmt == 8 || fmt == 9) {
        if (!f.ok(tlut, 14)) return false;
        const u32 pd = f.u32at(tlut), pf = f.u32at(tlut + 4), n = f.u16at(tlut + 0xC);
        if (!f.ok(pd, n * 2)) return false;
        for (u32 i = 0; i < n; i++) pal.push_back(colour16(f.u16at(pd + 2 * i), pf));
    }
    const int bw = fmt == 3 || fmt == 4 || fmt == 5 || fmt == 6 ? 4 : 8, bh = fmt == 0 || fmt == 8 || fmt == 14 ? 8 : 4;
    const int bits = fmt == 0 || fmt == 8 || fmt == 14 ? 4 : fmt == 1 || fmt == 2 || fmt == 9 ? 8 : fmt == 6 ? 32 : 16;
    const u32 bytes = u32((w + bw - 1) / bw) * u32((h + bh - 1) / bh) * u32(bw * bh * bits / 8);
    if (!f.ok(data, bytes)) return false;
    out.w = w, out.h = h;
    out.px.assign(size_t(w) * h, 0);
    auto put = [&](int x, int y, u32 c) {
        if (x < w && y < h) out.px[size_t(y) * w + x] = c;
    };
    u32 o = data;
    for (int by = 0; by < h; by += bh)
        for (int bx = 0; bx < w; bx += bw) {
            if (fmt == 14) {  // CMPR: 2x2 sub-blocks of DXT1
                for (int sb = 0; sb < 4; sb++, o += 8) {
                    const u16 a = f.u16at(o), b = f.u16at(o + 2);
                    const u32 bitsv = f.u32at(o + 4);
                    u32 cs[4] = {c565(a), c565(b), 0, 0};
                    if (a > b) cs[2] = mix(cs[0], cs[1], 2, 1, 3), cs[3] = mix(cs[0], cs[1], 1, 2, 3);
                    else cs[2] = mix(cs[0], cs[1], 1, 1, 2), cs[3] = 0;
                    for (int i = 0; i < 16; i++) put(bx + (sb & 1) * 4 + i % 4, by + (sb >> 1) * 4 + i / 4, cs[(bitsv >> (30 - 2 * i)) & 3]);
                }
                continue;
            }
            if (fmt == 6) {  // RGBA8: AR then GB halves
                for (int i = 0; i < 16; i++) {
                    const u32 ar = f.u16at(o + 2 * i), gb = f.u16at(o + 32 + 2 * i);
                    put(bx + i % 4, by + i / 4, rgba(ar & 255, gb >> 8, gb & 255, ar >> 8));
                }
                o += 64;
                continue;
            }
            for (int y = 0; y < bh; y++)
                for (int x = 0; x < bw; x++) {
                    const int i = y * bw + x;
                    u32 c = 0;
                    switch (fmt) {
                    case 0: { const u32 v = ((f.u8at(o + i / 2) >> (i % 2 ? 0 : 4)) & 15) * 17; c = rgba(v, v, v, 255); break; }
                    case 1: { const u32 v = f.u8at(o + i); c = rgba(v, v, v, 255); break; }
                    case 2: { const u32 b = f.u8at(o + i); c = rgba((b & 15) * 17, (b & 15) * 17, (b & 15) * 17, (b >> 4) * 17); break; }
                    case 3: { const u32 v = f.u16at(o + 2 * i); c = rgba(v & 255, v & 255, v & 255, v >> 8); break; }
                    case 4: c = c565(f.u16at(o + 2 * i)); break;
                    case 5: c = colour16(f.u16at(o + 2 * i), 2); break;
                    case 8: { const u32 k = (f.u8at(o + i / 2) >> (i % 2 ? 0 : 4)) & 15; c = k < pal.size() ? pal[k] : 0; break; }
                    case 9: { const u32 k = f.u8at(o + i); c = k < pal.size() ? pal[k] : 0; break; }
                    default: return false;
                    }
                    put(bx + x, by + y, c);
                }
            o += u32(bw * bh * bits / 8);
        }
    return true;
}

// ---- the model ----
using Mtx = std::array<float, 12>;  // 3x4, row-major
Mtx identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}; }
Mtx mul(const Mtx& a, const Mtx& b) {
    Mtx o{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            o[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + (j == 3 ? a[i * 4 + 3] : 0.f);
    return o;
}
Mtx srt(const float r[3], const float s[3], const float t[3]) {  // T * Rz * Ry * Rx * S (HSD's joints)
    const float cx = std::cos(r[0]), sx = std::sin(r[0]), cy = std::cos(r[1]), sy = std::sin(r[1]), cz = std::cos(r[2]), sz = std::sin(r[2]);
    const float R[9] = {cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz, cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz, -sy, sx * cy, cx * cy};
    Mtx m;
    for (int i = 0; i < 3; i++) m[i * 4] = R[i * 3] * s[0], m[i * 4 + 1] = R[i * 3 + 1] * s[1], m[i * 4 + 2] = R[i * 3 + 2] * s[2], m[i * 4 + 3] = t[i];
    return m;
}
void xf(const Mtx& m, const float v[3], float w, float out[3]) {
    for (int i = 0; i < 3; i++) out[i] = m[i * 4] * v[0] + m[i * 4 + 1] * v[1] + m[i * 4 + 2] * v[2] + m[i * 4 + 3] * w;
}

// A vertex as the model has it: in its matrix slot's space (a joint's, or the model's for a blend of joints).
struct Vtx {
    float p[3], n[3], uv[2][2];
    int slot;
};
struct Tex {
    int map = 0, coord = 0;  // GX texture map; texture coordinate (0, 1)
    int colormap = 0;        // HSD's: 1 alpha mask, 4 modulate, 5 replace, ...
    int wrapS = 0, wrapT = 0;
    float mulS = 1, mulT = 1, offS = 0, offT = 0;  // the texture matrix: (uv - offset) * mul
    std::vector<Image> frames;  // [0]: its own image; then the animation's images (colours or looks)
    int colourFrames = 0;       // (texture 0's: an image per colour)
};
struct Mesh {
    std::vector<Tex> texs;
    std::vector<Vtx> tris;  // 3 a triangle
    bool eyes = false;      // (its texture 1: the eyes' looks)
};

struct Joint {
    u32 addr;
    int parent;
    float r[3], s[3], t[3];  // its bind pose
    bool hasInv = false;
    Mtx inv{};  // inverse bind
};

// A matrix slot: a joint's world matrix, or a weighted blend of joints' (each times its inverse bind).
struct Slot {
    std::vector<std::pair<int, float>> ws;
    bool single = false;
};

// ---- animation (HSD figatrees) ----
// A track is a stream of keys: a header byte (interpolation in its low 4 bits, the key count - 1 above, 7 bits a
// byte while the top bit is set), then per key a value and/or a slope and the frames to the next. Kirby's files
// keep 16-bit and float values little-endian (the bytes swapped from the GameCube's order).
enum : u8 { kNone, kCon, kLin, kSpl0, kSpl, kSlp, kKey };
struct Key {
    float f, v, tin, tout;
    u8 op;
};
struct Track {
    u8 type;  // 1-3 rotation, 5-7 translation, 8-10 scale (x, y, z)
    std::vector<Key> keys;
};
struct Motion {
    float frames = 0;
    std::vector<std::vector<Track>> joints;   // by joint, in the tree's order
    std::vector<std::pair<float, int>> eyes;  // the script's eye looks: (from frame, look)
};

float animValue(const Dat& f, u32& o, u8 flags) {
    const u32 fmt = flags >> 5;
    const float scale = 1.f / float(1u << (flags & 31));
    switch (fmt) {
    case 0: {
        const u32 v = u32(f.u8at(o)) | u32(f.u8at(o + 1)) << 8 | u32(f.u8at(o + 2)) << 16 | u32(f.u8at(o + 3)) << 24;
        o += 4;
        float x;
        std::memcpy(&x, &v, 4);
        return x;
    }
    case 1: { const s16 v = s16(f.u8at(o) | f.u8at(o + 1) << 8); o += 2; return float(v) * scale; }
    case 2: { const u16 v = u16(f.u8at(o) | f.u8at(o + 1) << 8); o += 2; return float(v) * scale; }
    case 3: { const s8 v = s8(f.u8at(o)); o += 1; return float(v) * scale; }
    default: { const u8 v = f.u8at(o); o += 1; return float(v) * scale; }
    }
}
u32 packed(const Dat& f, u32& o) {
    u32 v = 0;
    for (int s = 0; s < 28; s += 7) {
        const u8 b = f.u8at(o++);
        v |= u32(b & 0x7F) << s;
        if (!(b & 0x80)) break;
    }
    return v;
}
std::vector<Key> decodeTrack(const Dat& f, u32 o, u32 len, u8 vflag, u8 tflag) {
    std::vector<Key> keys;
    const u32 end = o + len;
    float frame = 0;
    while (o < end && keys.size() < 4096) {
        u8 h = f.u8at(o++);
        const u8 op = h & 15;
        u32 n = (h >> 4) & 7;
        for (int s = 3; h & 0x80 && s < 24; s += 7) h = f.u8at(o++), n |= u32(h & 0x7F) << s;
        n++;
        if (op == kNone || op > kKey) break;
        for (u32 i = 0; i < n && o < end; i++) {
            float v = keys.empty() ? 0.f : keys.back().v, tan = 0;
            if (op == kSlp) {  // (a slope only: the previous key's, leaving it)
                tan = animValue(f, o, tflag);
                if (!keys.empty()) keys.back().tout = tan;
                continue;
            }
            v = animValue(f, o, vflag);
            if (op == kSpl) tan = animValue(f, o, tflag);
            keys.push_back({frame, v, tan, tan, op});
            if (op != kKey) frame += float(packed(f, o));
        }
    }
    return keys;
}
float evalTrack(const std::vector<Key>& k, float t) {
    if (t <= k[0].f || k.size() == 1) return k[0].v;
    for (size_t i = 0; i + 1 < k.size(); i++) {
        const Key &a = k[i], &b = k[i + 1];
        if (t >= b.f) continue;
        const float L = b.f - a.f;
        if (L <= 0 || a.op == kCon || a.op == kKey) return a.v;
        const float u = (t - a.f) / L;
        if (a.op == kLin) return a.v + (b.v - a.v) * u;
        const float u2 = u * u, u3 = u2 * u;  // Hermite, its slopes per frame
        return (2 * u3 - 3 * u2 + 1) * a.v + (-2 * u3 + 3 * u2) * b.v + ((u3 - 2 * u2 + u) * a.tout + (u3 - u2) * b.tin) * L;
    }
    return k.back().v;
}

// RdKirbyMotionWalk.dat's root (rdMotionKirbyWalk) lists the motions on foot, {figatree, script, flags} each; the
// first is Kirby standing (the wait). Its script: words with the command in the top 6 bits - 1 wait n frames, 2
// wait until frame n, 0x17 a look (byte 1: 4 the eyes, 8 the mouth; the look in the low half), 0 the end.
bool loadMotion(Motion& m, size_t jointCount) {
    std::vector<u8> file;
    if (!dvdReadFile("RdKirbyMotionWalk.dat", file) || file.size() < 0x20) return false;
    auto be = [&](size_t o) { return o + 4 <= file.size() ? u32(file[o]) << 24 | u32(file[o + 1]) << 16 | u32(file[o + 2]) << 8 | file[o + 3] : 0u; };
    const u32 dataSize = be(4), nrel = be(8), nroot = be(12);
    const size_t roots = 0x20 + size_t(dataSize) + size_t(nrel) * 4;
    if (roots + size_t(nroot) * 8 > file.size() || nroot < 1) return false;
    Dat f{file.data() + 0x20, dataSize};
    const u32 entry = be(roots), tree = f.u32at(entry), script = f.u32at(entry + 4);
    if (!f.ok(tree, 0x14) || f.u32at(tree) != 1) return false;
    m.frames = f.f32(tree + 8);
    std::vector<u8> counts;
    for (u32 o = f.u32at(tree + 0xC); f.ok(o, 1) && f.u8at(o) != 0xFF && counts.size() < 256; o++) counts.push_back(f.u8at(o));
    if (counts.size() != jointCount || !(m.frames > 0)) return false;
    u32 tr = f.u32at(tree + 0x10);
    for (u8 c : counts) {
        std::vector<Track> js;
        for (u8 i = 0; i < c; i++, tr += 0xC) {
            if (!f.ok(tr, 0xC)) return false;
            const u32 len = f.u16at(tr), data = f.u32at(tr + 8);
            if (!f.ok(data, len)) return false;
            Track t{f.u8at(tr + 4), decodeTrack(f, data, len, f.u8at(tr + 5), f.u8at(tr + 6))};
            if (!t.keys.empty()) js.push_back(std::move(t));
        }
        m.joints.push_back(std::move(js));
    }
    float at = 0;
    for (u32 o = script; f.ok(o, 4) && o < script + 0x400; o += 4) {
        const u32 w = f.u32at(o), cmd = w >> 26;
        if (cmd == 0) break;
        if (cmd == 1) at += float(w & 0xFFFFFF);
        else if (cmd == 2) at = float(w & 0xFFFFFF);
        else if (cmd == 0x17 && (w >> 16 & 0xFF) == 4) m.eyes.push_back({at, int(w & 0xFFFF)});
        else if (cmd != 0x17) break;  // (others: not in the wait's script)
    }
    return true;
}

struct Model {
    std::vector<Mesh> meshes;
    std::vector<Joint> joints;
    std::vector<Slot> slots;
    Motion motion;
    bool animated = false;
    float centre[3]{}, ext = 1;  // the framing (over the whole motion)
};

struct Loader {
    const Dat& f;
    Model& model;
    std::vector<std::pair<u32, int>> dobjs;  // (DObj, joint index), in the tree's order
    std::vector<u32> matanims;               // per DObj: its material animation (0 if none)

    int jointOf(u32 addr) const {
        for (size_t i = 0; i < model.joints.size(); i++)
            if (model.joints[i].addr == addr) return int(i);
        return -1;
    }
    void walk(u32 j, int parent, int depth) {
        for (int guard = 0; j && f.ok(j, 0x40) && depth < 64 && guard < 256; guard++) {
            const u32 flags = f.u32at(j + 4);
            Joint J{j, parent};
            for (int k = 0; k < 3; k++) J.r[k] = f.f32(j + 0x14 + 4 * k), J.s[k] = f.f32(j + 0x20 + 4 * k), J.t[k] = f.f32(j + 0x2C + 4 * k);
            if (const u32 inv = f.u32at(j + 0x38); f.ok(inv, 48)) {
                J.hasInv = true;
                for (int k = 0; k < 12; k++) J.inv[size_t(k)] = f.f32(inv + 4 * u32(k));
            }
            const int ji = int(model.joints.size());
            model.joints.push_back(J);
            if (!(flags & 0x4000))  // (not a spline / particle joint)
                for (u32 d = f.u32at(j + 0x10), g = 0; d && f.ok(d, 0x10) && g < 256; d = f.u32at(d + 4), g++) dobjs.push_back({d, ji});
            if (!(flags & 0x1000)) walk(f.u32at(j + 8), ji, depth + 1);  // (an instance's child is not its own)
            j = f.u32at(j + 0xC);
        }
    }
    void walkAnim(u32 j, int depth) {  // MatAnimJoint {child, next, MatAnim chain}: one MatAnim per DObj
        for (int guard = 0; j && f.ok(j, 12) && depth < 64 && guard < 256; guard++) {
            for (u32 m = f.u32at(j + 8), g = 0; m && f.ok(m, 0x10) && g < 256; m = f.u32at(m), g++) matanims.push_back(m);
            walkAnim(f.u32at(j), depth + 1);
            j = f.u32at(j + 4);
        }
    }
    int slotOf(Slot s) {
        model.slots.push_back(std::move(s));
        return int(model.slots.size() - 1);
    }

    // A vertex attribute's value (position, normal, texture coordinate) at an index.
    struct Attr {
        u32 attr, type, cnt, ctype, frac, stride, data;
    };
    static int comps(const Attr& a) {
        if (a.attr == 9) return a.cnt ? 3 : 2;
        if (a.attr == 10) return 3;
        return a.cnt ? 2 : 1;  // texture coordinates
    }
    bool read(const Attr& a, u32 idx, float* out) const {
        const int n = comps(a), sz = a.ctype == 4 ? 4 : a.ctype >= 2 ? 2 : 1;
        const u32 o = a.data + idx * a.stride;
        if (!f.ok(o, u32(n * sz))) return false;
        const float sc = a.ctype == 4 ? 1.f : 1.f / float(1u << (a.frac & 31));
        for (int k = 0; k < n; k++) {
            const u32 p = o + u32(k * sz);
            switch (a.ctype) {
            case 0: out[k] = float(f.u8at(p)) * sc; break;
            case 1: out[k] = float(s8(f.u8at(p))) * sc; break;
            case 2: out[k] = float(f.u16at(p)) * sc; break;
            case 3: out[k] = float(s16(f.u16at(p))) * sc; break;
            default: out[k] = f.f32(p); break;
            }
        }
        return true;
    }

    bool shapes(u32 po, int owner, Mesh& mesh) {
        const int rigid = slotOf({{{owner, 1.f}}, true});
        for (int guard = 0; po && f.ok(po, 0x18) && guard < 64; po = f.u32at(po + 4), guard++) {
            std::vector<Attr> attrs;
            for (u32 a = f.u32at(po + 8); f.ok(a, 0x18) && f.u32at(a) != 0xFF && attrs.size() < 16; a += 0x18)
                attrs.push_back({f.u32at(a), f.u32at(a + 4), f.u32at(a + 8), f.u32at(a + 0xC), f.u8at(a + 0x10), f.u16at(a + 0x12), f.u32at(a + 0x14)});
            // the skin: envelopes (lists of {joint, weight}) by the matrix index / 3, or the owning joint. One weight:
            // the vertices are in that joint's space; more: in the model's.
            std::vector<int> envs;
            if ((f.u16at(po + 0xC) & 0x3000) == 0x2000) {
                for (u32 l = f.u32at(po + 0x14); f.ok(l, 4) && f.u32at(l) && envs.size() < 256; l += 4) {
                    Slot s;
                    for (u32 e = f.u32at(l); f.ok(e, 8) && f.u32at(e) && s.ws.size() < 16; e += 8)
                        if (const int ji = jointOf(f.u32at(e)); ji >= 0) s.ws.push_back({ji, f.f32(e + 4)});
                    s.single = s.ws.size() == 1;
                    envs.push_back(s.ws.empty() ? rigid : slotOf(std::move(s)));
                }
            }
            const u32 dl = f.u32at(po + 0x10), dlEnd = dl + u32(f.u16at(po + 0xE)) * 0x20;
            if (!f.ok(dl, dlEnd - dl)) return false;
            for (u32 o = dl; o < dlEnd;) {
                const u8 op = f.u8at(o++);
                if (op == 0) continue;
                const u8 prim = op & 0xF8;
                const u32 cnt = f.u16at(o);
                o += 2;
                std::vector<Vtx> vs;
                vs.reserve(cnt);
                for (u32 i = 0; i < cnt; i++) {
                    Vtx v{};
                    v.n[2] = 1;
                    v.slot = rigid;
                    for (const Attr& a : attrs) {
                        u32 idx = 0;
                        if (a.type == 1) {
                            if (a.attr != 0) return false;  // (direct values other than the matrix index: not in these meshes)
                            const u32 k = f.u8at(o++) / 3;
                            if (k < envs.size()) v.slot = envs[k];
                            continue;
                        }
                        if (a.type == 2) idx = f.u8at(o++);
                        else if (a.type == 3) idx = f.u16at(o), o += 2;
                        else continue;
                        float val[3]{};
                        if (!read(a, idx, val)) return false;
                        if (a.attr == 9) std::memcpy(v.p, val, sizeof v.p);
                        else if (a.attr == 10) std::memcpy(v.n, val, sizeof v.n);
                        else if (a.attr == 13 || a.attr == 14) v.uv[a.attr - 13][0] = val[0], v.uv[a.attr - 13][1] = val[1];
                    }
                    vs.push_back(v);
                }
                auto tri = [&](u32 a, u32 b, u32 c) { mesh.tris.push_back(vs[a]), mesh.tris.push_back(vs[b]), mesh.tris.push_back(vs[c]); };
                switch (prim) {
                case 0x90: for (u32 i = 0; i + 2 < cnt; i += 3) tri(i, i + 1, i + 2); break;
                case 0x98: for (u32 i = 0; i + 2 < cnt; i++) i % 2 ? tri(i + 1, i, i + 2) : tri(i, i + 1, i + 2); break;
                case 0xA0: for (u32 i = 1; i + 1 < cnt; i++) tri(0, i, i + 1); break;
                case 0x80: for (u32 i = 0; i + 3 < cnt; i += 4) tri(i, i + 1, i + 2), tri(i, i + 2, i + 3); break;
                default: return false;  // (lines, points: not in these meshes)
                }
            }
        }
        return true;
    }

    bool mesh(size_t k) {
        const auto [dobj, owner] = dobjs[k];
        const u32 mobj = f.u32at(dobj + 8);
        if (!f.ok(mobj, 0x18)) return false;
        Mesh m;
        for (u32 t = f.u32at(mobj + 8), g = 0; t && f.ok(t, 0x5C) && g < 8; t = f.u32at(t + 4), g++) {
            Tex x;
            x.map = int(f.u32at(t + 8));
            const u32 src = f.u32at(t + 0xC);
            x.coord = src >= 4 && src <= 11 ? int(src - 4) : 0;
            x.colormap = int(f.u32at(t + 0x40) >> 16 & 15);
            x.wrapS = int(f.u32at(t + 0x34)), x.wrapT = int(f.u32at(t + 0x38));
            // HSD's texture matrix: scale repeat / scale, translate -translate (and a mirrored t one repeat on)
            const float sS = f.f32(t + 0x1C), sT = f.f32(t + 0x20);
            x.mulS = std::fabs(sS) < 1e-6f ? 0.f : float(std::max(1, int(f.u8at(t + 0x3C)))) / sS;
            x.mulT = std::fabs(sT) < 1e-6f ? 0.f : float(std::max(1, int(f.u8at(t + 0x3D)))) / sT;
            x.offS = f.f32(t + 0x28);
            x.offT = f.f32(t + 0x2C) + (x.wrapT == 2 && x.mulT != 0.f ? 1.f / x.mulT : 0.f);
            Image img;
            if (!decode(f, f.u32at(t + 0x4C), f.u32at(t + 0x50), img)) return false;
            x.frames.push_back(std::move(img));
            m.texs.push_back(std::move(x));
        }
        // the material animation's images: {next, texture map, AObj, images, palettes, u16 counts}; its frame n
        // shows image n
        if (k < matanims.size())
            for (u32 ta = f.u32at(matanims[k] + 8), g = 0; ta && f.ok(ta, 0x18) && g < 8; ta = f.u32at(ta), g++) {
                const u32 map = f.u32at(ta + 4), imgs = f.u32at(ta + 0xC), tluts = f.u32at(ta + 0x10);
                const u32 n = f.u16at(ta + 0x14), nt = f.u16at(ta + 0x16);
                for (Tex& x : m.texs) {
                    if (u32(x.map) != map) continue;
                    for (u32 i = 0; i < n && i < 32; i++) {
                        Image img;
                        if (!decode(f, f.u32at(imgs + 4 * i), nt ? f.u32at(tluts + 4 * std::min(i, nt - 1)) : 0, img)) break;
                        x.frames.push_back(std::move(img));
                    }
                    if (map == 0 && n >= ColourCount) x.colourFrames = ColourCount;
                    if (map == 1 && n == 5) m.eyes = true;  // (the eyes: open, half, shut, wide, cross; the mouth has 6)
                }
            }
        if (!shapes(f.u32at(dobj + 0xC), owner, m)) return false;
        model.meshes.push_back(std::move(m));
        return true;
    }
};

// The joints' world matrices at a frame of the motion (or the bind pose).
void pose(const Model& m, float frame, std::vector<Mtx>& W) {
    W.resize(m.joints.size());
    for (size_t i = 0; i < m.joints.size(); i++) {
        const Joint& J = m.joints[i];
        float r[3], s[3], t[3];
        std::memcpy(r, J.r, sizeof r), std::memcpy(s, J.s, sizeof s), std::memcpy(t, J.t, sizeof t);
        if (m.animated && i < m.motion.joints.size())
            for (const Track& tr : m.motion.joints[i]) {
                const float v = evalTrack(tr.keys, frame);
                if (tr.type >= 1 && tr.type <= 3) r[tr.type - 1] = v;
                else if (tr.type >= 5 && tr.type <= 7) t[tr.type - 5] = v;
                else if (tr.type >= 8 && tr.type <= 10) s[tr.type - 8] = v;
            }
        const Mtx local = srt(r, s, t);
        W[i] = J.parent >= 0 ? mul(W[size_t(J.parent)], local) : local;  // (parents come first)
    }
}
void slotMatrices(const Model& m, const std::vector<Mtx>& W, std::vector<Mtx>& out) {
    out.assign(m.slots.size(), Mtx{});
    for (size_t i = 0; i < m.slots.size(); i++) {
        const Slot& s = m.slots[i];
        if (s.single) {
            out[i] = W[size_t(s.ws[0].first)];
            continue;
        }
        for (auto [ji, w] : s.ws) {
            const Joint& J = m.joints[size_t(ji)];
            const Mtx x = J.hasInv ? mul(W[size_t(ji)], J.inv) : W[size_t(ji)];
            for (int k = 0; k < 12; k++) out[i][size_t(k)] += w * x[size_t(k)];
        }
    }
}

bool load(Model& model) {
    std::vector<u8> file;
    if (!dvdReadFile("RdKirby.dat", file) || file.size() < 0x20) return false;
    auto be = [&](size_t o) { return u32(file[o]) << 24 | u32(file[o + 1]) << 16 | u32(file[o + 2]) << 8 | file[o + 3]; };
    const u32 dataSize = be(4), nrel = be(8), nroot = be(12), nref = be(16);
    const size_t roots = 0x20 + size_t(dataSize) + size_t(nrel) * 4, names = roots + size_t(nroot + nref) * 8;
    if (names > file.size()) return false;
    Dat f{file.data() + 0x20, dataSize};
    u32 root = 0;
    for (u32 i = 0; i < nroot; i++) {
        const size_t s = names + be(roots + i * 8 + 4);
        if (s < file.size() && !std::strncmp(reinterpret_cast<const char*>(&file[s]), "rdDataKirby", std::min<size_t>(12, file.size() - s)))
            root = be(roots + i * 8);
    }
    const u32 desc = f.u32at(root + 4);
    if (!f.ok(desc, 0x1C)) return false;
    Loader L{f, model};
    L.walk(f.u32at(desc), -1, 0);
    L.walkAnim(f.u32at(desc + 4), 0);
    // the highest detail's parts 0 (body and arms) and 1 (feet), each its first option
    const u32 parts = f.u32at(desc + 0xC), lod = f.u32at(desc + 0x10);
    if (parts < 2 || !f.ok(lod, 16)) return false;
    for (u32 p = 0; p < 2; p++) {
        const u32 opts = f.u32at(lod + p * 8 + 4);
        const u32 n = f.u32at(opts), list = f.u32at(opts + 4);
        if (!f.u32at(lod + p * 8) || !f.ok(list, n)) return false;
        for (u32 i = 0; i < n; i++) {
            const u32 k = f.u8at(list + i);
            if (k >= L.dobjs.size() || !L.mesh(k)) return false;
        }
    }
    model.animated = loadMotion(model.motion, model.joints.size());
    // the framing: the bounds over the motion, so that Kirby stays put in the picture
    float lo[3]{1e9f, 1e9f, 1e9f}, hi[3]{-1e9f, -1e9f, -1e9f};
    std::vector<Mtx> W, S;
    const float frames = model.animated ? model.motion.frames : 1.f;
    for (float fr = 0; fr < frames; fr += 4) {
        pose(model, fr, W);
        slotMatrices(model, W, S);
        for (const Mesh& m : model.meshes)
            for (const Vtx& v : m.tris) {
                float p[3];
                xf(S[size_t(v.slot)], v.p, 1, p);
                for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
            }
    }
    for (int k = 0; k < 3; k++) model.centre[k] = (lo[k] + hi[k]) * 0.5f;
    model.ext = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}) * 0.5f;
    size_t tris = 0;
    for (const Mesh& m : model.meshes) tris += m.tris.size() / 3;
    log("[kirbymodel] RdKirby.dat: %zu meshes, %zu triangles, %zu joints; standing motion %s (%.0f frames, %zu eye looks)\n",
        model.meshes.size(), tris, model.joints.size(), model.animated ? "read" : "not read", double(model.motion.frames),
        model.motion.eyes.size());
    return tris > 0;
}

// ---- drawing ----
constexpr int kSize = 256, kSS = 2, kBig = kSize * kSS;

struct Raster {
    std::vector<float> depth;
    std::vector<u32> colour;  // RGBA; alpha 0: empty
    std::vector<Mtx> W, S;
};

float wrapCoord(float x, int n, int mode) {
    if (mode == 1) {  // repeat
        x = std::fmod(x, float(n));
        return x < 0 ? x + float(n) : x;
    }
    if (mode == 2) {  // mirror
        const float p = std::floor(x / float(n));
        x -= p * float(n);
        return int(p) % 2 ? float(n) - x : x;
    }
    return std::clamp(x, 0.f, float(n));
}
// Bilinear, as RGBA floats 0..1.
void sample(const Tex& t, const Image& im, float s, float u, float out[4]) {
    s = (s - t.offS) * t.mulS, u = (u - t.offT) * t.mulT;
    const float x = wrapCoord(s * float(im.w), im.w, t.wrapS) - 0.5f, y = wrapCoord(u * float(im.h), im.h, t.wrapT) - 0.5f;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = x - float(x0), fy = y - float(y0);
    auto px = [&](int xx, int yy) {
        xx = t.wrapS == 0 ? std::clamp(xx, 0, im.w - 1) : ((xx % im.w) + im.w) % im.w;
        yy = t.wrapT == 0 ? std::clamp(yy, 0, im.h - 1) : ((yy % im.h) + im.h) % im.h;
        return im.px[size_t(yy) * im.w + xx];
    };
    const u32 c[4] = {px(x0, y0), px(x0 + 1, y0), px(x0, y0 + 1), px(x0 + 1, y0 + 1)};
    const float w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    for (int ch = 0; ch < 4; ch++) {
        float v = 0;
        for (int k = 0; k < 4; k++) v += w[k] * float((c[k] >> (8 * ch)) & 255);
        out[ch] = v / 255.f;
    }
}

void render(const Model& m, float frame, int colour, int eyes, float yaw, Raster& r) {
    r.depth.assign(size_t(kBig) * kBig, 1e30f);
    r.colour.assign(size_t(kBig) * kBig, 0);
    pose(m, frame, r.W);
    slotMatrices(m, r.W, r.S);
    const float cx = m.centre[0], cy = m.centre[1], cz = m.centre[2], ext = m.ext;
    const float cyw = std::cos(yaw), syw = std::sin(yaw);
    const float pitch = 0.12f, cp = std::cos(pitch), sp = std::sin(pitch);  // (looked at from a little above)
    const float dist = ext * 7.f, focal = float(kBig) * 0.5f / (ext * 1.12f) * dist;  // a long lens: little distortion
    float L[3] = {-0.45f, 0.62f, 0.64f};  // the light: above, left, in front
    {
        const float l = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        for (float& c : L) c /= l;
    }
    struct P {
        float x, y, iz;  // screen, 1/depth
        float n[3], uv[2][2];
    };
    auto project = [&](const Vtx& v) {
        const Mtx& M = r.S[size_t(v.slot)];
        float wp[3], wn[3];
        xf(M, v.p, 1, wp);
        xf(M, v.n, 0, wn);
        const float nl = std::sqrt(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
        if (nl > 1e-6f)
            for (float& c : wn) c /= nl;
        const float x = wp[0] - cx, y = wp[1] - cy, z = wp[2] - cz;
        const float x1 = x * cyw + z * syw, z1 = -x * syw + z * cyw;
        const float y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;
        const float n0 = wn[0] * cyw + wn[2] * syw, n2 = -wn[0] * syw + wn[2] * cyw;
        P p;
        const float depth = dist - z2;
        p.iz = 1.f / depth;
        p.x = float(kBig) * 0.5f + x1 * focal * p.iz;
        p.y = float(kBig) * 0.5f - y2 * focal * p.iz;
        p.n[0] = n0, p.n[1] = wn[1] * cp - n2 * sp, p.n[2] = wn[1] * sp + n2 * cp;
        std::memcpy(p.uv, v.uv, sizeof p.uv);
        return p;
    };
    for (const Mesh& mesh : m.meshes) {
        // the images this mesh uses now: texture 0 in Kirby's colour, the eyes' look
        const Image* imgs[8]{};
        for (size_t t = 0; t < mesh.texs.size() && t < 8; t++) {
            const Tex& x = mesh.texs[t];
            int f = 0;
            if (x.colourFrames && colour >= 0 && colour < x.colourFrames) f = 1 + colour;
            if (mesh.eyes && x.map == 1 && eyes > 0 && 1 + eyes < int(x.frames.size())) f = 1 + eyes;
            imgs[t] = &x.frames[size_t(f)];
        }
        for (size_t i = 0; i + 2 < mesh.tris.size(); i += 3) {
            const P a = project(mesh.tris[i]), b = project(mesh.tris[i + 1]), c = project(mesh.tris[i + 2]);
            const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
            if (std::fabs(area) < 1e-6f) continue;
            const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x})))), x1 = std::min(kBig - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
            const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y})))), y1 = std::min(kBig - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
            const float inv = 1.f / area;
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    const float px = float(x) + 0.5f, py = float(y) + 0.5f;
                    const float w0 = ((b.x - px) * (c.y - py) - (c.x - px) * (b.y - py)) * inv;
                    const float w1 = ((c.x - px) * (a.y - py) - (a.x - px) * (c.y - py)) * inv;
                    const float w2 = 1.f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    const float iz = w0 * a.iz + w1 * b.iz + w2 * c.iz;
                    const float depth = 1.f / iz;
                    float& zb = r.depth[size_t(y) * kBig + x];
                    if (depth >= zb) continue;
                    // perspective-correct weights
                    const float q0 = w0 * a.iz * depth, q1 = w1 * b.iz * depth, q2 = w2 * c.iz * depth;
                    float col[4] = {1, 1, 1, 1};
                    for (size_t t = 0; t < mesh.texs.size() && t < 8; t++) {
                        const Tex& tx = mesh.texs[t];
                        const int k = tx.coord & 1;
                        float s[4];
                        sample(tx, *imgs[t], q0 * a.uv[k][0] + q1 * b.uv[k][0] + q2 * c.uv[k][0], q0 * a.uv[k][1] + q1 * b.uv[k][1] + q2 * c.uv[k][1], s);
                        switch (tx.colormap) {
                        case 5: for (int ch = 0; ch < 4; ch++) col[ch] = s[ch]; break;           // replace
                        case 4: for (int ch = 0; ch < 4; ch++) col[ch] *= s[ch]; break;          // modulate
                        default: for (int ch = 0; ch < 3; ch++) col[ch] += (s[ch] - col[ch]) * s[3]; break;  // alpha mask, blend
                        }
                    }
                    if (col[3] < 0.5f) continue;
                    float n[3];
                    for (int k = 0; k < 3; k++) n[k] = q0 * a.n[k] + q1 * b.n[k] + q2 * c.n[k];
                    const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-6f;
                    for (float& v : n) v /= nl;
                    const float diff = std::max(0.f, n[0] * L[0] + n[1] * L[1] + n[2] * L[2]);
                    const float rim = std::pow(std::max(0.f, 1.f - std::fabs(n[2])), 3.f) * 0.15f;  // (the edge, lit a little)
                    const float spec = std::pow(std::max(0.f, n[0] * 0.30f + n[1] * 0.42f + n[2] * 0.86f), 40.f) * 0.2f;
                    const float light = 0.6f + 0.48f * diff;
                    u32 o = 255u << 24;
                    for (int ch = 0; ch < 3; ch++) o |= u32(std::clamp((col[ch] * light + rim + spec) * 255.f, 0.f, 255.f)) << (8 * ch);
                    zb = depth;
                    r.colour[size_t(y) * kBig + x] = o;
                }
        }
    }
}

// 2x2 samples to a pixel: colour the covered samples' mean, alpha their share.
void resolve(const Raster& r, std::vector<u32>& out) {
    out.assign(size_t(kSize) * kSize, 0);
    for (int y = 0; y < kSize; y++)
        for (int x = 0; x < kSize; x++) {
            u32 sum[3]{}, n = 0;
            for (int sy = 0; sy < kSS; sy++)
                for (int sx = 0; sx < kSS; sx++) {
                    const u32 c = r.colour[size_t(y * kSS + sy) * kBig + x * kSS + sx];
                    if (!(c >> 24)) continue;
                    for (int ch = 0; ch < 3; ch++) sum[ch] += (c >> (8 * ch)) & 255;
                    n++;
                }
            if (n) out[size_t(y) * kSize + x] = rgba(sum[0] / n, sum[1] / n, sum[2] / n, n * 255 / (kSS * kSS));
        }
}

struct State {
    bool tried = false, ok = false;
    Model model;
    ImTextureID tex{};
    Raster raster;
    std::vector<u32> px;
    double drawnAt = -1;
    int drawnColour = -1;
};
State& st() {  // (never destroyed: the frontend may still draw while the program exits)
    static State* s = new State;
    return *s;
}

// The eyes' look at a frame of the motion, as its script sets them (without a motion: a blink every few seconds).
int eyesAt(const Model& m, float frame, float t) {
    if (!m.animated) {
        const float p = std::fmod(t, 3.6f);
        return p < 0.05f || (p >= 0.13f && p < 0.18f) ? 1 : p < 0.13f ? 2 : 0;
    }
    int look = 0;
    for (const auto& [at, l] : m.motion.eyes)
        if (frame >= at) look = l;
    return look;
}

}  // namespace

bool draw(ImDrawList* dl, ImVec2 c, float size, int colour, float t, float alpha) {
    State& s = st();
    if (!s.tried) {
        s.tried = true;
        s.ok = load(s.model);
        if (!s.ok) log("[kirbymodel] RdKirby.dat not read: the menus keep their own Kirby\n");
    }
    if (!s.ok) return false;
    if (!s.tex || t - s.drawnAt >= 1.0 / 60.0 || t < s.drawnAt || colour != s.drawnColour) {
        const Model& m = s.model;
        const float frame = m.animated ? std::fmod(t * 60.f, m.motion.frames) : 0.f;  // (the game's 60 a second)
        const float yaw = -0.28f + 0.22f * std::sin(t * 0.45f);
        render(m, frame, colour, eyesAt(m, frame, t), yaw, s.raster);
        resolve(s.raster, s.px);
        if (!s.tex) s.tex = aurora_imgui_add_texture(kSize, kSize, s.px.data());
        else aurora_imgui_update_texture(s.tex, kSize, kSize, s.px.data());
        menustyle::keepTexture(s.tex, kSize, kSize, s.px);  // (for the menus' test shots)
        s.drawnAt = t, s.drawnColour = colour;
    }
    const float h = size * 0.5f;
    dl->AddImage(s.tex, ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, int(alpha * 255)));
    return true;
}

}  // namespace rt::kirbymodel

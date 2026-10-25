// Kirby Air Ride's menu art for the port's menus, read at run time (src/runtime/gameart.h).
//
// MnAll.dat (the mode menus' archive) is a HAL archive: a header, the data, then a relocation table
// (the offsets in the data that hold pointers). Its pictures are image descriptors - {texels pointer,
// u16 width, u16 height, u32 GX format, ...} - found as relocated pointers followed by a plausible
// size and format; a colour-indexed picture's palette comes from the texture object that names it
// (image descriptor at +0x4C, palette descriptor at +0x50: {colours pointer, u32 format, u32 name,
// u16 count}). The pictures wanted are recognised by the FNV-1a hash of their texels.
//
// The font: the game letters its menu descriptions glyph by glyph, each a 32x32 I4 texture in main.dol
// (0x200 bytes apart: the digits, A-Z and a-z in a row, the punctuation in the Japanese font's order
// further on). Each glyph's width is its ink's.
#include "runtime/gameart.h"
#include "runtime/menustyle.h"
#include "runtime/runtime.h"

#include <aurora/imgui.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace rt {
bool dvdReadFile(const std::string& path, std::vector<u8>& out);  // src/runtime/hle_dvd.cpp

namespace {
#include "runtime/online_logo.inc"
}  // namespace
}  // namespace rt

namespace rt::gameart {

namespace {

u64 fnv(const u8* p, size_t n) {
    u64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// ---- GX textures to RGBA ----
int blockW(u32 fmt) { return fmt == 0 || fmt == 8 || fmt == 14 ? 8 : fmt == 1 || fmt == 2 || fmt == 9 ? 8 : 4; }
int blockH(u32 fmt) { return fmt == 0 || fmt == 8 || fmt == 14 ? 8 : 4; }
int bitsPP(u32 fmt) {
    switch (fmt) {
    case 0: case 8: case 14: return 4;
    case 1: case 2: case 9: return 8;
    case 6: return 32;
    default: return 16;
    }
}
size_t texBytes(int w, int h, u32 fmt) {
    const int bw = blockW(fmt), bh = blockH(fmt);
    return size_t((w + bw - 1) / bw) * size_t((h + bh - 1) / bh) * size_t(bw * bh * bitsPP(fmt) / 8);
}
u32 rgba(u32 r, u32 g, u32 b, u32 a) { return r | g << 8 | b << 16 | a << 24; }
u32 colour16(u16 v, u32 palFmt) {
    if (palFmt == 0) return rgba(v & 255, v & 255, v & 255, v >> 8);                                         // IA8
    if (palFmt == 1) return rgba((v >> 11) * 255 / 31, (v >> 5 & 63) * 255 / 63, (v & 31) * 255 / 31, 255);  // RGB565
    if (v & 0x8000) return rgba((v >> 10 & 31) * 255 / 31, (v >> 5 & 31) * 255 / 31, (v & 31) * 255 / 31, 255);
    return rgba((v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17, (v >> 12 & 7) * 255 / 7);           // RGB5A3
}

// I4 and I8 as white with the intensity for alpha (the menus' masks are tinted); IA4 / IA8 as they are;
// CI4 / CI8 through their palette.
bool decode(const u8* d, int w, int h, u32 fmt, const std::vector<u16>& pal, u32 palFmt, std::vector<u32>& out) {
    if (fmt != 0 && fmt != 1 && fmt != 2 && fmt != 3 && fmt != 8 && fmt != 9) return false;
    if ((fmt == 8 || fmt == 9) && pal.empty()) return false;
    out.assign(size_t(w) * h, 0);
    const int bw = blockW(fmt), bh = blockH(fmt), bpp = bitsPP(fmt);
    size_t pos = 0;
    for (int by = 0; by < h; by += bh)
        for (int bx = 0; bx < w; bx += bw)
            for (int y = by; y < by + bh; y++) {
                for (int x = bx; x < bx + bw; x++) {
                    const int i = x - bx;
                    u32 c = 0;
                    switch (fmt) {
                    case 0: {
                        const u8 b = d[pos + i / 2];
                        const u32 v = (i % 2 ? b & 15 : b >> 4) * 17;
                        c = rgba(255, 255, 255, v);
                        break;
                    }
                    case 1: c = rgba(255, 255, 255, d[pos + i]); break;
                    case 2: {
                        const u8 b = d[pos + i];
                        c = rgba((b & 15) * 17, (b & 15) * 17, (b & 15) * 17, (b >> 4) * 17);
                        break;
                    }
                    case 3: c = rgba(d[pos + 2 * i + 1], d[pos + 2 * i + 1], d[pos + 2 * i + 1], d[pos + 2 * i]); break;
                    case 8: {
                        const u8 b = d[pos + i / 2];
                        const u32 k = i % 2 ? b & 15 : b >> 4;
                        c = k < pal.size() ? colour16(pal[k], palFmt) : 0;
                        break;
                    }
                    case 9: c = d[pos + i] < pal.size() ? colour16(pal[d[pos + i]], palFmt) : 0; break;
                    }
                    if (x < w && y < h) out[size_t(y) * w + x] = c;
                }
                pos += size_t(bw * bpp / 8);
            }
    return true;
}

Texture g_art[size_t(Art::Count)];

void upload(Art a, int w, int h, const std::vector<u32>& px) {
    Texture& t = g_art[size_t(a)];
    t.id = aurora_imgui_add_texture(u32(w), u32(h), px.data());
    t.w = w, t.h = h;
    menustyle::keepTexture(t.id, w, h, px);
}

// The pictures wanted from MnAll.dat, by their texels' hash.
struct Wanted {
    Art art;
    u64 hash;
};
constexpr Wanted kWanted[] = {
    {Art::Button, 0x92826bd6ffee2261ull},
    {Art::Band, 0xfd2b38db7d39e705ull},
    {Art::TitleStep, 0x5b0a0b7cde03fc3bull},
    {Art::Gear, 0x4dab5b1dbb1aec73ull},
    {Art::Grooves, 0xed0ba7957bfb1bd0ull},
};

void loadArchive() {
    std::vector<u8> f;
    if (!dvdReadFile("MnAll.dat", f) || f.size() < 0x20) {
        log("[gameart] MnAll.dat not read: the menus keep their own drawing\n");
        return;
    }
    auto be32 = [&](size_t o) { return o + 4 <= f.size() ? u32(f[o]) << 24 | u32(f[o + 1]) << 16 | u32(f[o + 2]) << 8 | f[o + 3] : 0u; };
    auto be16 = [&](size_t o) { return o + 2 <= f.size() ? u16(f[o] << 8 | f[o + 1]) : u16(0); };
    const u32 dsize = be32(4), nrel = be32(8);
    if (0x20ull + dsize + 4ull * nrel > f.size()) return;
    const size_t D = 0x20;  // the data section
    std::vector<u32> rel(nrel);
    for (u32 i = 0; i < nrel; i++) rel[i] = be32(D + dsize + 4 * i);
    std::vector<bool> isRel(dsize / 4 + 1, false);
    for (u32 r : rel)
        if (r < dsize) isRel[r / 4] = true;
    int found = 0;
    for (u32 r : rel) {
        if (r + 12 > dsize) continue;
        const u32 p = be32(D + r), w = be16(D + r + 4), h = be16(D + r + 6), fmt = be32(D + r + 8);
        if (!(fmt <= 3 || fmt == 8 || fmt == 9) || w < 1 || w > 1024 || h < 1 || h > 1024 || p % 32) continue;
        const size_t n = texBytes(int(w), int(h), fmt);
        if (p + n > dsize) continue;
        const u64 hash = fnv(&f[D + p], n);
        for (const Wanted& wd : kWanted) {
            if (wd.hash != hash || g_art[size_t(wd.art)]) continue;
            std::vector<u16> pal;
            u32 palFmt = 2;
            if (fmt == 8 || fmt == 9)  // the texture object naming this image: its palette
                for (u32 t : rel)
                    if (t >= 0x4C && t + 4 < dsize && be32(D + t) == r && isRel[(t + 4) / 4]) {
                        const u32 tl = be32(D + t + 4);
                        const u32 lut = be32(D + tl), cnt = be16(D + tl + 12);
                        palFmt = be32(D + tl + 4);
                        if (cnt && cnt <= 256 && lut + 2 * cnt <= dsize)
                            for (u32 k = 0; k < cnt; k++) pal.push_back(be16(D + lut + 2 * k));
                        break;
                    }
            std::vector<u32> px;
            if (decode(&f[D + p], int(w), int(h), fmt, pal, palFmt, px)) upload(wd.art, int(w), int(h), px), found++;
        }
    }
    log("[gameart] MnAll.dat: %d of %d menu pictures\n", found, int(std::size(kWanted)));
}

// ---- the font ----
struct Glyph {
    float u0, v0, u1, v1;
    float left, width;  // the ink's, in the glyph's 32 units
};
constexpr u32 kDigits = 0x8050A040, kUpper = 0x8050B440, kLower = 0x8050E840;
constexpr u64 kGlyphAHash = 0xedc46ec02ae61802ull;  // 'A': this is the game's font
struct Punct {
    char c;
    u32 addr;
};
constexpr Punct kPunct[] = {
    {',', 0x80526C40}, {'.', 0x80526E40}, {':', 0x80527240}, {';', 0x80527440}, {'?', 0x80527640}, {'!', 0x80527840},
    {'^', 0x80527A40}, {'_', 0x80527C40}, {'/', 0x80528040}, {'~', 0x80528240}, {'|', 0x80528440}, {'\'', 0x80528640},
    {'"', 0x80528840}, {'(', 0x80528A40}, {')', 0x80528C40}, {'[', 0x80528E40}, {']', 0x80529040}, {'{', 0x80529240},
    {'}', 0x80529440}, {'+', 0x80529A40}, {'-', 0x80529C40}, {'=', 0x8052A040}, {'<', 0x8052A240}, {'>', 0x8052A440},
    {'$', 0x8052A840}, {'%', 0x8052AA40}, {'#', 0x8052AC40}, {'&', 0x8052AE40}, {'*', 0x8052B040}, {'@', 0x8052B240},
};
Glyph g_glyph[128];
bool g_has[128] = {};
bool g_font = false;
ImTextureID g_fontTex{};

void loadFont() {
    if (fnv(hostp(kUpper), 0x200) != kGlyphAHash) return;  // (not this game, or not loaded yet)
    struct Src {
        int c;
        u32 addr;
    };
    std::vector<Src> src;
    for (int i = 0; i < 10; i++) src.push_back({'0' + i, kDigits + u32(i) * 0x200});
    for (int i = 0; i < 26; i++) src.push_back({'A' + i, kUpper + u32(i) * 0x200}), src.push_back({'a' + i, kLower + u32(i) * 0x200});
    for (const Punct& p : kPunct) src.push_back({p.c, p.addr});
    constexpr int kCols = 16, kCell = 32, kPitch = 36;  // (cells 4 apart: no glyph bleeds into the next)
    const int rows = int(src.size() + kCols - 1) / kCols;
    const int W = kCols * kPitch, H = rows * kPitch;
    std::vector<u32> atlas(size_t(W) * H, 0);
    const std::vector<u16> none;
    for (size_t i = 0; i < src.size(); i++) {
        std::vector<u32> px;
        if (!decode(hostp(src[i].addr), kCell, kCell, 0, none, 0, px)) continue;
        const int ox = int(i % kCols) * kPitch + 2, oy = int(i / kCols) * kPitch + 2;
        int l = kCell, r = -1;
        for (int y = 0; y < kCell; y++)
            for (int x = 0; x < kCell; x++) {
                const u32 c = px[size_t(y) * kCell + x];
                atlas[size_t(oy + y) * W + ox + x] = c;
                if ((c >> 24) > 40) l = std::min(l, x), r = std::max(r, x);
            }
        Glyph& g = g_glyph[src[i].c];
        g.u0 = float(ox) / float(W), g.v0 = float(oy) / float(H), g.u1 = float(ox + kCell) / float(W), g.v1 = float(oy + kCell) / float(H);
        g.left = r >= l ? float(l) : 0.f;
        g.width = r >= l ? float(r - l + 1) : 8.f;
        g_has[src[i].c] = true;
    }
    g_fontTex = aurora_imgui_add_texture(u32(W), u32(H), atlas.data());
    menustyle::keepTexture(g_fontTex, W, H, atlas);
    g_font = true;
    log("[gameart] the menus' font: %zu glyphs\n", src.size());
}

constexpr float kSpacing = 2.5f, kSpace = 9.f;  // (in the glyph's 32 units)

// Each character of s..e: drawn (dl) or measured, in the game's font or the fallback.
float run(ImDrawList* dl, ImFont* fallback, float size, float x, float cy, ImU32 col, const char* s, const char* e) {
    if (!e) e = s + std::strlen(s);
    const float k = size / 32.f;
    float pen = x;
    ImFont* fb = fallback ? fallback : ImGui::GetFont();
    for (const char* p = s; p < e;) {
        const unsigned char c = u8(*p);
        if (c == ' ') {
            pen += kSpace * k;
            p++;
            continue;
        }
        if (g_font && c < 128 && g_has[c]) {
            const Glyph& g = g_glyph[c];
            if (dl) {
                const float gx = std::floor(pen - g.left * k);
                dl->AddImage(g_fontTex, ImVec2(gx, cy - size * 0.5f), ImVec2(gx + 32.f * k, cy + size * 0.5f), ImVec2(g.u0, g.v0),
                             ImVec2(g.u1, g.v1), col);
            }
            pen += (g.width + kSpacing) * k;
            p++;
            continue;
        }
        // (a character the font lacks, UTF-8: the fallback's, a little smaller to sit with the glyphs)
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        n = int(std::min<ptrdiff_t>(n, e - p));
        const float fs = size * 0.86f;
        const float w = fb->CalcTextSizeA(fs, FLT_MAX, 0.f, p, p + n).x;
        if (dl) dl->AddText(fb, fs, ImVec2(std::floor(pen), std::floor(cy - fs * 0.5f)), col, p, p + n);
        pen += w + kSpacing * k * 0.5f;
        p += n;
    }
    return pen - x;
}

}  // namespace

void load() {
    if (!ImGui::GetCurrentContext()) return;
    static bool s_archive = false;
    if (!s_archive) {
        s_archive = true;
        loadArchive();
        // the ONLINE emblem and lettering (online_logo.inc: I4 64x64, IA4 56x24)
        const std::vector<u16> none;
        std::vector<u32> px;
        if (decode(kOnlineEmblem, 64, 64, 0, none, 0, px)) upload(Art::Emblem, 64, 64, px);
        if (decode(kOnlineLettering, 56, 24, 2, none, 0, px)) upload(Art::Lettering, 56, 24, px);
    }
    static int s_fontTries = 0;
    if (!g_font && s_fontTries < 600) s_fontTries++, loadFont();
}

const Texture& get(Art a) { return g_art[size_t(a)]; }

bool fontReady() { return g_font; }

float text(ImDrawList* dl, ImFont* fallback, float size, float x, float cy, ImU32 col, const char* s, const char* e) {
    return run(dl, fallback, size, x, cy, col, s, e);
}

float measure(ImFont* fallback, float size, const char* s, const char* e) { return run(nullptr, fallback, size, 0, 0, 0, s, e); }

const char* wrap(ImFont* fallback, float size, const char* s, const char* e, float width) {
    if (!e) e = s + std::strlen(s);
    const char* lastBreak = nullptr;
    for (const char* p = s; p < e; p++) {
        if (*p == '\n') return p;
        if (*p == ' ') {
            if (measure(fallback, size, s, p) > width) return lastBreak ? lastBreak : p;
            lastBreak = p;
        }
    }
    if (measure(fallback, size, s, e) <= width) return e;
    if (lastBreak) return lastBreak;
    // one long word: as many characters as fit
    const char* p = s + 1;
    while (p < e && measure(fallback, size, s, p + 1) <= width) p++;
    return p;
}

}  // namespace rt::gameart

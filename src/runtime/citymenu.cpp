// City Trial's Game Settings screen gets a fifth row, "Stage": the city itself or a map mod played in its place
// (src/runtime/maps.cpp). Shown only when a map mod is there.
//
// The screen (the rules screens' code, MnSelruleAll.dat) is made for up to five rows - Air Ride's first page has five:
// its row arrays hold five, its layout model (Fpos) has a five-row arrangement - and City Trial keeps to four by its
// tables (r13 small data): rows 805D5188 (4), each row's kind 805D518C (0B 05 0C 0D, a fifth slot free), each row's
// option count 805D5194. A row's label is the Frame model's texture animation at the frame of its kind (15 pictures,
// 128x24 I4: kind 14 a second "Items" no screen of City Trial's shows - ours, its picture replaced); its value one of the
// Contents model's (20, 96x24 I4: 19 "None", ours). Kind 14 is no kind the screen's code knows: each of its switches
// (focus, unfocus, left/right, arrows) passes the row by, and this file does what they do for it:
//   - the screen's load (8001FBD8): the tables set for five rows; after the models are made (80132E8C, before the rows
//     take their pictures) the label's and value's pictures replaced; after it, the value made (801330E4) and the row
//     focused if the cursor starts there;
//   - each frame (8001EE60): onto the row, off it (the frame's highlight, the cursor, its arrows, the help line), left
//     and right on it (the value changed, the cursor's sound);
//   - leaving (800200AC): the map chosen is the one served from then on.
// The help line is the screen's text 1 (SisSelrule.dat's table, 8059A85C: an entry the screen never shows) pointed at
// ours, and the help table's entry for page 0, row 4 (804AA378) set to it.
#include "runtime/interp.h"
#include "runtime/maps.h"
#include "runtime/runtime.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imstb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace rt {
namespace {

constexpr u32 kRows = 0x805D5188, kKinds = 0x805D518C, kOptions = 0x805D5194;
constexpr u32 kHelpIds = 0x804AA338;      // s32 [page][row 0..4][value 0..3]
constexpr u32 kSisTable = 0x8059A85C;     // -> the screen's text table (SisSelrule.dat's)
constexpr u32 kState = 0x80535A94;        // the screen: +0 row, +1 page, +2 s16 hold timer
constexpr u32 kPadRepeat = 0x80537000 + 0x14 * 0x30 + 0x14;  // (8000ED10(0x14)'s low word: presses, with repeats)
constexpr u32 kWork = 0x80558788;         // the menu's work block: +D60 the Frame model, +DC4 the Contents model
constexpr u32 kLoad = 0x8001FBD8, kModels = 0x80132E8C, kThink = 0x8001EE60, kExit = 0x800200AC;
constexpr int kRow = 4, kKind = 14, kValueImage = 19, kHelpText = 1;

int s_choice = 0;      // the map shown (maps::count() of them)
bool s_active = false; // the row is on this screen
std::vector<u32> s_valueDesc;  // per map: its value picture's image descriptor (guest)
u32 s_valueTable = 0;  // the Contents texture animation's picture table (guest), its entry 19 ours

void gcall(Ctx& c, u32 fn, std::initializer_list<u32> args) {
    const u32 lr = c.lr;
    int i = 3;
    for (u32 a : args) c.r[i++] = a;
    c.call(fn);
    c.lr = lr;
}

// ---- pictures (I4: 8x8 tiles, two pixels a byte, the high nibble first) ----
u8 getI4(const u8* img, int w, int x, int y) {
    const int tile = (y / 8) * (w / 8) + x / 8, o = tile * 32 + (y % 8) * 4 + (x % 8) / 2;
    return (x & 1) ? img[o] & 15 : img[o] >> 4;
}
std::vector<u8> toI4(const std::vector<u8>& grey, int w, int h) {  // (grey: 0..255, w x h)
    std::vector<u8> out(size_t(w * h / 2));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const int tile = (y / 8) * (w / 8) + x / 8, o = tile * 32 + (y % 8) * 4 + (x % 8) / 2;
            const u8 v = u8(std::min(15, (grey[size_t(y * w + x)] + 8) / 17));
            out[size_t(o)] |= (x & 1) ? v : u8(v << 4);
        }
    return out;
}

// A picture in guest memory with its image descriptor (a copy of `like`'s, its size and data replaced).
u32 makeDesc(u32 like, const std::vector<u8>& i4, int w, int h) {
    const u32 data = hleAlloc(u32(i4.size()), 32);
    std::memcpy(hostp(data), i4.data(), i4.size());
    const u32 desc = hleAlloc(0x18, 32);
    std::memcpy(hostp(desc), hostp(like), 0x18);
    wr32(desc, data);
    wr16(desc + 4, u16(w)), wr16(desc + 6, u16(h));
    wr32(desc + 8, 0);  // (GX_TF_I4)
    return desc;
}

// Text in a system font (the player's own: nothing is shipped), white on black, fitted to w x h.
std::vector<u8> renderText(const std::string& text, int w, int h, std::initializer_list<const char*> fonts) {
    std::vector<u8> grey(size_t(w * h), 0);
    static std::vector<u8> s_ttf[4];
    static std::string s_ttfName[4];
    std::string dir;
    if (const char* windir = std::getenv("WINDIR")) dir = std::string(windir) + "/Fonts/";
    const std::vector<u8>* ttf = nullptr;
    for (const char* name : fonts) {
        for (int k = 0; k < 4 && !ttf; k++)
            if (s_ttfName[k] == name) ttf = &s_ttf[k];
        if (ttf) break;
        if (std::FILE* f = std::fopen((dir + name).c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            for (int k = 0; k < 4; k++)
                if (s_ttfName[k].empty()) {
                    s_ttf[k].resize(size_t(n));
                    if (std::fread(s_ttf[k].data(), 1, size_t(n), f) == size_t(n)) s_ttfName[k] = name, ttf = &s_ttf[k];
                    break;
                }
            std::fclose(f);
            if (ttf) break;
        }
    }
    if (!ttf) return grey;
    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, ttf->data(), stbtt_GetFontOffsetForIndex(ttf->data(), 0))) return grey;
    // (sized by its capitals: they fill most of the height, as the game's own values do)
    int x0, y0, x1, y1;
    stbtt_GetCodepointBox(&font, 'H', &x0, &y0, &x1, &y1);
    const float scale = float(h) * 0.68f / float(std::max(1, y1));
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
    // laid out on a wide strip, then squeezed across if it is wider than the picture
    const int baseline = int(std::lround(h * 0.5f + y1 * scale * 0.5f));
    float pen = 0;
    std::vector<float> strip;
    int stripW = 0;
    struct Glyph {
        int x, y, gw, gh;
        std::vector<u8> px;
    };
    std::vector<Glyph> glyphs;
    for (size_t i = 0; i < text.size(); i++) {
        const int cp = u8(text[i]);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font, cp, &adv, &lsb);
        int gx0, gy0, gx1, gy1;
        stbtt_GetCodepointBitmapBox(&font, cp, scale, scale, &gx0, &gy0, &gx1, &gy1);
        Glyph g{int(std::lround(pen)) + gx0, baseline + gy0, gx1 - gx0, gy1 - gy0, {}};
        if (g.gw > 0 && g.gh > 0) {
            g.px.resize(size_t(g.gw * g.gh));
            stbtt_MakeCodepointBitmap(&font, g.px.data(), g.gw, g.gh, g.gw, scale, scale, cp);
        }
        glyphs.push_back(std::move(g));
        pen += float(adv) * scale;
        if (i + 1 < text.size()) pen += float(stbtt_GetCodepointKernAdvance(&font, cp, u8(text[i + 1]))) * scale;
    }
    stripW = std::max(1, int(std::ceil(pen)) + 2);
    strip.assign(size_t(stripW * h), 0.f);
    for (const Glyph& g : glyphs)
        for (int y = 0; y < g.gh; y++)
            for (int x = 0; x < g.gw; x++) {
                const int sx = g.x + x, sy = g.y + y;
                if (sx >= 0 && sx < stripW && sy >= 0 && sy < h) strip[size_t(sy * stripW + sx)] = std::max(strip[size_t(sy * stripW + sx)], g.px[size_t(y * g.gw + x)] / 255.f);
            }
    const int margin = std::max(2, w / 32), room = w - 2 * margin;
    const float squeeze = stripW > room ? float(stripW) / float(room) : 1.f;
    const int outW = int(float(stripW) / squeeze), left = (w - outW) / 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < outW; x++) {
            // (a box filter over the strip's columns this pixel covers)
            const float a = float(x) * squeeze, b = float(x + 1) * squeeze;
            float sum = 0, n = 0;
            for (int sx = int(a); sx < int(std::ceil(b)) && sx < stripW; sx++) {
                const float cover = std::min(b, float(sx + 1)) - std::max(a, float(sx));
                sum += strip[size_t(y * stripW + sx)] * cover, n += cover;
            }
            grey[size_t(y * w + left + x)] = u8(std::lround(std::min(1.f, n > 0 ? sum / n : 0.f) * 255.f));
        }
    return grey;
}

// "Stage" from the game's own label pictures: "Sta" of "Stadium" (12) and "ge" of "Damage" (2), glyphs found by the
// empty columns between them. Empty if they can't be told apart (the caller then writes it in a system font).
std::vector<u8> composeStage(u32 tableAddr) {
    constexpr int W = 128, H = 24;
    auto img = [&](int k) { return hostp(rd32(rd32(tableAddr + 4 * u32(k)))); };
    auto glyphs = [&](const u8* p) {
        std::vector<std::pair<int, int>> spans;
        int start = -1;
        for (int x = 0; x <= W; x++) {
            bool ink = false;
            for (int y = 0; x < W && y < H && !ink; y++) ink = getI4(p, W, x, y) > 3;
            if (ink && start < 0) start = x;
            if (!ink && start >= 0) spans.push_back({start, x}), start = -1;
        }
        return spans;
    };
    const u8* stadium = img(12);
    const u8* damage = img(2);
    const auto s = glyphs(stadium), d = glyphs(damage);
    if (s.size() != 7 || d.size() != 6) {
        log("[citymenu] the label's glyphs: %zu in Stadium, %zu in Damage (7, 6 looked for): written in a font instead\n", s.size(), d.size());
        return {};
    }
    const int gap = s[1].first - s[0].second;
    const int aW = s[2].second - s[0].first, bW = d[5].second - d[4].first;
    const int total = aW + gap + bW, left = (W - total) / 2;
    std::vector<u8> grey(size_t(W * H), 0);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < aW; x++) grey[size_t(y * W + left + x)] = u8(getI4(stadium, W, s[0].first + x, y) * 17);
        for (int x = 0; x < bW; x++) grey[size_t(y * W + left + aW + gap + x)] = u8(getI4(damage, W, d[4].first + x, y) * 17);
    }
    return grey;
}

// The texture animation with `n` pictures in a model's material animations (its picture table), or 0.
u32 findTexAnim(u32 model, u32 n) {
    // (scene models: a null-ended list of groups - +0 the joints, +4 their animations, +8 the material animations)
    if ((model & 0xFE000000u) != 0x80000000u) return 0;
    const u32 group = rd32(model);
    if ((group & 0xFE000000u) != 0x80000000u) return 0;
    const u32 matAnims = rd32(group + 8);
    if ((matAnims & 0xFE000000u) != 0x80000000u) return 0;
    std::vector<u32> todo;
    for (u32 i = 0; i < 4; i++) {
        const u32 root = rd32(matAnims + 4 * i);
        if ((root & 0xFE000000u) != 0x80000000u) break;
        todo.push_back(root);
    }
    int guard = 0;
    while (!todo.empty() && guard++ < 4096) {
        const u32 j = todo.back();  // (MatAnimJoint: +0 child, +4 next, +8 its MatAnims)
        todo.pop_back();
        for (u32 m = rd32(j + 8); (m & 0xFE000000u) == 0x80000000u; m = rd32(m))  // (MatAnim: +0 next, +8 TexAnims)
            for (u32 t = rd32(m + 8); (t & 0xFE000000u) == 0x80000000u; t = rd32(t))  // (TexAnim: +C table, +14 count)
                if (rd16(t + 0x14) == n && (rd32(t + 0xC) & 0xFE000000u) == 0x80000000u) return rd32(t + 0xC);
        if ((rd32(j) & 0xFE000000u) == 0x80000000u) todo.push_back(rd32(j));
        if ((rd32(j + 4) & 0xFE000000u) == 0x80000000u) todo.push_back(rd32(j + 4));
    }
    return 0;
}

// The pictures, once the screen's models are made (before its rows take theirs).
void pictures() {
    s_valueDesc.clear();
    s_valueTable = 0;
    const u32 labels = findTexAnim(rd32(kWork + 0xD60), 15), values = findTexAnim(rd32(kWork + 0xDC4), 20);
    if (!labels || !values) {
        for (u32 off : {0xD44u, 0xD4Cu, 0xD54u, 0xD60u, 0xD98u, 0xDC4u}) {
            const u32 a = rd32(kWork + off), b = (a & 0xFE000000u) == 0x80000000u ? rd32(a) : 0;
            const u32 g = (b & 0xFE000000u) == 0x80000000u ? b : 0;
            log("[citymenu]   work+%X: %08X -> %08X -> %08X %08X %08X %08X\n", off, a, b, g ? rd32(g) : 0, g ? rd32(g + 4) : 0, g ? rd32(g + 8) : 0,
                g ? rd32(g + 12) : 0);
        }
        log("[citymenu] the screen's pictures not found (labels %08X, values %08X): no Stage row\n", labels, values);
        s_active = false;
        return;
    }
    std::vector<u8> label = composeStage(labels);
    if (label.empty()) label = renderText("Stage", 128, 24, {"georgiab.ttf", "timesbd.ttf"});
    wr32(labels + 4 * kKind, makeDesc(rd32(labels + 4 * kKind), toI4(label, 128, 24), 128, 24));
    // the values at twice the game's size (sharper: the picture is drawn at the same place, whatever its size)
    for (int i = 0; i < maps::count(); i++)
        s_valueDesc.push_back(makeDesc(rd32(values + 4 * kValueImage), toI4(renderText(maps::name(i), 192, 48, {"ARLRDBD.TTF", "segoeuib.ttf", "arialbd.ttf"}), 192, 48), 192, 48));
    s_valueTable = values;
    wr32(values + 4 * kValueImage, s_valueDesc[size_t(s_choice)]);
}

// The help line: SIS text (a header, each character 20 xx - a space a bare 1A - and an ending, as the game's own).
u32 sisText(const char* s) {
    static const u8 kHead[] = {0x0A, 0x00, 0x00, 0x06, 0x00, 0x12, 0x18, 0x16, 0x0C, 0xCC, 0xCC, 0xCC, 0x0E, 0x00, 0x99, 0x00, 0x99};
    static const u8 kTail[] = {0x03, 0x0D, 0x0F, 0x17, 0x19, 0x13, 0x00};
    std::vector<u8> b(kHead, kHead + sizeof kHead);
    for (const char* p = s; *p; p++) {
        const char ch = *p;
        if (ch == ' ') { b.push_back(0x1A); continue; }
        int code = -1;
        if (ch >= '0' && ch <= '9') code = ch - '0';
        else if (ch >= 'A' && ch <= 'Z') code = 0x0A + (ch - 'A');
        else if (ch >= 'a' && ch <= 'z') code = 0x24 + (ch - 'a');
        else if (ch == ',') code = 0xE6;
        else if (ch == '.') code = 0xE7;
        else if (ch == '!') code = 0xEC;
        else if (ch == '-') code = 0xFE;
        if (code >= 0) b.push_back(0x20), b.push_back(u8(code));
    }
    b.insert(b.end(), kTail, kTail + sizeof kTail);
    const u32 at = hleAlloc(u32(b.size()), 4);
    std::memcpy(hostp(at), b.data(), b.size());
    return at;
}

void showValue(Ctx& c) {
    if (!s_valueTable || s_valueDesc.empty()) return;
    wr32(s_valueTable + 4 * kValueImage, s_valueDesc[size_t(s_choice)]);
    gcall(c, 0x80133104, {0, u32(kRow), 0, u32(kValueImage)});  // (the value's picture set again)
}

void focus(Ctx& c) {
    gcall(c, 0x80133024, {0, u32(kRow)});  // the row's frame lit
    gcall(c, 0x80133268, {});
    gcall(c, 0x801332C8, {0, u32(kRow), 0});  // the cursor onto its value
    gcall(c, 0x80133308, {});
    gcall(c, 0x801332E8, {1});
    gcall(c, 0x80133450, {0, u32(kRow), 0});  // the help line
    gcall(c, 0x80133348, {}), gcall(c, 0x80133388, {});  // both arrows (the choice goes round)
}

}  // namespace

void cityMenuInit() {
    if (rd8(kRows) != 4 || rd8(kKinds) != 0x0B || rd8(kKinds + 4) != 0xFF || (rd32(kLoad) >> 16) != 0x9421 || (rd32(kThink) >> 16) != 0x9421 ||
        (rd32(kModels) >> 16) != 0x9421 || (rd32(kExit) >> 16) != 0x9421) {
        log("[citymenu] City Trial's settings screen is not as expected: no Stage row\n");
        return;
    }
    interpReplace(0, kLoad, [](Ctx& c, u32 entry) {
        maps::rescan();
        s_active = maps::count() > 1;
        s_choice = maps::selected();
        wr8(kRows, s_active ? 5 : 4);
        wr8(kKinds + 4, s_active ? kKind : 0xFF);
        wr8(kOptions + 4, s_active ? 1 : 0xFF);
        if (s_active) wr32(kHelpIds + 4 * 0x10, kHelpText);
        if (s8(rd8(kState)) >= rd8(kRows)) wr8(kState, u8(rd8(kRows) - 1));  // (the cursor left on a row gone since)
        interpOriginal(c, entry);
        if (!s_active) return;
        const u32 table = rd32(kSisTable);
        static u32 s_help = 0;
        if (!s_help) s_help = sisText("Choose where to play City Trial.");
        if ((table & 0xFE000000u) == 0x80000000u) wr32(table + 4 * kHelpText, s_help);
        if (!s_active) return;  // (the pictures weren't found)
        gcall(c, 0x801330E4, {0, u32(kRow), 0, u32(kValueImage)});  // the value
        showValue(c);
        if (s8(rd8(kState)) == kRow) focus(c);
        log("[citymenu] Stage row: %s (%d maps)\n", maps::name(s_choice).c_str(), maps::count());
    });
    interpReplace(0, kModels, [](Ctx& c, u32 entry) {
        interpOriginal(c, entry);
        if (s_active) pictures();
    });
    interpReplace(0, kThink, [](Ctx& c, u32 entry) {
        const int before = s8(rd8(kState));
        interpOriginal(c, entry);
        if (!s_active) return;
        const int after = s8(rd8(kState));
        if (before != kRow && after == kRow) focus(c);
        else if (before == kRow && after != kRow) gcall(c, 0x80133044, {0, u32(kRow)});
        else if (before == kRow && after == kRow && s16(rd16(kState + 2)) <= 0) {
            const u32 pad = rd32(kPadRepeat);
            const int step = (pad & 0x80002u) ? 1 : (pad & 0x40001u) ? -1 : 0;
            if (step && maps::count() > 1) {
                s_choice = (s_choice + step + maps::count()) % maps::count();
                gcall(c, 0x80061620, {});  // (the cursor's sound)
                showValue(c);
            }
        }
    });
    interpReplace(0, kExit, [](Ctx& c, u32 entry) {
        if (s_active && s_choice != maps::selected()) maps::select(s_choice);
        interpOriginal(c, entry);
    });
    log("[citymenu] City Trial's settings: a Stage row when map mods are there\n");
}

}  // namespace rt

#pragma once
// The look the port's own menus share (the F1 menu, src/runtime/menu.cpp; the Online menu,
// src/runtime/online_menu.cpp): Kirby Air Ride's menu colours, its leaning capsules, rings and stars,
// drawn with Dear ImGui's draw lists in design units (a 1280x720 window: 1 unit a pixel), and the
// test runs' picture of what was drawn (writeShot). Everything here runs on the frontend's thread.
#include "runtime/gameart.h"
#include "runtime/runtime.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace rt::menustyle {

// ---------------------------------------------------------------------------------------------------
// Colours (0xRRGGBB), taken from the game's menus
inline constexpr u32 kPlum = 0x2D2737;       // the frame around every menu screen
inline constexpr u32 kPlumLight = 0x463E55;  // its rim
inline constexpr u32 kInk = 0x141414;        // the buttons' outlines and lettering
inline constexpr u32 kTitleInk = 0x2E2E2E;   // a screen's title and the line under it
inline constexpr u32 kButton = 0xBFBFBF;     // a button not in focus
inline constexpr u32 kButtonLit = 0xDCDCDC;  // its sheen
inline constexpr u32 kBall = 0x333333;       // the ball at a button's left
inline constexpr u32 kBallLit = 0xEDE7DF;    // ... in focus
inline constexpr u32 kOrange = 0xFB9719;     // "Start Game"
inline constexpr u32 kOrangeLit = 0xFFC468;
inline constexpr u32 kMaroon = 0x5A0000;     // the band behind it
inline constexpr u32 kMaroonRim = 0x8E2A1E;
inline constexpr u32 kBand = 0x585046;       // the description band
inline constexpr u32 kBandText = 0xE6E0D2;
inline constexpr u32 kCream = 0xF2ECDF;
inline constexpr u32 kSlate = 0x4B5563;      // the "Options" capsule
inline constexpr u32 kSlateTail = 0x3A4456;
inline constexpr u32 kSlateText = 0xC9CED6;
inline constexpr u32 kWhite = 0xFFFFFF;

// The modes' colours: a tab's field and its deeper shade.
struct ModeColour {
    u32 field, deep;
};
inline constexpr ModeColour kAirRide{0xD07A74, 0xA4504B};
inline constexpr ModeColour kTopRide{0xD6B25E, 0xA9853A};
inline constexpr ModeColour kCityTrial{0x9DB283, 0x718A58};
inline constexpr ModeColour kSelectMode{0x718EBC, 0x4F6C9C};

// ---------------------------------------------------------------------------------------------------
// Fonts, scale, drawing helpers

struct Fonts {
    ImFont* label = nullptr;  // the buttons' lettering: a heavy serif, as the game's
    ImFont* bold = nullptr;   // titles, values, keycaps
    ImFont* text = nullptr;   // the description band: a narrow sans
};
inline Fonts g_font;  // (loaded by menu::init)

inline float g_s = 1.f;      // pixels per design unit (a 1280x720 window: 1)
inline float g_alpha = 1.f;  // what is being drawn fades in (a new page)

inline float U(float v) { return v * g_s; }
inline ImU32 col(u32 rgb, float a = 1.f) {
    return IM_COL32(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255, int(std::clamp(a * g_alpha, 0.f, 1.f) * 255.f + 0.5f));
}
inline u32 mixRgb(u32 a, u32 b, float t) {
    auto ch = [&](int sh) {
        const float x = float(a >> sh & 255), y = float(b >> sh & 255);
        return u32(std::clamp(x + (y - x) * t, 0.f, 255.f) + 0.5f) << sh;
    };
    return ch(16) | ch(8) | ch(0);
}
inline float clamp01(float t) { return std::clamp(t, 0.f, 1.f); }
inline float easeOut(float t) {
    t = clamp01(t);
    return 1.f - (1.f - t) * (1.f - t) * (1.f - t);
}
// Out past the end and back (the sheet settling as it comes down).
inline float easeOutBack(float t) {
    t = clamp01(t);
    constexpr float k = 1.3f;  // (about 5% past)
    const float u = t - 1.f;
    return 1.f + (k + 1.f) * u * u * u + k * u * u;
}
inline ImFont* F(ImFont* f) { return f ? f : ImGui::GetFont(); }
inline ImVec2 measure(ImFont* f, float size, const char* s, const char* e = nullptr) {
    return F(f)->CalcTextSizeA(size, FLT_MAX, 0.f, s, e);
}
// Lettering centred on y (cy); returns its width.
inline float text(ImDrawList* dl, ImFont* f, float size, float x, float cy, ImU32 c, const char* s, const char* e = nullptr) {
    dl->AddText(F(f), size, ImVec2(std::floor(x), std::floor(cy - size * 0.5f)), c, s, e);
    return measure(f, size, s, e).x;
}

// ---------------------------------------------------------------------------------------------------
// Shapes. Filled outlines go clockwise on the screen (ImGui's anti-aliased edge expects it).

inline void arc(std::vector<ImVec2>& pts, ImVec2 c, float r, float a0, float a1, int steps) {
    for (int i = 0; i <= steps; i++) {
        const float a = a0 + (a1 - a0) * float(i) / float(steps);
        pts.push_back(ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r));
    }
}
inline constexpr float kPi = 3.14159265f;

// A capsule leaning forward like the game's menu buttons: the top `lean` x height further right than the
// bottom. Its left end is round or cut straight.
inline void capsule(ImDrawList* dl, float x0, float y0, float x1, float y1, float lean, bool roundLeft, ImU32 c) {
    static std::vector<ImVec2> pts;
    pts.clear();
    const float r = (y1 - y0) * 0.5f, ym = (y0 + y1) * 0.5f;
    if (roundLeft) {
        arc(pts, ImVec2(x0 + r, ym), r, kPi * 0.5f, kPi * 1.5f, 10);
    } else {
        pts.push_back(ImVec2(x0, y1));
        pts.push_back(ImVec2(x0, y0));
    }
    arc(pts, ImVec2(x1 - r, ym), r, kPi * 1.5f, kPi * 2.5f, 12);
    for (ImVec2& p : pts) p.x += (ym - p.y) * lean;
    dl->AddConvexPolyFilled(pts.data(), int(pts.size()), c);
}

// A rectangle with its own radius at each corner (the field's big lower right curve).
inline void roundedRect(std::vector<ImVec2>& pts, ImVec2 a, ImVec2 b, float tl, float tr, float br, float bl) {
    pts.clear();
    arc(pts, ImVec2(a.x + tl, a.y + tl), tl, kPi, kPi * 1.5f, 8);
    arc(pts, ImVec2(b.x - tr, a.y + tr), tr, kPi * 1.5f, kPi * 2.f, 8);
    arc(pts, ImVec2(b.x - br, b.y - br), br, 0.f, kPi * 0.5f, 16);
    arc(pts, ImVec2(a.x + bl, b.y - bl), bl, kPi * 0.5f, kPi, 8);
}

// Triangles from a centre to each pair of points along an outline (no edge smoothing: the shapes it
// fills are outlined after).
inline void fan(ImDrawList* dl, ImVec2 centre, const std::vector<ImVec2>& pts, bool closed, ImU32 c) {
    const int n = int(pts.size()), tris = closed ? n : n - 1;
    if (tris <= 0) return;
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(tris * 3, n + 1);
    const ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
    dl->PrimWriteVtx(centre, uv, c);
    for (const ImVec2& p : pts) dl->PrimWriteVtx(p, uv, c);
    for (int i = 0; i < tris; i++) {
        dl->PrimWriteIdx(base);
        dl->PrimWriteIdx(ImDrawIdx(base + 1 + i));
        dl->PrimWriteIdx(ImDrawIdx(base + 1 + (i + 1) % n));
    }
}

// The frame's colour over the field's rounded corners (what was drawn on the field spills past them).
inline void maskCorners(ImDrawList* dl, ImVec2 a, ImVec2 b, float tl, float tr, float br, float bl, ImU32 c) {
    std::vector<ImVec2> pts;
    auto corner = [&](ImVec2 at, ImVec2 centre, float r, float a0) {
        if (r <= 0) return;
        pts.clear();
        arc(pts, centre, r, a0, a0 + kPi * 0.5f, 10);
        fan(dl, at, pts, false, c);
    };
    corner(a, ImVec2(a.x + tl, a.y + tl), tl, kPi);
    corner(ImVec2(b.x, a.y), ImVec2(b.x - tr, a.y + tr), tr, kPi * 1.5f);
    corner(b, ImVec2(b.x - br, b.y - br), br, 0.f);
    corner(ImVec2(a.x, b.y), ImVec2(a.x + bl, b.y - bl), bl, kPi * 0.5f);
}

inline void star(ImDrawList* dl, ImVec2 c, float outer, float inner, float turn, ImU32 fill, ImU32 edge, float edgeW) {
    std::vector<ImVec2> pts;
    for (int i = 0; i < 10; i++) {
        const float a = turn - kPi * 0.5f + float(i) * kPi / 5.f;
        const float r = i % 2 ? inner : outer;
        pts.push_back(ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r));
    }
    fan(dl, c, pts, true, fill);
    dl->AddPolyline(pts.data(), int(pts.size()), edge, ImDrawFlags_Closed, edgeW);
}

// Corner marks around what is in focus, breathing in and out a little.
inline void cornerMarks(ImDrawList* dl, ImVec2 a, ImVec2 b, float t) {
    const float grow = U(4.f) + std::sin(t * 5.5f) * U(2.f);
    a = ImVec2(a.x - grow, a.y - grow), b = ImVec2(b.x + grow, b.y + grow);
    const float arm = U(13.f), w = U(3.5f);
    auto mark = [&](ImVec2 p, float dx, float dy, ImU32 c, float off) {
        const ImVec2 pts[3] = {ImVec2(p.x + off, p.y + dy * arm + off), ImVec2(p.x + off, p.y + off),
                               ImVec2(p.x + dx * arm + off, p.y + off)};
        dl->AddPolyline(pts, 3, c, ImDrawFlags_None, w);
    };
    for (int pass = 0; pass < 2; pass++) {
        const ImU32 c = pass ? col(kWhite) : col(kInk, 0.6f);
        const float off = pass ? 0.f : U(1.5f);
        mark(a, 1, 1, c, off);
        mark(ImVec2(b.x, a.y), -1, 1, c, off);
        mark(b, -1, -1, c, off);
        mark(ImVec2(a.x, b.y), 1, -1, c, off);
    }
}

// A key of a keyboard: white, outlined, with a shaded lower lip. Returns its width.
inline float keycap(ImDrawList* dl, float right, float cy, const char* label, bool draw) {
    const float size = U(16.f), h = U(28.f);
    const float w = std::max(h, measure(g_font.bold, size, label).x + U(18.f));
    if (!draw) return w;
    const ImVec2 a(right - w, cy - h * 0.5f), b(right, cy + h * 0.5f);
    dl->AddRectFilled(ImVec2(a.x - U(2), a.y - U(2)), ImVec2(b.x + U(2), b.y + U(4)), col(kInk), U(8));
    dl->AddRectFilled(ImVec2(a.x, a.y + U(2)), ImVec2(b.x, b.y + U(2)), col(0x9A9A9A), U(6));
    dl->AddRectFilled(a, b, col(0xF7F7F7), U(6));
    text(dl, g_font.bold, size, a.x + (w - measure(g_font.bold, size, label).x) * 0.5f, cy, col(kInk), label);
    return w;
}

// Eased values that belong to a thing on screen (a switch's knob), keyed by its address.
inline float glide(const void* key, float target, float rate = 16.f) {
    static std::unordered_map<const void*, float> s_values;
    auto [it, fresh] = s_values.try_emplace(key, target);
    if (!fresh) it->second += (target - it->second) * (1.f - std::exp(-ImGui::GetIO().DeltaTime * rate));
    return it->second;
}

// ---------------------------------------------------------------------------------------------------
// Test runs

// The pictures given to ImGui besides its font (src/runtime/gameart.cpp), kept for writeShot.
struct CpuTexture {
    int w = 0, h = 0;
    std::vector<u32> rgba;  // (bytes R, G, B, A)
};
inline std::unordered_map<ImTextureID, CpuTexture>& cpuTextures() {
    static std::unordered_map<ImTextureID, CpuTexture> s_textures;
    return s_textures;
}
inline void keepTexture(ImTextureID id, int w, int h, const std::vector<u32>& rgba) { cpuTextures()[id] = CpuTexture{w, h, rgba}; }

// What the menu drew this frame, rasterised as the GPU would (ImGui's triangles, its font texture and the
// pictures kept above), with the game left transparent: for looking at in test runs.
inline void writeShot(const char* dir, const char* prefix, int n) {
    ImGuiIO& io = ImGui::GetIO();
    const int W = int(io.DisplaySize.x), H = int(io.DisplaySize.y);
    if (W <= 0 || H <= 0) return;
    unsigned char* fontTex = nullptr;
    int fontW = 0, fontH = 0;
    io.Fonts->GetTexDataAsRGBA32(&fontTex, &fontW, &fontH);
    const unsigned char* tex = fontTex;
    int tw = fontW, th = fontH;
    std::vector<float> img(size_t(W) * H * 4, 0.f);  // premultiplied
    auto sample = [&](float u, float v, float out[4]) {
        const float x = u * float(tw) - 0.5f, y = v * float(th) - 0.5f;
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        const float fx = x - float(x0), fy = y - float(y0);
        for (int k = 0; k < 4; k++) out[k] = 0;
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < 2; i++) {
                const int xi = std::clamp(x0 + i, 0, tw - 1), yi = std::clamp(y0 + j, 0, th - 1);
                const float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                const unsigned char* p = tex + (size_t(yi) * tw + xi) * 4;
                for (int k = 0; k < 4; k++) out[k] += w * float(p[k]) / 255.f;
            }
    };
    for (ImDrawList* dl : {ImGui::GetBackgroundDrawList(), ImGui::GetForegroundDrawList()}) {
        for (const ImDrawCmd& cmd : dl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            tex = fontTex, tw = fontW, th = fontH;
            if (auto it = cpuTextures().find(cmd.GetTexID()); it != cpuTextures().end())
                tex = reinterpret_cast<const unsigned char*>(it->second.rgba.data()), tw = it->second.w, th = it->second.h;
            const ImDrawIdx* idx = dl->IdxBuffer.Data + cmd.IdxOffset;
            const ImDrawVert* vtx = dl->VtxBuffer.Data + cmd.VtxOffset;
            for (unsigned t = 0; t + 2 < cmd.ElemCount; t += 3) {
                const ImDrawVert* v[3] = {&vtx[idx[t]], &vtx[idx[t + 1]], &vtx[idx[t + 2]]};
                const float area = (v[1]->pos.x - v[0]->pos.x) * (v[2]->pos.y - v[0]->pos.y) -
                                   (v[2]->pos.x - v[0]->pos.x) * (v[1]->pos.y - v[0]->pos.y);
                if (std::fabs(area) < 1e-6f) continue;
                const int bx0 = std::max({0, int(std::floor(std::min({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x}))), int(cmd.ClipRect.x)});
                const int by0 = std::max({0, int(std::floor(std::min({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y}))), int(cmd.ClipRect.y)});
                const int bx1 = std::min({W - 1, int(std::ceil(std::max({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x}))), int(cmd.ClipRect.z) - 1});
                const int by1 = std::min({H - 1, int(std::ceil(std::max({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y}))), int(cmd.ClipRect.w) - 1});
                for (int y = by0; y <= by1; y++)
                    for (int x = bx0; x <= bx1; x++) {
                        const float px = float(x) + 0.5f, py = float(y) + 0.5f;
                        float w[3];
                        for (int k = 0; k < 3; k++) {
                            const ImDrawVert* a = v[(k + 1) % 3];
                            const ImDrawVert* b = v[(k + 2) % 3];
                            w[k] = ((b->pos.x - a->pos.x) * (py - a->pos.y) - (b->pos.y - a->pos.y) * (px - a->pos.x)) / area;
                        }
                        if (w[0] < 0 || w[1] < 0 || w[2] < 0) continue;
                        float u = 0, vv = 0, c[4] = {0, 0, 0, 0};
                        for (int k = 0; k < 3; k++) {
                            u += w[k] * v[k]->uv.x, vv += w[k] * v[k]->uv.y;
                            for (int ch = 0; ch < 4; ch++) c[ch] += w[k] * float(v[k]->col >> (ch * 8) & 255) / 255.f;
                        }
                        float s[4];
                        sample(u, vv, s);
                        const float a = c[3] * s[3];
                        float* d = &img[(size_t(y) * W + x) * 4];
                        for (int ch = 0; ch < 3; ch++) d[ch] = c[ch] * s[ch] * a + d[ch] * (1 - a);
                        d[3] = a + d[3] * (1 - a);
                    }
            }
        }
    }
    char path[512];
    std::snprintf(path, sizeof path, "%s/%s_%05d.rgba", dir, prefix, n);
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return;
    const u32 hdr[3] = {u32(W), u32(H), 0};
    std::fwrite("RGBA", 1, 4, f);
    std::fwrite(hdr, 4, 3, f);
    std::vector<u8> line(size_t(W) * 4);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const float* d = &img[(size_t(y) * W + x) * 4];
            const float a = d[3];
            for (int ch = 0; ch < 3; ch++) line[x * 4 + ch] = u8(std::clamp(a > 0 ? d[ch] / a : 0.f, 0.f, 1.f) * 255.f + 0.5f);
            line[x * 4 + 3] = u8(std::clamp(a, 0.f, 1.f) * 255.f + 0.5f);
        }
        std::fwrite(line.data(), 1, line.size(), f);
    }
    std::fclose(f);
}

// The mouse, as a menu reads it each frame.
struct Mouse {
    ImVec2 pos;
    bool moved, clicked, down, released;
    float wheel;
    bool in(ImVec2 a, ImVec2 b) const { return pos.x >= a.x && pos.x < b.x && pos.y >= a.y && pos.y < b.y; }
};

// ---------------------------------------------------------------------------------------------------
// Kirby Air Ride's own art (src/runtime/gameart.h); each helper answers false (and draws nothing) until
// that art has loaded, and the menu then draws its flat stand-in.

// Lettering in the game's menu font (descriptions, titles, values) - in `fallback` until it is there.
// size: as an ImGui font's (the game's glyph cells are drawn a little larger to match its letters).
constexpr float kGameFontScale = 1.0f;
inline float gameText(ImDrawList* dl, ImFont* fallback, float size, float x, float cy, ImU32 c, const char* s, const char* e = nullptr) {
    if (!gameart::fontReady()) return text(dl, fallback, size, x, cy, c, s, e);
    return gameart::text(dl, F(fallback), size * kGameFontScale, std::floor(x), cy, c, s, e);
}
inline float gameMeasure(ImFont* fallback, float size, const char* s, const char* e = nullptr) {
    if (!gameart::fontReady()) return measure(fallback, size, s, e).x;
    return gameart::measure(F(fallback), size * kGameFontScale, s, e);
}
// Where a line `width` wide breaks (from s), in that lettering.
inline const char* gameWrap(ImFont* fallback, float size, const char* s, const char* e, float width) {
    if (!e) e = s + std::strlen(s);
    if (!gameart::fontReady()) {
        const float scale = size / F(fallback)->FontSize;
        const char* p = F(fallback)->CalcWordWrapPositionA(scale, s, e, std::max(width, size));
        return p > s ? p : s + 1;
    }
    return gameart::wrap(F(fallback), size * kGameFontScale, s, e, width);
}

// The mode menus' button: the ball at its left end, the body leaning forward, tinted `fill` (grey; orange
// in focus). Cut in three: the ball's end and the rounded end keep their shape, the middle stretches. Its
// ball's centre lands at x0 + 0.42 h.
inline bool artButton(ImDrawList* dl, float x0, float y0, float x1, float y1, u32 fill, float a = 1.f, bool ballLit = false) {
    const gameart::Texture& t = gameart::get(gameart::Art::Button);
    if (!t) return false;
    const float k = (y1 - y0) / float(t.h), kx = k * 1.3f;  // (the game draws the art 1.3 times wider than tall)
    const float l = 44.f, r = 30.f, tw = float(t.w);
    const float xl = x0 + l * kx, xr = std::max(xl, x1 - r * kx);
    const ImU32 c = col(fill, a);
    dl->AddImage(t.id, ImVec2(x0, y0), ImVec2(xl, y1), ImVec2(0, 0), ImVec2(l / tw, 1), c);
    if (xr > xl) dl->AddImage(t.id, ImVec2(xl, y0), ImVec2(xr, y1), ImVec2(l / tw, 0), ImVec2((tw - r) / tw, 1), c);
    dl->AddImage(t.id, ImVec2(xr, y0), ImVec2(xr + r * kx, y1), ImVec2((tw - r) / tw, 0), ImVec2(1, 1), c);
    // the ball: dark, white in focus (as the game lights it)
    const ImVec2 ball(x0 + 15.5f * kx, y0 + 19.5f * k);
    dl->AddEllipseFilled(ball, ImVec2(9.f * kx, 11.5f * k), col(ballLit ? 0xF4F0EA : 0x3A3A3A, a), 0.f, 24);
    dl->AddEllipseFilled(ImVec2(ball.x - 2.5f * kx, ball.y - 4.f * k), ImVec2(3.f * kx, 3.5f * k), col(kWhite, a * (ballLit ? 0.9f : 0.3f)), 0.f, 12);
    return true;
}

// The maroon band behind the button in focus: in from `xLeft`, its swept end at xRight.
inline bool artBand(ImDrawList* dl, float xLeft, float y0, float xRight, float y1, float a = 1.f) {
    const gameart::Texture& t = gameart::get(gameart::Art::Band);
    if (!t) return false;
    const float k = (y1 - y0) / float(t.h), cut = 20.f, tw = float(t.w);
    const float xe = std::max(xLeft, xRight - (tw - cut) * k);
    const ImU32 c = col(kWhite, a);
    if (xe > xLeft) dl->AddImage(t.id, ImVec2(xLeft, y0), ImVec2(xe, y1), ImVec2(0, 0), ImVec2(cut / tw, 1), c);
    dl->AddImage(t.id, ImVec2(xe, y0), ImVec2(xRight, y1), ImVec2(cut / tw, 0), ImVec2(1, 1), c);
    return true;
}

// A picture made of one quarter (its centre at the art's top right corner) turned four times round `c`,
// turned by `turn` radians: the backdrops' toothed ring, the disc's grooves.
inline bool artRing(ImDrawList* dl, gameart::Art which, ImVec2 c, float r, ImU32 tint, float turn = 0.f) {
    const gameart::Texture& t = gameart::get(which);
    if (!t) return false;
    const float cs = std::cos(turn), sn = std::sin(turn);
    auto at = [&](float x, float y) { return ImVec2(c.x + (x * cs - y * sn) * r, c.y + (x * sn + y * cs) * r); };
    for (int q = 0; q < 4; q++) {  // (the quarter lower left of the centre, then mirrored)
        const float mx = q & 1 ? -1.f : 1.f, my = q & 2 ? -1.f : 1.f;
        dl->AddImageQuad(t.id, at(-mx, 0), at(0, 0), at(0, my), at(-mx, my), ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), tint);
    }
    return true;
}

// The step in a screen's title line, rising from (x, yLow) to (x + w, yHigh).
inline bool artTitleStep(ImDrawList* dl, float x, float yLow, float yHigh, ImU32 tint) {
    const gameart::Texture& t = gameart::get(gameart::Art::TitleStep);
    if (!t) return false;
    // (the art's line runs from its lower left, 4/24 up from the bottom, to its upper right, 4/24 down)
    const float h = (yLow - yHigh) / (16.f / 24.f), w = h * float(t.w) / float(t.h);
    dl->AddImage(t.id, ImVec2(x, yHigh - h * 4.f / 24.f), ImVec2(x + w, yHigh - h * 4.f / 24.f + h), ImVec2(0, 0), ImVec2(1, 1), tint);
    return true;
}

}  // namespace rt::menustyle
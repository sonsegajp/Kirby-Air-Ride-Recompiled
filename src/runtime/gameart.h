#pragma once
// Kirby Air Ride's own menu art for the port's menus (the F1 menu, the Online menu), taken at run time
// from the player's disc and the game in memory - nothing of the game is in the port:
//   - pieces of the mode menus (MnAll.dat on the disc): the leaning button with its ball, the maroon
//     band behind the button in focus, the step in a screen's title line, the toothed ring of the
//     backdrops, the grooves of the emblem disc - each recognised by the hash of its texels (an
//     unknown disc revision just leaves the port's flat drawing in their place);
//   - the menus' font: the glyphs the game letters its descriptions with (main.dol, in memory);
//   - the ONLINE emblem and lettering (the port's own redrawing of the LAN logo, online_logo.inc).
// Everything here runs on the frontend's thread; textures go to Dear ImGui through Aurora.
#include <imgui.h>

#include <cstdint>

namespace rt::gameart {

enum class Art : uint8_t {
    Button,       // 128x48: a ball at the left of a capsule leaning forward (tinted: grey, orange in focus)
    Band,         // 80x56: the maroon band behind the button in focus, its right end swept
    TitleStep,    // 32x24: the step in a title line
    Gear,         // 128x128: a quarter of the backdrops' toothed ring (its centre at the top right)
    Grooves,      // 64x64: a quarter of the emblem disc's grooves (centre at the top right)
    Emblem,       // 64x64: the ONLINE globe (white on clear)
    Lettering,    // 56x24: "ONLINE"
    Count,
};

struct Texture {
    ImTextureID id{};
    int w = 0, h = 0;
    explicit operator bool() const { return w > 0; }
};

// Loads what is there to load (once the disc and the game are up; cheap after the first time).
void load();
const Texture& get(Art a);

// The game's menu font. size: the line's height in pixels. Characters it has no glyph for are lettered
// in `fallback` (an ImGui font). Returns the width.
bool fontReady();
float text(ImDrawList* dl, ImFont* fallback, float size, float x, float cy, ImU32 col, const char* s, const char* e = nullptr);
float measure(ImFont* fallback, float size, const char* s, const char* e = nullptr);
// Where a line of `width` pixels breaks, from s (the first character past it; at least one).
const char* wrap(ImFont* fallback, float size, const char* s, const char* e, float width);

}  // namespace rt::gameart

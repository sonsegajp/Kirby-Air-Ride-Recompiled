#pragma once
// Kirby's own model for the port's menus (the Online menu's Profile): RdKirby.dat from the player's disc - the
// rider's model, its textures and its colours - drawn by the port into a picture Dear ImGui shows. Nothing of
// the game is in the port. Runs on the frontend's thread.
#include <imgui.h>

namespace rt::kirbymodel {

// The game's Kirby colours, in the order of the model's colour textures.
enum Colour : int { Pink, Yellow, Blue, Green, White, Purple, Brown, Red, ColourCount };

// Draws Kirby standing in the square of `size` pixels centred at c, in his standing motion, turning a little (t:
// seconds). False when the model is not there (no disc yet, an unknown revision): the caller draws its own.
bool draw(ImDrawList* dl, ImVec2 c, float size, int colour, float t, float alpha = 1.f);

}  // namespace rt::kirbymodel

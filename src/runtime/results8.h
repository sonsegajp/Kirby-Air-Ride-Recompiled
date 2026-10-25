#pragma once
// City Trial's results with more than 4 riders (the 8-player build, docs/MORE_PLAYERS.md): the game's own results
// screen has places for 4 (its layout's rows, and the loops that fill them), so while it runs with 8 riders the
// port draws one with all of them over it - in the game's look (its green, its art and lettering, the winner's
// Kirby in their colour), from the game's own results (the Stadium's places, times, points and distances).
// Runs on the frontend's thread (src/runtime/frontend.cpp), after the game's picture.

namespace rt::results8 {

// Between aurora_begin_frame and aurora_end_frame, before the F1 menu's overlay.
void overlay(bool headless);

}  // namespace rt::results8

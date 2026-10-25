#pragma once
// The Online menu (src/runtime/online_menu.cpp): Kirby Air Ride's LAN mode played over the internet
// (docs/ONLINE.md). When the game enters its Online (LAN) mode it takes over the screen: sign in, browse
// or host servers, form a match group in a server's lobby, then hand back to the game's own LAN screens,
// which connect through the master server's relay. Drawn with Dear ImGui in the F1 menu's style
// (src/runtime/menustyle.h). Everything here runs on the frontend's thread (src/runtime/frontend.cpp).
#include <SDL3/SDL_events.h>

struct PADStatus;

namespace rt::online {

void event(const SDL_Event& e);    // each AURORA_SDL_EVENT: typing, Escape, F2 (the menu again, in Online mode)
// After each PADRead and menu::takePads: the menu is steered by the controllers; while it is open the game
// sees no input (but the presses the menu hands it: Retry / Cancel on its LAN screen).
void takePads(PADStatus pads[4]);
// Between aurora_begin_frame and aurora_end_frame, after menu::overlay (it draws behind the F1 menu).
void overlay(bool headless);
bool isOpen();
void shutdown();                   // at exit: leaves the server, closes the connection

}  // namespace rt::online

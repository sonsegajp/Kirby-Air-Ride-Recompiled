#pragma once
// The F1 menu (src/runtime/menu.cpp): the player's settings (src/runtime/settings.h) over the game, in
// Kirby Air Ride's own colours, drawn with Dear ImGui (Aurora draws it over the picture). Also the small
// things drawn over the game while it is closed: the frame counter and the "F1: Menu" reminder.
// Everything here runs on the frontend's thread (src/runtime/frontend.cpp).
#include <SDL3/SDL_events.h>

struct PADStatus;

namespace rt::menu {

void init();                    // once, after aurora_initialize (the menu's fonts)
void event(const SDL_Event& e); // each AURORA_SDL_EVENT: F1 opens and closes it, Escape closes it
// After each PADRead: the menu is steered by the controllers (and the keyboard standing in for port 1);
// while it is open the game sees no input at all.
void takePads(PADStatus pads[4]);
// Between aurora_begin_frame and aurora_end_frame. (Headless runs draw nothing, unless SA2_MENU_OPEN asks.)
void overlay(bool headless);
// The gain to play at: Master Volume, or nothing while the window is in the background and Mute in
// Background is on.
float volume();
bool isOpen();

}  // namespace rt::menu

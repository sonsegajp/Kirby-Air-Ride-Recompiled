#pragma once
// City Trial maps (src/runtime/maps.cpp): the city itself, or a map mod played in its place - each in a folder of its
// own under mods/maps beside the program, as the map editor builds it (tools/mapeditor: its stage files and mod.json).
#include <string>

namespace rt::maps {

int count();                   // the maps to choose from: the city (0), then each map mod found
std::string name(int i);       // as shown: the city's, or the mod's own (mod.json "name"; else its folder's)
int selected();
void select(int i);            // served from the next match on (and kept in the settings)
void rescan();                 // the mods folder looked at again (the choice kept if still there)
// (The game's thread, as each of these serves files to it.)

// An online session's stage (src/runtime/lansession.cpp, as each match starts): the map mod with this content
// (src/runtime/mapfiles.h: its hash), looked for in `folder` first; folder "": the city. Served without being kept in
// the settings; false (nothing changed) if this PC has no such map. endSession() serves the player's own choice again.
bool selectForSession(const std::string& folder, const std::string& hash);
void endSession();

}  // namespace rt::maps

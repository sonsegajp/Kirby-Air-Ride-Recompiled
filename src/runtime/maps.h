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

}  // namespace rt::maps

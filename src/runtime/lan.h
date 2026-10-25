#pragma once
// Inside the LAN support: src/runtime/lan.cpp (the adapter's driver, replaced) for
// src/runtime/lansync.cpp (the game's lockstep, pipelined). The transport's side is lanport.h.
#include "runtime/lanport.h"

#include <cstddef>
#include <cstdint>

namespace rt::lan {

// Frames of our own, beside the game's (lan.cpp): sent to the transport when its link is up; those
// of `etherType` that arrive go to `handler` (on the network thread) instead of the game.
void sendRaw(const uint8_t* frame, size_t len);
using RawHandler = void (*)(const uint8_t* frame, size_t len);
void setRawHandler(uint16_t etherType, RawHandler handler);
void ownMac(uint8_t out[6]);

void syncInstall();  // lansync.cpp: at boot, after the driver (lan.cpp)

}  // namespace rt::lan

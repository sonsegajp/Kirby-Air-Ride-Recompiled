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

// A session started by host code (src/runtime/lansession.cpp), for the lockstep and the connection
// (lansync.cpp): its PCs (2-8), this one's place among them, an id the same on all of them. The game
// sees at most 4 consoles: PCs 0-3 are consoles 0-3 (each its controller on port 1, pad 0); PCs 4-7
// sit on spare ports of consoles 0 and 1 (seatOf) - the raw network pads 6, 7, 5 and 9, which riders
// 6, 7, 8 and 9 read (src/runtime/lansession.cpp enters them). setSession(0, ...): none (the game's own
// LAN menus decide).
struct Seat {
    int console, port;
};
inline Seat seatOf(int pc) {
    static const Seat k[8] = {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {0, 2}, {0, 3}, {0, 1}, {1, 1}};
    return k[pc & 7];
}
inline int riderOf(int pc) { return pc < 4 ? pc : pc + 2; }  // its rider's slot (more than 4 PCs)
inline int padOf(int pc) { return pc < 4 ? pc : 4 + 4 * seatOf(pc).console + seatOf(pc).port; }  // the pad it reads
void setSession(int pcs, int me, uint32_t id);
int sessionPcs();  // 0: none
int sessionMe();
bool inSteps();         // the game's main loop is running logic steps (lansync.cpp)
uint32_t lanSteps();    // LAN logic steps run so far
uint32_t mainThread();  // the game's main loop thread (OSThread), once a LAN step ran

}  // namespace rt::lan

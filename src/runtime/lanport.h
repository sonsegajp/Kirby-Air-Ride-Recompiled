#pragma once
// Where Kirby Air Ride's LAN packets go. The game's own network library (its broadband-adapter
// driver) is replaced by host code (src/runtime/lan.cpp, HLE hooks - no adapter hardware is
// emulated), which hands each packet to a transport: the game's LAN mode, over whatever connects the
// consoles -
// another copy on this PC (a loopback for tests), or the Air Ride master server's relay
// (src/runtime/airnet.cpp), which joins the members of one match group into one virtual LAN.
//
// Frames are whole Ethernet frames as the game builds them (destination MAC, source MAC, type,
// payload; no preamble or CRC), 14..1514 bytes.
//
// What goes over it (measured, src/runtime/lan.cpp): the game runs the SDK's own IP stack, so a
// transport is a plain Ethernet segment - deliver unicast frames to the MAC they name and broadcast
// (ff:ff:ff:ff:ff:ff) / multicast (01:00:5e:...) frames to every other member; never back to the
// sender. On joining, each console sends DHCP discovers (3) and, unanswered, ~35 s later (the game's
// "may take about a minute") takes a 169.254.x.x link-local address (ARP probes/announce), joins
// 239.255.255.250 (IGMP) and finds the others by SSDP (UDP 1900 multicast), then the game's
// handshake (UDP 26502). The match itself (src/runtime/lansync.cpp, which replaces the game's own
// token ring - strict lockstep at 1 / (a trip round the ring) frames a second: 9 fps at 100 ms) is
// one broadcast frame per console per game frame, EtherType 0x88B5, ~89 bytes + 92 per round it
// carries (each round goes in the next 4 frames: ~460 bytes, ~28 KB/s out per console; resends of
// rounds not acknowledged within 1.5 round trips). Each console waits only for the others' round of
// D frames ago; D (input delay + 1) starts from the measured round trips (95th percentile) and adapts
// to lateness, per console (it changes when, not what, the game is given: sync is unaffected). A late
// round costs only the console it is late for. Up to 4 consoles.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rt::lan {

struct Port {
    virtual ~Port() = default;
    // The adapter sends a frame (any thread; must not block).
    virtual void send(const uint8_t* frame, size_t len) = 0;
    // A frame for the adapter, if one has arrived (any thread; must not block).
    virtual bool poll(std::vector<uint8_t>& frame) = 0;
    // Whether the link is up (the adapter reports its cable plugged in only then).
    virtual bool linkUp() const = 0;
};

// The transport in use (nullptr: no cable - the adapter is present but sees no network).
void setPort(Port* port);
Port* port();

// This console's MAC address (the adapter's; stable per installation, locally administered).
void macAddress(uint8_t out[6]);

// Whether the game is in its LAN (Online) mode now, asking for the link (src/runtime/lan.cpp: it polled
// the link state within the last second). The Online menu (src/runtime/online_menu.cpp) opens on it.
bool linkWanted();

// Whether the game's DHCP requests are answered locally (off by default; SA2_LAN_DHCP=1): with it,
// a console has its address in a second or two instead of ~35 s (the IP stack's DHCP tries, then
// its own link-local address). The address is the link-local one its MAC picks (169.254.x.y/16), so
// consoles with and without it still reach each other. A transport turns it on when it joins.
void setLocalDhcp(bool on);
// The address that local DHCP answer gives (src/runtime/lansession.cpp): the game orders the consoles of a
// session by address (console 0, the "master", lowest), so place p (0-3) answers 169.254.200.(10 + p) and
// the game's order follows the lobby's. -1 (the default): the address the MAC picks.
void setLocalDhcpPlace(int place);

}  // namespace rt::lan

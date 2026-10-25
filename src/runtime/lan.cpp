// Kirby Air Ride's LAN mode without the broadband adapter (src/runtime/lanport.h has the interface).
//
// The game links the Dolphin SDK's IP stack (IP/SO, with ARP, IGMP, DHCP and link-local
// addressing) over the SDK's ETH library, the broadband adapter's driver (EXI channel 0, device 2;
// "<< Dolphin SDK - ETH release build: Jun 4 2003 >>"). Nothing of the adapter is emulated here:
// ETH's API is replaced by host code (the functions run in place of the driver's, see install()),
// so the game's own IP stack and LAN code run unchanged on top and hand us whole Ethernet frames,
// which go to whatever transport is plugged in (rt::lan::Port: a loopback to other copies on this
// PC for tests, or the master server's relay).
//
// The ETH API as Kirby Air Ride (main.dol, GKYE01) has it, and what replaces it:
//   80467684 s32  ETHInit(s32 mode)                  adapter found and set up: 1 (here: always)
//   80467F44 BOOL ETHSendAsync(void* frame, s32 len, void (*done)(s32 status))
//                                                    the frame goes to the transport at once; `done`
//                                                    runs as an interrupt (status 0: sent)
//   80468148 void ETHGetMACAddr(u8 mac[6])           the driver's own (reads the MAC ETHInit stored)
//   80468170 void ETHSetRecvCallback(cb0, cb1)       receive on (cb0 set) or off; frames are delivered
//                                                    as the driver does: in an interrupt,
//                                                    buf = cb0(u16 type, s32 len, u8 status) - a
//                                                    buffer for it, or null to drop it - the frame
//                                                    copied there, then cb1(buf, len). len counts
//                                                    the frame plus 4 (the adapter's CRC).
//   804685F8 BOOL ETHGetLinkState(s32* up)           the transport's link
//   80468680 void ETHSetProtoType(u16* types, s32 n) the driver's own (stores the receive filter,
//                                                    applied here: IP, ARP, PPPoE)
//   80468838 void ETHAddMulticastAddress(u8 mac[6])  nothing: every multicast frame is let through
//   80468A9C void ETHClearMulticastAddresses(void)   (the IP stack keeps its own group list)
// The rest of the driver (its EXI interrupt handler 80466738, receive and DMA routines) never runs.
// EXIGetType(0, 2) - the game's "is there a broadband adapter" (f_803EB940, through EXIGetID) -
// answers the adapter's ID (0x04020200), which enables the LAN icon and starts the IP stack at boot.
//
// SA2_LAN=0: no adapter (as before). SA2_LAN_LOOPBACK=<n> (1-4): the loopback transport, copy n of
// up to 4 on this PC (UDP 127.0.0.1:47400+n, frames to the others), or SA2_LAN_LOOPBACK=<my port>:
// <peer port>[,<peer port>...]; SA2_LAN_DELAY_MS=<ms> adds latency to it, SA2_LAN_LOSS=<per 1000> loss,
// SA2_LAN_JITTER_MS / SA2_LAN_SPIKE jitter (Loopback). SA2_LAN_MAC=xx:xx:xx:xx:xx:xx sets
// the MAC; SA2_LAN_LOG=1 logs the first 200 frames and totals every 5 s, =2 every frame. SA2_LAN_DHCP=1:
// the game's DHCP requests are answered here (setLocalDhcp).
#include "runtime/lan.h"

#include "runtime/interp.h"
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#pragma comment(lib, "ws2_32.lib")
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rt::lan {

namespace {
// ---- the driver's functions and state in main.dol ----
constexpr u32 kETHInit = 0x80467684, kETHSendAsync = 0x80467F44, kETHSetRecvCallback = 0x80468170,
              kETHGetLinkState = 0x804685F8, kETHAddMulticastAddress = 0x80468838,
              kETHClearMulticastAddresses = 0x80468A9C;
constexpr u32 kSda = 0x805DD0E0;  // r13
constexpr u32 kMac = kSda + 0x14B8, kProtoTypes = kSda + 0x14CC, kProtoCount = kSda + 0x14D0,
              kRecvCb0 = kSda + 0x14D4, kRecvCb1 = kSda + 0x14D8, kSendCb = kSda + 0x14DC, kSending = kSda + 0x14C8;
constexpr u32 kStats = 0x805B5460, kStatsSize = 0x24;  // the driver's counters (ETHInit clears them)
// An instruction of each, to be sure this is the program they were found in.
constexpr struct {
    u32 addr, word;
} kSignature[] = {
    {0x804676AC, 0x806D00F8},  // ETHInit: lwz r3, __ETHVersion(r13)
    {0x80467F54, 0x906D14E4},  // ETHSendAsync: stw r3, 0x14E4(r13)
    {0x8046814C, 0x388D14B8},  // ETHGetMACAddr: addi r4, r13, 0x14B8
    {0x804681E0, 0x938D14D4},  // ETHSetRecvCallback: stw r28, 0x14D4(r13)
    {0x80468628, 0x38600031},  // ETHGetLinkState: li r3, 0x31 (the NWay status register)
    {0x804686A0, 0x93CD14CC},  // ETHSetProtoType: stw r30, 0x14CC(r13)
    {0x8046883C, 0x388D00FC},  // ETHAddMulticastAddress: addi r4, r13, broadcast
    {0x80468B0C, 0x38032600},  // ETHClearMulticastAddresses: the hash registers (0x26)
    {0x803EB970, 0x3C800402},  // EXIGetType: lis r4, 0x0402
};
constexpr u32 kAdapterId = 0x04020200;  // EXIGetID of the broadband adapter
constexpr size_t kMinFrame = 60, kMaxFrame = 1514;

bool s_enabled = false;  // the adapter is there (set at boot)
std::atomic<Port*> s_port{nullptr};
std::atomic<long long> s_linkPolled{0};  // steady_clock ticks of the last link poll in LAN mode (linkWanted)
u8 s_mac[6];
int s_log = 0;  // SA2_LAN_LOG: 1 the first frames and totals, 2 every frame
std::atomic<u32> s_logged{0};
int s_trace = 0;
bool logFrame() { return s_log >= 2 || (s_log == 1 && s_logged++ < 200); }

// ---- frames waiting for the game ----
std::mutex s_rxLock;
std::deque<std::vector<u8>> s_rx;
bool s_rxPosted = false;  // a delivery interrupt is queued
std::atomic<bool> s_pumpStarted{false};
std::atomic<u16> s_rawType{0};
std::atomic<RawHandler> s_rawHandler{nullptr};

// ---- statistics (SA2_LAN_LOG) ----
std::atomic<u64> s_txFrames{0}, s_txBytes{0}, s_rxFrames{0}, s_rxBytes{0}, s_rxDropped{0};

std::string describe(const u8* f, size_t n) {
    char b[256];
    auto mac = [](const u8* m) {
        char s[18];
        std::snprintf(s, sizeof s, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
        return std::string(s);
    };
    if (n < 14) return "runt";
    const u16 type = u16(f[12] << 8 | f[13]);
    std::string s = mac(f + 6) + " > " + mac(f) + " ";
    if (type == 0x0806 && n >= 42) {
        std::snprintf(b, sizeof b, "ARP %s %u.%u.%u.%u > %u.%u.%u.%u", f[21] == 1 ? "who-has" : "reply", f[28], f[29], f[30], f[31],
                      f[38], f[39], f[40], f[41]);
        return s + b;
    }
    if (type == 0x0800 && n >= 34) {
        const u8* ip = f + 14;
        const u32 ihl = (ip[0] & 15) * 4, len = u32(ip[2] << 8 | ip[3]);
        std::snprintf(b, sizeof b, "IP %u.%u.%u.%u > %u.%u.%u.%u proto %u len %u", ip[12], ip[13], ip[14], ip[15], ip[16], ip[17], ip[18],
                      ip[19], ip[9], len);
        s += b;
        if (ip[9] == 17 && n >= 14 + ihl + 8) {
            const u8* u = ip + ihl;
            std::snprintf(b, sizeof b, " UDP %u > %u (%u bytes)", u[0] << 8 | u[1], u[2] << 8 | u[3], (u[4] << 8 | u[5]) - 8);
            s += b;
        }
        return s;
    }
    std::snprintf(b, sizeof b, "type %04x len %zu", type, n);
    return s + b;
}

// ---- receiving: frames reach the game in an interrupt, as the adapter's did ----
void deliver(Ctx& c, std::vector<u8>& f) {
    const u32 cb0 = rd32(kRecvCb0), cb1 = rd32(kRecvCb1);
    if (!cb0) return;  // receiver off
    const u16 type = u16(f[12] << 8 | f[13]);
    if (const s32 n = s32(rd32(kProtoCount)); n > 0) {
        const u32 types = rd32(kProtoTypes);
        bool wanted = false;
        for (s32 i = 0; i < n && !wanted; i++) wanted = rd16(types + 2 * i) == type;
        if (!wanted) return;
    }
    if (f.size() < kMinFrame) f.resize(kMinFrame, 0);  // as it arrives: padded to the minimum
    const u32 len = u32(f.size()) + 4;                 // ... and its CRC counted
    const u32 buf = guestCall(c, cb0, {type, len, 0});
    if (!buf) {
        s_rxDropped++;
        return;
    }
    std::memcpy(hostp(buf), f.data(), f.size());
    std::memset(hostp(buf + u32(f.size())), 0, 4);
    if (cb1) guestCall(c, cb1, {buf, len});
}

void deliverAll(Ctx& c) {
    for (;;) {
        std::vector<u8> f;
        {
            std::lock_guard g(s_rxLock);
            if (s_rx.empty()) {
                s_rxPosted = false;
                return;
            }
            f = std::move(s_rx.front());
            s_rx.pop_front();
        }
        deliver(c, f);
    }
}

// A frame for the game from here (not the transport): delivered like the others.
void queueForGame(std::vector<u8> f) {
    bool post;
    {
        std::lock_guard g(s_rxLock);
        s_rx.push_back(std::move(f));
        post = !s_rxPosted;
        s_rxPosted = true;
    }
    if (post) sched::postIrq(deliverAll);
}

// Whether the adapter takes a frame: to its MAC, broadcast or multicast; not its own coming back.
bool accepts(const std::vector<u8>& f) {
    if (f.size() < 14 || f.size() > kMaxFrame + 4) return false;
    if (std::memcmp(f.data() + 6, s_mac, 6) == 0) return false;
    return (f[0] & 1) || std::memcmp(f.data(), s_mac, 6) == 0;
}

void pumpMain() {
    profileRegisterThread();
    hostFloatMode();
    auto lastStats = std::chrono::steady_clock::now();
    std::vector<u8> f;
    for (;;) {
        bool any = false;
        if (Port* p = s_port.load()) {
            while (p->poll(f)) {
                if (!accepts(f)) continue;
                if (const RawHandler h = s_rawHandler.load(); h && u16(f[12] << 8 | f[13]) == s_rawType.load()) {
                    h(f.data(), f.size());  // ours (lansync.cpp), not the game's
                    continue;
                }
                if (!rd32(kRecvCb0)) continue;
                s_rxFrames++, s_rxBytes += f.size();
                if (logFrame()) log("[lan] rx %s\n", describe(f.data(), f.size()).c_str());
                std::lock_guard g(s_rxLock);
                if (s_rx.size() < 256) s_rx.push_back(f);  // (a game that stopped taking them)
                any = true;
            }
        }
        if (any) {
            bool post;
            {
                std::lock_guard g(s_rxLock);
                post = !s_rxPosted && !s_rx.empty();
                if (post) s_rxPosted = true;
            }
            if (post) sched::postIrq(deliverAll);
        }
        if (s_log && std::chrono::steady_clock::now() - lastStats > std::chrono::seconds(5)) {
            lastStats = std::chrono::steady_clock::now();
            log("[lan] totals: sent %llu frames (%llu bytes), received %llu (%llu bytes), %llu dropped by the game\n",
                (unsigned long long)s_txFrames.load(), (unsigned long long)s_txBytes.load(), (unsigned long long)s_rxFrames.load(),
                (unsigned long long)s_rxBytes.load(), (unsigned long long)s_rxDropped.load());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void startPump() {
    bool expected = false;
    if (s_pumpStarted.compare_exchange_strong(expected, true)) std::thread(pumpMain).detach();
}

// ---- the replaced driver functions (guest calling convention: arguments r3.., result r3) ----
void ethInit(Ctx& c, u32) {
    std::memcpy(hostp(kMac), s_mac, 6);
    for (u32 a : {kProtoTypes, kProtoCount, kRecvCb0, kRecvCb1, kSendCb, kSending}) wr32(a, 0);
    std::memset(hostp(kStats), 0, kStatsSize);
    log("[lan] ETHInit(%u): adapter %02x:%02x:%02x:%02x:%02x:%02x, %s\n", c.r[3], s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4],
        s_mac[5], s_port.load() ? "transport attached" : "no transport yet");
    c.r[3] = 1;
}

// ---- a DHCP server of our own (setLocalDhcp): the IP stack asks for an address when its link comes
// up and, with nobody answering, gives up after 3 tries over ~35 s and takes a link-local one. This
// answers at once, from here, with the link-local address its MAC picks (169.254.x.y/16, the
// subnet the stack would end in anyway, so it still reaches consoles that took theirs themselves).
std::atomic<bool> s_localDhcp{false};
std::atomic<int> s_dhcpPlace{-1};  // setLocalDhcpPlace (src/runtime/lansession.cpp): 169.254.200.(10 + place)
constexpr u8 kDhcpServerIp[4] = {169, 254, 255, 254};
constexpr u8 kDhcpServerMac[6] = {0x02, 0x00, 0x5E, 0x00, 0x00, 0x67};

u16 ipChecksum(const u8* p, size_t n) {
    u32 sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2) sum += u32(p[i] << 8 | p[i + 1]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return u16(~sum);
}

void queueForGame(std::vector<u8> f);

// `f` a DHCP discover or request from the game: the offer or ack goes back to it (returns whether).
bool answerDhcp(const u8* f, size_t n) {
    if (n < 14 + 20 + 8 + 240 || f[12] != 0x08 || f[13] != 0x00 || f[14 + 9] != 17) return false;
    const u8* ip = f + 14;
    const size_t ihl = size_t(ip[0] & 15) * 4;
    const u8* udp = ip + ihl;
    if ((udp[0] << 8 | udp[1]) != 68 || (udp[2] << 8 | udp[3]) != 67) return false;
    const u8* bootp = udp + 8;
    const size_t blen = n - size_t(bootp - f);
    if (blen < 240 || bootp[0] != 1 || bootp[236] != 0x63 || bootp[237] != 0x82 || bootp[238] != 0x53 || bootp[239] != 0x63) return false;
    int type = 0;
    for (size_t i = 240; i + 1 < blen && bootp[i] != 255;) {
        if (bootp[i] == 0) { i++; continue; }
        if (bootp[i] == 53 && bootp[i + 1] >= 1 && i + 2 < blen) type = bootp[i + 2];
        i += 2 + bootp[i + 1];
    }
    if (type != 1 && type != 3) return false;  // discover, request
    u8 mine[4] = {169, 254, u8(1 + (s_mac[3] ^ s_mac[5]) % 254), u8(1 + (s_mac[4] ^ s_mac[2]) % 254)};
    if (const int place = s_dhcpPlace.load(); place >= 0) mine[2] = 200, mine[3] = u8(10 + place);  // (the game orders consoles by address)
    std::vector<u8> r(14 + 20 + 8 + 240 + 32, 0);
    std::memcpy(r.data(), f + 6, 6);  // to the console
    std::memcpy(r.data() + 6, kDhcpServerMac, 6);
    r[12] = 0x08, r[13] = 0x00;
    u8* rip = r.data() + 14;
    u8* rudp = rip + 20;
    u8* rb = rudp + 8;
    rb[0] = 2, rb[1] = 1, rb[2] = 6;
    std::memcpy(rb + 4, bootp + 4, 4);    // xid
    std::memcpy(rb + 10, bootp + 10, 2);  // flags
    std::memcpy(rb + 16, mine, 4);        // yiaddr
    std::memcpy(rb + 20, kDhcpServerIp, 4);
    std::memcpy(rb + 28, bootp + 28, 16);  // chaddr
    rb[236] = 0x63, rb[237] = 0x82, rb[238] = 0x53, rb[239] = 0x63;
    u8* o = rb + 240;
    *o++ = 53, *o++ = 1, *o++ = u8(type == 1 ? 2 : 5);  // offer / ack
    *o++ = 54, *o++ = 4, std::memcpy(o, kDhcpServerIp, 4), o += 4;
    *o++ = 51, *o++ = 4, *o++ = 0x00, *o++ = 0x01, *o++ = 0x51, *o++ = 0x80;  // lease: a day
    *o++ = 1, *o++ = 4, *o++ = 255, *o++ = 255, *o++ = 0, *o++ = 0;           // subnet 255.255.0.0
    *o++ = 255;
    const size_t udpLen = size_t(o - rudp);
    r.resize(size_t(o - r.data()));
    rip[0] = 0x45, rip[2] = u8((20 + udpLen) >> 8), rip[3] = u8(20 + udpLen), rip[8] = 64, rip[9] = 17;
    std::memcpy(rip + 12, kDhcpServerIp, 4);
    std::memset(rip + 16, 0xFF, 4);  // broadcast: the console has no address yet
    const u16 sum = ipChecksum(rip, 20);
    rip[10] = u8(sum >> 8), rip[11] = u8(sum);
    rudp[1] = 67, rudp[3] = 68, rudp[4] = u8(udpLen >> 8), rudp[5] = u8(udpLen);  // (no UDP checksum)
    log("[lan] DHCP %s: %u.%u.%u.%u (answered here)\n", type == 1 ? "discover" : "request", mine[0], mine[1], mine[2], mine[3]);
    queueForGame(std::move(r));
    return true;
}

void ethSendAsync(Ctx& c, u32) {
    const u32 addr = c.r[3], cb = c.r[5];
    const u32 len = std::min<u32>(std::max<u32>(c.r[4], kMinFrame), kMaxFrame);
    Port* p = s_port.load();
    if (s_localDhcp && p && p->linkUp() && answerDhcp(hostp(addr), len)) {
        // (not sent: the others' consoles need not see it)
    } else if (p && p->linkUp()) {
        p->send(hostp(addr), len);
        s_txFrames++, s_txBytes += len;
        if (logFrame()) log("[lan] tx %s\n", describe(hostp(addr), len).c_str());
    }
    if (s_trace > 0 && describe(hostp(addr), len).find("26502") != std::string::npos) {
        // SA2_LAN_TRACE=<n> (debugging): who sends the first n frames of the game's own protocol
        s_trace--;
        std::string chain;
        char b[16];
        std::snprintf(b, sizeof b, " %08X", c.lr);
        chain += b;
        for (u32 sp = c.r[1], i = 0; sp && i < 16; sp = rd32(sp), i++) {
            std::snprintf(b, sizeof b, " %08X", rd32(rd32(sp) + 4));
            chain += b;
            if (!rd32(sp)) break;
        }
        log("[lan] send %s from%s\n", describe(hostp(addr), len).c_str(), chain.c_str());
    }
    if (cb) sched::postIrq([cb](Ctx& ic) { guestCall(ic, cb, {0}); });  // sent (status: no errors)
    c.r[3] = 1;
}

void ethSetRecvCallback(Ctx& c, u32) {
    wr32(kRecvCb0, c.r[3]);
    wr32(kRecvCb1, c.r[4]);
    if (s_log) log("[lan] receive %s (%08X, %08X)\n", c.r[3] ? "on" : "off", c.r[3], c.r[4]);
    if (c.r[3]) startPump();
}

void ethGetLinkState(Ctx& c, u32) {
    // (the IP stack polls the link from boot on; it is "wanted" only while its receiver is on: the
    // game's LAN mode)
    if (rd32(kRecvCb0)) s_linkPolled.store(std::chrono::steady_clock::now().time_since_epoch().count());
    static const bool s_pollLog = std::getenv("SA2_LAN_POLL_LOG") != nullptr;  // (temporary: when the link is polled)
    if (s_pollLog) {
        static u64 s_n = 0;
        static auto s_t = std::chrono::steady_clock::now();
        s_n++;
        if (std::chrono::steady_clock::now() - s_t > std::chrono::seconds(1)) {
            log("[lan] link polled %llu times in the last second (receiver %s)\n", (unsigned long long)s_n, rd32(kRecvCb0) ? "on" : "off");
            s_n = 0, s_t = std::chrono::steady_clock::now();
        }
    }
    const Port* p = s_port.load();
    wr32(c.r[3], p && p->linkUp() ? 1 : 0);
    c.r[3] = 1;
}

void ethMulticast(Ctx&, u32) {}

// ---- the loopback transport: copies on this PC, over UDP on 127.0.0.1 ----
#ifdef _WIN32
using Sock = SOCKET;
constexpr Sock kNoSock = INVALID_SOCKET;
void closeSock(Sock s) { closesocket(s); }
#else
using Sock = int;
constexpr Sock kNoSock = -1;
void closeSock(Sock s) { close(s); }
#endif

struct Loopback : Port {
    Sock sock = kNoSock;
    std::vector<u16> peers;
    std::mutex sendLock;
    // SA2_LAN_DELAY_MS (tests): frames are held this long after they arrive - a network's latency.
    std::chrono::milliseconds delay{std::getenv("SA2_LAN_DELAY_MS") ? std::atoi(std::getenv("SA2_LAN_DELAY_MS")) : 0};
    // SA2_LAN_LOSS (tests): this many of every 1000 frames that arrive are dropped.
    int loss = std::getenv("SA2_LAN_LOSS") ? std::atoi(std::getenv("SA2_LAN_LOSS")) : 0;
    // SA2_LAN_JITTER_MS (tests): each frame's latency varies by up to this either way, in order (as a
    // TCP relay: a late frame holds up those after it). SA2_LAN_SPIKE=<per 1000>:<ms>: some frames are
    // that much later still.
    int jitter = std::getenv("SA2_LAN_JITTER_MS") ? std::atoi(std::getenv("SA2_LAN_JITTER_MS")) : 0;
    int spikeRate = 0, spikeMs = 0;
    std::chrono::steady_clock::time_point lastDue{};
    // Frames held back: when each is due (poll: one thread).
    std::deque<std::pair<std::chrono::steady_clock::time_point, std::vector<u8>>> delayed;

    Loopback(u16 port, std::vector<u16> peerPorts) : peers(std::move(peerPorts)) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == kNoSock) return;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(sock, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
            log("[lan] loopback: can't bind 127.0.0.1:%u\n", port);
            closeSock(sock);
            sock = kNoSock;
            return;
        }
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(sock, FIONBIO, &nb);
        BOOL report = FALSE;  // a peer not running yet: no "connection reset" on the next receive
        DWORD got = 0;
        WSAIoctl(sock, SIO_UDP_CONNRESET, &report, sizeof report, nullptr, 0, &got, nullptr, nullptr);
#else
        fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
#endif
        if (const char* sp = std::getenv("SA2_LAN_SPIKE")) {
            spikeRate = std::atoi(sp);
            if (const char* colon = std::strchr(sp, ':')) spikeMs = std::atoi(colon + 1);
        }
        std::string list;
        for (u16 p : peers) list += " " + std::to_string(p);
        log("[lan] loopback transport: 127.0.0.1:%u, peers at%s; latency %d ms, jitter %d ms, loss %d/1000, spikes %d/1000 of %d ms\n", port, list.c_str(), int(delay.count()), jitter, loss, spikeRate, spikeMs);
    }
    ~Loopback() override {
        if (sock != kNoSock) closeSock(sock);
    }
    void send(const uint8_t* frame, size_t len) override {
        if (sock == kNoSock) return;
        std::lock_guard g(sendLock);
        for (u16 p : peers) {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(p);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            sendto(sock, reinterpret_cast<const char*>(frame), int(len), 0, reinterpret_cast<sockaddr*>(&a), sizeof a);
        }
    }
    bool poll(std::vector<uint8_t>& frame) override {
        if (sock == kNoSock) return false;
        u8 buf[2048];
        const auto now = std::chrono::steady_clock::now();
        for (;;) {
            const int n = int(recvfrom(sock, reinterpret_cast<char*>(buf), sizeof buf, 0, nullptr, nullptr));
            if (n > 0) {
                if (loss > 0 && std::rand() % 1000 < loss) continue;  // (SA2_LAN_LOSS: a lossy network)
                auto due = now + delay;
                if (jitter > 0) due += std::chrono::milliseconds(std::rand() % (2 * jitter + 1) - jitter);
                if (spikeRate > 0 && std::rand() % 1000 < spikeRate) due += std::chrono::milliseconds(spikeMs);
                due = std::max(due, lastDue);  // (in order)
                lastDue = due;
                delayed.push_back({due, std::vector<u8>(buf, buf + n)});
                continue;
            }
#ifdef _WIN32
            if (n < 0 && WSAGetLastError() == WSAECONNRESET) continue;
#endif
            break;
        }
        if (delayed.empty() || now < delayed.front().first) return false;
        frame = std::move(delayed.front().second);
        delayed.pop_front();
        return true;
    }
    bool linkUp() const override { return sock != kNoSock; }
};

std::unique_ptr<Port> s_loopback;

void startLoopback(const char* spec) {
    constexpr u16 kBase = 47400;
    u16 mine = 0;
    std::vector<u16> peers;
    if (const char* colon = std::strchr(spec, ':')) {
        mine = u16(std::atoi(spec));
        for (const char* p = colon + 1; *p;) {
            peers.push_back(u16(std::atoi(p)));
            const char* comma = std::strchr(p, ',');
            if (!comma) break;
            p = comma + 1;
        }
    } else {
        const int n = std::atoi(spec);
        if (n < 1 || n > 4) return log("[lan] SA2_LAN_LOOPBACK=%s: expected 1-4 or <port>:<peer port>,...\n", spec);
        mine = u16(kBase + n);
        for (int k = 1; k <= 4; k++)
            if (k != n) peers.push_back(u16(kBase + k));
    }
    s_loopback = std::make_unique<Loopback>(mine, peers);
    setPort(s_loopback.get());
}

// This installation's MAC: locally administered, from the computer's name and the folder the game
// runs in (so copies in different folders differ, and a copy keeps its own).
void chooseMac() {
    if (const char* m = std::getenv("SA2_LAN_MAC")) {
        unsigned v[6];
        if (std::sscanf(m, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
            for (int i = 0; i < 6; i++) s_mac[i] = u8(v[i]);
            return;
        }
    }
    std::string id;
    if (const char* host = std::getenv("COMPUTERNAME")) id += host;
    else if (const char* h2 = std::getenv("HOSTNAME")) id += h2;
    std::error_code ec;
    id += "|" + std::filesystem::current_path(ec).string();
    u64 h = 1469598103934665603ull;  // FNV-1a
    for (unsigned char ch : id) h = (h ^ ch) * 1099511628211ull;
    for (int i = 0; i < 6; i++) s_mac[i] = u8(h >> (8 * i));
    s_mac[0] = u8((s_mac[0] & 0xFC) | 0x02);  // unicast, locally administered
}

void install() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    if (const char* e = std::getenv("SA2_LAN"); e && std::strcmp(e, "0") == 0) return;
    for (const auto& s : kSignature)
        if (rd32(s.addr) != s.word) return log("[lan] %08X is %08X, not the expected %08X: not this game's ETH library - no LAN\n", s.addr, rd32(s.addr), s.word);
    s_log = std::getenv("SA2_LAN_LOG") ? std::atoi(std::getenv("SA2_LAN_LOG")) : 0;
    s_trace = std::getenv("SA2_LAN_TRACE") ? std::atoi(std::getenv("SA2_LAN_TRACE")) : 0;
    chooseMac();
    bool ok = interpReplace(0, kETHInit, ethInit) && interpReplace(0, kETHSendAsync, ethSendAsync) &&
              interpReplace(0, kETHSetRecvCallback, ethSetRecvCallback) && interpReplace(0, kETHGetLinkState, ethGetLinkState) &&
              interpReplace(0, kETHAddMulticastAddress, ethMulticast) && interpReplace(0, kETHClearMulticastAddresses, ethMulticast);
    if (!ok) return log("[lan] the ETH library's functions are not where expected - no LAN\n");
    s_enabled = true;
    syncInstall();
    if (const char* lb = std::getenv("SA2_LAN_LOOPBACK")) startLoopback(lb);
    if (const char* dh = std::getenv("SA2_LAN_DHCP")) s_localDhcp = std::atoi(dh) != 0;
}
}  // namespace

void setPort(Port* p) { s_port.store(p); }
void sendRaw(const uint8_t* frame, size_t len) {
    Port* p = s_port.load();
    if (p && p->linkUp()) p->send(frame, len);
}
void setRawHandler(uint16_t etherType, RawHandler handler) {
    s_rawHandler.store(handler);
    s_rawType.store(etherType);
}
void ownMac(uint8_t out[6]) { std::memcpy(out, s_mac, 6); }
void setLocalDhcp(bool on) { s_localDhcp.store(on); }
void setLocalDhcpPlace(int place) { s_dhcpPlace.store(place); }
bool linkWanted() {
    const long long t = s_linkPolled.load();
    if (!t || !rd32(kRecvCb0)) return false;
    const auto age = std::chrono::steady_clock::now().time_since_epoch() - std::chrono::steady_clock::duration(t);
    return age < std::chrono::seconds(1);
}
Port* port() { return s_port.load(); }
void macAddress(uint8_t out[6]) {
    if (!s_mac[0] && !s_mac[1]) chooseMac();
    std::memcpy(out, s_mac, 6);
}

}  // namespace rt::lan

namespace rt {
// The EXI library starts at boot, before anything looks for the adapter: the driver is replaced then.
HLE(EXIInit) {
    callOriginal(c, "EXIInit");
    lan::install();
}

// EXIGetID(chan, dev, u32* id): the adapter (channel 0, device 2) answers its ID, as it would.
HLE(EXIGetID) {
    if (lan::s_enabled && c.r[3] == 0 && c.r[4] == 2) {
        wr32(c.r[5], lan::kAdapterId);
        c.r[3] = 1;
        return;
    }
    callOriginal(c, "EXIGetID");
}
}  // namespace rt

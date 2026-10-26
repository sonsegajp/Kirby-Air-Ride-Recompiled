// Kirby Air Ride's LAN lockstep, pipelined: the game plays over internet latency at 60 fps with a few
// frames of input delay (src/runtime/lan.cpp has the network driver underneath).
//
// How the game keeps its consoles in step (HAL's lbnet.c over its net library, both in main.dol):
//   The consoles form a ring in the order of their IP addresses (console 0, the "master", first). A
//   thread per console (the ring thread, 80487D78) passes one token round it: UDP port 26502, a type
//   byte (3) and 348 bytes - u32 lap, u16 length 0x154, then 0x154 bytes of data: u32 word0 (a value
//   the consoles agree on, the master's: the random seed and the like) and 4 slots of 0x54 bytes, one
//   per console: its 4 pads (PADStatus[4] and PADRead's result), flags (quitting, its queue of
//   pending input sets, agreement acks) and 25 bytes about it (disc id, ...).
//   Each console's turn is a call of lbnet's callback (8007B9CC, cb(in, out)): it puts together the
//   input sets of the round before - its own copy of the last token for the consoles before it in
//   the ring, the token in hand for itself and those after it - and queues them for the game
//   (HSD's network pad queue: the game's main loop runs one logic step per queued set, waiting at
//   each retrace for one); then it reads its own pads into its slot of the outgoing token. The
//   master's turn also waits for a retrace first. So each round, every console queues the same
//   input sets, in the same order: the game stays deterministic - but a round takes a whole trip of
//   the token round the ring, every hop a network trip: over the internet the game slows to a crawl
//   (one copy here, 100 ms round trip: 9 fps).
// Here, the ring thread is replaced (whole function, below) by a pipelined lockstep:
//   - every console takes one turn per retrace (all wait for it here; the master's callback, which
//     waits for one itself, is handed one): it calls the callback with a token made up so it queues
//     the input sets of the round due (the same data whatever its place in the ring: its saved copy
//     is overwritten too), and keeps its own slot from what it writes - its input for a new round;
//   - our rounds are made one per turn, always: never held up by the others, so a late round costs
//     only the console it is late for (pacing, below, has the rest);
//   - before a turn queues a set, the game has run all it queued before (it takes them at each
//     retrace): what a turn changes besides (the random seed the consoles agree on, applied at the
//     game's next logic step) then lands before the same step everywhere - as in the game's ring;
//   - the callback no longer wakes the game's main loop when it queues (it did, for a step at once,
//     a few ms after the retrace - and then none at the next): the set waits for the retrace, so the
//     game runs exactly one step per retrace and new frames come evenly;
//   - each console sends its slot of each round to all the others at once (raw Ethernet broadcast,
//     EtherType 0x88B5, through the same transport as the game's frames): each round in the next 4
//     frames it sends (redundancy: a lost or late frame costs nothing), acknowledged, resent if not
//     acknowledged within 1.5 round trips.
// The game's own ring traffic (port 26502) is gone; its menus and connection handshake are untouched.
// Every console must run this (it does not talk to the game's own ring). Leaving LAN works as the
// game's: the master's callback says when all are quitting, it tells the others (kEnd).
// SA2_LAN_SYNC=0: the game's own ring. SA2_LAN_INPUT_DELAY=<frames>: the starting delay (adapts from
// there). SA2_LAN_CHECK=<n> (tests): the game's random seed every n logic steps, to compare consoles.
// Log, every 10 s: [lansync] round, delay, turns, late (and ms), skipped, doubled, our slack p2/p50,
// the others' (median/delay), round trips p50/p95 per console.
#include "runtime/lan.h"

#include "runtime/interp.h"
#include "runtime/morepl.h"
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rt {
// The disc reads in a lockstep (src/runtime/hle_dvd.cpp).
void dvdGateBegin();
void dvdGateEnd();
}  // namespace rt

namespace rt::lan {

namespace {
// ---- the game's (main.dol, GKYE01) ----
constexpr u32 kRingThread = 0x80487D78;  // the net library's ring thread (replaced)
constexpr u32 kNet = 0x805C3C40;         // its state: +0x3C console addresses (u32 x4, ring order),
                                         // +0x4C count, +0x4D our place, +0x90 1: master,
                                         // +0x94 the callback, +0x108 1: stop
constexpr u32 kQuitCheck = 0x80488A28;   // ends the calling thread if the library is stopping
constexpr u32 kSetError = 0x80488978;    // records the library's error (10: lost a console)
constexpr u32 kWaitFrame = 0x8006293C;   // waits for a message on the game's frame queue n (1: a retrace)
constexpr u32 kFrameQueue1 = 0x80550CA8 + 0x24 + 0x34;  // its OSMessageQueue (a message each retrace)
constexpr u32 kNetQueueCount = 0x8058B080 + 0xB;  // (below)
constexpr u32 kRetraceCount = 0x805DD0E0 + 0xE8C;  // the VI library's count of retraces
constexpr u32 kSignalFrame = 0x80062900;  // f(n): a message on the game's frame queue n (0: wakes its main loop)  // HSD's network pad queue: sets not run yet
constexpr u32 kLb = 0x80552A30;          // lbnet's state: +0x28 calls of the callback, +0xA8 its copy
constexpr u32 kLbCalls = kLb + 0x28, kLbSaved = kLb + 0xA8;  // of the last token's data
constexpr u32 kLap = 0x805DD0E0 + 0x54C;  // lbnet's count of tokens (the callback asserts in[0] is it)
constexpr u32 kDataSize = 0x154, kSlotSize = 0x54, kTokenSize = 0x15C;
constexpr struct {
    u32 addr, word;
} kSignature[] = {
    {0x80487DDC, 0x81850094},  // ring thread: lwz r12, 0x94(r5) (the callback)
    {0x80487E6C, 0x480007CD},  // ring thread: bl receive
    {0x8007BA4C, 0x28000154},  // callback: cmplwi r0, 0x154 (the length it asserts)
    {0x8007BAA0, 0x4BFE6E9D},  // callback: bl wait for frame message (master)
    {0x8007C414, 0x483960A9},  // callback: bl queue the input sets
    {0x8007C540, 0x48395801},  // callback: bl read the pads
    {0x8007C860, 0x807F0028},  // callback: lwz r3, 0x28(r31) (calls)
};

// ---- the protocol ----
constexpr u16 kEtherType = 0x88B5;  // IEEE "local experimental"
constexpr u32 kMagic = 0x4B415273;  // "KARs"
constexpr u8 kVersion = 4;
constexpr int kMaxPeers = 8, kHistory = 512, kMaxEntries = 15, kRedundancy = 4;
// Our rounds made but not yet queued for our game, at most: a PC whose game takes no sets (a loading screen) stops
// making rounds there - the others wait for it - so no PC's rounds run more than ~2 x this past another's queue
// (kHistory: the rounds kept, past that they are overwritten).
constexpr int kMaxAhead = 90;
constexpr int kGameSlots = 4;  // the consoles the game's token has room for (PCs 4-7 ride on their spare ports)
constexpr int kHeader = 14 + 24 + kMaxPeers * 10 + 1, kEntry = 4 + 4 + kSlotSize;
enum : u8 { kData = 1, kEnd = 2 };
using Clock = std::chrono::steady_clock;
constexpr double kFrameMs = 1000.0 / 60.0;

struct Round {
    s32 round = -1;
    u32 word0 = 0;
    u8 slot[kSlotSize];
    Clock::time_point at{};      // the others' rounds: when it came; ours: when we first wanted to queue it
    Clock::time_point sentAt{};  // (our own rounds) when last sent, how often
    int sends = 0;
};
struct Peer {
    std::array<Round, kHistory> rounds;
    s32 contig = -1;     // highest round held with all before it
    s32 ackOfMine = -1;  // its contig of our rounds
    u32 echoT = 0;       // its last send time (its clock), and when we got it
    Clock::time_point echoAt{};
    bool heard = false;
    int samples = 0;
    double rtt = 0;                   // smoothed round trip, ms
    std::array<float, 256> rttLog{};  // the last round trips measured (ms)
    double slack = 0;                 // its slack (pacing, below) and input delay, as it last told us
    bool slackKnown = false;
    int delay = 0;
};

std::atomic<int> s_sessPcs{0}, s_sessMe{0};  // setSession (lansession.cpp): the PCs, this one's place
std::atomic<uint32_t> s_sessId{0};
std::mutex s_m;
std::condition_variable s_cv;
u32 s_checkEvery = 0, s_steps = 0;  // SA2_LAN_CHECK (below)
bool s_active = false, s_endSeen = false;
std::atomic<u32> s_lanSteps{0};  // LAN logic steps run (padRenew)
constexpr u32 kSceneLoop = 0x80006B58, kGmSceneOverFlag = 0x805359D8 + 0x7E0;
std::atomic<bool> s_loopReady{true};  // a scene's loop runs, past its start (sceneLoop, below): sets may be queued
std::atomic<u32> s_mainThread{0};  // the game's main loop (the OSThread running the steps)
std::atomic<bool> s_dvdArm{false};  // the ring started: its first logic step holds the disc reads from then on (hle_dvd.cpp)
std::atomic<bool> s_inSteps{false};  // the game's main loop is running logic steps (runSteps)
u32 s_session = 0;
int s_n = 0, s_me = 0;
u32 s_startAt = 0;     // when the consoles start making rounds (our clock, nowMs; 0: not known yet)
double s_mySlack = 0;  // ours, told to the others
bool s_mySlackKnown = false;
int s_myDelay = 0;
std::array<Peer, kMaxPeers> s_peers;
const auto s_epoch = Clock::now();
u32 nowMs() { return u32(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s_epoch).count()) | 1; }
double msBetween(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

void put32(u8* p, u32 v) { p[0] = u8(v >> 24), p[1] = u8(v >> 16), p[2] = u8(v >> 8), p[3] = u8(v); }
u32 get32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }

// A percentile of the last round trips to a console (ms).
double rttPercentile(const Peer& q, double pct) {
    const int n = std::min<int>(q.samples, int(q.rttLog.size()));
    if (n == 0) return q.rtt;
    std::vector<float> v(q.rttLog.begin(), q.rttLog.begin() + n);
    std::sort(v.begin(), v.end());
    return v[std::min<int>(n - 1, int(pct / 100.0 * n))];
}

// A frame of ours to every other console (under s_m).
void sendLocked(u8 kind) {
    u8 f[1514];
    std::memset(f, 0xFF, 6);
    ownMac(f + 6);
    f[12] = u8(kEtherType >> 8), f[13] = u8(kEtherType);
    u8* p = f + 14;
    put32(p, kMagic), p[4] = kVersion, p[5] = kind, p[6] = u8(s_me), p[7] = u8(s_n), put32(p + 8, s_session), put32(p + 12, nowMs());
    const int slack = s_mySlackKnown ? std::clamp(int(std::lround(s_mySlack * 10)), -32767, 32767) : -32768;  // (0.1 ms)
    p[16] = u8(slack >> 8), p[17] = u8(slack), p[18] = u8(s_myDelay), p[19] = 0;
    put32(p + 20, s_startAt);
    p += 24;
    const auto now = Clock::now();
    for (int j = 0; j < kMaxPeers; j++, p += 10) {
        const Peer& q = s_peers[j];
        put32(p, u32(q.contig));
        put32(p + 4, q.echoT);
        const auto age = q.echoT ? std::chrono::duration_cast<std::chrono::milliseconds>(now - q.echoAt).count() : 0;
        p[8] = u8(std::min<long long>(age, 0xFFFF) >> 8), p[9] = u8(std::min<long long>(age, 0xFFFF));
    }
    // Our rounds the slowest of the others has not got yet (oldest first): each in the next few frames
    // we send (one lost or late costs nothing), then again only if still not acknowledged a round
    // trip and a half later.
    Peer& mine = s_peers[s_me];
    s32 from = mine.contig + 1;
    double rtt = 0;
    for (int j = 0; j < s_n; j++)
        if (j != s_me) from = std::min(from, s_peers[j].ackOfMine + 1), rtt = std::max(rtt, s_peers[j].rtt);
    from = std::max(from, mine.contig - kHistory + 2);
    const auto resendAfter = std::chrono::milliseconds(std::max(30, int(rtt * 1.5)));
    u8* countAt = p++;
    int count = 0;
    for (s32 r = from; kind == kData && r <= mine.contig && count < kMaxEntries; r++) {
        Round& e = mine.rounds[r % kHistory];
        if (e.sends >= kRedundancy && now - e.sentAt < resendAfter) continue;
        e.sentAt = now, e.sends++;
        put32(p, u32(e.round)), put32(p + 4, e.word0);
        std::memcpy(p + 8, e.slot, kSlotSize);
        p += kEntry, count++;
    }
    *countAt = u8(count);
    sendRaw(f, size_t(p - f));
}

// A frame of ours from another console (the network thread).
void onFrame(const u8* f, size_t len) {
    if (len < size_t(kHeader)) return;
    const u8* p = f + 14;
    if (get32(p) != kMagic || p[4] != kVersion) return;
    const u8 kind = p[5], from = p[6];
    std::lock_guard g(s_m);
    if (!s_active || get32(p + 8) != s_session || from >= s_n || from == s_me || p[7] != s_n) return;
    Peer& q = s_peers[from];
    const auto now = Clock::now();
    q.heard = true;
    q.echoT = get32(p + 12), q.echoAt = now;
    const s16 slack = s16(u16(p[16] << 8 | p[17]));
    q.slackKnown = slack != -32768, q.slack = slack / 10.0, q.delay = p[18];
    if (from == 0 && get32(p + 20) && !s_startAt) {  // the master's start time, in its clock: in ours
        const double oneWay = (q.samples ? rttPercentile(q, 50) : 0) / 2;
        s_startAt = u32(s32(get32(p + 20)) - s32(get32(p + 12)) + s32(nowMs()) - s32(std::lround(oneWay))) | 1;
    }
    const u8* acks = p + 24;
    const u8* mine = acks + 10 * s_me;
    q.ackOfMine = std::max(q.ackOfMine, s32(get32(mine)));
    if (const u32 t = get32(mine + 4)) {
        const double rtt = double(s32(nowMs() - t) - s32(mine[8] << 8 | mine[9]));
        if (rtt >= 0 && rtt < 5000) {
            q.rtt = q.samples ? q.rtt * 0.875 + rtt * 0.125 : rtt;
            q.rttLog[size_t(q.samples) % q.rttLog.size()] = float(rtt);
            q.samples++;
        }
    }
    const u8* e = acks + 10 * kMaxPeers;
    const int count = *e++;
    if (len < size_t(e - f) + size_t(count) * kEntry) return;
    for (int k = 0; k < count; k++, e += kEntry) {
        const s32 r = s32(get32(e));
        if (r <= q.contig || r > q.contig + kHistory - 1) continue;
        Round& slot = q.rounds[r % kHistory];
        if (slot.round == r) continue;
        slot.round = r, slot.word0 = get32(e + 4), slot.at = now;
        std::memcpy(slot.slot, e + 8, kSlotSize);
    }
    while (q.rounds[(q.contig + 1) % kHistory].round == q.contig + 1) q.contig++;
    if (kind == kEnd && from == 0) s_endSeen = true;
    s_cv.notify_all();
}

// Waits (the guest CPU released, frames resent meanwhile) until `ready` (under s_m), the end, a stop
// request or `timeout`. Returns whether ready.
template <typename F>
bool waitFor(F ready, std::chrono::milliseconds timeout) {
    bool ok = false;
    sched::withCpuReleased([&] {
        std::unique_lock lk(s_m);
        const auto until = Clock::now() + timeout;
        while (!(ok = ready())) {
            if (s_endSeen || rd32(kNet + 0x108) == 1 || Clock::now() >= until) break;
            s_cv.wait_for(lk, std::chrono::milliseconds(15));
            sendLocked(kData);
        }
    });
    return ok;
}

u32 s_in = 0, s_out = 0;  // the tokens handed to the callback (guest memory)

// How long a round may be missing before its console counts as gone (5 s; SA2_LAN_LOST_MS: tests on one busy PC).
std::chrono::milliseconds lostAfter() {
    static const int s_ms = std::getenv("SA2_LAN_LOST_MS") ? std::max(1000, std::atoi(std::getenv("SA2_LAN_LOST_MS"))) : 5000;
    return std::chrono::milliseconds(s_ms);
}

void freshData(u8* d) {  // the data of a token the master starts: no pads, no flags
    std::memset(d, 0, kDataSize);
    for (int j = 0; j < kGameSlots; j++)
        for (int k = 0; k < 4; k++) d[4 + j * kSlotSize + 10 + 12 * k] = 0xFF;  // PADStatus.err: no controller
}

// ---- pacing ----
// D (frames) = our rounds made but not queued yet = input delay + 1. A console queues round q D turns
// after making its own round q; the others' round q must have come by then. Its slack is how long
// before that it came. Balanced, slack = D frames - the one-way trip. D starts from the measured
// round trips (95th percentile of the one-way trip + 8 ms), then, over windows of 120 turns (2 s):
//   - a round missing when wanted: nothing is queued this turn (the game has no new frame), the next
//     turn queues two (two steps in that frame) - this console only, the others are not held up;
//   - we are early - our median slack 1.5 frames under the others' (they tell us theirs): we skip turns
//     (make and queue nothing), half the difference, which moves a frame of slack from them to us;
//   - otherwise 2 or more late rounds in the window, or the 5th percentile of slack under 3 ms: D + 1
//     (a turn that queues nothing);
//   - no late round and the 5th percentile over D's worth (a frame + 11 ms) for three windows: D - 1
//     (a turn that queues two).
// Bounded to delays of 1..10 frames. D is ours alone: it changes when, not what, the game is given -
// every console still queues the same rounds in the same order, so a change needs no agreement.
constexpr double kSlackLow = 3.0, kSlackHigh = kSlackLow + kFrameMs + 8.0;
constexpr int kMinD = 2, kMaxD = 11, kWindow = 60;

int initialD() {
    if (const char* e = std::getenv("SA2_LAN_INPUT_DELAY")) return std::clamp(std::atoi(e) + 1, kMinD, kMaxD);
    double oneWay = 0;
    for (int j = 0; j < s_n; j++)
        if (j != s_me) oneWay = std::max(oneWay, rttPercentile(s_peers[j], 95) / 2);
    return std::clamp(int(std::ceil((oneWay + 8.0) / kFrameMs)), kMinD, kMaxD);
}

void ringThread(Ctx& c, u32) {
    const u32 cb = rd32(kNet + 0x94);
    // The game's consoles (at most 4) and this one's place among them; the PCs - a session of ours (setSession):
    // up to 8, those past 4 on spare ports of the game's consoles - and this one's place among those.
    const int consoles = std::clamp<int>(rd8(kNet + 0x4C), 1, kGameSlots), place = rd8(kNet + 0x4D) % kGameSlots;
    const bool ours = s_sessPcs.load() > 0;
    const int n = ours ? std::clamp(s_sessPcs.load(), 1, kMaxPeers) : consoles, me = ours ? s_sessMe.load() % kMaxPeers : place;
    const bool master = rd32(kNet + 0x90) == 1;  // the game's master (its callback's part: place 0)
    const bool first = me == 0;                  // ours (the start time, telling the end): PC 0
    const u32 sendMessage = guestSymbol("OSSendMessage"), receiveMessage = guestSymbol("OSReceiveMessage");
    if (!s_in) s_in = hleAlloc(kTokenSize + 4), s_out = hleAlloc(kTokenSize + 4);
    u32 session = 2166136261u;  // the session's id, or the consoles' addresses: the same on every one
    if (ours) {
        session = s_sessId.load();
    } else {
        for (u32 a = kNet + 0x3C; a < kNet + 0x3C + 4 * kGameSlots; a++) session = (session ^ rd8(a)) * 16777619u;
        session ^= u32(n);
    }
    {
        std::lock_guard g(s_m);
        s_active = true, s_endSeen = false, s_session = session, s_n = n, s_me = me, s_mySlackKnown = false, s_startAt = 0;
        s_dvdArm = true;
        for (Peer& q : s_peers) q = Peer{};
    }
    // Sync phase: everyone heard, round trips measured.
    const auto t0 = Clock::now();
    const bool heard = waitFor(
        [&] {
            for (int j = 0; j < n; j++)
                if (j != me && s_peers[j].samples < 24) return false;
            return true;
        },
        std::chrono::milliseconds(ours ? 20000 : 5000));  // (ours: everyone connects at once, but loads at its own pace)
    auto rttText = [&] {  // (under s_m)
        std::string t;
        char b[64];
        for (int j = 0; j < n; j++)
            if (j != me) std::snprintf(b, sizeof b, " %d/%d", int(rttPercentile(s_peers[j], 50)), int(rttPercentile(s_peers[j], 95))), t += b;
        return t;
    };
    int D;
    {
        std::lock_guard g(s_m);
        D = initialD();
        s_myDelay = D - 1;
        log("[lansync] PC %d of %d (the game's console %d of %d%s): round trips p50/p95 (ms)%s -> input delay %d frames (%s, %d ms)\n", me,
            n, place, consoles, master ? ", master" : "", rttText().c_str(), D - 1, heard ? "measured" : "not all heard",
            int(msBetween(t0, Clock::now())));
    }
    // All start making rounds at the same moment (the master picks it, a little ahead; the others
    // turn it into their clocks), so each console's rounds come to the others equally early.
    {
        std::lock_guard g(s_m);
        if (first) {
            double rtt = 0;
            for (int j = 0; j < n; j++)
                if (j != me) rtt = std::max(rtt, rttPercentile(s_peers[j], 95));
            s_startAt = (nowMs() + u32(std::max(150.0, 1.5 * rtt + 50))) | 1;
        }
    }
    waitFor([] { return s_startAt != 0 && s32(nowMs() - s_startAt) >= -20; }, std::chrono::milliseconds(ours ? 8000 : 3000));
    if (const s32 left = s_startAt ? s32(s_startAt - nowMs()) : 0; left > 0)
        sched::withCpuReleased([left] { std::this_thread::sleep_for(std::chrono::milliseconds(left)); });
    // The retrace messages that piled up while nobody waited for them: gone, so each turn waits for a
    // retrace of its own. (Every turn waits here, the master's too: its callback, which waits for one
    // as well, is handed one each time.)
    for (int k = 0; k < 100000 && receiveMessage && guestCall(c, receiveMessage, {kFrameQueue1, s_out + kTokenSize, 0}); k++) {}
    auto callCallback = [&](u32 in) {
        if (master && sendMessage) guestCall(c, sendMessage, {kFrameQueue1, 0, 0});
        return guestCall(c, cb, {in, s_out});
    };
    auto keepOwn = [&](s32 round) {  // our round `round`, as the callback just wrote it
        std::lock_guard g(s_m);
        Peer& mine = s_peers[me];
        Round& own = mine.rounds[round % kHistory];
        own.round = round, own.word0 = rd32(s_out + 6), own.sentAt = {}, own.sends = 0, own.at = {};
        std::memcpy(own.slot, hostp(s_out + 6 + 4 + place * kSlotSize), kSlotSize);  // (the game's slot for us)
        mine.contig = round;
    };
    // A turn that only makes our round (the callback is told it is its first turn: it queues nothing).
    s32 made = 0, queued = 0;  // our rounds made, rounds queued for the game
    auto makeOnly = [&] {
        u8 data[kDataSize];
        freshData(data);
        std::memcpy(hostp(s_in + 6), data, kDataSize);
        wr32(s_in, rd32(kLap));
        wr16(s_in + 4, kDataSize);
        wr32(kLbCalls, 0);
        callCallback(made == 0 && master ? 0 : s_in);  // (the master's very first turn has no token, as the game's)
        keepOwn(made++);
    };
    u32 retraceBase = rd32(kRetraceCount);  // (the next retrace is round 0's)
    int skipTurns = 0, cooldown = 0, highWindows = 0, windowSize = kWindow / 4;  // (the first one short)
    int latesInWindow = 0;
    struct Stats {
        u64 turns = 0, late = 0, skipped = 0, doubled = 0, caughtUp = 0;
        double lateMs = 0;
    } total, last;
    std::vector<double> window, logSlack;  // slack of the rounds queued: this window, since the last log
    auto lastLog = Clock::now();
    bool ended = false;
    while (!ended) {
        guestCall(c, kQuitCheck);       // (does not return when the library is stopping)
        guestCall(c, kWaitFrame, {1});  // a retrace
        {
            std::lock_guard g(s_m);
            if (s_endSeen) break;
        }
        if (skipTurns > 0) {  // (we are early: this turn makes and queues nothing)
            skipTurns--, total.skipped++, last.skipped++, retraceBase++;
            continue;
        }
        total.turns++, last.turns++;
        // One round made per retrace, whatever happens to this thread: a turn that missed retraces (the
        // host busy) makes the rounds it owes first, so the others never wait for us.
        for (int owed = std::min<int>(s32(rd32(kRetraceCount) - retraceBase) - made - 1, 3); owed > 0; owed--) {
            if (made >= queued + kMaxAhead) {  // (our game is not taking its sets - a loading screen: it owes none)
                retraceBase = rd32(kRetraceCount) - u32(made) - 1;
                break;
            }
            makeOnly();
            total.caughtUp++, last.caughtUp++;
            if (s_checkEvery) log("[lancheck] round %d made late (at step %u)\n", made - 1, s_steps);
        }
        const auto now = Clock::now();
        const int want = std::clamp(made - queued - D + 1, 0, 2);
        int done = 0;
        for (int k = 0; k < want && !ended; k++) {
            const s32 q = queued;
            bool have = true, lost = false;
            {
                std::lock_guard g(s_m);
                Round& mineQ = s_peers[me].rounds[q % kHistory];
                if (mineQ.at == Clock::time_point{}) mineQ.at = now;  // (first wanted now)
                Clock::time_point arrived{};
                for (int j = 0; j < n; j++) {
                    if (j == me) continue;
                    if (s_peers[j].contig < q) have = false;
                    else arrived = std::max(arrived, s_peers[j].rounds[q % kHistory].at);
                }
                // (lost: a console we have heard nothing at all from for that long - one loading a scene
                // sends no new rounds, the others wait for it, but it still answers)
                if (!have && now - mineQ.at > lostAfter())
                    for (int j = 0; j < n; j++)
                        if (j != me && s_peers[j].contig < q && (!s_peers[j].heard || now - s_peers[j].echoAt > lostAfter())) lost = true;
                if (have && k == 0) {  // its slack: how long before we first wanted it, it came
                    double slack = msBetween(arrived, mineQ.at);
                    if (arrived > mineQ.at) slack = -msBetween(mineQ.at, arrived);
                    window.push_back(slack), logSlack.push_back(slack);
                }
            }
            if (lost) {
                log("[lansync] round %d: a console stopped answering\n", q);
                guestCall(c, kSetError, {10});
                ended = true;
                break;
            }
            if (!have) {
                if (k == 0) {  // late: no set this frame; we wait a frame more from now on
                    total.late++, last.late++, latesInWindow++;
                    if (s_checkEvery) log("[lancheck] round %d late (at step %u)\n", q, s_steps);
                }
                break;
            }
            if (k > 0 && (rd32(kLb + 0x2C) == 1 || rd32(kLb + 0x38) == 1)) break;
            if (k == 0) {
                // The game has run every set queued before (at its own pace, on its main thread): what
                // this turn changes besides queueing (the random seed the consoles agreed on, applied at
                // the game's next logic step) then lands right before round q's step everywhere. A second
                // set right after it (two steps in the next frame) only while no agreement is under way
                // (+0x2C, +0x38 1: what the second could complete): the first's is then the last change.
                auto lastSent = Clock::now();
                while (rd8(kNetQueueCount) != 0 || s_inSteps || !s_loopReady || rd8(kGmSceneOverFlag) != 0) {
                    if (rd32(kNet + 0x108) == 1) guestCall(c, kQuitCheck);
                    sched::withCpuReleased([] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
                    if (Clock::now() - lastSent > std::chrono::milliseconds(15)) {  // (the others hear from us meanwhile)
                        std::lock_guard g(s_m);
                        sendLocked(kData);
                        lastSent = Clock::now();
                    }
                }
                // (SA2_LAN_CHECK: the game's seed here - every set before round q has run, on every PC alike)
                constexpr u32 kGmState = 0x805359D8;
                if (s_checkEvery && q % s_checkEvery < 2) {  // (and the scene, its frame count and countdown, the riders. places)
                    u32 h = 2166136261u;
                    for (u32 slot = 0; slot < morepl::count(morepl::k_ptab); slot++)
                        for (u32 o = 8; o < 0x14; o++) h = (h ^ rd8(morepl::at(morepl::k_ptab, slot) + o)) * 16777619u;
                    log("[lancheck] round %d seed %08X scene %02X frames %u count %d riders %08X\n", q, rd32(0x805DD0E0 - 0x3B0),
                        rd8(kGmState + 0x7D7), rd32(kGmState + 0x7E0 + 0x40), s32(rd32(kGmState + 0x7E0 + 0x44)), h);
                }
            }
            u8 data[kDataSize];
            freshData(data);
            {
                std::lock_guard g(s_m);
                const double waited = -std::min(0.0, k == 0 && !window.empty() ? window.back() : 0.0);
                total.lateMs += waited, last.lateMs += waited;
                put32(data, s_peers[0].rounds[q % kHistory].word0);
                for (int j = 0; j < std::min(n, kGameSlots); j++)
                    std::memcpy(data + 4 + j * kSlotSize, s_peers[j].rounds[q % kHistory].slot, kSlotSize);
                // PCs on spare ports: their controller (their pad 0) into their seat. (Outside a stage they send "no
                // controller" - lansession.cpp: in the menus a controller on another port of a console is another player
                // to wait for. Decided by the sender, it is in the data: the same for every PC.)
                for (int j = kGameSlots; j < n; j++) {
                    const Seat st = seatOf(j);
                    const u8* from = s_peers[j].rounds[q % kHistory].slot;
                    u8* to = data + 4 + st.console * kSlotSize;
                    std::memcpy(to + 12 * st.port, from, 12);  // its PADStatus (PADRead's mask, +0x30, stays the console's)
                }
            }
            std::memcpy(hostp(s_in + 6), data, kDataSize);
            wr32(s_in, rd32(kLap));
            wr16(s_in + 4, kDataSize);
            std::memcpy(hostp(kLbSaved), data, kDataSize);  // so all of it is round q, whatever our place
            if (s_checkEvery && q % s_checkEvery < 2) {  // (SA2_LAN_CHECK: what the game is handed for round q - the same on every PC)
                u32 h = 2166136261u;
                for (u32 o = 0; o < kDataSize; o++) h = (h ^ rd8(s_in + 6 + o)) * 16777619u;
                log("[lancheck] round %d data %08X\n", q, h);
            }
            const u32 result = callCallback(s_in);
            queued++, done++;
            if (k == 0) keepOwn(made++);  // (a second call's round is not kept: made again next turn)
            if (master && result == 1) {  // every console is leaving: tell them (a few times), as the game's ring does
                for (int t = 0; t < 3; t++) {
                    {
                        std::lock_guard g(s_m);
                        sendLocked(kEnd);
                    }
                    waitFor([] { return false; }, std::chrono::milliseconds(20));
                }
                ended = true;
                break;
            }
        }
        if (ended) break;
        if (done == 0) {  // nothing queued: still make our round, on time (not past kMaxAhead: below)
            if (made < queued + kMaxAhead) makeOnly();
            else retraceBase++;
        } else if (done == 2) {
            total.doubled++, last.doubled++;
            if (s_checkEvery) log("[lancheck] rounds %d-%d queued together (at step %u, queue %u)\n", queued - 2, queued - 1, s_steps, rd8(kNetQueueCount));
        }
        // Pacing, every window of turns that queued.
        if (window.size() >= size_t(windowSize)) {
            std::sort(window.begin(), window.end());
            const double low = window[window.size() * 5 / 100], mid = window[window.size() / 2];
            double others = 0;
            int known = 0;
            {
                std::lock_guard g(s_m);
                s_mySlack = mid, s_mySlackKnown = true;
                for (int j = 0; j < n; j++)
                    if (j != me && s_peers[j].slackKnown) others += s_peers[j].slack, known++;
            }
            if (known) others /= known;
            if (cooldown > 0) {
                cooldown--;
            } else if (known && others - mid > 1.5 * kFrameMs && low < kSlackHigh) {
                // We are early (the others' rounds come late to us, ours early to them): skip turns, half
                // the difference.
                skipTurns = std::clamp(int(std::lround((others - mid) / 2 / kFrameMs)), 1, 8);
                highWindows = 0, cooldown = 2;  // (until the others have told us how it went)
            } else if (latesInWindow >= 2 || low < kSlackLow) {
                D = std::min(D + 1, kMaxD);
                highWindows = 0;
            } else if (latesInWindow == 0 && low > kSlackHigh) {
                if (++highWindows >= 3 && D > kMinD) D--, highWindows = 0, cooldown = 1;
            } else {
                highWindows = 0;
            }
            window.clear(), latesInWindow = 0;
            windowSize = kWindow;
        }
        {
            std::lock_guard g(s_m);
            s_myDelay = D - 1;
            sendLocked(kData);
            if (Clock::now() - lastLog > std::chrono::seconds(10)) {
                lastLog = Clock::now();
                std::sort(logSlack.begin(), logSlack.end());
                auto pct = [&](double p) { return logSlack.empty() ? 0.0 : logSlack[std::min(logSlack.size() - 1, size_t(p / 100 * logSlack.size()))]; };
                std::string others;
                char b[48];
                for (int j = 0; j < n; j++)
                    if (j != me) std::snprintf(b, sizeof b, " %.0f/d%d", s_peers[j].slack, s_peers[j].delay), others += b;
                log("[lansync] round %d, delay %d: last %llu turns %llu late (%.0f ms), %llu skipped, %llu doubled, %llu made late; slack p2/p50 %.0f/%.0f ms "
                    "(others%s); round trips p50/p95%s\n",
                    queued, D - 1, (unsigned long long)last.turns, (unsigned long long)last.late, last.lateMs, (unsigned long long)last.skipped,
                    (unsigned long long)last.doubled, (unsigned long long)last.caughtUp, pct(2), pct(50), others.c_str(), rttText().c_str());
                last = Stats{}, logSlack.clear();
            }
        }
    }
    {
        std::lock_guard g(s_m);
        s_active = false;
    }
    log("[lansync] ring ended: %llu turns, %llu late (%.0f ms), %llu skipped, %llu doubled\n", (unsigned long long)total.turns,
        (unsigned long long)total.late, total.lateMs, (unsigned long long)total.skipped, (unsigned long long)total.doubled);
    s_dvdArm = false;
    dvdGateEnd();
    c.r[3] = 0;
}
// SA2_LAN_CHECK=<n> (tests): every n logic steps of a LAN game, the step count and the game's random
// seed, to compare between consoles (the same on all of them while they are in step).
constexpr u32 kPadRenew = 0x80062978;  // a logic step's pad update (in LAN games: from the queued sets)
void padRenew(Ctx& c, u32 entry) {
    const u32 from = c.lr;
    interpOriginal(c, entry);
    // (logic steps only - the game's step loop, 8000682C - and the random seed they use)
    if (!rd32(kLb + 0x1C) || from != 0x80006830) return;
    s_lanSteps++;
    s_mainThread = rd32(sched::kCurrentThread);
    if (s_dvdArm.exchange(false)) dvdGateBegin();
    if (s_checkEvery && ++s_steps % s_checkEvery == 0) log("[lancheck] step %u seed %08X\n", s_steps, rd32(0x805DD0E0 - 0x3B0));
}
// The callback wakes the game's main loop when it has queued a set (8007C5EC, f_80062900(0)): the
// loop then runs that step at once, a few ms after the retrace - and finds nothing at the next
// retrace, so new frames come unevenly (the display, interpolating, starved half its refreshes).
// Here sets are queued a frame ahead (right after the game took the last one), so the loop is left
// to wake at the retrace and finds its set there: a step at every retrace.
constexpr u32 kSignalFromCallback = 0x8007C5F0;
// The game's frame of logic steps (f(count): applies what the consoles agreed on, then runs count
// steps, then draws): a set is queued only between two of them - not while one is running (this
// thread can take the CPU from the main loop mid-step), so what the turn changes lands before the
// next one, as in the game's ring.
constexpr u32 kRunSteps = 0x800067A4;
// A scene's loop (80006B58) begins - once its files are in - by emptying the network pad queue and starting
// the consoles' agreement for it (8007CBB0, lbnet +0x2C). A set queued before that, while the scene loaded, was
// thrown away; one queued after it went to the agreement: which of the two a round was depended on how long
// this PC took to load - the PCs parted at scene changes (the machine stats above all: the Stadium preloads
// there). So a set is queued only while a scene's loop runs, past its start (s_loopReady), with no scene end
// pending (gm +0x7E0): every round lands in the same place on every PC.
void sceneLoop(Ctx& c, u32 entry) {
    s_loopReady = false;
    interpOriginal(c, entry);
    s_loopReady = false;
}
void loopWaitFrame(Ctx& c, u32 entry) {  // (the loop's wait for its next frame, 80006B94: past its start)
    if (c.lr == 0x80006B98) s_loopReady = true;
    interpOriginal(c, entry);
}
void runSteps(Ctx& c, u32 entry) {
    s_inSteps = true;
    interpOriginal(c, entry);
    s_inSteps = false;
}
void signalFrame(Ctx& c, u32 entry) {
    if (c.lr == kSignalFromCallback && s_active) return;
    interpOriginal(c, entry);
}
// The net library's connect (804876A0, its connect thread): it finds the other consoles (SSDP over the LAN),
// sorts their addresses into the ring's order and agrees on that with them - 4 at most, and about 14 s. In a
// session of ours (setSession) every PC knows the order already: it is filled in at once - the consoles, this
// one's place, its neighbours in the ring, an address each - with no traffic; the game goes on as after its own
// (the ring thread: the lockstep above).
constexpr u32 kConnect = 0x804876A0;
void connect(Ctx& c, u32 entry) {
    const int pcs = s_sessPcs.load();
    if (pcs <= 0) return interpOriginal(c, entry);
    const int me = s_sessMe.load(), consoles = std::min(pcs, kGameSlots), place = seatOf(me).console;
    for (int k = 0; k < kGameSlots; k++) wr32(kNet + 0x3C + 4 * u32(k), k < consoles ? 0xA9FEC80Au + u32(k) : 0u);  // (169.254.200.10 + k)
    wr8(kNet + 0x4C, u32(consoles));
    wr8(kNet + 0x4D, u32(place));
    wr8(kNet + 0x4E, u32(place == 0 ? consoles - 1 : place - 1));  // (the ring's previous console)
    wr8(kNet + 0x4F, u32(place == consoles - 1 ? 0 : place + 1));  // (and next)
    log("[lansync] connected at once: PC %d of %d, the game's console %d of %d\n", me, pcs, place, consoles);
    c.r[3] = 0;
}
}  // namespace

void setSession(int pcs, int me, uint32_t id) {
    s_sessPcs = std::clamp(pcs, 0, kMaxPeers);
    s_sessMe = std::clamp(me, 0, kMaxPeers - 1);
    s_sessId = id;
}
bool inSteps() { return s_inSteps.load(); }
uint32_t lanSteps() { return s_lanSteps.load(); }
uint32_t mainThread() { return s_mainThread.load(); }
int sessionPcs() { return s_sessPcs.load(); }
int sessionMe() { return s_sessMe.load(); }

void syncInstall() {
    if (const char* e = std::getenv("SA2_LAN_CHECK")) s_checkEvery = std::max(1, std::atoi(e));
    interpReplace(0, kPadRenew, padRenew);
    if (const char* e = std::getenv("SA2_LAN_SYNC"); e && std::strcmp(e, "0") == 0) return;
    for (const auto& s : kSignature)
        if (rd32(s.addr) != s.word) return log("[lansync] %08X is %08X, not %08X: the game's own ring stays\n", s.addr, rd32(s.addr), s.word);
    setRawHandler(kEtherType, onFrame);
    if (rd32(0x8007C5EC) == 0x4BFE6315) interpReplace(0, kSignalFrame, signalFrame);  // (bl f_80062900)
    if (rd32(0x800067E4) == 0x4807644D) interpReplace(0, kRunSteps, runSteps);         // (bl f_8007CC30)
    if (rd32(kSceneLoop) == 0x9421FFF0 && rd32(0x80006B94) == 0x4805BDA9)  // (bl f_8006293C)
        interpReplace(0, kSceneLoop, sceneLoop), interpReplace(0, kWaitFrame, loopWaitFrame);
    if (!interpReplace(0, kRingThread, ringThread)) log("[lansync] the ring thread is not where expected: the game's own ring stays\n");
    if (rd32(kConnect) == 0x9421FC90) interpReplace(0, kConnect, connect);  // (stwu r1, -0x370(r1))
}

}  // namespace rt::lan

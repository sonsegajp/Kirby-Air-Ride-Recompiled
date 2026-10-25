// DVD: the disc's file system table sits in guest memory the way the IPL leaves it, so the SDK's
// own file-system code (DVDOpen, DVDConvertPathToEntrynum, DVDChangeDir...) runs natively. Only the
// reads are replaced: a worker thread serves them from the user's disc image and completes each one
// through the interrupt path (callbacks run in interrupt context, as on the console).
#include "disc/discimage.h"
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <string>
#include <cstring>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace rt {
u64 gxFrames();  // src/runtime/gx.cpp

namespace {
std::unique_ptr<disc::Image> s_disc;  // .iso/.gcm, .ciso or .rvz (src/disc)
std::vector<u8> s_fstCopy;  // the FST as read from the disc, for SA2_DVD_LOG checks
std::mutex s_discLock;

// DVDCommandBlock (2001 SDK)
enum : u32 {
    CB_COMMAND = 0x08,
    CB_STATE = 0x0C,
    CB_OFFSET = 0x10,
    CB_LENGTH = 0x14,
    CB_ADDR = 0x18,
    CB_CURR = 0x1C,
    CB_TRANSFERRED = 0x20,
    CB_ID = 0x24,
    CB_CALLBACK = 0x28,
};
enum : u32 { ST_END = 0, ST_BUSY = 1, ST_CANCELED = 10 };

struct Request {
    u32 block, addr, length, callback;
    u64 offset;
    u64 seq;
};
// The latest request issued on each command block: a completion for an older one (the game
// cancelled it and reused the block) must not finish the new request with the old data.
std::mutex s_seqLock;
std::unordered_map<u32, u64> s_blockSeq;
u64 s_seq = 0;
std::atomic<u32> s_inFlight{0};
std::mutex s_qLock;
std::condition_variable s_qCv;
std::deque<Request> s_queue;
bool s_workerStarted = false;

bool readRaw(u64 offset, void* dst, u32 len) {
    std::lock_guard g(s_discLock);
    return s_disc && s_disc->read(offset, dst, len);
}

void complete(const Request& r, s32 result) {
    s_inFlight--;
    sched::postIrq([r, result](Ctx& c) {
        {
            std::lock_guard g(s_seqLock);
            if (s_blockSeq[r.block] != r.seq) return;  // stale: cancelled or superseded
        }
        if (rd32(r.block + CB_STATE) == ST_CANCELED) return;
        wr32(r.block + CB_STATE, result >= 0 ? ST_END : u32(-1));
        wr32(r.block + CB_TRANSFERRED, result >= 0 ? u32(result) : 0);
        if (r.callback) {
            c.r[3] = u32(result);
            c.r[4] = r.block;
            c.call(r.callback);
        }
    });
}

// ---- the drive's timing ----
// A GameCube disc spins at a constant 28.5 turns a second; data sits on a spiral from 24 mm out
// to 38 mm at constant density, so a read streams at 2.1 MiB/s at the hub up to 3.3 MiB/s at the
// rim. The drive reads whole 32 KiB ECC blocks; moving the head costs a seek (35 ms plus head
// travel within a millimetre, 75 ms plus travel beyond) and then the wait for the block to come
// round. While idle it keeps reading ahead up to 1 MiB past the last block it was asked for, and
// what it already holds goes to memory at 32 MiB/s. (The figures are the hardware measurements
// Dolphin's drive model uses, the reference this runtime is checked against.)
const auto s_clock0 = std::chrono::steady_clock::now();
double hostSeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - s_clock0).count(); }
bool fastDisc() {  // (asked once the command line has been read)
    static const bool fast = std::getenv("SA2_FAST_DISC") != nullptr;
    return fast;
}

struct Drive {
    static constexpr u64 kBlock = 32 * 1024, kAhead = 1024 * 1024;
    static constexpr double kInner = 0.024, kOuter = 0.038;
    double busyUntil = 0;
    // Read-ahead: blocks [aheadFrom, aheadTo) arrive between aheadT0 and aheadT1 (none if equal).
    u64 aheadFrom = 0, aheadTo = 0;
    double aheadT0 = 0, aheadT1 = 0;

    static double radius(u64 off) {  // where an offset lies: area grows with r^2 (single-layer DVD density)
        constexpr double layer = 0x118240000, rimOfDvd = 0.058;
        return std::sqrt(double(off) / layer * (rimOfDvd * rimOfDvd - kInner * kInner) + kInner * kInner);
    }
    static double streamTime(u64 off, u64 len) {
        const double r = std::min(radius(off + len / 2), kOuter);
        const double speed = (2.1 + (r - kInner) / (kOuter - kInner) * (3.325 - 2.1)) * 1024 * 1024;
        return double(len) / speed;
    }
    static double seekTime(u64 from, u64 to) {
        const double d = std::fabs(radius(from) - radius(to));
        return d < 0.001 ? 0.035 + d * 50 : 0.075 + d * 4.5;
    }
    static double turnWait(u64 off, double t) {  // until the block's angle comes under the head
        const double target = std::fmod(radius(off) / 0.00000074, 1.0), now = std::fmod(t * 28.5, 1.0);
        return std::fmod(target + 1.0 - now, 1.0) / 28.5;
    }

    // When a read of [off, off+len) issued at time t completes.
    double read(u64 off, u64 len, double t) {
        // What the read-ahead holds by now, and where the head is.
        u64 heldFrom = 0, heldTo = 0, head = 0;
        if (aheadT1 > aheadT0) {
            heldFrom = aheadTo > kAhead ? aheadTo - kAhead : 0;
            heldTo = t >= aheadT1 ? aheadTo
                                  : aheadFrom + u64((t - aheadT0) / (aheadT1 - aheadT0) * double(aheadTo - aheadFrom)) / kBlock * kBlock;
            head = heldTo;
        }
        u64 block = off / kBlock * kBlock;
        if (block < heldFrom) heldFrom = heldTo = 0;  // reading behind the buffer drops it
        const u64 first = block;
        double done = t + 0.0006;  // the command itself
        bool sought = false;
        for (u64 pos = off, left = len; left;) {
            const u64 chunk = std::min<u64>(left, (pos / kBlock + 1) * kBlock - pos);
            if (block >= heldFrom && block < heldTo) {
                done += double(chunk) / (32.0 * 1024 * 1024);
            } else {
                if (block != head) {
                    sought = true;
                    done += seekTime(head, block);
                    done += turnWait(block, done);
                } else {
                    done += streamTime(block, kBlock);
                }
                head = block + kBlock;
            }
            pos += chunk, left -= chunk, block += kBlock;
        }
        // The drive goes on reading ahead from the last block (keeping it and the one before).
        const u64 last = block - kBlock;
        if (!(last - heldFrom <= 2 * kBlock && heldFrom != heldTo)) {
            aheadFrom = last >= heldTo ? last : heldTo;
            aheadTo = last + kAhead - 2 * kBlock;
            if (sought) aheadTo = std::max(aheadTo, first + kAhead);
            aheadT0 = done;
            aheadT1 = done + streamTime(aheadFrom, aheadTo - aheadFrom);
        }
        return done;
    }
} s_drive;

void worker() {
    profileRegisterThread();
    log("[dvd] drive timing: %s\n", fastDisc() ? "fast (--fast-disc)" : "as the console's drive");
    std::vector<u8> buf;
    for (;;) {
        Request r;
        {
            std::unique_lock lk(s_qLock);
            s_qCv.wait(lk, [] { return !s_queue.empty(); });
            r = s_queue.front();
            s_queue.pop_front();
        }
        // A cancelled request never reaches memory: the game frees its buffer once DVDCancel returns
        // and reuses it (Kirby Air Ride cancels its loads at scene changes; the late data overwrote
        // the next scene's objects). The check and the copy hold s_seqLock, so a cancel waits for a
        // copy under way, as the console's waits for the transfer to stop.
        auto cancelled = [&r] { return s_blockSeq[r.block] != r.seq; };
        {
            std::lock_guard g(s_seqLock);
            if (cancelled()) {
                s_inFlight--;
                continue;
            }
        }
        buf.resize(r.length);
        const bool ok = readRaw(r.offset, buf.data(), r.length);
        // A read takes as long as on the console's drive: the game's streams (music, the movie
        // layers of cutscenes) start when their data arrives, and cutscenes are timed for that.
        // --fast-disc (SA2_FAST_DISC): nearly instant instead.
        const double start = std::max(hostSeconds(), s_drive.busyUntil);
        const double done = fastDisc() ? start + 0.0005 + r.length / 16e6 : s_drive.read(r.offset, r.length, start);
        s_drive.busyUntil = done;
        sleepUntil(s_clock0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(done)), false);
        {
            std::lock_guard g(s_seqLock);
            if (cancelled()) {
                s_inFlight--;
                continue;
            }
            if (ok) std::memcpy(hostp(r.addr), buf.data(), r.length);
        }
        complete(r, ok ? s32(r.length) : -1);
    }
}

// File containing a disc offset (from the FST in guest memory), for SA2_DVD_LOG.
std::string fileAt(u64 offset) {
    const u32 fst = rd32(0x80000038);
    if (!fst) return "?";
    const u32 n = rd32(fst + 8);
    const u32 strtab = fst + n * 12;
    // Walk the tree keeping the directory path of each entry.
    std::vector<std::pair<u32, std::string>> dirs{{n, ""}};
    for (u32 i = 1; i < n; i++) {
        while (dirs.size() > 1 && i >= dirs.back().first) dirs.pop_back();
        const u32 w0 = rd32(fst + i * 12), w1 = rd32(fst + i * 12 + 4), w2 = rd32(fst + i * 12 + 8);
        const std::string name = gstr(strtab + (w0 & 0xFFFFFF));
        if (w0 >> 24) dirs.push_back({w2, dirs.back().second + name + "/"});
        else if (offset >= w1 && offset < u64(w1) + w2) return dirs.back().second + name + "+" + std::to_string(offset - w1);
    }
    return "?";
}

void submit(u32 block, u32 addr, u32 length, u64 offset, u32 callback, u32 command) {
    static const bool s_log = std::getenv("SA2_DVD_LOG") != nullptr;
    if (s_log) log("[dvd] frame %llu: read %s (%u bytes) -> %08X\n", (unsigned long long)gxFrames(), fileAt(offset).c_str(), length, addr);
    wr32(block + CB_COMMAND, command);
    wr32(block + CB_STATE, ST_BUSY);
    wr32(block + CB_ADDR, addr);
    wr32(block + CB_LENGTH, length);
    wr32(block + CB_OFFSET, u32(offset));
    wr32(block + CB_TRANSFERRED, 0);
    wr32(block + CB_CALLBACK, callback);
    u64 seq;
    {
        std::lock_guard g(s_seqLock);
        seq = s_blockSeq[block] = ++s_seq;
    }
    s_inFlight++;
    std::lock_guard g(s_qLock);
    if (!s_workerStarted) {
        s_workerStarted = true;
        std::thread(worker).detach();
    }
    s_queue.push_back({block, addr, length, callback, offset, seq});
    s_qCv.notify_one();
}
}  // namespace

// For stall reports: requests issued but not yet completed, and queued ones.
std::string dvdPendingSummary() {
    std::lock_guard g(s_qLock);
    std::string out = "in flight " + std::to_string(s_inFlight.load()) + ", queued " + std::to_string(s_queue.size());
    for (const Request& r : s_queue) out += "; " + fileAt(r.offset);
    return out;
}

// A whole file of the disc, by its path ("MnAll.dat"; any case), from the FST read at boot: the port's
// own menus take the game's menu art from it at run time (src/runtime/gameart.cpp).
bool dvdReadFile(const std::string& path, std::vector<u8>& out) {
    const std::vector<u8>& f = s_fstCopy;
    auto be = [&](size_t o) { return u32(f[o]) << 24 | u32(f[o + 1]) << 16 | u32(f[o + 2]) << 8 | f[o + 3]; };
    if (f.size() < 12) return false;
    const u32 n = be(8);
    const size_t strtab = size_t(n) * 12;
    if (strtab > f.size()) return false;
    auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower(u8(c)));
        return s;
    };
    const std::string want = lower(path);
    std::vector<std::pair<u32, std::string>> dirs{{n, ""}};
    for (u32 i = 1; i < n; i++) {
        while (dirs.size() > 1 && i >= dirs.back().first) dirs.pop_back();
        const u32 w0 = be(i * 12), w1 = be(i * 12 + 4), w2 = be(i * 12 + 8);
        std::string name;
        for (size_t k = strtab + (w0 & 0xFFFFFF); k < f.size() && f[k]; k++) name += char(f[k]);
        if (w0 >> 24) dirs.push_back({w2, dirs.back().second + name + "/"});
        else if (lower(dirs.back().second + name) == want) {
            out.resize(w2);
            return readRaw(w1, out.data(), w2);
        }
    }
    return false;
}

void dvdOpenDisc(const std::string& path) {
    std::string err;
    s_disc = disc::open(std::filesystem::path(path), err);
    if (!s_disc) fatal("can't open disc image %s: %s", path.c_str(), err.c_str());
    // The apploader loads the FST to the top of MEM1 (room for the header's maximum FST size),
    // records it in the boot block and puts arena hi just below it. The game relies on this: it
    // hardcodes buffers (movie work area, module load address) up to a few hundred KB below.
    u8 h[12];
    readRaw(0x424, h, 12);
    auto be = [&](int o) { return u32(h[o]) << 24 | h[o + 1] << 16 | h[o + 2] << 8 | h[o + 3]; };
    const u32 fstOff = be(0), fstSize = be(4), fstMax = std::max(be(8), fstSize);
    const u32 fst = (0x80000000 + kMem1Size - fstMax) & ~31u;
    readRaw(fstOff, hostp(fst), fstSize);
    s_fstCopy.assign(static_cast<const u8*>(hostp(fst)), static_cast<const u8*>(hostp(fst)) + fstSize);
    wr32(0x80000038, fst);
    wr32(0x8000003C, fstSize);
    wr32(0x80000034, fst);  // arena hi
    log("[dvd] disc %s (%s), FST %u bytes at %08X\n", path.c_str(), s_disc->format(), fstSize, fst);
}

// Path lookups and directory changes stay native; with SA2_DVD_LOG they are logged.
HLE(DVDConvertPathToEntrynum) {
    static const bool s_log = std::getenv("SA2_DVD_LOG") != nullptr;
    const std::string path = gstr(c.r[3]);
    callOriginal(c, "DVDConvertPathToEntrynum");
    if (s_log) log("[dvd] path '%s' -> entry %d\n", path.c_str(), s32(c.r[3]));
    if (s_log && s32(c.r[3]) < 0) {
        // Is the FST in guest memory still the one read from the disc?
        const u32 fst = rd32(0x80000038);
        const u8* g = static_cast<const u8*>(hostp(fst));
        u32 diffs = 0, first = 0;
        for (u32 i = 0; i < s_fstCopy.size(); i++)
            if (g[i] != s_fstCopy[i] && diffs++ == 0) first = i;
        log("[dvd] FST at %08X: %u bytes differ from the disc's", fst, diffs);
        if (diffs) log(" (first at +%X = %08X)", first, fst + first);
        log("; current dir %d\n", s32(rd32(c.r[13] - 0x7568)));
    }
}
HLE(DVDChangeDir) {
    static const bool s_log = std::getenv("SA2_DVD_LOG") != nullptr;
    const std::string path = gstr(c.r[3]);
    callOriginal(c, "DVDChangeDir");
    if (s_log) log("[dvd] chdir '%s' -> %d\n", path.c_str(), s32(c.r[3]));
}

HLE(__DVDInitWA) {}
HLE(DVDReset) {}
HLE(DVDCheckDisk) { c.r[3] = 1; }
HLE(DVDGetDriveStatus) { c.r[3] = 0; }  // idle
HLE(DVDReadAbsAsyncPrio) {  // (block, addr, length, offset, callback, prio)
    submit(c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], 1);
    c.r[3] = 1;
}
HLE(DVDReadAbsAsyncForBS) {  // (block, addr, length, offset, callback)
    submit(c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], 4);
    c.r[3] = 1;
}
HLE(DVDReadDiskID) {  // (block, diskID, callback)
    submit(c.r[3], c.r[4], 0x20, 0, c.r[5], 5);
    c.r[3] = 1;
}
void invalidate(u32 block) {
    std::lock_guard g(s_seqLock);
    s_blockSeq[block] = ++s_seq;
}

// Cancelling, as the SDK's DVDCancelAsync: a request still in progress ends - the block becomes
// CANCELED and its own callback gets DVD_RESULT_CANCELED (-3), which games wait for (Kirby Air
// Ride's loader calls DVDCancel until its read callback has seen a failed result) - then the
// canceller's callback gets 0. A block with nothing in progress keeps its state.
constexpr u32 kResultCanceled = u32(-3);
u32 cancelBlock(u32 block) {  // the block's callback to tell, 0: nothing was in progress
    invalidate(block);
    if (rd32(block + CB_STATE) != ST_BUSY) return 0;
    wr32(block + CB_STATE, ST_CANCELED);
    return rd32(block + CB_CALLBACK);
}

HLE(DVDCancelAsync) {  // (block, callback)
    const u32 block = c.r[3], cb = c.r[4];
    const u32 blockCb = cancelBlock(block);
    if (blockCb || cb) sched::postIrq([block, cb, blockCb](Ctx& ic) {
        if (blockCb) guestCall(ic, blockCb, {kResultCanceled, block});
        if (cb) guestCall(ic, cb, {0, block});
    });
    c.r[3] = 1;
}
HLE(DVDCancel) {  // (block): returns once cancelled
    const u32 block = c.r[3];
    if (const u32 blockCb = cancelBlock(block)) guestCall(c, blockCb, {kResultCanceled, block});
    c.r[3] = 0;
}

}  // namespace rt

// DVD: the disc's file system table sits in guest memory the way the IPL leaves it, so the SDK's
// own file-system code (DVDOpen, DVDConvertPathToEntrynum, DVDChangeDir...) runs natively. Only the
// reads are replaced: a worker thread serves them from the user's disc image and completes each one
// through the interrupt path (callbacks run in interrupt context, as on the console).
#include "disc/discimage.h"
#include "runtime/lan.h"
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
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace rt {
u64 gxFrames();  // src/runtime/gx.cpp
extern std::atomic<bool> g_customCity;  // src/runtime/maps.cpp
std::atomic<u32> g_dvdGatePending{0};  // a held read issued and not yet ended for the game (below)

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
    u64 gseq = 0;  // held in a lockstep (below): its number there (1, 2, ...), of that lockstep (epoch)
    u32 epoch = 0;
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
bool dvdLog() {
    static const bool on = std::getenv("SA2_DVD_LOG") != nullptr;
    return on;
}

// ---- in a lockstep (a LAN or online session of several PCs: src/runtime/lansync.cpp) ----
// A read ends when the drive (each PC's own disc image, at host speed) has it, so on each PC the game would
// see it end in a different logic step - and what it does once a file is in (the machine stats screen's
// countdown, the Stadium's start) would run at different rounds: the PCs part. So in a lockstep a read's end
// (its callback, the command block's state) is held, and told the game at a point of its own run that is the
// same on every PC (below). (The reads are numbered in the order the game issues them.)
struct Gate {
    std::mutex m;
    std::condition_variable cv;  // (a read ended)
    bool on = false;
    u32 epoch = 0;   // which lockstep
    u64 issued = 0;  // reads held in it so far
    std::map<u64, std::function<void(Ctx&)>> held;  // ended here, not yet told the game (by number)
    std::set<u64> ended;                            // ended (or cancelled) past `done`
    u64 done = 0;                                   // reads 1 .. done have all ended here
    u64 released = 0;                               // reads 1 .. released told the game
} s_gate;
thread_local bool t_inHeldEnd = false;  // telling the game a held read's end (what it asks for next is held too)
void gateEnded(u64 gseq) {  // (under s_gate.m)
    s_gate.ended.insert(gseq);
    while (s_gate.ended.count(s_gate.done + 1)) s_gate.ended.erase(++s_gate.done);
    s_gate.cv.notify_all();
}

// Files served in place of the disc's (City Trial maps: dvdOverlay below): the FST entry of each points past the end
// of the disc, to its data here.
constexpr u32 kOverlayBase = 0xA0000000u;  // (a disc is 1.4 GB: well below)
struct Overlay {
    u32 at;
    std::vector<u8> data;
    std::string path;
    u32 loadedAt = 0;  // where the game last read its start into (its guest address; 0: not read)
};
std::vector<Overlay> s_overlays;  // (under s_discLock)
u32 s_overlayNext = kOverlayBase;
struct Watched {  // a disc file whose place in guest memory is wanted (dvdWatch)
    std::string path;
    u64 offset;
    u32 loadedAt = 0;
};
std::vector<Watched> s_watched;  // (under s_discLock)

bool readRaw(u64 offset, void* dst, u32 len) {
    std::lock_guard g(s_discLock);
    if (offset >= kOverlayBase) {
        for (const Overlay& o : s_overlays)
            if (offset >= o.at && offset < o.at + ((o.data.size() + 0x7FFF) & ~size_t(0x7FFF))) {
                const size_t from = size_t(offset - o.at), n = from < o.data.size() ? std::min<size_t>(len, o.data.size() - from) : 0;
                if (n) std::memcpy(dst, o.data.data() + from, n);
                if (n < len) std::memset(static_cast<u8*>(dst) + n, 0, len - n);  // (the padding after the file)
                return true;
            }
        return false;
    }
    return s_disc && s_disc->read(offset, dst, len);
}

void complete(const Request& r, s32 result) {
    s_inFlight--;
    auto fn = [r, result](Ctx& c) {
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
    };
    if (r.gseq) {
        std::lock_guard g(s_gate.m);
        if (s_gate.on && r.epoch == s_gate.epoch) {
            s_gate.held.emplace(r.gseq, [fn = std::move(fn)](Ctx& c) {
                t_inHeldEnd = true;
                fn(c);
                t_inHeldEnd = false;
            });
            gateEnded(r.gseq);
            return;
        }
    }
    sched::postIrq(std::move(fn));
}
// When a held read ends for the game: at the main thread's next safe point (a loop's back edge in the game's
// code: the same place on every PC, wherever it is - a logic step, a loading screen's loop) every read issued
// so far ends - it waits there for any still on its way. And should the main thread sleep before one (on
// something other than its frame queues - it waits for its own read), there: asleep at the same point
// everywhere. So the game sees each read end at the same point of its own run on every PC, whatever the disc's
// speed; nothing about it travels between them.
constexpr u32 kFrameQueues = 0x80550CA8 + 0x24, kFrameQueueSize = 0x34;  // the game's frame queues (4 OSMessageQueues)
// (under s_gate.m) Every read issued so far, once ended here: their ends, in order; `lk` released while waiting.
std::vector<std::function<void(Ctx&)>> takeIssued(std::unique_lock<std::mutex>& lk) {
    std::vector<std::function<void(Ctx&)>> now;
    const u64 upTo = s_gate.issued;
    const auto t0 = std::chrono::steady_clock::now();
    bool told = false;
    while (s_gate.on && s_gate.done < upTo) {
        s_gate.cv.wait_for(lk, std::chrono::milliseconds(100));
        if (!told && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(10))
            told = true, log("[dvdgate] waiting 10 s for read #%llu to end\n", (unsigned long long)(s_gate.done + 1));
    }
    for (auto it = s_gate.held.begin(); it != s_gate.held.end() && it->first <= upTo;) now.push_back(std::move(it->second)), it = s_gate.held.erase(it);
    s_gate.released = std::max(s_gate.released, upTo);
    if (s_gate.issued == upTo) g_dvdGatePending = 0;
    return now;
}
void gateWatch() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!g_dvdGatePending) continue;
        const u32 main = lan::mainThread();
        if (!main || rd16(main + sched::T_STATE) != sched::S_WAITING) continue;
        const u32 q = rd32(main + sched::T_QUEUE);
        if (!q || (q >= kFrameQueues && q < kFrameQueues + 4 * kFrameQueueSize)) continue;
        std::vector<std::function<void(Ctx&)>> now;
        {
            std::unique_lock lk(s_gate.m);
            if (!s_gate.on) continue;
            now = takeIssued(lk);
        }
        if (dvdLog() && !now.empty()) log("[dvdgate] %zu reads end, up to #%llu (the game asleep on %08X)\n", now.size(), (unsigned long long)s_gate.released, q);
        for (auto& fn : now) sched::postIrq(std::move(fn));
    }
}

// A read that ends with nothing to tell (cancelled): in a lockstep it still counts as ended.
void dropped(const Request& r) {
    s_inFlight--;
    if (!r.gseq) return;
    std::lock_guard g(s_gate.m);
    if (s_gate.on && r.epoch == s_gate.epoch) gateEnded(r.gseq);
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
                dropped(r);
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
        if (const double late = done - hostSeconds(); late > 0.5) {
            static int s_n = 0;
            if (s_n++ < 40) log("[dvd] read %llX (%u bytes) done in %.2f s (drive busy until then)\n", (unsigned long long)r.offset, r.length, late);
        }
        sleepUntil(s_clock0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(done)), false);
        {
            std::lock_guard g(s_seqLock);
            if (cancelled()) {
                dropped(r);
                continue;
            }
            if (ok) std::memcpy(hostp(r.addr), buf.data(), r.length);
            if (ok && r.offset >= kOverlayBase) {  // (a served file's start read: where it is, for its users)
                std::lock_guard g2(s_discLock);
                for (Overlay& o : s_overlays)
                    if (o.at == r.offset) o.loadedAt = r.addr;
            }
            if (ok) {  // (a watched disc file's start read: dvdWatch)
                std::lock_guard g2(s_discLock);
                for (Watched& w : s_watched)
                    if (w.offset == r.offset) w.loadedAt = r.addr;
            }
        }
        complete(r, ok ? s32(r.length) : -1);
    }
}

// The disc's files, from the FST in guest memory (built once, when first asked: the FST stays where the
// apploader put it): the file at an offset, for SA2_DVD_LOG, and whether it is a stream.
struct DiscFile {
    u64 start, end;
    std::string path;
    bool stream;  // music (.hps) or a movie (.h4m): read as it plays, in real time
};
const std::vector<DiscFile>& discFiles() {
    static std::vector<DiscFile> files;
    static std::once_flag once;
    std::call_once(once, [] {
        const u32 fst = rd32(0x80000038);
        if (!fst) return;
        const u32 n = rd32(fst + 8);
        const u32 strtab = fst + n * 12;
        // Walk the tree keeping the directory path of each entry.
        std::vector<std::pair<u32, std::string>> dirs{{n, ""}};
        for (u32 i = 1; i < n; i++) {
            while (dirs.size() > 1 && i >= dirs.back().first) dirs.pop_back();
            const u32 w0 = rd32(fst + i * 12), w1 = rd32(fst + i * 12 + 4), w2 = rd32(fst + i * 12 + 8);
            const std::string name = gstr(strtab + (w0 & 0xFFFFFF));
            if (w0 >> 24) {
                dirs.push_back({w2, dirs.back().second + name + "/"});
                continue;
            }
            std::string low = name;
            for (char& ch : low) ch = char(std::tolower(u8(ch)));
            const bool stream = low.size() > 4 && (low.compare(low.size() - 4, 4, ".hps") == 0 || low.compare(low.size() - 4, 4, ".h4m") == 0);
            files.push_back({w1, u64(w1) + w2, dirs.back().second + name, stream});
        }
        std::sort(files.begin(), files.end(), [](const DiscFile& a, const DiscFile& b) { return a.start < b.start; });
    });
    return files;
}
const DiscFile* fileOf(u64 offset) {
    const auto& files = discFiles();
    auto it = std::upper_bound(files.begin(), files.end(), offset, [](u64 o, const DiscFile& f) { return o < f.start; });
    if (it == files.begin()) return nullptr;
    --it;
    return offset < it->end ? &*it : nullptr;
}
std::string fileAt(u64 offset) {
    const DiscFile* f = fileOf(offset);
    return f ? f->path + "+" + std::to_string(offset - f->start) : "?";
}

void submit(u32 block, u32 addr, u32 length, u64 offset, u32 callback, u32 command) {
    static const bool s_log = std::getenv("SA2_DVD_LOG") && std::strcmp(std::getenv("SA2_DVD_LOG"), "gate") != 0;  // (gate: only the lockstep's held reads)
    if (s_log)
        log("[dvd] frame %llu: read %s (%u bytes) -> %08X (thread %08X%s, LAN step %u%s, callback %08X)\n", (unsigned long long)gxFrames(),
            fileAt(offset).c_str(), length, addr, rd32(sched::kCurrentThread), sched::inInterrupt() ? " irq" : "", lan::lanSteps(),
            lan::inSteps() ? " in steps" : "", callback);
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
    // Held in a lockstep: what the game's threads read, and what the end of a held read asks for next (its
    // callback, as it is told - the same step everywhere). Not: music and movies (they read as they play, in
    // real time - nothing in step waits for them), nor what other interrupts ask for (the streams' own).
    u64 gseq = 0;
    u32 epoch = 0;
    {
        std::lock_guard g(s_gate.m);
        const DiscFile* f = fileOf(offset);
        if (s_gate.on && (!sched::inInterrupt() || t_inHeldEnd) && !(f && f->stream)) {
            gseq = ++s_gate.issued, epoch = s_gate.epoch;
            g_dvdGatePending = 1, g_irqPending = 1;  // (the main thread's next safe point ends it)
            if (dvdLog()) log("[dvdgate] read #%llu: %s\n", (unsigned long long)gseq, fileAt(offset).c_str());
        }
    }
    s_inFlight++;
    std::lock_guard g(s_qLock);
    if (!s_workerStarted) {
        s_workerStarted = true;
        std::thread(worker).detach();
        std::thread(gateWatch).detach();
    }
    s_queue.push_back({block, addr, length, callback, offset, seq, gseq, epoch});
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

// The lockstep's reads (above; src/runtime/lansync.cpp calls these). Begin: at its first logic step, the reads
// from then on held.
void dvdGateBegin() {
    std::lock_guard g(s_gate.m);
    s_gate.on = true, s_gate.epoch++, s_gate.issued = 0, s_gate.held.clear(), s_gate.ended.clear(), s_gate.done = 0, s_gate.released = 0;
    g_dvdGatePending = 0;
    log("[dvdgate] on: the game's reads end at its main thread's next safe point\n");
}
// End (the lockstep over): what is held goes to the game now, and reads end as they come again.
void dvdGateEnd() {
    std::map<u64, std::function<void(Ctx&)>> held;
    {
        std::lock_guard g(s_gate.m);
        if (!s_gate.on) return;
        s_gate.on = false;
        g_dvdGatePending = 0;
        held.swap(s_gate.held);
        log("[dvdgate] off: %llu reads held, %zu given at once\n", (unsigned long long)s_gate.issued, held.size());
    }
    for (auto& [seq, fn] : held) sched::postIrq(std::move(fn));
}
// A safe point of a guest thread with a held read pending (src/runtime/cpu.cpp, before the interrupts): on the
// game's main thread, every read issued so far ends now - the same point of its run on every PC.
void dvdGateSafePoint(Ctx& c) {
    if (sched::inInterrupt() || !(c.msr & 0x8000) || rd32(sched::kCurrentThread) != lan::mainThread()) return;
    std::vector<std::function<void(Ctx&)>> now;
    {
        std::unique_lock lk(s_gate.m);
        if (!s_gate.on) {
            g_dvdGatePending = 0;
            return;
        }
        now = takeIssued(lk);
    }
    if (dvdLog() && !now.empty()) log("[dvdgate] LAN step %u: %zu reads end, up to #%llu\n", lan::lanSteps(), now.size(), (unsigned long long)s_gate.released);
    for (auto& fn : now) sched::postIrq(std::move(fn));  // (run by the interrupts that come next, at this point)
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

namespace {
// The FST entry (in guest memory) of a disc file, by its path (any case): its index, or 0.
u32 fstEntry(const std::string& path) {
    const u32 fst = rd32(0x80000038);
    if (!fst) return 0;
    const u32 n = rd32(fst + 8), strtab = fst + n * 12;
    auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower(u8(c)));
        return s;
    };
    const std::string want = lower(path);
    std::vector<std::pair<u32, std::string>> dirs{{n, ""}};
    for (u32 i = 1; i < n; i++) {
        while (dirs.size() > 1 && i >= dirs.back().first) dirs.pop_back();
        const u32 w0 = rd32(fst + i * 12), w2 = rd32(fst + i * 12 + 8);
        const std::string name = gstr(strtab + (w0 & 0xFFFFFF));
        if (w0 >> 24) dirs.push_back({w2, dirs.back().second + name + "/"});
        else if (lower(dirs.back().second + name) == want) return i;
    }
    return 0;
}
}  // namespace

// Where in guest memory the game last read a served file's start (0: not read yet), and its size.
u32 dvdOverlayLoadedAt(const std::string& path, u32* size) {
    std::lock_guard g(s_discLock);
    for (const Overlay& o : s_overlays)
        if (o.path == path) {
            if (size) *size = u32(o.data.size());
            return o.loadedAt;
        }
    return 0;
}

// Serves `data` as the disc file at `path` from now on (the game reads a file's place and size from the FST when it
// opens it: the next open sees the new file). Empty data: the disc's own again. False if the disc has no such file.
// The game keeps its own copies of some files' places (opened once, early: their DVDFileInfo's start and length, as
// the FST had them): each copy found in MEM1 - the old place and size side by side - moved with the FST's entry.
void moveCopies(u32 oldAt, u32 oldSize, u32 newAt, u32 newSize, const std::string& path) {
    int n = 0;
    for (u32 a = 0x80003100; a + 8 <= 0x80000000 + kMem1Size; a += 4)
        if (rd32(a) == oldAt && rd32(a + 4) == oldSize) wr32(a, newAt), wr32(a + 4, newSize), n++;
    if (n) log("[dvd] %s: %d copies of its place moved too\n", path.c_str(), n);
}

bool dvdOverlay(const std::string& path, std::vector<u8> data) {
    const u32 i = fstEntry(path);
    if (!i) return false;
    const u32 fst = rd32(0x80000038);
    std::lock_guard g(s_discLock);
    const u32 oldAt = rd32(fst + i * 12 + 4), oldSize = rd32(fst + i * 12 + 8);
    if (data.empty()) {  // (the disc's entry, from the FST as read)
        if (size_t(i) * 12 + 12 <= s_fstCopy.size()) std::memcpy(hostp(fst + i * 12 + 4), s_fstCopy.data() + i * 12 + 4, 8);
        if (oldAt != rd32(fst + i * 12 + 4)) moveCopies(oldAt, oldSize, rd32(fst + i * 12 + 4), rd32(fst + i * 12 + 8), path);
        return true;
    }
    const u32 at = s_overlayNext;
    s_overlayNext += u32((data.size() + 0x7FFF) & ~size_t(0x7FFF));
    wr32(fst + i * 12 + 4, at);
    wr32(fst + i * 12 + 8, u32(data.size()));
    moveCopies(oldAt, oldSize, at, u32(data.size()), path);
    log("[dvd] %s: %zu bytes served in place of the disc's\n", path.c_str(), data.size());
    {
        std::string low = path;
        for (char& ch : low) ch = char(std::tolower(u8(ch)));
        if (low == "grcity1.dat") g_customCity = true;  // (a custom map in the city's place: src/runtime/maps.cpp)
    }
    s_overlays.push_back({at, std::move(data), path});
    return true;
}

// A folder's files served in place of the disc's at the same paths (a City Trial map: GrCity1.dat; audio/jp/city.hps in
// its audio/jp folder); the paths served added to `served`. Its other files (the map's description, pictures) are not.
void dvdServeDir(const std::string& dir, std::vector<std::string>& served, const std::vector<std::string>& skip) {
    std::error_code ec;
    const auto root = std::filesystem::u8path(dir);
    for (const auto& e : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (!e.is_regular_file()) continue;
        const std::u8string rel = e.path().lexically_relative(root).generic_u8string();
        const std::string name(rel.begin(), rel.end());
        if (fstEntry(name) == 0 || std::find(skip.begin(), skip.end(), name) != skip.end()) continue;
        std::FILE* f = _wfopen(e.path().c_str(), L"rb");
        if (!f) continue;
        std::vector<u8> data(size_t(e.file_size(ec)));
        const size_t got = std::fread(data.data(), 1, data.size(), f);
        std::fclose(f);
        if (got == data.size() && dvdOverlay(name, std::move(data))) served.push_back(name);
    }
}

// The disc's own files at those paths again (between matches: nothing of theirs is being read), their data let go.
void dvdUnserve(const std::vector<std::string>& paths) {
    for (const std::string& path : paths) {
        dvdOverlay(path, {});
        std::lock_guard g(s_discLock);
        std::erase_if(s_overlays, [&](const Overlay& o) { return o.path == path; });
        std::string low = path;
        for (char& ch : low) ch = char(std::tolower(u8(ch)));
        if (low == "grcity1.dat") g_customCity = false;
    }
    std::lock_guard g(s_discLock);
    if (s_overlays.empty()) s_overlayNext = kOverlayBase;
}

u32 dvdEntryNum(const std::string& path) { return fstEntry(path); }

// A disc file as the disc has it (whatever is served in its place).
std::vector<u8> dvdDiscFile(const std::string& path) {
    const u32 i = fstEntry(path);
    if (!i || size_t(i) * 12 + 12 > s_fstCopy.size()) return {};
    auto be = [&](size_t o) { return u32(s_fstCopy[o]) << 24 | s_fstCopy[o + 1] << 16 | s_fstCopy[o + 2] << 8 | s_fstCopy[o + 3]; };
    std::vector<u8> data(be(i * 12 + 8));
    if (!readRaw(be(i * 12 + 4), data.data(), u32(data.size()))) return {};
    return data;
}

// Where the game reads a disc file to, from now on (dvdLoadedAt): its start's reads are watched.
void dvdWatch(const std::string& path) {
    const u32 i = fstEntry(path);
    if (!i || size_t(i) * 12 + 12 > s_fstCopy.size()) return;
    const size_t o = i * 12 + 4;
    std::lock_guard g(s_discLock);
    s_watched.push_back({path, u64(u32(s_fstCopy[o]) << 24 | s_fstCopy[o + 1] << 16 | s_fstCopy[o + 2] << 8 | s_fstCopy[o + 3])});
}
u32 dvdLoadedAt(const std::string& path) {
    std::lock_guard g(s_discLock);
    for (const Watched& w : s_watched)
        if (w.path == path) return w.loadedAt;
    return 0;
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

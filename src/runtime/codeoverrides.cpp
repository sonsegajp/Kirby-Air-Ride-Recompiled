// The recompiler's instruction overrides at run time (src/recompiler/overrides.h; docs/MORE_PLAYERS.md), and its
// memory-access trace. Both exist only in programs generated with --overrides / --trace-mem: the generated code
// registers its tables here when it starts; a program generated without them never calls any of this.
//
//   - The changed instruction words go into guest memory once main.dol is loaded (applyCodeOverrides), so the
//     interpreter - functions replaced at run time (interpReplace) still run their own code there - runs the
//     same code as the translation.
//   - The moves (an instruction's address moved to new storage) are kept for the interpreter (codeMove).
//   - SA2_MEMTRACE=<file> (with --trace-mem): every instruction whose load/store address or addi/add/ori result
//     fell in the traced range, with the distinct values it had (up to 24) and the scenes it ran in; written
//     at exit and every 20 s. One line per instruction:
//       <pc> <kinds: L load S store V value> <size> <count> <scene mask> <n distinct|+ more> <values...>
#include "runtime/runtime.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rt {

namespace {
const u32* s_words = nullptr;
u32 s_wordCount = 0;
std::unordered_map<u32, std::vector<std::array<u32, 3>>>* s_moves = nullptr;

// ---- the trace ----
struct Entry {
    u32 pc;
    u32 kinds;       // 1 load, 2 store, 4 value
    u32 size;        // largest access
    u64 count;
    u64 scenes;      // bit (scene & 63)
    u8 n;            // distinct values kept
    bool more;       // ... and others
    u32 v[24];
};
constexpr u32 kTableBits = 20;
Entry* s_table = nullptr;
std::atomic<u32> s_used{0};
std::string s_traceFile;
std::mutex s_dumpLock;

void dumpTrace() {
    if (!s_table || s_traceFile.empty()) return;
    std::lock_guard g(s_dumpLock);
    const std::string tmp = s_traceFile + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    for (u32 i = 0; i < (1u << kTableBits); i++) {
        const Entry& e = s_table[i];
        if (!e.pc) continue;
        std::fprintf(f, "%08X %s%s%s %u %llu %016llX %u%s", e.pc, e.kinds & 1 ? "L" : "", e.kinds & 2 ? "S" : "", e.kinds & 4 ? "V" : "",
                     e.size, (unsigned long long)e.count, (unsigned long long)e.scenes, e.n, e.more ? "+" : "");
        for (u32 k = 0; k < e.n; k++) std::fprintf(f, " %08X", e.v[k]);
        std::fputc('\n', f);
    }
    std::fclose(f);
    std::remove(s_traceFile.c_str());
    std::rename(tmp.c_str(), s_traceFile.c_str());
}

void traceInit() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    const char* e = std::getenv("SA2_MEMTRACE");
    if (!e || !*e) return;
    s_traceFile = e;
    s_table = static_cast<Entry*>(std::calloc(size_t(1) << kTableBits, sizeof(Entry)));
    std::atexit(dumpTrace);
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(20));
            dumpTrace();
        }
    }).detach();
    log("[memtrace] tracing to %s\n", e);
}
}  // namespace

void registerCodeOverrides(const u32* words, u32 wordCount, const u32* moves, u32 moveCount) {
    s_words = words;
    s_wordCount = wordCount;
    static std::unordered_map<u32, std::vector<std::array<u32, 3>>> m;
    for (u32 i = 0; i < moveCount; i++) m[moves[4 * i]].push_back({moves[4 * i + 1], moves[4 * i + 2], moves[4 * i + 3]});
    s_moves = &m;
}

// main.dol is in guest memory: the changed words go in.
void applyCodeOverrides() {
    traceInit();
    if (!s_wordCount && !s_moves) return;
    for (u32 i = 0; i < s_wordCount; i++) wr32(s_words[2 * i], s_words[2 * i + 1]);
    log("[overrides] %u instruction words changed, %zu instructions with moves\n", s_wordCount, s_moves ? s_moves->size() : size_t(0));
}

// The interpreter's view of the moves: the address `x` the instruction at `pc` computed, moved.
bool codeMovesActive() { return s_moves && !s_moves->empty(); }
u32 codeMove(u32 pc, u32 x) {
    if (!s_moves) return x;
    const auto it = s_moves->find(pc);
    if (it == s_moves->end()) return x;
    for (const auto& m : it->second)
        if (x - m[0] < m[1]) x = x - m[0] + m[2];
    return x;
}

void memTrace(u32 pc, u32 x, u32 info) {
    if (!s_table) {
        traceInit();
        if (!s_table) return;
    }
    u32 h = (pc * 2654435761u) >> (32 - kTableBits);
    for (;;) {
        Entry& e = s_table[h];
        if (e.pc == pc) break;
        if (e.pc == 0) {
            if (s_used.load() > (3u << kTableBits) / 4) return;  // (full: the rest are not traced)
            e.pc = pc;
            s_used++;
            break;
        }
        h = (h + 1) & ((1u << kTableBits) - 1);
    }
    Entry& e = s_table[h];
    e.count++;
    e.scenes |= u64(1) << (rd8(0x805359D8 + 0x7D7) & 63);  // (the game's major scene: Kirby Air Ride's gm state)
    e.kinds |= (info & 0x200) ? 4u : (info & 0x100) ? 2u : 1u;
    if ((info & 0xFF) > e.size) e.size = info & 0xFF;
    for (u32 k = 0; k < e.n; k++)
        if (e.v[k] == x) return;
    if (e.n < 24) e.v[e.n++] = x;
    else e.more = true;
}

}  // namespace rt

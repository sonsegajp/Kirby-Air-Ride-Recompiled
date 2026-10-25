#include "recompiler/analyze.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace sa2rc {

namespace {

bool isUncondBo(u32 bo) { return (bo & 0x14) == 0x14; }

struct Target {
    bool ok = false;
    u32 module = 0;  // module id of the target
    u32 addr = 0;
};

// Target of a b/bl at `a` (relocated for RELs).
Target branchTarget(const Module& m, u32 a, u32 w) {
    if (!m.isDol()) {
        if (const Reloc* r = m.relocOf(a, false); r && r->type == R_PPC_REL24) return {true, r->module, r->target};
    }
    const s32 li = sext(w & 0x03FFFFFC, 26);
    const u32 t = (w & 2) ? u32(li) : a + u32(li);
    return {true, m.id, t};
}

Target bcTarget(const Module& m, u32 a, u32 w) {
    if (!m.isDol()) {
        if (const Reloc* r = m.relocOf(a, false); r && r->type == R_PPC_REL14) return {true, r->module, r->target};
    }
    const s32 bd = sext16(w & 0xFFFC);
    const u32 t = (w & 2) ? u32(bd) : a + u32(bd);
    return {true, m.id, t};
}

u32 sectionEnd(const Module& m, u32 a) {
    const Section* s = m.sectionAt(a);
    return s ? s->addr + s->size : a;
}

// Value of register `reg` built by lis/addi (or lis/ori) shortly before `at`, or by an
// ADDR16_HA/LO relocation pair in a REL. Returns false when it cannot be resolved.
bool constBefore(const Module& m, u32 at, u32 lo, u32 reg, u32& value) {
    u32 low = 0;
    bool haveLow = false;
    u32 cur = reg;
    for (u32 p = at; p >= lo && p + 64 * 4 > at; p -= 4) {
        const u32 w = m.word(p);
        const u32 op = w >> 26;
        const u32 d = bits(w, 6, 10), a = bits(w, 11, 15);
        if (!haveLow && (op == 14 || op == 24) && d == cur) {  // addi / ori
            if (!m.isDol()) {
                if (const Reloc* r = m.relocOf(p, true)) {
                    value = r->target;
                    return true;
                }
            }
            low = op == 14 ? u32(sext16(w & 0xFFFF)) : (w & 0xFFFF);
            haveLow = true;
            cur = a;
            if (op == 14 && a == 0) {  // li
                value = low;
                return true;
            }
            continue;
        }
        if (op == 15 && d == cur) {  // lis / addis
            if (!m.isDol()) {
                if (const Reloc* r = m.relocOf(p, true)) {
                    value = r->target;
                    return true;
                }
            }
            if (a != 0) return false;
            value = ((w & 0xFFFF) << 16) + (haveLow ? low : 0);
            return true;
        }
        if (p == lo) break;
    }
    return false;
}

// Case targets of the jump table dispatched by the bctr at `a`, if this is the MWCC switch idiom.
std::vector<u32> findJumpTable(const Module& m, u32 a, u32 lo, u32 hi, std::vector<u32>& tableWordsOut) {
    std::vector<u32> none;
    u32 ctrReg = 32, mtctrAt = 0;
    for (u32 p = a - 4; p >= lo && p + 24 * 4 > a; p -= 4) {
        const u32 w = m.word(p);
        // mtspr: the SPR number is split, low half in bits 11-15 and high half in bits 16-20.
        if ((w >> 26) == 31 && bits(w, 21, 30) == 467 && (bits(w, 11, 15) | bits(w, 16, 20) << 5) == 9) {
            ctrReg = bits(w, 6, 10);
            mtctrAt = p;
            break;
        }
        if (p == lo) break;
    }
    if (ctrReg == 32) return none;
    // The load that produced the target.
    u32 loadAt = 0, baseA = 32, baseB = 32;
    s32 disp = 0;
    for (u32 p = mtctrAt - 4; p >= lo && p + 16 * 4 > mtctrAt; p -= 4) {
        const u32 w = m.word(p);
        const u32 op = w >> 26;
        if (op == 31 && bits(w, 21, 30) == 23 && bits(w, 6, 10) == ctrReg) {  // lwzx
            baseA = bits(w, 11, 15);
            baseB = bits(w, 16, 20);
            loadAt = p;
            break;
        }
        if (op == 32 && bits(w, 6, 10) == ctrReg) {  // lwz
            baseA = bits(w, 11, 15);
            disp = sext16(w & 0xFFFF);
            loadAt = p;
            break;
        }
        if (p == lo) break;
    }
    if (!loadAt) return none;
    u32 table = 0;
    bool found = false;
    for (u32 r : {baseA, baseB}) {
        if (r >= 32 || r == 0) continue;
        if (constBefore(m, loadAt - 4, lo, r, table)) {
            found = true;
            break;
        }
    }
    if (!found) return none;
    table += u32(disp);
    // Bound: the nearest preceding cmplwi rX, N gives N+1 entries.
    u32 count = 0;
    for (u32 p = loadAt - 4; p >= lo && p + 24 * 4 > loadAt; p -= 4) {
        const u32 w = m.word(p);
        if ((w >> 26) == 10 && bits(w, 9, 10) == 0) {  // cmpli crf, 0, rA, UIMM
            count = (w & 0xFFFF) + 1;
            break;
        }
        if (p == lo) break;
    }
    const bool bounded = count != 0;
    if (!bounded) count = 1024;
    std::vector<u32> out;
    for (u32 i = 0; i < count; i++) {
        const u32 at = table + i * 4;
        u32 t;
        if (m.isDol()) {
            if (!m.inData(at) && !m.inText(at)) break;
            t = m.word(at);
        } else {
            const auto* rs = m.relocsAt(at);
            if (!rs) break;
            const Reloc* r = nullptr;
            for (const auto& x : *rs)
                if (x.type == R_PPC_ADDR32) r = &x;
            if (!r || r->module != m.id) break;
            t = r->target;
        }
        if (t < lo || t >= hi || (t & 3)) {
            if (bounded) return none;  // the bound says this entry belongs to the table: not our idiom
            break;
        }
        out.push_back(t);
        tableWordsOut.push_back(at);
    }
    return out;
}

struct Walk {
    u32 maxEnd = 0;
    std::vector<u32> tails;  // branch targets outside the region, in this module
    std::vector<std::pair<u32, std::vector<u32>>> tables;
    std::vector<u32> tableWords;
    size_t unresolvedBctr = 0;
};

Walk walk(const Module& m, u32 S, u32 R) {
    Walk w;
    w.maxEnd = S + 4;
    std::vector<u32> stack{S};
    std::set<u32> seen;
    while (!stack.empty()) {
        u32 a = stack.back();
        stack.pop_back();
        for (;;) {
            if (a < S || a >= R || seen.count(a)) break;
            seen.insert(a);
            w.maxEnd = std::max(w.maxEnd, a + 4);
            const u32 ins = m.word(a);
            const u32 op = ins >> 26;
            if (op == 18) {
                if (ins & 1) {
                    a += 4;
                    continue;
                }
                const Target t = branchTarget(m, a, ins);
                if (t.module == m.id) {
                    if (t.addr >= S && t.addr < R) stack.push_back(t.addr);
                    else w.tails.push_back(t.addr);
                }
                break;
            }
            if (op == 16) {
                const u32 bo = bits(ins, 6, 10);
                if (!(ins & 1)) {
                    const Target t = bcTarget(m, a, ins);
                    if (t.module == m.id) {
                        if (t.addr >= S && t.addr < R) stack.push_back(t.addr);
                        else w.tails.push_back(t.addr);
                    }
                    if (isUncondBo(bo)) break;
                }
                a += 4;
                continue;
            }
            if (op == 19) {
                const u32 xo = bits(ins, 21, 30);
                const u32 bo = bits(ins, 6, 10);
                if (xo == 16) {  // bclr
                    if (!(ins & 1) && isUncondBo(bo)) break;
                    a += 4;
                    continue;
                }
                if (xo == 528) {  // bcctr
                    if (!(ins & 1)) {
                        std::vector<u32> tw;
                        auto targets = findJumpTable(m, a, S, R, tw);
                        if (!targets.empty()) {
                            for (u32 t : targets) stack.push_back(t);
                            w.tables.push_back({a, std::move(targets)});
                            w.tableWords.insert(w.tableWords.end(), tw.begin(), tw.end());
                        } else {
                            w.unresolvedBctr++;
                        }
                        if (isUncondBo(bo | 4)) break;
                    }
                    a += 4;
                    continue;
                }
                if (xo == 50) break;  // rfi
            }
            a += 4;
        }
    }
    return w;
}

bool isCode(const Module& m, u32 a) { return m.inText(a) && !(a & 3); }

// Call targets, cross-module seeds and pointer candidates gathered by linear scans.
struct Scan {
    std::set<u32> strong;
    // Pointer candidates: target -> where the pointer was found (module id, data address), or
    // {module, 0} when code built it with lis/addi. Jump-table entries are filtered out later.
    std::map<u32, std::vector<std::pair<u32, u32>>> weak;
};

// True when some source of the candidate is not a jump-table entry of `m`.
bool realPointer(const Module& m, const std::vector<std::pair<u32, u32>>& srcs, const std::set<u32>& tableWords) {
    for (auto& [mod, at] : srcs)
        if (mod != m.id || at == 0 || !tableWords.count(at)) return true;
    return false;
}

void scanModule(const Module& m, std::map<u32, Scan>& scans) {
    Scan& self = scans[m.id];
    for (const auto& s : m.sections) {
        if (!s.exec || !s.fileOff || !s.size) continue;
        if (m.word(s.addr)) self.strong.insert(s.addr);
        u32 hiReg[32]{};
        bool hiValid[32]{};
        for (u32 a = s.addr; a < s.addr + s.size; a += 4) {
            const u32 w = m.word(a);
            const u32 op = w >> 26;
            if (op == 18 && (w & 1)) {
                const Target t = branchTarget(m, a, w);
                if (t.module == m.id && t.addr == a + 4) continue;  // bl to the next instruction: reads the PC
                scans[t.module].strong.insert(t.addr);
            } else if (op == 18 && !m.isDol()) {
                // b with a relocation into another module is a tail call to a function there.
                if (const Reloc* r = m.relocOf(a, false); r && r->type == R_PPC_REL24 && r->module != m.id)
                    scans[r->module].strong.insert(r->target);
            }
            // Code addresses materialised in registers.
            if (m.isDol()) {
                const u32 d = bits(w, 6, 10), ra = bits(w, 11, 15);
                if (op == 15 && ra == 0) {
                    hiReg[d] = (w & 0xFFFF) << 16;
                    hiValid[d] = true;
                } else if ((op == 14 || op == 24) && ra != 0 && hiValid[ra]) {
                    const u32 v = hiReg[ra] + (op == 14 ? u32(sext16(w & 0xFFFF)) : (w & 0xFFFF));
                    if (isCode(m, v)) self.weak[v].push_back({m.id, 0});
                }
            } else if (const Reloc* r = m.relocOf(a, true); r && (r->type == R_PPC_ADDR16_LO)) {
                scans[r->module].weak[r->target].push_back({m.id, 0});
            }
        }
    }
    // Pointers stored in data.
    if (m.isDol()) {
        for (const auto& s : m.sections) {
            if (s.exec || !s.fileOff) continue;
            for (u32 a = s.addr; a + 4 <= s.addr + s.size; a += 4) {
                const u32 v = m.word(a);
                if (isCode(m, v)) self.weak[v].push_back({m.id, a});
            }
        }
    }
    for (const auto& [at, rs] : m.relocs) {
        for (const auto& r : rs) {
            if (r.type != R_PPC_ADDR32) continue;
            scans[r.module].weak[r.target].push_back({m.id, at});
        }
    }
    if (m.prolog) self.strong.insert(m.prolog);
    if (m.epilog) self.strong.insert(m.epilog);
    if (m.unresolved) self.strong.insert(m.unresolved);
    if (m.isDol()) self.strong.insert(m.entry);
}

void discover(ModuleInfo& mi, const Scan& scan) {
    const Module& m = *mi.mod;
    std::set<u32> seeds;
    for (u32 s : scan.strong)
        if (isCode(m, s)) seeds.insert(s);
    std::set<u32> tableWords;
    for (;;) {
        mi.funcs.clear();
        mi.jumpTables.clear();
        tableWords.clear();
        mi.unresolvedBctr = 0;
        std::vector<u32> v(seeds.begin(), seeds.end());
        std::set<u32> fresh;
        for (size_t i = 0; i < v.size(); i++) {
            const u32 S = v[i];
            u32 R = sectionEnd(m, S);
            if (i + 1 < v.size() && v[i + 1] < R) R = v[i + 1];
            Walk w = walk(m, S, R);
            Func f;
            f.start = S;
            f.end = w.maxEnd;
            mi.funcs[S] = f;
            for (u32 t : w.tails)
                if (isCode(m, t) && !seeds.count(t)) fresh.insert(t);
            for (auto& [at, tg] : w.tables) mi.jumpTables[at] = std::move(tg);
            tableWords.insert(w.tableWords.begin(), w.tableWords.end());
            mi.unresolvedBctr += w.unresolvedBctr;
        }
        if (!fresh.empty()) {
            mi.tailSeeds += fresh.size();
            seeds.insert(fresh.begin(), fresh.end());
            continue;
        }
        // Gaps: non-padding code no function reached.
        std::vector<std::pair<u32, u32>> cover;
        for (auto& [s, f] : mi.funcs) cover.push_back({f.start, f.end});
        size_t ci = 0;
        for (const auto& s : m.sections) {
            if (!s.exec || !s.fileOff) continue;
            for (u32 a = s.addr; a < s.addr + s.size; a += 4) {
                while (ci < cover.size() && cover[ci].second <= a) ci++;
                if (ci < cover.size() && cover[ci].first <= a) {
                    a = cover[ci].second - 4;
                    continue;
                }
                if (m.word(a) == 0) continue;
                fresh.insert(a);
                // skip to the next covered address
                while (a + 4 < s.addr + s.size && !(ci < cover.size() && cover[ci].first <= a + 4)) a += 4;
            }
        }
        if (!fresh.empty()) {
            mi.gapSeeds += fresh.size();
            seeds.insert(fresh.begin(), fresh.end());
            continue;
        }
        // Pointers that land outside every function.
        for (auto& [t, srcs] : scan.weak) {
            if (!isCode(m, t) || seeds.count(t) || !realPointer(m, srcs, tableWords)) continue;
            auto it = mi.funcs.upper_bound(t);
            bool inside = false;
            if (it != mi.funcs.begin()) {
                --it;
                inside = t < it->second.end;
            }
            if (!inside) fresh.insert(t);
        }
        if (!fresh.empty()) {
            mi.pointerSeeds += fresh.size();
            seeds.insert(fresh.begin(), fresh.end());
            continue;
        }
        break;
    }
    mi.tableWords = tableWords;
    // Pointers into the middle of a function: alias entries.
    std::map<u32, Func> aliases;
    for (auto& [t, srcs] : scan.weak) {
        if (!isCode(m, t) || mi.funcs.count(t) || !realPointer(m, srcs, tableWords)) continue;
        auto it = mi.funcs.upper_bound(t);
        if (it == mi.funcs.begin()) continue;
        --it;
        if (t >= it->second.end) continue;
        Func f;
        f.start = t;
        f.body = it->second.start;
        f.end = it->second.end;
        f.alias = true;
        aliases[t] = f;
    }
    mi.aliases = aliases.size();
    for (auto& [a, f] : aliases) mi.funcs[a] = f;
}

}  // namespace

std::string funcSymbol(const Module& m, u32 addr) {
    char buf[32];
    if (m.isDol()) std::snprintf(buf, sizeof buf, "f_%08X", addr);
    else std::snprintf(buf, sizeof buf, "m%u_%08X", m.id, addr);
    return buf;
}

void analyze(Program& p) {
    p.info.assign(p.modules.size(), {});
    std::map<u32, Scan> scans;
    for (const auto& m : p.modules) scanModule(m, scans);
    for (size_t i = 0; i < p.modules.size(); i++) {
        p.info[i].mod = &p.modules[i];
        discover(p.info[i], scans[p.modules[i].id]);
    }
}

void applyNames(Program& p, const std::string& text, const std::set<std::string>& hleLibs) {
    std::istringstream in(text);
    std::string line;
    ModuleInfo& dol = p.info[0];
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string addrS, name, lib;
        ls >> addrS >> name >> lib;
        if (name.empty()) continue;
        const u32 addr = u32(std::stoul(addrS, nullptr, 16));
        auto it = dol.funcs.find(addr);
        if (it == dol.funcs.end()) continue;
        it->second.name = name;
        if (hleLibs.count(lib)) it->second.hle = true;
    }
}

}  // namespace sa2rc

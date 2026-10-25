// Developer CLI for the recompiler: sa2recompile --dol main.dol --rels <dir> [--names f] [--funcs out.tsv]
// [--out dir]. The shipped program runs the same code in-process at first-run setup.
#include "recompiler/analyze.h"
#include "recompiler/emit.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace sa2rc;

static bool readFile(const std::filesystem::path& p, std::vector<u8>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

int main(int argc, char** argv) {
    std::string dolPath, relDir, funcsOut, outDir, namesPath, patchesPath, overridesPath, traceMem;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string k = argv[i];
        if (k == "--dol") dolPath = argv[i + 1];
        else if (k == "--rels") relDir = argv[i + 1];
        else if (k == "--funcs") funcsOut = argv[i + 1];
        else if (k == "--out") outDir = argv[i + 1];
        else if (k == "--names") namesPath = argv[i + 1];
        else if (k == "--patches") patchesPath = argv[i + 1];
        else if (k == "--overrides") overridesPath = argv[i + 1];  // recompiler/overrides.h
        else if (k == "--trace-mem") traceMem = argv[i + 1];       // LO:HI (hex): accesses there reported
    }
    if (dolPath.empty()) {
        std::fprintf(stderr, "usage: sa2recompile --dol main.dol [--rels dir] [--funcs out.tsv]\n");
        return 2;
    }
    Program prog;
    std::string err;
    std::vector<u8> data;
    if (!readFile(dolPath, data)) return std::fprintf(stderr, "can't read %s\n", dolPath.c_str()), 1;
    prog.modules.emplace_back();
    if (!loadDol(data, prog.modules.back(), err)) return std::fprintf(stderr, "%s\n", err.c_str()), 1;
    if (!relDir.empty()) {
        std::vector<std::filesystem::path> rels;
        for (auto& e : std::filesystem::directory_iterator(relDir))
            if (e.path().extension() == ".rel") rels.push_back(e.path());
        std::sort(rels.begin(), rels.end());
        for (auto& p : rels) {
            if (!readFile(p, data)) return std::fprintf(stderr, "can't read %s\n", p.string().c_str()), 1;
            Module m;
            if (!loadRel(data, p.filename().string(), m, err)) return std::fprintf(stderr, "%s\n", err.c_str()), 1;
            // ChaoStgLobby.rel reuses ChaoMotionsD's id and nothing loads it: keep the first module per id.
            if (prog.byId(m.id)) {
                std::fprintf(stderr, "note: skipping %s (module id %u already used)\n", m.name.c_str(), m.id);
                continue;
            }
            prog.modules.push_back(std::move(m));
        }
    }
    analyze(prog);
    // --patches <file>: main.dol addresses (hex, one per line, '#' comments) where native mod code
    // can run (src/runtime/mods.cpp registers it by address).
    if (!patchesPath.empty()) {
        std::ifstream f(patchesPath);
        for (std::string line; std::getline(f, line);) {
            line = line.substr(0, line.find('#'));
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            const u32 a = u32(std::stoul(line, nullptr, 16));
            if (!prog.patchPoints.count(a)) prog.patchPoints.emplace(a, int(prog.patchPoints.size()));
        }
    }
    if (!overridesPath.empty()) {
        if (!loadOverrides(overridesPath, prog.modules[0], prog.overrides, err)) return std::fprintf(stderr, "%s\n", err.c_str()), 1;
        size_t nm = 0;
        for (auto& [a, ms] : prog.overrides.moves) nm += ms.size();
        std::printf("overrides: %zu instruction words, %zu moves at %zu instructions\n", prog.overrides.words.size(), nm,
                    prog.overrides.moves.size());
    }
    if (!traceMem.empty()) {
        const size_t colon = traceMem.find(':');
        if (colon == std::string::npos) return std::fprintf(stderr, "--trace-mem LO:HI\n"), 2;
        prog.overrides.trace = true;
        prog.overrides.traceLo = u32(std::stoul(traceMem.substr(0, colon), nullptr, 16));
        prog.overrides.traceHi = u32(std::stoul(traceMem.substr(colon + 1), nullptr, 16));
        std::printf("tracing memory accesses in %08X-%08X\n", prog.overrides.traceLo, prog.overrides.traceHi);
    }
    if (!namesPath.empty()) {
        std::ifstream f(namesPath);
        std::string text((std::istreambuf_iterator<char>(f)), {});
        applyNames(prog, text, {"os", "gx", "vi", "pad", "si", "dvd", "ai", "ar", "arq", "dsp", "card", "exi", "ax", "db", "OdemuExi2", "amcstubs", "odenotstub", "TRK"});
    }
    size_t totalF = 0, totalA = 0;
    u64 totalWords = 0, covered = 0;
    for (auto& mi : prog.info) {
        const Module& m = *mi.mod;
        u64 words = 0, cov = 0;
        for (auto& s : m.sections) {
            if (!s.exec || !s.fileOff) continue;
            for (u32 a = s.addr; a < s.addr + s.size; a += 4) words += m.word(a) != 0;
        }
        size_t nf = 0;
        for (auto& [a, f] : mi.funcs) {
            if (f.alias) continue;
            nf++;
            for (u32 x = f.start; x < f.end; x += 4) cov += m.word(x) != 0;
        }
        totalF += nf;
        totalA += mi.aliases;
        totalWords += words;
        covered += cov;
        if (m.isDol() || m.name == "stg13D.rel" || m.name == "titleD.rel")
            std::printf("%-20s funcs %6zu aliases %4zu tail-seeds %4zu gap-seeds %4zu ptr-seeds %4zu jumptables %4zu "
                        "unresolved-bctr %3zu coverage %.3f%%\n",
                        m.name.c_str(), nf, mi.aliases, mi.tailSeeds, mi.gapSeeds, mi.pointerSeeds,
                        mi.jumpTables.size(), mi.unresolvedBctr, words ? 100.0 * cov / words : 0.0);
    }
    std::printf("ALL %zu modules: %zu functions + %zu aliases, %llu code words, coverage %.3f%%\n", prog.modules.size(),
                totalF, totalA, (unsigned long long)totalWords, totalWords ? 100.0 * covered / totalWords : 0.0);
    if (!funcsOut.empty()) {
        std::ofstream o(funcsOut);
        for (auto& mi : prog.info)
            for (auto& [a, f] : mi.funcs)
                o << mi.mod->id << '\t' << std::hex << f.start << '\t' << f.end << std::dec << '\t' << (f.alias ? 1 : 0)
                  << '\n';
    }
    if (!outDir.empty()) {
        EmitStats st;
        auto files = emitProgram(prog, st);
        std::filesystem::create_directories(outDir);
        u64 bytes = 0;
        for (auto& f : files) {
            const auto path = std::filesystem::path(outDir) / f.name;
            // Only rewrite changed files so incremental builds stay incremental.
            std::ifstream old(path, std::ios::binary);
            std::string prev((std::istreambuf_iterator<char>(old)), {});
            if (prev != f.text) std::ofstream(path, std::ios::binary) << f.text;
            bytes += f.text.size();
        }
        std::printf("emitted %zu files, %.1f MB: %zu functions (%zu HLE), %zu instructions\n", files.size(),
                    bytes / 1048576.0, st.functions, st.hle, st.instructions);
        for (auto& [k, v] : st.unsupported) std::printf("  unsupported %s: %zu\n", k.c_str(), v);
        std::printf("direct branches to addresses no function starts at: %zu\n", st.unresolvedTargets.size());
        for (auto& [k, v] : st.unresolvedTargets) std::printf("  %s (%zu)\n", k.c_str(), v);
    }
    return 0;
}

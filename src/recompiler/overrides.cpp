#include "recompiler/overrides.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace sa2rc {

namespace {

struct Parser {
    const Module& dol;
    CodeOverrides& out;
    std::map<std::string, u32> names;
    std::string error;

    bool number(const std::string& t, u32& v) {
        if (t.empty()) return false;
        const bool hex0x = t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X');
        bool allHex = true, allDec = true;
        for (size_t i = hex0x ? 2 : 0; i < t.size(); i++) {
            allHex = allHex && std::isxdigit(u8(t[i]));
            allDec = allDec && std::isdigit(u8(t[i]));
        }
        if (hex0x && allHex) return v = u32(std::stoul(t.substr(2), nullptr, 16)), true;
        if (t.size() == 8 && allHex) return v = u32(std::stoul(t, nullptr, 16)), true;
        if (allDec) return v = u32(std::stoul(t, nullptr, 10)), true;
        return false;
    }
    bool expr(const std::string& e, u32& v) {
        v = 0;
        size_t p = 0;
        int sign = 1;
        bool term = false;
        while (p < e.size()) {
            const char ch = e[p];
            if (ch == '+' || ch == '-') {
                if (!term && ch == '-') sign = -sign;
                else if (term) sign = ch == '-' ? -1 : 1, term = false;
                p++;
                continue;
            }
            size_t q = p;
            while (q < e.size() && e[q] != '+' && e[q] != '-') q++;
            const std::string t = e.substr(p, q - p);
            u32 x;
            if (auto it = names.find(t); it != names.end()) x = it->second;
            else if (!number(t, x)) return error = "bad term '" + t + "'", false;
            v += sign > 0 ? x : 0u - x;
            sign = 1, term = true, p = q;
        }
        if (!term) return error = "empty expression", false;
        return true;
    }

    bool file(const std::filesystem::path& path) {
        std::ifstream f(path);
        if (!f) return error = "can't read " + path.string(), false;
        int lineNo = 0;
        for (std::string line; std::getline(f, line);) {
            lineNo++;
            line = line.substr(0, line.find('#'));
            std::istringstream in(line);
            std::vector<std::string> tok;
            for (std::string t; in >> t;) tok.push_back(t);
            if (tok.empty()) continue;
            auto fail = [&](const std::string& why) {
                error = path.string() + ":" + std::to_string(lineNo) + ": " + why;
                return false;
            };
            auto args = [&](size_t n) { return tok.size() == n + 1; };
            std::vector<u32> v(tok.size(), 0);
            const std::string& k = tok[0];
            if (k == "include") {
                if (!args(1)) return fail("include FILE");
                if (!file(path.parent_path() / tok[1])) return false;
                continue;
            }
            if (k == "set") {
                if (!args(2) || !expr(tok[2], v[2])) return fail("set NAME EXPR: " + error);
                names[tok[1]] = v[2];
                continue;
            }
            for (size_t i = 1; i < tok.size(); i++)
                if (!expr(tok[i], v[i])) return fail(error);
            const u32 a = tok.size() > 1 ? v[1] : 0;
            if ((k == "word" || k == "imm" || k == "was" || k == "move") && (a & 3 || !dol.inText(a)))
                return fail("not an instruction address");
            if (k == "word") {
                if (!args(2)) return fail("word ADDR EXPR");
                out.words[a] = v[2];
            } else if (k == "imm") {
                if (!args(2)) return fail("imm ADDR EXPR");
                const u32 w = out.words.count(a) ? out.words[a] : dol.word(a);
                out.words[a] = (w & 0xFFFF0000u) | (v[2] & 0xFFFFu);
            } else if (k == "was") {
                if (!args(2)) return fail("was ADDR WORD");
                if (dol.word(a) != v[2]) {
                    char b[96];
                    std::snprintf(b, sizeof b, "%08X is %08X, not %08X: not the program this file is for", a, dol.word(a), v[2]);
                    return fail(b);
                }
            } else if (k == "move") {
                if (!args(4)) return fail("move ADDR FROM LEN TO");
                if (v[3] == 0) return fail("empty range");
                out.moves[a].push_back({v[2], v[3], v[4]});
            } else {
                return fail("unknown directive '" + k + "'");
            }
        }
        return true;
    }
};

}  // namespace

bool loadOverrides(const std::string& path, const Module& dol, CodeOverrides& out, std::string& error) {
    Parser p{dol, out, {}, {}};
    if (!p.file(path)) return error = p.error, false;
    return true;
}

}  // namespace sa2rc

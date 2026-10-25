// Host replacements registered by name (HLE(name) { ... }), hook resolution, guest calls and the
// printf-style formatting of guest varargs.
#include "runtime/modules.h"
#include "runtime/runtime.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace rt {

namespace {
std::unordered_map<std::string, HleFn>& registry() {
    static std::unordered_map<std::string, HleFn> r;
    return r;
}
std::unordered_map<std::string, u32>& symbols() {
    static std::unordered_map<std::string, u32> s = [] {
        std::unordered_map<std::string, u32> m;
        for (u32 i = 0; i < kDolNameCount; i++)
            if (kDolNames[i].name) m.emplace(kDolNames[i].name, kDolNames[i].addr);
        return m;
    }();
    return s;
}
}  // namespace

HleReg::HleReg(const char* name, HleFn fn) { registry()[name] = fn; }

HleFn hleLookup(const char* name) {
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : it->second;
}

void hleMissing(const char* name, u32 addr, Ctx& c) { fatal("no host implementation of %s (%08X), lr=%08X", name, addr, c.lr); }

void resolveHooks() {
    u32 n = 0;
    for (u32 i = 0; i < kHookCount; i++) {
        if (!kHookTable[i].name) continue;
        g_hooks[i] = hleLookup(kHookTable[i].name);
        n += g_hooks[i] != nullptr;
    }
    // Registered names that match no named function are typos or names the matcher missed.
    for (auto& [name, fn] : registry())
        if (!symbols().count(name)) log("[hle] warning: %s is registered but not named in this executable\n", name.c_str());
    log("[hle] %u of %u named functions replaced by host code\n", n, kHookCount);
}

GuestFn originalOf(const char* name) {
    for (u32 i = 0; i < kHookCount; i++)
        if (kHookTable[i].name && std::strcmp(kHookTable[i].name, name) == 0) return kHookTable[i].orig;
    fatal("originalOf: %s is not a hooked function", name);
}

void callOriginal(Ctx& c, const char* name) {
    // Looked up once per call site (its name literal): a search of every name each call costs the
    // game where hooks run often.
    thread_local std::unordered_map<const char*, GuestFn> t_cache;
    GuestFn& fn = t_cache[name];
    if (!fn) fn = originalOf(name);
    fn(c);
}

u32 guestSymbol(const char* name) {
    auto it = symbols().find(name);
    return it == symbols().end() ? 0 : it->second;
}

const char* guestName(u32 addr) {
    for (u32 i = 0; i < kDolNameCount; i++)
        if (kDolNames[i].addr == addr) return kDolNames[i].name;
    return nullptr;
}

u32 guestCall(Ctx& c, u32 fn, std::initializer_list<u32> args) {
    u32 i = 3;
    for (u32 a : args) c.r[i++] = a;
    const u32 savedLr = c.lr;
    c.call(fn);
    c.lr = savedLr;
    return c.r[3];
}

// printf for guest varargs (PowerPC EABI): integers continue in r3..r10 then the stack overflow
// area at r1+8, doubles in f1..f8 (the caller sets CR1[EQ] when it passes any).
std::string guestFormat(Ctx& c, u32 fmtAddr, int gpr, int fpr) {
    std::string out;
    int stack = 0;
    auto nextInt = [&]() -> u32 { return gpr <= 10 ? c.r[gpr++] : stackArg(c, stack++); };
    auto nextDbl = [&]() -> double {
        if (fpr <= 8) return c.f[fpr++];
        stack = (stack + 1) & ~1;
        const u32 a = c.r[1] + 8 + 4 * stack;
        stack += 2;
        return rdf64(a);
    };
    const char* f = gstr(fmtAddr);
    for (size_t i = 0; f[i]; i++) {
        if (f[i] != '%') {
            out += f[i];
            continue;
        }
        std::string spec = "%";
        i++;
        while (f[i] && std::strchr("-+ #0", f[i])) spec += f[i++];
        if (f[i] == '*') {
            spec += std::to_string(s32(nextInt()));
            i++;
        }
        while (f[i] >= '0' && f[i] <= '9') spec += f[i++];
        if (f[i] == '.') {
            spec += f[i++];
            if (f[i] == '*') {
                spec += std::to_string(s32(nextInt()));
                i++;
            }
            while (f[i] >= '0' && f[i] <= '9') spec += f[i++];
        }
        int longs = 0;
        while (f[i] == 'l' || f[i] == 'h') {
            if (f[i] == 'l') longs++;
            i++;
        }
        const char conv = f[i];
        if (!conv) break;
        char buf[512];
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
            if (longs >= 2) {
                gpr += (gpr & 1) ? 0 : 1;  // 64-bit values take an odd/even register pair
                const u64 hi = nextInt(), lo = nextInt();
                std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), (unsigned long long)(hi << 32 | lo));
            } else {
                std::snprintf(buf, sizeof buf, (spec + conv).c_str(), nextInt());
            }
            out += buf;
            break;
        }
        case 'p': std::snprintf(buf, sizeof buf, "%08X", nextInt()); out += buf; break;
        case 's': {
            const u32 a = nextInt();
            std::snprintf(buf, sizeof buf, (spec + 's').c_str(), a ? gstr(a) : "(null)");
            out += buf;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            std::snprintf(buf, sizeof buf, (spec + conv).c_str(), nextDbl());
            out += buf;
            break;
        case '%': out += '%'; break;
        default: out += spec + conv; break;
        }
    }
    return out;
}

}  // namespace rt

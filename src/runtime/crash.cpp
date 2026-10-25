// Crash reporting: an unhandled exception (access violation, ...) prints the exception, the host
// address (with the module it belongs to) and what every guest thread was doing.
#include "runtime/runtime.h"
#include "runtime/sched.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rt {

void dumpAllThreads();  // src/runtime/cpu.cpp

namespace {
LONG WINAPI onCrash(EXCEPTION_POINTERS* ep) {
    const auto* r = ep->ExceptionRecord;
    const auto pc = reinterpret_cast<uintptr_t>(r->ExceptionAddress);
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(pc), &mod);
    char name[MAX_PATH] = "?";
    if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
    // (through log(): into kirbyairride.log too - a game started by the launcher has no console)
    char what[160] = "";
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        const auto addr = r->ExceptionInformation[1];
        const auto mem = reinterpret_cast<uintptr_t>(g_mem);
        std::snprintf(what, sizeof what, ", %s %llX%s", r->ExceptionInformation[0] ? "writing" : "reading", (unsigned long long)addr,
                      addr >= mem && addr < mem + kHostMemSize ? " (guest memory)" : "");
    }
    log("\nCRASH: exception %08lX at %p (%s+%llX)%s\n", r->ExceptionCode, r->ExceptionAddress, name,
        (unsigned long long)(pc - reinterpret_cast<uintptr_t>(mod)), what);
    // the host call stack (through the exception dispatch into the faulting code), as module+offset
    void* frames[48];
    const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(frames[i]), &m);
        char mn[MAX_PATH] = "?";
        if (m) GetModuleFileNameA(m, mn, MAX_PATH);
        const char* base = std::strrchr(mn, '\\');
        log("  %s+%llX\n", base ? base + 1 : mn, (unsigned long long)(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(m)));
    }
    dumpAllThreads();
    sched::debugDump();
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}
}  // namespace

// abort() (also reached from std::terminate, e.g. an uncaught C++ exception) fast-fails past the
// exception filter; report it the same way, with the host call stack as module+offset.
static void onAbort(int) {
    log("\nABORT on host thread %lu; host stack:\n", GetCurrentThreadId());
    void* frames[48];
    const USHORT n = CaptureStackBackTrace(1, 48, frames, nullptr);
    for (USHORT i = 0; i < n; i++) {
        HMODULE mod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(frames[i]), &mod);
        char name[MAX_PATH] = "?";
        if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
        const char* base = std::strrchr(name, '\\');
        log("  %s+%llX\n", base ? base + 1 : name, (unsigned long long)(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(mod)));
    }
    std::fflush(stdout);
    dumpAllThreads();
    sched::debugDump();
    std::fflush(stdout);
    std::_Exit(3);
}

void installCrashHandler() {
    SetUnhandledExceptionFilter(onCrash);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::signal(SIGABRT, onAbort);
}

// Debug watchpoint (SA2_WATCH=<guest address>[:<length>]): the host pages holding it are made read-only and
// writes to it are reported (the first SA2_WATCH_HITS, default 3) with the guest's registers and stack, then allowed.
// SA2_WATCH_NAN=1: only writes that leave a float NaN (a word with all exponent bits set) are reported.
// Each report names the guest function writing (the recompiled function the host instruction is in). With
// SA2_WATCH_EXCEPT=<lo>-<hi> (guest addresses, hex), writes by functions in that range are not reported - the
// owner's own writes - so a stray writer stands out.
namespace {
uintptr_t s_watchFirst = 0, s_watchEnd = 0;  // host pages
u32 s_watchGuest = 0, s_watchLen = 0;
int s_watchHits = 0, s_watchMax = 3;
bool s_watchNan = false;
u32 s_exceptLo[4] = {}, s_exceptHi[4] = {};  // (up to 4 ranges, comma-separated)
thread_local bool t_rearm = false;   // this thread is single-stepping past a write to the pages
thread_local u32 t_written = 0;      // ... to this guest address in the watched range (0: none)
thread_local u64 t_rip = 0;          // ... by the host instruction there
// The guest function a host address is in: its recompiled function's name f_XXXXXXXX (0: not one).
u32 guestFnAt(u64 rip, char* name, size_t n) {
    static const bool s_sym = [] {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    }();
    if (!s_sym) return 0;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    DWORD64 disp = 0;
    if (!SymFromAddr(GetCurrentProcess(), rip, &disp, sym)) return 0;
    std::snprintf(name, n, "%s", sym->Name);
    const char* f = std::strstr(sym->Name, "f_");
    return f ? u32(std::strtoul(f + 2, nullptr, 16)) : 0;
}
void protectWatch(DWORD prot) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(s_watchFirst), s_watchEnd - s_watchFirst, prot, &old);
}
void reportWatch(u32 guest, u32 value, const char* writer) {
    std::fprintf(stdout, "\n[watch] write to %08X (watching %08X+%u): now %08X, by %s\n", guest, s_watchGuest, s_watchLen, value, writer);
    if (const Ctx* c = sched::currentCtx()) {
        for (int i = 0; i < 32; i++) std::fprintf(stdout, "r%-2d %08X%s", i, c->r[i], i % 8 == 7 ? "\n" : "  ");
        std::fprintf(stdout, "lr %08X  ctr %08X\nguest stack:", c->lr, c->ctr);
        // Back chain: each frame's first word links to the caller's, whose LR save word is at +4.
        u32 sp = c->r[1];
        for (int i = 0; i < 16 && (sp >> 24) == 0x80 + ((sp >> 24) & 1); i++) {
            sp = rd32(sp);
            if (!sp) break;
            const u32 ret = rd32(sp + 4);
            const char* nm = guestName(ret);
            std::fprintf(stdout, " %08X%s%s", ret, nm ? ":" : "", nm ? nm : "");
        }
        std::fprintf(stdout, "\n");
    }
    if (!std::getenv("SA2_WATCH_QUIET")) dumpAllThreads();
    std::fflush(stdout);
}
LONG WINAPI onWatch(EXCEPTION_POINTERS* ep) {
    const auto* r = ep->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && t_rearm) {
        t_rearm = false;
        if (const u32 g = t_written) {  // the write done: what it left
            t_written = 0;
            const u32 v = rd32(g & ~3u);
            const bool nan = v == 0x7FC00000u || v == 0xFFC00000u || v == 0x7FFFFFFFu;  // (what float maths leaves; not ints like -1)
            char writer[260] = "?";
            const u32 fn = guestFnAt(t_rip, writer, sizeof writer);
            static const bool s_guestOnly = std::getenv("SA2_WATCH_GUESTONLY") != nullptr;  // (not host code's writes: memset's)
            bool owner = s_guestOnly && !fn;
            for (int i = 0; i < 4; i++) owner = owner || (fn >= s_exceptLo[i] && fn < s_exceptHi[i]);
            if (!owner && (!s_watchNan || nan) && s_watchHits++ < s_watchMax) reportWatch(g, v, writer);
        }
        protectWatch(PAGE_READONLY);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || !s_watchFirst) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t addr = r->ExceptionInformation[1];
    if (addr < s_watchFirst || addr >= s_watchEnd) return EXCEPTION_CONTINUE_SEARCH;
    const u32 guest = 0x80000000u | u32(addr - reinterpret_cast<uintptr_t>(g_mem));
    t_written = guest - s_watchGuest < s_watchLen ? guest : 0;
    t_rip = ep->ContextRecord->Rip;
    // Let this one write through: unprotect, single-step it, protect again.
    protectWatch(PAGE_READWRITE);
    ep->ContextRecord->EFlags |= 0x100;
    t_rearm = true;
    return EXCEPTION_CONTINUE_EXECUTION;
}
}  // namespace

void installWatch(u32 guest, u32 len) {
    s_watchGuest = guest;
    s_watchLen = len;
    if (const char* m = std::getenv("SA2_WATCH_HITS")) s_watchMax = std::atoi(m);
    s_watchNan = std::getenv("SA2_WATCH_NAN") != nullptr;
    if (const char* e = std::getenv("SA2_WATCH_EXCEPT")) {
        for (int i = 0; i < 4 && *e; i++) {
            char* end = nullptr;
            s_exceptLo[i] = u32(std::strtoul(e, &end, 16));
            if (*end == '-') s_exceptHi[i] = u32(std::strtoul(end + 1, &end, 16));
            e = *end == ',' ? end + 1 : end;
        }
    }
    s_watchFirst = reinterpret_cast<uintptr_t>(hostp(guest)) & ~uintptr_t(0xFFF);
    s_watchEnd = (reinterpret_cast<uintptr_t>(hostp(guest + len - 1)) | 0xFFF) + 1;
    AddVectoredExceptionHandler(1, onWatch);
    protectWatch(PAGE_READONLY);
    log("[watch] guarding %08X\n", guest);
}

}  // namespace rt

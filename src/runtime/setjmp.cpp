// setjmp / longjmp of the game's C library (Kirby Air Ride leaves deep call chains with them).
// Every recompiled function is a host function, so longjmp cannot just restore the guest registers
// and return: that returns into the function that called longjmp, with the stack pointer and saved
// registers of another frame (the game then crashed in whatever ran next). The recompiler puts a
// host setjmp at every call of __setjmp, keyed by the guest jmp_buf (src/recompiler/emit.cpp);
// longjmp lets the game's own code restore the guest registers, then jumps there with the host's
// longjmp, which unwinds the host frames in between.
#include "runtime/runtime.h"

#include <setjmp.h>

#include <memory>
#include <unordered_map>

namespace rt {

namespace {
struct HostJmp {
    jmp_buf buf;
};
thread_local std::unordered_map<u32, std::unique_ptr<HostJmp>> t_jmps;  // by guest jmp_buf address
}  // namespace

void* guestJmpBuf(u32 env) {  // (a jmp_buf: the game module sees the C runtime's type by another name)
    auto& j = t_jmps[env];
    if (!j) j = std::make_unique<HostJmp>();
    return &j->buf;
}

HLE(longjmp) {  // (env, value)
    const u32 env = c.r[3];
    callOriginal(c, "longjmp");  // the guest registers as __setjmp saved them, r3 = the value
    const auto it = t_jmps.find(env);
    if (it == t_jmps.end()) fatal("longjmp to %08X: no setjmp for it on this thread (lr %08X)", env, c.lr);
    longjmp(it->second->buf, 1);
}

}  // namespace rt

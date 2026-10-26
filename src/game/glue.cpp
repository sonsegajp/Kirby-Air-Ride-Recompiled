// The game module's own part: built with the code the recompiler translates from the player's disc into
// kirbyairride_game.dll (KAR_GAME_DLL). It holds the state that code uses all the time and hands the runtime
// the module's tables (src/runtime/modules.h, src/runtime/game.cpp).
//
// The module has no C library (setup builds it with a bundled compiler and linker alone, no SDK): the few
// functions the compiler may call on its own are here. (setjmp comes from the C runtime through the program:
// src/runtime/ctx.h.)
#include "runtime/modules.h"

namespace rt {

u8* g_mem = nullptr;
volatile u32 g_irqPending = 0;
bool g_traceOn = false;
u32 g_relBase[256][32];

namespace {
GameModule s_module;  // filled in when asked for: the counts live in other files (no static constructors here)
}

}  // namespace rt

extern "C" __declspec(dllexport) const rt::GameModule* kirbyairride_game_module() {
    using namespace rt;
    GameModule& m = s_module;
    m.abi = kGameAbi;
    m.dolFuncs = kDolFuncs;
    m.dolFuncCount = kDolFuncCount;
    m.dolNames = kDolNames;
    m.dolNameCount = kDolNameCount;
    m.hookTable = kHookTable;
    m.hookCount = kHookCount;
    m.hooks = g_hooks;
    m.relModules = kRelModules;
    m.relModuleCount = kRelModuleCount;
    m.fnSlotCount = kFnSlotCount;
    m.fnOverride = g_fnOverride;
    m.patchPoints = g_patchPoints;
    m.patchPointAddrs = kPatchPointAddrs;
    m.patchPointCount = kPatchPointCount;
    m.overrideWords = kOverrideWords;
    m.overrideWordCount = kOverrideWordCount;
    m.overrideMoves = kOverrideMoves;
    m.overrideMoveCount = kOverrideMoveCount;
    m.mem = &g_mem;
    m.irqPending = &g_irqPending;
    m.traceOn = &g_traceOn;
    m.relBase = g_relBase;
    return &m;
}

// ---- what the compiler calls on its own ----
using size_t = decltype(sizeof 0);
extern "C" {
int _fltused = 0;  // any floating point in an MSVC-ABI object refers to it

void* memcpy(void* d, const void* s, size_t n) {
    void* r = d;
    asm volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}
void* memmove(void* d, const void* s, size_t n) {
    void* r = d;
    if (static_cast<char*>(d) <= static_cast<const char*>(s) || static_cast<char*>(d) >= static_cast<const char*>(s) + n) {
        asm volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    } else {  // overlapping, destination above: backwards
        d = static_cast<char*>(d) + n - 1;
        s = static_cast<const char*>(s) + n - 1;
        asm volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    }
    return r;
}
void* memset(void* d, int v, size_t n) {
    void* r = d;
    asm volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(v) : "memory");
    return r;
}
int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = static_cast<const unsigned char*>(a);
    const unsigned char* y = static_cast<const unsigned char*>(b);
    for (; n; n--, x++, y++)
        if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}
double fma(double a, double b, double c) { return rt::hostFma(a, b, c); }
double sqrt(double v) {
    double r;
    asm("sqrtsd %1, %0" : "=x"(r) : "x"(v));
    return r;
}
}

// Stack probe for frames over a page (__chkstk, as the MSVC runtime has it): touches each page from the
// caller's stack pointer down by RAX bytes so the guard page grows the stack in order; changes nothing but
// flags.
asm(R"(
    .att_syntax
    .text
    .globl __chkstk
    .p2align 4
__chkstk:
    pushq %rcx
    pushq %rax
    cmpq $0x1000, %rax
    leaq 24(%rsp), %rcx
    jb 1f
2:
    subq $0x1000, %rcx
    testq %rcx, (%rcx)
    subq $0x1000, %rax
    cmpq $0x1000, %rax
    ja 2b
1:
    subq %rax, %rcx
    testq %rcx, (%rcx)
    popq %rax
    popq %rcx
    retq
)");

#pragma once
// Guest threads (OSThread) and interrupts.
//
// Every guest thread runs on its own host thread, but only one executes guest code at a time: the
// scheduler hands the CPU to the highest-priority ready thread, like the console's OS. Switches
// happen when a thread blocks, when it makes a higher-priority thread ready, or after interrupts.
// Interrupts are queued by host threads (VI retrace, alarms, DVD completion...) and run on a
// dedicated interrupt context at the running thread's next safe point (loop back-edge, SDK call)
// with MSR[EE] set, or by the poster itself while the CPU is idle.
//
// The guest-visible OSThread fields (state, suspend, priority, queue links, __OSCurrentThread at
// 0x800000E4) are kept up to date so the game's own mutex/message code runs natively on top.
#include "runtime/runtime.h"

#include <functional>

namespace rt::sched {

// OSThread layout (Dolphin SDK, 2001).
enum : u32 {
    T_CONTEXT = 0x000,
    T_STATE = 0x2C8,   // u16
    T_ATTR = 0x2CA,    // u16
    T_SUSPEND = 0x2CC,
    T_PRIORITY = 0x2D0,
    T_BASE = 0x2D4,
    T_VAL = 0x2D8,
    T_QUEUE = 0x2DC,
    T_LINK = 0x2E0,    // next, prev
    T_JOIN = 0x2E8,    // OSThreadQueue head, tail
    T_MUTEX = 0x2F0,
    T_MUTEXQ = 0x2F4,
    T_ACTIVE = 0x2FC,
    T_STACKBASE = 0x304,
    T_STACKEND = 0x308,
};
enum : u16 { S_READY = 1, S_RUNNING = 2, S_WAITING = 4, S_MORIBUND = 8 };
constexpr u32 kCurrentThread = 0x800000E4;
constexpr u32 kCurrentContext = 0x800000D4;

struct Thread;
struct ThreadExit {};

// The calling host thread (running __start) becomes the guest thread at `guest`.
void adoptBootThread(u32 guest, Ctx& ctx);
Thread* current();
bool inInterrupt();
Ctx* currentCtx();  // guest context of the calling host thread (debugging)
// Whether the calling host thread may run guest code now (it owns the guest CPU, or serves
// interrupts); otherwise the guest thread it runs and the owner's, for the report.
bool ownsCpu(u32* self = nullptr, u32* owner = nullptr);

// OS thread API (guest addresses).
bool create(u32 guest, u32 entry, u32 arg, u32 stackTop, u32 stackSize, s32 prio, u32 attr);
s32 resume(u32 guest);
s32 suspend(u32 guest);
void sleepOn(u32 queue);
void wakeup(u32 queue);
void yield();
[[noreturn]] void exitThread(u32 val);
void cancel(u32 guest);
bool join(u32 guest, u32* val);
void setPriority(u32 guest, s32 prio);
void disableScheduler();
void enableScheduler();

// Interrupts.
void postIrq(std::function<void(Ctx&)> fn);
void serviceInterrupts(Ctx& c);
// Runs `fn` with the guest CPU released (for long host waits); interrupts and other threads go on.
void withCpuReleased(const std::function<void()>& fn);

// Alarms (OSAlarm at `alarm`): fire `handler(alarm, context)` at `when` (time base ticks), then
// every `period` ticks when nonzero.
void setAlarm(u32 alarm, u64 when, u64 period, u32 handler);
void cancelAlarm(u32 alarm);

void debugDump();  // scheduler state for the watchdog

}  // namespace rt::sched

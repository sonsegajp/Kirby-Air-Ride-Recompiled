// Waiting for a moment in host time, precisely. The game counts video retraces to pace itself
// (f_80117DF8 spins until the count passes last frame's); retraces that come late and then two
// together read as a frame the game missed. A plain sleep wakes at the system timer's next tick
// (up to 15.6 ms late); a high-resolution waitable timer wakes within about half a millisecond,
// and a short spin after it lands on the moment.
#include "runtime/runtime.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <immintrin.h>
#include <timeapi.h>

#include <chrono>

#pragma comment(lib, "winmm.lib")

namespace rt {

// At startup: a 1 ms system timer for everything else that sleeps, and no power throttling -
// Windows slows processes it considers in the background (no visible window, or occluded) and
// ignores their timer requests, and the game must keep its 60 Hz either way.
void hostTimingInit() {
    timeBeginPeriod(1);
    PROCESS_POWER_THROTTLING_STATE s{};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    s.StateMask = 0;  // controlled, and off
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof s);
}

// For the thread that keeps the retrace beat (it runs for microseconds a frame).
void hostThreadTimeCritical() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL); }

void sleepUntil(std::chrono::steady_clock::time_point t, bool spin) {
    using namespace std::chrono;
    thread_local HANDLE s_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const auto margin = spin ? microseconds(400) : microseconds(0);
    for (;;) {
        const auto left = t - steady_clock::now();
        if (left <= nanoseconds(0)) return;
        if (left > margin) {
            if (!s_timer) {
                Sleep(DWORD(duration_cast<milliseconds>(left - margin).count()));
                if (!spin) return;
                continue;
            }
            LARGE_INTEGER due;
            due.QuadPart = -std::max<long long>(1, duration_cast<nanoseconds>(left - margin).count() / 100);  // relative, 100 ns
            if (SetWaitableTimerEx(s_timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(s_timer, INFINITE);
            if (!spin) return;
            continue;
        }
        _mm_pause();
    }
}

}  // namespace rt

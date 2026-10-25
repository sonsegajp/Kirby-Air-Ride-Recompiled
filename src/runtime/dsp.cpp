// The audio DSP's side of the mailbox, implemented natively (the DSP itself is never emulated).
// The SDK's DSP task code and the AX library run recompiled and talk to these registers:
//   0xCC005000/02  CPU->DSP mail (hi/lo; writing lo sends it), bit 15 of hi = "mail not taken yet"
//   0xCC005004/06  DSP->CPU mail (bit 15 of hi = valid; reading lo takes it)
//   0xCC00500A     DSP control: RES 0x1, AIDINT 0x8, ARINT 0x20, DSPINT 0x80 (interrupt bits W1C)
// First the boot ROM protocol (0x8071FEED, then the 0x80F3xxxx upload parameters), then the AX
// microcode protocol: DSP_INIT after boot, command lists (0xBABE0000|size + address) answered with
// DSP_YIELD, RESUME answered with DSP_RESUME. Command lists are mixed in src/runtime/ax.cpp
// (silent until then).
#include "runtime/runtime.h"
#include "runtime/sched.h"

#include <deque>
#include <mutex>

namespace rt {

u32 mmioGet16(u32 off);
void mmioSet16(u32 off, u32 v);
void axCommandList(u32 addr, u32 size);  // src/runtime/ax.cpp

namespace {
enum : u32 {
    DSP_INIT = 0xDCD10000,
    DSP_RESUME = 0xDCD10001,
    DSP_YIELD = 0xDCD10002,
    DSP_DONE = 0xDCD10003,
    MAIL_RESUME = 0xCDD10000,
    MAIL_NEW_UCODE = 0xCDD10001,
    MAIL_RESET = 0xCDD10002,
    MAIL_CONTINUE = 0xCDD10003,
};
enum : u32 { IRQ_DSP_DSP = 7, DSPCR_DSPINT = 0x80 };

std::recursive_mutex s_lock;  // interrupt handlers may read the mailbox while it is held
std::deque<u32> s_toCpu;
enum class Mode { Rom, Ucode } s_mode = Mode::Rom;
u32 s_romParam = 0;
u32 s_cpuHi = 0;  // high half of the mail being sent
u32 s_cmdlistSize = 0;
bool s_nextIsCmdlist = false;
bool s_greeted = false;  // boot ROM sent 0x8071FEED since the last reset

void raiseDspInterrupt() {
    raiseInterrupt(IRQ_DSP_DSP, [] { mmioSet16(0x500A, mmioGet16(0x500A) | DSPCR_DSPINT); });
}

void push(u32 mail, bool interrupt) {
    s_toCpu.push_back(mail);
    if (interrupt) raiseDspInterrupt();
}

void romMail(u32 mail) {
    if (!s_romParam) {
        if ((mail & 0xFFFF0000) == 0x80F30000) s_romParam = mail;
        return;
    }
    const u32 param = s_romParam;
    s_romParam = 0;
    if (param == 0x80F3D001) {  // start PC: the microcode now runs
        s_mode = Mode::Ucode;
        log("[dsp] microcode started (entry %04X)\n", mail & 0xFFFF);
        push(DSP_INIT, true);
    }
}

void ucodeMail(u32 mail) {
    if (s_nextIsCmdlist) {
        s_nextIsCmdlist = false;
        axCommandList(mail, s_cmdlistSize);
        push(DSP_YIELD, true);
        return;
    }
    if ((mail & 0xFFFF0000) == 0xBABE0000) {
        s_cmdlistSize = mail & 0xFFFF;
        s_nextIsCmdlist = true;
    } else if (mail == MAIL_RESUME) {
        push(DSP_RESUME, true);
    } else if (mail == MAIL_RESET || mail == MAIL_NEW_UCODE) {
        s_mode = Mode::Rom;
        s_greeted = false;
    } else if (mail != MAIL_CONTINUE) {
        log("[dsp] unknown mail %08X\n", mail);
    }
}
}  // namespace

// Register access from mmio.cpp for 0xCC005000-0xCC00500B.
u32 dspRead16(u32 off) {
    std::lock_guard g(s_lock);
    switch (off) {
    case 0x5000: return mmioGet16(0x5000) & 0x7FFF;  // the mail is always taken at once
    case 0x5004:
        // The boot ROM greets whoever waits for it first after a reset (__OSInitAudioSystem, which
        // would have seen it on the console, is not run).
        if (s_toCpu.empty() && s_mode == Mode::Rom && !s_greeted) {
            s_greeted = true;
            s_toCpu.push_back(0x8071FEED);
        }
        return s_toCpu.empty() ? 0 : (0x8000 | (s_toCpu.front() >> 16));
    case 0x5006: {
        if (s_toCpu.empty()) return 0;
        const u32 lo = s_toCpu.front() & 0xFFFF;
        s_toCpu.pop_front();
        return lo;
    }
    case 0x500A: return mmioGet16(0x500A) & ~0x0801u;  // reset and boot-init done
    default: return mmioGet16(off);
    }
}

void dspWrite16(u32 off, u32 v) {
    std::lock_guard g(s_lock);
    switch (off) {
    case 0x5000:
        s_cpuHi = v & 0xFFFF;
        mmioSet16(0x5000, v | 0x8000);
        return;
    case 0x5002: {
        const u32 mail = s_cpuHi << 16 | (v & 0xFFFF);
        mmioSet16(0x5002, v);
        mmioSet16(0x5000, s_cpuHi & 0x7FFF);
        if (s_mode == Mode::Rom) romMail(mail);
        else ucodeMail(mail);
        return;
    }
    case 0x500A: {
        const u32 old = mmioGet16(0x500A);
        u32 nv = (old & 0x00A8 & ~v) | (v & ~0x00A8u);  // interrupt bits are write-1-to-clear
        if (v & 1) {                                    // reset: the boot ROM greets the CPU
            s_toCpu.clear();
            s_mode = Mode::Rom;
            s_romParam = 0;
            s_greeted = false;
            nv &= ~1u;
        }
        mmioSet16(0x500A, nv);
        return;
    }
    default: mmioSet16(off, v); return;
    }
}

}  // namespace rt

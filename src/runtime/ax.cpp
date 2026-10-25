// The AX audio microcode's work, done natively (the DSP itself is never emulated). The AX library
// runs recompiled and sends a command list every 5 ms audio frame (src/runtime/dsp.cpp hands it
// here). Each list mixes one frame of 160 samples at 32 kHz:
//   0000 SETUP addr        initial values/ramps of the nine mix buses (main, aux A, aux B; L/R/S)
//   0007 SET_LR addr       main L/R from a 32-bit buffer
//   0002 PB_ADDR addr      first voice parameter block (linked through PB.next)
//   0003 PROCESS           mix every running voice, 1 ms at a time, and write its state back
//   0004/0005 MIX_AUXA/B w r  aux bus out to the CPU (effects), the CPU's result added to main
//   0006 UPLOAD_LRS addr   main L/R/S out
//   0009 MIX_AUXB_NOWRITE r  aux B result added to main
//   000D MORE addr size    continue with another list
//   000E OUTPUT s out      main surround out (32-bit) and the stereo frame for the AI (16-bit R,L)
//   000F END
// and the 2003 microcode's (Billy Hatcher; its list is built by f_801E61F4):
//   0010 MIX_AUXB_LR w r   Pro Logic II aux B      0011 SET_OPPOSITE_LR addr   surround mode
//   0012 COMPRESSOR 8000 000A table                0013 Pro Logic II aux A (not seen in stereo)
// Voices read their samples from ARAM through the accelerator (4-bit ADPCM, 16-bit or 8-bit PCM),
// are resampled, shaped by the volume envelope and mixed with per-bus volume ramps. The voice's
// position, decoder state and running flag go back to the parameter block: the game's streaming
// (ADX) refills its ARAM ring buffer by watching them.
#include "runtime/runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace rt {

u8* aramPtr(u32 a);  // src/runtime/hle_hw.cpp
u64 gxFrames();     // src/runtime/gx.cpp

namespace {
constexpr u32 kSpms = 32;           // samples per millisecond
constexpr u32 kFrame = kSpms * 5;   // one 5 ms frame

enum Bus { MAIN_L, MAIN_R, MAIN_S, AUXA_L, AUXA_R, AUXA_S, AUXB_L, AUXB_R, AUXB_S, kBuses };
s32 s_bus[kBuses][kFrame];

// Parameter block (AXPB), in 16-bit words.
enum : u32 {
    PB_NEXT = 0,
    PB_SRC_TYPE = 4,
    PB_MIXER_CONTROL = 6,
    PB_RUNNING = 7,
    PB_IS_STREAM = 8,
    PB_MIXER = 9,     // volume/delta pairs: L, R, AL, AR, BL, BR, BS, S, AS
    PB_UPDATES = 34,  // 5 per-ms update counts, then the address of the (word, value) pairs
    PB_DPOP = 41,     // L, AL, BL, R, AR, BR, S, AS, BS
    PB_VOL = 50,      // envelope: volume, delta
    PB_LOOPING = 55,
    PB_FORMAT = 56,
    PB_LOOP_ADDR = 57,
    PB_END_ADDR = 59,
    PB_CUR_ADDR = 61,
    PB_COEFS = 63,  // 16 ADPCM coefficients, gain, pred/scale, yn1, yn2
    PB_PRED_SCALE = 80,
    PB_YN1 = 81,
    PB_YN2 = 82,
    PB_RATIO = 83,  // 16.16 resampling ratio
    PB_FRAC = 85,
    PB_LAST = 86,   // the resampler's last 4 input samples
    PB_LOOP_PRED_SCALE = 90,
    PB_LOOP_YN1 = 91,
    PB_LOOP_YN2 = 92,
    kPbWords = 96,
};

struct Voice {
    u16 w[kPbWords];
    u32 get32(u32 i) const { return u32(w[i]) << 16 | w[i + 1]; }
    void set32(u32 i, u32 v) { w[i] = u16(v >> 16), w[i + 1] = u16(v); }
};

// The accelerator: reads one sample at the voice's current address and handles the loop/end.
struct Accelerator {
    Voice& v;
    u32 cur, end, loop, format;
    s32 yn1, yn2;
    u32 predScale;
    bool stopped = false;

    explicit Accelerator(Voice& voice)
        : v(voice), cur(voice.get32(PB_CUR_ADDR)), end(voice.get32(PB_END_ADDR)), loop(voice.get32(PB_LOOP_ADDR)),
          format(voice.w[PB_FORMAT]), yn1(s16(voice.w[PB_YN1])), yn2(s16(voice.w[PB_YN2])),
          predScale(voice.w[PB_PRED_SCALE]) {}

    s16 next() {
        if (stopped) return 0;
        s32 val = 0;
        u32 step;
        switch (format) {
        case 0x00: {  // 4-bit ADPCM: 8-byte frames of a pred/scale byte and 14 nibbles
            if ((cur & 15) == 0) {
                predScale = *aramPtr((cur & ~15u) >> 1);
                cur += 2;
            }
            step = (end & 15) == 0 ? 1 : (end & 15) == 1 ? 0 : 2;
            const s32 scale = 1 << (predScale & 15);
            const u32 ci = (predScale >> 4) & 7;
            const s32 c1 = s16(v.w[PB_COEFS + ci * 2]), c2 = s16(v.w[PB_COEFS + ci * 2 + 1]);
            const u8 byte = *aramPtr(cur >> 1);
            s32 nib = (cur & 1) ? (byte & 15) : (byte >> 4);
            if (nib >= 8) nib -= 16;
            val = std::clamp(scale * nib + ((0x400 + c1 * yn1 + c2 * yn2) >> 11), -0x7FFF, 0x7FFF);
            break;
        }
        case 0x0A: {  // 16-bit PCM (word addresses)
            const u8* p = aramPtr(cur * 2);
            val = s16(p[0] << 8 | p[1]);
            step = 2;
            break;
        }
        case 0x19:  // 8-bit PCM
            val = s8(*aramPtr(cur)) << 8;
            step = 1;
            break;
        default: stopped = true; return 0;
        }
        yn2 = yn1;
        yn1 = val;
        cur++;
        if (cur == end + step - 1) {
            cur = loop;
            if (v.w[PB_LOOPING]) {
                predScale = v.w[PB_LOOP_PRED_SCALE];
                if (!v.w[PB_IS_STREAM]) {
                    yn1 = s16(v.w[PB_LOOP_YN1]);
                    yn2 = s16(v.w[PB_LOOP_YN2]);
                }
            } else {
                v.w[PB_RUNNING] = 0;  // a one-shot voice ends
                stopped = true;
            }
        }
        return s16(val);
    }

    void store() {
        v.set32(PB_CUR_ADDR, cur);
        v.w[PB_YN1] = u16(yn1);
        v.w[PB_YN2] = u16(yn2);
        v.w[PB_PRED_SCALE] = u16(predScale);
    }
};

// One millisecond of input, resampled to 32 kHz: 0 polyphase (its coefficient table lives in the
// DSP ROM; linear stands in), 1 linear, 2 none.
void readInput(Voice& v, s16* out, u32 count) {
    Accelerator acc(v);
    s16* last = reinterpret_cast<s16*>(&v.w[PB_LAST]);
    const u32 ratio = v.get32(PB_RATIO);
    if (v.w[PB_SRC_TYPE] == 2) {
        for (u32 i = 0; i < count; i++) out[i] = acc.next();
        for (u32 i = 0; i < 4; i++) last[i] = out[count - 4 + i];
    } else {
        u32 pos = v.w[PB_FRAC];
        for (u32 i = 0; i < count; i++) {
            for (u32 n = pos >> 16; n; n--) {
                last[0] = last[1], last[1] = last[2], last[2] = last[3];
                last[3] = acc.next();
            }
            pos &= 0xFFFF;
            out[i] = s16(last[2] + ((s32(last[3]) - last[2]) * s32(pos) >> 16));
            pos += ratio;
        }
        v.w[PB_FRAC] = u16(pos & 0xFFFF);
        // Whole samples still owed carry over in the position (ratios above 1).
        for (u32 n = pos >> 16; n; n--) {
            last[0] = last[1], last[1] = last[2], last[2] = last[3];
            last[3] = acc.next();
        }
    }
    acc.store();
}

void mixInto(s32* bus, const s16* in, u32 count, u16* vol, u16* dpop, bool ramp) {
    u16 volume = vol[0];
    const s16 delta = ramp ? s16(vol[1]) : 0;
    for (u32 i = 0; i < count; i++) {
        const s32 s = std::clamp(s32((s64(in[i]) * volume) >> 15), -32767, 32767);
        bus[i] += s;
        volume = u16(volume + delta);
        *dpop = u16(s);
    }
    vol[0] = volume;
}

void processVoice(Voice& v, u32 offset) {
    s16 in[kSpms];
    readInput(v, in, kSpms);
    // Volume envelope.
    u16 env = v.w[PB_VOL];
    const s16 envDelta = s16(v.w[PB_VOL + 1]);
    for (u32 i = 0; i < kSpms; i++) {
        in[i] = s16(std::clamp((s32(in[i]) * s32(env)) >> 15, -32767, 32767));
        env = u16(env + envDelta);
    }
    v.w[PB_VOL] = env;
    // Mixer control (the 2003 AX microcode Billy Hatcher uses - SA2B's older one had main L/R always
    // on): a bit per bus and one per ramp, as the game's own mixer sets them (f_802379CC):
    //   0001 L  0002 R  0004 S  0008 main ramps | 0010 AL  0020 AR  0040 AL/AR ramp  0080 AS  0100 AS ramp
    //   0200 BL  0400 BR  0800 BL/BR ramp  1000 BS  2000 BS ramp | 4000 Dolby Pro Logic II
    // Mixer words: volume/delta pairs L, R, AL, AR, BL, BR, BS, S, AS; dpop: L, AL, BL, R, AR, BR, S, AS, BS.
    struct Route {
        u32 bit, ramp;
        Bus bus;
        u32 mix, dpop;
    };
    static constexpr Route kRoutes[] = {
        {0x0001, 0x0008, MAIN_L, 0, 0},  {0x0002, 0x0008, MAIN_R, 2, 3},  {0x0004, 0x0008, MAIN_S, 14, 6},
        {0x0010, 0x0040, AUXA_L, 4, 1},  {0x0020, 0x0040, AUXA_R, 6, 4},  {0x0080, 0x0100, AUXA_S, 16, 7},
        {0x0200, 0x0800, AUXB_L, 8, 2},  {0x0400, 0x0800, AUXB_R, 10, 5}, {0x1000, 0x2000, AUXB_S, 12, 8},
    };
    const u32 mc = v.w[PB_MIXER_CONTROL];
    for (const Route& r : kRoutes)
        if (mc & r.bit) mixInto(s_bus[r.bus] + offset, in, kSpms, &v.w[PB_MIXER + r.mix], &v.w[PB_DPOP + r.dpop], mc & r.ramp);
}

void processVoices(u32 pb) {
    static u32 s_frame = 0;
    static const u32 s_logEvery = std::getenv("SA2_AX_VOICES") ? u32(std::atoi(std::getenv("SA2_AX_VOICES"))) : 0;
    const bool logNow = s_logEvery && ++s_frame % s_logEvery == 0;
    for (u32 n = 0; pb && n < 256; n++) {
        Voice v;
        for (u32 i = 0; i < kPbWords; i++) v.w[i] = u16(rd16(pb + i * 2));
        if (logNow && v.w[PB_RUNNING]) {
            char mix[160] = "";
            for (u32 i = 0; i < 18; i++) std::snprintf(mix + i * 5, 6, "%04X ", v.w[PB_MIXER + i]);
            const u32 cur = v.get32(PB_CUR_ADDR);
            log("[ax] voice %08X fmt %02X stream %u mc %04X ratio %08X env %04X loop %08X end %08X cur %08X "
                "data %04X %04X %04X %04X upd %u/%u/%u/%u/%u mixer %s\n",
                pb, v.w[PB_FORMAT], v.w[PB_IS_STREAM], v.w[PB_MIXER_CONTROL], v.get32(PB_RATIO), v.w[PB_VOL],
                v.get32(PB_LOOP_ADDR), v.get32(PB_END_ADDR), cur, rd16(0) * 0 + (aramPtr(cur * 2)[0] << 8 | aramPtr(cur * 2)[1]),
                aramPtr(cur * 2 + 2)[0] << 8 | aramPtr(cur * 2 + 3)[0], aramPtr(cur * 2 + 64)[0] << 8 | aramPtr(cur * 2 + 65)[0],
                aramPtr(cur * 2 + 512)[0] << 8 | aramPtr(cur * 2 + 513)[0], v.w[PB_UPDATES], v.w[PB_UPDATES + 1],
                v.w[PB_UPDATES + 2], v.w[PB_UPDATES + 3], v.w[PB_UPDATES + 4], mix);
        }
        // SA2_AX_STARTS: each voice start, with the game frame it came on (stream timing checks).
        static const bool s_starts = std::getenv("SA2_AX_STARTS") != nullptr;
        if (s_starts) {
            static std::unordered_map<u32, u16> s_wasRunning;
            u16& was = s_wasRunning[pb];
            if (v.w[PB_RUNNING] && !was)
                log("[ax] frame %llu: voice %08X starts (fmt %02X stream %u ratio %08X cur %08X loop %08X)\n",
                    (unsigned long long)gxFrames(), pb, v.w[PB_FORMAT], v.w[PB_IS_STREAM], v.get32(PB_RATIO),
                    v.get32(PB_CUR_ADDR), v.get32(PB_LOOP_ADDR));
            was = v.w[PB_RUNNING];
        }
        const u32 updates = v.get32(PB_UPDATES + 5);
        u32 first = 0;
        for (u32 ms = 0; ms < 5; ms++) {
            // Parameter updates the CPU scheduled for this millisecond: (word index, value) pairs.
            const u32 count = v.w[PB_UPDATES + ms];
            for (u32 k = first; k < first + count; k++) {
                const u32 idx = rd16(updates + k * 4);
                if (idx < kPbWords) v.w[idx] = u16(rd16(updates + k * 4 + 2));
            }
            first += count;
            if (v.w[PB_RUNNING] == 1) processVoice(v, ms * kSpms);
        }
        for (u32 i = 0; i < kPbWords; i++) wr16(pb + i * 2, v.w[i]);
        pb = v.get32(PB_NEXT);
    }
}

void readBuses(u32 addr, std::initializer_list<Bus> buses, bool add) {
    for (Bus b : buses)
        for (u32 i = 0; i < kFrame; i++, addr += 4) s_bus[b][i] = (add ? s_bus[b][i] : 0) + s32(rd32(addr));
}
void writeBuses(u32 addr, std::initializer_list<Bus> buses) {
    for (Bus b : buses)
        for (u32 i = 0; i < kFrame; i++, addr += 4) wr32(addr, u32(s_bus[b][i]));
}
}  // namespace

void axCommandList(u32 addr, u32 size) {
    u32 pb = 0;
    const u32 limit = addr + size * 2;
    auto arg32 = [&] {
        const u32 v = u32(rd16(addr)) << 16 | rd16(addr + 2);
        addr += 4;
        return v;
    };
    for (u32 guard = 0; guard < 4096; guard++) {
        if (addr >= limit) return;
        const u32 cmd = rd16(addr);
        addr += 2;
        switch (cmd) {
        case 0x00: {  // SETUP: per bus, a 32-bit start value and a 16-bit per-sample delta
            const u32 init = arg32();
            for (u32 b = 0; b < kBuses; b++) {
                s32 val = s32(u32(rd16(init + b * 6)) << 16 | rd16(init + b * 6 + 2));
                const s16 delta = s16(rd16(init + b * 6 + 4));
                for (u32 i = 0; i < kFrame; i++, val += delta) s_bus[b][i] = val;
            }
            break;
        }
        case 0x02: pb = arg32(); break;
        case 0x03: processVoices(pb); break;
        case 0x04:
        case 0x05: {
            const u32 out = arg32(), in = arg32();
            const bool a = cmd == 0x04;
            if (out) writeBuses(out, a ? std::initializer_list<Bus>{AUXA_L, AUXA_R, AUXA_S} : std::initializer_list<Bus>{AUXB_L, AUXB_R, AUXB_S});
            readBuses(in, {MAIN_L, MAIN_R, MAIN_S}, true);
            break;
        }
        case 0x06: writeBuses(arg32(), {MAIN_L, MAIN_R, MAIN_S}); break;
        case 0x07: {  // SET_LR
            u32 src = arg32();
            for (u32 i = 0; i < kFrame; i++, src += 4) {
                s_bus[MAIN_L][i] = s_bus[MAIN_R][i] = s32(rd32(src));
                s_bus[MAIN_S][i] = 0;
            }
            break;
        }
        case 0x09: readBuses(arg32(), {MAIN_L, MAIN_R, MAIN_S}, true); break;
        case 0x0A: addr += 4; break;  // compressor table address
        case 0x10: {  // MIX_AUXB_LR (Pro Logic II): aux B L/R out, the CPU's result added to main L/R
            const u32 out = arg32(), in = arg32();
            writeBuses(out, {AUXB_L, AUXB_R});
            readBuses(in, {MAIN_L, MAIN_R}, true);
            break;
        }
        case 0x11: {  // SET_OPPOSITE_LR (surround mode): main L from a 32-bit buffer, R its opposite
            u32 src = arg32();
            for (u32 i = 0; i < kFrame; i++, src += 4) {
                s_bus[MAIN_L][i] = s32(rd32(src));
                s_bus[MAIN_R][i] = -s_bus[MAIN_L][i];
                s_bus[MAIN_S][i] = 0;
            }
            break;
        }
        case 0x12:  // COMPRESSOR: gain 8000, 000A, table address (the limiter on main: not applied)
            addr += 8;
            break;
        case 0x0D: {                  // MORE
            const u32 next = arg32(), words = rd16(addr);
            axCommandList(next, words);
            return;
        }
        case 0x0E: {  // OUTPUT: surround (32-bit), then the AI frame, 16-bit right/left pairs
            const u32 sur = arg32(), out = arg32();
            writeBuses(sur, {MAIN_S});
            for (u32 i = 0; i < kFrame; i++) {
                wr16(out + i * 4, u16(std::clamp(s_bus[MAIN_R][i], -32767, 32767)));
                wr16(out + i * 4 + 2, u16(std::clamp(s_bus[MAIN_L][i], -32767, 32767)));
            }
            break;
        }
        case 0x0F: return;
        default: {
            static int s_warned = 0;
            if (s_warned++ < 5) log("[ax] unknown command %04X; rest of the list skipped\n", cmd);
            return;
        }
        }
    }
}

}  // namespace rt

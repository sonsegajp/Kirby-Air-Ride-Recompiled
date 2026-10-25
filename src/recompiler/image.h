#pragma once
// Guest program images: main.dol and the REL modules, as the recompiler sees them.
//
// Every module has its own 32-bit code address space. For main.dol that is the real guest address.
// A REL is position independent until OSLink places it, so its addresses are "section-relative":
// (section << 24) | offset. Branch arithmetic works the same in both spaces, and a REL relocation
// target in the same module becomes such an address too. Code that loads a REL-relative address at
// runtime adds the section's load base (src/runtime/modules.cpp) instead.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sa2rc {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

inline u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
inline u16 be16(const u8* p) { return u16(p[0] << 8 | p[1]); }

// Big-endian PPC field extraction: bits a..b inclusive, bit 0 = MSB.
inline u32 bits(u32 w, int a, int b) { return (w >> (31 - b)) & ((1u << (b - a + 1)) - 1); }
inline s32 sext16(u32 v) { return s32(s16(u16(v))); }
inline s32 sext(u32 v, int n) { return s32(v << (32 - n)) >> (32 - n); }

// REL relocation types (PowerPC EABI subset + Dolphin's control entries).
enum : u8 {
    R_PPC_NONE = 0,
    R_PPC_ADDR32 = 1,
    R_PPC_ADDR24 = 2,
    R_PPC_ADDR16 = 3,
    R_PPC_ADDR16_LO = 4,
    R_PPC_ADDR16_HI = 5,
    R_PPC_ADDR16_HA = 6,
    R_PPC_ADDR14 = 7,
    R_PPC_REL24 = 10,
    R_PPC_REL14 = 11,
    R_DOLPHIN_NOP = 201,
    R_DOLPHIN_SECTION = 202,
    R_DOLPHIN_END = 203,
};

// A relocation as it applies to one location of a module.
struct Reloc {
    u8 type = 0;
    u32 module = 0;   // target module id (0 = main.dol: `target` is an absolute address)
    u32 target = 0;   // target address in that module's code address space
};

struct Section {
    u32 addr = 0;     // DOL: guest address; REL: index << 24
    u32 size = 0;
    u32 fileOff = 0;  // 0 for bss
    bool exec = false;
};

struct Module {
    u32 id = 0;                  // 0 = main.dol, else the REL module id
    std::string name;            // file name ("main.dol", "stg13D.rel")
    std::vector<u8> file;
    std::vector<Section> sections;
    // REL header fields
    u32 bssSize = 0;
    u32 prolog = 0, epilog = 0, unresolved = 0;  // code addresses (0 = none)
    // DOL
    u32 entry = 0;
    u32 bssAddr = 0, bssLen = 0;
    // Relocations of this module, keyed by the address of the 32-bit word they patch
    // (ADDR16_LO/HI/HA patch the low halfword of an instruction; they are keyed by the instruction).
    std::unordered_map<u32, std::vector<Reloc>> relocs;

    bool isDol() const { return id == 0; }
    const Section* sectionAt(u32 addr) const;
    bool inText(u32 addr) const;
    bool inData(u32 addr) const;  // initialised non-exec section
    u32 word(u32 addr) const;     // 0 when outside the initialised image
    const std::vector<Reloc>* relocsAt(u32 addr) const {
        auto it = relocs.find(addr);
        return it == relocs.end() ? nullptr : &it->second;
    }
    // The relocation of a given kind at an instruction, if any.
    const Reloc* relocOf(u32 addr, bool half) const;
};

bool loadDol(const std::vector<u8>& data, Module& out, std::string& error);
bool loadRel(const std::vector<u8>& data, const std::string& name, Module& out, std::string& error);

inline u32 relSecAddr(u32 sec) { return sec << 24; }
inline u32 relSecOf(u32 addr) { return addr >> 24; }

}  // namespace sa2rc

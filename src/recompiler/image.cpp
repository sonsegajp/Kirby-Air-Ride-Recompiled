#include "recompiler/image.h"

#include <cstdio>

namespace sa2rc {

namespace {
bool isHalfType(u8 t) { return t == R_PPC_ADDR16 || t == R_PPC_ADDR16_LO || t == R_PPC_ADDR16_HI || t == R_PPC_ADDR16_HA; }
}  // namespace

const Section* Module::sectionAt(u32 addr) const {
    for (const auto& s : sections)
        if (s.size && addr >= s.addr && addr - s.addr < s.size) return &s;
    return nullptr;
}

bool Module::inText(u32 addr) const {
    const Section* s = sectionAt(addr);
    return s && s->exec && s->fileOff;
}

bool Module::inData(u32 addr) const {
    const Section* s = sectionAt(addr);
    return s && !s->exec && s->fileOff;
}

u32 Module::word(u32 addr) const {
    const Section* s = sectionAt(addr);
    if (!s || !s->fileOff) return 0;
    const u32 off = s->fileOff + (addr - s->addr);
    if (off + 4 > file.size()) return 0;
    return be32(&file[off]);
}

const Reloc* Module::relocOf(u32 addr, bool half) const {
    const auto* v = relocsAt(addr);
    if (!v) return nullptr;
    for (const auto& r : *v)
        if (isHalfType(r.type) == half) return &r;
    return nullptr;
}

bool loadDol(const std::vector<u8>& d, Module& m, std::string& error) {
    if (d.size() < 0x100) {
        error = "main.dol is too small";
        return false;
    }
    m = {};
    m.id = 0;
    m.name = "main.dol";
    m.file = d;
    for (int i = 0; i < 18; i++) {
        Section s;
        s.fileOff = be32(&d[i * 4]);
        s.addr = be32(&d[0x48 + i * 4]);
        s.size = be32(&d[0x90 + i * 4]);
        s.exec = i < 7;
        if (!s.size) continue;
        if (s.fileOff + u64(s.size) > d.size()) {
            error = "main.dol section runs past the end of the file";
            return false;
        }
        m.sections.push_back(s);
    }
    m.bssAddr = be32(&d[0xD8]);
    m.bssLen = be32(&d[0xDC]);
    m.entry = be32(&d[0xE0]);
    return true;
}

bool loadRel(const std::vector<u8>& d, const std::string& name, Module& m, std::string& error) {
    if (d.size() < 0x40) {
        error = name + ": too small for a REL";
        return false;
    }
    m = {};
    m.name = name;
    m.file = d;
    m.id = be32(&d[0]);
    const u32 nsec = be32(&d[0x0C]);
    const u32 secOff = be32(&d[0x10]);
    m.bssSize = be32(&d[0x20]);
    const u32 impOff = be32(&d[0x28]);
    const u32 impSize = be32(&d[0x2C]);
    const u8 prologSec = d[0x30], epilogSec = d[0x31], unresSec = d[0x32];
    const u32 prolog = be32(&d[0x34]), epilog = be32(&d[0x38]), unres = be32(&d[0x3C]);
    if (secOff + u64(nsec) * 8 > d.size() || impOff + u64(impSize) > d.size()) {
        error = name + ": header points outside the file";
        return false;
    }
    m.sections.resize(nsec);
    for (u32 i = 0; i < nsec; i++) {
        Section& s = m.sections[i];
        const u32 o = be32(&d[secOff + i * 8]);
        s.size = be32(&d[secOff + i * 8 + 4]);
        s.exec = o & 1;
        s.fileOff = o & ~1u;
        s.addr = relSecAddr(i);
        if (s.fileOff && s.fileOff + u64(s.size) > d.size()) {
            error = name + ": section runs past the end of the file";
            return false;
        }
    }
    // Section 0 is always the null section, so a code address of 0 can mean "none".
    m.prolog = prologSec ? relSecAddr(prologSec) + prolog : 0;
    m.epilog = epilogSec ? relSecAddr(epilogSec) + epilog : 0;
    m.unresolved = unresSec ? relSecAddr(unresSec) + unres : 0;

    for (u32 i = 0; i < impSize / 8; i++) {
        const u32 tmod = be32(&d[impOff + i * 8]);
        u32 o = be32(&d[impOff + i * 8 + 4]);
        u32 sec = 0, pos = 0;
        for (;;) {
            if (o + 8 > d.size()) {
                error = name + ": relocation list runs past the end of the file";
                return false;
            }
            const u16 delta = be16(&d[o]);
            const u8 type = d[o + 2];
            const u8 tsec = d[o + 3];
            const u32 addend = be32(&d[o + 4]);
            o += 8;
            if (type == R_DOLPHIN_END) break;
            if (type == R_DOLPHIN_SECTION) {
                sec = tsec;
                pos = 0;
                continue;
            }
            pos += delta;
            if (type == R_DOLPHIN_NOP || type == R_PPC_NONE) continue;
            Reloc r;
            r.type = type;
            r.module = tmod;
            r.target = tmod == 0 ? addend : relSecAddr(tsec) + addend;
            const u32 at = relSecAddr(sec) + (isHalfType(type) ? (pos & ~3u) : pos);
            m.relocs[at].push_back(r);
        }
    }
    return true;
}

}  // namespace sa2rc

# Decodes a frame of the GX bridge's output stream (build/gxframe.bin, SA2_GXDUMP_FRAME=N) into a
# readable command list: BP/CP/XF register loads, GX_AURORA extension commands and draws.
import struct, sys

BP_NAMES = {0x00: 'GENMODE', 0x20: 'SCISSOR_TL', 0x21: 'SCISSOR_BR', 0x40: 'ZMODE', 0x41: 'BLENDMODE', 0x42: 'CONSTALPHA',
            0x43: 'PE_CTRL', 0x45: 'PE_DONE', 0x47: 'TOKEN', 0x48: 'TOKEN_INT', 0x49: 'COPY_SRC_TL', 0x4A: 'COPY_SRC_WH',
            0x4B: 'COPY_DEST', 0x4D: 'COPY_STRIDE', 0x4F: 'CLEAR_AR', 0x50: 'CLEAR_GB', 0x51: 'CLEAR_Z', 0x52: 'COPY',
            0x59: 'SCISSOR_OFS', 0x64: 'TLUT_SRC', 0x65: 'TLUT_LOAD', 0xF3: 'ALPHA_COMPARE', 0xFE: 'BP_MASK'}
AUR = {0x10: 'ARRAYBASE', 0x30: 'LOAD_TEXOBJ', 0x31: 'LOAD_TLUT', 0x35: 'COPY_SRC', 0x36: 'COPY_DST', 0x37: 'COPY_DEST'}
PRIM = {0x80: 'QUADS', 0x88: 'QUADS2', 0x90: 'TRIANGLES', 0x98: 'TRISTRIP', 0xA0: 'TRIFAN', 0xA8: 'LINES', 0xB0: 'LINESTRIP', 0xB8: 'POINTS'}

d = open(sys.argv[1] if len(sys.argv) > 1 else 'build/gxframe.bin', 'rb').read()
vcd = [0, 0]; vat = {}
COMP = [1, 1, 2, 2, 4, 0, 0, 0]; CLR = [2, 3, 4, 2, 3, 4, 0, 0]
def vsize(fmt):
    lo, hi = vcd; a, b, c = vat.get(('A', fmt), 0), vat.get(('B', fmt), 0), vat.get(('C', fmt), 0)
    s = sum((lo >> i) & 1 for i in range(9))
    def attr(t, direct): return direct if t == 1 else 1 if t == 2 else 2 if t == 3 else 0
    s += attr((lo >> 9) & 3, (3 if a & 1 else 2) * COMP[(a >> 1) & 7])
    t = (lo >> 11) & 3
    if t: s += ((9 if (a >> 9) & 1 else 3) * COMP[(a >> 10) & 7]) if t == 1 else (1 if t == 2 else 2) * (3 if ((a >> 9) & 1 and a >> 31) else 1)
    s += attr((lo >> 13) & 3, CLR[(a >> 14) & 7]) + attr((lo >> 15) & 3, CLR[(a >> 18) & 7])
    tex = [((a >> 21) & 1, (a >> 22) & 7), (b & 1, (b >> 1) & 7), ((b >> 9) & 1, (b >> 10) & 7), ((b >> 18) & 1, (b >> 19) & 7),
           ((b >> 27) & 1, (b >> 28) & 7), ((c >> 5) & 1, (c >> 6) & 7), ((c >> 14) & 1, (c >> 15) & 7), ((c >> 23) & 1, (c >> 24) & 7)]
    for i in range(8): s += attr((hi >> (2 * i)) & 3, (2 if tex[i][0] else 1) * COMP[tex[i][1]])
    return s
i = 0; draws = 0
while i < len(d):
    op = d[i]
    if op in (0x00, 0x44, 0x48): print('%05x %s' % (i, {0: 'NOP', 0x44: 'METRICS', 0x48: 'INVAL_VC'}[op])); i += 1
    elif op == 0x08:
        reg, v = d[i + 1], struct.unpack_from('>I', d, i + 2)[0]
        if reg == 0x50: vcd[0] = v
        elif reg == 0x60: vcd[1] = v
        elif 0x70 <= reg < 0x78: vat[('A', reg & 7)] = v
        elif 0x80 <= reg < 0x88: vat[('B', reg & 7)] = v
        elif 0x90 <= reg < 0x98: vat[('C', reg & 7)] = v
        print('%05x CP  %02X = %08X' % (i, reg, v)); i += 6
    elif op == 0x10:
        n = struct.unpack_from('>H', d, i + 1)[0] + 1; addr = struct.unpack_from('>H', d, i + 3)[0]
        vals = struct.unpack_from('>%dI' % n, d, i + 5)
        if addr >= 0x1000 and n == 1: fl = ['%08X' % vals[0]]
        else: fl = ['%.3f' % struct.unpack('>f', struct.pack('>I', x))[0] for x in vals[:12]]
        print('%05x XF  %04X x%d %s' % (i, addr, n, ' '.join(fl))); i += 5 + 4 * n
    elif op in (0x20, 0x28, 0x30, 0x38): print('%05x XF_INDX %02X %08X' % (i, op, struct.unpack_from('>I', d, i + 1)[0])); i += 5
    elif op == 0x61:
        v = struct.unpack_from('>I', d, i + 1)[0]; r = v >> 24
        print('%05x BP  %02X = %06X %s' % (i, r, v & 0xFFFFFF, BP_NAMES.get(r, ''))); i += 5
    elif op == 0x50:
        sub = struct.unpack_from('>H', d, i + 1)[0]; i += 3
        name = AUR.get(sub & 0xFFF0 if (sub & 0xFFF0) == 0x10 else sub, '%04X' % sub)
        if (sub & 0xFFF0) == 0x10: ptr, size, le = struct.unpack_from('>QIB', d, i); i += 13; print('      AURORA ARRAYBASE %d ptr %X size %X' % (sub & 15, ptr, size))
        elif sub == 0x30:
            m, ptr, w, h, fmt, tl, mip, oid, ver = struct.unpack_from('>BQIIIIBII', d, i); i += 34
            print('      AURORA TEXOBJ map %d %dx%d fmt %X tlut %d ptr %X' % (m, w, h, fmt, tl, ptr))
        elif sub == 0x31: s, ptr, fmt, n, a1, a2 = struct.unpack_from('>BQIHII', d, i); i += 23; print('      AURORA TLUT slot %d fmt %d n %d' % (s, fmt, n))
        elif sub == 0x35: print('      AURORA COPY_SRC', struct.unpack_from('>4I', d, i)); i += 16
        elif sub == 0x36: print('      AURORA COPY_DST', struct.unpack_from('>3IB', d, i)); i += 13
        elif sub == 0x37: print('      AURORA COPY_DEST %X' % struct.unpack_from('>Q', d, i)); i += 8
        elif sub == 0x60: print('      AURORA WIDE_SEGMENT %d' % d[i]); i += 1
        elif sub == 0x61: print('      AURORA WIDE_2D %d' % d[i]); i += 1
        else: print('      AURORA %04X ?' % sub); break
    elif 0x80 <= op < 0xC0:
        n = struct.unpack_from('>H', d, i + 1)[0]; vs = vsize(op & 7); draws += 1
        print('%05x DRAW %s fmt %d count %d vsize %d (VCD %08X/%08X)' % (i, PRIM.get(op & 0xF8, '?'), op & 7, n, vs, vcd[0], vcd[1]))
        i += 3 + n * vs
    else: print('%05x ??? %02X' % (i, op)); break
print('draws:', draws)

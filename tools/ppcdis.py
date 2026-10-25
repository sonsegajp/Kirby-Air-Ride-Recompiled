# Dev helper: disassemble main.dol with names. Usage: ppcdis.py <addr> [count] | dis.py --calls <addr>
import struct, sys, capstone
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
d = open('orig/main.dol','rb').read()
offs = struct.unpack_from('>18I', d, 0); addrs = struct.unpack_from('>18I', d, 0x48); sizes = struct.unpack_from('>18I', d, 0x90)
names = {int(l.split()[0],16): l.split()[1] for l in open('config/GKYE01/names.txt') if not l.startswith('#')}
ends = {}
for l in open('build/funcs.tsv'):
    m, s, e, al = l.split('\t')
    if m == '0': ends[int(s,16)] = int(e,16)
def word(a):
    for i in range(18):
        if sizes[i] and addrs[i] <= a < addrs[i]+sizes[i]: return struct.unpack_from('>I', d, offs[i] + a - addrs[i])[0]
def line(p):
    w = word(p); ins = list(md.disasm(struct.pack('>I', w), p))
    t = (ins[0].mnemonic + ' ' + ins[0].op_str) if ins else '.word 0x%08x' % w
    if ins and ins[0].mnemonic in ('bl', 'b') and ins[0].op_str.startswith('0x'):
        tgt = int(ins[0].op_str, 16); t += '   ; ' + names.get(tgt, 'f_%08X' % tgt)
    return '%08x  %08x  %s' % (p, w, t)
args = sys.argv[1:]
if args[0] == '--calls':
    a = int(args[1], 16); e = ends.get(a, a + 0x400)
    print(names.get(a, 'f_%08X' % a), hex(a), '-', hex(e))
    for p in range(a, e, 4):
        w = word(p)
        if (w >> 26) == 18 and (w & 1): print(line(p))
else:
    a = int(args[0], 16); n = int(args[1]) if len(args) > 1 else (ends.get(a, a + 0x80) - a) // 4
    print(names.get(a, 'f_%08X' % a))
    for p in range(a, a + 4 * n, 4): print(line(p))

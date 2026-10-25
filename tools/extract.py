# Dev helper: extract the user's own disc in full into orig/ (gitignored): every file under
# orig/disc/ in the disc's own tree, main.dol, and every code module in orig/rel/ - the .rel files,
# the .str module-name tables, and the modules the game keeps PRS-compressed (CHAOMAIN.prs and the
# other Chao World modules), decompressed to .rel. The shipped program reads the disc itself
# (src/disc).
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prs

import discimage  # .iso/.gcm, .ciso or .rvz

iso = sys.argv[1]; out = sys.argv[2] if len(sys.argv) > 2 else 'orig'
img = discimage.open_disc(iso)
def rd(off, n): return img.read(off, n)
dol_off, fst_off, fst_size = struct.unpack('>III', rd(0x420, 12))
fst = rd(fst_off, fst_size)
n = struct.unpack('>I', fst[8:12])[0]
strtab = fst[n*12:]
def nm(o): return strtab[o:strtab.index(b'\0', o)].decode('latin1')
files = []
def walk(i, end, path):
    while i < end:
        a, off, ln = struct.unpack('>III', fst[i*12:i*12+12])
        name = nm(a & 0xFFFFFF)
        if a >> 24: walk(i+1, ln, path+name+'/'); i = ln
        else: files.append((path+name, off, ln)); i += 1
walk(1, n, '')
os.makedirs(out + '/rel', exist_ok=True)
# DOL size = max(section offset + size) over the 18 sections
hdr = rd(dol_off, 0x100)
offs = struct.unpack('>18I', hdr[0:72]); sizes = struct.unpack('>18I', hdr[0x90:0xD8])
dol_size = max(o + s for o, s in zip(offs, sizes) if s)
open(out + '/main.dol', 'wb').write(rd(dol_off, dol_size))


def is_rel(h):
    """A REL module header: id, null list links, a sane section table, version 1-3."""
    if len(h) < 0x40: return False
    mid, nxt, prv, nsec, secoff, _, _, ver = struct.unpack('>8I', h[:0x20])
    return 0 < mid < 0x1000 and nxt == 0 and prv == 0 and 1 < nsec < 64 and secoff in (0x40, 0x48, 0x4C) and 1 <= ver <= 3


cnt = packed = 0
for p, off, ln in files:
    data = rd(off, ln)
    dst = os.path.join(out, 'disc', *p.split('/'))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if not os.path.exists(dst) or os.path.getsize(dst) != ln:
        open(dst, 'wb').write(data)
    low = p.lower()
    if low.endswith(('.rel', '.str')):
        open(out + '/rel/' + p.replace('/', '_'), 'wb').write(data); cnt += 1
    elif low.endswith('.prs'):
        try:
            if is_rel(prs.decompress(data, 0x40)):
                rel = prs.decompress(data)
                name = p.replace('/', '_')[:-4] + '_prs.rel'  # ChaoStgLobby.prs and .rel are different modules
                open(out + '/rel/' + name, 'wb').write(rel); cnt += 1; packed += 1
                print('compressed module %d: %s -> rel/%s (%d bytes)' % (struct.unpack('>I', rel[:4])[0], p, name, len(rel)))
        except IndexError:
            pass  # not PRS after all
with open(out + '/files.tsv', 'w') as m:
    for p, off, ln in files: m.write('%s\t%x\t%d\n' % (p, off, ln))
print('main.dol', dol_size, 'bytes;', cnt, 'modules/name tables (%d PRS-compressed);' % packed, len(files), 'files extracted to', out + '/disc')

# Names the game's Dolphin SDK / runtime functions by matching them against Nintendo's own Wind Waker
# linker map (shipped on the WW disc) and WW's main.dol. Address-dependent fields are masked so
# the same library code matches at any address. Output: config/<GAME>/names.txt ("ADDR NAME LIB").
import re, struct, sys, hashlib, collections

WW_MAP = 'C:/Users/hyper/wwrecomp/disc/maps_framework.map'
WW_DOL = 'C:/Users/hyper/wwrecomp/main.dol'
GAME = 'GKYE01'  # Kirby Air Ride (USA)
SA_DOL = 'orig/main.dol'
FUNCS = 'build/funcs.tsv'
SDK_LIBS = {'os', 'exi', 'si', 'db', 'mtx', 'dvd', 'vi', 'pad', 'ai', 'ar', 'ax', 'axfx', 'mix', 'dsp', 'card',
            'gx', 'gd', 'base', 'Runtime.PPCEABI.H', 'MSL_C.PPCEABI.bare.H', 'TRK_MINNOW_DOLPHIN',
            'OdemuExi2', 'amcstubs', 'odenotstub', 'thp', 'gba', 'hio', 'perf', 'texPalette', 'fileCache', 'dtk', 'arq'}

class Dol:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        o = struct.unpack_from('>18I', self.d, 0); a = struct.unpack_from('>18I', self.d, 0x48); s = struct.unpack_from('>18I', self.d, 0x90)
        self.secs = [(a[i], s[i], o[i]) for i in range(18) if s[i]]
    def words(self, addr, n):
        for a, s, o in self.secs:
            if a <= addr and addr + n * 4 <= a + s:
                return list(struct.unpack_from('>%dI' % n, self.d, o + addr - a))
        return None

def masked(ws):
    out = []; lis = set()
    for w in ws:
        op = w >> 26; d = (w >> 21) & 31; a = (w >> 16) & 31
        if op == 18: w &= 0xFC000003
        elif op == 15:
            if a == 0: lis.add(d)
            w &= 0xFFFF0000
        elif op in (14, 24, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55):
            if a in (2, 13) or a in lis: w &= 0xFFFF0000
        out.append(w)
    return out

def h(ws): return hashlib.blake2b(struct.pack('>%dI' % len(ws), *ws), digest_size=12).digest()

# --- WW reference functions from Nintendo's map ---
ref = []  # (name, lib, tu, addr, size)
section = None
for line in open(WW_MAP, encoding='latin1'):
    if 'section layout' in line: section = line.split()[0]; continue
    if section != '.text': continue
    m = re.match(r'\s+[0-9a-f]{8} ([0-9a-f]{6}) ([0-9a-f]{8})\s+\d+ (\S+)\s+(\S+)\s*(\S*)', line)
    if not m: continue
    size, va, name, f1, f2 = m.groups()
    if name.startswith('.') or not int(size, 16): continue
    lib, tu = (f1[:-2], f2) if f1.endswith('.a') else ('', f1)
    if lib in SDK_LIBS: ref.append((name, lib, tu, int(va, 16), int(size, 16)))
ww = Dol(WW_DOL); sa = Dol(SA_DOL)
# More references: our earlier recomps (SA2B's 2001 SDK, Billy Hatcher's 2003 one) - their programs
# and the names already worked out for them (matched + hand-identified), sizes from their function
# lists. Each reference entry carries which program it is in.
ref = [r + (ww,) for r in ref]
known = {r[0] for r in ref}
for root, gid in (('C:/Users/hyper/SA2BRecomp', 'GSNE8P'), ('C:/Users/hyper/BillyHatcherRecomp', 'GEZE8P')):
    other = Dol(root + '/orig/main.dol')
    other_end = {}
    for l in open(root + '/build/funcs.tsv'):
        m_, st, en, al = l.split('	')
        if m_ == '0' and al.strip() == '0': other_end[int(st, 16)] = int(en, 16)
    for line in open(root + '/config/' + gid + '/names.txt'):
        if line.startswith('#'): continue
        a, n, lib, tu = line.split()[:4]
        a = int(a, 16)
        if n not in known and a in other_end:
            ref.append((n, lib, tu, a, other_end[a] - a, other)); known.add(n)
print('reference SDK/runtime functions:', len(ref), 'libs:', sorted(collections.Counter(r[1] for r in ref).items()))

# --- SA2B functions from our discovery ---
funcs = []
for l in open(FUNCS):
    m_, s, e, al = l.split('\t')
    if m_ == '0' and al.strip() == '0': funcs.append((int(s, 16), int(e, 16)))
funcs.sort()
byhash = collections.defaultdict(list)
for s, e in funcs:
    ws = sa.words(s, (e - s) // 4)
    if ws: byhash[h(masked(ws))].append(s)

# Tier B mask: every memory/addi immediate too (struct layouts such as __GXData moved between SDKs).
def loose(ws):
    out = []
    for w in masked(ws):
        op = w >> 26
        if op in (14, 24, 25, 28, 29, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 60, 61):
            w &= 0xFFFF0000
        out.append(w)
    return out
def opseq(ws):  # opcode + extended opcode + registers, for similarity
    return [w & 0xFC0007FE if (w >> 26) in (4, 19, 31, 59, 63) else w & 0xFFFF0000 for w in loose(ws)]
byloose = collections.defaultdict(list)
sa_words = {}
for s_, e_ in funcs:
    ws = sa.words(s_, (e_ - s_) // 4)
    if ws:
        sa_words[s_] = ws
        byloose[h(loose(ws))].append(s_)
ref_words = {}
for r in ref:
    ws = r[5].words(r[3], r[4] // 4)
    if ws: ref_words[r] = ws

names = {}; tier = {}; amb = 0
used = set()
for r in ref:
    ws = ref_words.get(r)
    if not ws: continue
    hit = [x for x in byhash.get(h(masked(ws)), []) if x not in used]
    if len(hit) == 1:
        names[hit[0]] = r; used.add(hit[0]); tier[hit[0]] = 'A'
    elif len(hit) > 1: amb += 1
for r in ref:
    if r in names.values() or r not in ref_words: continue
    hit = [x for x in byloose.get(h(loose(ref_words[r])), []) if x not in used]
    if len(hit) == 1:
        names[hit[0]] = r; used.add(hit[0]); tier[hit[0]] = 'B'

# Tier C: inside each translation unit the link order is fixed; anchor on matches and align the
# functions between anchors by similarity.
import difflib
def sim(a, b):
    if not a or not b: return 0.0
    if max(len(a), len(b)) > 3 * min(len(a), len(b)): return 0.0
    return difflib.SequenceMatcher(None, opseq(a), opseq(b), autojunk=False).ratio()
sa_order = [s_ for s_, e_ in funcs]
sa_index = {a: i for i, a in enumerate(sa_order)}
by_tu = collections.OrderedDict()
for r in ref: by_tu.setdefault((id(r[5]), r[1], r[2]), []).append(r)
rev = {r: a for a, r in names.items()}
added = 0
for tu, rs in by_tu.items():
    anchors = [(i, sa_index[rev[r]]) for i, r in enumerate(rs) if r in rev]
    if not anchors: continue
    # extend before the first and after the last anchor, then fill gaps
    segs = []
    fi, fs = anchors[0]
    segs.append((0, fi, fs - fi, fs))
    for (i0, s0), (i1, s1) in zip(anchors, anchors[1:]):
        if s1 > s0: segs.append((i0 + 1, i1, s0 + 1, s1))
    li, ls = anchors[-1]
    segs.append((li + 1, len(rs), ls + 1, ls + 1 + (len(rs) - li - 1)))
    for ri0, ri1, si0, si1 in segs:
        ref_seg = rs[ri0:ri1]; sa_seg = [sa_order[k] for k in range(max(si0, 0), min(si1, len(sa_order)))]
        if not ref_seg or not sa_seg: continue
        # DP alignment maximising summed similarity (only pairs above 0.6 count)
        n, m = len(ref_seg), len(sa_seg)
        if n * m > 4000: continue
        S = [[sim(ref_words.get(ref_seg[i]), sa_words.get(sa_seg[j])) for j in range(m)] for i in range(n)]
        dp = [[0.0] * (m + 1) for _ in range(n + 1)]
        for i in range(n - 1, -1, -1):
            for j in range(m - 1, -1, -1):
                take = S[i][j] + dp[i + 1][j + 1] if S[i][j] >= 0.6 else -1
                dp[i][j] = max(take, dp[i + 1][j], dp[i][j + 1])
        i = j = 0
        while i < n and j < m:
            if S[i][j] >= 0.6 and abs(dp[i][j] - (S[i][j] + dp[i + 1][j + 1])) < 1e-9:
                a = sa_seg[j]
                if a not in names and ref_seg[i] not in rev:
                    names[a] = ref_seg[i]; rev[ref_seg[i]] = a; tier[a] = 'C%.2f' % S[i][j]; added += 1
                i += 1; j += 1
            elif dp[i][j] == dp[i + 1][j]: i += 1
            else: j += 1
# SA2B links the SDK in two blocks (base..MSL, then gba/OdemuExi2/card at the end of .text). Short
# functions match by accident anywhere (getters, one-call wrappers inside CRI's middleware), so
# outside those blocks only long exact matches are trusted. Hand-identified names come from
# config/<GAME>/names_manual.txt.
# The blocks are where long exact matches cluster (the libraries are linked in runs).
long_hits = sorted(a for a in names if tier[a] == 'A' and names[a][4] >= 64)
SDK_BLOCKS = []
for a in long_hits:
    if SDK_BLOCKS and a - SDK_BLOCKS[-1][1] < 0x4000: SDK_BLOCKS[-1][1] = a + names[a][4]
    else: SDK_BLOCKS.append([a, a + names[a][4], 0])
    SDK_BLOCKS[-1][2] += 1
SDK_BLOCKS = [(lo - 0x400, hi + 0x400) for lo, hi, k in SDK_BLOCKS if k >= 8]
print('SDK blocks:', ' '.join('%08X-%08X' % b for b in SDK_BLOCKS))
def in_sdk(a): return any(lo <= a < hi for lo, hi in SDK_BLOCKS)
for a in list(names):
    if not in_sdk(a) and not (tier[a] == 'A' and names[a][4] >= 64):
        del names[a]; del tier[a]
per_lib = collections.Counter(names[a][1] for a in names)
per_lib_tot = collections.Counter(r[1] for r in ref)
print('matches: A %d, B %d, C %d (ambiguous exact %d)' % (sum(1 for t in tier.values() if t == 'A'), sum(1 for t in tier.values() if t == 'B'), added, amb))
for lib in sorted(per_lib_tot): print('  %-24s %4d / %4d' % (lib, per_lib[lib], per_lib_tot[lib]))
manual = {}
for line in open('config/' + GAME + '/names_manual.txt'):
    if line.startswith('#') or not line.strip(): continue
    a, n, lib, tu, why = line.split()[:5]
    manual[int(a, 16)] = (n, lib, tu)
for a, (n, lib, tu) in manual.items():
    names[a] = (n, lib, tu, 0, 0); tier[a] = 'M'
with open('config/' + GAME + '/names.txt', 'w') as f:
    f.write('# Kirby Air Ride (' + GAME + ') main.dol: address name library translation-unit match-tier\n')
    f.write('# Generated by tools/sdkmatch.py from Nintendo\'s Wind Waker map (A exact, B loose, C aligned+similarity).\n')
    for a in sorted(names):
        n, lib, tu = names[a][0], names[a][1], names[a][2]
        f.write('%08X %s %s %s %s\n' % (a, n, lib, tu, tier[a]))

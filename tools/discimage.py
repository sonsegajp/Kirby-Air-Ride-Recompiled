# Reads GameCube disc images as the plain disc they hold: .iso/.gcm (as is), .ciso (block map) and
# .rvz/.wia (Dolphin's compressed formats: chunk groups compressed with Zstandard - or stored,
# bzip2 or LZMA for WIA - and RVZ's packed padding, regenerated from its seeds). Format details
# from Dolphin's docs/WiaAndRvz.md. Usage: open_disc(path) -> object with .read(offset, size),
# .size.
import bz2
import lzma
import struct
import threading


class IsoImage:
    def __init__(self, f):
        self.f = f
        f.seek(0, 2)
        self.size = f.tell()

    def read(self, off, size):
        self.f.seek(off)
        return self.f.read(size)


class CisoImage:
    """'CISO', block size (LE), a map of 0x7FF8 used/unused bytes; used blocks follow in order."""

    def __init__(self, f):
        self.f = f
        hdr = f.read(0x8000)
        self.block = struct.unpack('<I', hdr[4:8])[0]
        self.index = []
        n = 0
        for used in hdr[8:0x8000]:
            self.index.append(n if used == 1 else None)
            n += used == 1
        last = max((i for i, x in enumerate(self.index) if x is not None), default=-1)
        self.size = (last + 1) * self.block

    def read(self, off, size):
        out = bytearray()
        while size > 0:
            b, inner = divmod(off, self.block)
            n = min(size, self.block - inner)
            k = self.index[b] if b < len(self.index) else None
            if k is None:
                out += bytes(n)
            else:
                self.f.seek(0x8000 + k * self.block + inner)
                out += self.f.read(n)
            off += n
            size -= n
        return bytes(out)


def _lfg_bytes(seed, offset, count):
    """GC/Wii padding: lagged Fibonacci (xor, j=32, k=521) from a 17-word seed, starting `offset`
    bytes into its 32 KiB block."""
    buf = list(struct.unpack('>17I', seed)) + [0] * (521 - 17)
    for i in range(17, 521):
        buf[i] = ((buf[i - 17] << 23) & 0xFFFFFFFF) ^ (buf[i - 16] >> 9) ^ buf[i - 1]

    def forward():
        for i in range(32):
            buf[i] ^= buf[i + 521 - 32]
        for i in range(32, 521):
            buf[i] ^= buf[i - 32]

    for _ in range(4):
        forward()
    pos = offset % 0x8000
    while pos >= 521 * 4:
        forward()
        pos -= 521 * 4
    out = bytearray()
    while len(out) < count:
        block = b''.join(struct.pack('>4B', (x >> 24) & 255, (x >> 18) & 255, (x >> 8) & 255, x & 255) for x in buf)
        take = min(count - len(out), len(block) - pos)
        out += block[pos:pos + take]
        pos += take
        if pos == len(block):
            forward()
            pos = 0
    return bytes(out)


class RvzImage:
    def __init__(self, f):
        self.f = f
        h = f.read(0x48)
        self.rvz = h[:4] == b'RVZ\x01'
        disc_size = struct.unpack('>I', h[12:16])[0]
        self.size = struct.unpack('>Q', h[0x24:0x2C])[0]
        d = f.read(disc_size)
        disc_type, self.comp, _, self.chunk = struct.unpack('>IIiI', d[:16])
        self.dhead = d[16:0x90]
        n_part = struct.unpack('>I', d[0x90:0x94])[0]
        if n_part:
            raise ValueError('Wii discs (partitions) are not supported')
        o = 0x90 + 4 + 4 + 8 + 20
        n_raw, raw_off, raw_size, n_groups, group_off, group_size = struct.unpack('>IQIIQI', d[o:o + 32])
        self.comp_data = d[o + 33:o + 33 + d[o + 32]]
        raw = self._decompress(self._file(raw_off, raw_size), n_raw * 24)
        self.raws = []
        for i in range(n_raw):
            off, size, gi, ng = struct.unpack('>QQII', raw[i * 24:i * 24 + 24])
            skipped = off % 0x8000  # (rounded down to a 32 KiB boundary, see the format notes)
            self.raws.append((off - skipped, size + skipped, gi, ng))
        gsz = 12 if self.rvz else 8
        groups = self._decompress(self._file(group_off, group_size), n_groups * gsz)
        self.groups = [struct.unpack('>III', groups[i * 12:i * 12 + 12]) if self.rvz else struct.unpack('>II', groups[i * 8:i * 8 + 8]) + (0,)
                       for i in range(n_groups)]
        self.cache = {}
        self.lock = threading.Lock()

    def _file(self, off, size):
        self.f.seek(off)
        return self.f.read(size)

    def _decompress(self, data, size_hint):
        if self.comp == 0:
            return data
        if self.comp == 5:
            import zstandard
            return zstandard.ZstdDecompressor().decompress(data, max_output_size=max(size_hint, 1 << 20))
        if self.comp == 2:
            return bz2.decompress(data)
        if self.comp in (3, 4):
            props = self.comp_data
            if self.comp == 3:
                d = props[0]
                filt = {'id': lzma.FILTER_LZMA1, 'lc': d % 9, 'lp': (d // 9) % 5, 'pb': d // 45,
                        'dict_size': struct.unpack('<I', props[1:5])[0]}
            else:
                p = props[0]
                filt = {'id': lzma.FILTER_LZMA2, 'dict_size': 0xFFFFFFFF if p == 40 else (2 | (p & 1)) << (p // 2 + 11)}
            return lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[filt]).decompress(data)
        raise ValueError('unsupported compression %d' % self.comp)

    def _group(self, gi, disc_off, length):
        if gi in self.cache:
            return self.cache[gi]
        data_off4, data_size, packed = self.groups[gi]
        compressed = bool(data_size & 0x80000000) or not self.rvz
        size = data_size & 0x7FFFFFFF
        if size == 0:
            out = bytes(length)
        else:
            raw = self._file(data_off4 * 4, size)
            if compressed:
                raw = self._decompress(raw, packed or length)
            if self.rvz and packed:
                out = bytearray()
                p = 0
                while p < len(raw):
                    n = struct.unpack('>I', raw[p:p + 4])[0]
                    p += 4
                    if n & 0x80000000:
                        n &= 0x7FFFFFFF
                        out += _lfg_bytes(raw[p:p + 68], disc_off + len(out), n)
                        p += 68
                    else:
                        out += raw[p:p + n]
                        p += n
                out = bytes(out)
            else:
                out = raw
        if len(self.cache) > 8:
            self.cache.clear()
        self.cache[gi] = out
        return out

    def read(self, off, size):
        with self.lock:
            out = bytearray()
            while size > 0:
                piece = None
                for start, length, gi, ng in self.raws:
                    if start <= off < start + length:
                        k, inner = divmod(off - start, self.chunk)
                        glen = min(self.chunk, length - k * self.chunk)
                        n = min(size, glen - inner)
                        g = self._group(gi + k, start + k * self.chunk, glen)
                        piece = g[inner:inner + n]
                        break
                if piece is None:  # not covered by any raw data: zeros
                    n = size
                    nxt = min((s for s, _, _, _ in self.raws if s > off), default=off + size)
                    n = min(size, max(1, nxt - off))
                    piece = bytes(n)
                if off < 0x80:  # the first 0x80 bytes come from the header
                    head = self.dhead[off:off + len(piece)]
                    piece = head + piece[len(head):]
                out += piece
                off += len(piece)
                size -= len(piece)
            return bytes(out)


def open_disc(path):
    f = open(path, 'rb')
    magic = f.read(4)
    f.seek(0)
    if magic == b'CISO':
        return CisoImage(f)
    if magic in (b'RVZ\x01', b'WIA\x01'):
        return RvzImage(f)
    return IsoImage(f)

# Builds a formatted memory card image (raw, as a real card and Dolphin's .raw files) holding the
# given .gci saves, for the emulated slot A (src/runtime/exi.cpp). The format is the SDK's
# CARDFormat for a console whose card flash ID is all zero (the runtime's SRAM), format time 0.
#   mkcard.py <out.raw> <save.gci>... [--mbit 16]
import struct, sys

BLOCK = 0x2000


def checksum(data):
    s = si = 0
    for (w,) in struct.iter_unpack('>H', data):
        s = (s + w) & 0xFFFF
        si = (si + (~w & 0xFFFF)) & 0xFFFF
    return (0 if s == 0xFFFF else s), (0 if si == 0xFFFF else si)


def s64(v):
    v &= (1 << 64) - 1
    return v - (1 << 64) if v >> 63 else v


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    mbit = int(sys.argv[sys.argv.index('--mbit') + 1]) if '--mbit' in sys.argv else 16
    if '--mbit' in sys.argv:
        args.remove(str(mbit))
    out, gcis = args[0], args[1:]
    nblocks = mbit * 1024 * 1024 // 8 // BLOCK
    card = bytearray(b'\xff' * (nblocks * BLOCK))

    # ID block: serial from the flash ID (zeros) and the format time, checksummed (508 bytes)
    idb = bytearray(b'\xff' * 512)
    rand = time = 0
    for i in range(12):
        rand = s64(rand * 1103515245 + 12345) >> 16
        idb[i] = rand & 0xFF
        rand = (s64(rand * 1103515245 + 12345) >> 16) & 0x7FFF
    idb[12:20] = struct.pack('>q', time)
    idb[20:24] = struct.pack('>I', 0)  # SRAM counter bias
    idb[24:28] = struct.pack('>I', 0)  # language
    idb[28:32] = struct.pack('>I', 0)  # VI DTV status
    idb[0x20:0x26] = struct.pack('>HHH', 0, mbit, 0)  # device id, size (Mbit), encoding (ANSI)
    idb[0x1FC:0x200] = struct.pack('>HH', *checksum(bytes(idb[:0x1FC])))
    card[0:512] = idb

    # files: directory entries (the .gci header) and blocks from 5 on, chained in the FAT
    dir_ = bytearray(b'\xff' * BLOCK)
    fat = [0] * (BLOCK // 2)
    nextblock = 5
    for n, path in enumerate(gcis):
        g = open(path, 'rb').read()
        hdr, data = bytearray(g[:0x40]), g[0x40:]
        length = struct.unpack('>H', hdr[0x38:0x3A])[0]
        assert len(data) == length * BLOCK, path
        hdr[0x36:0x38] = struct.pack('>H', nextblock)
        dir_[n * 0x40:(n + 1) * 0x40] = hdr
        for b in range(length):
            blk = nextblock + b
            card[blk * BLOCK:(blk + 1) * BLOCK] = data[b * BLOCK:(b + 1) * BLOCK]
            fat[blk] = blk + 1 if b + 1 < length else 0xFFFF
        print('%s: %s blocks %d-%d' % (path, hdr[8:40].split(b'\0')[0].decode(), nextblock, nextblock + length - 1))
        nextblock += length
    for copy in range(2):  # blocks 1-2: directory, 3-4: FAT; the copy with the higher check code is current
        d = bytearray(dir_)
        d[0x1FFA:0x1FFC] = struct.pack('>h', copy)
        d[0x1FFC:0x2000] = struct.pack('>HH', *checksum(bytes(d[:0x1FFC])))
        card[(1 + copy) * BLOCK:(2 + copy) * BLOCK] = d
        f = list(fat)
        f[2], f[3], f[4] = copy, nblocks - nextblock, nextblock - 1  # check code, free blocks, last slot
        fb = bytearray(struct.pack('>%dH' % len(f), *f))
        fb[0:4] = struct.pack('>HH', *checksum(bytes(fb[4:])))
        card[(3 + copy) * BLOCK:(4 + copy) * BLOCK] = fb
    open(out, 'wb').write(card)
    print('%s: %d Mbit card, %d of %d blocks free' % (out, mbit, nblocks - nextblock, nblocks - 5))


main()

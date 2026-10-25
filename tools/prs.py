# Sega PRS (LZ77) decompression, as the game's own PRS loader does it. limit stops early (header
# peeks).
def decompress(src, limit=None):
    out = bytearray()
    i = 0
    bits = 0
    ctrl = 0

    def bit():
        nonlocal i, bits, ctrl
        if bits == 0:
            ctrl = src[i]
            i += 1
            bits = 8
        b = ctrl & 1
        ctrl >>= 1
        bits -= 1
        return b

    while limit is None or len(out) < limit:
        if bit():  # literal
            out.append(src[i])
            i += 1
            continue
        if bit():  # long copy: 13-bit offset, 3-bit size (0: size in the next byte)
            v = src[i] | src[i + 1] << 8
            i += 2
            if v == 0:
                break
            off = (v >> 3) - 0x2000
            size = v & 7
            if size:
                size += 2
            else:
                size = src[i] + 1
                i += 1
        else:  # short copy: 2-bit size, 8-bit offset
            size = (bit() << 1 | bit()) + 2
            off = src[i] - 0x100
            i += 1
        start = len(out) + off
        for k in range(size):
            out.append(out[start + k])
    return bytes(out)

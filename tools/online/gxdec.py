# Decodes GX textures (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8) dumped by SA2_TEX_DUMP to PNG.
import sys, re, os
from PIL import Image

BLOCK = {0: (8, 8), 1: (8, 4), 2: (8, 4), 3: (4, 4), 4: (4, 4), 5: (4, 4), 6: (4, 4)}


def decode(data, w, h, fmt):
    img = Image.new('RGBA', (w, h))
    px = img.load()
    bw, bh = BLOCK[fmt]
    pos = 0
    for by in range(0, h, bh):
        for bx in range(0, w, bw):
            if fmt == 6:  # RGBA8: AR block then GB block
                ar = data[pos:pos + 32]; gb = data[pos + 32:pos + 64]; pos += 64
                for i in range(16):
                    x, y = bx + i % 4, by + i // 4
                    if x < w and y < h:
                        px[x, y] = (ar[2 * i + 1], gb[2 * i], gb[2 * i + 1], ar[2 * i])
                continue
            for y in range(by, by + bh):
                for x in range(bx, bx + bw):
                    if fmt == 0:  # I4
                        b = data[pos + (x - bx) // 2]
                        v = (b >> 4) if (x - bx) % 2 == 0 else (b & 15)
                        c = (v * 17, v * 17, v * 17, 255)
                    elif fmt == 1:  # I8
                        v = data[pos + (x - bx)]; c = (v, v, v, 255)
                    elif fmt == 2:  # IA4
                        b = data[pos + (x - bx)]; i = (b & 15) * 17; c = (i, i, i, (b >> 4) * 17)
                    elif fmt == 3:  # IA8
                        a, i = data[pos + 2 * (x - bx)], data[pos + 2 * (x - bx) + 1]; c = (i, i, i, a)
                    else:
                        v = data[pos + 2 * (x - bx)] << 8 | data[pos + 2 * (x - bx) + 1]
                        if fmt == 4:
                            c = ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255)
                        elif v & 0x8000:
                            c = (((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31, 255)
                        else:
                            c = (((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 7) * 255 // 7)
                    if x < w and y < h:
                        px[x, y] = c
                pos += {0: 4, 1: 8, 2: 8, 3: 8, 4: 8, 5: 8}[fmt]
    return img


for f in sys.argv[1:]:
    m = re.search(r'_(\d+)x(\d+)_f([0-9A-F])\.bin$', f)
    w, h, fmt = int(m[1]), int(m[2]), int(m[3], 16)
    if fmt not in BLOCK:
        print(f, 'format', fmt, 'skipped'); continue
    img = decode(open(f, 'rb').read(), w, h, fmt)
    out = f[:-4] + '.png'
    img.save(out)
    print(out)

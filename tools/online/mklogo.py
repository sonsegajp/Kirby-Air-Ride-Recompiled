# The Online logo, made from the LAN logo's own pieces and style: a globe orbited by the LAN emblem's
# dotted chain (I4 64x64, white on black like the original), and "ONLINE" lettered like "LAN" (IA4 56x24:
# black letters with a white stroke). Writes previews and the C++ data (GX-encoded) with the
# originals' content hashes so the game's textures are swapped wherever they load.
import math, os, sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "runtime", "online_logo.inc")
# the originals: dumped from the game with SA2_TEX_DUMP=B0D060,B0CB20 (tex_*.bin), decoded with gxdec.py (tex_*.png)
LOGO = sys.argv[1] if len(sys.argv) > 1 else "."
S = 8  # supersampling


def emblem():
    n = 64 * S
    im = Image.new('L', (n, n), 0)
    d = ImageDraw.Draw(im)
    cx, cy, r = n * 0.5, n * 0.5, n * 0.30
    # the globe: a disc outline, meridians and parallels
    lw = int(3.2 * S)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=255, width=lw)
    for k in (0.42, 0.82):  # meridians (ellipses, narrower)
        d.ellipse([cx - r * k, cy - r, cx + r * k, cy + r], outline=255, width=int(2.4 * S))
    d.line([cx, cy - r, cx, cy + r], fill=255, width=int(2.4 * S))
    for t in (-0.5, 0.0, 0.5):  # parallels (chords)
        y = cy + r * t
        half = r * math.sqrt(max(0.0, 1 - t * t))
        d.line([cx - half, y, cx + half, y], fill=255, width=int(2.4 * S))
    # the LAN emblem's dotted chain, as an orbit around the globe (tilted ellipse of dots)
    ring = Image.new('L', (n, n), 0)
    rd = ImageDraw.Draw(ring)
    a, b, tilt = n * 0.46, n * 0.17, math.radians(-28)
    for i in range(18):
        th = 2 * math.pi * i / 18
        x, y = a * math.cos(th), b * math.sin(th)
        X = cx + x * math.cos(tilt) - y * math.sin(tilt)
        Y = cy + x * math.sin(tilt) + y * math.cos(tilt)
        front = math.sin(th) > 0  # the near half of the orbit passes in front of the globe
        rr = (2.6 if front else 2.2) * S
        val = 255 if front else 150
        if not front:
            # hidden behind the globe's disc: skip dots inside it
            if (X - cx) ** 2 + (Y - cy) ** 2 < (r * 0.98) ** 2:
                continue
        rd.ellipse([X - rr, Y - rr, X + rr, Y + rr], fill=val)
    # dots in front cut a dark gap in the globe lines, as the chain does over the plugs
    gap = ring.point(lambda v: 255 if v > 0 else 0).filter(ImageFilter.MaxFilter(int(2 * S) | 1))
    im.paste(0, mask=gap)
    im = Image.composite(ring, im, ring.point(lambda v: 255 if v > 0 else 0))
    # a small satellite/arrow pair suggesting send/receive, where the plugs' tips were
    for sx, sy, ang in ((n * 0.80, n * 0.20, 45), (n * 0.20, n * 0.80, 225)):
        L = n * 0.075
        pts = [(0, -L), (L * 0.8, L * 0.5), (0, L * 0.15), (-L * 0.8, L * 0.5)]
        ca, sa = math.cos(math.radians(ang)), math.sin(math.radians(ang))
        d2 = ImageDraw.Draw(im)
        d2.polygon([(sx + px * ca - py * sa, sy + px * sa + py * ca) for px, py in pts], fill=255)
    return im.resize((64, 64), Image.LANCZOS)


def lettering():
    w, h = 56 * S, 24 * S
    font = None
    for name in ("ariblk.ttf", "impact.ttf", "arialbd.ttf"):
        try:
            font = ImageFont.truetype(os.path.join(r"C:\Windows\Fonts", name), int(19 * S))
            break
        except OSError:
            pass
    text = "ONLINE"
    # fit the width: letters squeezed horizontally like the LAN label's heavy face
    tmp = Image.new('L', (w * 3, h * 2), 0)
    ImageDraw.Draw(tmp).text((S * 4, S * 2), text, font=font, fill=255)
    bbox = tmp.getbbox()
    glyphs = tmp.crop(bbox)
    inner_w, inner_h = w - int(6 * S), h - int(5.5 * S)
    glyphs = glyphs.resize((inner_w, inner_h), Image.LANCZOS)
    fill = Image.new('L', (w, h), 0)
    fill.paste(glyphs, ((w - inner_w) // 2, (h - inner_h) // 2))
    outline = fill.filter(ImageFilter.MaxFilter(int(3.4 * S) | 1))   # dark outline
    rim = outline.filter(ImageFilter.MaxFilter(int(1.6 * S) | 1))     # light rim around it
    inten = Image.new('L', (w, h), 0)
    inten.paste(255, mask=outline)    # white stroke
    inten.paste(0, mask=fill)         # black letters
    alpha = outline
    inten = inten.resize((56, 24), Image.LANCZOS)
    alpha = alpha.resize((56, 24), Image.LANCZOS)
    return inten, alpha


def enc_i4(img):
    w, h = img.size
    px = img.load()
    out = bytearray()
    for by in range(0, h, 8):
        for bx in range(0, w, 8):
            for y in range(by, by + 8):
                for x in range(bx, bx + 8, 2):
                    a = px[x, y] >> 4 if x < w and y < h else 0
                    b = px[x + 1, y] >> 4 if x + 1 < w and y < h else 0
                    out.append(a << 4 | b)
    return bytes(out)


def enc_ia4(inten, alpha):
    w, h = inten.size
    pi, pa = inten.load(), alpha.load()
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 8):
            for y in range(by, by + 4):
                for x in range(bx, bx + 8):
                    if x < w and y < h:
                        out.append((pa[x, y] >> 4) << 4 | (pi[x, y] >> 4))
                    else:
                        out.append(0)
    return bytes(out)


def fnv(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


em = emblem()
inten, alpha = lettering()
# previews: the new pieces beside the old, 4x
prev = Image.new('RGBA', (64 * 4 * 2 + 56 * 4 * 2 + 60, 64 * 4 + 20), (40, 90, 160, 255))
old_em = Image.open(os.path.join(LOGO, 'tex_B0D060_64x64_f0.png')).resize((256, 256), Image.NEAREST)
old_tx = Image.open(os.path.join(LOGO, 'tex_B0CB20_56x24_f2.png')).resize((224, 96), Image.NEAREST)
prev.alpha_composite(old_em, (10, 10))
prev.alpha_composite(Image.merge('RGBA', (em, em, em, Image.new('L', em.size, 255))).resize((256, 256), Image.NEAREST), (276, 10))
prev.alpha_composite(old_tx, (542, 10))
new_tx = Image.merge('RGBA', (inten, inten, inten, alpha)).resize((224, 96), Image.NEAREST)
prev.alpha_composite(new_tx, (542, 120))
prev.save(os.path.join(LOGO, 'online_preview.png'))

em_bytes = enc_i4(em)
tx_bytes = enc_ia4(inten, alpha)
orig_em = open(os.path.join(LOGO, 'tex_B0D060_64x64_f0.bin'), 'rb').read()
orig_tx = open(os.path.join(LOGO, 'tex_B0CB20_56x24_f2.bin'), 'rb').read()
assert len(em_bytes) == len(orig_em) and len(tx_bytes) == len(orig_tx)


def arr(name, data):
    lines = [f"alignas(32) const u8 {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 24):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 24]) + ",")
    lines.append("};")
    return "\n".join(lines)


with open(OUT, 'w') as f:
    f.write("// Generated by the Online logo script (the LAN logo's pieces redrawn): do not edit by hand.\n")
    f.write(f"// The LAN emblem (I4 64x64) and lettering (IA4 56x24), by the FNV-1a hashes of their data.\n")
    f.write(f"constexpr u64 kLanEmblemHash = 0x{fnv(orig_em):016X}ull;\n")
    f.write(f"constexpr u64 kLanLetteringHash = 0x{fnv(orig_tx):016X}ull;\n")
    f.write(arr("kOnlineEmblem", em_bytes) + "\n")
    f.write(arr("kOnlineLettering", tx_bytes) + "\n")
print("ok", len(em_bytes), len(tx_bytes))

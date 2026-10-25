# Converts the runtime's raw frame dumps (SA2_DUMP_DIR, *.rgba) to PNG. WebGPU's BGRA8 formats are
# swapped to RGB. Usage: rgba2png.py <file.rgba|dir> [...]
import os, struct, sys
from PIL import Image
BGRA_FORMATS = {1}  # header format 1 = BGRA byte order (frontend dumps)
def convert(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'RGBA'
    w, h, fmt = struct.unpack_from('<3I', d, 4)
    img = Image.frombytes('RGBA', (w, h), d[16:16 + w * h * 4])
    r, g, b, a = img.split()
    if fmt in BGRA_FORMATS: img = Image.merge('RGB', (b, g, r))
    else: img = Image.merge('RGB', (r, g, b))
    out = path[:-5] + '.png'
    img.save(out)
    print(out, w, h, 'format', fmt)
for arg in sys.argv[1:]:
    if os.path.isdir(arg):
        for n in sorted(os.listdir(arg)):
            if n.endswith('.rgba'): convert(os.path.join(arg, n))
    else:
        convert(arg)

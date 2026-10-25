# Prints per-frame pixel ranges of dumped PNG frames and makes a contact sheet. Usage: framestats.py <dir> [step]
import glob, os, sys
from PIL import Image
d = sys.argv[1]; step = int(sys.argv[2]) if len(sys.argv) > 2 else 1
files = sorted(glob.glob(os.path.join(d, '*.png')))
files = [f for f in files if not f.endswith('sheet.png')]
for f in files[::step]:
    im = Image.open(f).convert('RGB')
    print(os.path.basename(f), im.getextrema())
sel = files[::max(1, len(files) // 6)][:6]
if sel:
    w, h = 640, 480
    sheet = Image.new('RGB', (w * 3, h * 2))
    for i, f in enumerate(sel):
        sheet.paste(Image.open(f).convert('RGB').resize((w, h)), ((i % 3) * w, (i // 3) * h))
    sheet.save(os.path.join(d, 'sheet.png'))
    print('sheet:', os.path.join(d, 'sheet.png'))

"""Extract the bitmaps from the bootloader logo.bin: usage: logo_extract.py <logo.bin> <outdir>   (needs Pillow)"""
import struct, sys, zlib
from PIL import Image
p = sys.argv[1]   # logo.bin of the firmware package
out = sys.argv[2]   # output directory (must exist)
d0 = open(p, 'rb').read()
print('size', len(d0), 'magic', d0[:4].hex(), 'name', d0[8:40])
d = d0[512:]
n, total = struct.unpack_from('<II', d, 0)
print('logos', n, 'total', total)
offs = struct.unpack_from('<%dI' % n, d, 8)
print('offsets', offs)
base = 0
for i in range(n):
    st = offs[i]
    en = offs[i + 1] if i + 1 < n else total
    z = d[st:en]
    try:
        raw = zlib.decompressobj().decompress(z)
    except Exception as e:
        print(i, 'decompress failed', e, z[:8].hex())
        continue
    print(i, 'compressed', len(z), 'raw', len(raw), 'hex', z[:4].hex())
    for (w, h) in ((1200, 1920), (1920, 1200)):
        if len(raw) == w * h * 4:
            im = Image.frombuffer('RGBA', (w, h), raw, 'raw', 'BGRA', 0, 1).convert('RGB')
            im.save('%s/logo_%d_%dx%d.png' % (out, i, w, h))
            print('   saved', w, h)
            break

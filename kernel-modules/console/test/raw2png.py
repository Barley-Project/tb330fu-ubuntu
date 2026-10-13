import sys
from PIL import Image
raw, out, stride, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
data = open(raw, 'rb').read()
im = Image.frombuffer('RGBA', (stride // 4, h), data[:stride * h], 'raw', 'BGRA', 0, 1).crop((0, 0, w, h)).convert('RGB')
im.save(out)
print(out, im.size)

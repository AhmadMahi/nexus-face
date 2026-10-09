# Pixel-accurate stand-in for Adafruit GFX on a 128x64 SSD1306, using the
# library's own 5x7 font bitmaps, so widths and clipping are exact.
import re
from PIL import Image, ImageDraw
src = open('glcdfont.c').read()
body = src[src.index('{', src.index('font[]')) + 1: src.index('};', src.index('font[]'))]
FONT = [int(x, 16) for x in re.findall(r'0x[0-9A-Fa-f]{2}', body)]
W, H = 128, 64
class OLED:
    def __init__(s): s.fb = [[0]*W for _ in range(H)]; s.col = 1; s.over = []
    def px(s, x, y, c):
        if 0 <= x < W and 0 <= y < H: s.fb[y][x] = c
    def rect(s, x, y, w, h, c=1):
        for yy in range(y, y+h):
            for xx in range(x, x+w): s.px(xx, yy, c)
    def rrect(s, x, y, w, h, r, c=1):
        for yy in range(y, y+h):
            for xx in range(x, x+w):
                dx = max(x+r-xx, xx-(x+w-1-r), 0); dy = max(y+r-yy, yy-(y+h-1-r), 0)
                if dx*dx + dy*dy <= r*r + r: s.px(xx, yy, c)
    def tri(s, a, b, c_, col):
        xs = [a[0], b[0], c_[0]]; ys = [a[1], b[1], c_[1]]
        def side(p, q, r): return (q[0]-p[0])*(r[1]-p[1]) - (q[1]-p[1])*(r[0]-p[0])
        for yy in range(min(ys), max(ys)+1):
            for xx in range(min(xs), max(xs)+1):
                d1, d2, d3 = side(a, b, (xx, yy)), side(b, c_, (xx, yy)), side(c_, a, (xx, yy))
                if not ((d1 < 0 or d2 < 0 or d3 < 0) and (d1 > 0 or d2 > 0 or d3 > 0)): s.px(xx, yy, col)
    def text(s, x, y, t, size=1, tag=''):
        if x < 0 or x + len(t)*6*size > W + 1: s.over.append(f'{tag}: {t!r} x={x} right={x+len(t)*6*size}')
        for k, ch in enumerate(t):
            o = ord(ch)
            if o >= 176: o += 1
            for i in range(5):
                bits = FONT[o*5 + i]
                for j in range(8):
                    if bits >> j & 1:
                        s.rect(x + (k*6+i)*size, y + j*size, size, size, s.col)
    def ctr(s, t, y, size=1, tag=''):
        w = len(t)*6*size; s.text((W - w)//2 if w < W else 0, y, t, size, tag)
    def titleBar(s, t, right=''):
        s.rect(0, 0, W, 11, 1); s.col = 0; s.text(3, 2, t)
        if right: s.text(W - 3 - len(right)*6, 2, right)
        s.col = 1
    def titleBarC(s, t):
        s.rect(0, 0, W, 11, 1); s.col = 0; s.text((W - len(t)*6)//2, 2, t); s.col = 1
    def png(s, path, scale=5):
        im = Image.new('RGB', (W*scale + 8, H*scale + 8), (40, 40, 40)); d = ImageDraw.Draw(im)
        d.rectangle([4, 4, 4+W*scale-1, 4+H*scale-1], fill=(0, 0, 0))
        for y in range(H):
            for x in range(W):
                if s.fb[y][x]: d.rectangle([4+x*scale, 4+y*scale, 4+x*scale+scale-1, 4+y*scale+scale-1], fill=(120, 220, 255))
        im.save(path)

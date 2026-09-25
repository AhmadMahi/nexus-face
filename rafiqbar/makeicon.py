# The app icon: the same robot that sits on the menu bar, drawn large.
from PIL import Image, ImageDraw
import os, subprocess

S = 1024
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

def sc(v): return int(v * S / 18.0)

# rounded slab behind it
d.rounded_rectangle([sc(0.6), sc(0.6), S - sc(0.6), S - sc(0.6)],
                    radius=sc(4.0), fill=(22, 26, 32, 255))

W = sc(0.55)                      # stroke weight
INK = (228, 232, 238, 255)
EYE = (52, 211, 153, 255)         # the connected green

def y(v): return sc(18 - v)       # PIL counts down, the icon was drawn counting up

# aerial
d.line([sc(9), y(14.2), sc(9), y(16.1)], fill=INK, width=W)
d.ellipse([sc(8.0), y(17.0), sc(10.0), y(15.0)], fill=INK)

# head
d.rounded_rectangle([sc(2.6), y(14.2), sc(15.4), y(3.2)],
                    radius=sc(3.2), outline=INK, width=W)
# ears
d.line([sc(1.0), y(9), sc(2.6), y(9)], fill=INK, width=W)
d.line([sc(15.4), y(9), sc(17.0), y(9)], fill=INK, width=W)
# eyes
for cx in (6.55, 11.45):
    d.ellipse([sc(cx - 1.5), y(11.0), sc(cx + 1.5), y(8.0)], fill=EYE)
# mouth
d.line([sc(6.3), y(5.7), sc(11.7), y(5.7)], fill=INK, width=W)

os.makedirs("Rafiq.iconset", exist_ok=True)
for px in (16, 32, 64, 128, 256, 512, 1024):
    img.resize((px, px), Image.LANCZOS).save(f"Rafiq.iconset/icon_{px}x{px}.png")
    if px > 16:
        img.resize((px, px), Image.LANCZOS).save(f"Rafiq.iconset/icon_{px//2}x{px//2}@2x.png")
subprocess.run(["iconutil", "-c", "icns", "Rafiq.iconset", "-o", "Rafiq.icns"], check=True)
print("Rafiq.icns written")

import sys, json, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfx import OLED, W, H
from PIL import Image
IC = json.load(open("icons.json"))
PAD, BAR, ROWY, ROWH, ROWS, HINT = 3, 11, 14, 12, 4, 54
problems = []

class Screen:
    def __init__(s, name): s.o = OLED(); s.name = name; s.els = []
    def el(s, kind, x, y, w, h, tag=""): s.els.append((kind, x, y, w, h, tag))
    def text(s, x, y, t, col=1):
        s.o.col = col; s.o.text(x, y, t, 1, s.name); s.o.col = 1
        if t: s.el("text", x, y, len(t) * 6 - 1, 8, t)
    def icon(s, x, y, name, scale=1, col=1):
        for j, row in enumerate(IC[name]):
            for i in range(8):
                if row & (0x80 >> i): s.o.rect(x + i * scale, y + j * scale, scale, scale, col)
        s.el("icon", x, y, 8 * scale, 8 * scale, name)
    def bar(s, left, right="14:05"):
        if right == "14:05" and PAD + len(left) * 6 + 6 + 30 > W - PAD: right = ""   # the firmware's rule
        s.o.rect(0, 0, W, BAR, 1); s.o.col = 0
        s.o.text(3, 2, left, 1, s.name)
        if right: s.o.text(W - 3 - len(right) * 6, 2, right, 1, s.name)
        s.o.col = 1
        if 3 + len(left) * 6 + (len(right) * 6 + 6 if right else 0) > W - 3: problems.append(f"{s.name}: title '{left}' meets '{right}'")
    def ctr(s, t, y): s.text((W - len(t) * 6) // 2, y, t)

def scroll(sc, first, total):
    if total <= ROWS: return
    top = BAR + 2; h = H - top - 1
    th = max(6, h * ROWS // total); ty = top + (h - th) * first // (total - ROWS)
    sc.o.rect(W - 1, top, 1, h, 1); sc.o.rect(W - 2, ty, 2, th, 1)
    sc.el("scroll", W - 2, top, 2, h)
def first_of(sel, total):
    f = sel - (ROWS - 1) if sel > ROWS - 1 else 0
    f = min(f, total - ROWS); return max(f, 0)
def row(sc, r, ic, label, value, on, scrolled):
    y = ROWY + r * ROWH; fg = 0 if on else 1
    if on: sc.o.rrect(1, y - 2, W - 2 - (3 if scrolled else 0), ROWH, 2, 1)
    tx = PAD + 1
    if ic: sc.icon(PAD + 1, y, ic, 1, fg); tx = PAD + 8 + 4 + 1
    room = (W - tx - PAD - (5 if scrolled else 0)) // 6
    vlen = len(value) if value else 0
    sc.text(tx, y, label[:max(0, room - (vlen + 1 if vlen else 0))], fg)
    if vlen: sc.text(W - PAD - (5 if scrolled else 0) - vlen * 6, y, value, fg)
def setting_row(sc, r, label, v, on, rows):
    y = ROWY + r * ROWH; s5 = 5 if rows > ROWS else 0; fg = 0 if on else 1
    if on: sc.o.rrect(1, y - 2, W - 2 - (3 if s5 else 0), ROWH, 2, 1)
    sc.text(PAD + 1, y, label, fg)
    room = max(0, (W - PAD - s5 - (PAD + 1) - (len(label) + 1) * 6) // 6)
    v = v[:room]
    if v: sc.text(W - PAD - s5 - len(v) * 6, y, v, fg)

def card(name, ic, l1, l2):
    sc = Screen(name); sc.bar(name)
    iy = BAR + (HINT - BAR - 16) // 2
    sc.icon(PAD + 5, iy, ic, 2)
    tx = PAD + 5 + 16 + 8; two = bool(l2)
    ty = iy - 1 if two else iy + 4
    sc.text(tx, ty, l1[:15])
    if two: sc.text(tx, ty + 10, l2[:15])
    sc.ctr("hold to open", HINT); return sc
def menu(name, items, sel):
    sc = Screen(f"{name} menu sel{sel}"); n = len(items)
    sc.bar(name, f"{sel + 1}/{n}")
    f = first_of(sel, n); scd = n > ROWS
    for r in range(min(ROWS, n - f)):
        ic, lab, val = items[f + r]; row(sc, r, ic, lab, val, f + r == sel, scd)
    scroll(sc, f, n); return sc
def settings(name, rows_, sel):
    sc = Screen(f"settings {name} sel{sel}"); n = len(rows_)
    sc.bar(name.upper())
    f = first_of(sel, n)
    for r in range(min(ROWS, n - f)):
        lab, v = rows_[f + r]; setting_row(sc, r, lab, v, f + r == sel, n)
    scroll(sc, f, n); return sc

def audit(sc):
    for (k, x, y, w, h, t) in sc.els:
        if k == "scroll": continue
        if x < PAD or x + w > W - PAD: problems.append(f"{sc.name}: {k} '{t}' outside the padding (x {x}..{x + w})")
        if y < BAR + 1 or y + h > H - 1: problems.append(f"{sc.name}: {k} '{t}' too close to the top or bottom (y {y}..{y + h})")
    E = sc.els
    for i in range(len(E)):
        for j in range(i + 1, len(E)):
            a, b = E[i], E[j]
            if a[1] < b[1] + b[3] and b[1] < a[1] + a[3] and a[2] < b[2] + b[4] and b[2] < a[2] + a[4]:
                problems.append(f"{sc.name}: '{a[5]}' overlaps '{b[5]}'")
    if sc.o.over: problems.append(f"{sc.name}: past the panel {sc.o.over}")

today = [("IC_BELL", "Notifications", "12"), ("IC_CHECK", "Reminders", "3"), ("IC_MAC", "Mac", ""),
         ("IC_SUN", "Weather", "28C"), ("IC_CAR", "Vehicle", "")]
faith = [("IC_MOON", "Prayer times", ""), ("IC_BEADS", "Zikr", ""), ("IC_DAWN", "Adhkar", ""),
         ("IC_BOOK", "Quran", ""), ("IC_STAR", "99 Names", ""), ("IC_SLIDE", "Prayer settings", "")]
calm = [("IC_LEAF", "Relax", ""), ("IC_PAGE", "Short reads", ""), ("IC_PAD", "Games", "")]
groups = [("IC_SUN", "Display", ""), ("IC_HAND", "Touch and motion", ""), ("IC_BT", "Connections", ""),
          ("IC_SHIELD", "Away and safety", ""), ("IC_CHIP", "System", "")]
display = [("Brightness", "75%"), ("Face", "infograph"), ("Clock", "24 h"), ("Sleep after", "10 s"),
           ("Popup time", "10 s"), ("Page turn", "auto"), ("Eye style", "default"), ("Vehicle", "off")]
conn = [("Network", "Bluetooth"), ("Multi-link", "on, 2 in"), ("Primary", "iPhone"), ("Second", "Mac Ahmed-MBP"),
        ("Pair a Mac", "hold"), ("Hotspot", "off")]
screens = [
    card("TODAY", "IC_BELL", "12 new", "3 reminders"), card("TODAY", "IC_BELL", "All read", "28C outside"),
    card("TODAY", "IC_BELL", "All read", ""),
    card("FAITH", "IC_MOON", "Maghrib 18:42", "in 1h 05m"), card("CALM", "IC_LEAF", "Relax, reads", "and 10 games"),
    card("SETTINGS", "IC_SLIDE", "Battery 100%", "2 linked"),
    menu("TODAY", today, 0), menu("TODAY", today, 4), menu("FAITH", faith, 5), menu("CALM", calm, 1),
    menu("SETTINGS", groups, 0), menu("SETTINGS", groups, 4),
    settings("System", [("Update", "from a file"), ("Power down", "2 h"), ("Battery full", "4.20 V"), ("Reset settings", "hold"), ("Reboot", "hold"), ("About", "hold")], 2),
    settings("Display", display, 1), settings("Display", display, 7),
    settings("Connections", conn, 3),
    settings("Touch and motion", [("Wake by", "touch+move"), ("Wake on hold", "off"), ("Hold time", "0.7 s"), ("Go back by", "touch"), ("Knocks", "on"), ("Tap strength", "medium"), ("Accelerometer", "hold")], 6),
    settings("Away and safety", [("Auto away", "on"), ("Phone guard", "off"), ("Tamper alarm", "hold"), ("Tamper log", "hold")], 0), settings("Prayer settings", [("Prayer times", "saved"), ("Hijri shift", "+0 d")], 0),
]

# 7.10: the watch face picker's strip. The face itself is whatever the
# face draws, which this does not mirror; what has to be checked is the
# strip along the bottom, where a twelve character name and a five
# character count share twenty one characters and used to collide.
def facepick(left, right):
    sc = Screen("Face picker: " + left)
    sc.o.rect(0, 51, W, 1, 1)
    sc.text(PAD, HINT, left)
    sc.text(W - PAD - len(right) * 6, HINT, right)
    return sc
# the longest of each: the longest face name, truncated as the firmware
# truncates it, and the highest count
screens.append(facepick("hold:keep", "23/23"))
screens.append(facepick("Regulator"[:12], "23/23"))
screens.append(facepick("M" * 12, "23/23"))

for sc in screens: audit(sc)
print("screens checked:", len(screens))
print("problems:", len(problems))
for p in problems: print("  ", p)
# a sheet to look at
tiles = []
for sc in screens:
    im = Image.new("RGB", (W, H), (5, 7, 11)); px = im.load()
    for y in range(H):
        for x in range(W):
            if sc.o.fb[y][x]: px[x, y] = (226, 240, 255)
    tiles.append(im.resize((W * 3, H * 3), Image.NEAREST))
cols = 4; sw, sh = W * 3 + 12, H * 3 + 12
sheet = Image.new("RGB", (cols * sw + 12, ((len(tiles) + cols - 1) // cols) * sh + 12), (70, 70, 78))
for i, t in enumerate(tiles): sheet.paste(t, (12 + (i % cols) * sw, 12 + (i // cols) * sh))
sheet.save("screens.png")

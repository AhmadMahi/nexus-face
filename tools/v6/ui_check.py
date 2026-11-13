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


# 7.13: keyring mode's cards, and the screen that warns you before it
# starts. The cards are the only place the firmware draws at size 2 and
# size 1 on the same screen, so both are measured.
def keycard(name, lines):
    sc = Screen("Keyring: " + name)
    for t, y, size in lines:
        w = len(t) * 6 * size
        x = (W - w) // 2
        sc.o.text(x, y, t, size, sc.name)
        if t: sc.el("text", x, y, w - 1, 8 * size, t)
    return sc

PHONE = "+91 86600 27729"
screens += [
    keycard("contact",     [("Ahmed", 12, 2), (PHONE, 32, 1), ("if found, call me", 48, 1)]),
    keycard("don't touch", [("Do not touch me", 20, 1), ("I am very busy", 36, 1)]),
    keycard("handsome",    [("Oh, hello", 12, 2), ("and who are you?", 40, 1)]),
    keycard("lost",        [("Lost?", 12, 2), (PHONE, 34, 1), ("that is my human", 50, 1)]),
    keycard("keyring",     [("Just a keyring", 20, 1), ("nothing to see", 36, 1)]),
    keycard("sleepy",      [("Shhh", 12, 2), ("I am asleep", 40, 1)]),
    keycard("dizzy",       [("Stop it", 12, 2), ("I am getting dizzy", 40, 1)]),
]
_c = Screen("Keyring confirm"); _c.bar("KEYRING MODE")
for _t, _y in [("No radios, no menus", 16), ("Out: hold 4s, or a", 28),
               ("home within 2 min", 38), ("1 moves   hold yes", 54)]:
    _c.ctr(_t, _y)
screens.append(_c)

# 7.14: the ten second notice at power on. The countdown is measured at
# its widest, which is two digits, because a one character difference in
# the middle of a centred line is how a centred line runs off the end.
for _secs in (10, 9):
    _n = Screen(f"Keyring notice {_secs}s"); _n.bar("KEYRING MODE")
    _n.ctr(f"Going quiet in {_secs}s", 18)
    _n.ctr("hold 4s to stop", 34)
    _n.ctr("or send home", 46)
    screens.append(_n)

# 7.14: the terminal face. A watch face owns the whole panel, with no
# title bar and no padding, so the list rules in audit() do not apply to
# it. What does apply is the right edge: the face writes at x 10 and
# "bluetooth off" is the longest value any of its lines has carried.
def termline(label, value):
    line = f"{label:<6}{value}"       # every label in that face is padded to six
    right = 10 + len(line) * 6
    if right > W:
        problems.append(f"face terminal: '{line}' runs {right - W}px past the right edge")
    return right

for _lab, _val in [("time", "23:41:07"), ("date", "Thu 13 Nov"),
                   ("net", "bluetooth off"), ("net", "bluetooth"), ("net", "none"),
                   ("rssi", "radio off"), ("rssi", "-99 dBm"), ("rssi", "--"),
                   ("batt", "100% 4.09V"), ("batt", "no pack"), ("up", "123d 23h")]:
    termline(_lab, _val)


# 7.13's new rows, at their longest values, because a long label beside
# a long value is where a settings row runs out of width.
#
# 7.14: the group list itself, because "Phone and keyring" is the
# longest name any group has had, and the rows it holds.
screens.append(menu("SETTINGS", [(None, "Display", ""), (None, "Phone and keyring", ""),
                                 (None, "Touch and motion", ""), (None, "Away and safety", "")], 1))
screens.append(settings("Phone and keyring", [("Notifications", "off, daily"),
                                              ("Keyring mode", "off"),
                                              ("Card", "don't touch"),
                                              ("Network", "Bluetooth")], 0))
screens.append(settings("Phone and keyring", [("Multi-link", "on, 2 in"), ("Primary", "iPhone"),
                                              ("Second", "MacBook Pro"), ("Pair a Mac", "paired")], 2))
screens.append(settings("Battery", [("Battery use", "hold"), ("Power down", "2 h"),
                                    ("Processor", "160 MHz"), ("Night sleep", "on")], 2))
screens.append(settings("Touch and motion", [("Tap strength", "medium"), ("Accelerometer", "hold"),
                                             ("Motion sensor", "100 Hz"), ("Knocks", "on")], 2))
screens.append(settings("Away and safety", [("Tamper log", "hold"), ("Keyring mode", "off"),
                                            ("Card", "don't touch"), ("Auto away", "on")], 2))

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

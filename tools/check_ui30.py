"""The bits v3.0.0 adds to the screen, against the 128x64 it has."""
import re, sys
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
bad = []
W, H, CH = 128, 64, 6

# Every settings row, name against the widest value that row can show.
# Checking the longest name against the widest value anywhere is how a
# real overlap hides behind a false one, and how "Tap strength" sat
# 16px over "ultra light" for as long as the strengths have had names.
def lit(name, src=src):
    m = re.search(name + r'\[[^\]]*\]\s*=\s*\{(.*?)\};', src, re.S)
    return re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)) if m else []
FACES  = lit("FACE_NAME")
TAPS   = lit("TAP_SHORT")
BRIGHT = re.findall(r'"([^"]*)"', re.search(r'BRIGHT_NAME\[\]\s*=\s*\{(.*?)\};', src, re.S).group(1))
EYES   = re.findall(r'\{\s*"([a-z]+)"', re.search(r'STYLES\[\] = \{(.*?)\};', src, re.S).group(1))
names  = lit("C_NAME")
ROWS = {
  "Brightness": BRIGHT, "Watch face": FACES, "Sleep after": ["never","10m","15s"],
  "Page turn": ["auto","knock"], "Popup time": ["off","30s"], "Eye style": EYES,
  "Prayer times": ["saved","none"], "Hijri shift": ["+2 d","-2 d"],
  # 5.4.0 swapped "x2" for "hold": the pad drives the robot, and
  # knocking twice is a feature that is off unless you ask for it.
  "Network": ["no signal","off","on"], "Vehicle": ["on","off"],
  "Hotspot": ["on","hold"], "Accelerometer": ["hold"],
  "Knocks": ["on","off"], "Tap strength": TAPS + ["off"], "Pair a Mac": ["paired","hold"],
  "Go back by": ["touch","shake","both"],
  "Power down": lit("DEEP_NAME"),
  "Wake on hold": ["off","1s","3s"],
  "Battery full": ["4.25V"],
  "Check update": ["offline","hold"], "Auto update": ["on","off"],
  "Reset settings": ["hold"], "Reboot": ["hold"], "About": ["hold"],
}
n_enum = len([x for x in re.sub(r"//[^\n]*", "", re.search(r"enum \{ C_BRIGHT = 0,(.*?)C_COUNT \};", src, re.S).group(1)).split(",") if x.strip()]) + 1
if n_enum != len(names): bad.append(f"{n_enum} settings in the enum, {len(names)} names")
for nm in names:
    if nm not in ROWS: bad.append(f"settings row '{nm}' has no values listed to check"); continue
    worst = max(ROWS[nm], key=len)
    ends, starts = 3 + len(nm)*CH, W - 3 - len(worst)*CH
    if ends > starts:
        bad.append(f"settings '{nm}' ends {ends}, its widest value '{worst}' starts {starts}")
tight = max(names, key=lambda nm: 3+len(nm)*CH - (W-3-len(max(ROWS.get(nm,['x2']), key=len))*CH))
wv = max(ROWS[tight], key=len)
print(f"  settings  {len(names)} rows all checked; tightest is '{tight}' + '{wv}', "
      f"{3+len(tight)*CH} vs {W-3-len(wv)*CH}")

# The hold bar is gone on request: a long press is the way in to
# everything now, so a bar filling along the bottom was on screen for a
# good part of the time you were using the thing.
if "SCRH - 3, SCRW, 3" in src: bad.append("the hold bar is still drawn")
if "touchLongDone" in src.split("void loop()")[-1]:
    bad.append("the loop still draws something for a long press")
print("  hold bar  gone, as asked")

# the face mark, top right of the clock
if "oled.fillCircle(SCRW - 5, 4, 3, SSD1306_BLACK);" not in src:
    bad.append("the face mark does not clear behind itself")
print(f"  face mark centre ({W-5},4) r3 -> x {W-8}..{W-2}, y 1..7")

# the System touch row: state, a count numStr caps at four, and the
# knock marker. Read the format out of the source so it cannot drift.
fmt = re.search(r'snprintf\(v, sizeof\(v\), "([^"]+)", touchOn', src).group(1)
widest = "ON 999M +k"
if "+k" not in src[src.index("touchOn ? \"ON\""):src.index("touchOn ? \"ON\"")+220]:
    bad.append("the System row no longer shows whether knocks are on")
if 128 - 4 - len(widest)*CH < 4 + len("Network")*CH:
    bad.append(f"System touch row: '{widest}' reaches the label")
print(f"  system    widest '{widest}' starts {128-4-len(widest)*CH}, labels end {4+len('Network')*CH}")


tight = []
# Every centred line, against the 128 it has. This is the check that
# was missing: the settings rows were covered and the title bars were
# covered, and a plain ctr() in the middle of a screen was not, which
# is how a 126 pixel line reached the edge unnoticed.
for m in re.finditer(r'ctr\("((?:[^"\\]|\\.)*)"\s*,\s*(\d+)\s*,\s*(\d+)\)', src):
    s, y, size = m.group(1), int(m.group(2)), int(m.group(3))
    w = len(s) * CH * size
    if w > W:
        bad.append(f"centred '{s}' is {w}px of {W}: it runs off the edge")
    elif w > W - 4:
        tight.append(f"'{s}' {w}px")
    if y + 7 * size > H:
        bad.append(f"centred '{s}' at y={y} size {size} paints past the bottom")
# and every placed line
for m in re.finditer(r'at\((\d+),\s*(\d+),\s*"((?:[^"\\]|\\.)*)"\)', src):
    x, y, s = int(m.group(1)), int(m.group(2)), m.group(3)
    if x + len(s) * CH > W:
        bad.append(f"'{s}' at x={x} needs {x + len(s)*CH} of {W}")
print(f"  text      every ctr() and at() literal measured"
      + (f", {len(tight)} with no margin: {', '.join(tight)}" if tight else ""))

print()
if bad:
    print(f"{len(bad)} PROBLEM(S):"); [print("  -", b) for b in bad]; sys.exit(1)
print("PASS: everything v3.0.0 adds fits where it is put")

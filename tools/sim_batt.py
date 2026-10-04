"""The pack on GPIO1, through two 200K resistors."""
import re, sys
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
fails = []
def must(c, w):
    if not c: fails.append(w)

must("#define BATT_PIN  1" in src, "the divider is read on GPIO1")
must("#define BATT_MUL  2" in src, "200K over 200K is a half, so doubled back")
must("analogReadMilliVolts(BATT_PIN)" in src, "read calibrated, not as raw counts")
must("analogSetPinAttenuation(BATT_PIN, ADC_11db)" in src,
     "the full range, or the top half of the pack reads as maximum")
must("readBattery();" in src and src.count("readBattery();") >= 2, "it is actually called")
# GPIO1 must not be anyone else's
for d in ("SDA_PIN 8", "SCL_PIN 9", "TAP_INT_PIN 4", "TOUCH_PIN 5"):
    must(d in src, f"{d} unchanged")
must(not re.search(r"#define (SDA_PIN|SCL_PIN|TAP_INT_PIN|TOUCH_PIN) 1\b", src),
     "nothing else claims GPIO1")

V = [float(x) for x in re.search(r"static const float V\[\] = \{(.*?)\};", src, re.S).group(1).replace("f","").split(",")]
P = [int(x) for x in re.search(r"static const int   P\[\] = \{(.*?)\};", src, re.S).group(1).split(",")]

def pct(v):
    if v <= V[0]: return 0
    for i in range(1, len(V)):
        if v <= V[i]:
            f = (v - V[i-1]) / (V[i] - V[i-1])
            return P[i-1] + int(f * (P[i] - P[i-1]) + 0.5)
    return 100

def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)

print(f"\ncurve: {len(V)} points, {V[0]}V to {V[-1]}V\n")

def t_ends():
    assert pct(4.20) == 100, pct(4.20)
    assert pct(3.00) == 0, pct(3.00)
    assert pct(4.50) == 100, "over full should still read full"
    assert pct(2.00) == 0, "flat should read flat, not negative"
run("Full reads 100, flat reads 0, and nothing goes outside", t_ends)

def t_monotonic():
    last = -1
    v = 2.8
    while v <= 4.4:
        p = pct(v)
        assert p >= last, f"{v:.2f}V gave {p} after {last}"
        last = p; v += 0.01
    print(f"        never goes backwards across 2.8V to 4.4V")
run("More volts never shows less battery", t_monotonic)

def t_shape():
    # the point of a curve: a straight line would sit near half for
    # most of the useful range and then fall off a cliff
    lin = lambda v: max(0, min(100, int((v - 3.0) / 1.2 * 100)))
    mid = 3.82
    assert pct(mid) == 50, f"the knee is not at the middle: {pct(mid)}"
    assert pct(3.70) < lin(3.70), "the curve is not below a straight line down low"
    print(f"        3.70V: curve {pct(3.70)}%, a straight line would say {lin(3.70)}%")
    print(f"        4.00V: curve {pct(4.00)}%, a straight line would say {lin(4.00)}%")
run("It follows the discharge shape, not a straight line", t_shape)

def t_divider():
    # what the ADC sees, and whether it is inside what it can read
    for pack in (3.0, 3.7, 4.2):
        adc = pack / 2
        assert adc < 2.5, f"a {pack}V pack puts {adc}V on the pin, past the usable range"
    print(f"        a 4.2V pack puts 2.10V on the pin; the 11db range reaches about 2.5V")
run("The divider keeps a full pack inside what the ADC can read", t_divider)

def t_absent():
    m = re.search(r"battV = \(v > ([\d.]+)f\) \? v : NAN;", src)
    assert m, "nothing decides there is no pack"
    thr = float(m.group(1))
    assert 1.5 <= thr <= 3.0, f"the no-pack threshold is {thr}V"
    print(f"        under {thr}V on the pack is read as nothing connected")
run("An empty pin reads as no battery, not as a flat one", t_absent)

print("\nthe System screen, now five rows\n")
def t_rows():
    LY = [int(x) for x in re.search(r"const int LY\[5\] = \{([^}]*)\}", src).group(1).split(",")]
    LB = re.findall(r'"([^"]*)"', re.search(r"const char\* LB\[5\] = \{([^}]*)\}", src).group(1))
    assert len(LY) == len(LB) == 5, "five labels and five positions"
    # size 1 is a 5x7 glyph in a 6x8 cell, and with a transparent
    # background it paints seven rows: y to y+6.
    for y in LY: assert y + 6 <= 63, f"a row at {y} paints past the bottom"
    ipy = int(re.search(r"ctr\(ip\.c_str\(\), (\d+), 1\);", src).group(1))
    assert LY[-1] + 6 < ipy, f"the last row ends {LY[-1]+6}, the address starts {ipy}"
    assert ipy + 6 <= 63, f"the address paints to {ipy+6}"
    assert "oled.drawFastHLine(4, 50" not in src, "the rule is still there, in the way"
    widest = "100% 4.20V"
    assert 128 - 4 - len(widest)*6 > 4 + len("Battery")*6, "the battery value reaches its label"
    print(f"        rows {LY}, address {ipy}, '{widest}' starts {128-4-len(widest)*6}, "
          f"labels end {4+len('Network')*6}")
run("Five rows and the address all fit, with nothing overlapping", t_rows)

print("\nthe two feel changes\n")
def t_nobar():
    assert "touchOn && !touchLongDone" not in src.split("void loop()")[-1], \
        "the hold bar is still drawn"
    assert "SCRH - 3, SCRW, 3" not in src, "the hold bar strip is still there"
run("The bar under a long press is gone", t_nobar)

def t_instant():
    assert "static bool doubleMeansSomething()" in src, "nothing decides when to wait"
    d = re.search(r"static bool doubleMeansSomething\(\) \{.*?\n\}", src, re.S).group(0)
    assert "if (depth != 0) return true;" in d, "it would not wait inside a menu"
    assert "faceMode || inReader()" in d, "it would not wait where a double turns a page"
    assert "if (!touchOn && touchTaps == 1 && !doubleMeansSomething())" in src, \
        "the instant path is not wired in"
run("A single lands at once only where a double means nothing", t_instant)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

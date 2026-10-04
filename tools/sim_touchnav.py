"""The pad as the way the robot is driven: one, two, three and long."""
import re, sys
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
fails = []
def must(c, w):
    if not c: fails.append(w)

LONG = int(re.search(r"#define TOUCH_LONG_MS (\d+)", src).group(1))
GAP  = int(re.search(r"#define TOUCH_GAP_MS  (\d+)", src).group(1))
DEB  = int(re.search(r"#define TOUCH_DEBOUNCE (\d+)", src).group(1))

must("bool cfgKnock = false;" in src, "knocking is off unless asked for")
must('prefs.getBool("knock", false)' in src, "and off again after a restart")
# 5.12.0 added one exception: gesture mode, where knocking the desk
# IS the mode and the pad is only the way out of it. Everywhere else
# knocks still do nothing unless you have switched them on.
must("if (!cfgKnock && !tapTesting && !cfgGesture) { burst = 0; return; }" in src,
     "knocks do nothing while switched off, except in gesture mode")
must("C_KNOCK" in src, "there is a setting for it")
# The Wake on setting is gone with v4: the pad is the only thing that
# wakes it, because a bag being carried or a desk leaned on was costing
# a wake, a WiFi reconnect and a slice of the battery for nothing.
# 5.6.0 moved the arming into sleepNow(), shared by going to sleep on
# purpose and by a touch that turned out to be nothing.
must("esp_deep_sleep_enable_gpio_wakeup(BIT(TOUCH_PIN)," in src, "the pad is the wake source")
# WAKE_NAME is the hold-to-wake setting now, not the old "Wake on"
must("const char*    WAKE_NAME[] = { \"off\", \"1s\", \"3s\" };" in src,
     "the hold to wake setting has gone missing")
must("if (!intWired || deepOff) return;" not in src,
     "deep sleep no longer needs INT1 soldered to happen at all")
must('"Touch it back"' in src and '"Touch to puff"' in src, "both games moved to the pad")

ONE, TWO, THREE, LONG_G = 1, 2, 3, 4
NAME = {1:"one", 2:"two", 3:"three", 4:"long"}

class Pad:
    """The recogniser, as the firmware has it."""
    def __init__(s):
        s.on=False; s.edge=0; s.pressAt=0; s.liftAt=0; s.taps=0; s.longDone=False
        s.out=[]
    def step(s, down, now):
        if down != s.on:
            if not s.edge: s.edge = now
            if now - s.edge >= DEB:
                s.on = down; s.edge = 0
                if s.on:
                    s.pressAt = now; s.longDone = False
                elif not s.longDone:
                    if s.taps < 3: s.taps += 1
                    s.liftAt = now
                    if s.taps == 3: s.out.append(THREE); s.taps = 0
        else:
            s.edge = 0
        if s.on and not s.longDone and now - s.pressAt >= LONG:
            s.longDone = True; s.taps = 0; s.out.append(LONG_G)
        if not s.on and s.taps and now - s.liftAt >= GAP:
            n = s.taps; s.taps = 0
            s.out.append(ONE if n == 1 else TWO)

def play(script, tail=600):
    """script: list of (down, ms). Returns the gestures, and when each fired."""
    p = Pad(); t = 0; seen = []
    def run(down, ms):
        nonlocal t
        for _ in range(0, ms, 5):
            before = len(p.out)
            p.step(down, t)
            for g in p.out[before:]: seen.append((g, t))
            t += 5
    for down, ms in script: run(down, ms)
    run(False, tail)
    return seen

def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)

print(f"\nlong {LONG}ms, gap {GAP}ms, settle {DEB}ms\n")

def t_one():
    g = play([(True,120),(False,50)])
    assert [x[0] for x in g] == [ONE], [NAME[x[0]] for x in g]
    lift = 120 + DEB
    assert g[0][1] - lift <= GAP + 20, f"a single took {g[0][1]-lift}ms after the lift"
    print(f"        a single lands {g[0][1]-lift}ms after you let go")
run("A single press is one, and only one", t_one)

def t_two():
    g = play([(True,110),(False,110),(True,110),(False,60)])
    assert [x[0] for x in g] == [TWO], [NAME[x[0]] for x in g]
run("Two presses are a two, never two ones", t_two)

def t_three():
    g = play([(True,100),(False,100),(True,100),(False,100),(True,100),(False,50)])
    assert [x[0] for x in g] == [THREE], [NAME[x[0]] for x in g]
run("Three presses are a three", t_three)

def t_three_instant():
    # nothing can follow a three, so it does not wait for the window
    p = Pad(); t = 0
    for down, ms in [(True,100),(False,100),(True,100),(False,100),(True,100)]:
        for _ in range(0, ms, 5): p.step(down, t); t += 5
    for _ in range(0, 60, 5): p.step(False, t); t += 5    # the third lift
    assert p.out == [THREE], f"a three waited: {p.out}"
    print(f"        a three fires on the lift, with no wait at all")
run("A three does not wait for a window that cannot be filled", t_three_instant)

def t_long():
    g = play([(True, LONG + 200),(False,50)])
    assert [x[0] for x in g] == [LONG_G], [NAME[x[0]] for x in g]
    assert g[0][1] <= LONG + DEB + 20, "the long press fired late"
    print(f"        a long press fires at {g[0][1]}ms, under your finger")
run("A long press fires while the finger is still down", t_long)

def t_long_not_also_one():
    g = play([(True, LONG + 300),(False,50)])
    assert [x[0] for x in g] == [LONG_G], f"the lift after a long press also counted: {[NAME[x[0]] for x in g]}"
run("Letting go after a long press is not also a single", t_long_not_also_one)

def t_long_then_one():
    g = play([(True, LONG+150),(False,500),(True,100),(False,60)])
    assert [x[0] for x in g] == [LONG_G, ONE], [NAME[x[0]] for x in g]
run("A long press and then a single are two separate things", t_long_then_one)

def t_bounce():
    # a contact that chatters on the way down and on the way up
    s = []
    for _ in range(3): s += [(True,10),(False,10)]
    s += [(True,120)]
    for _ in range(3): s += [(False,10),(True,10)]
    s += [(False,60)]
    g = play(s)
    assert [x[0] for x in g] == [ONE], f"chatter turned into {[NAME[x[0]] for x in g]}"
run("A chattering contact is still one press", t_bounce)

def t_four_is_three():
    g = play([(True,80),(False,80)]*4)
    assert THREE in [x[0] for x in g], "a fourth press lost the three"
run("A fourth press cannot be mistaken for nothing", t_four_is_three)

def t_slow_double_is_two_ones():
    g = play([(True,100),(False,GAP+200),(True,100),(False,60)])
    assert [x[0] for x in g] == [ONE, ONE], [NAME[x[0]] for x in g]
run("Two presses far apart are two singles, not a double", t_slow_double_is_two_ones)

print("\nWhere the gestures go\n")

def t_map():
    d = re.search(r"static void touchGesture\(uint8_t g\) \{.*?\n\}", src, re.S).group(0)
    tail = d[d.index("  switch (g) {"):]
    assert "case TG_ONE:   knockOne();" in tail, "one is not next"
    assert "case TG_LONG:  clickShrink(); knockTwo();" in tail, "long does not go in"
    # 5.5.0 put the pad back out of it. "Go back by" is about the body
    # of the robot, a knock or a shake; two presses always go back.
    assert "case TG_TWO:   knockThree();" in tail, "two does not go back"
    assert "screen = S_HOME; depth = 0;" in tail, "three does not go home"
run("one next, long in, two back, three home", t_map)

def t_reader():
    d = re.search(r"static void touchGesture\(uint8_t g\) \{.*?\n\}", src, re.S).group(0)
    m = re.search(r"if \(inReader\(\)\) \{.*?\n  \}", d, re.S)
    assert m, "a reader is not treated differently at all"
    r = m.group(0)
    assert "case TG_ONE:   nextPage();" in r, "one is not the next page"
    assert "case TG_TWO:   prevPage();" in r, "two is not the page before"
    assert "case TG_LONG:  depth = 0;" in r, "a long press does not leave the reader"
    assert "default:       break;" in r, "three no longer reaches home from a read"
run("In a reader: one on, two back a page, long out, three home", t_reader)

def t_facemode():
    d = re.search(r"static void touchGesture\(uint8_t g\) \{.*?\n\}", src, re.S).group(0)
    m = re.search(r"if \(faceMode\) \{.*?\n  \}", d, re.S).group(0)
    assert "case TG_ONE:   cfgFace = (cfgFace + 1) % FACE_N" in m, "one does not walk the faces"
    assert "cfgFace = (cfgFace + FACE_N - 1) % FACE_N" in m, "two does not walk them back"
    assert "default:       faceMode = false;" in m, "nothing leaves face mode"
    assert m.count('prefs.putInt("face", cfgFace)') == 2, "a face is not kept as you go"
    assert "if (g == TG_LONG && screen == S_HOME && depth == 0 && timeOk)" in d, \
        "a long press on the clock does not open the faces"
    assert "clickShrink();" in d, "going in does not shrink"
run("Faces read like a reader: one on, two back, long out", t_facemode)

def t_shrink():
    c = re.search(r"static void clickShrink\(\) \{.*?\n\}", src, re.S).group(0)
    assert "oled.getBuffer()" in c, "it does not scale the real screen"
    assert "memcpy(snap, buf, sizeof(snap))" in c, "it does not take a copy first"
    pct = [int(x) for x in re.search(r"pct\[4\] = \{ ([^}]*) \}", c).group(1).split(",")]
    assert pct[0] < 100 and pct[-1] == 100, f"it does not come back to full size: {pct}"
    assert min(pct) >= 70, f"it shrinks to {min(pct)}%, which is a jump not a press"
    # The waiting is only half of it. Each frame is a whole 1024 byte
    # panel transfer, and at the 400kHz this sets, that is about 23ms
    # that no delay() accounts for.
    dly = int(re.search(r"delay\((\d+)\)", c).group(1))
    assert "Wire.setClock(400000)" in src, "the frame cost below assumes 400kHz"
    frame_ms = 23
    total = dly * (len(pct) - 1) + frame_ms * len(pct)
    assert 120 <= total <= 400, f"the whole click takes {total}ms"
    print(f"        four frames {pct}, {dly}ms apart, ~{total}ms all in")
run("The shrink scales the real buffer and comes back to full size", t_shrink)

def t_blink_gone():
    assert "blinkUntil" not in src, "the old inverse blink is still in there"
    assert "oled.invertDisplay" not in src, "something still inverts the panel"
run("The inverse blink is gone, replaced by the shrink", t_blink_gone)

def t_wake():
    g = re.search(r"static void sleepNow\(long secs\) \{.*?esp_deep_sleep_start\(\);", src, re.S).group(0)
    assert "TAP_INT_PIN" not in g, "the accelerometer can still wake it"
    assert "BIT(TOUCH_PIN)" in g, "the pad cannot wake it"
    d = re.search(r"static void goDeep\(\) \{.*?sleepNow\(secs\);", src, re.S).group(0)
    assert "if (deepOff) return;" in d, "deep sleep cannot be switched off"
    # and the gate has to honour it too, or a refused wake would put a
    # board that is meant to stay on back to sleep with no way out
    w = re.search(r"static void wakeGate\(\) \{.*?\n\}\n", src, re.S).group(0)
    assert 'bool noDeep = prefs.getBool("nodeep", false);' in w and \
           "if (!need || noDeep) return;" in w, \
        "the wake gate ignores deep sleep being switched off"
run("The pad is the only way back, and deep sleep can still be turned off", t_wake)

def t_fall_survives():
    # the drop animation is on the accelerometer's own interrupt and has
    # nothing to do with whether knocks drive the menus
    i = src.index("wake(\"fall\"); onFall(); return;")
    before = src[:i]
    assert "cfgKnock" not in before[before.rindex("if (adxl) {"):], \
        "the drop animation now depends on knocks being on"
run("Dropping it still does the animation with knocks off", t_fall_survives)

def t_no_trap():
    d = re.search(r"static void touchGesture\(uint8_t g\) \{.*?\n\}", src, re.S).group(0)
    # anything that is not one or two must leave face mode, or a mode
    # with no way out is a robot you have to unplug
    m = re.search(r"if \(faceMode\) \{.*?\n  \}", d, re.S).group(0)
    assert "default:       faceMode = false;" in m, "there is a way into face mode and not out"
run("Nothing can leave you stuck in face mode", t_no_trap)

def t_tap_test_escape():
    d = re.search(r"static void touchGesture\(uint8_t g\) \{.*?\n\}", src, re.S).group(0)
    assert "if (tapTesting && tapChosen && g == TG_LONG) { tapTesting = false; return; }" in d, \
        "no way off the knock test screen using the pad"
run("The pad can always get you off the knock test screen", t_tap_test_escape)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS: four gestures, each distinct, and every one goes somewhere sensible")

"""Every settings row, checked one at a time.

For each row: does holding it do anything, does it show a value, does
the name and the value fit on a 128 pixel line, and is the value drawn
from a list the firmware actually has. Also that nothing on any menu
still tells you to knock, now that the pad is how the robot is driven.
"""
import re, sys
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
fails = []
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)

ROWS = [x.strip().split(" ")[0] for x in
        re.split(r",", re.search(r"enum \{ (C_BRIGHT = 0,.*?)C_COUNT \};", src, re.S).group(1))
        if x.strip()]
ROWS = [r.replace("C_BRIGHT", "C_BRIGHT") for r in ROWS if r.startswith("C_")]
NAMES = re.findall(r'"([^"]*)"',
         re.search(r"const char\* C_NAME\[C_COUNT\] =\s*\{(.*?)\};", src, re.S).group(1))
print(f"\n{len(ROWS)} settings rows\n")

def t_names_match():
    assert len(ROWS) == len(NAMES), f"{len(ROWS)} rows but {len(NAMES)} names"
    print("        " + ", ".join(NAMES[:5]) + ", ...")
run("Every row has a name", t_names_match)

def block(after, opener="switch (itemIdx) {"):
    """The body of the switch that follows this marker, by brace."""
    i = src.index(opener, src.index(after))
    j, d = i + len(opener) - 1, 0
    while True:
        if src[j] == "{": d += 1
        elif src[j] == "}": d -= 1
        if d == 0: return src[i:j + 1]
        j += 1

VAL  = block("at(3, y, C_NAME[i]);", "switch (i) {")
# a single press at depth two walks the value; a long press opens or acts
# 5.14.0 put the settings into groups, so a press at depth one walks
# the groups or the rows of one rather than the whole flat list.
CYCLE = block("else            itemIdx = sgNext(setGrp, itemIdx);  // within one")
def blockContaining(marker, opener="switch (itemIdx) {"):
    """The switch that actually holds this case. knockTwo has several
    switches on itemIdx and taking the first one silently measured the
    wrong menu."""
    end = src.index(marker)
    i = src.rindex(opener, 0, end)
    j, d = i + len(opener) - 1, 0
    while True:
        if src[j] == "{": d += 1
        elif src[j] == "}": d -= 1
        if d == 0: return src[i:j + 1]
        j += 1

ACT = blockContaining("      case C_SHAKE:\n        cfgBack")
def nocomment(t):
    return re.sub(r"//[^\n]*", "", t)

def t_every_row_does_something():
    """A row you can select and hold that does nothing is a dead row.
    Either a press walks its value, or a hold opens it or acts on it."""
    # Anything with no case of its own falls to the default, which
    # opens it at depth two; that only counts as doing something if
    # there is a screen there to see.
    opens = re.findall(r"depth == 2 && itemIdx == (C_\w+)", src) + \
            re.findall(r"itemIdx == (C_\w+) && depth >= 2", src)
    assert "default:        depth = 2; break;" in ACT, \
        "rows with no case of their own no longer open anything"
    dead = [n for r, n in zip(ROWS, NAMES)
            if f"case {r}:" not in CYCLE and f"case {r}:" not in ACT and r not in opens]
    assert not dead, f"nothing happens on: {', '.join(dead)}"
    cyc = sum(1 for r in ROWS if f"case {r}:" in CYCLE)
    act = sum(1 for r in ROWS if f"case {r}:" in ACT)
    print(f"        {cyc} walk a value on a press, {act} act on a hold, "
          f"{len(set(opens) & set(ROWS))} open a screen of their own")
run("Every row responds to the pad", t_every_row_does_something)

def t_every_row_shows_something():
    own = set(re.findall(r"case (C_\w+):", VAL))
    assert '"hold"' in VAL, "the fallback no longer says how to open a row"
    print(f"        {len(own)} show a value, {len(ROWS) - len(own)} say 'hold'")
run("Every row shows either a value or how to open it", t_every_row_shows_something)

def t_no_knock_language():
    """The pad drives the robot. 'x2' meant knock twice."""
    v = nocomment(VAL)
    assert '"x2"' not in v, "a row still tells you to knock twice"
    assert '"x3"' not in nocomment(src) and '"x4"' not in nocomment(src), \
        "something still counts knocks at you"
    assert '"knock"' not in v, "a row still offers knocking as a value"
    assert "HOW TO KNOCK" not in src, "the card on a cold boot still teaches knocking"
    assert '"Knock four times"' not in src, "the reads shelf still asks for four knocks"
    assert '"Knocks"' in src and '"TAP STRENGTH"' in src, \
        "the knock settings themselves have gone missing"
    print("        no row and no card asks you to knock")
run("Nothing in the menus tells you to knock any more", t_no_knock_language)

def longest(name, fallback):
    m = re.search(rf"{name}\[[^\]]*\] =\s*\{{(.*?)\}};", src, re.S)
    if not m: return fallback
    opts = re.findall(r'"([^"]*)"', m.group(1))
    return max(opts, key=len) if opts else fallback

def t_rows_fit():
    """Name on the left, value right aligned, 128 pixels between."""
    LONGEST = {
        "C_BRIGHT": longest("BRIGHT_NAME", "100%"),
        "C_FACE":   longest("FACE_NAME", "regulator"),
        "C_DEEP":   longest("DEEP_NAME", "30 min"),
        "C_EYES":   "sleepy",   "C_SHAKE": max(["touch","shake","both"], key=len),
        "C_MODE":   "no signal","C_BATT":  "4.25V",
        "C_SLEEP":  "never",    "C_POPUP": "off",
        "C_TURN":   "touch",    "C_HIJRI": "+30 d",
        "C_PAIR":   "paired",   "C_TAP":   "hard",
        "C_PRAYER": "saved",    "C_UPDATE": "offline",
        "C_KNOCK":  "off",      "C_BIKE":  "off",   "C_AUTOUP": "off",
        "C_HOTSPOT":"hold",
    }
    bad, widest = [], 0
    for r, n in zip(ROWS, NAMES):
        v = LONGEST.get(r, "hold")
        w = 3 + len(n) * 6 + 4 + len(v) * 6 + 3
        widest = max(widest, w)
        if w > 128: bad.append(f"{n!r} + {v!r} needs {w}px")
    assert not bad, "; ".join(bad)
    print(f"        widest row {widest}px of 128")
run("Name and value fit on every row, at their longest", t_rows_fit)

def t_knocks_reach_everything():
    """Knocks are optional, but where they are on they must open the
    same things the pad does."""
    d0 = block("static void knockTwo() {\n", "switch (screen) {")
    for scr in ("S_FOCUS", "S_FAITH", "S_READS", "S_GAMES", "S_SETTINGS", "S_REMIND"):
        assert f"case {scr}:" in d0, f"two knocks cannot open {scr}"
    print("        two knocks open every screen a hold opens")
run("Knocks reach everything the pad reaches", t_knocks_reach_everything)

def t_back_three_ways():
    n = int(re.search(r"BACK_N \};", src) is not None)
    names = re.findall(r'"([^"]*)"',
             re.search(r"BACK_NAME\[BACK_N\] = \{(.*?)\}", src).group(1))
    assert names == ["knock", "shake", "both"], f"the options are {names}"
    assert "cfgBack = (cfgBack + 1) % BACK_N;" in src, "holding does not walk them"
    assert 'flash(cfgBack == BACK_KNOCK ? (cfgKnock ? "BACK BY KNOCK"' in src, \
        "it does not say what it chose"
    assert "BACK_NAME[cfgBack]" in src, "the row does not show which one is set"
    # the pad is not part of this: two presses always go back
    assert "case TG_TWO:   knockThree(); break;" in src, \
        "a setting about the body of the robot has taken the pad with it"
    assert "else if (n == 3) { if (backByKnock()) knockThree(); }" in src, \
        "choosing the shake does not stop a knock going back"
    assert "if (cfgShake && !byHand" in src, "choosing the knock does not stop a shake going back"
    assert '"knock off"' in src, "it does not say when the knock it is set to is switched off"
    print(f"        {names}, walked by holding, shown on the row")
run("Going back is a three way setting walked like every other", t_back_three_ways)

def t_back_cannot_strand():
    """Whatever is set, there is a way out of everywhere."""
    assert "screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0;" in \
        src[src.index("case TG_THREE:"):src.index("case TG_THREE:") + 400], \
        "three presses no longer go home"
    # 5.10.0: four seconds goes home and decides it. The count after
    # that is an announcement, not a question, because the pad cannot
    # be relied on to still be reporting a finger by then.
    # touchSeenFree: nothing that can switch the robot off fires
    # until the pad has been seen settled at rest. See sim_pad.
    assert "if (!cfgGesture && touchSeenFree && !sleepArmed && held >= TOUCH_HOME_MS) {" in src, \
        "a four second hold no longer goes home"
    # and in gesture mode the same hold leaves the mode rather than
    # switching the robot off underneath you
    assert "if (cfgGesture && touchSeenFree && held >= TOUCH_HOME_MS) {" in src, \
        "a four second hold in gesture mode does not leave it"
    assert "STAYING UP" not in src, "letting go can still take the decision back"
    assert src.count("upState = U_OFF; swOn = false; swRun = false;") >= 1, \
        "going home leaves the update screen or the stopwatch holding the panel"
    # and the page turning doubles are untouched
    g = src[src.index("if (screen == S_REMIND && depth > 0) {"):]
    assert "case TG_TWO:  if (remIdx > 0) remIdx--; else depth = 0; return;" in g[:900], \
        "the reminder reader lost its way out"
    print("        three presses and a five second hold always go home")
run("No setting of it can leave you stuck", t_back_cannot_strand)

def t_migrates():
    assert 'if (prefs.isKey("back"))' in src, "it does not look for the new setting"
    assert 'cfgBack = prefs.getBool("shake", true) ? BACK_BOTH : BACK_KNOCK;' in src, \
        "an existing robot loses its setting on this update"
    assert 'else if (k == "shake"){ cfgBack = v ? BACK_BOTH : BACK_KNOCK;' in src, \
        "the Mac's shake switch stops working"
    assert '"shake\\":" + String(cfgShake' in src, "the Mac can no longer read it back"
    print("        old setting carried over, and the Mac still speaks its own word")
run("An existing robot keeps what it had", t_migrates)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

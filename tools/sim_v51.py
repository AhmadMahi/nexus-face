"""The shake that was undoing your long press, and the rest of 5.1.0."""
import re, sys
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
app = open("rafiq-app/mac/Sources/Grid.swift").read()
set_ = open("rafiq-app/mac/Sources/SettingsPane.swift").read()
fails=[]
def must(c,w):
    if not c: fails.append(w)
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)

AFTER = int(re.search(r"#define SHAKE_AFTER_MS (\d+)", src).group(1))
LONG  = int(re.search(r"#define TOUCH_LONG_MS (\d+)", src).group(1))
print(f"\nlong press {LONG}ms, shakes ignored for {AFTER}ms after a press\n")

# the bug, as a sequence
class Bot:
    def __init__(s, guard): s.depth=0; s.guard=guard; s.touchOn=False; s.pressAt=-99999
    def press(s,t): s.touchOn=True; s.pressAt=t
    def lift(s,t): s.touchOn=False
    def longFires(s,t): s.depth=1                       # held long enough: go in
    def jolt(s,t):
        byHand = s.touchOn or (t - s.pressAt) < AFTER
        if s.guard and byHand: return
        if s.depth>0: s.depth-=1

def t_the_bug():
    # press, hold, it goes in, and the press itself jolts the robot
    old = Bot(guard=False); old.press(0); old.longFires(LONG); old.jolt(LONG+5)
    assert old.depth == 0, "model wrong"
    new = Bot(guard=True);  new.press(0); new.longFires(LONG); new.jolt(LONG+5)
    assert new.depth == 1, "the press still shakes itself back out"
    print(f"        unguarded: in then straight back out. guarded: stays in.")
run("Pressing the pad no longer undoes the press", t_the_bug)

def t_lift_jolt():
    # and the jolt as your finger comes off
    b = Bot(guard=True); b.press(0); b.longFires(LONG); b.lift(LONG+40); b.jolt(LONG+60)
    assert b.depth == 1, "letting go shook it back out"
run("Nor does letting go of it", t_lift_jolt)

def t_real_shake_works():
    b = Bot(guard=True); b.press(0); b.longFires(LONG); b.lift(LONG+40)
    b.jolt(LONG + 40 + AFTER + 100)        # a proper shake, well after
    assert b.depth == 0, "a real shake no longer goes back"
run("A real shake, a moment later, still goes back", t_real_shake_works)

def t_guard_in_source():
    # 5.3.0 widened it to the lift as well as the press. The jolt that
    # was actually getting through was the one from letting go, which
    # lands after the long press has already fired. sim_v53 runs it.
    assert "uint32_t sinceHand = now - (touchLiftAt > touchPressAt ? touchLiftAt : touchPressAt);" in src, \
        "the guard is not there, or no longer covers the lift"
    assert "bool byHand = touchOn || sinceHand < SHAKE_AFTER_MS;" in src, "the guard is not formed"
    assert "if (cfgShake && !byHand &&" in src, "the guard is not applied"
run("The guard is in the shake path, not somewhere decorative", t_guard_in_source)

print("\nthe reminder screen\n")
def t_screen():
    """5.2.0 took the title band off the reading view. 5.5.0 put a
    ribbon back, but a ribbon with the day on one edge and the time on
    the other, which is a header rather than a caption."""
    d = re.search(r"static void drawReminders\(\) \{.*?\n\}\n", src, re.S).group(0)
    reader = d[d.index("const Rem& r = rems[remIdx];"):]
    assert "bar(" not in reader, "the reading view is back to a plain title band"
    assert 'snprintf(ofN, sizeof(ofN), "%d of %d", remIdx + 1, remCount);' in reader, \
        "no which-of-how-many"
    assert "ctr(ofN, 56, 1);" in reader, "the count is not along the bottom"
    assert "bool hasWhen = r.at && timeOk;" in reader, \
        "it would stamp a reminder with a time the board invented"
    assert 'strftime(day, sizeof(day), "%a %d %b", &lt);' in reader and \
           'strftime(hm,  sizeof(hm),  "%H:%M", &lt);' in reader, \
        "the day and the time are not separate"
    assert "at(2, 2, day);" in reader, "the day is not on the left edge"
    assert "at(SCRW - 2 - (int)strlen(right) * 6, 2, right);" in reader, \
        "the time is not on the right edge"
    # the ribbon has to be drawn after the words, or fitText wipes it
    assert reader.index("fitText(") < reader.index("oled.fillRect(0, 0, SCRW, 12, SSD1306_WHITE);"), \
        "the ribbon is drawn before fitText, which cuts the window and paints over it"
    assert reader.index("fitText(") < reader.index("ctr(ofN, 56, 1);"), \
        "the count is drawn before fitText, which would wipe it"
    print("        ribbon after the words, day left, time right, count at 56")
run("The reading view is a ribbon and the whole reminder", t_screen)

def t_card():
    """The card that fires. No bar, a ribbon saying what a press does,
    the whole message under it, and a flash you cannot miss."""
    d = src[src.index("static void drawToast() {"):]
    d = d[:d.index("\n}")]
    loud = d[d.index("bool loud = "):d.index("} else {")]
    assert "oled.drawRect(15, 52" not in d, "the bar along the bottom is still there"
    assert 'ctr("2 to snooze 15 min", 2, 1);' in loud, "the ribbon does not say how to snooze"
    assert "fitText(" in loud, "the message is not fitted to the screen"
    assert loud.index("fitText(") < loud.index("oled.fillRect(0, 0, SCRW, 12, SSD1306_WHITE);"), \
        "the ribbon is drawn before fitText, which paints over it"
    assert "% 3000UL < 170UL" in loud, "it does not flash every three seconds"
    assert "SSD1306_INVERSE" in loud, "the flash blanks the words instead of turning them over"
    print("        no bar, ribbon, whole message, a flash every three seconds")
run("The card that fires is a ribbon and the whole reminder", t_card)

def t_ten_then_fifteen():
    show = int(re.search(r"#define REM_SHOW_MS (\d+)", src).group(1))
    waved = int(re.search(r"#define REM_WAVED_MIN (\d+)", src).group(1))
    steps = int(re.search(r"#define REM_STEPS (\d+)", src).group(1))
    assert show == 10000, f"it holds the screen for {show}ms"
    assert waved == 15, f"it snoozes for {waved} minutes"
    assert "toastUntil = millis() + REM_SHOW_MS;" in src, "the card does not take itself away"
    # unanswered and waved away have to mean the same thing now
    exp = src[src.index("if (toastUntil && now >= toastUntil) {"):]
    exp = exp[:exp.index("\n  }")]
    assert "REM_WAVED_MIN" in exp, "being ignored still uses a different interval"
    assert "r.tries++" in exp, "it would ask for ever"
    assert "if (r.tries >= REM_STEPS)" in exp, "it never gives up"
    print(f"        {show // 1000}s on screen, then {waved} min, {steps} times, then it lets go")
run("Ten seconds of asking, then fifteen minutes, seven times", t_ten_then_fifteen)

def t_fit():
    """Two big lines if they will go, small ones if not, a crawl when
    even small will not fit. Run the firmware's own numbers over real
    sentences and check each lands where it should."""
    # 5.5.0 pulled this out of drawReminders so the card that fires
    # uses the same rule. Both callers are checked below.
    r = re.search(r"static void fitText\(.*?\n\}\n", src, re.S).group(0)
    assert "int size = 2, n = wrapInto(text, 10, REM_LN);" in r, "it does not try big first"
    assert "if (n * 18 > h) { size = 1; n = wrapInto(text, 21, REM_LN); }" in r, \
        "it does not fall back to small"
    assert src.count("fitText(") >= 3, "only one screen uses it, so they can drift apart"
    top_with, top_without, bottom = 14, 3, 53
    def wrap(t, cols):
        out, pos = [], 0
        while pos < len(t):
            take = min(len(t) - pos, cols)
            if pos + take < len(t):
                sp = take
                while sp > 0 and t[pos + sp] != " ": sp -= 1
                if sp > 0: take = sp
            out.append(t[pos:pos + take]); pos += take
            while pos < len(t) and t[pos] == " ": pos += 1
        return out
    def fit(t, has_when=True):
        h = bottom - (top_with if has_when else top_without) + 1
        lines = wrap(t, 10)
        if len(lines) * 18 > h: return 1, wrap(t, 21), h
        return 2, lines, h
    cases = [("Call Amma", 2, False), ("Service the bike at 10", 1, False),
             ("Submit the Workday integration report before the review call", 1, False),
             ("Pick up the parcel from the gate office and sign for it before six, "
              "it closes early", 1, True)]
    for text, want_size, want_crawl in cases:
        size, lines, h = fit(text)
        lh = 18 if size == 2 else 10
        crawl = len(lines) * lh > h
        assert size == want_size, f"{text!r} drew at size {size}, wanted {want_size}"
        assert crawl == want_crawl, f"{text!r} crawl={crawl}, wanted {want_crawl}"
        assert all(len(l) <= (10 if size == 2 else 21) for l in lines), \
            f"{text!r} made a line too wide for the screen"
    # the longest thing that can be stored must still be readable
    cap = int(re.search(r"#define REM_TEXT (\d+)", src).group(1))
    size, lines, h = fit("w" * (cap - 1))
    assert len(lines) <= 10, f"a full-length reminder needs {len(lines)} lines, buffer holds 10"
    print(f"        {cap - 1} chars max, worst case {len(lines)} lines, all four cases land right")
run("Long words shrink, longer ones crawl, nothing is cut off silently", t_fit)

def t_tile():
    """The summary is an icon and a count, the way Settings is an icon
    and a word. It used to be a number in large type with no picture."""
    d = re.search(r"static void drawReminders\(\) \{.*?\n\}\n", src, re.S).group(0)
    tile = d[:d.index("// Past the last one")]
    assert "bellIcon(SCRW / 2, 30, 11);" in tile, "no icon on the tile"
    assert 'ctr(c, 46, 1);' in tile, "the count is not under the icon"
    for word in ('"nothing waiting"', '"1 reminder"', '"%d reminders"', '"all done"'):
        assert word in tile, f"the tile cannot say {word}"
    # the bell must fit between the title band and the two lines of text
    r = 11
    assert 30 - int(r * 0.78) - 3 > 10, "the bell reaches into the title band"
    assert 30 + r + 2 < 46, "the bell reaches into the count"
    print("        bell 19..43, count 46, hint 55")
run("The reminders tile is an icon with the count under it", t_tile)

def t_walk():
    m = re.search(r"if \(screen == S_REMIND && depth > 0\) \{.*?\n  \}", src, re.S).group(0)
    assert "case TG_ONE:  if (remIdx < remCount) remIdx++;" in m, "one does not advance"
    assert "remConfirm = true" in m, "the end does not offer to clear"
    assert "rems[remIdx].done = true" in m, "holding does not mark it done"
    assert "depth = 0" in m, "two does not come back out"
run("One advances, hold completes, the end offers to clear, two leaves", t_walk)

print("\nthe rest\n")
def t_bright_app():
    dv = open("rafiq-app/mac/Sources/Device.swift").read()
    fo = [int(x) for x in re.search(r"BRIGHT_OPTS\[\] = \{([^}]*)\}", src).group(1).split(",")]
    fn = re.findall(r'"([^"]*)"', re.search(r"BRIGHT_NAME\[\] = \{([^}]*)\}", src).group(1))
    an = re.findall(r'"([^"]*)"', re.search(r"brightNames = \[(.*?)\]", dv, re.S).group(1))
    av = [int(x) for x in re.search(r"brightVals  = \[(.*?)\]", dv, re.S).group(1).split(",")]
    assert an == fn, f"app names {an} vs robot {fn}"
    assert av == fo, f"app values {av} vs robot {fo}"
    # and the sleep list, same trap
    fs = [int(x) for x in re.search(r"SLEEP_OPTS\[\] = \{([^}]*)\}", src).group(1).split(",")]
    as_ = re.findall(r'"([^"]*)"', re.search(r"sleepNames  = \[(.*?)\]", dv, re.S).group(1))
    assert len(fs) == len(as_), f"robot has {len(fs)} sleep steps, app lists {len(as_)}"
    # and the vehicle layouts, which are also sent as an index
    ft = int(re.search(r"#define BIKE_TPL_N (\d+)", src).group(1))
    at = re.findall(r'"([^"]*)"', re.search(r"bikeTemplates = \[(.*?)\]", dv, re.S).group(1))
    assert len(at) == ft, f"robot has {ft} vehicle layouts, app lists {len(at)}"
    print(f"        brightness, sleep and vehicle lists agree: {len(fn)}, {len(fs)}, {ft} steps")
run("Every list the app sends an index for matches the robot's", t_bright_app)

def t_bright():
    o = [int(x) for x in re.search(r"BRIGHT_OPTS\[\] = \{([^}]*)\}", src).group(1).split(",")]
    n = re.findall(r'"([^"]*)"', re.search(r"BRIGHT_NAME\[\] = \{([^}]*)\}", src).group(1))
    assert o[0] != 0, "the 0 contrast step is still offered"
    assert n == ["10%","25%","50%","75%","100%"], n
    assert len(o) == len(n)
    print(f"        {n}")
run("The faintest step is gone and the names match the levels", t_bright)

def t_stays_online():
    assert "bool hadNet = false;" in src, "nothing remembers it was online"
    assert "&& !hadNet) {" in src, "a dropout still flips it to offline mid session"
    assert "hadNet = true;" in src, "it never records having been online"
run("Losing the signal mid session does not flip it to offline", t_stays_online)

def t_card():
    assert "static void remAddedCard(int n, uint32_t when)" in src, "nothing says one landed"
    assert src.count("remAddedCard(") >= 4, "not every way in says so"
    assert 'n == 1 ? "%d reminder added" : "%d reminders added"' in src, "it cannot count"
run("Every way of adding one puts a card on the screen", t_card)

def t_app():
    assert "store.remove(r)" in app, "no way to delete one in the app"
    assert "ForEach(store.pending) { r in" in app, "the app does not list them"
    assert "tray.and.arrow.up" in app, "nothing marks one as still queued"
    # 5.2.0: once, not twice. The same list was drawn above and below
    # the form, so two reminders read as four.
    assert app.count("ForEach(store.pending") == 1, "the list is drawn more than once"
    assert "store.update(r, text: editText" in app, "no way to change one in the app"
    assert "store.clearAll()" in app, "no way to empty it from the app"
    assert "private var bulk: String" in set_, "no bulk example"
    assert "api/rems?t=" in set_, "the bulk example is not a link"
    assert "private var curl: String" in set_, "no script form of the bulk call"
    for f in ('field("n",', 'field("t0..",', 'field("a0..",', 'field("clear",'):
        assert f in set_, f"the bulk call does not document {f}"
    for f in ('field("id",', 'field("drop",'):
        assert f in set_, f"editing a single reminder is not documented: {f}"
run("The app lists them once, edits, deletes, and documents the bulk call", t_app)

def t_app_mirrors():
    """The robot's list is the real one: reminders arrive there from a
    plain URL and never pass through this Mac. The app used to show
    only its own copy, so anything added by URL was invisible in it."""
    rem = open("rafiq-app/mac/Sources/Reminders.swift").read()
    dv  = open("rafiq-app/mac/Sources/Device.swift").read()
    assert "func fetchReminders() async -> (rems: [RobotRem], clock: Bool)?" in dv, \
        "the app cannot read the robot's list"
    assert "var robotId: UInt32?" in rem, "the app has no handle on a robot reminder"
    assert "func refresh() async {" in rem, "nothing merges the robot's list in"
    assert "items.removeAll { r in r.robotId.map { !live.contains($0) } ?? false }" in rem, \
        "clearing on the robot does not clear in the app"
    assert "await refresh()" in rem.split("func deliver()")[1][:1200], \
        "a push never learns the ids it was given"
    # a reminder still queued here must not be deleted for being absent
    merge = rem[rem.index("func refresh() async {"):]
    merge = merge[:merge.index("\n    }")]
    assert "r.robotId.map" in merge, "undelivered reminders would be swept away"
    assert "robotClock = clock" in merge, "the app ignores whether the robot has a clock"
    print("        robot list read, ids claimed, clears mirrored, queue left alone")
run("The app mirrors the robot's list rather than only its own", t_app_mirrors)

def t_app_text_cap():
    rem = open("rafiq-app/mac/Sources/Reminders.swift").read()
    dv  = open("rafiq-app/mac/Sources/Device.swift").read()
    cap = int(re.search(r"#define REM_TEXT (\d+)", src).group(1))
    app_cap = int(re.search(r"static let remTextMax = (\d+)", dv).group(1))
    assert app_cap == cap - 1, f"robot holds {cap - 1} characters, app sends {app_cap}"
    assert "String(r.text.prefix(Device.remTextMax))" in dv, "the push uses its own number"
    assert "prefix(Device.remTextMax)" in rem, "editing uses its own number"
    print(f"        both sides agree on {app_cap} characters")
run("The app never sends more words than the robot can keep", t_app_text_cap)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -",f) for f in fails]; sys.exit(1)
print("PASS")

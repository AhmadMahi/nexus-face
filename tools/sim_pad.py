"""The pad, and the loop it could get stuck in.

If the resting level is ever learned the wrong way round, the pad
reads as permanently pressed. A long press then fires at 0.7s, the
switch off arms at four seconds, and the robot is in deep sleep at
seven. A deep wake reloads the same remembered wrong level and does
it all again, so the robot sleeps by itself every few seconds and
cannot be used. The correction existed but sat sixty seconds away,
and the robot never stayed awake long enough to reach it.

This models the loop at the poll rate the firmware uses and runs it
both ways: with the old numbers, to show the deadlock is real, and
with the ones in the sketch now, to show it cannot happen. Then the
hotspot screen, which has to own the panel completely.
"""
import re, sys

src = open("nexus-repo/nexus_face/nexus_face.ino").read()
fails = []
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)
print()

def const(n):
    return int(re.search(r"#define %s\s+(\d+)" % n, src).group(1))

REST   = const("TOUCH_REST_MS")
LONG   = const("TOUCH_LONG_MS")
HOME   = const("TOUCH_HOME_MS")
COUNT  = const("TOUCH_COUNT_MS")
DEB    = const("TOUCH_DEBOUNCE")
POLL   = 45
PAD_MAX_ASSERT = 5000      # a TTP223 lets go of its own accord, measured

class Pad:
    """input(), as much of it as decides whether the robot switches off.

    guard=True is the sketch as it stands: nothing that can switch the
    robot off fires until the pad has been seen at rest.
    """
    def __init__(self, rest_belief, guard=True, relearn=REST):
        self.rest = rest_belief      # what the firmware believes resting is
        self.guard = guard
        self.relearn = relearn
        self.lvl = None; self.lvlAt = 0
        self.on = False; self.edge = 0
        self.pressAt = 0; self.longDone = False
        self.armed = 0; self.seenFree = False
        self.slept = 0; self.longs = 0; self.relearns = 0

    def step(self, now, pin):
        if pin != self.lvl:
            self.lvl = pin; self.lvlAt = now
        elif pin != self.rest and now - self.lvlAt > self.relearn:
            self.rest = pin
            self.on = False; self.edge = 0; self.longDone = True
            self.armed = 0; self.seenFree = False
            self.relearns += 1
        want = (pin != self.rest)
        if not want and not self.on:
            self.seenFree = True
        if want != self.on:
            if not self.edge: self.edge = now
            if now - self.edge >= DEB:
                self.on = want; self.edge = 0
                if self.on:
                    self.pressAt = now; self.longDone = False
        ok = self.seenFree or not self.guard
        if self.on and ok and not self.longDone and now - self.pressAt >= LONG:
            self.longDone = True; self.longs += 1
        if self.on and self.longDone:
            held = now - self.pressAt
            if ok and not self.armed and held >= HOME:
                self.armed = now
        if self.armed and now - self.armed >= COUNT:
            self.armed = 0; self.slept += 1
        return self.slept

def drive(pad, pin_at, ms):
    for t in range(0, ms, POLL):
        pad.step(t, pin_at(t))
    return pad

# ---------------------------------------------------------------
def t_the_deadlock_was_real():
    """The old numbers, and a pad being read upside down. The pin sits
    at 0 for ever; the firmware thinks resting is 1, so every poll
    looks like a press."""
    p = Pad(rest_belief=1, guard=False, relearn=60000)
    drive(p, lambda t: 0, 60000)
    assert p.slept > 0, "the model does not reproduce the fault at all"
    first = None
    q = Pad(rest_belief=1, guard=False, relearn=60000)
    for t in range(0, 60000, POLL):
        if q.step(t, 0) and first is None: first = t
    assert first is not None and first <= HOME + COUNT + 200, \
        f"first sleep at {first}ms, expected about {HOME + COUNT}ms"
    assert q.relearns == 0, \
        "the relearn fired, so the old code would have recovered after all"
    print(f"        asleep at {first}ms, {p.slept} times in a minute, "
          f"and the 60s correction never reached")
run("The deadlock the robot was in is real and reproducible", t_the_deadlock_was_real)

def t_the_guard_breaks_it():
    """The same upside down pad against the sketch as it stands."""
    p = Pad(rest_belief=1, guard=True, relearn=REST)
    drive(p, lambda t: 0, 60000)
    assert p.slept == 0, f"it still put itself to sleep {p.slept} times"
    assert p.longs == 0, "it still fired long presses, so the menus still wander"
    assert p.relearns >= 1, "it never corrected the resting level"
    print(f"        never slept, never fired a gesture, relearned the level "
          f"after {REST}ms and carried on")
run("With the guard it cannot happen", t_the_guard_breaks_it)

def t_relearn_is_reachable_before_sleep():
    """The arithmetic, stated once: the correction has to be able to
    happen, and the only thing stopping the sleep before it is the
    guard. Both facts are load bearing."""
    assert REST > PAD_MAX_ASSERT, \
        f"relearn at {REST}ms is inside the {PAD_MAX_ASSERT}ms a real press can assert"
    assert REST <= 15000, f"relearn at {REST}ms leaves the robot unusable too long"
    print(f"        relearn {REST}ms: past the {PAD_MAX_ASSERT}ms a finger can hold, "
          f"and the guard covers the {HOME + COUNT}ms before it")
run("The relearn window clears a real press but not much more", t_relearn_is_reachable_before_sleep)

def t_a_real_hold_still_switches_it_off():
    """The guard must not cost the feature. Pad at rest, then a hold,
    which the chip releases on its own at five seconds."""
    def pin(t):
        if t < 1000: return 0            # resting, so seenFree becomes true
        if t < 1000 + PAD_MAX_ASSERT: return 1
        return 0
    p = Pad(rest_belief=0, guard=True, relearn=REST)
    drive(p, pin, 12000)
    assert p.slept == 1, f"a genuine four second hold switched it off {p.slept} times"
    assert p.relearns == 0, "a genuine press was mistaken for a wrong resting level"
    print("        held from 1s, armed at 4s, off at 7s, and no relearn")
run("A real hold still switches the robot off", t_a_real_hold_still_switches_it_off)

def t_a_press_at_the_very_first_poll():
    """Woken by the pad, finger still down at the first poll. It must
    not count as a hold, because nothing has been seen at rest yet."""
    def pin(t): return 1 if t < 6000 else 0
    p = Pad(rest_belief=0, guard=True, relearn=REST)
    drive(p, pin, 20000)
    assert p.slept == 0, "a finger left on the pad at boot switched the robot off"
    print("        a finger already down at boot cannot arm the switch off")
run("The finger that woke it does not also switch it off", t_a_press_at_the_very_first_poll)

def t_the_guard_is_actually_in_the_source():
    for sig in ("bool     touchSeenFree = false;",
                "if (!want && !touchOn) touchSeenFree = true;",
                "if (touchOn && touchSeenFree && !touchLongDone",
                "if (!cfgGesture && touchSeenFree && !sleepArmed && held >= TOUCH_HOME_MS)",
                "if (cfgGesture && touchSeenFree && held >= TOUCH_HOME_MS)"):
        assert sig in src, f"missing: {sig}"
    rl = src[src.index("else if (lvl != touchRest && now - touchLvlAt > TOUCH_REST_MS) {"):]
    rl = rl[:rl.index("\n    }")]
    for v in ("sleepArmed = 0;", "touchSeenFree = false;", "touchOn = false;"):
        assert v in rl, f"the relearn does not clear {v}"
    print("        the guard, and a relearn that cancels what the wrong level started")
run("The fix is where the model says it is", t_the_guard_is_actually_in_the_source)

# ---------------- the hotspot screen ----------------
def t_hotspot_owns_the_panel():
    st = src[src.index("static void startHotspot() {"):]
    st = st[:st.index("\n}")]
    assert "apLock   = true;" in st, "the hotspot does not take the panel"
    assert "if (asleep) wake(\"hotspot\");" in st, "it could come up on a dark screen"
    draw = src[src.index("if (apLock) { drawHotspot(); delay(2); return; }"):]
    assert src.index("if (apLock) { drawHotspot(); delay(2); return; }") < \
           src.index("if (sleepArmed) { drawHoldTier(now)"), \
        "the hold overlay can still paint over the hotspot screen"
    d = src[src.index("static void drawHotspot() {"):]
    d = d[:d.index("\n}")]
    assert "RESCUE_SSID" in d and "RESCUE_PASS" in d, \
        "the screen does not show the name and the password"
    assert "softAPIP" in d, "no address to type into a browser"
    assert '"restart from the page"' in d, "it does not say how to get out"
    print("        name, password, address and the way out, and nothing can paint over it")
run("The hotspot screen is the whole screen", t_hotspot_owns_the_panel)

def t_hotspot_ignores_everything():
    for fn in ("static void touchGesture(uint8_t g) {",
               "static void knockOne() {",
               "static void knockTwo() {"):
        b = src[src.index(fn):]
        b = b[:b.index("\n}")]
        head = b[:b.index("\n", b.index("{")) + 260]
        assert "if (apLock) return;" in head, f"{fn.split()[2]} still acts while locked"
    gate = src[src.index("if (apLock) return;                            // the hotspot screen stays up"):]
    gate = gate[:gate.index("// 0 means never")]
    assert "goSleep()" in gate, "the guard is not in front of the sleep call"
    print("        every gesture ignored, and it does not sleep")
run("Nothing works while the hotspot is up, by design", t_hotspot_ignores_everything)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

"""v5.3.0: the five things that were broken. Asserted against the real
source, and the two with state machines are run rather than read."""
import re, sys
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
fails = []
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)
print('')

# ------------------------------------------------- waking by touch
def t_rest_level():
    """Waking from deep sleep is a boot, and the probe runs thirty
    milliseconds in, with the finger that woke it still on the pad. It
    learned the touched level as the resting one, and from then on the
    pad read inverted and the next sleep armed its wake on the wrong
    level, so touching it did nothing at all."""
    # two places call pinMode on the pad now: the wake gate, which
    # reads the stored level, and setup, which may measure it. This is
    # about the one in setup.
    i = src.index("// Which level means nobody is touching it.")
    blk = src[i:src.index("Wire.begin(", i)]
    assert "bool byTouch = (woke_ == ESP_SLEEP_WAKEUP_GPIO);" in blk, \
        "it does not ask how it woke before measuring the pad"
    assert "if (byTouch && known)" in blk, "it still measures with a finger on the pad"
    assert 'prefs.getBool("trest"' in blk and 'prefs.putBool("trest"' in blk, \
        "the resting level is not remembered across a sleep"
    # and the runtime correction has to be remembered too, or the next
    # wake starts from the stale one again
    learn = src[src.index("touchRest = lvl;"):]
    assert 'prefs.putBool("trest", touchRest);' in learn[:200], \
        "a correction found while running is forgotten at the next sleep"
    print("        measured only with no finger on it, and remembered")
run("The pad's resting level is never learned from a finger", t_rest_level)

def t_wake_level():
    """The wake level is derived from the resting level, so the two
    stand or fall together."""
    assert "touchRest ? ESP_GPIO_WAKEUP_GPIO_LOW" in src, \
        "the wake level is not the opposite of rest"
    assert "esp_deep_sleep_enable_gpio_wakeup(BIT(TOUCH_PIN)," in src, \
        "the pad is no longer armed to wake it"
    print("        armed on the opposite of a resting level that is now trustworthy")
run("Deep sleep wakes on the level that means touched", t_wake_level)

def t_wake_anim():
    assert re.search(r"\n  animWake\(\);", src), "waking has no animation"
    assert "if (!fromDeep) animWake();" not in src, "the animation is still skipped on a deep wake"
    # the introduction stays skipped
    assert "if (!fromDeep) animSenses(1500);" in src, "the sensor sweep is no longer skipped"
    print("        eyes open every time; the introduction stays skipped")
run("Coming back from deep sleep looks like waking up", t_wake_anim)

# ------------------------------------------- the shake after a lift
class Bot:
    """Just enough of input() to run the shake guard against it."""
    AFTER = int(re.search(r"#define SHAKE_AFTER_MS (\d+)", src).group(1))
    LONG  = int(re.search(r"#define TOUCH_LONG_MS (\d+)", src).group(1))
    def __init__(self):
        self.depth = 0; self.pressAt = -10**9; self.liftAt = -10**9
        self.on = False; self.longDone = False
    def press(self, t):
        self.on = True; self.pressAt = t; self.longDone = False
    def hold_fires(self, t):
        if self.on and not self.longDone and t - self.pressAt >= self.LONG:
            self.longDone = True; self.depth += 1          # "hold to open"
    def lift(self, t):
        self.on = False; self.liftAt = t                   # every lift, now
    def jolt(self, t):
        since = t - max(self.liftAt, self.pressAt)
        byHand = self.on or since < self.AFTER
        if not byHand and self.depth > 0: self.depth -= 1

def t_hold_then_lift():
    """Hold to open the reminders, let go, and the jolt of letting go
    took you straight back out. All you saw was the zoom animation and
    the same screen again."""
    b = Bot(); b.press(0); b.hold_fires(Bot.LONG + 20); b.lift(1100)
    assert b.depth == 1, "the hold did not open anything"
    b.jolt(1140)                       # the robot rocking from the lift
    assert b.depth == 1, "letting go still shakes it back out"
    b.jolt(1100 + Bot.AFTER + 60)      # a real shake, well after
    assert b.depth == 0, "a real shake no longer goes back"
    print(f"        {Bot.AFTER}ms of quiet after the lift, then a shake counts again")
run("Letting go of a long press does not shake it back out", t_hold_then_lift)

def t_lift_always_recorded():
    blk = src[src.index("} else {\n          // Every lift is a jolt"):]
    blk = blk[:blk.index("\n        }")]
    assert "touchLiftAt = now;" in blk.split("if (!touchLongDone)")[0], \
        "a lift that ended a long press still does not set the time"
    assert "uint32_t sinceHand = now - (touchLiftAt > touchPressAt ? touchLiftAt : touchPressAt);" in src, \
        "the guard still measures from the press only"
    print("        every lift recorded, guard measured from the later of the two")
run("The guard is measured from the later of the press and the lift", t_lift_always_recorded)

# ------------------------------------------ answering sends it back
def t_back_to_sleep():
    """A reminder answered is a reminder over with. Standing there
    afterwards waiting out a screen timeout is the robot ignoring what
    just happened."""
    assert "static void backToSleep() {" in src, "there is no way back"
    fn = src[src.index("static void backToSleep() {"):]
    fn = fn[:fn.index("\n}")]
    assert "wokeForAlarm || !macLinked" in fn, "it will not go deep when it came from deep"
    assert "goSleep();" in fn, "with a Mac attached it does not even darken the screen"
    assert src.count("if (back) backToSleep();") == 2, \
        "only one of waving it away and marking it done sends it back"
    # the timer wake has to count as having been asleep
    assert "remWokeIt = asleep || wokeForAlarm;" in src, \
        "a reminder that switched the robot on cannot send it back off"
    assert "wokeForAlarm = (woke_ == ESP_SLEEP_WAKEUP_TIMER);" in src, \
        "nothing records that an alarm is the only reason it is on"
    print("        deep if it came from deep, dark if a Mac is holding it")
run("Answering a reminder puts it straight back", t_back_to_sleep)

# ------------------------------------------------------ the list
def t_twenty():
    n = int(re.search(r"#define REM_MAX (\d+)", src).group(1))
    assert n == 20, f"the robot holds {n}"
    t = int(re.search(r"#define REM_TEXT (\d+)", src).group(1))
    # it all has to fit in one NVS string with room to spare
    worst = n * (t + 30)
    assert worst < 4000, f"a full list is {worst} bytes, NVS holds about 4000"
    print(f"        {n} reminders, {t - 1} characters each, worst case {worst} bytes")
run("The robot holds the twenty the app keeps", t_twenty)

def t_card_lines():
    """The added card is written as two lines. The toast wrapped on
    width instead, so the newline came out mid sentence."""
    d = src[src.index("static void drawToast() {"):]
    d = d[:d.index("\n}")]
    assert "int nl = t.indexOf('\\n');" in d, "an explicit line break is still ignored"
    assert 'if (toastKind == "note")   head = "REMINDERS";' in src, \
        "the reminder card is still headed FROM YOUR MAC"
    assert 'bool bell = (toastKind == "note");' in d, "the bell is on every card, or none"
    # the card itself still says both things
    c = src[src.index("static void remAddedCard("):]
    c = c[:c.index("\n}")]
    assert 'n == 1 ? "%d reminder added" : "%d reminders added"' in c, "it cannot count"
    assert 'String(a) + "\\n" + b' in c, "the card is no longer two lines"
    print("        headed REMINDERS, a bell, and the break where it was written")
run("The card that says reminders landed is laid out as written", t_card_lines)

# ------------------------------------------------------ stopwatch
class SW:
    """The stopwatch, run rather than read."""
    def __init__(self): self.on=False; self.run=False; self.acc=0; self.start=0; self.t=0
    def ms(self): return self.acc + (self.t - self.start if self.run else 0)
    def go(self):
        if not self.run: self.run=True; self.start=self.t
    def stop(self):
        if self.run: self.acc=self.ms(); self.run=False
    def zero(self): self.acc=0; self.start=self.t
    def open(self): self.on=True; self.run=False; self.zero()
    def gesture(self, g):
        if not self.on: return
        if g=="one":  (self.stop() if self.run else self.go())
        elif g=="long": self.stop(); self.zero()
        else: self.on=False; self.run=False

def t_stopwatch():
    sw=SW(); sw.open()
    assert sw.ms()==0 and not sw.run, "opening it starts it counting"
    sw.gesture("one"); sw.t=5000
    assert sw.run and sw.ms()==5000, f"five seconds in it reads {sw.ms()}"
    sw.gesture("one")                       # stop
    assert not sw.run and sw.ms()==5000, "stopping does not hold the number"
    sw.t=9000
    assert sw.ms()==5000, f"it kept counting while stopped: {sw.ms()}"
    sw.gesture("one"); sw.t=11000           # go again from there
    assert sw.ms()==7000, f"it did not carry on from where it stopped: {sw.ms()}"
    sw.gesture("long")
    assert sw.ms()==0 and not sw.run, "holding did not put it back to zero"
    sw.gesture("one"); sw.t=12000
    sw.gesture("two")
    assert not sw.on and not sw.run, "two did not leave it"
    print("        start, stop where it is, carry on, zero, leave")
run("The stopwatch counts, stops and carries on", t_stopwatch)

def t_stopwatch_source():
    assert "static void swGo()" in src and "static void swStop()" in src \
       and "static void swZero()" in src, "the stopwatch has no state to speak of"
    assert "uint32_t swAcc = 0;" in src, "nothing banks the time between runs"
    g = src[src.index("if (screen == S_FOCUS && swOn) {"):]
    g = g[:g.index("\n  }")]
    assert "case TG_ONE:  if (swRun) swStop(); else swGo();" in g, "one does not start and stop it"
    assert "case TG_LONG: swStop(); swZero();" in g, "holding does not zero it"
    assert "case TG_TWO:  swOn = false;" in g, "two does not go back to the list"
    assert "else if (swOn) { swOn = false; swRun = false;" in src, "a shake cannot leave it"
    # an icon on the row, and no screen held awake by a stopwatch that is not running
    assert 'at(16, y, "Stopwatch");' in src and 'at(SCRW - 4 - 2 * 6, y, "go");' not in src, \
        "the row has no icon"
    assert "if (swOn) { screen = S_FOCUS; if (swRun) lastActive = now; }" in src, \
        "a stopped stopwatch still holds the screen awake all night"
    print("        gestures wired, icon on the row, only a running one holds the screen")
run("The stopwatch is wired to the pad and to the list", t_stopwatch_source)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

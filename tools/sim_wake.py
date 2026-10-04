"""Waking by hold, and the alarm surviving a touch that was nothing.

The dangerous part is not the gate, it is what a refused wake does on
its way back down: the deep sleep timer is set at sleep time, so a
brush that goes back to sleep without re-arming it silently cancels
the next reminder. Every case here checks the alarm as well as the
wake.
"""
import re, sys
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
fails = []
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)

OPTS = [int(x) for x in re.search(r"WAKE_OPTS\[\] = \{([^}]*)\}", src).group(1).split(",")]
NAMES = re.findall(r'"([^"]*)"', re.search(r"WAKE_NAME\[\] = \{([^}]*)\}", src).group(1))
RING_AFTER = int(re.search(r"#define WAKE_RING_AFTER (\d+)", src).group(1))
RING_MIN   = int(re.search(r"#define WAKE_RING_MIN\s+(\d+)", src).group(1))
RELEASE_MS = int(re.search(r"#define SLEEP_RELEASE_MS (\d+)", src).group(1))
RETRY_S    = int(re.search(r"#define SLEEP_RETRY_S\s+(\d+)", src).group(1))
DEFAULT    = int(re.search(r'prefs\.getInt\("wakeh", (\d+)\)', src).group(1))
print(f"\nwake on hold {dict(zip(NAMES, OPTS))}, default {NAMES[DEFAULT]}\n")

def t_options():
    assert OPTS == [0, 1000, 3000], OPTS
    assert NAMES == ["off", "1s", "3s"], NAMES
    assert DEFAULT == 2, f"the default is {NAMES[DEFAULT]}"
    assert len(OPTS) == len(NAMES) == 3
    print(f"        {NAMES}, default {NAMES[DEFAULT]}")
run("Off, one second, three seconds, and three is the default", t_options)

# ------------------------------------------------------ the model
class Board:
    """A board asleep, and what a touch does to it. Times in ms.
    `clock` is the wall clock in seconds; `alarm` is when the next
    reminder is due, as the firmware stores it in RTC memory."""
    def __init__(self, gate_idx=DEFAULT, alarm=None, clock=1000):
        self.need = OPTS[gate_idx]
        self.clock = clock
        self.alarm = alarm                 # absolute, like rtcAlarmAt
        self.state = "deep"
        self.lit = False                   # did the screen come on
        self.cost_ms = 0                   # time with the chip running
        self.armed_timer = None            # what the re-sleep armed
        self.armed_pad = None

    def touch(self, hold_ms, release=True, now=None):
        """A touch of hold_ms. Returns 'woke' or 'slept'."""
        if now is not None: self.clock = now
        if self.state != "deep": return "already up"
        self.lit = False
        if self.need == 0:
            self.cost_ms += 1; self.state = "up"; return "woke"
        ring = self.need >= RING_MIN
        held = 0
        while held < hold_ms and held < self.need:
            held = min(held + 8, hold_ms, self.need)
            if ring and held >= RING_AFTER: self.lit = True
        self.cost_ms += held
        if held >= self.need:
            self.state = "up"; return "woke"
        # refused: back down, and the alarm must come with it
        secs = None
        if self.alarm is not None:
            secs = self.alarm - self.clock
            if secs <= 2:
                self.state = "up"; return "woke"      # due anyway, stay up
        self.sleep(secs)
        return "slept"

    def sleep(self, secs, pad_held=False):
        if pad_held:
            self.armed_pad = False
            secs = RETRY_S if (secs is None or secs > RETRY_S) else secs
        else:
            self.armed_pad = True
        self.armed_timer = secs
        self.state = "deep"

def t_brush_is_refused():
    for ms in (80, 200, 400, 900):
        b = Board()
        assert b.touch(ms) == "slept", f"a {ms}ms brush woke it"
        assert b.cost_ms <= ms, "it ran longer than the touch lasted"
    print("        80, 200, 400 and 900ms all refused")
run("A brush does not wake it", t_brush_is_refused)

def t_hold_wakes_it():
    for ms in (3000, 3200, 5000):
        b = Board()
        assert b.touch(ms) == "woke", f"a {ms}ms hold did not wake it"
    b = Board(gate_idx=1)
    assert b.touch(1000) == "woke" and Board(gate_idx=1).touch(600) == "slept"
    print("        three seconds wakes it; at the 1s setting one second does")
run("Holding it wakes it", t_hold_wakes_it)

def t_ring_only_when_worth_it():
    b = Board(gate_idx=2); b.touch(2000)
    assert b.lit, "three seconds of nothing, with no sign it is working"
    b = Board(gate_idx=2); b.touch(300)
    assert not b.lit, "a brush lit the screen"
    b = Board(gate_idx=1); b.touch(900)
    assert not b.lit, "one second is too short to be worth lighting the screen for"
    assert RING_AFTER < OPTS[2] and RING_AFTER > 0
    assert OPTS[1] < RING_MIN <= OPTS[2], \
        f"the ring threshold {RING_MIN} does not separate {OPTS[1]} from {OPTS[2]}"
    print(f"        screen at {RING_AFTER}ms, and only for a gate of {RING_MIN}ms or more")
run("The ring appears only where the wait is long enough to need it", t_ring_only_when_worth_it)

def t_alarm_survives():
    """The one that matters. A brush at eight o'clock must not cancel
    the nine o'clock reminder."""
    due = 1000 + 3600                       # an hour away
    b = Board(alarm=due)
    assert b.touch(200) == "slept", "the brush woke it"
    assert b.armed_timer == 3600, f"it re-armed for {b.armed_timer}s, not 3600"
    assert b.armed_pad, "it went back down without the pad able to wake it"
    # and again, later, still right
    b.touch(150, now=1000 + 1800)
    assert b.armed_timer == 1800, f"the second brush left {b.armed_timer}s"
    # and the alarm still lands
    b.touch(100, now=due - 1)
    assert b.state == "up", "a brush a second before it was due put it back to sleep"
    print("        brushed at an hour out and at half an hour out, still due on time")
run("A refused wake re-arms the alarm it was holding", t_alarm_survives)

def t_no_alarm():
    b = Board(alarm=None)
    assert b.touch(200) == "slept"
    assert b.armed_timer is None, "it invented an alarm it did not have"
    assert b.armed_pad, "it went down with no way to be woken at all"
    print("        nothing pending, so nothing armed but the pad")
run("With nothing due it arms nothing but the pad", t_no_alarm)

def t_something_resting_on_it():
    """A pad held for ever cannot have a level wake armed on it: that
    fires the instant it sleeps. It has to go down on the clock and
    come back to look."""
    b = Board(); b.sleep(None, pad_held=True)
    assert b.armed_pad is False, "it armed a wake that would fire at once"
    assert b.armed_timer == RETRY_S, f"it will not look again for {b.armed_timer}s"
    b2 = Board(); b2.sleep(30, pad_held=True)
    assert b2.armed_timer == 30, "a nearer alarm was pushed back by the retry"
    print(f"        no pad wake while it is held, and another look in {RETRY_S}s")
run("Something resting on the pad does not spin it", t_something_resting_on_it)

def t_cost():
    """What the whole thing is for."""
    awake_ms = 30000                        # a modest screen timeout
    brush = Board(); brush.touch(200)
    assert brush.cost_ms < awake_ms / 50, \
        f"a brush still costs {brush.cost_ms}ms against {awake_ms}ms awake"
    print(f"        a brush runs the chip for {brush.cost_ms}ms instead of {awake_ms}ms")
run("A refused wake costs a fraction of a real one", t_cost)

# ----------------------------------------------------- the source
def t_gate_runs_first():
    setup = src[src.index("void setup()"):]
    setup = setup[:setup.index("\nvoid loop()")]
    i = setup.index("if (woke_ == ESP_SLEEP_WAKEUP_GPIO) wakeGate();")
    before = setup[:i]
    for bad in ("prefs.begin", "Wire.begin", "oled.begin", "WiFi.", "startSensors",
                "analogSetPinAttenuation", "readBattery", "delay(300)"):
        assert bad not in before, f"{bad} runs before the gate decides"
    assert "cBoot" not in before, "a touch that was nothing still counts as a boot"
    print("        nothing but Serial.begin runs ahead of it")
run("The gate runs before anything costs anything", t_gate_runs_first)

def t_gate_body():
    g = src[src.index("static void wakeGate() {"):]
    g = g[:g.index("\nstatic void goDeep()")]
    assert 'touchRest = prefs.getBool("trest", false);' in g, \
        "it measures the resting level with a finger on the pad again"
    assert "if (!need || noDeep) return;" in g, "the off setting does not switch it off"
    assert "WiFi" not in g, "the gate brings the radio up"
    assert "startSensors" not in g, "the gate starts the sensors"
    assert "ringArc(" in g, "there is no ring"
    assert "SSD1306_DISPLAYOFF" in g, "the screen is left on after a refused wake"
    assert "if (secs <= 2) {" in g, "a brush just before an alarm would sleep through it"
    assert "sleepNow(secs);" in g, "it does not go back down properly"
    print("        reads the pad, lights only the screen, and hands the alarm on")
run("The gate touches nothing it does not need", t_gate_body)

def t_sleep_waits():
    n = src[src.index("static void sleepNow(long secs) {"):]
    n = n[:n.index("\nstatic void wakeGate()")]
    assert "while (held && millis() - t0 < SLEEP_RELEASE_MS)" in n, \
        "it sleeps with a finger on the pad, which wakes it again at once"
    assert "if (held) {" in n and "SLEEP_RETRY_S" in n, \
        "a pad held for ever has no way out"
    assert "esp_deep_sleep_enable_gpio_wakeup" in n, "the pad cannot wake it"
    # goDeep has to go through it, not around it
    d = src[src.index("static void goDeep() {"):]
    d = d[:d.index("\nstatic void wake(const char* why)")]
    assert "sleepNow(secs);" in d, "goDeep still sleeps on its own terms"
    assert "esp_deep_sleep_start()" not in d, "goDeep still has its own way down"
    assert "rtcAlarmAt = (secs > 0 && timeOk)" in d, "it does not leave the alarm behind"
    print(f"        waits up to {RELEASE_MS // 1000}s for the finger, both paths")
run("Nothing sleeps with a finger still on the pad", t_sleep_waits)

def t_press_swallowed():
    assert "touchLongDone = true; touchTaps = 0;\n  }" in src, \
        "the press that woke it is not spent"
    assert "if (digitalRead(TOUCH_PIN) != touchRest) {\n    touchOn = true;" in src, \
        "the finger still on the pad is not accounted for"
    print("        the hold that woke it does not also open something")
run("The press that woke it is not also a command", t_press_swallowed)

# ------------------------------------------- the clock across sleeps
def t_resync():
    """The RTC keeps running through deep sleep but it drifts, and a
    board that wakes and sleeps a dozen times a day accumulates that
    until a reminder set for nine goes off at some other time. It used
    to ask for the time only when it had none, and after a deep wake
    it always thinks it has one."""
    blk = src[src.index("if (online()) {"):]
    blk = blk[:blk.index("if ((long)(now - nextWx) >= 0)")]
    assert "(long)(now - nextResync) >= 0" in blk, \
        "it only asks for the time when it has none, so it never corrects the drift"
    assert "nextResync  = now + TIME_RESYNC_MS;" in blk, "it does not space the asks out"
    assert "unsigned long nextResync = 0;" in src, \
        "the first pass after a wake does not ask"
    hours = int(re.search(r"#define TIME_RESYNC_MS \((\d+)UL \* 3600000UL\)", src).group(1))
    assert 1 <= hours <= 24, f"re-asking every {hours} hours is not sensible"
    # and the number it lands on is what the next sleep is worked out from
    d = src[src.index("static void goDeep() {"):]
    d = d[:d.index("\nstatic void wake(const char* why)")]
    assert "long pray = secsToNextAlert();" in d and "long rem  = secsToNextRem();" in d, \
        "the alarm is not worked out from the clock at sleep time"
    assert "rtcAlarmAt = (secs > 0 && timeOk)" in d, "the answer is not carried into the sleep"
    print(f"        asked on every wake with a network, and every {hours} hours after")
run("The clock is asked for again on every wake, not only when it is missing", t_resync)

def t_answer_and_back():
    """Woken by a reminder, answered, and straight back down. Waking
    that way is a boot, so nothing downstream could tell it from
    someone picking the robot up."""
    assert "wokeForAlarm = (woke_ == ESP_SLEEP_WAKEUP_TIMER);" in src, \
        "nothing records that an alarm is the only reason it is on"
    assert "remWokeIt = asleep || wokeForAlarm;" in src, \
        "a reminder that switched the robot on cannot send it back off"
    assert src.count("if (back) backToSleep();") == 2, \
        "only one of waving it away and marking it done sends it back"
    fn = src[src.index("static void backToSleep() {"):]
    fn = fn[:fn.index("\n}")]
    assert "wokeForAlarm || !macLinked" in fn, "it will not go deep when it came from deep"
    assert "goSleep();" in fn, "with a Mac attached it does not even darken the screen"
    print("        answered is answered: deep again, or dark if a Mac is holding it")
run("A reminder that woke it sends it back the moment it is answered", t_answer_and_back)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

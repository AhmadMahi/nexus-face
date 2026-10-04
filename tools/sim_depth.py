"""Navigation, run rather than read.

The reminders could not be opened for three releases. The press worked,
the animation played, and then loop() cleared depth on its very next
pass because S_REMIND was missing from the list of screens that have an
inside. Every check written for it asserted the gesture handler, which
was correct, and none of them ran a loop pass afterwards, which is where
it died.

So this one models the loop. Nothing here asserts that a line of code
exists; it walks the journey and looks at what is on the screen.
"""
import re, sys
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
fails = []
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)

SCREENS = [x.strip() for x in
           re.split(r"[,\s]+", re.search(r"enum \{ (S_HOME = 0,.*?)\};", src, re.S).group(1))
           if x.strip().startswith("S_")]
SCREENS = ["S_HOME"] + [s for s in SCREENS if s not in ("S_HOME", "S_COUNT")]

# The rule, taken from the firmware rather than restated here. Older
# firmware wrote it out by hand in the middle of loop(); read that form
# too, so this file can be pointed at an older build and will say the
# same thing about it that it says about this one.
m = re.search(r"static bool screenHasDepth\(int s\) \{(.*?)\}", src, re.S)
if m:
    HAS_DEPTH = set(re.findall(r"s == (S_\w+)", m.group(1)))
else:
    m = re.search(r"if \((screen != S_\w+(?:\s*&&\s*screen != S_\w+)*)\s*&&\s*depth\)", src)
    assert m, "cannot find the rule that clears depth in this source"
    HAS_DEPTH = set(re.findall(r"S_\w+", m.group(1)))
print(f"\nscreens with an inside: {sorted(HAS_DEPTH)}\n")

# ------------------------------------------------- the static trap
def t_every_depth_screen_listed():
    """What should have been checked the first time. Any screen that
    puts a depth on itself and is not in the rule cannot be entered."""
    missing = []
    for s in SCREENS:
        sets = re.findall(rf"screen == {s}[^\n]*\n(?:[^\n]*\n){{0,3}}?[^\n]*depth = ([1-9])", src)
        if sets and s not in HAS_DEPTH:
            missing.append(f"{s} sets depth {sorted(set(sets))}")
    assert not missing, "; ".join(missing) + " but loop() will clear it"
    print(f"        every screen that sets a depth is in the rule")
run("No screen can set a depth the loop will throw away", t_every_depth_screen_listed)

def t_rule_is_in_one_place():
    assert "static bool screenHasDepth(int s)" in src, "the rule has no name"
    assert "if (depth && !screenHasDepth(screen))" in src, "the loop does not use the rule"
    assert "screen != S_FAITH && screen != S_READS" not in src, \
        "the old hand written list is still in the loop"
    print("        one function, used by the loop, no second copy")
run("The rule lives in one place", t_rule_is_in_one_place)

# ------------------------------------------------------- the model
class Robot:
    """screen, depth and the reminder cursor, plus the loop pass that
    was the whole problem. Gesture rules mirror touchGesture()."""
    def __init__(self, rems=5):
        self.screen = "S_HOME"; self.depth = 0; self.remIdx = 0
        self.remCount = rems; self.confirm = False
        self.done = [False] * rems
        self.carousel = [s for s in SCREENS if s not in ("S_BIKE", "S_WEATHER")]

    def tick(self):
        """One pass through loop(), which runs every couple of
        milliseconds between anything a finger does."""
        if self.depth and self.screen not in HAS_DEPTH:
            self.depth = 0

    def gesture(self, g):
        if self.screen == "S_REMIND" and self.depth > 0:
            if self.confirm:
                if g == "long": self.confirm = False; self.depth = 0
                elif g == "two": self.confirm = False
                return
            if g == "one":
                if self.remIdx < self.remCount: self.remIdx += 1
            elif g == "two":
                if self.remIdx > 0: self.remIdx -= 1
                else: self.depth = 0
            elif g == "long":
                if self.remIdx >= self.remCount and self.remCount:
                    self.confirm = True
                elif self.remIdx < self.remCount:
                    self.done[self.remIdx] = True; self.remIdx += 1
            return
        if g == "long" and self.screen == "S_REMIND" and self.depth == 0 and self.remCount:
            self.depth = 1; self.remIdx = 0; self.confirm = False
            return
        if g == "one" and self.depth == 0:
            i = self.carousel.index(self.screen)
            self.screen = self.carousel[(i + 1) % len(self.carousel)]

    def showing(self):
        """What a person sees."""
        if self.screen != "S_REMIND": return self.screen
        if self.depth == 0: return f"summary: {sum(1 for d in self.done if not d)} waiting"
        if self.confirm: return "clear them all?"
        if self.remIdx >= self.remCount: return "that is all of them"
        return f"reminder {self.remIdx + 1} of {self.remCount}"

def toReminders(b):
    for _ in range(len(b.carousel) + 1):
        b.tick()
        if b.screen == "S_REMIND": return
        b.gesture("one")
    raise AssertionError("the reminders are not reachable from the clock at all")

def t_hold_opens_them():
    """The report, exactly: hold on the reminders and it zooms and
    comes straight back to five waiting."""
    b = Robot(rems=5); toReminders(b)
    assert b.showing() == "summary: 5 waiting", b.showing()
    b.gesture("long")
    b.tick()                      # the pass that used to undo it
    assert b.showing() == "reminder 1 of 5", \
        f"holding showed {b.showing()!r}: it opened and the loop closed it again"
    for _ in range(50): b.tick()  # and it stays open
    assert b.showing() == "reminder 1 of 5", f"it closed itself after a moment: {b.showing()!r}"
    print("        hold opens the first one, and it stays open")
run("Holding on the reminders shows the first reminder", t_hold_opens_them)

def t_walk_all_of_them():
    b = Robot(rems=5); toReminders(b); b.gesture("long"); b.tick()
    seen = []
    for _ in range(5):
        seen.append(b.showing())
        b.gesture("one"); b.tick()
    assert seen == [f"reminder {i} of 5" for i in range(1, 6)], seen
    assert b.showing() == "that is all of them", b.showing()
    b.gesture("long"); b.tick()
    assert b.showing() == "clear them all?", b.showing()
    print(f"        {' -> '.join(s.split(':')[0] for s in seen)} -> the end")
run("One press walks every reminder and stops at the offer to clear", t_walk_all_of_them)

def t_pressing_does_not_leave():
    """The other half of the report: a press went to the next menu,
    because depth had already been thrown away."""
    b = Robot(rems=5); toReminders(b); b.gesture("long"); b.tick()
    for _ in range(3):
        b.gesture("one"); b.tick()
        assert b.screen == "S_REMIND", "a press walked out of the reminders to another screen"
    print("        presses stay inside and turn the page")
run("Pressing inside the reminders does not walk out of them", t_pressing_does_not_leave)

def t_back_out():
    b = Robot(rems=3); toReminders(b); b.gesture("long"); b.tick()
    b.gesture("one"); b.tick()
    b.gesture("two"); b.tick()
    assert b.showing() == "reminder 1 of 3", b.showing()
    b.gesture("two"); b.tick()
    assert b.showing() == "summary: 3 waiting", f"two did not come back out: {b.showing()!r}"
    print("        two steps back a page, then out")
run("Two goes back a page and then leaves", t_back_out)

def t_hold_completes():
    b = Robot(rems=3); toReminders(b); b.gesture("long"); b.tick()
    b.gesture("long"); b.tick()           # mark the first done
    assert b.done[0] and b.showing() == "reminder 2 of 3", b.showing()
    print("        held, marked, moved on")
run("Holding inside marks one done and moves on", t_hold_completes)

def t_empty():
    b = Robot(rems=0); toReminders(b)
    b.gesture("long"); b.tick()
    assert b.showing() == "summary: 0 waiting", \
        "an empty list opened into nothing rather than staying put"
    print("        nothing to read, so holding does nothing")
run("An empty list cannot be opened", t_empty)

def t_twenty():
    n = int(re.search(r"#define REM_MAX (\d+)", src).group(1))
    b = Robot(rems=n); toReminders(b); b.gesture("long"); b.tick()
    for i in range(n):
        assert b.showing() == f"reminder {i+1} of {n}", b.showing()
        b.gesture("one"); b.tick()
    print(f"        all {n} of them, one press at a time")
run("A full list of twenty walks end to end", t_twenty)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

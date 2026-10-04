"""The update screen, and getting out of it.

It took over the panel and was driven only by knocks, which are off
unless you ask for them. So opening Check update put a screen in front
of you that the pad could not touch: presses fell through to the
carousel underneath, invisibly, and nothing came back. Not a crash, but
no way out short of the battery.
"""
import re, sys
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
fails = []
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)
print()

def t_pad_reaches_it():
    """Every screen that returns early from loop() has to be reachable
    by the pad, because the pad is how the robot is driven."""
    g = src[src.index("static void touchGesture(uint8_t g) {"):]
    g = g[:g.index("\n}")]
    head = g[:g.index("Serial.printf(\"touch gesture")]
    assert "if (upState != U_OFF) {" in head, \
        "the pad cannot reach the update screen, and it owns the panel"
    blk = head[head.index("if (upState != U_OFF) {"):]
    blk = blk[:blk.index("\n  }")]
    for want, why in [("case TG_ONE:  updateKnock(1);", "one does not walk the choices"),
                      ("case TG_LONG: updateKnock(2);", "holding does not pick one"),
                      ("case TG_TWO:  updateKnock(3);", "two does not go back"),
                      ("default:      upState = U_OFF;", "three presses cannot leave it")]:
        assert want in blk, why
    print("        one walks, hold picks, two backs out, three leaves")
run("The pad drives the update screen", t_pad_reaches_it)

def t_knocks_were_the_only_way():
    """What made it a trap: the knock path is behind a setting."""
    # 5.12.0 added one exception: gesture mode, where knocking IS
    # the mode. Everywhere else they are still off unless asked for.
    assert "if (!cfgKnock && !tapTesting && !cfgGesture) { burst = 0; return; }" in src, \
        "knocks are no longer optional, so this test has lost its point"
    assert src.count("updateKnock(") > 2, "the knock path is still the only caller"
    print("        knocks are off by default, so they cannot be the only way in")
run("Knocks being off cannot strand you on it", t_knocks_were_the_only_way)

def t_other_ways_out():
    """Two more, because one way out of a screen that owns the panel
    is one too few."""
    sh = src[src.index("if (cfgShake && !byHand"):]
    sh = sh[:sh.index("Serial.println(\"shake -> back\");")]
    assert "if (upState != U_OFF) { updateKnock(3); }" in sh, "a shake cannot leave it"
    assert "upState = U_OFF; swOn = false; swRun = false;" in src, \
        "going home leaves it owning the panel"
    print("        a shake and a five second hold both get out of it")
run("A shake and a long hold also get out of it", t_other_ways_out)

# --------------------------------------------------- walk the states
STATES = re.search(r"enum \{ (U_OFF = 0,.*?) \};", src).group(1)
STATES = [x.strip() for x in STATES.split(",")]
STATES = [s.split(" ")[0] for s in STATES]

class Up:
    """updateKnock(), run. n: 1 next, 2 choose, 3 back."""
    def __init__(self): self.st="U_MENU"; self.pick=0; self.sel=0; self.rels=3
    def knock(self, n):
        if self.st=="U_MENU":
            if n==1: self.pick=(self.pick+1)%2; return
            if n==2: self.st="U_LOOK"; return
            self.st="U_OFF"; return
        if self.st=="U_LOOK":
            if n>=3: self.st="U_MENU"
            return
        if self.st=="U_ASK":
            if n==1: self.yes=not getattr(self,"yes",True); return
            if n==2: self.st="installing" if getattr(self,"yes",True) else "U_MENU"; return
            self.st="U_MENU"; return
        if self.st=="U_LIST":
            if n==1: self.sel=(self.sel+1)%self.rels; return
            if n==2: self.st="U_ASK"; self.yes=True; return
            self.st="U_MENU"; return
        self.st="U_MENU"
    def gesture(self, g):
        if g=="one": self.knock(1)
        elif g=="long": self.knock(2)
        elif g=="two": self.knock(3)
        else: self.st="U_OFF"
    def answered(self, which):     # the network task coming back
        self.st = which

def t_latest_install():
    """Open it, hold on the newest release, say yes, and it installs.
    This is the path that did nothing at all with the pad."""
    u=Up()
    assert u.pick==0, "it does not start on the newest release"
    u.gesture("long")
    assert u.st=="U_LOOK", f"holding on the newest release went to {u.st}"
    u.answered("U_ASK")
    u.gesture("long")
    assert u.st=="installing", f"saying yes went to {u.st}"
    print("        hold, it looks, hold again, it installs")
run("Holding on the newest release installs it", t_latest_install)

def t_earlier_list():
    u=Up(); u.gesture("one")
    assert u.pick==1, "a press does not move to the earlier releases"
    u.gesture("long"); u.answered("U_LIST")
    seen=[u.sel]
    for _ in range(2): u.gesture("one"); seen.append(u.sel)
    assert seen==[0,1,2], seen
    u.gesture("long")
    assert u.st=="U_ASK", f"holding on one of the list went to {u.st}"
    u.gesture("long")
    assert u.st=="installing", f"saying yes went to {u.st}"
    print("        press to the list, press through it, hold to install")
run("The earlier releases can be walked and installed", t_earlier_list)

def t_always_a_way_back():
    """From any state, going back must eventually reach the menu and
    then leave entirely."""
    for st in ("U_MENU","U_LOOK","U_ASK","U_LIST","U_NONE","U_FAIL"):
        u=Up(); u.st=st
        for i in range(4):
            u.gesture("two")
            if u.st=="U_OFF": break
        assert u.st=="U_OFF", f"from {st} four backs left it in {u.st}"
        u=Up(); u.st=st; u.gesture("three")
        assert u.st=="U_OFF", f"from {st} three presses left it in {u.st}"
    print("        every state backs out, and three presses leave at once")
run("There is a way out of every state it can be in", t_always_a_way_back)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

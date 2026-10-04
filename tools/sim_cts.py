"""The clock from the phone.

Two kinds of check. The date arithmetic is extracted from the sketch,
compiled, and run against Python's own timegm over sixty years of
dates, because an off by one in a leap year rule is exactly the sort
of thing that reads correctly and is wrong. The state machine is
modelled and driven through the cases that matter: a phone with no
clock service, a phone that answers with rubbish, a connection that
drops halfway, and a pairing that has not finished.
"""
import re, sys, os, subprocess, calendar, random, tempfile

src = open("nexus-repo/nexus_face/nexus_face.ino").read()
fails = []
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)
print()

def const(name):
    m = re.search(r"#define %s\s+(\d+)" % name, src)
    assert m, name
    return int(m.group(1))

# ---------------------------------------------------------------
#  1. the date arithmetic, compiled and run
# ---------------------------------------------------------------
def t_dates():
    fn = src[src.index("static time_t utcFromTm(const struct tm* t) {"):]
    fn = fn[:fn.index("\n}") + 2]
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, "t.c")
        open(c, "w").write(
            "#include <stdio.h>\n#include <time.h>\n#include <stdlib.h>\n"
            + fn.replace("static time_t", "static long long")
                .replace("return (time_t)days", "return (long long)days")
            + """
int main(int argc, char** argv){
  struct tm t; int i;
  for (i = 1; i + 5 < argc; i += 6) {
    t.tm_year = atoi(argv[i]) - 1900; t.tm_mon = atoi(argv[i+1]) - 1;
    t.tm_mday = atoi(argv[i+2]);      t.tm_hour = atoi(argv[i+3]);
    t.tm_min  = atoi(argv[i+4]);      t.tm_sec  = atoi(argv[i+5]);
    printf("%lld\\n", utcFromTm(&t));
  }
  return 0;
}
""")
        exe = os.path.join(d, "t")
        r = subprocess.run(["cc", "-O1", "-o", exe, c], capture_output=True, text=True)
        assert r.returncode == 0, "the extracted function will not compile: " + r.stderr[:300]

        cases = []
        # every leap and century boundary that has ever caught anyone
        for y, m, dd in [(2024,2,29),(2000,2,29),(2100,2,28),(2023,12,31),
                         (2026,1,1),(2026,3,1),(2024,3,1),(2026,10,5)]:
            cases.append((y,m,dd,0,0,0)); cases.append((y,m,dd,23,59,59))
        rnd = random.Random(7)
        for _ in range(4000):
            y = rnd.randint(2020, 2080); m = rnd.randint(1,12)
            dd = rnd.randint(1, 28); 
            cases.append((y,m,dd,rnd.randint(0,23),rnd.randint(0,59),rnd.randint(0,59)))
        args = [str(x) for c in cases for x in c]
        out = subprocess.run([exe] + args, capture_output=True, text=True).stdout.split()
        assert len(out) == len(cases), f"{len(out)} answers for {len(cases)} dates"
        bad = []
        for c, got in zip(cases, out):
            want = calendar.timegm((c[0], c[1], c[2], c[3], c[4], c[5], 0, 0, 0))
            if int(got) != want: bad.append((c, int(got), want))
        assert not bad, f"{len(bad)} wrong, first {bad[0]}"
    print(f"        {len(cases)} dates including every leap and century case, all match timegm")
run("The date arithmetic is right, not just plausible", t_dates)

def t_no_timezone_in_it():
    """mktime would read TZ, and TZ is set from whatever last had an
    opinion. The whole point is that this value is absolute."""
    fn = src[src.index("static time_t utcFromTm"):]
    fn = fn[:fn.index("\n}")]
    for bad in ("mktime", "localtime", "getenv", "tzset"):
        assert bad not in fn, f"{bad} makes the answer depend on the timezone"
    print("        no mktime, no localtime: the answer does not depend on TZ")
run("The conversion cannot be moved by the timezone", t_no_timezone_in_it)

# ---------------------------------------------------------------
#  2. the state machine, driven
# ---------------------------------------------------------------
SETTLE  = const("CTS_SETTLE_MS")
ANSWER  = const("CTS_ANSWER_MS")
RETRY   = const("CTS_RETRY_MS")
REFRESH = const("CTS_REFRESH_MS")
GIVEUP  = const("CTS_GIVE_UP")

IDLE, LOOKING, GOT, NONE = range(4)

class Cts:
    """ctsTick plus the callbacks, as a machine you can run."""
    def __init__(self, phone_has_cts=True, phone_answers_rubbish=False):
        self.state = IDLE; self.askedAt = 0; self.syncedAt = 0; self.fails = 0
        self.bonded = False; self.btSince = 0; self.conn = False
        self.now = 0; self.asks = 0; self.setClock = 0; self.lastEpoch = None
        self.has = phone_has_cts; self.rubbish = phone_answers_rubbish
    def connect(self, t):
        self.bonded = True; self.conn = True; self.btSince = t
    def drop(self):
        self.conn = False; self.bonded = False
        self.state = IDLE; self.syncedAt = 0; self.fails = 0   # onDisconnect
    def ask(self):
        self.asks += 1; self.fails += 1
        self.state = LOOKING; self.askedAt = self.now
        if not self.has: return                       # silence: times out
        if self.rubbish: self.state = NONE; return    # rejected by the guard
        self.state = GOT; self.lastEpoch = 1760000000
    def tick(self, now):
        self.now = now
        if self.state == GOT:
            self.setClock += 1; self.syncedAt = now; self.fails = 0; self.state = IDLE
            return
        if not self.bonded or not self.conn: return
        if now - self.btSince < SETTLE: return
        if self.state == LOOKING:
            if now - self.askedAt > ANSWER: self.state = NONE
            return
        if self.state == NONE:
            if self.fails >= GIVEUP: return
            if now - self.askedAt < RETRY: return
            self.ask(); return
        if self.syncedAt == 0 or now - self.syncedAt > REFRESH: self.ask()

def drive(c, until, step=500):
    for t in range(0, until, step): c.tick(t)
    return c

def t_happy():
    c = Cts(); c.connect(0); drive(c, 20000)
    assert c.setClock == 1, f"the clock was set {c.setClock} times, wanted once"
    assert c.asks == 1, f"{c.asks} asks for one answer"
    print(f"        bonded at 0s, asked once, clock set once")
run("A phone with a clock sets it, once", t_happy)

def t_not_before_the_bond():
    """Reading CTS before the link is encrypted gets an error from
    iOS, and burns one of the five tries doing it."""
    c = Cts(); c.conn = True; c.bonded = False
    drive(c, 60000)
    assert c.asks == 0, "it asked before the pairing finished"
    c.connect(60000); drive(c, 90000)
    assert c.asks == 1 and c.setClock == 1
    print("        silent until bonded, then asks")
run("It does not ask an unbonded phone", t_not_before_the_bond)

def t_settles_first():
    c = Cts(); c.connect(0)
    c.tick(SETTLE - 1)
    assert c.asks == 0, "asked before the bond had settled"
    c.tick(SETTLE + 1)
    assert c.asks == 1
    print(f"        waits {SETTLE}ms after the bond, then asks")
run("It lets the bond settle", t_settles_first)

def t_phone_without_a_clock():
    """A phone that never answers must not turn into a machine that
    asks forever over a radio you are trying to keep quiet."""
    c = Cts(phone_has_cts=False); c.connect(0)
    drive(c, 1000 * 1000)
    assert c.setClock == 0
    assert c.asks == GIVEUP, f"{c.asks} asks; it should stop at {GIVEUP}"
    print(f"        {c.asks} tries over {1000}s, then it stops asking")
run("A phone with no clock service is given up on", t_phone_without_a_clock)

def t_rubbish_is_refused():
    c = Cts(phone_answers_rubbish=True); c.connect(0)
    drive(c, 1000 * 1000)
    assert c.setClock == 0, "it believed a bad date"
    assert c.asks == GIVEUP
    print("        a nonsense date never reaches the clock")
run("Nonsense from the phone is refused, not believed", t_rubbish_is_refused)

def t_reconnect_starts_over():
    c = Cts(); c.connect(0); drive(c, 20000)
    assert c.setClock == 1
    c.drop()
    c.connect(30000)
    drive(c, 60000)
    assert c.setClock == 2, "it did not re-read the clock on a new connection"
    print("        a new connection means new handles, so it looks again")
run("Dropping and coming back re-reads the clock", t_reconnect_starts_over)

def t_refresh():
    c = Cts(); c.connect(0)
    for t in range(0, int(REFRESH) + 60000, 5000): c.tick(t)
    assert c.setClock == 2, f"set {c.setClock} times across one refresh window"
    print(f"        and again every {int(REFRESH)//3600000} hours")
run("It drifts back into step on its own", t_refresh)

def t_not_in_wifi_mode():
    assert 'if (cfgNet != NET_BT) return;' in \
        src[src.index("static void ctsTick() {"):][:200], \
        "it would run with no Bluetooth at all"
    print("        ctsTick is the first thing out of the way on WiFi")
run("None of this runs unless Bluetooth is the mode", t_not_in_wifi_mode)

def t_callbacks_do_not_touch_the_robot():
    """They run on the NimBLE host task. Drawing or sleeping from
    there is how you get a crash that only happens when a phone is
    nearby."""
    blob = src[src.index("static int ctsOnRead"):src.index("static void ctsTick")]
    for bad in ("oled.", "settimeofday", "flash(", "delay(", "screen =", "eyes."):
        assert bad not in blob, f"{bad} in a callback that is not on the main task"
    assert "settimeofday" in src[src.index("static void ctsTick"):], \
        "the clock is not actually set on the main task either"
    print("        callbacks only write down what they found")
run("The host task callbacks touch nothing they should not", t_callbacks_do_not_touch_the_robot)

def t_handles_are_not_reused_across_connections():
    dis = src[src.index("void onDisconnect(BLEServer* sv, ble_gap_conn_desc* d) override"):]
    dis = dis[:dis.index("\n  }")]
    for v in ("ctsState = CTS_IDLE", "ctsSyncedAt = 0", "ctsHandle = 0"):
        assert v in dis, f"{v} missing: a stale handle would be read on the next phone"
    print("        every handle is dropped when the connection goes")
run("Stale GATT handles cannot survive a disconnect", t_handles_are_not_reused_across_connections)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

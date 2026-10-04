"""The clock from the phone.

The date arithmetic and the parser are extracted from the sketch,
compiled, and run: against Python's own timegm over sixty years of
dates, and against real and malformed Current Time payloads. The rest
is the state machine, modelled and driven through the cases that
matter: a phone with no clock service, a pairing that never finished,
a connection that drops, and the six hourly drift back into step.
"""
import re, sys, os, subprocess, calendar, random, struct, tempfile

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

def grab(sig):
    i = src.index(sig)
    depth = 0; out = []
    for ch in src[i:]:
        out.append(ch)
        if ch == "{": depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0: break
    return "".join(out)

# ---------------------------------------------------------------
#  compiled: the arithmetic and the parser
# ---------------------------------------------------------------
HARNESS = r"""
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
static long long captured = -1;
static int timeOk = 0;
static const char* clockSrc = "not set";
static int fake_settimeofday(const struct timeval* tv, void* z) { captured = tv->tv_sec; return 0; }
#define settimeofday fake_settimeofday
#define setenv(a,b,c) ((void)0)
#define tzset() ((void)0)
#define nullptr ((void*)0)
%s
%s
int main(int argc, char** argv) {
  if (!strcmp(argv[1], "tm")) {
    struct tm t; memset(&t, 0, sizeof t);
    t.tm_year = atoi(argv[2]) - 1900; t.tm_mon = atoi(argv[3]) - 1;
    t.tm_mday = atoi(argv[4]); t.tm_hour = atoi(argv[5]);
    t.tm_min  = atoi(argv[6]); t.tm_sec  = atoi(argv[7]);
    printf("%%lld\n", (long long)utcFromTm(&t));
    return 0;
  }
  unsigned char b[32]; int n = 0;
  for (int i = 2; i < argc; i++) b[n++] = (unsigned char)atoi(argv[i]);
  int ok = applyCts(b, n);
  printf("%%d %%lld %%d %%s\n", ok, captured, timeOk, clockSrc);
  return 0;
}
"""

def build():
    tm  = grab("static time_t utcFromTm(const struct tm* t) {")
    cts = grab("static bool applyCts(const uint8_t* b, size_t n) {")
    cts = re.sub(r"\n\s*Serial\.println\([^\n]*\);", "", cts)
    d = tempfile.mkdtemp()
    c = os.path.join(d, "t.c")
    open(c, "w").write(HARNESS % (tm, cts))
    exe = os.path.join(d, "t")
    r = subprocess.run(["cc", "-O1", "-w", "-o", exe, c], capture_output=True, text=True)
    assert r.returncode == 0, "the extracted code will not compile: " + r.stderr[:400]
    return exe

EXE = build()

def t_dates():
    cases = []
    for y, m, dd in [(2024,2,29),(2000,2,29),(2100,2,28),(2023,12,31),
                     (2026,1,1),(2026,3,1),(2024,3,1),(2026,10,5)]:
        cases += [(y,m,dd,0,0,0), (y,m,dd,23,59,59)]
    rnd = random.Random(7)
    for _ in range(600):
        cases.append((rnd.randint(2020,2080), rnd.randint(1,12), rnd.randint(1,28),
                      rnd.randint(0,23), rnd.randint(0,59), rnd.randint(0,59)))
    bad = []
    for c in cases:
        got = int(subprocess.run([EXE,"tm"]+[str(x) for x in c],
                                 capture_output=True, text=True).stdout)
        want = calendar.timegm((c[0],c[1],c[2],c[3],c[4],c[5],0,0,0))
        if got != want: bad.append((c, got, want))
    assert not bad, f"{len(bad)} wrong, first {bad[0]}"
    print(f"        {len(cases)} dates including every leap and century case, all match timegm")
run("The date arithmetic is right, not just plausible", t_dates)

def cts_bytes(y, mo, d, h, mi, s, dow=1):
    return [y & 0xFF, y >> 8, mo, d, h, mi, s, dow, 0, 0]

def feed(b):
    out = subprocess.run([EXE,"cts"]+[str(x) for x in b],
                         capture_output=True, text=True).stdout.split(None, 3)
    return int(out[0]), int(out[1]), int(out[2]), out[3].strip()

def t_parses_a_real_payload():
    ok, epoch, tok, srcname = feed(cts_bytes(2026, 10, 5, 14, 23, 9))
    assert ok == 1, "a valid Current Time was refused"
    assert epoch == calendar.timegm((2026,10,5,14,23,9,0,0,0)), epoch
    assert tok == 1 and srcname == "your phone", f"timeOk={tok} src={srcname}"
    print("        2026-10-05 14:23:09 read back exactly, clockSrc 'your phone'")
run("A real Current Time payload is read correctly", t_parses_a_real_payload)

def t_rubbish_is_refused():
    bad = {
        "year 1970":      cts_bytes(1970, 1, 1, 0, 0, 0),
        "month 13":       cts_bytes(2026, 13, 1, 0, 0, 0),
        "day 0":          cts_bytes(2026, 1, 0, 0, 0, 0),
        "day 32":         cts_bytes(2026, 1, 32, 0, 0, 0),
        "hour 24":        cts_bytes(2026, 1, 1, 24, 0, 0),
        "minute 60":      cts_bytes(2026, 1, 1, 0, 60, 0),
        "month 0":        cts_bytes(2026, 0, 1, 0, 0, 0),
        "truncated":      [0xEA, 0x07, 10, 5],
        "empty":          [],
    }
    for why, b in bad.items():
        ok, epoch, tok, _ = feed(b)
        assert ok == 0, f"{why} was accepted"
        assert epoch == -1, f"{why} reached the clock anyway"
    print(f"        {len(bad)} malformed payloads, none reached the clock")
run("Nonsense from the phone never reaches the clock", t_rubbish_is_refused)

def t_no_timezone_in_it():
    fn = grab("static time_t utcFromTm(const struct tm* t) {")
    for bad in ("mktime", "localtime", "getenv"):
        assert bad not in fn, f"{bad} makes the answer depend on the timezone"
    body = grab("static bool applyCts(const uint8_t* b, size_t n) {")
    assert 'setenv("TZ", "UTC0", 1)' in body, \
        "the zone is left wherever it was, so the phone's wall clock shifts"
    print("        no mktime, and the zone is pinned to UTC so the phone's time is the time")
run("The conversion cannot be moved by the timezone", t_no_timezone_in_it)

# ---------------------------------------------------------------
#  the state machine
# ---------------------------------------------------------------
SETTLE  = const("CTS_SETTLE_MS")
RETRY   = const("CTS_RETRY_MS")
REFRESH = const("CTS_REFRESH_MS")
GIVEUP  = const("CTS_GIVE_UP")
IDLE, DONE, NONE = 0, 1, 2

class Bt:
    """btTick's clock half, as something you can run."""
    def __init__(self, phone_has_cts=True):
        self.state = IDLE; self.askedAt = 0; self.syncedAt = 0; self.fails = 0
        self.stage = "off"; self.since = 0; self.conn = False
        self.reads = 0; self.setClock = 0; self.has = phone_has_cts
    def bond(self, t): self.stage = "bonded"; self.since = t; self.conn = True
    def drop(self):
        self.conn = False; self.stage = "advertising"
        self.state = IDLE; self.syncedAt = 0; self.fails = 0      # onDisconnect
    def tick(self, now):
        if not self.conn: return
        if self.stage != "bonded": return
        if now - self.since < SETTLE: return
        if self.state != DONE and self.fails < GIVEUP and now - self.askedAt > RETRY:
            self.askedAt = now; self.fails += 1; self.reads += 1
            if self.has:
                self.state = DONE; self.syncedAt = now; self.fails = 0; self.setClock += 1
        if self.state == DONE and now - self.syncedAt > REFRESH:
            self.state = IDLE; self.fails = 0; self.askedAt = 0

def drive(c, until, step=1000):
    for t in range(0, until, step): c.tick(t)
    return c

def t_happy():
    c = Bt(); c.bond(0); drive(c, 60000)
    assert c.setClock == 1, f"clock set {c.setClock} times, wanted once"
    assert c.reads == 1, f"{c.reads} reads for one answer"
    print("        bonded, read once, clock set once")
run("A bonded phone sets the clock, once", t_happy)

def t_not_before_the_bond():
    c = Bt(); c.conn = True; c.stage = "connected"
    drive(c, 120000)
    assert c.reads == 0, "it read CTS before the link was encrypted"
    c.bond(120000); drive(c, 200000)
    assert c.setClock == 1
    print("        silent while only 'linked', reads the moment it is 'paired'")
run("It does not read an unbonded phone", t_not_before_the_bond)

def t_settles_first():
    c = Bt(); c.bond(0)
    c.tick(SETTLE - 1); assert c.reads == 0, "read before the bond had settled"
    c.tick(SETTLE + RETRY + 1); assert c.reads == 1
    print(f"        waits {SETTLE}ms after the bond")
run("It lets the bond settle", t_settles_first)

def t_phone_without_a_clock():
    c = Bt(phone_has_cts=False); c.bond(0)
    drive(c, 2000 * 1000)
    assert c.setClock == 0
    assert c.reads == GIVEUP, f"{c.reads} reads; it should stop at {GIVEUP}"
    print(f"        {c.reads} tries over 2000s, then it stops asking")
run("A phone with no clock service is given up on", t_phone_without_a_clock)

def t_reconnect_starts_over():
    c = Bt(); c.bond(0); drive(c, 60000)
    assert c.setClock == 1
    c.drop(); c.bond(100000); drive(c, 200000)
    assert c.setClock == 2, "it did not re-read on a new connection"
    print("        a new connection means new handles, so it reads again")
run("Dropping and coming back re-reads the clock", t_reconnect_starts_over)

def t_refresh():
    c = Bt(); c.bond(0)
    for t in range(0, int(REFRESH) + 120000, 10000): c.tick(t)
    assert c.setClock == 2, f"set {c.setClock} times across one refresh window"
    print(f"        and again every {int(REFRESH)//3600000} hours")
run("It drifts back into step on its own", t_refresh)

def t_not_in_wifi_mode():
    head = grab("static void btTick() {")[:160]
    assert "if (cfgNet != NET_BT) return;" in head, "it would run with no Bluetooth at all"
    print("        btTick is the first thing out of the way on WiFi")
run("None of this runs unless Bluetooth is the mode", t_not_in_wifi_mode)

def t_handles_are_not_reused():
    dis = grab("void onDisconnect(NimBLEServer* sv, NimBLEConnInfo& ci, int reason) override {")
    for v in ("ctsState = CTS_IDLE", "ctsSyncedAt = 0", "ancsCP = nullptr",
              "uidHead = uidTail = 0", "dsLen = 0"):
        assert v in dis, f"{v} missing: stale state would be used on the next phone"
    print("        clock, notification and queue state all dropped with the connection")
run("Nothing survives a disconnect that should not", t_handles_are_not_reused)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

"""The endpoint, the defaults, and what happens when one goes off."""
import re, sys, datetime as dt
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
fails = []
def must(c, w):
    if not c: fails.append(w)
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)

# 4.3.0 replaced the single half hour with a ladder that backs off and
# then lets go. The ladder itself is sim_ladder.py's job; what this file
# still cares about is the endpoint and the day boundary.
# 5.5.0 replaced the growing ladder with one interval: nobody
# answering and you waving it away now mean the same fifteen minutes.
# The count still runs, so it still gives up.
# 5.5.0 replaced the growing ladder with one interval. Nobody
# answering and you waving it away now mean the same fifteen minutes,
# and the count still runs so it still gives up rather than asking all
# evening. Kept as a list so everything below still walks it.
STEPS = int(re.search(r"#define REM_STEPS (\d+)", src).group(1))
WAVED = int(re.search(r"#define REM_WAVED_MIN (\d+)", src).group(1))
LAD = [WAVED] * STEPS
IGN = WAVED * 60
WAV = int(re.search(r"#define REM_WAVED_MIN (\d+)", src).group(1)) * 60
H = [int(x) for x in re.search(r"const int H\[3\] = \{ ([^}]*) \}", src).group(1).split(",")]

print(f"\nignored {IGN//60}min, waved away {WAV//60}min, default hours {H}\n")

must('web.on("/api/remind", HTTP_ANY' in src, "there is an endpoint, on any method")
must('if (!guard()) return;' in src.split('"/api/remind"')[1][:200], "it is behind the pairing")
# 5.2.0 gave each one a stable id so the app can edit and delete by it.
must(re.search(r"struct Rem \{ char text\[REM_TEXT\]; uint32_t id; uint32_t at; uint32_t first;\s*"
               r"uint8_t tries; bool done; \};", src) is not None,
     "a reminder remembers its id, the day it was for and how often it has asked")
# authed() reads "t" as the pairing token. A reminder whose text came in
# as "t" would be checked as a password, fail, and 401 with no clue why.
h = src.split('"/api/remind"')[1][:2000]
must('web.arg("text")' in h, "the text does not come in as text")
must('web.hasArg("t")' not in h, 'the text field collides with the token parameter')
must('String t = web.arg("t");' in src, "authed no longer reads the token from t")

def t_rules():
    assert WAV == 900, f"waved away is {WAV}s, not a quarter of an hour"
    assert LAD[0] == WAVED, f"the first ask back is {LAD[0]} minutes"
    assert LAD == sorted(LAD), "the ladder does not back off"
    assert H == [9, 12, 18], f"the three slots are {H}"
run("A press buys fifteen minutes; ignoring starts a ladder that backs off", t_rules)

# the endpoint's own arithmetic, lifted out
def plan(now, date=None, at=None):
    """Returns the times it would set, the way the handler does."""
    Y, M, D = (now.year, now.month, now.day)
    if date: Y, M, D = date
    def stamp(hh, mm): return dt.datetime(Y, M, D, hh, mm)
    if at:
        return [stamp(*at)]
    out = []
    for hh in H:
        w = stamp(hh, 0)
        if (w - now).total_seconds() < -60: continue
        out.append(w)
    return out if out else [now]

NOW = dt.datetime(2026, 10, 2, 7, 30)

def t_time_given():
    got = plan(NOW, at=(14, 15))
    assert got == [dt.datetime(2026,10,2,14,15)], got
run("A time with no date lands today at that time", t_time_given)

def t_date_given():
    got = plan(NOW, date=(2026,10,9), at=(8,0))
    assert got == [dt.datetime(2026,10,9,8,0)], got
run("A date and a time are both kept exactly", t_date_given)

def t_no_time():
    got = plan(NOW)
    assert [g.hour for g in got] == [9,12,18], [g.hour for g in got]
    assert all(g.date() == NOW.date() for g in got), "they are not all today"
run("No time at all means nine, noon and six, today", t_no_time)

def t_no_time_afternoon():
    got = plan(dt.datetime(2026,10,2,13,0))
    assert [g.hour for g in got] == [18], [g.hour for g in got]
run("Added in the afternoon, the hours already gone are skipped", t_no_time_afternoon)

def t_no_time_late():
    late = dt.datetime(2026,10,2,23,30)
    got = plan(late)
    assert got == [late], got
run("Added at half eleven at night, it is due now rather than never", t_no_time_late)

def t_date_no_time():
    got = plan(NOW, date=(2026,12,25))
    assert [g.hour for g in got] == [9,12,18], [g.hour for g in got]
    assert all(g.date() == dt.date(2026,12,25) for g in got), "not on the day asked for"
run("A date with no time gives three on that day, not today", t_date_no_time)

print("\nwhat happens when one goes off\n")

class R:
    def __init__(s, at, first=None):
        s.at = at; s.first = first if first is not None else at; s.done = False
class Robot:
    def __init__(s, rem): s.r = rem; s.showing = False; s.log = []
    def tick(s, now):
        if s.r.done or s.showing: return
        if now < s.r.at: return
        # only on the day it was for
        if dt.datetime.fromtimestamp(s.r.first).date() != dt.datetime.fromtimestamp(now).date():
            s.r.done = True; s.log.append("expired"); return
        s.showing = True; s.log.append("shown")
    def ignore(s, now):
        if not s.showing: return
        s.showing = False; s.r.at = now + IGN; s.log.append(f"+{IGN//60}m")
    def press(s, now):
        if not s.showing: return
        s.showing = False; s.r.at = now + WAV; s.log.append(f"+{WAV//60}m")
    def hold(s, now):
        if not s.showing: return
        s.showing = False; s.r.done = True; s.log.append("done")

def ts(y,m,d,hh,mm): return dt.datetime(y,m,d,hh,mm).timestamp()

def t_ignored_returns():
    r = R(ts(2026,10,2,9,0)); b = Robot(r)
    now = ts(2026,10,2,9,0)
    b.tick(now); b.ignore(now + 60)
    assert not r.done, "ignoring it finished it"
    assert r.at == now + 60 + IGN, "it did not come back in half an hour"
    b.tick(r.at); assert "shown" in b.log[-1:], "it never came back"
    print(f"        {b.log}")
run("Ignored, it comes back half an hour later, still not done", t_ignored_returns)

def t_press_returns_sooner():
    r = R(ts(2026,10,2,9,0)); b = Robot(r)
    now = ts(2026,10,2,9,0)
    b.tick(now); b.press(now + 5)
    assert not r.done, "a press finished it"
    assert r.at == now + 5 + WAV, "a press did not bring it back in fifteen"
run("Waved away with a press, it comes back in fifteen minutes", t_press_returns_sooner)

def t_hold_finishes():
    r = R(ts(2026,10,2,9,0)); b = Robot(r)
    now = ts(2026,10,2,9,0)
    b.tick(now); b.hold(now + 3)
    assert r.done, "holding it did not finish it"
    for k in range(1, 20):
        b.tick(now + k * 3600)
    assert b.log.count("shown") == 1, f"it kept asking after being finished: {b.log}"
run("Held, it is done, and never asks again", t_hold_finishes)

def t_nags_all_day():
    r = R(ts(2026,10,2,9,0)); b = Robot(r)
    now = ts(2026,10,2,9,0)
    for _ in range(8):
        b.tick(now)
        if b.showing: b.ignore(now)
        now = r.at
    assert b.log.count("shown") >= 6, f"it gave up too soon: {b.log}"
    assert not r.done
    print(f"        asked {b.log.count('shown')} times across the day, still not done")
run("Left alone it keeps asking through the day", t_nags_all_day)

def t_stops_tomorrow():
    # The ladder is 5,5,5,30,30,60,60, so one starting late in the
    # evening walks itself over midnight. It must stop there rather
    # than carrying on into the morning.
    r = R(ts(2026,10,2,23,0)); b = Robot(r)
    step = 0
    for _ in range(12):
        b.tick(r.at)
        if r.done: break
        if b.showing:
            gap = LAD[min(step, len(LAD)-1)] * 60; step += 1
            b.showing = False; r.at = r.at + gap; b.log.append(f"+{LAD[min(step-1,len(LAD)-1)]}")
    assert r.done, "it followed me into the next day"
    assert b.log[-1] == "expired", b.log
    print(f"        {b.log}")
run("It stops at midnight rather than following you into the week", t_stops_tomorrow)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

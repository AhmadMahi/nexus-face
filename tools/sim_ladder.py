"""The reminder ladder, the way out of it, and the URL it arrives by."""
import re, sys
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
app = open("rafiq-app/mac/Sources/SettingsPane.swift").read()
fails=[]
def must(c,w):
    if not c: fails.append(w)
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)

# 5.5.0 replaced the growing ladder with one interval. Nobody
# answering and you waving it away now mean the same fifteen minutes,
# and the count still runs so it still gives up rather than asking all
# evening. Kept as a list so everything below still walks it.
STEPS = int(re.search(r"#define REM_STEPS (\d+)", src).group(1))
WAVED = int(re.search(r"#define REM_WAVED_MIN (\d+)", src).group(1))
LAD = [WAVED] * STEPS
WAV = int(re.search(r"#define REM_WAVED_MIN (\d+)", src).group(1))
print(f"\nladder {LAD} minutes, a press buys {WAV}\n")

def t_shape():
    assert LAD == [WAVED] * STEPS, LAD
    assert "REM_LADDER" not in src, "the old ladder array is still there, unused"
    assert WAV == 15, WAV
    total = sum(LAD)
    print(f"        seven asks across {total} minutes, then it lets go")
run("Three quick, two at half an hour, two at an hour, then done", t_shape)

class R:
    def __init__(s): s.tries=0; s.done=False; s.at=0
class Bot:
    def __init__(s, woke=True): s.r=R(); s.showing=False; s.woke=woke; s.slept=False; s.log=[]
    def fire(s,now):
        if s.r.done: return
        s.showing=True; s.log.append("ask")
    def ignore(s,now):
        s.showing=False
        if s.r.tries >= len(LAD): s.r.done=True; s.log.append("gave up"); return
        m=LAD[s.r.tries]; s.r.tries+=1; s.r.at=now+m*60; s.log.append(f"+{m}")
    def press(s,now):
        s.showing=False; s.r.at=now+WAV*60; s.log.append(f"press +{WAV}")
        if s.woke: s.slept=True; s.log.append("sleep")
    def hold(s,now):
        s.showing=False; s.r.done=True; s.log.append("done")
        if s.woke: s.slept=True; s.log.append("sleep")

def t_ignored_ladder():
    b=Bot(); now=0
    for _ in range(10):
        b.fire(now)
        if b.showing: b.ignore(now); now=b.r.at
        if b.r.done: break
    gaps=[x for x in b.log if x.startswith("+")]
    assert gaps==[f"+{m}" for m in LAD], gaps
    assert b.r.done, "it never gave up"
    assert b.log.count("ask")==len(LAD)+1, f"asked {b.log.count('ask')} times"
    print(f"        {b.log}")
run("Ignored it walks the ladder once, then lets go", t_ignored_ladder)

def t_press_sleeps():
    b=Bot(woke=True); b.fire(0); b.press(0)
    assert not b.r.done, "a press finished it"
    assert b.r.at==WAV*60, "a press did not buy fifteen minutes"
    assert b.slept, "it stayed awake after being told later"
run("A press buys fifteen minutes and sends it straight back to sleep", t_press_sleeps)

def t_press_no_ladder():
    b=Bot(); b.fire(0)
    for _ in range(20): b.press(0); b.fire(0)
    assert b.r.tries==0, f"pressing used up {b.r.tries} rungs of the ladder"
    assert not b.r.done, "pressing eventually finished it"
run("Pressing never uses up the ladder, so you can keep saying later", t_press_no_ladder)

def t_hold_sleeps():
    b=Bot(woke=True); b.fire(0); b.hold(0)
    assert b.r.done and b.slept, "holding did not finish it and go back"
run("Holding finishes it and sends it back to sleep", t_hold_sleeps)

def t_awake_stays_awake():
    b=Bot(woke=False); b.fire(0); b.press(0)
    assert not b.slept, "it dropped off while being used"
run("Answering one while you are using it does not put it to sleep", t_awake_stays_awake)

print("\nthe source\n")
# 5.3.0: answering it always puts it back, because waiting out a screen
# timeout after you have dealt with the thing is the robot ignoring you.
# What changed is the kind of sleep, not whether it goes: deep when it
# came from deep or there is no Mac, otherwise the screen darkens and
# the Mac keeps its connection. backToSleep owns that choice now.
must("bool back = remWokeIt;" in src,
     "it would stay up after being answered, or go back when nobody woke it")
must(src.count("if (back) backToSleep();") == 2,
     "only one of waving it away and marking it done sends it back")
must("if (wokeForAlarm || !macLinked) { wokeForAlarm = false; wantDeep = true; }" in src,
     "it would stay on the network when nothing is holding it there")
must("else                            goSleep();" in src,
     "it would drop a connected Mac just to save a little current")
must('wake("update");' in src.split('"/api/update"')[1][:400],
     "an update asked for from the Mac still runs in the dark")
must('web.hasArg("in")' in src, "no relative time")
must("mins > 60 * 24 * 30" in src, "in= is unbounded")

def t_url():
    assert 'http://\\(host)/api/remind?t=\\(tok)' in app, "the app is not offering a plain URL"
    assert 'url("in=45")' in app and 'url("at=18:30")' in app, "both forms are not shown"
    assert "addingPercentEncoding" in app, "a space in the text would break the link"
    # A single reminder stays a plain address you can paste anywhere.
    # 5.2.0 added a curl form, but only for the bulk call, where the
    # address gets too long to paste at about eight reminders.
    single = app[:app.index("private var curl")] if "private var curl" in app else app
    assert "curl" not in single, "the single reminder is being shown as a shell command"
    assert "api/rems" in app[app.index("private var curl"):][:400], \
        "the curl example is not the bulk call"
run("A single reminder is a plain address; only the bulk call gets a command", t_url)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -",f) for f in fails]; sys.exit(1)
print("PASS")

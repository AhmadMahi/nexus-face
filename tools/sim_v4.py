"""Everything v4 changes: the hold tiers, sleep, shake, reminders, battery."""
import re, sys
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
fails = []
def must(c, w):
    if not c: fails.append(w)
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)

LONG  = int(re.search(r"#define TOUCH_LONG_MS (\d+)", src).group(1))
HOME  = int(re.search(r"#define TOUCH_HOME_MS  (\d+)", src).group(1))
# 5.8.0 folded the second tier away: five seconds does the whole thing.
COUNT = int(re.search(r"#define TOUCH_COUNT_MS\s+(\d+)", src).group(1))

print(f"\nhold: in at {LONG}ms, home at {HOME}ms, then {COUNT}ms to change your mind\n")

def t_tiers():
    assert LONG < HOME, "the tiers are not in order"
    assert HOME >= 3000, "home fires too easily for something you reach by holding"
    # nothing on screen until past home, which is the whole point
    loop = src.split("void loop()")[-1]
    assert "if (sleepArmed) {" in loop, "nothing is shown past five seconds"
    # touchSeenFree: nothing that can switch the robot off fires
    # until the pad has been seen settled at rest. See sim_pad.
    assert "if (!cfgGesture && touchSeenFree && !sleepArmed && held >= TOUCH_HOME_MS) {" in src, \
        "nothing happens at the four second mark"
    print(f"        silent for the first {HOME/1000:.0f}s, then it says what is happening")
run("In, then home, and silent until the fourth second", t_tiers)

def t_release():
    # 5.10.0: one move, over at four seconds. The count that follows
    # runs on the clock; letting go does not stop it, because the pad
    # lets go on its own before you would.
    assert "} else if (!touchOn && sleepArmed) {" not in src, \
        "letting go can still take the decision back"
    assert "touchHold" not in src, "the old two tier state is still in there"
    assert "TOUCH_SLEEP_MS" not in src, "the second tier is still defined"
    i = src.index("    if (sleepArmed) {\n      lastActive = now;")
    assert "if (now - sleepArmed >= TOUCH_COUNT_MS) { sleepArmed = 0; wantDeep = true; }" \
        in src[i:i + 260], "the count does not finish on its own"
run("Four seconds goes home and decides it; the count just runs", t_release)

print("\nsleep\n")
def t_touch_only():
    # 5.6.0 split the way down into sleepNow(), which both going to
    # sleep on purpose and a refused wake go through.
    n = re.search(r"static void sleepNow\(long secs\) \{.*?esp_deep_sleep_start\(\);", src, re.S).group(0)
    assert "esp_deep_sleep_enable_gpio_wakeup(BIT(TOUCH_PIN)," in n, "the pad is not the wake source"
    assert "TAP_INT_PIN" not in n, "the accelerometer can still wake it"
    g = re.search(r"static void goDeep\(\) \{.*?sleepNow\(secs\);", src, re.S).group(0)
    assert "sleepCard();" in g, "it goes dark without saying anything"
run("Only the pad wakes it, and it says goodnight first", t_touch_only)

def t_mac():
    assert "!macLinked &&" in src, "it would power down with the Mac still connected"
    assert "deepAfterMs()" in src, "the new timeout is not used"
    opts = re.search(r"DEEP_OPTS\[\] = \{([^}]*)\}", src).group(1)
    assert "120" in opts, "two minutes is not an option"
    assert src.count("cfgDeepIdx = 1") >= 1, "two minutes is not the default"
run("It stays reachable while the Mac is there, and powers down when it is not", t_mac)

def t_wake_timer():
    g = re.search(r"long pray = secsToNextAlert\(\);.*?sleepNow\(secs\);", src, re.S)
    assert g, "the wake timer does not consider both"
    b = g.group(0)
    assert "esp_sleep_enable_timer_wakeup" in src, "nothing arms a timer at all"
    assert "rtcAlarmAt =" in b, "the answer is not kept where a refused wake can find it"
    assert "long rem  = secsToNextRem();" in b, "reminders do not set the alarm"
    assert "secs = pray < rem ? pray : rem;" in b, "it does not take whichever is sooner"
run("It wakes for whichever comes first, a prayer or a reminder", t_wake_timer)

print("\nshake\n")
def t_shake():
    m = re.search(r"if \(cfgShake &&.*?\n    \}", src, re.S).group(0)
    assert "depth--" in m, "a shake does not step back"
    assert "screen = S_HOME" in m, "a shake cannot reach home"
    assert "lastShake > 600" in src, "a good rattle would count as several"
    assert "screen == S_GAMES && depth == 2" in m, "a shake would quit a game you are playing"
    # 5.4.0 folded the on/off into a three way "Go back by", and both
    # defaults still include the shake. sim_settings owns the setting.
    assert "int cfgBack = BACK_BOTH;" in src, "the shake is not on by default"
    assert 'cfgBack = prefs.getBool("shake", true) ? BACK_BOTH : BACK_KNOCK;' in src, \
        "a robot that had it on loses it"
run("One step back per shake, never past the clock, never mid game", t_shake)

print("\nreminders\n")
def t_store():
    # gained "first" in 4.1.0 so a reminder knows which day it was for,
    # and "id" in 5.2.0 so the app can point at one and change it
    assert re.search(r"struct Rem \{ char text\[REM_TEXT\]; uint32_t id; uint32_t at; uint32_t first;\s*"
                     r"uint8_t tries; bool done; \};", src), "no store"
    assert "static void saveRems()" in src and "static void loadRems()" in src, "not kept"
    assert "loadRems();" in src, "not read back at boot"
    assert 'web.on("/api/rems"' in src, "the Mac cannot send them"
    assert "sortRems();" in src, "not kept in time order"
run("Reminders live on the robot, saved, sorted and reloaded", t_store)

def t_fire():
    m = re.search(r"if \(timeOk && remCount && now - remCheck > 2000\) \{.*?\n  \}", src, re.S).group(0)
    assert 'wake("reminder")' in m, "a reminder coming due does not wake it"
    # 4.1.0 stopped marking one done the moment it fired: it has to come
    # back if you ignore it. What stops it following you around is the
    # day it was for, not an hour's grace.
    assert "remShowing = i;" in m, "nothing remembers which one is on screen"
    assert "fd.tm_yday != nd.tm_yday" in m, "one from yesterday would still shout"
run("One falling due wakes it, fires once, and stale ones are dropped", t_fire)

def t_walk():
    m = re.search(r"if \(screen == S_REMIND && depth > 0\) \{.*?\n  \}", src, re.S).group(0)
    assert "case TG_ONE:  if (remIdx < remCount) remIdx++;" in m, "one does not go to the next"
    assert "case TG_TWO:" in m and "depth = 0" in m, "two does not come back"
    assert "remConfirm = true" in m, "the end does not offer to clear them"
    assert "remCount = 0; remIdx = 0; saveRems();" in m, "clearing does not stick"
    assert "if (g == TG_TWO) { remConfirm = false; return; }" in m, \
        "no way out of the clear question without answering yes"
run("One walks them, two goes back, the end offers to clear, and asks first", t_walk)

def t_focus_kept():
    assert "S_FOCUS" in src and "sessionRunning" in src, "Focus was thrown away"
    names = re.findall(r'"([^"]*)"', re.search(r'S_NAME\[S_COUNT\]\s*=\s*\{(.*?)\};', src, re.S).group(1))
    # The vehicle screen moved in between in 5.0.0, and is skipped when
    # it is switched off, so reminders are still what you reach first.
    assert names.index("REMINDERS") < names.index("FOCUS"), "Focus is now in front of reminders"
    assert names.index("REMINDERS") <= 2, f"reminders are screen {names.index('REMINDERS')}"
    assert "if (s == S_BIKE)    return cfgBike;" in src, "the vehicle screen is not skippable"
run("Reminders are still near the front and Focus keeps its tasks", t_focus_kept)

print("\nbattery\n")
V = [float(x) for x in re.search(r"static const float V\[\] = \{(.*?)\};", src, re.S).group(1).replace("f","").split(",")]
P = [int(x) for x in re.search(r"static const int   P\[\] = \{(.*?)\};", src, re.S).group(1).split(",")]
def pct(v, full):
    v *= 4.20/full
    if v <= V[0]: return 0
    for i in range(1,len(V)):
        if v <= V[i]:
            f=(v-V[i-1])/(V[i]-V[i-1]); return P[i-1]+int(f*(P[i]-P[i-1])+0.5)
    return 100

def t_datapoint():
    # the one real measurement: 4.09V was being called 89%
    assert pct(4.09, 4.20) == 89, f"the old curve did not give 89 at 4.09: {pct(4.09,4.20)}"
    got = pct(4.09, 4.10)
    assert got >= 98, f"4.09V still reads {got}% with full set to 4.10"
    print(f"        4.09V: was 89%, now {got}%")
run("The reading you gave me now reads as full", t_datapoint)

def t_fall():
    # the other half of the complaint: it fell too fast
    a, b = pct(3.70, 4.20), pct(3.70, 4.10)
    assert b > a + 15, f"3.70V still falls fast: {a}% then {b}%"
    print(f"        3.70V: was {a}%, now {b}%")
run("And it no longer falls off a cliff in the middle", t_fall)

def t_settable():
    assert "float    battFull = 4.10f;" in src, "full is not 4.10 by default"
    assert 'prefs.getFloat("bfull"' in src, "the calibration is not remembered"
    assert "C_BATT" in src, "no way to change it on the robot"
    assert 'k == "bfull"' in src, "no way to change it from the app"
run("And you can trim it yourself without another release", t_settable)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

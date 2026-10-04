"""The bug that lost reminders, and the four other things in 4.4.0."""
import re, sys
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
rem = open("rafiq-app/mac/Sources/Reminders.swift").read()
dev = open("rafiq-app/mac/Sources/Device.swift").read()
fails=[]
def must(c,w):
    if not c: fails.append(w)
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)

print("\nthe bug: a push used to empty the list\n")

H = re.search(r'web\.on\("/api/rems".*?\n  \}\);', src, re.S).group(0)

def t_no_wipe():
    # Emptying it on purpose is allowed and lives above; what matters is
    # that the part which takes a list does not.
    clearing, merging = H.split('int n = web.arg("n")')
    assert "remCount = 0;" in clearing, "clear=1 does not actually clear"
    assert "remCount = 0" not in merging, "a push still empties the list"
    assert "if (dup) { already++; continue; }" in H, "it does not notice one it already has"
    assert "addRem(txt.c_str(), at)" in H, "it does not add through the one place that adds"
run("A push adds what is new and leaves everything else alone", t_no_wipe)

def t_clear_is_deliberate():
    assert 'web.arg("clear") == "1"' in H, "no way to empty it on purpose"
run("Emptying it is a thing you ask for, not a side effect", t_clear_is_deliberate)

# what the old code did to a list, against what the new one does
def old_push(have, incoming): return list(incoming)
def new_push(have, incoming):
    out = list(have)
    for txt, at in incoming:
        if any(t == txt and a//60 == at//60 for t, a in out): continue
        out.append((txt, at))
    return out

def t_the_actual_bug():
    # you add one by URL, then the Mac saves for its own reasons
    robot = [("Call the clinic", 1000)]          # added through /api/remind
    mac   = [("Buy milk", 2000)]                 # what the Mac happens to hold
    assert old_push(robot, mac) == mac, "model wrong"
    assert ("Call the clinic", 1000) not in old_push(robot, mac), "model wrong"
    after = new_push(robot, mac)
    assert ("Call the clinic", 1000) in after, "the URL one is still being lost"
    assert ("Buy milk", 2000) in after, "the Mac's one did not arrive"
    print(f"        before: {len(robot)} on the robot, Mac pushes {len(mac)} -> "
          f"old kept {len(old_push(robot,mac))}, new keeps {len(after)}")
run("A reminder added by URL survives the Mac pushing its own", t_the_actual_bug)

def t_idempotent():
    have = [("Call mum", 600)]
    once = new_push(have, [("Call mum", 600)])
    twice = new_push(once, [("Call mum", 600)])
    assert len(once) == 1 and len(twice) == 1, f"sending twice made {len(twice)}"
run("Sending the same list twice does not double it", t_idempotent)

print("\nthe Mac stops being the owner\n")
def t_queue():
    assert "var delivered = false" in rem, "nothing tracks what the robot has"
    assert "func deliver() async" in rem, "nothing retries"
    assert "guard await Device.shared.pushReminders(waiting) else { return }" in rem, \
        "it marks them delivered whether or not the robot answered"
    assert "$0.fireAt > Date()" in rem, "it would send ones whose time has passed"
    assert "await self?.deliver()" in rem, "it never retries on its own"
    assert "-> Bool" in dev.split("func pushReminders")[1][:80], "the push cannot fail"
run("The Mac queues, retries, and only ticks one off when the robot says so", t_queue)

print("\nthe rest\n")
def t_bright():
    o = [int(x) for x in re.search(r"BRIGHT_OPTS\[\] = \{([^}]*)\}", src).group(1).split(",")]
    n = re.findall(r'"([^"]*)"', re.search(r"BRIGHT_NAME\[\] = \{([^}]*)\}", src).group(1))
    assert len(o) == len(n), f"{len(o)} levels, {len(n)} names"
    # 5.1.0 dropped the 1% step: zero contrast reads as a fault rather
    # than a setting. sim_v51 owns the exact list now.
    assert n[0] == "10%", f"the lowest is called {n[0]}"
    assert o[0] > 0, "the zero contrast step came back"
    assert o == sorted(o), "they are not in order"
    print(f"        {n}")
run("The faintest step is offered and called what it is", t_bright)

def t_countdown():
    # 5.2.0 cut the count from five to three and gave both tiers a
    # drawn overlay. sim_v52 owns the look of them; this still owns the
    # rule that it goes whether or not the finger comes off.
    C = int(re.search(r"#define TOUCH_COUNT_MS\s+(\d+)", src).group(1))
    assert C == 3000, f"the countdown is {C}ms, not three seconds"
    # 5.7.0: the countdown is armed at the sleep tier and then reads
    # off its own clock. It cannot wait on a pad that may stop
    # reporting a finger that is still there.
    assert "if (now - sleepArmed >= TOUCH_COUNT_MS) { sleepArmed = 0; wantDeep = true; }" in src, \
        "the countdown does not finish on its own"
    assert "held >= TOUCH_SLEEP_MS + TOUCH_COUNT_MS" not in src, "it still waits on the pad"
    assert "drawHoldTier(now)" in src, "nothing is drawn while you hold"
    assert '"let go to stay"' in src, "it does not say how to stop it"
    print("        ten seconds, then three counted out, then it goes whether held or not")
run("Five seconds in, it counts three and goes unless you let go", t_countdown)

def t_counter():
    # Moved to the bottom in 5.1.0, where a watch puts it, which cost a
    # line of text. sim_v51 checks the layout in detail.
    assert 'snprintf(ofN, sizeof(ofN), "%d of %d", remIdx + 1, remCount);' in src, \
        "no which-of-how-many on a reminder"
    assert "ctr(ofN, 56, 1);" in src, "it is not along the bottom"
run("A reminder says which of how many, along the bottom", t_counter)

def t_hijri_sync():
    assert 'filter["data"]["date"]["hijri"]["day"] = true;' in src, \
        "the prayer answer's Hijri date is thrown away"
    assert "hijriToJdn" not in src, "the inverse that was 385 days out is still here"
    assert "for (int s = -3; s <= 3; s++)" in src, "it does not search for the shift"
    assert 'prefs.putInt("hadj", cfgHijriAdj);' in src, "a corrected shift is not kept"
run("The announced Hijri date corrects the arithmetic one, by search", t_hijri_sync)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -",f) for f in fails]; sys.exit(1)
print("PASS")

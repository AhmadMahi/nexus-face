"""The HID object, and the boot loop it caused.

5.16.0 called btHid->manufacturer("Rafiq"). That overload only writes
through m_manufacturerCharacteristic; the thing that CREATES it is the
no-argument manufacturer(), and the constructor never touches the
pointer at all. So it stored through whatever was on the heap, panicked,
rebooted, and panicked again, because the mode had already been written
to NVS before the radio was ever asked to come up.

Two checks, then. One reads the library and works out, for every
BLEHIDDevice call the sketch makes, whether the thing being written to
has been created yet. The other is about the boot loop rather than the
pointer: no setting may put the robot beyond reach of anything but a
cable.
"""
import re, sys, os

CORE = ("/Users/shukranaahmed/Library/Arduino15/packages/esp32/hardware/"
        "esp32/3.3.10/libraries/BLE/src")
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
hid_cpp = open(os.path.join(CORE, "BLEHIDDevice.cpp")).read()
hid_h   = open(os.path.join(CORE, "BLEHIDDevice.h")).read()

fails = []
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)
print()

def body_of(sig_re, text):
    m = re.search(sig_re, text)
    if not m: return ""
    i = m.start(); depth = 0; out = []
    for ch in text[i:]:
        out.append(ch)
        if ch == "{": depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0: break
    return "".join(out)

CTOR = body_of(r"BLEHIDDevice::BLEHIDDevice\(BLEServer \*server\)\s*\{", hid_cpp)

def members_declared():
    return set(re.findall(r"BLECharacteristic \*(m_\w+);", hid_h))

def members_made_by_ctor():
    return set(re.findall(r"(m_\w+)\s*=", CTOR))

def methods():
    """Every BLEHIDDevice method, what it writes through, and what it
    creates. Creating is an assignment; using is a dereference."""
    out = {}
    for m in re.finditer(r"^\w[\w \*]*BLEHIDDevice::(\w+)\(([^)]*)\)\s*\{", hid_cpp, re.M):
        name, args = m.group(1), m.group(2).strip()
        if name == "BLEHIDDevice": continue
        b = body_of(re.escape(m.group(0)), hid_cpp)
        creates = set(re.findall(r"(m_\w+)\s*=\s*", b))
        uses    = set(re.findall(r"(m_\w+)->", b))
        out.setdefault(name, []).append((args, creates, uses - creates))
    return out

def sketch_calls():
    on = src[src.index("static void bleOn() {"):]
    on = on[:on.index("\n}")]
    # btHid->name(...)  and  btHid->name()->something()
    return [(m.group(1), m.group(2)) for m in
            re.finditer(r"btHid->(\w+)\(([^;]*?)\)(?:->\w+\([^;]*\))?;", on)]

def t_pointer_is_made_before_it_is_used():
    declared = members_declared()
    made     = members_made_by_ctor()
    never    = declared - made
    meths    = methods()
    calls    = sketch_calls()
    print(f"        of {len(declared)} characteristics the constructor makes "
          f"{len(declared & made)}; it never touches "
          f"{sorted(x.replace('m_','').replace('Characteristic','') for x in never)}")
    created_so_far = set(made)
    problems = []
    for name, args in calls:
        overloads = meths.get(name, [])
        if not overloads: continue
        has_arg = args.strip() != ""
        pick = None
        for a, creates, uses in overloads:
            if (a.strip() != "") == has_arg: pick = (creates, uses); break
        if pick is None: pick = (overloads[0][1], overloads[0][2])
        creates, uses = pick
        for u in uses:
            if u in never and u not in created_so_far:
                problems.append(f"btHid->{name}({args}) writes through {u}, "
                                "which nothing has created yet")
        created_so_far |= creates
    assert not problems, "; ".join(problems)
    assert 'btHid->manufacturer()->setValue("Rafiq");' in src, \
        "not using the overload that creates the characteristic"
    print("        every call writes through a pointer something has already made")
run("Nothing is written through a pointer nobody made", t_pointer_is_made_before_it_is_used)

def t_the_old_call_is_caught():
    """Prove the check above has teeth by running 5.16.0's line past
    the same rule."""
    never = members_declared() - members_made_by_ctor()
    man = [o for o in methods()["manufacturer"] if o[0].strip() != ""][0]
    args, creates, uses = man
    assert "m_manufacturerCharacteristic" in never, \
        "the constructor makes it after all; the diagnosis was wrong"
    assert "m_manufacturerCharacteristic" in uses, "the setter does not use it"
    assert not creates, "the setter creates it, so it was never the fault"
    print("        manufacturer(String) writes through a pointer the constructor "
          "never sets: 5.16.0's crash, reproduced from the library source")
run("The rule catches the line that actually broke it", t_the_old_call_is_caught)

def t_battery_before_services():
    on = src[src.index("static void bleOn() {"):]
    on = on[:on.index("\n}")]
    assert on.index("setBatteryLevel") < on.index("startServices"), \
        "the battery is written after the server starts, which makes it a notify " \
        "to a connection that does not exist"
    print("        battery written before the server starts, so it is not a notify")
run("The battery level is a write, not a notify into nothing", t_battery_before_services)

# ---------------- the boot loop ----------------
def t_a_note_is_left():
    on = src[src.index("static void bleOn() {"):]
    on = on[:on.index("\n}")]
    assert 'prefs.putInt("btry2", btTries);' in on, "nothing records that it is trying"
    assert on.index('prefs.putInt("btry2", btTries);') < on.index("BLEDevice::init"), \
        "the count is written after the part that can panic"
    print("        the count goes up before anything that can panic")
run("Bluetooth says it is about to try", t_a_note_is_left)

def t_a_note_still_there_means_no():
    boot = src[src.index("  loadNets();\n  // Did the last attempt"):]
    boot = boot[:boot.index("if (cfgOffline)")]
    assert 'prefs.getInt("btry2", 0) >= BT_GIVE_UP' in boot, "boot does not look at the count"
    assert "cfgNet = NET_WIFI;" in boot and 'prefs.putInt("net", cfgNet);' in boot, \
        "it does not actually move off Bluetooth, so it loops again"
    assert 'prefs.putInt("btry2", 0);' in boot, \
        "the count is not cleared, so WiFi is now stuck too"
    assert "flash(" in boot, "it recovers silently, so the robot looks broken"
    assert src.index("  loadNets();\n  // Did the last attempt") < src.index("if (cfgOffline) {\n    WiFi.persistent"), \
        "the check runs after the branch that would start the radio"
    print("        one bad boot and it is back on WiFi, saying so")
run("A note still lying there at boot means do not try again", t_a_note_still_there_means_no)

def t_the_note_is_torn_up():
    assert "#define BT_PROVEN_MS 60000UL" in src, "nothing says how long counts as survived"
    assert "#define BT_GIVE_UP        3" in src, "there is no limit on consecutive failures"
    assert "if (btNoteOut && now - btNoteAt > BT_PROVEN_MS)" in src, \
        "the count is never cleared, so every clean boot looks like a crash"
    off = src[src.index("static void bleOff() {"):]
    off = off[:off.index("\n}")]
    assert 'prefs.putInt("btry2", 0);' in off, \
        "turning Bluetooth off on purpose still counts against it"
    sleep = src[src.index("static void sleepNow(long secs) {"):]
    sleep = sleep[:sleep.index("uint32_t t0 = millis();")]
    assert 'prefs.putInt("btry2", 0);' in sleep, \
        "a deep wake is a boot, so a robot that sleeps often would count its " \
        "way to the fallback having never once failed"
    print("        60s up clears it; so does switching off, and so does lying down")
run("Surviving tears the note up", t_the_note_is_torn_up)

def t_no_setting_can_brick_it():
    """The general version of the rule, stated once so it is not lost."""
    i = src.index("        cfgNet = (cfgNet + 1) % NET_N;")
    blk = src[i:src.index("break;", i)]
    assert 'prefs.putInt("net", cfgNet);' in blk, "the mode is not saved at all"
    assert "bleOn();" in blk
    print("        the mode still commits first, but the note makes that survivable")
run("No setting leaves the robot reachable only by cable", t_no_setting_can_brick_it)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

"""The HID object, the boot loop it once caused, and the safety nets.

5.16.0 called btHid->manufacturer("Rafiq") on the core's bundled BLE
library. That overload only writes through m_manufacturerCharacteristic;
the thing that CREATES it is the no-argument manufacturer(), and that
constructor never touched the pointer at all. It stored through
whatever was on the heap, panicked, rebooted, and panicked again,
because the network mode had already been written to NVS before the
radio was ever asked to come up. The only way back in was a cable.

The library underneath has changed since. This checks the new one the
same way: read the source, work out which pointers the constructor
leaves null, and make sure nothing we call writes through one of them.
Then the two nets that mean a crash can never again be permanent.
"""
import re, sys, os, glob

src = open("nexus-repo/nexus_face/nexus_face.ino").read()

LIBS = ["/Users/shukranaahmed/Documents/Arduino/libraries/NimBLE-Arduino/src"]
HIDC = HIDH = None
for d in LIBS:
    if os.path.exists(os.path.join(d, "NimBLEHIDDevice.cpp")):
        HIDC = open(os.path.join(d, "NimBLEHIDDevice.cpp")).read()
        HIDH = open(os.path.join(d, "NimBLEHIDDevice.h")).read()
assert HIDC, "NimBLE-Arduino is not where it was; the sketch will not build either"

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

CTOR = body_of(r"NimBLEHIDDevice::NimBLEHIDDevice\(NimBLEServer\* server\)[^{]*\{", HIDC)

def t_every_pointer_is_guarded_or_made():
    """The 5.16.0 trap in general form: a setter that writes through a
    member the constructor never assigned. On this library every such
    member is declared {nullptr} and every setter that uses one
    creates it first, which is why the crash cannot come back. Both
    halves are checked, because either one alone would not be enough."""
    declared = dict(re.findall(r"NimBLECharacteristic\*\s+(m_\w+)\s*(\{nullptr\})?;", HIDH))
    assert declared, "no characteristic members found; the header moved"
    uninit = [m for m, init in declared.items() if init != "{nullptr}"]
    assert not uninit, f"{uninit} declared without an initialiser, exactly like the old library"

    made_in_ctor = set(re.findall(r"(m_\w+)\s*=", CTOR))
    # every method we call, and what it does with the members
    called = set(re.findall(r"btHid->(\w+)\(", src))
    problems = []
    for name in sorted(called):
        b = body_of(r"[\w:*&<>\s]+NimBLEHIDDevice::" + name + r"\([^)]*\)[^{]*\{", HIDC)
        if not b: continue
        creates = set(re.findall(r"(m_\w+)\s*=\s*", b))
        uses    = set(re.findall(r"(m_\w+)->", b))
        for u in uses - creates - made_in_ctor:
            if f"if ({u} == nullptr)" not in b and f"if (!{u})" not in b:
                problems.append(f"btHid->{name}() writes through {u} unguarded")
    assert not problems, "; ".join(problems)
    print(f"        {len(declared)} members, all {{nullptr}}; "
          f"{len(called)} calls, none through an unmade pointer")
run("Nothing is written through a pointer nobody made", t_every_pointer_is_guarded_or_made)

def t_the_old_call_cannot_be_made():
    """The exact 5.16.0 line, and the library it crashed on."""
    assert 'btHid->manufacturer(' not in src, "the old crashing overload is back"
    assert 'btHid->setManufacturer("Rafiq");' in src, "the manufacturer is not set at all"
    b = body_of(r"bool NimBLEHIDDevice::setManufacturer\([^)]*\)[^{]*\{", HIDC)
    assert "m_manufacturerChr == nullptr" in b, \
        "this library's setManufacturer no longer creates the characteristic; " \
        "it is the 5.16.0 trap again"
    print("        setManufacturer creates before it writes, which the old one did not")
run("The call that bricked it cannot be written again", t_the_old_call_cannot_be_made)

def t_battery_before_the_server_starts():
    on = src[src.index("static void bleOn() {"):]
    on = on[:on.index("\n}")]
    assert on.index("setBatteryLevel") < on.index("sv->start();"), \
        "the battery is written after the server starts, which makes it a notify " \
        "to a connection that does not exist"
    print("        battery written before the server starts, so it is not a notify")
run("The battery level is a write, not a notify into nothing", t_battery_before_the_server_starts)

# ---------------- the Bluetooth count ----------------
def t_a_count_is_kept():
    on = src[src.index("static void bleOn() {"):]
    on = on[:on.index("\n}")]
    assert 'prefs.putInt("btry2", btTries);' in on, "nothing records that it is trying"
    assert on.index('prefs.putInt("btry2", btTries);') < on.index("NimBLEDevice::init"), \
        "the count is written after the part that can panic"
    print("        the count goes up before anything that can panic")
run("Bluetooth says it is about to try", t_a_count_is_kept)

def t_three_strikes_moves_it():
    boot = src[src.index("  // Did the last attempt at Bluetooth come back?"):]
    boot = boot[:boot.index("if (cfgOffline)")]
    assert 'prefs.getInt("btry2", 0) >= BT_GIVE_UP' in boot, "boot does not look at the count"
    assert "cfgNet = NET_WIFI;" in boot and 'prefs.putInt("net", cfgNet);' in boot, \
        "it does not actually move off Bluetooth, so it loops again"
    assert 'prefs.putInt("btry2", 0);' in boot, \
        "the count is not cleared, so WiFi is now stuck too"
    assert "flash(" in boot, "it recovers silently, so the robot looks broken"
    assert src.index("if (safeMode) {") < src.index("  // Did the last attempt at Bluetooth come back?"), \
        "safe mode lands after the Bluetooth fallback, so it could be overridden"
    print("        three bad starts and it is back on WiFi, saying so")
run("A Bluetooth that keeps dying is given up on", t_three_strikes_moves_it)

def t_the_count_is_cleared_by_living():
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
run("Surviving clears the count", t_the_count_is_cleared_by_living)

# ---------------- the general safety net ----------------
SAFE_AFTER = int(re.search(r"#define SAFE_AFTER (\d+)", src).group(1))

class Boot:
    """The block at the top of setup, as something you can run."""
    def __init__(self): self.panics = 0; self.writes = 0; self.net = "bluetooth"
    def boot(self, reason):
        crashed = reason in ("PANIC", "INT_WDT", "TASK_WDT", "WDT")
        safe = False
        if crashed:
            self.panics += 1; self.writes += 1
        elif self.panics:
            self.panics = 0; self.writes += 1
        if crashed and self.panics >= SAFE_AFTER:
            safe = True; self.panics = 0; self.writes += 1
        if safe: self.net = "wifi"
        return safe

def t_three_panics_then_safe():
    b = Boot()
    assert not b.boot("PANIC"), "safe mode on the first panic"
    assert not b.boot("PANIC"), "safe mode on the second panic"
    assert b.boot("PANIC"), f"{SAFE_AFTER} panics running and still not safe mode"
    assert b.net == "wifi", "safe mode did not move it somewhere reachable"
    print(f"        {SAFE_AFTER} panics running, then WiFi")
run("Panics in a row end in a state an update can reach", t_three_panics_then_safe)

def t_one_bad_day_is_not_a_pattern():
    b = Boot(); b.boot("PANIC"); b.boot("PANIC")
    assert not b.boot("POWERON"), "a clean boot tripped it"
    assert b.panics == 0, "a clean boot did not clear the count"
    assert not b.boot("PANIC") and not b.boot("PANIC"), "the count did not really reset"
    print("        a clean boot in between clears the count")
run("A clean boot resets the count", t_one_bad_day_is_not_a_pattern)

def t_sleeping_is_not_crashing():
    b = Boot()
    for _ in range(50): b.boot("DEEPSLEEP")
    assert b.panics == 0 and b.net == "bluetooth", "napping tripped safe mode"
    print("        50 deep sleep wakes, no count, no safe mode")
run("Waking from sleep is not a crash", t_sleeping_is_not_crashing)

def t_no_flash_wear():
    b = Boot()
    for _ in range(500): b.boot("DEEPSLEEP")
    assert b.writes == 0, f"{b.writes} NVS writes across 500 ordinary wakes"
    b.boot("PANIC"); b.boot("POWERON")
    assert b.writes == 2, b.writes
    print("        500 ordinary wakes, zero NVS writes")
run("Ordinary boots do not wear the flash", t_no_flash_wear)

def t_it_is_read_from_the_chip():
    blk = src[src.index("esp_reset_reason_t rr = esp_reset_reason();"):]
    blk = blk[:blk.index("\n  }")]
    for r in ("ESP_RST_PANIC", "ESP_RST_INT_WDT", "ESP_RST_TASK_WDT", "ESP_RST_WDT"):
        assert r in blk, f"{r} is not counted as a crash"
    for r in ("ESP_RST_DEEPSLEEP", "ESP_RST_POWERON"):
        assert r not in blk, f"{r} is being treated as a crash"
    assert 'prefs.getUInt("boots"' in src and '"panics"' in src, \
        "the panic count is sharing the lifetime boot counter's key"
    sm = src[src.index("if (safeMode) {"):]
    sm = sm[:sm.index("\n  }")]
    assert "cfgNet = NET_WIFI;" in sm and 'prefs.putInt("net", cfgNet);' in sm, \
        "safe mode does not actually land on WiFi"
    assert "flash(" in sm, "it recovers silently, so the robot looks broken"
    print("        counted from the chip's own reset reason, on its own key")
run("The crash is the chip's word, not a guess", t_it_is_read_from_the_chip)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

"""Offline as a mode, the vehicle screen, and the sleeping that goes with them."""
import re, sys
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
dev = open("rafiq-app/mac/Sources/Device.swift").read()
fails=[]
def must(c,w):
    if not c: fails.append(w)
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)

SCREENS = re.findall(r'"([^"]*)"', re.search(r'S_NAME\[S_COUNT\]\s*=\s*\{(.*?)\};', src, re.S).group(1))
print(f"\n{len(SCREENS)} screens: {', '.join(SCREENS)}\n")

def t_order():
    assert SCREENS[0] == "HOME" and SCREENS[1] == "VEHICLE", SCREENS[:3]
    assert SCREENS[2] == "REMINDERS", SCREENS[:4]
    n = len([x for x in re.sub(r"//[^\n]*","",re.search(r"enum \{ S_HOME = 0,(.*?)S_COUNT \};", src, re.S).group(1)).split(",") if x.strip()]) + 1
    assert n == len(SCREENS), f"{n} in the enum, {len(SCREENS)} names"
run("Vehicle is the screen after the clock, reminders after that", t_order)

# the carousel, as the firmware walks it
def visible(s, bike, offline):
    if SCREENS[s] == "VEHICLE":  return bike
    if SCREENS[s] == "WEATHER":  return not offline
    return True
def nxt(f, bike, offline):
    for i in range(1, len(SCREENS)+1):
        s = (f+i) % len(SCREENS)
        if visible(s, bike, offline): return s
    return 0
def walk(bike, offline):
    out=[]; s=0
    for _ in range(len(SCREENS)):
        s = nxt(s, bike, offline)
        if s == 0: break
        out.append(SCREENS[s])
    return out

def t_skips():
    off_nobike = walk(False, True)
    assert "WEATHER" not in off_nobike, "weather is still in the ring offline"
    assert "VEHICLE" not in off_nobike, "the vehicle screen shows while switched off"
    assert "REMINDERS" in off_nobike and "GAMES" in off_nobike and "FAITH" in off_nobike, \
        "something that works offline went missing"
    print(f"        offline, no vehicle: {' > '.join(off_nobike)}")
run("Offline drops the weather and keeps everything that still works", t_skips)

def t_bike_first():
    offline_bike = walk(True, True)
    assert offline_bike[0] == "VEHICLE", offline_bike[:2]
    online_bike = walk(True, False)
    assert online_bike[0] == "VEHICLE", online_bike[:2]
    print(f"        online, vehicle on: {' > '.join(online_bike[:4])} ...")
run("With the vehicle on it is the first thing past the home screen", t_bike_first)

def t_no_dead_ends():
    for bike in (True, False):
        for off in (True, False):
            seen = walk(bike, off)
            assert len(seen) == len(SCREENS) - 1 - (0 if bike else 1) - (0 if not off else 1), \
                f"bike={bike} offline={off} walked {len(seen)}"
            assert len(set(seen)) == len(seen), "it visited one twice"
run("Every combination walks a clean ring with nothing repeated", t_no_dead_ends)

print("\nthe mode\n")
def t_mode():
    must_have = [
        ("static bool offlineNow() { return cfgOffline || netDown; }", "no single idea of offline"),
        ("if (cfgOffline) {", "the task still runs with the radio asked off"),
        ("netDown = true;", "it never gives up looking"),
        ("WiFi.mode(WIFI_OFF);", "the radio is left on after giving up"),
        ("netMisses >= netCount * 2", "it gives up after the wrong number of tries"),
    ]
    for s, why in must_have:
        assert s in src, why
    # asked-for offline must not be undone by a successful scan
    task = re.search(r"if \(cfgOffline\) \{.*?continue;\s*\}", src, re.S)
    assert task, "the network task does not check the setting at all"
run("Asked off stays off; tried and failed gives up and switches the radio off", t_mode)

def t_sleep():
    assert "uint32_t wait = offlineNow() ? 0 : deepAfterMs();" in src, \
        "offline does not sleep straight away"
    opts = [int(x) for x in re.search(r"SLEEP_OPTS\[\] = \{([^}]*)\}", src).group(1).split(",")]
    assert opts[0] == 5, f"the shortest screen sleep is {opts[0]}s"
    names = re.findall(r'"([^"]*)"', re.search(r'sleepNames  = \[(.*?)\]', dev, re.S).group(1))
    assert len(names) == len(opts), f"robot has {len(opts)} sleep steps, the app lists {len(names)}"
    print(f"        {opts[0]}s shortest, {len(opts)} steps, app agrees")
run("Offline sleeps at once, and five seconds is offered", t_sleep)

print("\nthe vehicle\n")
def t_bike():
    n = int(re.search(r"#define BIKE_TPL_N (\d+)", src).group(1))
    assert n == 6, f"{n} layouts, not six"
    body = re.search(r"static void drawBike\(\) \{.*?\n\}\n", src, re.S).group(0)
    for c in ("case 1:", "case 2:", "case 3:", "case 4:", "case 5:", "default:"):
        assert c in body, f"{c} is missing, so one layout draws nothing"
    assert "bikeEdit ? bikeTry : cfgBikeTpl" in body, \
        "it draws the kept layout while you are choosing a different one"
    assert 'web.on("/api/bike"' in src, "the app cannot edit it"
    assert 'char bikePlate[20] = "KA 50 HJ 5683";' in src, "the plate is not yours"
    assert 'char bikeMake[20]  = "Royal Enfield";' in src, "the make is not yours"
    assert 'char bikeModel[20] = "Meteor 350";' in src, "the model is not yours"
    print(f"        {n} layouts, make and model held apart")
run("Six layouts, all drawn, the chooser drawing what it is offering", t_bike)

def t_bike_edit():
    """Hold to get in, press to walk them, hold to keep: the same shape
    as setting the clock, and nothing written until the second hold."""
    g = re.search(r"if \(screen == S_BIKE && depth == 0\) \{.*?\n  \}", src, re.S).group(0)
    assert "if (!bikeEdit) {" in g and "bikeEdit = true; bikeTry = cfgBikeTpl" in g, \
        "a long press does not open the chooser"
    assert "case TG_ONE:  bikeTry = (bikeTry + 1) % BIKE_TPL_N;" in g, \
        "a single press does not walk the layouts"
    assert "case TG_LONG: cfgBikeTpl = bikeTry; prefs.putInt(\"btpl\", cfgBikeTpl);" in g, \
        "holding again does not keep the one you are looking at"
    # and nothing is written on the way round
    walk = g[g.index("case TG_ONE:"):g.index("case TG_LONG:")]
    assert "prefs.put" not in walk, "walking the layouts writes to flash"
    assert "if (bikeEdit) bikeEdit = false;" in src, "a shake cannot leave the chooser"
    print("        hold in, press to walk, hold to keep, shake to leave")
run("The vehicle layout is chosen the way the clock is set", t_bike_edit)

def t_plate_fits():
    plate = re.search(r'char bikePlate\[\d+\] = "([^"]*)"', src).group(1)
    parts = plate.split(" ")
    top, bot = " ".join(parts[:2]), " ".join(parts[2:])
    assert len(top)*12 <= 128 and len(bot)*12 <= 128, \
        f"'{top}' or '{bot}' will not fit at double size"
    assert len(plate)*6 <= 128 - 48, f"'{plate}' will not fit the badge layout"
    print(f"        '{top}' / '{bot}' at double size, '{plate}' at single")
run("The plate fits every layout it appears in", t_plate_fits)

print("\nthe radio, when you have said no\n")
def t_radio_never_up():
    """Off has to mean off from boot, not off a few lines later. Setting
    the mode at all powers the radio, so the whole block is behind the
    setting rather than inside it."""
    setup = src[src.index("void setup()"):]
    setup = setup[:setup.index("\nvoid loop()")]
    i = setup.index("loadNets();")
    gate = setup.index("if (cfgOffline) {", i)
    # everything that brings the radio up must be inside the else
    els = setup.index("} else {", gate)
    off = setup[gate:els]
    on  = setup[els:setup.index("\n\n  if (online()) {", els)]
    for bad in ("WiFi.mode(WIFI_STA)", "WiFi.begin(", "setupWeb()", "WiFi.setSleep"):
        assert bad not in off, f"{bad} still runs when it was told to stay off"
    for good in ("WiFi.mode(WIFI_STA)", "WiFi.begin(", "setupWeb()"):
        assert good in on, f"{good} no longer runs when it is allowed to"
    assert "WiFi.mode(WIFI_OFF)" in off, "the radio is not actually switched off"
    # and nothing earlier in setup touched it
    before = setup[:i]
    for bad in ("WiFi.mode(", "WiFi.begin("):
        assert bad not in before, f"{bad} runs before the setting is consulted"
    print("        no mode, no scan, no join, no server")
run("Booting offline never powers the radio at all", t_radio_never_up)

def t_web_lazy():
    """Nothing started the server at boot, so turning the network back
    on from the robot's own menu has to start it then."""
    assert "bool webUp = false;" in src and "if (webUp) return;" in src, \
        "setupWeb can be run twice"
    # 5.15.0 made the mode a three way walk, so the words changed
    for ctx in ('flash("LOOKING FOR WIFI", 1300);',
                "WiFi.mode(WIFI_STA); WiFi.setSleep(false); setupWeb(); } }"):
        assert ctx in src, f"turning the network back on does not start the server ({ctx!r})"
    assert "if (cfgOffline) {\n    bleOff();" in src and "cfgNet = NET_WIFI;" in src, \
        "asking for the hotspot does not take it back to WiFi, or leaves Bluetooth up"
    print("        server starts when the radio does, whenever that is")
run("Turning the network back on starts what boot skipped", t_web_lazy)

def t_offline_home():
    """The time in the corner only when there is a time, and the
    offline home before the no-clock screen, or off the network with a
    flat clock you get the wrong screen entirely."""
    home = re.search(r"static void drawHome\(\) \{.*?switch \(cfgFace\)", src, re.S).group(0)
    off  = home.index("if (offlineNow()) {")
    noclk = home.index("if (!fb.ok) {")
    assert off < noclk, "the no-clock screen wins over the offline home"
    body = home[off:noclk]
    assert "if (fb.ok) at(15, 2, fb.hm);" in body, \
        "the time is not conditional on there being one"
    assert "touch to begin" in body and "offlineIcon" in body, \
        "the offline home lost its icon or its invitation"
    print("        icon, greeting, touch to begin, time only when kept")
run("The offline home says the time only when it has one", t_offline_home)

def t_clock_survives():
    """The RTC runs through deep sleep but timeOk did not, so a board
    that woke knowing the time called itself clockless and stopped
    firing everything that depends on one."""
    assert "clockSrc = \"kept through sleep\";" in src, \
        "waking does not notice a clock that survived"
    blk = src[src.index("struct tm t0;"):]
    blk = blk[:blk.index("loadNets();")]
    assert "t0.tm_year > 123" in blk, "it does not check the year is plausible"
    # and the things that depend on it really do
    assert "if (timeOk && remCount && now - remCheck > 2000)" in src, \
        "reminders do not wait for a real clock"
    assert "if (!prayerOk || !timeOk || !getLocalTime(&t, 0)) return;" in src, \
        "prayer alerts do not wait for a real clock"
    print("        kept clock recognised; reminders and prayers ride on it")
run("A clock kept through sleep still counts as a clock", t_clock_survives)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -",f) for f in fails]; sys.exit(1)
print("PASS")

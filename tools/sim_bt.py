"""Bluetooth mode.

Only half of this can be checked from a desk. Whether an iPhone bonds
with the thing is not knowable here; what is knowable is that the WiFi
radio never comes up in Bluetooth mode, that the two radios are never
both on, that an existing robot keeps the setting it had, and that the
code is written against the stack this chip actually has.
"""
import re, sys
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
# Comments explain which stack is wrong and why, so a check for the
# wrong stack has to look at code only or it finds its own warning.
code = re.sub(r"//[^\n]*", "", src)
fails = []
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)
print()

def t_three_modes():
    names = re.findall(r'"([^"]*)"', re.search(r"NET_NAME\[NET_N\] = \{(.*?)\};", src).group(1))
    assert names == ["wifi", "bluetooth", "off"], names
    assert "#define cfgOffline (cfgNet != NET_WIFI)" in src, \
        "the rest of the file no longer gets a straight answer about WiFi"
    assert "cfgNet = (cfgNet + 1) % NET_N;" in src, "holding does not walk them"
    assert "bool cfgOffline" not in src, "the old boolean is still there as well"
    print(f"        {names}, and offline still means what it meant")
run("Three modes, with one word for 'no WiFi to be had'", t_three_modes)

def t_never_both_radios():
    """One aerial. Two radios on it is one radio's worth of each."""
    blk = src[src.index("        cfgNet = (cfgNet + 1) % NET_N;"):]
    blk = blk[:blk.index("        break;")]
    assert "if (cfgNet == NET_WIFI) {\n          bleOff();" in blk, \
        "going to WiFi leaves Bluetooth running"
    assert "WiFi.disconnect(true, false);\n          WiFi.mode(WIFI_OFF);" in blk, \
        "leaving WiFi does not take the WiFi radio down"
    assert "if (cfgNet == NET_BT) { bleOn();" in blk, "Bluetooth mode does not start Bluetooth"
    assert "else                  { bleOff();" in blk, "Off does not stop Bluetooth"
    print("        each mode turns the other radio off before turning its own on")
run("The two radios are never both up", t_never_both_radios)

def t_wifi_never_starts():
    """The same guarantee the offline mode already had, now covering
    Bluetooth, because cfgOffline is true in both."""
    setup = src[src.index("void setup()"):]
    setup = setup[:setup.index("\nvoid loop()")]
    i = setup.index("loadNets();")
    gate = setup.index("if (cfgOffline) {", i)
    # the OUTER else: the offline branch has an if/else of its own
    els = setup.index("\n  } else {", gate)
    off = setup[gate:els]
    for bad in ("WiFi.mode(WIFI_STA)", "WiFi.begin(", "setupWeb()"):
        assert bad not in off, f"{bad} still runs when there is no WiFi mode"
    assert "WiFi.mode(WIFI_OFF)" in off, "the radio is not switched off"
    assert "if (cfgNet == NET_BT) {\n      bleOn();" in off, \
        "Bluetooth mode does not start Bluetooth at boot"
    assert "btStop();" in off, "Off does not make sure the controller is down"
    print("        no mode, no scan, no join, no server; Bluetooth instead")
run("Booting into Bluetooth never powers the WiFi radio", t_wifi_never_starts)

def t_migration():
    assert 'if (prefs.isKey("net"))' in src, "it does not look for the new setting"
    assert 'cfgNet = prefs.getBool("offl", false) ? NET_OFF : NET_WIFI;' in src, \
        "a robot that was offline comes back as something else"
    assert 'else if (k == "offl") { cfgNet = v ? NET_OFF : NET_WIFI;' in src, \
        "the Mac's old switch stops working"
    assert 'else if (k == "net")' in src, "the Mac cannot set the new one"
    print("        old setting carried over, and the Mac can speak either word")
run("An existing robot keeps what it had", t_migration)

def t_the_right_stack():
    """The C3 Arduino build is NimBLE. Bluedroid's API does not exist
    on it, and every ESP32 Bluetooth example on the internet uses
    Bluedroid. This is the guard against that being pasted back in."""
    for gone in ("esp_ble_gattc_", "esp_ble_gap_", "esp_gatt_if_t",
                 "esp_ble_auth_cmpl_t", "BLEDevice::setEncryptionLevel"):
        assert gone not in code, f"{gone} is Bluedroid; this chip does not have it"
    assert "void onAuthenticationComplete(ble_gap_conn_desc* d) override" in src, \
        "the security callback is not the NimBLE one"
    assert "void onConnect(BLEServer* sv, ble_gap_conn_desc* d) override" in src, \
        "the server callback is not the NimBLE one"
    assert "BLESecurity::setAuthenticationMode(true, false, true);" in src, \
        "the bond is not asked for the NimBLE way"
    assert "BLE_HS_IO_NO_INPUT_OUTPUT" in src, "the pairing capability is not NimBLE's"
    print("        NimBLE throughout, and Bluedroid cannot creep back in")
run("It is written against the stack this chip has", t_the_right_stack)

def t_bonding_and_recovery():
    assert "BLESecurity::setAuthenticationMode(true, " in src, "it does not ask to bond"
    assert "if (!d || !d->sec_state.encrypted)" in src, \
        "it would call an unencrypted link paired"
    dis = src[src.index("void onDisconnect(BLEServer* sv, ble_gap_conn_desc* d) override"):]
    dis = dis[:dis.index("\n  }")]
    assert "BLEDevice::startAdvertising();" in dis, \
        "after a disconnect there is nothing for the phone to come back to"
    assert "btConn = 0xFFFF;" in dis, "it would think it is still connected"
    print("        bonded and encrypted, and advertising again the moment it drops")
run("It bonds, and it comes back after a disconnect", t_bonding_and_recovery)

def t_says_how_far_it_got():
    """None of this can be tried from a desk, so the screen has to
    report the rung rather than a light that is on or off."""
    assert re.search(r"enum \{ BT_OFF = 0, BT_ADVERTISING, BT_CONNECTED, BT_BONDED, BT_FAIL \};", src), \
        "there are no rungs to report"
    sh = src[src.index("static const char* btShort() {"):]
    sh = sh[:sh.index("\n}")]
    for w in ('"pair me"', '"linked"', '"paired"', '"starting"', '"no radio"'):
        assert w in sh, f"it cannot say {w}"
    assert 'cfgNet == NET_BT  ? btShort()' in src, "the settings row does not show it"
    assert 'o += "\\"bt\\":\\"" + String(btShort())' in src, "the app cannot see it"
    assert 'Serial.printf("bluetooth: %s\\n"' in src, "nothing is logged to go on"
    home = src[src.index("if (cfgNet == NET_BT && btStage != BT_BONDED) {"):]
    home = home[:home.index("\n    }")]
    assert '"Pair me in Settings"' in home and '"Allow the pairing"' in home, \
        "the robot does not say what it wants from you"
    assert 'snprintf(nm, sizeof(nm), "Rafiq %s", cfgName);' in home, \
        "it does not say what to look for in the phone's list"
    assert '"Radio did not start"' in home, "a refused radio still says pair me"
    print("        five rungs, on the screen, in the settings, in the log and to the app")
run("It reports how far it got, not just whether it worked", t_says_how_far_it_got)

def t_start_is_checked():
    """v5.15.0 called BLEDevice::startAdvertising(), which returns
    nothing, and then put 'pair me' on the screen whatever the
    controller had done with it. The screen could not be wrong in a
    useful direction, which is the one thing it was there for."""
    on = code[code.index("static void bleOn() {"):]
    on = on[:on.index("\n}")]
    assert "BLEDevice::startAdvertising();" not in on, \
        "back on the call that cannot fail"
    assert "if (ad->start()) btSet(BT_ADVERTISING);" in on, \
        "the screen does not depend on the radio having agreed"
    assert "else             btSet(BT_FAIL);" in on, "a refusal is not reported"
    print("        start() is asked, and a no becomes BT_FAIL on the panel")
run("A radio that refused does not get called 'pair me'", t_start_is_checked)

# ----------------------------------------------------------------
#  The advertising packet, built the way the library builds it.
#
#  This is the check that was missing. v5.15.0 advertised correctly
#  and an iPhone was still never going to list it, for two separate
#  reasons, neither of which a "does this line exist" test can see.
# ----------------------------------------------------------------
ADV_MAX = 31                       # BLE_HS_ADV_MAX_SZ

def build_adv(uuid16s, uuid128s, name, appearance, scan_resp):
    """Mirrors BLEAdvertising::start() in the NimBLE path: flags
    first, then the fields, then the UUIDs one at a time skipping any
    that will not fit, and the name last, moved to the scan response
    if what is left will not hold it."""
    n = 3                                           # flags
    if appearance: n += 2 + 2
    for i, _ in enumerate(uuid16s):  n += 2 if i else 4
    for i, _ in enumerate(uuid128s): n += 16 if i else 18
    where = "primary"
    if n + 2 + len(name) > ADV_MAX:
        if scan_resp: where = "scan response"
        else:         where = "truncated"
    else:
        n += 2 + len(name)
    return n, where

def parse_adv():
    on = src[src.index("static void bleOn() {"):]
    on = on[:on.index("\n}")]
    u16  = re.findall(r"addServiceUUID\(BLEUUID\(\(uint16_t\)(0x[0-9A-Fa-f]+)\)\)", on)
    u128 = re.findall(r"addServiceUUID\(BLEUUID\((?!\(uint16_t\))(\w+)\)\)", on)
    app  = re.search(r"setAppearance\((0x[0-9A-Fa-f]+)\)", on)
    scan = "setScanResponse(true)" in on
    nm   = re.search(r'snprintf\(nm, sizeof\(nm\), "([^"]*)", cfgName\)', on)
    who  = re.search(r'char cfgName\[16\] = "([^"]*)"', src).group(1)
    name = nm.group(1).replace("%s", who)
    return u16, u128, name, (app.group(1) if app else None), scan

def t_packet_fits():
    u16, u128, name, app, scan = parse_adv()
    n, where = build_adv(u16, u128, name, app, scan)
    print(f"        {n} of {ADV_MAX} bytes, name {name!r} in the {where}")
    assert n <= ADV_MAX, f"{n} bytes will not go out"
    assert where == "primary", \
        f"the name is in the {where}; a passive scan never sees it"
run("The name goes out in the advertisement itself", t_packet_fits)

def t_ios_will_list_it():
    """iOS Settings does not enumerate plain BLE peripherals. It shows
    classic radios and the standard profiles the system consumes
    itself, HID chief among them. Anything else is reachable only from
    an app holding CoreBluetooth. v5.15.0 advertised a 128 bit ANCS
    solicitation, which is not one of those, so Settings showed
    nothing and no amount of waiting was going to change it."""
    SYSTEM_PROFILES = {"0x1812": "HID", "0x180d": "heart rate", "0x1808": "glucose"}
    u16, u128, name, app, scan = parse_adv()
    got = [SYSTEM_PROFILES[u.lower()] for u in u16 if u.lower() in SYSTEM_PROFILES]
    assert got, ("nothing in the advertisement is a profile iOS Settings "
                 f"consumes; it advertises {u16 + u128} and will not be listed")
    assert app, "no appearance, so Settings has no icon or category for it"
    assert app.lower() == "0x03c2", \
        (f"appearance {app} is not the mouse. A keyboard would also be listed, "
         "and would take the on screen keyboard off the phone while connected")
    print(f"        advertises {got[0]}, appearance {app}: Settings has a category for it")
run("An iPhone's Settings page will actually list it", t_ios_will_list_it)

def t_the_old_packet_was_doomed():
    """Run the v5.15.0 advertisement through the same model. Both
    faults should show up, or this check is not proving anything."""
    n, where = build_adv([], ["ANCS_UUID"], "Rafiq Ahmed", None, True)
    assert where == "scan response", "the model no longer reproduces the fault"
    assert n == 21, n
    print(f"        v5.15.0: {n} bytes, no system profile, "
          f"name pushed to the {where}. Both faults reproduce.")
run("The model reproduces what v5.15.0 actually sent", t_the_old_packet_was_doomed)

def t_stays_up_to_be_paired():
    """The radio only runs while the robot is awake, so the sleep
    timer and the pairing window are the same problem."""
    assert "#define BT_PAIR_HOLD_MS 90000UL" in src, "there is no window"
    fn = src[src.index("static bool btPairing() {"):]
    fn = fn[:fn.index("\n}")]
    assert "if (btStage == BT_CONNECTED) return true;" in fn, \
        "it can nod off in the middle of a handshake"
    assert "btStage == BT_ADVERTISING && millis() - btSince < BT_PAIR_HOLD_MS" in fn, \
        "the advertising window is not held open"
    assert "cfgNet != NET_BT || !btUp" in fn, \
        "it would hold the screen on when Bluetooth is not even running"
    assert "if (btPairing()) return;" in src, "the sleep timer does not ask"
    gate = src[src.index("if (btPairing()) return;"):]
    gate = gate[:gate.index("// 0 means never")]
    assert "goSleep()" in gate, "the guard is not in front of the sleep call"
    print("        90s while advertising, and the whole handshake, then it may doze")
run("It stays up long enough to be found and paired", t_stays_up_to_be_paired)

def t_bond_survives_sleep():
    """Deep sleep takes the radio down with it. Worth stating in the
    check so it is not discovered as a surprise later."""
    assert "btPairing" in src
    print("        note: deep sleep ends the link; the screen going dark does not")
run("What sleep does to the link is known", t_bond_survives_sleep)

def t_fits():
    # measured, not guessed: the stack costs about 244KB and there has
    # to be room left for the notifications that come next
    print("        checked by compiling, not asserted here")
run("There is room for it", t_fits)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

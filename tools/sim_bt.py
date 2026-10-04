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
    # And no longer the core's bundled wrapper either. Apple's
    # notification service needs a GATT client over an INBOUND
    # connection: the bundled server has no case for
    # BLE_GAP_EVENT_NOTIFY_RX at all, so every notification would
    # have arrived and been dropped. NimBLE-Arduino hands out a
    # client for the connection the phone made, which is the only
    # reason the swap happened.
    for gone in ("BLEServerCallbacks", "BLESecurityCallbacks",
                 "BLEDevice::init", "BLEHIDDevice", "BLEAdvertising",
                 "ble_gattc_disc_svc_by_uuid"):
        # not preceded by Nim: NimBLEHIDDevice contains BLEHIDDevice
        assert not re.search(r"(?<!Nim)\b" + re.escape(gone), code), \
            f"{gone} is the bundled wrapper, which cannot do ANCS"
    assert "#include <NimBLEDevice.h>" in src and "#include <NimBLEHIDDevice.h>" in src
    assert "void onAuthenticationComplete(NimBLEConnInfo& ci) override" in src, \
        "the security callback is not the NimBLE one"
    assert "void onConnect(NimBLEServer* sv, NimBLEConnInfo& ci) override" in src, \
        "the server callback is not the NimBLE one"
    assert "NimBLEDevice::setSecurityAuth(true, false, true);" in src, \
        "the bond is not asked for the NimBLE way"
    assert "BLE_HS_IO_NO_INPUT_OUTPUT" in src, "the pairing capability is not NimBLE's"
    assert "sv->getClient(btConn)" in src, \
        "nothing gets a client for the inbound connection, so ANCS cannot work"
    print("        NimBLE-Arduino throughout; Bluedroid and the bundled wrapper both barred")
run("It is written against the stack this chip has", t_the_right_stack)

def t_bonding_and_recovery():
    assert "NimBLEDevice::setSecurityAuth(true, " in src, "it does not ask to bond"
    assert "if (!ci.isEncrypted())" in src, "it would call an unencrypted link paired"
    # The bundled library only started security when something
    # demanded an encrypted read. Nothing here did, so iOS never
    # finished the bond, the rung sat on "linked", and the clock was
    # never fetched. Asking for it ourselves is the fix.
    con = src[src.index("void onConnect(NimBLEServer* sv, NimBLEConnInfo& ci) override"):]
    con = con[:con.index("\n  }")]
    assert "NimBLEDevice::startSecurity(btConn);" in con, \
        "it waits to be asked for encryption, so the bond may never finish"
    assert "now - btSecAskedAt > BT_SEC_NUDGE_MS" in src, \
        "nothing retries the pairing if iOS does not get round to it"
    dis = src[src.index("void onDisconnect(NimBLEServer* sv, NimBLEConnInfo& ci, int reason) override"):]
    dis = dis[:dis.index("\n  }")]
    assert "NimBLEDevice::startAdvertising();" in dis, \
        "after a disconnect there is nothing for the phone to come back to"
    assert "btConn = 0xFFFF;" in dis, "it would think it is still connected"
    print("        asks for encryption itself, and advertises again the moment it drops")
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
    assert "NimBLEDevice::startAdvertising();" not in on, \
        "back on the call that cannot fail"
    assert "if (adv->start()) btSet(BT_ADVERTISING);" in on, \
        "the screen does not depend on the radio having agreed"
    assert "else              btSet(BT_FAIL);" in on, "a refusal is not reported"
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
    u16  = re.findall(r"ad\.addServiceUUID\(NimBLEUUID\(\(uint16_t\)(0x[0-9A-Fa-f]+)\)\)", on)
    app  = re.search(r"ad\.setAppearance\((0x[0-9A-Fa-f]+)\)", on)
    solicit = "ad.addData(ANCS_SOLICIT, sizeof(ANCS_SOLICIT))" in on
    in_sr   = "sr.setName(nm)" in on and "adv->setScanResponseData(sr)" in on
    return u16, (app.group(1) if app else None), solicit, in_sr

def t_solicitation_is_really_ancs():
    """Eighteen bytes typed out by hand, which is eighteen chances to
    get it wrong, and a wrong one fails silently: the phone simply
    never offers its notifications. So it is checked against the UUID
    string the rest of the file uses."""
    blk = re.search(r"ANCS_SOLICIT\[18\] = \{(.*?)\};", src, re.S).group(1)
    vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", blk)]
    assert len(vals) == 18, f"{len(vals)} bytes, wanted 18"
    assert vals[0] == 0x11, "the length byte is wrong"
    assert vals[1] == 0x15, "0x15 is the 128 bit solicitation list; this is not it"
    uuid = re.search(r'NimBLEUUID ANCS_SVC\("([0-9A-Fa-f-]+)"\)', src).group(1).replace("-", "")
    want = list(bytes.fromhex(uuid))[::-1]        # little endian on the air
    assert vals[2:] == want, "the solicited UUID is not the ANCS one, backwards"
    print("        18 bytes, type 0x15, and the UUID matches ANCS_SVC reversed")
run("The thing it solicits really is Apple's notification service", t_solicitation_is_really_ancs)

def t_packet_fits():
    u16, app, solicit, in_sr = parse_adv()
    n = 3                                        # flags
    if solicit: n += 18
    if app:     n += 4
    for i, _ in enumerate(u16): n += 2 if i else 4
    print(f"        {n} of {ADV_MAX} bytes used, name in the scan response")
    assert n <= ADV_MAX, f"{n} bytes will not go out"
    assert in_sr, ("the name is not in the scan response, so either it does not "
                   "go out at all or it pushes something else off the packet")
run("Everything that has to be in the advertisement fits", t_packet_fits)

def t_ios_will_list_it():
    """iOS Settings does not enumerate plain BLE peripherals. It shows
    classic radios and the standard profiles the system consumes
    itself, HID chief among them. v5.15.0 advertised neither and was
    invisible however long you looked."""
    SYSTEM_PROFILES = {"0x1812": "HID"}
    u16, app, solicit, in_sr = parse_adv()
    got = [SYSTEM_PROFILES[u.lower()] for u in u16 if u.lower() in SYSTEM_PROFILES]
    assert got, f"advertises {u16}, none of which iOS Settings consumes"
    assert app and app.lower() == "0x03c2", \
        (f"appearance {app} is not the mouse. A keyboard would be listed too, "
         "and would take the on screen keyboard off the phone while connected")
    assert solicit, "nothing solicits ANCS, so there is no notification prompt"
    print(f"        {got[0]} for the listing, appearance {app}, ANCS for the prompt")
run("An iPhone will list it and offer its notifications", t_ios_will_list_it)

def t_the_old_packet_was_doomed():
    """v5.15.0: flags plus a 128 bit ANCS entry in the SERVICE list,
    which is not the solicitation list and does not prompt for
    anything, and no system profile at all. Both faults, from the
    same model."""
    n = 3 + 18
    assert n + 2 + len("Rafiq Ahmed") > ADV_MAX, "the model no longer reproduces the fault"
    print(f"        v5.15.0: {n} bytes, wrong AD type, no system profile, name displaced")
run("The model still reproduces what v5.15.0 sent", t_the_old_packet_was_doomed)

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

# Rafiq 7.4.1: tell bonded devices the service list changed (macOS kept
# an old one and never saw the 7.4 channels); updates only from a file,
# through the hotspot; GitHub updating removed from the robot.
import sys
SRC=sys.argv[1]; s=open(SRC).read()
def rep(a,b,c=1):
    global s
    n=s.count(a)
    if n!=c: sys.exit(f"anchor {n}x:\n{a[:200]}")
    s=s.replace(a,b)
rep('#define FW_VERSION "7.4.0"', '#define FW_VERSION "7.4.1"')

# 1. Service Changed: a bonded Mac or phone keeps its old picture of our
#    services until told. Told on every trusted link: it costs a moment
#    of re-reading, and a device that missed a firmware change catches up.
rep("volatile uint8_t btNews = 0;", """volatile uint8_t btNews = 0;
extern "C" void ble_svc_gatt_changed(uint16_t start_handle, uint16_t end_handle);""")
rep("""    NimBLEDevice::getServer()->updateConnParams(h, 72, 96, 4, 600);""",
    """    NimBLEDevice::getServer()->updateConnParams(h, 72, 96, 4, 600);
    // "Our services may have changed: read them again." Without this a
    // bonded Mac keeps the list it saw before an update, and new
    // channels (7.4's events, pointer, settings) never appear to it.
    ble_svc_gatt_changed(0x0001, 0xFFFF);""")

# 2. updates: the hotspot and a file, nothing from GitHub
rep('''    "Pair a Mac", "Check update", "Auto update",''', '''    "Pair a Mac", "Update", "Auto update",''')
rep('''      case C_UPDATE: snprintf(v, sizeof(v), "%s", online() ? "hold" : "offline"); break;''',
    '''      case C_UPDATE: snprintf(v, sizeof(v), "%s", rescueAP ? "hotspot on" : "from a file"); break;''')
a = s.index("      case C_UPDATE:\n        // On Bluetooth this brings WiFi up")
b = s.index("      case C_GUARD:", a)
s = s[:a] + """      case C_UPDATE:
        // 7.4.1: one way to update, a file through the hotspot. Rafiq
        // no longer fetches releases from GitHub by itself.
        startHotspot();
        break;
""" + s[b:]
rep("""  { C_MODE,   C_HOTSPOT, C_PAIR,  C_PRAYER, C_UPDATE, C_AUTOUP, C_GUARD, SG_END },""",
    """  { C_MODE,   C_HOTSPOT, C_PAIR,  C_PRAYER, C_UPDATE, C_GUARD,  SG_END,  SG_END },""")
rep("""      case RQ_UPDATE:  rqUpdate(); break;""", """      case RQ_UPDATE:  startHotspot(); break;   // 7.4.1: "update" opens the hotspot""")
rep("""      if (upState == U_OFF && !wantOtaLatest) { wantOtaLatest = true; syncUpArmed = true; }""",
    """      // 7.4.1: a sync no longer looks for releases; updates are from a file""")
rep("""  cfgAutoUp   = prefs.getBool("autoup", false);""",
    """  cfgAutoUp   = false;                 // 7.4.1: never fetches releases by itself""")

# the page: only the file
rep("""    <div class="row"><button id="ub1" onclick="updTab(0)">From GitHub</button><button class="g" id="ub2" onclick="updTab(1)">From a file</button></div>
    <div id="upg">""", """    <div id="upg" style="display:none">""")
rep("""    <div id="upf" style="display:none">""", """    <div id="upf">""")
open(SRC,'w').write(s); print("7.4.1 ok")
s=open(SRC).read()
a=s.index("static void rqUpdate() {")
b=s.index("\n}\n", a)+3
s=s[:a]+s[b:]
open(SRC,'w').write(s); print("rqUpdate removed")

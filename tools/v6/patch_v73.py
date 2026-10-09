#!/usr/bin/env python3
# Rafiq 7.3.0: two links at once with preferred devices, automatic
# Away when the phone has gone, and new timer and contact screens.
# Applied after patch_v721.
import re, sys
SRC = sys.argv[1]
s = open(SRC).read()
def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count: sys.exit(f"anchor {n}x:\n{old[:220]}")
    s = s.replace(old, new)
def replace_func(name, new):
    global s
    m = re.search(r'\n(static [^\n;{]*?\b' + re.escape(name) + r'\([^)]*\)\s*\{)', s)
    if not m: sys.exit("no function " + name)
    i = m.end() - 1; d = 0
    for j in range(i, len(s)):
        if s[j] == '{': d += 1
        elif s[j] == '}':
            d -= 1
            if d == 0: s = s[:m.start(1)] + new + s[j + 1:]; return

rep('#define FW_VERSION "7.2.1"', '#define FW_VERSION "7.3.0"')

# =================================================================
# GLOBALS
# =================================================================
rep("volatile uint8_t btNews = 0;", """volatile uint8_t btNews = 0;
// ---- 7.3: devices and links ----
//  Every device that has paired is remembered with a name. Two can be
//  preferred: Primary (the notification phone: clock, notifications,
//  guard, auto Away) and Second (a companion, say the Mac). With
//  Multi-link on, two stay connected; a preferred device arriving takes
//  the place of one that is not. btConn stays "the phone" for all the
//  older code; btConn2 is the companion.
struct DevRec { uint8_t a[6]; char label[18]; };
#define DEV_N 6
DevRec   devs[DEV_N];
int      devN = 0;
uint8_t  prefA[2][6];                  // all zero: no preference
bool     cfgMulti = false, cfgAutoAway = true;
volatile uint16_t btConn2 = 0xFFFF;
struct LinkRec { uint16_t h; uint8_t a[6]; bool authed; };
LinkRec  links[3];
int      linkN = 0;                    // set on the NimBLE task, read by loop
volatile uint16_t authQ[4];            // links that just finished pairing, for loop
volatile uint8_t  authHead = 0, authTail = 0;
bool     btEverLinked = false;
// ---- 7.3: automatic Away ----
bool     awayAuto = false;             // this Away began because the phone left
RTC_DATA_ATTR uint8_t rtcAwayCheck = 0;
bool     awayCheckBoot = false, awayQuietDeep = false;
#define  AUTO_AWAY_MS 120000UL""")

# =================================================================
# LINKS: the callbacks only write down; loop decides
# =================================================================
m = re.search(r"class BtServerCb : public NimBLEServerCallbacks \{.*?\n\};\n", s, re.S)
if not m: sys.exit("BtServerCb not found")
s = s[:m.start()] + r'''static int linkFind(uint16_t h) { for (int i = 0; i < linkN; i++) if (links[i].h == h) return i; return -1; }
static void linkDrop(uint16_t h) {
  int i = linkFind(h);
  if (i < 0) return;
  for (int k = i; k < linkN - 1; k++) links[k] = links[k + 1];
  linkN--;
}
// Everything that was set up on the phone's link belongs to that link.
static void phoneReset(uint16_t h) {
  btConn = h;
  btClStale = true; btApp = false;
  ctsState = CTS_IDLE; ctsSyncedAt = 0; ctsFails = 0;
  ancsState = (h == 0xFFFF) ? ANCS_NONE : ANCS_WAIT; ancsTries = 0;
  ancsCP = nullptr; ancsBusy = false;
  uidHead = uidTail = 0; dsLen = 0;
}

class BtServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* sv, NimBLEConnInfo& ci) override {
    uint16_t h = ci.getConnHandle();
    if (linkN < 3) { links[linkN].h = h; memset(links[linkN].a, 0, 6); links[linkN].authed = false; linkN++; }
    if (btConn == 0xFFFF) {                       // the first link is the phone, until told otherwise
      phoneReset(h);
      btSet(ci.isEncrypted() ? BT_BONDED : BT_CONNECTED);
      btWokeReq = true;
    } else {
      btConn2 = h;                                // a companion
    }
    // Ask for the encryption ourselves rather than waiting to be
    // asked; with nothing demanding it the bond never completed.
    btSecAskedAt = millis();
    NimBLEDevice::startSecurity(h);
    // With Multi-link on, keep a door open: one more can come in, and
    // loop decides who stays (two at most, preferred devices first).
    if (cfgMulti && linkN < 3) NimBLEDevice::startAdvertising();
  }
  void onDisconnect(NimBLEServer* sv, NimBLEConnInfo& ci, int reason) override {
    uint16_t h = ci.getConnHandle();
    linkDrop(h);
    if (h == btConn2) btConn2 = 0xFFFF;
    if (h == btConn) {
      btDropAt = millis();
      // The phone went. If a companion is still here, it becomes the
      // phone, so the clock and the rest carry on from it.
      uint16_t other = btConn2;
      btConn2 = 0xFFFF;
      phoneReset(other);
      if (other != 0xFFFF) btSet(BT_BONDED);
      else                 btSet(BT_ADVERTISING);
    }
    // Straight back to advertising, or the phone has nothing to come
    // back to and you would be pairing it by hand every time.
    NimBLEDevice::startAdvertising();
  }
  void onAuthenticationComplete(NimBLEConnInfo& ci) override {
    uint16_t h = ci.getConnHandle();
    if (!ci.isEncrypted()) {
      Serial.println("bluetooth: pairing did not take");
      if (h == btConn) btSet(BT_CONNECTED);
      return;
    }
    int i = linkFind(h);
    if (i >= 0) { memcpy(links[i].a, ci.getIdAddress().getVal(), 6); links[i].authed = true; }
    uint8_t nx = (authHead + 1) % 4;
    if (nx != authTail) { authQ[authHead] = h; authHead = nx; }
    if (h == btConn) btSet(BT_BONDED);
  }
};

// ---- the device list, kept in flash ----
static void devSave() {
  prefs.putBytes("devs", devs, sizeof(DevRec) * devN);
  prefs.putInt("devn", devN);
  prefs.putBytes("prefa", prefA, sizeof(prefA));
}
static void devLoad() {
  devN = constrain(prefs.getInt("devn", 0), 0, DEV_N);
  if (devN) prefs.getBytes("devs", devs, sizeof(DevRec) * devN);
  memset(prefA, 0, sizeof(prefA));
  if (prefs.isKey("prefa")) prefs.getBytes("prefa", prefA, sizeof(prefA));
}
static int devFind(const uint8_t* a) { for (int i = 0; i < devN; i++) if (!memcmp(devs[i].a, a, 6)) return i; return -1; }
static int devSeen(const uint8_t* a) {
  int i = devFind(a);
  if (i >= 0) return i;
  if (devN == DEV_N) { for (int k = 0; k < DEV_N - 1; k++) devs[k] = devs[k + 1]; devN--; }   // forget the oldest
  i = devN++;
  memcpy(devs[i].a, a, 6);
  snprintf(devs[i].label, sizeof(devs[i].label), "Device %02X%02X", a[1], a[0]);
  devSave();
  return i;
}
static void devLabel(const uint8_t* a, const char* label, bool onlyIfDefault) {
  int i = devSeen(a);
  if (onlyIfDefault && strncmp(devs[i].label, "Device ", 7)) return;
  snprintf(devs[i].label, sizeof(devs[i].label), "%.17s", label);
  devSave();
}
static int prefRank(const uint8_t* a) {        // 1 primary, 2 second, 0 neither
  static const uint8_t zero[6] = { 0 };
  for (int k = 0; k < 2; k++) if (memcmp(prefA[k], zero, 6) && !memcmp(prefA[k], a, 6)) return k + 1;
  return 0;
}
static const char* prefName(int k) {
  static const uint8_t zero[6] = { 0 };
  if (!memcmp(prefA[k], zero, 6)) return "any";
  int i = devFind(prefA[k]);
  return i < 0 ? "?" : devs[i].label;
}
static bool anyLinked() {
  for (int i = 0; i < linkN; i++) if (links[i].authed) return true;
  return false;
}

// Loop's half: who stays, and who is the phone.
static void linkTick() {
  while (authTail != authHead) {
    uint16_t h = authQ[authTail]; authTail = (authTail + 1) % 4;
    int i = linkFind(h);
    if (i < 0) continue;
    btEverLinked = true;
    devSeen(links[i].a);
    int rank = prefRank(links[i].a);
    NimBLEServer* sv = NimBLEDevice::getServer();
    // More than two: the one to let go is a device nobody preferred,
    // the newcomer itself if it is that, or else the other one.
    if (linkN > 2 && sv) {
      int victim = -1;
      if (rank == 0) victim = i;
      else for (int k = 0; k < linkN; k++) if (k != i && links[k].authed && prefRank(links[k].a) == 0) { victim = k; break; }
      if (victim < 0) victim = i;
      Serial.printf("links: three, letting %04X go\n", links[victim].h);
      sv->disconnect(links[victim].h);
      if (victim == i) continue;
    }
    // The preferred phone always becomes the phone.
    if (rank == 1 && h != btConn) {
      uint16_t old = btConn;
      phoneReset(h);
      btConn2 = old;
      btSince = millis();
      btSet(BT_BONDED);
      Serial.println("links: the primary device is the phone now");
    }
    if (!cfgMulti && linkN > 1 && sv && h != btConn) sv->disconnect(h);   // one at a time when Multi-link is off
  }
}

''' + s[m.end():]
rep("static void syncBegin();\n", "static void syncBegin();\nstatic void linkTick();\nstatic void devLoad();\nstatic bool anyLinked();\nstatic void devLabel(const uint8_t* a, const char* label, bool onlyIfDefault);\nstatic const char* prefName(int k);\nstatic void ringInit();\nstatic void ringPx(int cx, int cy, int r, int k);\nstatic void fmtDur(long s, char* b, size_t n);\nstatic void fmtClock(time_t t, char* b, size_t n);\n")

# iPhones name themselves by having Apple's services
rep("""      ancsState = ANCS_READY;
      Serial.println("ancs: ready");""", """      ancsState = ANCS_READY;
      Serial.println("ancs: ready");
      { int li = linkFind(btConn); if (li >= 0 && links[li].authed) devLabel(links[li].a, "iPhone", true); }""")

# the app says who it is: "iam Android Pixel 7"
rep("struct AppMsg { uint8_t kind; uint16_t len; char data[420]; };",
    "struct AppMsg { uint8_t kind; uint16_t len; uint16_t conn; char data[420]; };")
rep("""    m.len = (uint16_t)n; m.kind = kind;""", """    m.len = (uint16_t)n; m.kind = kind; m.conn = ci.getConnHandle();""")
rep("""      if (!strcmp(m.data, "ping")) { }             // the app saying hello""",
    """      if (!strcmp(m.data, "ping")) { }             // the app saying hello
      else if (!strncmp(m.data, "iam ", 4)) {      // the app saying who it is
        int li = linkFind(m.conn);
        if (li >= 0 && links[li].authed) devLabel(links[li].a, m.data + 4, false);
        appRxCmd--;                                 // not a command anyone sent
      }""")

# =================================================================
# SETTINGS: a Devices group
# =================================================================
rep("""       C_GUARD, C_TAMPER, C_TLOG, C_HOLD, C_CLOCK, C_WAKEBY, C_COUNT };""",
    """       C_GUARD, C_TAMPER, C_TLOG, C_HOLD, C_CLOCK, C_WAKEBY,
       C_MULTI, C_DEV1, C_DEV2, C_AUTOAWAY, C_COUNT };""")
rep("""    "Phone guard", "Tamper alarm", "Tamper log", "Hold time", "Clock", "Wake by" };""",
    """    "Phone guard", "Tamper alarm", "Tamper log", "Hold time", "Clock", "Wake by",
    "Multi-link", "Primary", "Second", "Auto away" };""")
rep("""enum { SG_DISPLAY = 0, SG_WIRELESS, SG_CONTROLS, SG_SYSTEM, SG_COUNT };
const char* SG_NAME[SG_COUNT] = { "Display", "Wireless", "Controls", "System" };""",
    """enum { SG_DISPLAY = 0, SG_WIRELESS, SG_DEVICES, SG_CONTROLS, SG_SYSTEM, SG_COUNT };
const char* SG_NAME[SG_COUNT] = { "Display", "Wireless", "Devices", "Controls", "System" };""")
rep("""  { C_MODE,   C_HOTSPOT, C_PAIR,  C_PRAYER, C_UPDATE, C_AUTOUP, C_GUARD, SG_END },
""", """  { C_MODE,   C_HOTSPOT, C_PAIR,  C_PRAYER, C_UPDATE, C_AUTOUP, C_GUARD, SG_END },
  { C_MULTI,  C_DEV1,    C_DEV2,  C_AUTOAWAY, SG_END, SG_END,  SG_END,  SG_END },
""")
rep("""  // The four groups. They all fit on one screen, which is the point.
  if (depth == 1 && setGrp < 0) {
    bar("SETTINGS");
    for (int g = 0; g < SG_COUNT; g++) {
      int y = 14 + g * 12;""", """  // The groups. Four fit on the screen; the list moves to keep the
  // chosen one in view.
  if (depth == 1 && setGrp < 0) {
    bar("SETTINGS");
    int first = grpSel > 3 ? grpSel - 3 : 0;
    for (int g = first; g < SG_COUNT && g < first + 4; g++) {
      int y = 14 + (g - first) * 12;""")
rep("""      case C_WAKEBY: snprintf(v, sizeof(v), "%s", WAKEBY_NAME[cfgWakeBy]); break;""",
    """      case C_WAKEBY: snprintf(v, sizeof(v), "%s", WAKEBY_NAME[cfgWakeBy]); break;
      case C_MULTI:  snprintf(v, sizeof(v), "%s", cfgMulti ? "on" : "off"); break;
      case C_DEV1:   snprintf(v, sizeof(v), "%.12s", prefName(0)); break;
      case C_DEV2:   snprintf(v, sizeof(v), "%.12s", prefName(1)); break;
      case C_AUTOAWAY: snprintf(v, sizeof(v), "%s", cfgAutoAway ? "on" : "off"); break;""")
rep("""      case C_WAKEBY:
        cfgWakeBy = (cfgWakeBy + 1) % 3; prefs.putInt("wakeby", cfgWakeBy);
        break;""", """      case C_WAKEBY:
        cfgWakeBy = (cfgWakeBy + 1) % 3; prefs.putInt("wakeby", cfgWakeBy);
        break;
      case C_MULTI:
        cfgMulti = !cfgMulti; prefs.putBool("multi", cfgMulti);
        if (cfgMulti && btUp) NimBLEDevice::startAdvertising();
        else if (!cfgMulti && btConn2 != 0xFFFF && NimBLEDevice::getServer())
          NimBLEDevice::getServer()->disconnect(btConn2);
        flash(cfgMulti ? "TWO AT ONCE" : "ONE AT A TIME", 1100);
        break;
      case C_DEV1: case C_DEV2: {
        // Walk the remembered devices, then "any". The other slot's
        // device is skipped, so one device cannot be both.
        int k = (itemIdx == C_DEV1) ? 0 : 1;
        static const uint8_t zero[6] = { 0 };
        int cur = memcmp(prefA[k], zero, 6) ? devFind(prefA[k]) : -1;
        for (int step = 0; step <= devN; step++) {
          cur++;
          if (cur >= devN) { memset(prefA[k], 0, 6); break; }
          if (memcmp(devs[cur].a, prefA[1 - k], 6)) { memcpy(prefA[k], devs[cur].a, 6); break; }
        }
        devSave();
        break;
      }
      case C_AUTOAWAY:
        cfgAutoAway = !cfgAutoAway; prefs.putBool("autoaway", cfgAutoAway);
        break;""")
rep("""  cfg12h     = prefs.getBool("h12", false);
  cfgWakeBy  = constrain(prefs.getInt("wakeby", 0), 0, 2);""", """  cfg12h     = prefs.getBool("h12", false);
  cfgWakeBy  = constrain(prefs.getInt("wakeby", 0), 0, 2);
  cfgMulti    = prefs.getBool("multi", false);
  cfgAutoAway = prefs.getBool("autoaway", true);
  awayAuto    = prefs.getBool("awaya", false);
  devLoad();""")

# =================================================================
# LOOP: links, and automatic Away
# =================================================================
rep("""  appTick();                           // 7.2: what the Rafiq app sent""",
    """  appTick();                           // 7.2: what the Rafiq app sent
  linkTick();                          // 7.3: who stays connected, and who is the phone
  // 7.3: the phone gone two minutes, nothing else connected: Away, by
  // itself. It ends by itself too, when anything Rafiq knows comes back.
  if (cfgAutoAway && !awayOn && cfgNet == NET_BT && btUp && btEverLinked && !tmrOn &&
      linkN == 0 && btDropAt && (int32_t)(millis() - btDropAt) > (int32_t)AUTO_AWAY_MS) {
    tlogAdd("Auto away: phone gone");
    awaySet(true, nullptr);
    awayAuto = true; prefs.putBool("awaya", true);
    awayListenUntil = millis() + 4000;   // the message once, then off
    awayShowMs = 3000;
  }
  if (awayOn && awayAuto && anyLinked()) {
    tlogAdd("Auto away: phone back");
    awaySet(false, nullptr);
  }""")

# manual away is never automatic; home and the phone end both
rep("""  if (!strcmp(first, "away")) {
    if (!fresh) return;""", """  if (!strcmp(first, "away")) {
    if (!fresh) return;
    awayAuto = false; prefs.putBool("awaya", false);   // sent by hand: only home ends it""")
rep("""  } else {
    awayFlush(true);
    tlogAdd("Away off");""", """  } else {
    awayFlush(true);
    tlogAdd("Away off");
    if (awayAuto) { awayAuto = false; prefs.putBool("awaya", false); }""")

# Away's sleep: when automatic, a timer to look for the phone
rep("""  } else {
    rtcAwayDeep = 1;
    if (touchWakes())""", """  } else {
    rtcAwayDeep = 1;
    rtcAwayCheck = 0;
    if (awayAuto) {
      // Every 3 minutes for the first hour, every 5 after: a short look
      // for the phone, the screen dark, then off again if it is not there.
      uint32_t tnow = (uint32_t)time(nullptr);
      if (!rtcAwaySince) rtcAwaySince = tnow;
      uint32_t chk = (tnow - rtcAwaySince < 3600) ? 180 : 300;
      esp_sleep_enable_timer_wakeup((uint64_t)chk * 1000000ULL);
      rtcAwayCheck = 1;
    }
    if (touchWakes())""")
rep("""static void awayDeepGo() {
  awayFlush(true);
  tlogAdd("Away asleep");""", """static void awayDeepGo() {
  awayFlush(true);
  if (!awayQuietDeep) tlogAdd("Away asleep");
  awayQuietDeep = false;""")
rep("""  awayText   = prefs.getString("awayt", "Away");
  cfgBright  = prefs.getInt("bri", cfgBright);""", """  awayText   = prefs.getString("awayt", "Away");
  cfgBright  = prefs.getInt("bri", cfgBright);
  awayAuto   = prefs.getBool("awaya", false);""")

# setup: a check wake from Away goes through the full start, dark
rep("""  if (rtcAwayDeep && (woke_ == ESP_SLEEP_WAKEUP_GPIO || woke_ == ESP_SLEEP_WAKEUP_TIMER))
    awayDeepWake(woke_ == ESP_SLEEP_WAKEUP_TIMER);""", """  if (rtcAwayDeep && woke_ == ESP_SLEEP_WAKEUP_TIMER && rtcAwayCheck) awayCheckBoot = true;
  else if (rtcAwayDeep && (woke_ == ESP_SLEEP_WAKEUP_GPIO || woke_ == ESP_SLEEP_WAKEUP_TIMER))
    awayDeepWake(woke_ == ESP_SLEEP_WAKEUP_TIMER);
  rtcAwayCheck = 0;""")
rep("""    if (wokeForAlarm && rtcWakeCheck) {          // only looking for the phone
      wokeForAlarm = false; wokeBy = "check";
      checkWake = true;
    }""", """    if ((wokeForAlarm && rtcWakeCheck) || awayCheckBoot) {   // only looking for the phone
      wokeForAlarm = false; wokeBy = "check";
      checkWake = true;
    }""")
rep("""  if (awayOn) {
    // Switched off and on: eyes, the message for three seconds, then""",
    """  if (awayOn && !checkWake) {
    // Switched off and on: eyes, the message for three seconds, then""")
rep("""    else if (now > checkUntil) { checkWake = false; deepAuto = true; goDeep(); }""",
    """    else if (now > checkUntil) {
      checkWake = false;
      if (awayOn) { awayQuietDeep = true; awayDeepGo(); }   // still away: back to Away's sleep
      else        { deepAuto = true; goDeep(); }
    }""")
rep("""    if (phoneHeld()) { checkWake = false; Serial.println("check: phone is back, staying linked"); }""",
    """    if (phoneHeld() || anyLinked()) { checkWake = false; Serial.println("check: phone is back, staying linked"); }""")

# =================================================================
# SCREENS: the timer (a ring) and the contact card
# =================================================================
rep("static void syncBegin() {", r"""// A ring, worked out once: 180 points, 2 degrees apart, cos and sin
// times 1024. The C3 has no floating point unit, so nothing per frame.
static int16_t ringC[180], ringS[180];
static bool ringReady = false;
static void ringInit() {
  if (ringReady) return;
  for (int k = 0; k < 180; k++) {
    float t = (k * 2 - 90) * 3.14159265f / 180.0f;
    ringC[k] = (int16_t)lroundf(cosf(t) * 1024.0f);
    ringS[k] = (int16_t)lroundf(sinf(t) * 1024.0f);
  }
  ringReady = true;
}
static void ringPx(int cx, int cy, int r, int k) {
  oled.drawPixel(cx + (r * ringC[k] + (ringC[k] >= 0 ? 512 : -512)) / 1024,
                 cy + (r * ringS[k] + (ringS[k] >= 0 ? 512 : -512)) / 1024, SSD1306_WHITE);
}
static void fmtDur(long s, char* b, size_t n) {
  if (s >= 3600)     snprintf(b, n, "%ldh %02ldm", s / 3600, (s / 60) % 60);
  else if (s >= 60)  snprintf(b, n, "%ldm %02lds", s / 60, s % 60);
  else               snprintf(b, n, "%lds", s);
}
static void fmtClock(time_t t, char* b, size_t n) {
  struct tm lt; localtime_r(&t, &lt);
  if (cfg12h) snprintf(b, n, "%d:%02d %s", lt.tm_hour % 12 ? lt.tm_hour % 12 : 12, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM");
  else        snprintf(b, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

static void syncBegin() {""")

m = re.search(r"  long left = tmrLeft\(\);\n  char t\[12\];.*?  oled\.display\(\);\n\}\n", s, re.S)
if not m: sys.exit("timer body not found")
s = s[:m.start()] + r"""  // 7.3: a ring that fills as the time goes, the time inside it, and
  // what is left and when it ends beside it. Small type, laid out.
  ringInit();
  long left = tmrLeft();
  long total = (long)((tmrEnd - tmrStart) / 1000);
  char b[16];
  at(0, 1, "TIMER");
  if (tmrPinned) oled.fillCircle(36, 4, 2, SSD1306_WHITE);   // held on by a touch
  if (total >= 3600) snprintf(b, sizeof(b), "%ldh %02ldm", total / 3600, (total / 60) % 60);
  else               snprintf(b, sizeof(b), "%ld min", (total + 59) / 60);
  at(SCRW - (int)strlen(b) * 6, 1, b);
  oled.drawFastHLine(0, 11, SCRW, SSD1306_WHITE);
  const int cx = 32, cy = 38, r = 23;
  for (int k = 0; k < 180; k += 4) ringPx(cx, cy, r, k);          // the track, dotted
  int upto = total > 0 ? (int)(180L * (total - left) / total) : 180;
  for (int k = 0; k < upto; k++) { ringPx(cx, cy, r, k); ringPx(cx, cy, r - 1, k); ringPx(cx, cy, r - 2, k); }
  if (left >= 3600) snprintf(b, sizeof(b), "%ldh%02ld", left / 3600, (left / 60) % 60);
  else              snprintf(b, sizeof(b), "%02ld:%02ld", left / 60, left % 60);
  at(cx - (int)strlen(b) * 3, cy - 3, b);
  at(66, 22, "left");
  fmtDur(left, b, sizeof(b));
  at(66, 32, b);
  if (timeOk) {
    at(66, 46, "ends");
    fmtClock(time(nullptr) + left, b, sizeof(b));
    at(66, 56, b);
  }
  oled.display();
}
""" + s[m.end():]

replace_func("drawContact", r"""// Small pictures, seven by seven: a person, a handset, an envelope.
static const uint8_t ICO_PERSON[7] = { 0x1C, 0x3E, 0x3E, 0x1C, 0x00, 0x7F, 0x7F };
static const uint8_t ICO_PHONE[7]  = { 0x60, 0x70, 0x30, 0x18, 0x0D, 0x07, 0x03 };
static const uint8_t ICO_MAIL[7]   = { 0x7F, 0x63, 0x55, 0x49, 0x41, 0x7F, 0x00 };
static void icon7(int x, int y, const uint8_t* rows) {
  for (int j = 0; j < 7; j++)
    for (int i = 0; i < 7; i++)
      if (rows[j] & (0x40 >> i)) oled.drawPixel(x + i, y + j, SSD1306_WHITE);
}
// Who to get in touch with: the second half of every showing. A small
// heading and a line, then one thing to a row with its picture.
static void drawContact() {
  oled.clearDisplay();
  ctr("GET IN TOUCH", 1, 1);
  oled.drawFastHLine(0, 11, SCRW, SSD1306_WHITE);
  icon7(4, 17, ICO_PERSON); at(16, 17, OWNER_NAME);
  icon7(4, 29, ICO_PHONE);  at(16, 29, OWNER_PHONE_SHOW);
  icon7(4, 41, ICO_MAIL);
  const char* m = OWNER_MAIL;
  const char* atp = strchr(m, '@');
  if (atp) {
    char user[22]; snprintf(user, sizeof(user), "%.*s", (int)(atp - m), m);
    at(16, 41, user);
    at(16, 51, atp);
  } else at(16, 41, m);
  oled.display();
}""")
rep("""#define OWNER_PHONE "+918660027729\"""", """#define OWNER_PHONE "+918660027729"
#define OWNER_PHONE_SHOW "+91 86600 27729\"""")

open(SRC, 'w').write(s)
print("7.3 ok")
s = open(SRC).read()
def rep2(old, new):
    global s
    if s.count(old) != 1: sys.exit("7.3b anchor:\n" + old[:200])
    s = s.replace(old, new)
# only the phone's own link says "this phone runs the app"
rep2("""    btApp = true;                      // only the app writes here""",
     """    if (ci.getConnHandle() == btConn) btApp = true;   // the phone runs the app (a companion's writes do not count)""")
# the door: advertising while full only when a preferred device is missing
rep2("""    if (cfgMulti && linkN < 3) NimBLEDevice::startAdvertising();""",
     """    if (cfgMulti && linkN < 2) NimBLEDevice::startAdvertising();   // a second is welcome; more is loop's call""")
rep2("bool     btEverLinked = false;", """bool     btEverLinked = false;
uint32_t doorPauseUntil = 0;           // after turning a device away, a minute closed""")
rep2("""      Serial.printf("links: three, letting %04X go\\n", links[victim].h);
      sv->disconnect(links[victim].h);
      if (victim == i) continue;""", """      Serial.printf("links: three, letting %04X go\\n", links[victim].h);
      sv->disconnect(links[victim].h);
      if (victim == i) {
        // Turned away: keep the door shut a minute, or a phone that
        // reconnects by itself would be in and out all day.
        doorPauseUntil = millis() + 60000;
        NimBLEDevice::stopAdvertising();
        continue;
      }""")
rep2("""    if (!cfgMulti && linkN > 1 && sv && h != btConn) sv->disconnect(h);   // one at a time when Multi-link is off
  }
}""", """    if (!cfgMulti && linkN > 1 && sv && h != btConn) sv->disconnect(h);   // one at a time when Multi-link is off
  }
  // Two connected: the door stays open only while a preferred device is
  // missing and one of the two is not preferred, so it has a place.
  if (cfgMulti && btUp && linkN == 2) {
    static const uint8_t zero[6] = { 0 };
    bool missing = false, room = false;
    for (int k = 0; k < 2; k++) {
      if (!memcmp(prefA[k], zero, 6)) continue;
      bool here = false;
      for (int j = 0; j < linkN; j++) if (links[j].authed && !memcmp(links[j].a, prefA[k], 6)) here = true;
      if (!here) missing = true;
    }
    for (int j = 0; j < linkN; j++) if (!links[j].authed || prefRank(links[j].a) == 0) room = true;
    bool want = missing && room && (int32_t)(millis() - doorPauseUntil) > 0;
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (want && !adv->isAdvertising()) NimBLEDevice::startAdvertising();
    if (!want && adv->isAdvertising()) NimBLEDevice::stopAdvertising();
  }
}""")
open(SRC, 'w').write(s)
print("7.3b ok")

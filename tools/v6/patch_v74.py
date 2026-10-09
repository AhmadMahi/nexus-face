#!/usr/bin/env python3
# Rafiq 7.4.0: everything the Mac app does, over Bluetooth; gestures and
# follow-the-pointer channels; WiFi never on by itself; quieter radio;
# long notifications that scroll without overlapping. After patch_v731.
import re, sys
SRC = sys.argv[1]
s = open(SRC).read()
def rep(a, b, c=1):
    global s
    n = s.count(a)
    if n != c: sys.exit(f"anchor {n}x:\n{a[:220]}")
    s = s.replace(a, b)

rep('#define FW_VERSION "7.3.1"', '#define FW_VERSION "7.4.0"')

# =================================================================
# 1. WIFI NEVER COMES ON BY ITSELF
# =================================================================
#  Switching it off and on quickly counted as "Bluetooth did not
#  start", and three of those fell back to WiFi and a scan. Only a
#  crash counts now, and even then it stays on Bluetooth.
rep("""  if (cfgNet == NET_BT && prefs.getInt("btry2", 0) >= BT_GIVE_UP) {
    prefs.putInt("btry2", 0);
    btFellBack = true;
    // For this start only. Home stays Bluetooth, and the next restart
    // tries it again, by which time an update may have fixed it.
    if (netCount) { cfgNet = NET_WIFI; wsKind = WS_MANUAL; wsStartMs = wsLastUse = millis(); }
    else          { cfgNet = NET_OFF; needHotspot = true; }
    Serial.printf("bluetooth failed %d starts running: %s for now\\n", BT_GIVE_UP,
                  netCount ? "wifi" : "the hotspot");
    flash("BLUETOOTH FAILED", 1400);
    flash(netCount ? "ON WIFI FOR NOW" : "HOTSPOT FOR NOW", 1200);
  }""", """  // 7.4: only a crash is a failed start. Power off and on, the reset
  // button, a restart or a deep-sleep wake are not, and used to add up
  // until WiFi came on by itself. And a real failure no longer turns
  // WiFi on: WiFi is only ever on because it was asked for.
  {
    esp_reset_reason_t r = esp_reset_reason();
    bool crash = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT);
    if (!crash) prefs.putInt("btry2", 0);
  }
  if (cfgNet == NET_BT && prefs.getInt("btry2", 0) >= BT_GIVE_UP) {
    prefs.putInt("btry2", 0);
    btFellBack = true;
    Serial.printf("bluetooth crashed %d starts running: trying again, no WiFi\\n", BT_GIVE_UP);
    flash("BLUETOOTH RESTARTED", 1400);
  }""")
rep("#define WS_IDLE_S          1800UL      // thirty minutes",
    "#define WS_IDLE_S           600UL      // ten minutes unused, then off (7.4)")

# =================================================================
# 2. RADIO: silent when full, slow when waiting, light on the link
# =================================================================
rep("""  // Two connected: the door stays open only while a preferred device is
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
  }""", """  // 7.4: two in, nothing goes out. Not even listening for a third.
  if (btUp && linkN >= 2) {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (adv->isAdvertising()) NimBLEDevice::stopAdvertising();
  }
  // Fast for half a minute after a link goes (so it comes straight
  // back), then slow: a phone that is away does not need hearing ten
  // times a second, and the battery does.
  if (btUp && !advSlow && (int32_t)(millis() - advFastUntil) > 0) {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    bool was = adv->isAdvertising();
    if (was) NimBLEDevice::stopAdvertising();
    adv->setMinInterval(874); adv->setMaxInterval(874);   // 546.25 ms
    advSlow = true;
    if (was) NimBLEDevice::startAdvertising();
  }""")
rep("uint32_t doorPauseUntil = 0;", """uint32_t doorPauseUntil = 0;
volatile uint32_t advFastUntil = 30000;     // fast for the first half minute after start
volatile bool     advSlow = false;
static void advFast() {                     // a link just went: listen quickly for a while
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setMinInterval(244); adv->setMaxInterval(244);    // 152.5 ms
  advSlow = false;
  advFastUntil = millis() + 30000;
}""")
rep("""    // Straight back to advertising, or the phone has nothing to come
    // back to and you would be pairing it by hand every time.
    NimBLEDevice::startAdvertising();""", """    // Straight back to advertising, or the phone has nothing to come
    // back to and you would be pairing it by hand every time.
    if (linkN < (cfgMulti ? 2 : 1)) { advFast(); NimBLEDevice::startAdvertising(); }
    // A companion gone takes gesture mode with it.
    if (wasComp) cfgGesture = false;""")
rep("""  void onDisconnect(NimBLEServer* sv, NimBLEConnInfo& ci, int reason) override {
    uint16_t h = ci.getConnHandle();
    linkDrop(h);""", """  void onDisconnect(NimBLEServer* sv, NimBLEConnInfo& ci, int reason) override {
    uint16_t h = ci.getConnHandle();
    bool wasComp = (h == btConn2);       // 7.4: the companion, not the phone
    linkDrop(h);""")
# each link asks for a light rhythm once it is trusted
rep("""    uint8_t nx = (authHead + 1) % 4;
    if (nx != authTail) { authQ[authHead] = h; authHead = nx; }""", """    uint8_t nx = (authHead + 1) % 4;
    if (nx != authTail) { authQ[authHead] = h; authHead = nx; }
    // 90 to 120 ms, may skip 4, 6 s to give up: Apple's own guidance,
    // and most of the radio's time asleep. Following the pointer asks
    // for a quicker one while it runs.
    NimBLEDevice::getServer()->updateConnParams(h, 72, 96, 4, 600);""")
rep("""  adv->setAdvertisementData(ad);
  adv->setScanResponseData(sr);""", """  adv->setAdvertisementData(ad);
  adv->setScanResponseData(sr);
  advFast();""")

# =================================================================
# 3. THE NEW CHANNELS: events out, the pointer in, settings read
# =================================================================
rep('#define RQ_STAT "52a1f004-7a3e-4b5c-9d6f-0a1b2c3d4e5f"', """#define RQ_STAT "52a1f004-7a3e-4b5c-9d6f-0a1b2c3d4e5f"
//  7.4:
//    EVT   notify  gestures, as they happen ("1", "2", "hold" ...)
//    PTR   write without response  "x y", -1000..1000, the pointer
//    CFG   read    the settings, as JSON with /api/state's own names
#define RQ_EVT  "52a1f005-7a3e-4b5c-9d6f-0a1b2c3d4e5f"
#define RQ_PTR  "52a1f006-7a3e-4b5c-9d6f-0a1b2c3d4e5f"
#define RQ_CFG  "52a1f007-7a3e-4b5c-9d6f-0a1b2c3d4e5f"
NimBLECharacteristic* rqEvt = nullptr;
extern bool cfgGesture, cfgFollow;     // both declared with gestures, further down""")
rep("""  svc->createCharacteristic(RQ_STAT, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC)
     ->setCallbacks(new RqChrCb(4));
}""", """  svc->createCharacteristic(RQ_STAT, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC)
     ->setCallbacks(new RqChrCb(4));
  rqEvt = svc->createCharacteristic(RQ_EVT, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY);
  svc->createCharacteristic(RQ_PTR, NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC)
     ->setCallbacks(new RqChrCb(5));
  svc->createCharacteristic(RQ_CFG, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC)
     ->setCallbacks(new RqChrCb(6));
}""")
# the pointer is handled at once, on the NimBLE task: a few floats
rep("""  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
    if (ci.getConnHandle() == btConn) btApp = true;   // the phone runs the app (a companion's writes do not count)""",
    """  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
    if (kind == 5) {                     // the pointer: no queue, ten a second
      NimBLEAttValue v = c->getValue();
      char b[24]; size_t n = v.length() < sizeof(b) - 1 ? v.length() : sizeof(b) - 1;
      memcpy(b, v.data(), n); b[n] = 0;
      cursorFeed(b);
      return;
    }
    if (ci.getConnHandle() == btConn) btApp = true;   // the phone runs the app (a companion's writes do not count)""")
rep("""  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
    char b[220];""", """  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
    if (kind == 6) { String j = cfgJson(); c->setValue((const uint8_t*)j.c_str(), j.length()); return; }
    char b[240];""")
rep("""             "fw=%s;bat=%d;away=%d;timer=%ld;unread=%d;quiet=%d;guard=%d;wake=%d;h12=%d;clock=%d;rxc=%u;rxn=%u;last=%s",""",
    """             "fw=%s;bat=%d;away=%d;timer=%ld;unread=%d;quiet=%d;guard=%d;wake=%d;h12=%d;clock=%d;rxc=%u;rxn=%u;relax=%d;follow=%d;gest=%d;last=%s",""")
rep("""             timeOk ? 1 : 0, (unsigned)appRxCmd, (unsigned)appRxNote, appLast);""",
    """             timeOk ? 1 : 0, (unsigned)appRxCmd, (unsigned)appRxNote,
             relaxOn ? 1 : 0, cfgFollow ? 1 : 0, cfgGesture ? 1 : 0, appLast);""")
rep("static void appTick();\n", "static void appTick();\nstatic void cursorFeed(const char* b);\nstatic String cfgJson();\nstatic bool cfgApply(const String& k, int v);\n")

# gestures out over Bluetooth as well as WiFi
rep("""static void sendTap(const char* what) {
  if (!cfgGesture || !macLinked || !online() || macAddr == IPAddress()) return;""", """static void sendTap(const char* what) {
  if (!cfgGesture) return;
  // 7.4: over Bluetooth, to whoever is listening (the Mac app). The link
  // is bonded and encrypted, so no token travels with it.
  if (rqEvt && linkN) { rqEvt->setValue((const uint8_t*)what, strlen(what)); rqEvt->notify(); }
  if (!macLinked || !online() || macAddr == IPAddress()) return;""")
# the pointer parser, shared by UDP and Bluetooth
rep("""static void serviceCursor() {
  if (!cfgFollow) return;
  int n = cursorUdp.parsePacket();
  while (n > 0) {
    char b[32];
    int got = cursorUdp.read(b, sizeof(b) - 1);
    if (got > 0) {
      b[got] = 0;
      int x = 0, y = 0;
      if (sscanf(b, "%d %d", &x, &y) == 2) {
        curX = constrain(x / 1000.0f, -1.0f, 1.0f);
        curY = constrain(y / 1000.0f, -1.0f, 1.0f);
        curUntil = millis() + CURSOR_HOLD_MS;
      }
    }
    n = cursorUdp.parsePacket();
  }
}""", """// "x y", each -1000..1000. From UDP on WiFi, or Bluetooth (7.4).
static void cursorFeed(const char* b) {
  if (!cfgFollow) return;
  int x = 0, y = 0;
  if (sscanf(b, "%d %d", &x, &y) == 2) {
    curX = constrain(x / 1000.0f, -1.0f, 1.0f);
    curY = constrain(y / 1000.0f, -1.0f, 1.0f);
    curUntil = millis() + CURSOR_HOLD_MS;
  }
}
static void serviceCursor() {
  if (!cfgFollow || !online()) return;
  int n = cursorUdp.parsePacket();
  while (n > 0) {
    char b[32];
    int got = cursorUdp.read(b, sizeof(b) - 1);
    if (got > 0) { b[got] = 0; cursorFeed(b); }
    n = cursorUdp.parsePacket();
  }
}""")

# =================================================================
# 4. SETTINGS: one function for WiFi and Bluetooth, and a read-back
# =================================================================
start = s.index('  web.on("/api/cfgv", HTTP_POST, []() {')
head = '    int v = web.arg("v").toInt();\n'
c0 = s.index(head, start) + len(head)
c1 = s.index('    else { web.send(400, "application/json"', c0)
end = s.index('    okJson();\n  });\n', c1) + len('    okJson();\n  });\n')
chain = s[c0:c1]
s = s[:start] + """  web.on("/api/cfgv", HTTP_POST, []() {
    if (!guard()) return;
    if (!cfgApply(web.arg("k"), web.arg("v").toInt())) {
      web.send(400, "application/json", "{\\"ok\\":false,\\"err\\":\\"no such setting\\"}");
      return;
    }
    okJson();
  });
""" + s[end:]
CFGAPPLY = "// One setting, by the name the page and the apps use. WiFi and\n// Bluetooth both come here (7.4), so they can never disagree.\nstatic bool cfgApply(const String& k, int v) {\n" + chain + "    else return false;\n  return true;\n}\n"

rep("static void syncBegin() {", CFGAPPLY + r"""
// The settings the apps show, with /api/state's own names, so the apps
// read this exactly as they read that. Kept under 512 bytes (the most a
// Bluetooth value can be): networks go last and stop when it is full.
static String cfgJson() {
  String o;
  o.reserve(512);
  long dl = dndUntil && (long)(dndUntil - millis()) > 0 ? (long)(dndUntil - millis()) / 1000 : 0;
  o += "{\"fw\":\"" FW_VERSION "\",\"bri\":" + String(cfgBright) + ",\"face\":" + String(cfgFace) +
       ",\"slpi\":" + String(cfgSleepIdx) + ",\"popi\":" + String(cfgPopupIdx) +
       ",\"eye\":" + String(cfgEyes) + ",\"tap\":" + String(cfgTap) +
       ",\"deepi\":" + String(cfgDeepIdx) + ",\"btpl\":" + String(cfgBikeTpl) +
       ",\"knock\":" + String(cfgKnock ? "true" : "false") +
       ",\"shake\":" + String(cfgBack == BACK_BOTH ? "true" : "false") +
       ",\"bike\":" + String(cfgBike ? "true" : "false") +
       ",\"follow\":" + String(cfgFollow ? "true" : "false") +
       ",\"relax\":" + String(relaxOn ? "true" : "false") +
       ",\"gesture\":" + String(cfgGesture ? "true" : "false") +
       ",\"deepOff\":" + String(deepOff ? "true" : "false") +
       ",\"autoUp\":" + String(cfgAutoUp ? "true" : "false") +
       ",\"turn\":" + String(cfgAutoTurn ? "true" : "false") +
       ",\"offline\":" + String(cfgNetHome == NET_OFF ? "true" : "false") +
       ",\"intWired\":" + String(intWired ? "true" : "false") +
       ",\"dndLeft\":" + String(dl) +
       ",\"battPct\":" + String(isnan(battV) ? -1 : battPct(battV)) +
       ",\"battFull\":" + String(battFull, 2) +
       ",\"netMax\":" + String(NET_MAX) + ",\"nets\":[";
  for (int i = 0; i < netCount; i++) {
    String nm = String(netSsid[i]); nm.replace("\\", " "); nm.replace("\"", "'");
    String one = String(i ? "," : "") + "{\"ssid\":\"" + nm + "\",\"on\":false}";
    if (o.length() + one.length() + 3 > 510) break;
    o += one;
  }
  o += "]}";
  return o;
}

// "!" commands: only from the apps, never a Shortcut, because they reach
// settings, networks and the pointer. They do what the HTTP calls of the
// same names do.
static int remBatch = 0;
static uint32_t remBatchAt = 0, remBatchSoonest = 0;
static void appBang(char* c, uint16_t conn) {
  char* a = c + 1;                                 // after the "!"
  char* sp = strchr(a, ' ');
  String verb = sp ? String(a).substring(0, sp - a) : String(a);
  const char* rest = sp ? sp + 1 : "";
  if (verb == "cfg") {
    char k[12] = ""; int v = 0;
    if (sscanf(rest, "%11s %d", k, &v) == 2) cfgApply(String(k), v);
  } else if (verb == "relax") {
    relaxOn = atoi(rest) != 0; relaxUntil = 0;
    if (relaxOn) { relaxKind = 0; relaxNext = millis() + 30000UL; wake("relax"); }
  } else if (verb == "follow") {
    cfgFollow = atoi(rest) != 0;
    prefs.putBool("follow", cfgFollow);
    if (cfgFollow) wake("follow"); else curUntil = 0;
    // a quicker link while the eyes follow, back to the light one after
    NimBLEServer* sv = NimBLEDevice::getServer();
    if (sv) sv->updateConnParams(conn, cfgFollow ? 24 : 72, cfgFollow ? 40 : 96, cfgFollow ? 0 : 4, cfgFollow ? 400 : 600);
  } else if (verb == "dnd") {
    int mm = constrain(atoi(rest), 0, 480);
    dndUntil = mm ? millis() + (unsigned long)mm * 60000UL : 0;
    dndLine = (int)random(DND_N);
    if (mm) wake("break");
  } else if (verb == "busy") {
    int cm = 0, mi = 0, mu = -1;
    sscanf(rest, "%d %d %d", &cm, &mi, &mu);
    if ((cm || mi) && !(busyCam || busyMic)) { busyAt = millis(); wake("live"); }
    busyCam = cm; busyMic = mi;
    if (mu >= 0) gestMuted = mu != 0;
  } else if (verb == "tap") {
    cfgTap = constrain(atoi(rest), 0, TAP_N - 1); prefs.putInt("tap", cfgTap); applyTap();
  } else if (verb == "deep") {
    deepOff = atoi(rest) != 0; prefs.putBool("nodeep", deepOff);
  } else if (verb == "autoup") {
    cfgAutoUp = atoi(rest) != 0; prefs.putBool("autoup", cfgAutoUp);
  } else if (verb == "turn") {
    cfgAutoTurn = atoi(rest) != 0; prefs.putBool("turn", cfgAutoTurn);
  } else if (verb == "bike") {                     // plate \x1F make \x1F model \x1F owner
    char* f[4] = { (char*)rest, nullptr, nullptr, nullptr };
    int k = 1;
    for (char* p = (char*)rest; *p && k < 4; p++) if (*p == 0x1F) { *p = 0; f[k++] = p + 1; }
    struct { char* dst; size_t n; const char* pref; } D[] = {
      { bikePlate, sizeof(bikePlate), "bplate" }, { bikeMake, sizeof(bikeMake), "bmake" },
      { bikeModel, sizeof(bikeModel), "bmodel" }, { bikeOwner, sizeof(bikeOwner), "bowner" } };
    for (int i = 0; i < k && i < 4; i++) {
      if (!f[i] || !*f[i]) continue;
      snprintf(D[i].dst, D[i].n, "%s", f[i]);
      prefs.putString(D[i].pref, D[i].dst);
    }
  } else if (verb == "net") {                      // add ssid\x1Fpass | del i | up i
    if (!strncmp(rest, "add ", 4)) {
      String ss = String(rest + 4), pw = "";
      int sep = ss.indexOf((char)0x1F);
      if (sep >= 0) { pw = ss.substring(sep + 1); ss = ss.substring(0, sep); }
      ss.trim();
      if (ss.length()) {
        int at = -1;
        for (int i = 0; i < netCount; i++) if (ss == netSsid[i]) { at = i; break; }
        if (at < 0 && netCount < NET_MAX) at = netCount++;
        if (at >= 0) {
          strncpy(netSsid[at], ss.c_str(), 32); netSsid[at][32] = 0;
          strncpy(netPass[at], pw.c_str(), 64); netPass[at][64] = 0;
          saveNets(); netReload = true;
          flash("NETWORK SAVED", 1000);
        } else flash("NETWORKS FULL", 1200);
      }
    } else if (!strncmp(rest, "del ", 4)) {
      int d = atoi(rest + 4);
      if (d >= 0 && d < netCount) {
        for (int i = d; i < netCount - 1; i++) { strncpy(netSsid[i], netSsid[i + 1], 33); strncpy(netPass[i], netPass[i + 1], 65); }
        netCount--; netSsid[netCount][0] = netPass[netCount][0] = 0;
        saveNets(); netReload = true;
      }
    } else if (!strncmp(rest, "up ", 3)) {
      int u = atoi(rest + 3);
      if (u > 0 && u < netCount) {
        char ts[33], tp[65];
        strncpy(ts, netSsid[u], 33); strncpy(tp, netPass[u], 65);
        strncpy(netSsid[u], netSsid[u - 1], 33); strncpy(netPass[u], netPass[u - 1], 65);
        strncpy(netSsid[u - 1], ts, 33); strncpy(netPass[u - 1], tp, 65);
        saveNets(); netReload = true;
      }
    }
  } else if (verb == "remclear") {
    remCount = 0; remIdx = 0; saveRems();
  } else if (verb == "rem") {                      // wall-seconds done text
    // The app sends the time as a wall clock (local seconds). The robot's
    // own clock is either the phone's wall clock (Bluetooth) or real time
    // with a zone (WiFi); the difference between the two is the offset.
    unsigned long wall = 0; int done = 0, used = 0;
    if (sscanf(rest, "%lu %d %n", &wall, &done, &used) >= 2 && used > 0 && wall > 1700000000UL) {
      time_t now = time(nullptr); struct tm lt; localtime_r(&now, &lt);
      long off = (long)(utcFromTm(&lt) - now);
      uint32_t at = (uint32_t)((long)wall - off);
      String txt = String(rest + used); txt.trim();
      bool dup = false;
      for (int k = 0; k < remCount; k++)
        if (rems[k].at / 60 == at / 60 && txt == rems[k].text) { dup = true; break; }
      if (!dup && txt.length() && addRem(txt.c_str(), at)) {
        if (done) rems[remCount - 1].done = true;
        remBatch++; remBatchAt = millis();
        if (!done && (!remBatchSoonest || at < remBatchSoonest)) remBatchSoonest = at;
      }
    }
  }
}

static void syncBegin() {""")
rep("static void syncBegin();\n", "static void syncBegin();\nstatic void appBang(char* c, uint16_t conn);\n")
rep("""      if (!strcmp(m.data, "ping")) { }             // the app saying hello""",
    """      if (!strcmp(m.data, "ping")) { }             // the app saying hello
      else if (m.data[0] == '!') appBang(m.data, m.conn);   // 7.4: what the apps' HTTP calls did""")
# a batch of reminders is sorted, kept and announced once, after it ends
rep("""  linkTick();                          // 7.3: who stays connected, and who is the phone""",
    """  linkTick();                          // 7.3: who stays connected, and who is the phone
  if (remBatch && millis() - remBatchAt > 800) {
    sortRems(); saveRems();
    if (remIdx > remCount) remIdx = remCount;
    remAddedCard(remBatch, remBatchSoonest);
    remBatch = 0; remBatchSoonest = 0;
  }""")

# =================================================================
# 5. LONG NOTIFICATIONS: the text scrolls under the header, not over it
# =================================================================
rep("""  noteHeader(n, when);
  {
    char app[22]; snprintf(app, sizeof(app), "%s", appShort(n));
    ctr(app, 14, 1);
  }
  if (noteIsCall(n)) marquee(n.title[0] ? n.title : "unknown", 30, 1);
  else               fitText(n.msg[0] ? n.msg : "(no text)", 24, 50, n.at);""",
"""  // 7.4: the text first, then everything that frames it drawn over a
  // cleared strip. fitText scrolls long text by moving it up past its
  // top line, and drawn last it ran over the header and the buttons.
  if (noteIsCall(n)) marquee(n.title[0] ? n.title : "unknown", 30, 1);
  else               fitText(n.msg[0] ? n.msg : "(no text)", 24, 50, n.at);
  oled.fillRect(0, 0, SCRW, 23, SSD1306_BLACK);
  oled.fillRect(0, 51, SCRW, 13, SSD1306_BLACK);
  noteHeader(n, when);
  {
    char app[22]; snprintf(app, sizeof(app), "%s", appShort(n));
    ctr(app, 14, 1);
  }""")
rep("""  if (noteIsCall(n)) marquee(n.title[0] ? n.title : "unknown", 26, 1);
  else               fitText(n.msg[0] ? n.msg : "(no text)", 14, 50, n.at);
  twoButtons("tap:close", "hold:open");""", """  if (noteIsCall(n)) marquee(n.title[0] ? n.title : "unknown", 26, 1);
  else               fitText(n.msg[0] ? n.msg : "(no text)", 14, 50, n.at);
  oled.fillRect(0, 0, SCRW, 13, SSD1306_BLACK);      // the frame over the text
  oled.fillRect(0, 51, SCRW, 13, SSD1306_BLACK);
  noteHeader(n, app);
  twoButtons("tap:close", "hold:open");""")
rep("""  char app[12]; snprintf(app, sizeof(app), "%.8s", appShort(n));
  noteHeader(n, app);
  if (noteIsCall(n))""", """  char app[12]; snprintf(app, sizeof(app), "%.8s", appShort(n));
  if (noteIsCall(n))""")

open(SRC, 'w').write(s)
print("7.4 ok")

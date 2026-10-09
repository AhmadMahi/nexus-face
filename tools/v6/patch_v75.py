#!/usr/bin/env python3
# Rafiq 7.5.0: the Mac batch. Tagged gestures (pad, knock, lean, the
# tilt knob), the Mac screen (pinned task, top three, Mac health), walk
# away, dim, prayer to the Mac, calls answered on the robot, app filter
# and VIPs, prayer times from the Mac, and updates over Bluetooth.
# After patch_v741.
import sys
SRC = sys.argv[1]
s = open(SRC).read()
def rep(a, b, c=1):
    global s
    n = s.count(a)
    if n != c: sys.exit(f"anchor {n}x:\n{a[:220]}")
    s = s.replace(a, b)

rep('#define FW_VERSION "7.4.1"', '#define FW_VERSION "7.5.0"')

# =================================================================
# GLOBALS
# =================================================================
rep("NimBLECharacteristic* rqEvt = nullptr;", """NimBLECharacteristic* rqEvt = nullptr;
//  7.5:
//    LST  read   {"apps":[seen],"muted":[...],"vip":[...]}
//    OTA  write without response: firmware bytes, after "!ota begin"
#define RQ_LST  "52a1f008-7a3e-4b5c-9d6f-0a1b2c3d4e5f"
#define RQ_OTA  "52a1f009-7a3e-4b5c-9d6f-0a1b2c3d4e5f"
// ---- 7.5: the Mac's cards: 0 health, 1 top three, 2 pinned task ----
struct Card { bool on; char title[22]; char line[3][24]; int8_t bar; };
Card cards[3];
int  macSel = 0;
bool knobOn = false, knobUsed = false, walkOn = false, macBye = false, macDim = false;
float knobRef = 0; int knobLast = 0; uint32_t knobAt = 0;
int8_t leanState = 0;                  // -1 left, 0 middle, 1 right (gesture mode)
uint32_t walkUntil = 0;
// ---- app filter and VIPs, kept on the robot so every phone obeys it ----
#define APPS_N 12
#define MUTE_N 12
#define VIP_N  8
char seenApps[APPS_N][20]; int seenN = 0;
char mutedApps[MUTE_N][20]; int mutedN = 0;
char vipWords[VIP_N][20];  int vipN = 0;
// ---- updates over Bluetooth ----
volatile bool otaOn = false, otaErr = false;
volatile uint32_t otaSize = 0, otaGot = 0, otaLastAt = 0;
uint16_t otaConn = 0xFFFF;
int otaPctSent = -1;""")

# =================================================================
# EVENTS OUT: one sender for everything the apps hear
# =================================================================
rep("""static void sendTap(const char* what) {
  if (!cfgGesture) return;
  // 7.4: over Bluetooth, to whoever is listening (the Mac app). The link
  // is bonded and encrypted, so no token travels with it.
  if (rqEvt && linkN) { rqEvt->setValue((const uint8_t*)what, strlen(what)); rqEvt->notify(); }""",
"""// 7.5: anything the apps should hear: gestures, a task done, a prayer,
// update progress. Bonded and encrypted, so no token travels with it.
static void evtSend(const char* what) {
  if (rqEvt && linkN) { rqEvt->setValue((const uint8_t*)what, strlen(what)); rqEvt->notify(); }
}
static void sendTap(const char* what) {
  if (!cfgGesture) return;
  evtSend(what);""")

# =================================================================
# GESTURES: tagged by where they came from
# =================================================================
rep("""  if (cfgGesture) {
    if (gestByTouch()) {
      if (g == TG_ONE) sendTap("1");
      else if (g == TG_TWO) sendTap("2");
    }
    return;
  }""", """  if (cfgGesture) {
    // 7.5: the pad, named as the pad, so the Mac can tell it from a knock
    // (a mute must never come from a bumped desk). A hold that turned the
    // knob was the knob, not a hold.
    if (g == TG_ONE) sendTap("t1");
    else if (g == TG_LONG) { if (!knobUsed) sendTap("th"); }
    else if (g == TG_TWO) { if (!knobUsed) sendTap("t2"); }
    knobUsed = false;
    return;
  }""")
rep("""    if (gestByKnock() && !byPad) {
      if (n == 1) sendTap("1");
      else if (n == 2) sendTap("2");
    }
    return;""", """    if (!byPad) {                        // 7.5: named, and three counts
      if (n == 1) sendTap("k1");
      else if (n == 2) sendTap("k2");
      else if (n >= 3) sendTap("k3");
    }
    return;""")

# lean and the knob, in loop while gesture mode is on
rep("""  linkTick();                          // 7.3: who stays connected, and who is the phone""",
    """  linkTick();                          // 7.3: who stays connected, and who is the phone
  gestTilt();                          // 7.5: lean and the tilt knob, for the Mac""")
rep("static void syncBegin() {", r"""// Lean left or right sends once, then waits to come back to the middle.
// Holding the pad with the knob on turns tilt into a dial instead: the
// angle from where the hold began, sent as it changes.
static void gestTilt() {
  if (!cfgGesture || !linkN) { leanState = 0; return; }
  uint32_t now = millis();
  if (now - knobAt < 80) return;
  knobAt = now;
  float tx, ty; tiltRead(tx, ty);
  if (touchOn && knobOn) {
    float deg = asinf(constrain(tx, -0.99f, 0.99f)) * 57.3f;
    if (!knobUsed && fabsf(deg - knobRef) < 6) { if (knobLast == 0) knobRef = deg; }
    int rel = (int)lroundf(deg - knobRef);
    if (abs(rel) >= 6) knobUsed = true;
    if (knobUsed && abs(rel - knobLast) >= 3) {
      knobLast = rel;
      char b[16]; snprintf(b, sizeof(b), "kv %d", rel);
      evtSend(b);
    }
    return;
  }
  knobLast = 0;
  if (leanState == 0 && tx > 0.35f)       { leanState = 1;  sendTap("lr"); }
  else if (leanState == 0 && tx < -0.35f) { leanState = -1; sendTap("ll"); }
  else if (leanState != 0 && fabsf(tx) < 0.15f) leanState = 0;
}

// ---- app filter and VIPs ----
static void listLoad(const char* key, char (*dst)[20], int max, int& n) {
  n = 0;
  String v = prefs.getString(key, "");
  int a = 0;
  while (a < (int)v.length() && n < max) {
    int b = v.indexOf((char)0x1F, a); if (b < 0) b = v.length();
    String one = v.substring(a, b); one.trim();
    if (one.length()) { snprintf(dst[n], 20, "%s", one.c_str()); n++; }
    a = b + 1;
  }
}
static void filtersLoad() {
  listLoad("seena", seenApps, APPS_N, seenN);
  listLoad("mutea", mutedApps, MUTE_N, mutedN);
  listLoad("vipw", vipWords, VIP_N, vipN);
}
static void listSave(const char* key, char (*src)[20], int n) {
  String v;
  for (int i = 0; i < n; i++) { if (i) v += (char)0x1F; v += src[i]; }
  prefs.putString(key, v);
}
static bool hasWord(const char* hay, const char* w) {
  size_t L = strlen(w); if (!L) return false;
  for (const char* p = hay; *p; p++) {
    size_t i = 0;
    while (i < L && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)w[i])) i++;
    if (i == L) return true;
  }
  return false;
}
static bool noteVip(const Note& n) {
  for (int i = 0; i < vipN; i++)
    if (hasWord(n.title, vipWords[i]) || hasWord(n.msg, vipWords[i]) || hasWord(n.app, vipWords[i])) return true;
  return false;
}
// Seen once, remembered, so the apps can offer it to switch off.
static void seenApp(const char* a) {
  for (int i = 0; i < seenN; i++) if (!strcasecmp(seenApps[i], a)) return;
  if (seenN == APPS_N) { for (int i = 0; i < APPS_N - 1; i++) memcpy(seenApps[i], seenApps[i + 1], 20); seenN--; }
  snprintf(seenApps[seenN++], 20, "%s", a);
  listSave("seena", seenApps, seenN);
}
// False: dropped, never stored or shown. A VIP always comes through.
static bool noteAllowed(const Note& n) {
  const char* a = appShort(n);
  seenApp(a);
  if (noteVip(n)) return true;
  for (int i = 0; i < mutedN; i++) if (!strcasecmp(mutedApps[i], a)) return false;
  return true;
}
static String lstJson() {
  String o = "{\"apps\":[";
  for (int i = 0; i < seenN; i++) { if (i) o += ","; o += "\""; o += seenApps[i]; o += "\""; }
  o += "],\"muted\":[";
  for (int i = 0; i < mutedN; i++) { if (i) o += ","; o += "\""; o += mutedApps[i]; o += "\""; }
  o += "],\"vip\":[";
  for (int i = 0; i < vipN; i++) { if (i) o += ","; o += "\""; o += vipWords[i]; o += "\""; }
  o += "]}";
  if (o.length() > 510) o = o.substring(0, 500) + "]}";
  return o;
}

// ---- a call ringing on the popup: tap answers, hold declines ----
static bool popRinging() {
  if (!noteN) return false;
  const Note& n = notes[0];
  return n.cat == CAT_CALL && n.uid < 0x40000000UL && ancsState == ANCS_READY && millis() - n.at < 60000UL;
}

// ---- the Mac screen ----
static int macItems(int* idx) {        // what a hold can tick off: pinned, then the three
  int k = 0;
  if (cards[2].on && cards[2].line[0][0]) idx[k++] = 0;
  if (cards[1].on) for (int i = 0; i < 3; i++) if (cards[1].line[i][0]) idx[k++] = i + 1;
  return k;
}
static void drawMac() {
  oled.clearDisplay();
  char tb[8]; clockStr(tb, sizeof(tb), false);
  if (depth == 0) {
    titleBar("MAC", tb);
    int y = 14;
    if (cards[2].on && cards[2].line[0][0]) { char b[24]; snprintf(b, sizeof(b), "> %.19s", cards[2].line[0]); at(0, y, b); y += 12; }
    if (cards[1].on) for (int i = 0; i < 3 && y <= 38; i++) if (cards[1].line[i][0]) {
      char b[24]; snprintf(b, sizeof(b), "o %.19s", cards[1].line[i]); at(0, y, b); y += 12; }
    if (y == 14 && !cards[0].on) ctr("nothing from the Mac", 30, 1);
    if (cards[0].on && cards[0].line[0][0]) {
      oled.drawFastHLine(0, 51, SCRW, SSD1306_WHITE);
      ctr(cards[0].line[0], 54, 1);
    }
  } else {
    int idx[4]; int k = macItems(idx);
    titleBar("DONE?", tb);
    if (!k) { ctr("nothing to tick off", 30, 1); oled.display(); return; }
    if (macSel >= k) macSel = 0;
    for (int r = 0; r < k; r++) {
      int y = 14 + r * 12;
      const char* t = idx[r] == 0 ? cards[2].line[0] : cards[1].line[idx[r] - 1];
      if (r == macSel) { oled.fillRect(0, y - 2, SCRW, 12, SSD1306_WHITE); oled.setTextColor(SSD1306_BLACK); }
      char b[24]; snprintf(b, sizeof(b), "%s%.19s", idx[r] == 0 ? "> " : "o ", t);
      at(0, y, b); oled.setTextColor(SSD1306_WHITE);
    }
  }
  oled.display();
}
static bool cardsAny() { return cards[0].on || cards[1].on || cards[2].on; }

// ---- Mac left behind ----
static void drawWalk() {
  oled.clearDisplay();
  bool on = (millis() / 500) % 2;
  if (on) oled.drawRoundRect(0, 0, SCRW, SCRH, 6, SSD1306_WHITE);
  ctr("YOUR MAC", 12, 2);
  ctr("is left behind", 36, 1);
  ctr("touch to dismiss", 52, 1);
  oled.display();
}

// ---- update over Bluetooth ----
static void drawBleOta() {
  oled.clearDisplay();
  titleBarC("UPDATE");
  ctr("from your Mac", 16, 1);
  int pct = otaSize ? (int)((uint64_t)otaGot * 100 / otaSize) : 0;
  oled.drawRoundRect(8, 30, 112, 10, 3, SSD1306_WHITE);
  oled.fillRoundRect(10, 32, (108 * pct) / 100, 6, 2, SSD1306_WHITE);
  char b[8]; snprintf(b, sizeof(b), "%d%%", pct);
  ctr(b, 46, 1);
  oled.display();
}
static void otaStop(const char* why) {
  if (otaOn) Update.abort();
  otaOn = false;
  char b[32]; snprintf(b, sizeof(b), "ota err %s", why); evtSend(b);
  Serial.printf("ota over bluetooth stopped: %s\n", why);
  flash("UPDATE STOPPED", 1400);
}

static void syncBegin() {""")
rep("static void syncBegin();\n", "static void syncBegin();\nstatic void gestTilt();\nstatic void evtSend(const char* what);\nstatic bool noteAllowed(const Note& n);\nstatic bool noteVip(const Note& n);\nstatic String lstJson();\nstatic void filtersLoad();\nstatic bool popRinging();\nstatic void drawMac();\nstatic bool cardsAny();\nstatic void drawWalk();\nstatic void drawBleOta();\nstatic void otaStop(const char* why);\nstatic void listSave(const char* key, char (*src)[20], int n);\n")

# =================================================================
# CHARACTERISTICS: the list, and the update bytes
# =================================================================
rep("""  svc->createCharacteristic(RQ_CFG, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC)
     ->setCallbacks(new RqChrCb(6));""", """  svc->createCharacteristic(RQ_CFG, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC)
     ->setCallbacks(new RqChrCb(6));
  svc->createCharacteristic(RQ_LST, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC)
     ->setCallbacks(new RqChrCb(8));
  svc->createCharacteristic(RQ_OTA, NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC)
     ->setCallbacks(new RqChrCb(9));""")
rep("""    if (kind == 6) { String j = cfgJson(); c->setValue((const uint8_t*)j.c_str(), j.length()); return; }""",
    """    if (kind == 6) { String j = cfgJson(); c->setValue((const uint8_t*)j.c_str(), j.length()); return; }
    if (kind == 8) { String j = lstJson(); c->setValue((const uint8_t*)j.c_str(), j.length()); return; }""")
rep("""    if (kind == 5) {                     // the pointer: no queue, ten a second""",
    """    if (kind == 9) {                     // 7.5: firmware bytes, straight to flash
      if (!otaOn || otaErr) return;
      NimBLEAttValue v = c->getValue();
      size_t n = v.length();
      if (otaGot + n > otaSize || Update.write((uint8_t*)v.data(), n) != n) { otaErr = true; return; }
      otaGot += n; otaLastAt = millis();
      return;
    }
    if (kind == 5) {                     // the pointer: no queue, ten a second""")

# =================================================================
# "!" COMMANDS
# =================================================================
rep("""  } else if (verb == "remclear") {""", r"""  } else if (verb == "card") {          // slot title \x1F line \x1F line \x1F line \x1F bar
    int slot = atoi(rest);
    if (slot < 0 || slot > 2) return;
    const char* p = strchr(rest, ' ');
    Card& cd = cards[slot];
    memset(&cd, 0, sizeof(cd)); cd.bar = -1;
    if (!p || !p[1]) return;                       // no text: the card is gone
    char* f[5] = { (char*)p + 1, nullptr, nullptr, nullptr, nullptr };
    int k = 1;
    for (char* q = (char*)p + 1; *q && k < 5; q++) if (*q == 0x1F) { *q = 0; f[k++] = q + 1; }
    snprintf(cd.title, sizeof(cd.title), "%.21s", f[0]);
    for (int i = 0; i < 3; i++) if (k > i + 1 && f[i + 1]) snprintf(cd.line[i], sizeof(cd.line[i]), "%.23s", f[i + 1]);
    if (k > 4 && f[4]) cd.bar = (int8_t)constrain(atoi(f[4]), -1, 100);
    cd.on = true;
  } else if (verb == "knob") {
    knobOn = atoi(rest) != 0;
  } else if (verb == "walk") {
    walkOn = atoi(rest) != 0;
  } else if (verb == "bye") {                      // the Mac is going to sleep: not left behind
    macBye = true;
  } else if (verb == "dim") {
    macDim = atoi(rest) != 0; applyBright();
  } else if (verb == "mute" || verb == "vip") {    // the whole list, \x1F between
    bool vip = verb == "vip";
    char tmp[MUTE_N][20]; int n = 0;
    const char* a = rest;
    while (*a && n < (vip ? VIP_N : MUTE_N)) {
      const char* b = strchr(a, 0x1F); size_t L = b ? (size_t)(b - a) : strlen(a);
      if (L) { snprintf(tmp[n], 20, "%.*s", (int)(L < 19 ? L : 19), a); n++; }
      if (!b) break; a = b + 1;
    }
    if (vip) { vipN = n; for (int i = 0; i < n; i++) memcpy(vipWords[i], tmp[i], 20); listSave("vipw", tmp, n); }
    else     { mutedN = n; for (int i = 0; i < n; i++) memcpy(mutedApps[i], tmp[i], 20); listSave("mutea", mutedApps, mutedN); }
  } else if (verb == "pt") {                       // five prayer times, minutes after midnight
    int v[5];
    if (sscanf(rest, "%d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5) {
      for (int i = 0; i < 5; i++) prayerMin[i] = constrain(v[i], 0, 1439);
      struct tm t; if (nowLocal(&t)) prayerDay = t.tm_yday;
      savePrayer();
    }
  } else if (verb == "ota") {
    if (!strncmp(rest, "begin ", 6)) {
      uint32_t sz = strtoul(rest + 6, nullptr, 10);
      if (otaOn) Update.abort();
      if (sz < 100000 || !Update.begin(sz)) { evtSend("ota err begin"); flash("NO ROOM", 1200); return; }
      otaOn = true; otaErr = false; otaSize = sz; otaGot = 0; otaLastAt = millis(); otaPctSent = -1;
      otaConn = conn;
      NimBLEServer* sv = NimBLEDevice::getServer();
      if (sv) sv->updateConnParams(conn, 6, 12, 0, 400);   // as fast as the link goes
      pmSet(false);
      wake("update");
      evtSend("ota ready");
    } else if (!strcmp(rest, "end")) {
      if (!otaOn) return;
      if (otaErr || otaGot != otaSize) { otaStop(otaErr ? "write" : "short"); return; }
      if (!Update.end(true)) { otaOn = false; evtSend("ota err verify"); flash("UPDATE BAD", 1400); return; }
      otaOn = false;
      evtSend("ota ok");
      flash("UPDATED", 900);
      delay(600);
      ESP.restart();
    } else if (!strcmp(rest, "abort")) {
      otaStop("abort");
    }
  } else if (verb == "remclear") {""")

# =================================================================
# THE MAC SCREEN takes the place Focus left
# =================================================================
rep("""  if (s == S_FOCUS)   return false;                   // 6.1: gone from the round""",
    """  if (s == S_FOCUS)   return cardsAny();              // 7.5: the Mac screen, when it has something""")
rep('''  { "HOME", "VEHICLE", "REMINDERS", "FOCUS", "WEATHER", "NOTIFICATIONS", "PRAYER",''',
    '''  { "HOME", "VEHICLE", "REMINDERS", "MAC", "WEATHER", "NOTIFICATIONS", "PRAYER",''')
rep("""const int S_ORDER[] = { S_HOME, S_MSG, S_REMIND, S_BIKE, S_WEATHER, S_PRAYER,
                        S_FAITH, S_READS, S_GAMES, S_SETTINGS, S_SYSTEM, S_FOCUS };""",
    """const int S_ORDER[] = { S_HOME, S_MSG, S_FOCUS, S_REMIND, S_BIKE, S_WEATHER, S_PRAYER,
                        S_FAITH, S_READS, S_GAMES, S_SETTINGS, S_SYSTEM };""")
rep("""      case S_FOCUS:    drawFocus();    break;""", """      case S_FOCUS:    drawMac();      break;   // 7.5""")
# touches on it: hold opens the tick list; tap moves; hold ticks; back returns
rep("""  if (screen == S_MSG && depth == 1) {
    switch (g) {
      case TG_ONE:  noteSel = (noteSel + 1) % (noteN + 1); return;""", """  if (screen == S_FOCUS) {                    // 7.5: the Mac screen
    int idx[4]; int k = macItems(idx);
    if (depth == 0) {
      if (g == TG_LONG && k) { depth = 1; macSel = 0; clickShrink(); return; }
      if (g == TG_LONG) return;
    } else {
      switch (g) {
        case TG_ONE:  if (k) macSel = (macSel + 1) % k; return;
        case TG_LONG:
          if (k) {
            int w = idx[macSel < k ? macSel : 0];
            char b[12]; snprintf(b, sizeof(b), "done %d", w); evtSend(b);
            if (w == 0) cards[2].on = false;
            else { cards[1].line[w - 1][0] = 0; }
            flash("DONE", 700);
            if (macItems(idx) == 0) depth = 0;
          }
          return;
        case TG_TWO: depth = 0; return;
        default: return;
      }
    }
  }
  if (screen == S_MSG && depth == 1) {
    switch (g) {
      case TG_ONE:  noteSel = (noteSel + 1) % (noteN + 1); return;""")
rep("static int nextScreen(int from) {", "static bool cardsAny();\nstatic int macItems(int* idx);\nstatic int nextScreen(int from) {")
rep("static void drawMac();\n", "static void drawMac();\nstatic int macItems(int* idx);\n")

# =================================================================
# CALLS: answered or declined from the popup
# =================================================================
rep("""  if (popOn) {
    if (g == TG_LONG) popupClose(true);                  // open it""", """  if (popOn && popRinging()) {                           // 7.5: a call, ringing
    ancsAction(notes[0].uid, g == TG_LONG ? 1 : 0);      // hold declines, tap answers
    flash(g == TG_LONG ? "DECLINED" : "ANSWERED", 900);
    popupClose(false);
    return;
  }
  if (popOn) {
    if (g == TG_LONG) popupClose(true);                  // open it""")
rep("""  twoButtons("tap:close", "hold:open");""", """  if (popRinging()) twoButtons("tap:answer", "hold:decline");
  else              twoButtons("tap:close", "hold:open");""")

# =================================================================
# THE FILTER: on both ways in
# =================================================================
rep("""      addNote(noteStage);
      if (!cfgQuiet) popupShow();
    }
  }""", """      // 7.5: switched-off apps never arrive; a VIP shows even when quiet.
      if (noteAllowed(noteStage)) {
        addNote(noteStage);
        if (!cfgQuiet || noteVip(noteStage)) popupShow();
      }
    }
  }""")
rep("""        addNote(n);
        if (!cfgQuiet) popupShow();""", """        if (noteAllowed(n)) {
          addNote(n);
          if (!cfgQuiet || noteVip(n)) popupShow();
        }""")

# =================================================================
# PRAYER: the Mac hears the call too
# =================================================================
rep("""    wake("prayer");
    Serial.printf("prayer alert: %s in %d min\\n", PRAYERS[i], d);""", """    wake("prayer");
    if (d == 0) { char b[24]; snprintf(b, sizeof(b), "pray %s", PRAYERS[i]); evtSend(b); }   // 7.5: the Mac pauses
    Serial.printf("prayer alert: %s in %d min\\n", PRAYERS[i], d);""")

# =================================================================
# DIM, WALK AWAY, UPDATES: in loop and on the link
# =================================================================
rep("""static void applyBright() {
  oled.ssd1306_command(SSD1306_SETCONTRAST);
  oled.ssd1306_command(cfgBright);
}""", """static void applyBright() {
  oled.ssd1306_command(SSD1306_SETCONTRAST);
  oled.ssd1306_command(macDim ? min(cfgBright, 8) : cfgBright);   // 7.5: low while you type
}""")
rep("static void applyBright() {", "extern bool macDim;\nstatic void applyBright() {")
rep("""    // A companion gone takes gesture mode with it.
    if (wasComp) cfgGesture = false;""", """    // A companion gone takes gesture mode with it.
    if (wasComp) {
      cfgGesture = false; knobOn = false; macDim = false;
      // 7.5: gone while awake, without saying goodnight: left behind.
      if (walkOn && !macBye) { walkUntil = millis() + 60000UL; btWokeReq = true; }
      macBye = false;
    }
    if (otaOn && h == otaConn) { otaErr = true; }""")
rep("""  if (pgUntil && now > pgUntil) pgUntil = 0;
  if (pgUntil) {""", """  // 7.5: an update arriving over Bluetooth owns the screen
  if (otaOn) {
    lastActive = now;
    if (otaErr) otaStop("link");
    else if ((int32_t)(now - otaLastAt) > 20000) otaStop("timeout");
    else {
      int pct = otaSize ? (int)((uint64_t)otaGot * 100 / otaSize) : 0;
      if (pct / 5 != otaPctSent / 5) { otaPctSent = pct; char b[12]; snprintf(b, sizeof(b), "ota %d", pct); evtSend(b); }
      if (now - lastDraw >= 250) { lastDraw = now; drawBleOta(); }
      delay(2); return;
    }
  }
  if (walkUntil && now > walkUntil) walkUntil = 0;
  if (walkUntil) {
    lastActive = now;
    if (now - lastDraw >= 120) { lastDraw = now; drawWalk(); }
    delay(2); return;
  }
  if (pgUntil && now > pgUntil) pgUntil = 0;
  if (pgUntil) {""")
rep("""  if (pgUntil)       { pgUntil = 0; return; }
  if (findUntil)     { findUntil = 0; return; }""", """  if (pgUntil)       { pgUntil = 0; return; }
  if (walkUntil)     { walkUntil = 0; return; }     // 7.5: Mac left behind, seen
  if (findUntil)     { findUntil = 0; return; }""", 2)
# the filters come back at start
rep("""  devLoad();""", """  devLoad();
  filtersLoad();                       // 7.5""")
# STAT says 7.5's channels are there
rep("""relax=%d;follow=%d;gest=%d;last=%s",""", """relax=%d;follow=%d;gest=%d;knob=%d;walk=%d;last=%s",""")
rep("""             relaxOn ? 1 : 0, cfgFollow ? 1 : 0, cfgGesture ? 1 : 0, appLast);""",
    """             relaxOn ? 1 : 0, cfgFollow ? 1 : 0, cfgGesture ? 1 : 0, knobOn ? 1 : 0, walkOn ? 1 : 0, appLast);""")

open(SRC, 'w').write(s)
print("7.5 ok")

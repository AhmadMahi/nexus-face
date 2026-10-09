#!/usr/bin/env python3
# Rafiq 6.4.0. Applied after patch_v63.
import re, sys
SRC = sys.argv[1]
s = open(SRC).read()
def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count: sys.exit(f"anchor {n}x:\n{old[:220]}")
    s = s.replace(old, new)

rep('#define FW_VERSION "6.3.0"', '#define FW_VERSION "6.4.0"')

# ---------------------------------------------------------------- globals
rep("uint32_t    glanceUntil = 0;", """// ---- 6.4 Away ----
//  "Away: back at 3, call Ahmed". The screen shows that and the time,
//  nothing else, through restarts, until a RAFIQ home. Everything done
//  to it meanwhile is counted into the tamper log. Bluetooth stays on,
//  or home could never arrive.
bool     awayOn = false;
String   awayText = "";
#define  AWAY_SHOW_MS 8000UL
enum { AE_TOUCH = 0, AE_SHAKE, AE_MOVE, AE_KNOCK, AE_N };
const char* AE_NAME[AE_N] = { "Touched", "Shaken", "Moved", "Knocked" };
uint16_t aeCount[AE_N] = { 0, 0, 0, 0 };
uint32_t aeLast[AE_N]  = { 0, 0, 0, 0 };
static void awayEv(int k) { if (!awayOn) return; aeCount[k]++; aeLast[k] = millis(); }
uint32_t popGlanceMs = 1000;           // how long news shows when it woke the robot
// ---- 6.4 phone-away sleep ----
//  Ten minutes dark and linked-or-listening after the phone goes, then
//  off, waking every 3 minutes for the first hour and every 5 after to
//  see whether it is back. A check wake is silent: no screen.
RTC_DATA_ATTR uint32_t rtcAwaySince = 0;   // when checking began, system clock
RTC_DATA_ATTR uint8_t  rtcWakeCheck = 0;   // this timer wake is a check, not a prayer
bool     deepAuto = false;             // this deep sleep is the phone-away kind
bool     checkWake = false;            // woke only to look for the phone
uint32_t checkUntil = 0;
#define  CHECK_WINDOW_MS 20000UL
uint32_t    glanceUntil = 0;""")
rep("#define BT_DEEP_GRACE_MS 60000UL       // no phone for this long, then off",
    "#define BT_DEEP_GRACE_MS 600000UL      // no phone for ten minutes, then off (6.4)")

# ---------------------------------------------------------------- flashes: black, not white
rep("""static void drawFlash() {
  oled.clearDisplay();
  oled.fillRect(0, 0, SCRW, SCRH, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  int n = strlen(flashWord);""", """static void drawFlash() {
  // 6.4: white words on black. Full white screens are kept for the
  // things that must be noticed: the call to prayer, the guard, find.
  oled.clearDisplay();
  int n = strlen(flashWord);""")

# ---------------------------------------------------------------- away: drawing and the loop
rep("static void syncBegin() {", r"""static void drawAway() {
  oled.clearDisplay();
  fitText(awayText.length() ? awayText.c_str() : "Away", 2, 50, 0);
  if (fb.ok || timeOk) { loadBits(); ctr(fb.hm, 56, 1); }
  oled.display();
}
static void awayFlush(bool all) {
  uint32_t now = millis();
  for (int k = 0; k < AE_N; k++) {
    if (!aeCount[k] || (!all && now - aeLast[k] < 20000)) continue;
    char l[24];
    if (aeCount[k] == 1) snprintf(l, sizeof(l), "%s", AE_NAME[k]);
    else                 snprintf(l, sizeof(l), "%s x%u", AE_NAME[k], (unsigned)aeCount[k]);
    tlogAdd(l);
    aeCount[k] = 0;
  }
}
static void awaySet(bool on, const char* text) {
  if (text && *text) { awayText = String(text).substring(0, 160); prefs.putString("awayt", awayText); }
  if (on == awayOn) { if (on) wake("away"); return; }
  awayOn = on;
  prefs.putBool("away", awayOn);
  if (on) {
    tlogAdd("Away on");
    popOn = false; pgUntil = 0; findUntil = 0; upState = U_OFF;
    wake("away");
  } else {
    awayFlush(true);
    tlogAdd("Away off");
    screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0;
    wake("shortcut");
    flash("WELCOME BACK", 1200);
  }
}

static void syncBegin() {""")
rep("static void syncBegin();\n", "static void syncBegin();\nstatic void drawAway();\nstatic void awayFlush(bool all);\nstatic void awaySet(bool on, const char* text);\n")

# loop: count flushes, the check wake, and away takes the screen
rep("""  serviceTamper();
  if (wxDirty)""", """  serviceTamper();
  // 6.4: these run asleep as well as awake.
  awayFlush(false);
  if (phoneHeld()) rtcAwaySince = 0;   // the phone is here: the checking schedule starts over
  if (checkWake) {
    if (phoneHeld()) { checkWake = false; Serial.println("check: phone is back, staying linked"); }
    else if (now > checkUntil) { checkWake = false; deepAuto = true; goDeep(); }
  }
  if (wxDirty)""")
rep("""  if (glanceUntil && now > glanceUntil) {""", """  if (awayOn && !asleep) {
    if (now - lastActive > AWAY_SHOW_MS) { goSleepQuick(); return; }
    if (now - lastDraw >= 500) { lastDraw = now; drawAway(); }
    delay(5); return;
  }
  if (glanceUntil && now > glanceUntil) {""")

# away holds off deep sleep (it keeps counting); auto deep is the phone-away kind
rep("""      (now - since) > wait && upState == U_OFF && !storyBusy) {
    goDeep();
  }""", """      (now - since) > wait && upState == U_OFF && !storyBusy && !awayOn && !checkWake) {
    deepAuto = true;
    goDeep();
  }""")

# goDeep: phone-away sleeps are silent and set the check timer
rep("""  if (deepOff) return;
  Serial.println("switching off until touched");
  sleepCard();
""", """  if (deepOff) return;
  awayFlush(true);
  bool checking = deepAuto && cfgNet == NET_BT && NimBLEDevice::getNumBonds() > 0;
  Serial.println(checking ? "switching off, checking for the phone" : "switching off until touched");
  if (!checking) { sleepCard(); rtcAwaySince = 0; }
""")
rep("""  rtcAlarmAt = (secs > 0 && timeOk) ? (uint32_t)time(nullptr) + (uint32_t)secs : 0;""",
"""  rtcWakeCheck = 0;
  if (checking) {
    uint32_t tnow = (uint32_t)time(nullptr);
    if (!rtcAwaySince) rtcAwaySince = tnow;
    long chk = (tnow - rtcAwaySince < 3600) ? 180 : 300;   // 3 min for an hour, then 5
    if (secs <= 0 || chk < secs) { secs = chk; rtcWakeCheck = 1; }
  }
  rtcAlarmAt = (secs > 0 && timeOk) ? (uint32_t)time(nullptr) + (uint32_t)secs : 0;""")

# setup: a check wake stays dark
rep("""  if (fromDeep) {
    wokeForAlarm = (woke_ == ESP_SLEEP_WAKEUP_TIMER);
    wokeBy = wokeForAlarm ? "prayer" : "picked up";
    Serial.printf("back from being off (%s)\\n", wokeBy.c_str());
  }
  drawHome();                          // on screen before anything can block""",
"""  if (fromDeep) {
    wokeForAlarm = (woke_ == ESP_SLEEP_WAKEUP_TIMER);
    wokeBy = wokeForAlarm ? "prayer" : "picked up";
    if (wokeForAlarm && rtcWakeCheck) {          // only looking for the phone
      wokeForAlarm = false; wokeBy = "check";
      checkWake = true;
    }
    Serial.printf("back from being off (%s)\\n", wokeBy.c_str());
  }
  if (!fromDeep) rtcAwaySince = 0;
  rtcWakeCheck = 0;
  awayOn   = prefs.getBool("away", false);
  awayText = prefs.getString("awayt", "");
  if (checkWake) {
    // Dark, listening, for twenty seconds. The loop decides: the phone
    // came back and it stays linked, or it is off again.
    asleep = true; sleptAt = millis();
    screenPower(false);
    checkUntil = millis() + CHECK_WINDOW_MS;
  } else {
    drawHome();                        // on screen before anything can block
  }""")

# away: nothing works but looking at it
GUARD = "  if (awayOn) { lastActive = millis(); return; }   // away: nothing but the message\n"
rep("""static void touchGesture(uint8_t g) {
  lastActive = millis();
""", """static void touchGesture(uint8_t g) {
  lastActive = millis();
""" + GUARD)
for fn in ("knockOne() {\n  cTap++;\n", "knockTwo() {\n  cDouble++;\n", "knockThree() {\n  cTriple++;\n", "knockFour() {\n  cQuad++;\n"):
    rep("static void " + fn, "static void " + fn + GUARD)
rep("""static void servicePrayerAlert() {
  struct tm t;""", """static void servicePrayerAlert() {
  if (awayOn) return;                  // away: nothing comes up
  struct tm t;""")
rep("""static void pgFire(const char* why) {""", """static void pgFire(const char* why) {
  if (awayOn) return;""")
rep("""static bool gamePlaying() { return screen == S_GAMES && gState == GS_PLAY; }
static void popupShow() {""", """static bool gamePlaying() { return screen == S_GAMES && gState == GS_PLAY; }
static void popupShow() {
  if (awayOn) return;                  // still kept in the list""")
rep("""  return holdShown && touchOn && !sleepArmed && !cfgGesture && !gamePlaying();""",
    """  return holdShown && touchOn && !sleepArmed && !cfgGesture && !gamePlaying() && !awayOn;""")
# no switch-off by holding, while away
rep("""      if (!cfgGesture && !sleepArmed && held >= TOUCH_HOME_MS) {
        sleepArmed = now;""", """      if (!cfgGesture && !sleepArmed && !awayOn && held >= TOUCH_HOME_MS) {
        sleepArmed = now;""")
# a glance while away shows the message for its full time
rep("""  glanceUntil = glance ? millis() + GLANCE_MS : 0;""",
    """  glanceUntil = (glance && !awayOn) ? millis() + GLANCE_MS : 0;""")

# away: count what happens to it
rep("""          glanceUntil = 0;                       // a touch makes a glance a real wake""",
    """          glanceUntil = 0;                       // a touch makes a glance a real wake
          awayEv(AE_TOUCH);""")
rep("""    if (asleep) { if (motionWakes()) wake("shake"); return; }""",
    """    awayEv(AE_SHAKE);
    if (asleep) { if (motionWakes()) wake("shake"); return; }""")
rep("""    if (asleep) { if (!motionWakes()) return; wake("picked up"); }""",
    """    if (asleep) { awayEv(AE_MOVE); if (!motionWakes()) return; wake("picked up"); }""")
rep("""    if (asleep) { if (!motionWakes()) return; wake("moved"); }""",
    """    if (asleep) { awayEv(AE_MOVE); if (!motionWakes()) return; wake("moved"); }""")
rep("""    if ((s & INT_TAP1) && asleep && !motionWakes()) { /* asleep and set to wake by touch only */ }""",
    """    if (s & INT_TAP1) awayEv(AE_KNOCK);
    if ((s & INT_TAP1) && (awayOn || (asleep && !motionWakes()))) {
      if (awayOn && asleep && motionWakes()) wake("knock");   // show the message, act on nothing
    }""")

# ---------------------------------------------------------------- RAFIQ: away, home, wake by
rep("""static void rafiqPayload(const char* p, bool fresh) {
  // The first word decides whether the rest belongs to it.
  char first[16]; int n = 0;
  for (const char* q = p; *q && *q != '\\n' && *q != ':' && *q != ' ' && n < 15; q++)
    first[n++] = tolower((unsigned char)*q);
  first[n] = 0;
""", """static void rafiqPayload(const char* p, bool fresh) {
  // The first word decides whether the rest belongs to it.
  char first[16]; int n = 0;
  for (const char* q = p; *q && *q != '\\n' && *q != ':' && *q != ' ' && n < 15; q++)
    first[n++] = tolower((unsigned char)*q);
  first[n] = 0;

  // Away: the rest of the text is the message. Away on its own turns it
  // on with the last message. While away, only home and away are heard.
  if (!strcmp(first, "away")) {
    if (!fresh) return;
    const char* t = p + n;
    while (*t == ':' || *t == ' ') t++;
    String m = String(t); m.replace("\\r", ""); m.trim();
    awaySet(true, m.c_str());
    return;
  }
  if (awayOn) {
    char fc[16]; strcpy(fc, first);                // "Home." and "home!" count too
    size_t L = strlen(fc);
    while (L && (fc[L - 1] == '.' || fc[L - 1] == '!')) fc[--L] = 0;
    if (fresh && !strcmp(fc, "home")) awaySet(false, nullptr);
    return;
  }
""")
rep("""  else if (is("home")) { screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0; }""",
    """  else if (is("home")) { screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0; }
  else if (is("wake touch") || is("wake by touch")) { cfgWakeBy = 1; prefs.putInt("wakeby", 1); flash("WAKE BY TOUCH", 1200); }
  else if (is("wake move") || is("wake shake") || is("wake by move") || is("wake by shake") || is("wake motion")) {
    cfgWakeBy = 2; prefs.putInt("wakeby", 2); flash("WAKE BY MOVE", 1200);
  }
  else if (is("wake both") || is("wake by both") || is("wake all")) { cfgWakeBy = 0; prefs.putInt("wakeby", 0); flash("WAKE BY BOTH", 1200); }
  else if (is("wake by")) {
    cfgWakeBy = (cfgWakeBy + 1) % 3; prefs.putInt("wakeby", cfgWakeBy);
    flash(cfgWakeBy == 1 ? "WAKE BY TOUCH" : cfgWakeBy == 2 ? "WAKE BY MOVE" : "WAKE BY BOTH", 1200);
  }""")

# msg: shows for 3 s when it wakes the robot
rep("""    addNote(pn);
    if (popupSecs()) popupShow();
    else { wake("message"); screen = S_MSG; depth = 2; noteIdx = 0; }""", """    addNote(pn);
    popGlanceMs = 3000;                  // something you sent: three seconds, not one
    if (popupSecs()) popupShow();
    else { wake("message"); screen = S_MSG; depth = 2; noteIdx = 0; }
    popGlanceMs = 1000;""")
rep("""  popUntil = millis() + (popWoke ? 1000UL : popupSecs() * 1000UL);""",
    """  popUntil = millis() + (popWoke ? popGlanceMs : popupSecs() * 1000UL);""")

# ---------------------------------------------------------------- tamper log: clear all, delete one
rep("""static const char* tlLabel(int i, char* b, size_t n) { snprintf(b, n, "%s", tlLines[i]); return b; }
static void drawTlog() {
  char r[8]; snprintf(r, sizeof(r), "%d", tlN);
  drawList("TAMPER LOG", r, tlN, tlSel, tlLabel);
}""", """static const char* tlLabel(int i, char* b, size_t n) {
  if (i >= tlN) snprintf(b, n, "Clear all");
  else          snprintf(b, n, "%s", tlLines[i]);
  return b;
}
static void drawTlog() {
  char r[8]; snprintf(r, sizeof(r), "%d", tlN);
  drawList("TAMPER LOG", r, tlN + 1, tlSel, tlLabel);
}
// Remove the k-th newest line, as the list shows them.
static void tlogDelete(int k) {
  if (!fsOk) return;
  File f = LittleFS.open(TLOG_PATH, "r");
  if (!f) return;
  String all = f.readString(); f.close();
  int total = 0;
  for (unsigned i = 0; i < all.length(); i++) if (all[i] == '\\n') total++;
  int target = total - 1 - k, line = 0;
  String out;
  int start = 0;
  for (unsigned i = 0; i < all.length(); i++) {
    if (all[i] != '\\n') continue;
    if (line != target) out += all.substring(start, i + 1);
    line++; start = i + 1;
  }
  File w = LittleFS.open(TLOG_PATH, "w");
  if (w) { w.print(out); w.close(); }
}""")
rep("""  if (screen == S_SETTINGS && depth == 2 && itemIdx == C_TLOG && g == TG_ONE) {
    if (tlN) tlSel = (tlSel + 1) % tlN;
    return;
  }""", """  if (screen == S_SETTINGS && depth == 2 && itemIdx == C_TLOG && g == TG_ONE) {
    tlSel = (tlSel + 1) % (tlN + 1);
    return;
  }
  if (screen == S_SETTINGS && depth == 2 && itemIdx == C_TLOG && g == TG_LONG) {
    if (tlSel >= tlN) { if (fsOk) LittleFS.remove(TLOG_PATH); flash("LOG CLEARED", 1100); }
    else              { tlogDelete(tlSel); flash("DELETED", 800); }
    int keep = tlSel;
    tlogLoad();
    tlSel = keep < tlN ? keep : tlN;
    return;
  }""")
rep("static void drawTlog();\n", "static void drawTlog();\nstatic void tlogDelete(int k);\n")

open(SRC, 'w').write(s)
print("6.4 ok")

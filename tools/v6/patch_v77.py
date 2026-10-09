#!/usr/bin/env python3
# Rafiq 7.7.0: battery. A slower link while the screen is dark, night
# sleep (with "an hour later" whenever you need it), and the
# accelerometer's low-power mode. After patch_v761.
import sys
SRC = sys.argv[1]
s = open(SRC).read()
def rep(a, b, c=1):
    global s
    n = s.count(a)
    if n != c: sys.exit(f"anchor {n}x:\n{a[:220]}")
    s = s.replace(a, b)

rep('#define FW_VERSION "7.6.1"', '#define FW_VERSION "7.7.0"')

# =================================================================
# C. THE ACCELEROMETER'S LOW-POWER MODE
# =================================================================
#  Same 100 readings a second, so knocks are still caught; about 50 uA
#  instead of 140 (datasheet, table 7), all day and all night.
rep("""    wReg(adxl, A_DATA_FORMAT, 0x0B);
    wReg(adxl, A_THRESH_TAP, 0x28); wReg(adxl, A_DUR, 0x10);""",
    """    wReg(adxl, A_DATA_FORMAT, 0x0B);
    wReg(adxl, 0x2C, 0x1A);            // 7.7: BW_RATE, low power at 100 Hz
    wReg(adxl, A_THRESH_TAP, 0x28); wReg(adxl, A_DUR, 0x10);""")

# =================================================================
# GLOBALS AND SETTINGS ROWS
# =================================================================
rep("volatile uint8_t btNews = 0;", """volatile uint8_t btNews = 0;
// ---- 7.7: night sleep ----
bool cfgNight = false;
int  cfgBed = 23 * 60;                 // bedtime, minutes after midnight
RTC_DATA_ATTR int nightPush = 0;       // "an hour later", for tonight only
uint32_t nightCardUntil = 0;
bool nightDeep = false;""")
rep("""       C_MULTI, C_DEV1, C_DEV2, C_AUTOAWAY, C_COUNT };""",
    """       C_MULTI, C_DEV1, C_DEV2, C_AUTOAWAY, C_NIGHT, C_BED, C_COUNT };""")
rep('''    "Multi-link", "Primary", "Second", "Auto away" };''',
    '''    "Multi-link", "Primary", "Second", "Auto away", "Night sleep", "Bedtime" };''')
rep("""  { C_UPDATE,   C_DEEP,  C_BATT,  C_RESET,  C_REBOOT, C_ABOUT,  SG_END,  SG_END },""",
    """  { C_UPDATE,   C_DEEP,  C_BATT,  C_NIGHT,  C_BED,    C_RESET,  C_REBOOT, C_ABOUT },""")
rep("""      case C_AUTOAWAY: snprintf(v, sizeof(v), "%s", cfgAutoAway ? "on" : "off"); break;""",
    """      case C_AUTOAWAY: snprintf(v, sizeof(v), "%s", cfgAutoAway ? "on" : "off"); break;
      case C_NIGHT:  snprintf(v, sizeof(v), "%s", cfgNight ? "on" : "off"); break;
      case C_BED:    snprintf(v, sizeof(v), "%02d:%02d", cfgBed / 60, cfgBed % 60); break;""")
rep("""      case C_AUTOAWAY:
        cfgAutoAway = !cfgAutoAway; prefs.putBool("autoaway", cfgAutoAway);
        break;""", """      case C_AUTOAWAY:
        cfgAutoAway = !cfgAutoAway; prefs.putBool("autoaway", cfgAutoAway);
        break;
      case C_NIGHT:
        cfgNight = !cfgNight; prefs.putBool("night", cfgNight);
        flash(cfgNight ? "NIGHT SLEEP ON" : "NIGHT SLEEP OFF", 1100);
        break;
      case C_BED: {
        // 21:00 to 01:30, half an hour a press, then round again
        int b = cfgBed < 720 ? cfgBed + 1440 : cfgBed;
        b += 30; if (b > 1440 + 90) b = 21 * 60;
        cfgBed = b % 1440; prefs.putInt("bed", cfgBed);
        break;
      }""")
rep("""  cfgAutoAway = prefs.getBool("autoaway", true);""", """  cfgAutoAway = prefs.getBool("autoaway", true);
  cfgNight    = prefs.getBool("night", false);
  cfgBed      = constrain(prefs.getInt("bed", 23 * 60), 0, 1439);""")

# the apps: night and bedtime as settings, and the push
rep("""    else return false;
  return true;
}""", """    else if (k == "night") { cfgNight = v != 0; prefs.putBool("night", cfgNight); }
    else if (k == "bed")   { cfgBed = constrain(v, 0, 1439); prefs.putInt("bed", cfgBed); }
    else return false;
  return true;
}""")
rep('''       ",\\"intWired\\":" + String(intWired ? "true" : "false") +''',
    '''       ",\\"intWired\\":" + String(intWired ? "true" : "false") +
       ",\\"night\\":" + String(cfgNight ? "true" : "false") +
       ",\\"bed\\":" + String(cfgBed) + ",\\"npush\\":" + String(nightPush) +''')
rep("""  } else if (verb == "remclear") {""", """  } else if (verb == "night") {                    // "!night 60": tonight, an hour later
    nightPush = constrain(nightPush + constrain(atoi(rest), 0, 240), 0, 360);
    nightCardUntil = 0;
    char b[24]; snprintf(b, sizeof(b), "SLEEP AT %02d:%02d", ((cfgBed + nightPush) % 1440) / 60, ((cfgBed + nightPush) % 1440) % 60);
    flash(b, 1300);
  } else if (verb == "remclear") {""")

# =================================================================
# NIGHT SLEEP: when, the card, and the deep sleep that ends at dawn
# =================================================================
rep("static void syncBegin() {", r"""// The night ends a quarter of an hour before Fajr (or at 05:00 with no
// prayer times), so the Fajr alert is never slept through.
static int nightEnd() {
  if (prayerOk) { int f = prayerAt(0) - 15; return f < 0 ? f + 1440 : f; }
  return 5 * 60;
}
static bool nightNow(int nowMin) {
  int bed = (cfgBed + nightPush) % 1440, end = nightEnd();
  if (bed == end) return false;
  return bed < end ? (nowMin >= bed && nowMin < end) : (nowMin >= bed || nowMin < end);
}
static long secsToNightEnd() {
  struct tm t; if (!nowLocal(&t)) return -1;
  int now = t.tm_hour * 60 + t.tm_min;
  int d = nightEnd() - now; if (d <= 0) d += 1440;
  return (long)d * 60 - t.tm_sec;
}
static void drawNightCard() {
  oled.clearDisplay();
  titleBarC("NIGHT SLEEP");
  long left = ((long)nightCardUntil - (long)millis()) / 1000 + 1;
  char b[22]; snprintf(b, sizeof(b), "in %ld s", left < 0 ? 0 : left);
  ctr(b, 18, 1);
  ctr("tap: an hour later", 34, 1);
  ctr("hold: sleep now", 46, 1);
  oled.display();
}
// Asleep with the screen dark and nothing going on, at night: a card for
// ten seconds, then deep sleep until just before Fajr.
static void nightTick() {
  static uint32_t lastCheck = 0;
  if (!cfgNight || !timeOk) return;
  uint32_t now = millis();
  struct tm t;
  if (nightCardUntil) {
    if ((int32_t)(now - nightCardUntil) >= 0) { nightCardUntil = 0; nightGo(); }
    return;
  }
  if (now - lastCheck < 15000) return;
  lastCheck = now;
  if (!nowLocal(&t)) return;
  int m = t.tm_hour * 60 + t.tm_min;
  // The push is for tonight: it is forgotten in the hour after the night ends.
  if (nightPush && !nightNow(m) && (m - nightEnd() + 1440) % 1440 < 60) nightPush = 0;
  if (!nightNow(m) || !asleep) return;
  if (tmrOn || awayOn || relaxOn || otaOn || rescueAP || cfgNet != NET_BT || upState != U_OFF) return;
  nightCardUntil = now + 10000;
  wake("night");
}
static void nightGo() {
  nightDeep = true;
  int e = nightEnd();
  oled.clearDisplay();
  ctr("Good night", 20, 1);
  char b[22]; snprintf(b, sizeof(b), "awake at %02d:%02d", e / 60, e % 60);
  ctr(b, 34, 1);
  oled.display();
  delay(1500);
  deepAuto = false;
  goDeep();
}

static void syncBegin() {""")
rep("static void syncBegin();\n", "static void syncBegin();\nstatic void nightTick();\nstatic void nightGo();\nstatic void drawNightCard();\nstatic long secsToNightEnd();\n")
# deep sleep at night wakes at its end (or sooner, for an alert or a reminder)
rep("""  rtcAlarmAt = (secs > 0 && timeOk) ? (uint32_t)time(nullptr) + (uint32_t)secs : 0;""",
    """  if (nightDeep) {                        // 7.7: night sleep ends just before Fajr
    long ne = secsToNightEnd();
    if (ne > 0 && (secs <= 0 || ne < secs)) secs = ne;
    nightDeep = false;
  }
  rtcAlarmAt = (secs > 0 && timeOk) ? (uint32_t)time(nullptr) + (uint32_t)secs : 0;""")
# the card owns the screen and the pad while it is up
rep("""  if (walkUntil && now > walkUntil) walkUntil = 0;""", """  if (nightCardUntil) {
    lastActive = now;
    if (now - lastDraw >= 200) { lastDraw = now; drawNightCard(); }
    delay(2); return;                                  // the card owns the screen
  }
  if (walkUntil && now > walkUntil) walkUntil = 0;""")
rep("""  if (pgUntil)       { pgUntil = 0; return; }
  if (walkUntil)     { walkUntil = 0; return; }     // 7.5: Mac left behind, seen
  if (findUntil)     { findUntil = 0; return; }
  if (popOn && popRinging()) {""",
    """  if (nightCardUntil) {                               // 7.7: tap, an hour later; hold, now
    if (g == TG_ONE) {
      nightPush = constrain(nightPush + 60, 0, 360); nightCardUntil = 0;
      char b[24]; int at = (cfgBed + nightPush) % 1440;
      snprintf(b, sizeof(b), "SLEEP AT %02d:%02d", at / 60, at % 60); flash(b, 1300);
    } else { nightCardUntil = 0; nightGo(); }
    return;
  }
  if (pgUntil)       { pgUntil = 0; return; }
  if (walkUntil)     { walkUntil = 0; return; }     // 7.5: Mac left behind, seen
  if (findUntil)     { findUntil = 0; return; }
  if (popOn && popRinging()) {""", 1)
rep("""  linkTick();                          // 7.3: who stays connected, and who is the phone""",
    """  linkTick();                          // 7.3: who stays connected, and who is the phone
  nightTick();                         // 7.7: bedtime
  idleRhythm();                        // 7.7: a slower link while the screen is dark""")

# =================================================================
# A. THE LINK'S RHYTHM: quick while you use it, slow while it is dark
# =================================================================
rep("static void nightTick();\n", "static void nightTick();\nstatic void idleRhythm();\n")
rep("static int nightEnd() {", r"""// Awake: 90 to 120 ms, may skip 4 (about half a second). Dark: 120 to
// 150 ms, may skip 12 (under two seconds, Apple's limit). The radio then
// wakes roughly once every two seconds instead of twice a second.
static void idleRhythm() {
  static bool was = false;
  bool idle = asleep && !cfgFollow && !otaOn;
  if (idle == was) return;
  was = idle;
  NimBLEServer* sv = NimBLEDevice::getServer();
  if (!sv || !btUp) return;
  for (int i = 0; i < linkN; i++) {
    if (!links[i].authed) continue;
    if (idle) sv->updateConnParams(links[i].h, 96, 120, 12, 600);
    else      sv->updateConnParams(links[i].h, 72, 96, 4, 600);
  }
}
static int nightEnd() {""")
# a Shortcut can push it too: "RAFIQ night later"
rep("""  else if (is("relax") || is("relax on")) {""", """  else if (is("night later") || is("night +1")) {                   // 7.7
    nightPush = constrain(nightPush + 60, 0, 360); nightCardUntil = 0;
    flash("AN HOUR LATER", 1200);
  }
  else if (is("relax") || is("relax on")) {""")

open(SRC, 'w').write(s)
print("7.7 ok")

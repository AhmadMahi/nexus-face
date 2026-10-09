#!/usr/bin/env python3
# Rafiq 6.1.0. Applied after patch_v6 .. patch_v6f.
import re, sys
SRC = sys.argv[1]
s = open(SRC).read()

def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f"anchor matched {n} times, wanted {count}:\n{old[:220]}")
    s = s.replace(old, new)

def func_span(name):
    m = re.search(r'\n(static [^\n;{]*?\b' + re.escape(name) + r'\([^)]*\)\s*\{)', s)
    if not m: sys.exit("no function " + name)
    i = m.end() - 1; d = 0
    for j in range(i, len(s)):
        if s[j] == '{': d += 1
        elif s[j] == '}':
            d -= 1
            if d == 0: return m.start(1), j + 1
def replace_func(name, new):
    global s
    a, b = func_span(name)
    s = s[:a] + new + s[b:]

rep('#define FW_VERSION "6.0.1"', '#define FW_VERSION "6.1.0"')

# =================================================================
# 1. THE NOTIFICATION FIX
#    NimBLEServer::getClient() deletes every service it has discovered
#    on each call. Rafiq called it from setupAncs() and again from
#    readCts() in the same pass, so reading the clock threw away the
#    notification subscriptions the moment they were made, and left
#    ancsCP pointing at freed memory. Nothing ever arrived, and the
#    next write through ancsCP could crash. Once per connection now.
# =================================================================
rep("""static NimBLEClient* btPeer() {
  NimBLEServer* sv = NimBLEDevice::getServer();
  return (sv && btConn != 0xFFFF) ? sv->getClient(btConn) : nullptr;
}""", """//  Once per connection, never more. NimBLEServer::getClient() deletes
//  everything it has discovered every time it is called, so calling it
//  for the clock after the notifications were set up threw the
//  notification subscriptions away (nothing ever arrived) and left
//  ancsCP pointing at freed memory (the occasional reboot). C3 Buddy
//  learned this first; this is its rule. (btCl and btClStale are
//  declared with the 6.0 globals, because the disconnect callback,
//  further up, has to mark it stale.)
static NimBLEClient* btPeer() {
  NimBLEServer* sv = NimBLEDevice::getServer();
  if (!sv || btConn == 0xFFFF) return nullptr;
  if (btClStale || !btCl) { btCl = sv->getClient(btConn); btClStale = false; }
  return btCl;
}""")
rep("""    btConn = 0xFFFF;
    btDropAt = millis();""", """    btConn = 0xFFFF;
    btDropAt = millis();
    btClStale = true;                    // the next link gets a fresh client""")
rep("static void bleOff() {\n", "static void bleOff() {\n  btCl = nullptr; btClStale = true;   // deinit deletes it\n")

# =================================================================
# 2. C3 BUDDY TOUCH GRAMMAR
#    Tap: next, the moment you let go (no waiting to see if a second
#    tap is coming). Hold: a bar says what letting go will do: Open,
#    then Cancel, then Back. Keep holding: Switch off.
# =================================================================
rep("uint32_t sleepArmed   = 0;\n", """uint32_t sleepArmed   = 0;
// 6.1: the hold bar. Times follow C3 Buddy: Open from the hold time
// until 1.5 s after it (at least 2 s), Cancel for a second, then Back,
// then from Back + 2 s a switch-off bar that ends in deep sleep 3 s on.
const uint16_t HOLD_OPTS[] = { 500, 700, 1000, 1500, 2000, 2500 };
#define HOLD_N 6
int  cfgHoldIdx = 1;
bool holdShown  = false;
bool cfg12h     = false;               // 12 hour clock
static uint32_t holdMs()          { return HOLD_OPTS[cfgHoldIdx]; }
static uint32_t holdMaxMs()       { uint32_t a = holdMs() + 1500; return a > 2000 ? a : 2000; }
static uint32_t holdBackMs()      { return holdMaxMs() + 1000; }
static uint32_t holdSleepShowMs() { return holdBackMs() + 2000; }
static uint32_t holdSleepLongMs() { return holdSleepShowMs() + 3000; }
""")

rep_old_touch = re.search(r"        if \(touchOn\) \{\n          touchCount\+\+;.*?      touchGesture\(n == 1 \? TG_ONE : TG_TWO\);\n    \}\n", s, re.S)
if not rep_old_touch: sys.exit("touch block not found")
s = s[:rep_old_touch.start()] + """        if (touchOn) {
          touchCount++;
          touchPressAt = now;
          touchLongDone = false;
          holdShown = false;
          if (asleep) { wake("touch"); touchTaps = 0; touchLongDone = true; }
        } else {
          touchLiftAt = now;
          // What the bar said is what happens. Nothing is decided while
          // the finger is still down, so nothing has to be undone.
          uint32_t held = now - touchPressAt;
          if (!touchLongDone) {
            if      (held < holdMs())          touchGesture(TG_ONE);   // a tap, at once
            else if (held < holdMaxMs())       touchGesture(TG_LONG);  // Open
            else if (held < holdBackMs())      { }                     // Cancel
            else if (held < holdSleepShowMs()) touchGesture(TG_TWO);   // Back
            // past that it was on its way to switching off: let go, nothing
          }
          touchLongDone = false; holdShown = false; touchTaps = 0;
        }
      }
    } else touchEdge = 0;

    if (touchOn && !touchLongDone) {
      uint32_t held = now - touchPressAt;
      if (held > touchLongest) touchLongest = held;
      if (held >= holdMs() && !cfgGesture) holdShown = true;
      if (cfgGesture && held >= TOUCH_HOME_MS) {
        cfgGesture = false;
        touchLongDone = true; holdShown = false;
        flash("GESTURE OFF", 1100);
        Serial.println("held to four in gesture mode: back to being a robot");
      }
      if (!cfgGesture && held >= holdSleepLongMs()) {
        touchLongDone = true; holdShown = false;
        Serial.println("held to the end of the bar: switching off");
        wantDeep = true;
      }
    }
""" + s[rep_old_touch.end():]

a, b = func_span("doubleMeansSomething")       # nothing waits for a second tap any more
s = s[:a] + s[b:]

# the bar itself
rep("static void drawHoldTier(uint32_t now) {", r"""static void drawHoldBar(uint32_t now) {
  uint32_t held = now - touchPressAt;
  oled.clearDisplay();
  const char* lab; const char* hint;
  float f; int tick = -1;
  if (held >= holdSleepShowMs()) {
    lab = "SWITCH OFF"; hint = "keep holding";
    f = (float)(held - holdSleepShowMs()) / (float)(holdSleepLongMs() - holdSleepShowMs());
  } else {
    uint32_t a = holdMs(), b = holdBackMs();
    f = (float)(held - a) / (float)(b - a);
    tick = 6 + (int)(116.0f * (float)(holdMaxMs() - a) / (float)(b - a));
    if (held < holdMaxMs())       { lab = "OPEN";   hint = "let go to open"; }
    else if (held < holdBackMs()) { lab = "CANCEL"; hint = "let go: nothing"; }
    else                          { lab = "BACK";   hint = "let go to go back"; f = 1; }
  }
  if (f < 0) f = 0;
  if (f > 1) f = 1;
  titleBarC(lab);
  oled.drawRoundRect(4, 24, 120, 14, 4, SSD1306_WHITE);
  int w = (int)(116 * f);
  if (w > 2) oled.fillRoundRect(6, 26, w, 10, 3, SSD1306_WHITE);
  if (tick > 0) { oled.drawFastVLine(tick, 20, 3, SSD1306_WHITE); oled.drawFastVLine(tick, 39, 3, SSD1306_WHITE); }
  ctr(hint, 50, 1);
  oled.display();
}
static void drawHoldTier(uint32_t now) {""")

# settings rows: Hold time (Controls), Clock (System)
rep("""       C_GUARD, C_TAMPER, C_TLOG, C_COUNT };""", """       C_GUARD, C_TAMPER, C_TLOG, C_HOLD, C_CLOCK, C_COUNT };""")
rep("""    "Phone guard", "Tamper alarm", "Tamper log" };""", """    "Phone guard", "Tamper alarm", "Tamper log", "Hold time", "Clock" };""")
rep("""  { C_KNOCK,  C_TAP,     C_SHAKE, C_WAKEH,  C_ACCEL,  SG_END,   SG_END,  SG_END },""",
    """  { C_HOLD,   C_KNOCK,   C_TAP,   C_SHAKE,  C_WAKEH,  C_ACCEL,  SG_END,  SG_END },""")
rep("""  { C_DEEP,   C_BATT,    C_TAMPER, C_TLOG, C_RESET,  C_REBOOT, C_ABOUT, SG_END },""",
    """  { C_CLOCK,  C_DEEP,    C_BATT,  C_TAMPER, C_TLOG,   C_RESET,  C_REBOOT, C_ABOUT },""")
rep("""      case C_TLOG:   snprintf(v, sizeof(v), "hold"); break;""",
    """      case C_TLOG:   snprintf(v, sizeof(v), "hold"); break;
      case C_HOLD:   snprintf(v, sizeof(v), "%u.%u s", HOLD_OPTS[cfgHoldIdx] / 1000, (HOLD_OPTS[cfgHoldIdx] % 1000) / 100); break;
      case C_CLOCK:  snprintf(v, sizeof(v), "%s", cfg12h ? "12 hour" : "24 hour"); break;""")
rep("""      case C_TLOG:   tlogLoad(); depth = 2; break;""", """      case C_TLOG:   tlogLoad(); depth = 2; break;
      case C_HOLD:
        cfgHoldIdx = (cfgHoldIdx + 1) % HOLD_N; prefs.putInt("holdi", cfgHoldIdx);
        break;
      case C_CLOCK:
        cfg12h = !cfg12h; prefs.putBool("h12", cfg12h);
        break;""")
rep("""  cfgPGuard  = prefs.getBool("guard", false);""", """  cfgPGuard  = prefs.getBool("guard", false);
  cfgHoldIdx = constrain(prefs.getInt("holdi", 1), 0, HOLD_N - 1);
  cfg12h     = prefs.getBool("h12", false);""")

# =================================================================
# 3. 12 HOUR CLOCK
# =================================================================
rep("""  snprintf(fb.hm, sizeof(fb.hm), "%02d:%02d", t.tm_hour, t.tm_min);
  snprintf(fb.hh, sizeof(fb.hh), "%02d", t.tm_hour);""", """  if (cfg12h) {
    int h12 = t.tm_hour % 12; if (!h12) h12 = 12;
    snprintf(fb.hm, sizeof(fb.hm), "%d:%02d", h12, t.tm_min);
    snprintf(fb.hh, sizeof(fb.hh), "%d", h12);
  } else {
    snprintf(fb.hm, sizeof(fb.hm), "%02d:%02d", t.tm_hour, t.tm_min);
    snprintf(fb.hh, sizeof(fb.hh), "%02d", t.tm_hour);
  }""")
rep("""  if (sec) snprintf(o, n, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  else     snprintf(o, n, "%02d:%02d", t.tm_hour, t.tm_min);""", """  int h = t.tm_hour;
  if (cfg12h) { h %= 12; if (!h) h = 12; }
  if (sec) snprintf(o, n, cfg12h ? "%d:%02d:%02d" : "%02d:%02d:%02d", h, t.tm_min, t.tm_sec);
  else     snprintf(o, n, cfg12h ? "%d:%02d" : "%02d:%02d", h, t.tm_min);""")

# =================================================================
# 4. SIGNAL: on Bluetooth, the phone link's strength
# =================================================================
rep("static int sigBars() {\n  if (!online()) return 0;\n  int r = (int)WiFi.RSSI();",
    """// WiFi's signal on WiFi; on Bluetooth, the phone link's.
static bool linkRssi(int& r) {
  if (online()) { r = (int)WiFi.RSSI(); return true; }
  if (cfgNet == NET_BT && btUp && btConn != 0xFFFF) {
    int8_t v = 0;
    if (ble_gap_conn_rssi(btConn, &v) == 0 && v < 0) { r = v; return true; }
  }
  return false;
}
static int sigBars() {
  int r;
  if (!linkRssi(r)) return 0;""")
rep("""  if (online()) snprintf(r, sizeof(r), "%ddBm", (int)WiFi.RSSI());
  else          snprintf(r, sizeof(r), "no wifi");""", """  { int q;
    if (linkRssi(q)) snprintf(r, sizeof(r), "%ddBm", q);
    else             snprintf(r, sizeof(r), cfgNet == NET_BT ? "no phone" : "no wifi"); }""")
rep("""  if (online()) snprintf(l[5], 14, "rssi %d", (int)WiFi.RSSI());
  else          snprintf(l[5], 14, "offline");""", """  { int q;
    if (linkRssi(q)) snprintf(l[5], 14, "%s %d", online() ? "rssi" : "bt", q);
    else             snprintf(l[5], 14, "offline"); }""")
rep("""  } else {
    snprintf(l[2], 24, "net   none");
    snprintf(l[3], 24, "rssi  --");""", """  } else {
    int q;
    snprintf(l[2], 24, "net   %s", cfgNet == NET_BT ? "bluetooth" : "none");
    if (linkRssi(q)) snprintf(l[3], 24, "rssi  %d dBm", q);
    else             snprintf(l[3], 24, "rssi  --");""")
rep("""  else if (cfgNet == NET_BT) snprintf(v, sizeof(v), "bt %s", btShort());""",
    """  else if (cfgNet == NET_BT) {
    int q;
    if (linkRssi(q)) snprintf(v, sizeof(v), "bt %d dBm", q);
    else             snprintf(v, sizeof(v), "bt %s", btShort());
  }""")

# =================================================================
# 5. SCREENS: Notifications second, Focus gone
# =================================================================
rep("""  { "HOME", "VEHICLE", "REMINDERS", "FOCUS", "WEATHER", "NOTICES", "PRAYER",""",
    """  { "HOME", "VEHICLE", "REMINDERS", "FOCUS", "WEATHER", "NOTIFICATIONS", "PRAYER",""")
rep("""static bool screenOn_(int s) {
  if (s == S_BIKE)    return cfgBike;""", """static bool screenOn_(int s) {
  if (s == S_FOCUS)   return false;                   // 6.1: gone from the round
  if (s == S_BIKE)    return cfgBike;""")
rep("""static int nextScreen(int from) {
  for (int i = 1; i <= S_COUNT; i++) {
    int s = (from + i) % S_COUNT;
    if (screenOn_(s)) return s;
  }
  return S_HOME;
}""", """// The round, in order. Notifications come straight after home: they
// are the thing most often looked for. The enum keeps its numbers for
// the page and the Mac; only the walk changes.
const int S_ORDER[] = { S_HOME, S_MSG, S_REMIND, S_BIKE, S_WEATHER, S_PRAYER,
                        S_FAITH, S_READS, S_GAMES, S_SETTINGS, S_SYSTEM, S_FOCUS };
#define S_ORDER_N (int)(sizeof(S_ORDER) / sizeof(S_ORDER[0]))
static int nextScreen(int from) {
  int at = 0;
  for (int i = 0; i < S_ORDER_N; i++) if (S_ORDER[i] == from) { at = i; break; }
  for (int i = 1; i <= S_ORDER_N; i++) {
    int s = S_ORDER[(at + i) % S_ORDER_N];
    if (screenOn_(s)) return s;
  }
  return S_HOME;
}""")
# RAFIQ focus commands go
rep("""  else if (is("focus stop") || is("focus off")) { stopSession(); flash("FOCUS STOPPED", 1000); }
  else if (pre("focus")) { int m = atoi(c + 5); focusBegin(m > 0 ? constrain(m, 1, 240) : 25); }
""", "")

# =================================================================
# 6. POPUPS AND THE NOTIFICATIONS INBOX (C3 Buddy)
# =================================================================
rep("volatile uint8_t btNews = 0;", """volatile uint8_t btNews = 0;
// 6.1 popups: drawn over wherever you are. A tap closes it and you are
// exactly where you were; a hold opens it in Notifications. If it woke
// the robot, closing it puts the robot back to sleep.
bool     popOn = false, popWoke = false;
uint32_t popUntil = 0;
bool     notesDirty = false;
uint32_t notesSavedAt = 0;
int      noteSel = 0;
static void popupShow();
static void popupClose(bool open);
static void saveNotes();
NimBLEClient* btCl = nullptr;          // the phone, as a client: fetched once per link
volatile bool btClStale = true;""")

rep("""      addNote(noteStage);
      if (!cfgQuiet) {
        wake("notification");
        if (popupSecs()) {
          screen = S_MSG; depth = 0;
          popupUntil = millis() + popupSecs() * 1000UL;
        }
      }""", """      addNote(noteStage);
      if (!cfgQuiet) popupShow();""")

rep("static void syncBegin() {", r"""static void popupShow() {
  if (!noteN || !popupSecs()) return;
  // Never over a game being played, an update being asked, or the call
  // to prayer. It still lands in the list.
  if ((screen == S_GAMES && gState == GS_PLAY) || upState != U_OFF || alertPhase != AL_NONE) return;
  popWoke = asleep || popWoke;
  wake("notification");
  popOn = true;
  popUntil = millis() + popupSecs() * 1000UL;
}
static void popupClose(bool open) {
  popOn = false;
  if (open) {
    notes[0].unread = false; notesDirty = true;
    screen = S_MSG; depth = 2; noteIdx = 0; noteSel = 0;
    popWoke = false;
    return;
  }
  if (popWoke) { popWoke = false; goSleep(); }
}
static void drawPopup() {
  oled.clearDisplay();
  if (!noteN) { popOn = false; return; }
  const Note& n = notes[0];
  char r[12]; snprintf(r, sizeof(r), "%d", noteUnread());
  titleBar(appShort(n), r);
  if (noteIsCall(n)) {
    ctr(n.cat == CAT_MISSED ? "Missed call" : n.cat == CAT_VOICE ? "Voicemail" : "Calling", 18, 1);
    marquee(n.title[0] ? n.title : "unknown", 32, 1);
  } else {
    if (n.title[0]) marquee(n.title, 15, 1);
    fitText(n.msg[0] ? n.msg : "(no text)", 27, 46, n.at);
  }
  oled.drawFastHLine(0, 54, SCRW, SSD1306_WHITE);
  ctr("tap close  hold open", 56, 1);
  oled.display();
}

// The list is kept in flash: on Bluetooth deep sleep is every time the
// phone is away, and a list that emptied every time would be no list.
// Times are kept as an age, so they survive the clock starting again.
#define NOTES_PATH "/notes.bin"
static void saveNotes() {
  notesDirty = false; notesSavedAt = millis();
  if (!fsOk) return;
  File f = LittleFS.open(NOTES_PATH, "w");
  if (!f) return;
  uint32_t hdr[3] = { 0x52464E31u, (uint32_t)noteN, (uint32_t)time(nullptr) };
  f.write((const uint8_t*)hdr, sizeof(hdr));
  uint32_t now = millis();
  for (int i = 0; i < noteN; i++) {
    Note c = notes[i];
    c.at = now - notes[i].at;                    // age in ms
    f.write((const uint8_t*)&c, sizeof(c));
  }
  f.close();
}
static void loadNotes() {
  if (!fsOk) return;
  File f = LittleFS.open(NOTES_PATH, "r");
  if (!f) return;
  uint32_t hdr[3];
  if (f.read((uint8_t*)hdr, sizeof(hdr)) == sizeof(hdr) && hdr[0] == 0x52464E31u) {
    uint32_t tnow = (uint32_t)time(nullptr);
    uint32_t gone = tnow >= hdr[2] ? (tnow - hdr[2]) : 0;
    if (gone > 30UL * 86400UL) gone = 30UL * 86400UL;
    int n = (int)hdr[1];
    noteN = 0;
    for (int i = 0; i < n && i < NOTE_MAX; i++) {
      Note c;
      if (f.read((uint8_t*)&c, sizeof(c)) != sizeof(c)) break;
      c.at = millis() - c.at - gone * 1000UL;    // back to this boot's millis
      notes[noteN++] = c;
    }
  }
  f.close();
}

static void syncBegin() {""")

# addNote marks the list for saving
rep("static void addNote(const Note& n) {\n", "static void addNote(const Note& n) {\n  notesDirty = true;\n")

# the old popup paths (page message, RAFIQ msg) use the new popup
rep("""      wake("message");
      if (popupSecs()) { screen = S_MSG; depth = 0; popupUntil = millis() + popupSecs() * 1000UL; }""",
    """      popupShow();""")
rep("""    addNote(pn);
    wake("message");
    screen = S_MSG; depth = 0;
    popupUntil = millis() + (unsigned long)max(popupSecs(), 10) * 1000UL;
    return;""", """    addNote(pn);
    if (popupSecs()) popupShow();
    else { wake("message"); screen = S_MSG; depth = 2; noteIdx = 0; }
    return;""")

# the inbox screen
replace_func("drawMessage", r"""static const char* noteRow(int i, char* b, size_t n) {
  if (i >= noteN) { snprintf(b, n, "Clear all"); return b; }
  const Note& x = notes[i];
  snprintf(b, n, "%c%s: %s", x.unread ? '*' : ' ', appShort(x),
           x.title[0] ? x.title : x.msg);
  return b;
}
static void drawMessage() {
  oled.clearDisplay();

  if (depth == 0) {
    bar("NOTIFICATIONS");
    bellIcon(SCRW / 2, 31, 11);
    char l[26];
    int un = noteUnread();
    if (!noteN)  snprintf(l, sizeof(l), "Nothing yet");
    else if (un) snprintf(l, sizeof(l), "%d new of %d", un, noteN);
    else         snprintf(l, sizeof(l), "%d kept", noteN);
    ctr(l, 45, 1);
    ctr(noteN ? "Hold to open"
              : cfgNet != NET_BT        ? "needs bluetooth"
              : ancsState == ANCS_READY ? "from your phone"
              : ancsState == ANCS_FAIL  ? "allow it on the phone"
                                        : "linking up", 56, 1);
    oled.display();
    return;
  }

  if (depth == 1) {
    if (noteSel > noteN) noteSel = noteN;
    char r[12]; snprintf(r, sizeof(r), "%d", noteN);
    drawList("NOTIFICATIONS", r, noteN + 1, noteSel, noteRow);
    return;
  }

  // depth 2: one of them, read in full
  if (!noteN) { depth = 0; return; }
  if (noteIdx >= noteN) noteIdx = noteN - 1;
  Note& n = notes[noteIdx];
  char when[8];
  if (timeOk) {
    uint32_t ago = (millis() - n.at) / 1000UL;
    time_t   at  = time(nullptr) - (time_t)ago;
    struct tm* lt = localtime(&at);
    if (lt) {
      int h = lt->tm_hour;
      if (cfg12h) { h %= 12; if (!h) h = 12; }
      snprintf(when, sizeof(when), cfg12h ? "%d:%02d" : "%02d:%02d", h, lt->tm_min);
    } else snprintf(when, sizeof(when), "--:--");
  } else {
    snprintf(when, sizeof(when), "--:--");
  }
  titleBar(appShort(n), when);
  if (noteIsCall(n)) {
    ctr(n.cat == CAT_MISSED ? "Missed call"
      : n.cat == CAT_VOICE  ? "Voicemail" : "Calling", 20, 1);
    marquee(n.title[0] ? n.title : "unknown", 34, 1);
  } else {
    if (n.title[0]) marquee(n.title, 15, 1);
    fitText(n.msg[0] ? n.msg : "(no text)", 27, 50, n.at);
  }
  char foot[24];
  snprintf(foot, sizeof(foot), "%d/%d  hold clears", noteIdx + 1, noteN);
  ctr(foot, 56, 1);
  oled.display();
}""")

old_in = re.search(r"  // Notices read the way the reminders do.*?  if \(g == TG_LONG && screen == S_MSG && depth == 0 && noteN\) \{\n    depth = 1; noteIdx = 0; noteConfirm = false;\n    clickShrink\(\);\n    return;\n  \}\n", s, re.S)
if not old_in: sys.exit("old notices input not found")
s = s[:old_in.start()] + """  // Notifications, C3 Buddy's way. The list: a tap moves, a hold opens
  // the one picked, and holding on the last row clears them all. A
  // message: a tap goes to the next, a hold clears this one, back
  // returns to the list.
  if (screen == S_MSG && depth == 1) {
    switch (g) {
      case TG_ONE:  noteSel = (noteSel + 1) % (noteN + 1); return;
      case TG_LONG:
        if (noteSel >= noteN) {
          if (noteN) { noteN = 0; noteIdx = 0; noteSel = 0; notesDirty = true; flash("CLEARED", 1100); }
          depth = 0;
        } else {
          noteIdx = noteSel; notes[noteIdx].unread = false; notesDirty = true; depth = 2;
        }
        return;
      case TG_TWO:  depth = 0; return;
      default: return;
    }
  }
  if (screen == S_MSG && depth == 2) {
    switch (g) {
      case TG_ONE:
        if (noteN) { noteIdx = (noteIdx + 1) % noteN; notes[noteIdx].unread = false; notesDirty = true; }
        return;
      case TG_LONG:
        if (noteIdx < noteN) {
          Note& n = notes[noteIdx];
          if (!noteIsCall(n)) ancsAction(n.uid, 1);   // clear it on the phone too
          for (int i = noteIdx; i < noteN - 1; i++) notes[i] = notes[i + 1];
          noteN--; notesDirty = true;
          noteSel = noteIdx < noteN ? noteIdx : noteN;
          flash("CLEARED", 700);
        }
        depth = noteN ? 1 : 0;
        return;
      case TG_TWO:  depth = 1; noteSel = noteIdx; return;
      default: return;
    }
  }
  if (g == TG_LONG && screen == S_MSG && depth == 0 && noteN) {
    depth = 1; noteSel = 0;
    clickShrink();
    return;
  }
""" + s[old_in.end():]

# popup takes presses and knocks first, after the 6.0 cards
rep("""  if (findUntil)     { findUntil = 0; return; }
  if (screen == S_SETTINGS && depth == 2 && itemIdx == C_TLOG && g == TG_ONE) {""", """  if (findUntil)     { findUntil = 0; return; }
  if (popOn)         { popupClose(g == TG_LONG); return; }
  if (screen == S_SETTINGS && depth == 2 && itemIdx == C_TLOG && g == TG_ONE) {""")
rep("""static void knockOne() {
  cTap++;
  // The new cards from 6.0 take a press before anything else: it
  // cancels a countdown, quiets the guard, stops the finder.
  if (tamperCountAt) { tamperCountAt = 0; flash("NOT ARMED", 1200); return; }
  if (pgUntil)       { pgUntil = 0; return; }
  if (findUntil)     { findUntil = 0; return; }
""", """static void knockOne() {
  cTap++;
  // The new cards from 6.0 take a press before anything else: it
  // cancels a countdown, quiets the guard, stops the finder.
  if (tamperCountAt) { tamperCountAt = 0; flash("NOT ARMED", 1200); return; }
  if (pgUntil)       { pgUntil = 0; return; }
  if (findUntil)     { findUntil = 0; return; }
  if (popOn)         { popupClose(false); return; }
""")

# loop: the hold bar and the popup are drawn above everything but the call to prayer
rep("""  // 6.0's cards, below the call to prayer and above everything else.
  if (tamperCountAt) {""", """  // 6.1: the hold bar, while a finger is on the pad past the hold time.
  // Not inside a game being played: games keep their own feel.
  if (holdShown && touchOn && !(screen == S_GAMES && gState == GS_PLAY)) {
    lastActive = now;
    if (now - lastDraw >= 33) { lastDraw = now; drawHoldBar(now); }
    delay(2); return;
  }
  if (popOn && now > popUntil) popupClose(false);
  if (popOn) {
    lastActive = now;
    if (now - lastDraw >= 100) { lastDraw = now; drawPopup(); }
    delay(2); return;
  }
  if (notesDirty && now - notesSavedAt > 3000) saveNotes();

  // 6.0's cards, below the call to prayer and above everything else.
  if (tamperCountAt) {""")
rep("static void goDeep() {\n", "static void goDeep() {\n  if (notesDirty) saveNotes();         // the list survives the sleep\n")
rep("""  loadWx();
  loadRems();""", """  loadWx();
  loadRems();""")
rep("""  if (tamperWas) { prefs.putBool("tamper", false); tlogAdd("Disarmed"); }""",
    """  if (tamperWas) { prefs.putBool("tamper", false); tlogAdd("Disarmed"); }
  loadNotes();""")

# faster wakes: the eyes open quickly for a touch, and not at all for news
rep("""  if (!cfgGesture)
    for (int i = 0; i < 18; i++) { eyesFrame(); delay(16); }""", """  // 6.1: a short opening for a touch, none for a notification or a
  // card, so news is on the screen the moment it arrives.
  bool news = !strcmp(why, "notification") || !strcmp(why, "shortcut") || !strcmp(why, "guard") ||
              !strcmp(why, "update") || !strcmp(why, "sync") || !strcmp(why, "message");
  if (!cfgGesture && !news)
    for (int i = 0; i < 8; i++) { eyesFrame(); delay(16); }""")

# =================================================================
# 7. UPDATE FROM A FILE, ON THE MAIN PAGE
# =================================================================
rep("""  <h2>Update</h2><div class="card">
    <button onclick="if(confirm('Install the newest release?'))act('/api/update')">Install the newest release</button>
    <button class="g" onclick="listRel()">List earlier releases</button>
    <div id="rel"></div>
  </div>""", """  <h2>Update</h2><div class="card">
    <div class="row"><button id="ub1" onclick="updTab(0)">From GitHub</button><button class="g" id="ub2" onclick="updTab(1)">From a file</button></div>
    <div id="upg">
      <button onclick="if(confirm('Install the newest release?'))act('/api/update')">Install the newest release</button>
      <button class="g" onclick="listRel()">List earlier releases</button>
      <div id="rel"></div>
    </div>
    <div id="upf" style="display:none">
      <p style="margin:10px 0 6px">Pick the <b>APP</b> bin (Rafiq_vX_APP_wireless_update.bin), not the FULL one. Keep this page open until it says it is restarting.</p>
      <input type="file" id="upfile" accept=".bin" style="width:100%">
      <button onclick="upFile()">Install this file</button>
      <div id="ups" style="margin-top:8px"></div>
    </div>
  </div>""")
rep("window.hdr=function(){", """window.updTab=function(f){document.getElementById('upg').style.display=f?'none':'';document.getElementById('upf').style.display=f?'':'none';document.getElementById('ub1').className=f?'g':'';document.getElementById('ub2').className=f?'':'g'};
window.upFile=function(){const x=document.getElementById('upfile').files[0],st=document.getElementById('ups');if(!x){st.textContent='Pick a file first';return}const d=new FormData();d.append('f',x,x.name);const r=new XMLHttpRequest();r.open('POST','/ota');const t=tok();if(t)r.setRequestHeader('X-Rafiq-Token',t);r.upload.onprogress=function(e){if(e.lengthComputable)st.textContent='Sending '+Math.round(e.loaded*100/e.total)+'%'};r.onload=function(){st.textContent=r.responseText};r.onerror=function(){st.textContent='The connection dropped. If Rafiq restarted, it worked.'};st.textContent='Sending';r.send(d)};
window.hdr=function(){""")
# the separate /ota page goes; the upload address stays for the page above
rep("""  web.on("/ota", HTTP_GET, []() { wsTouch(); web.send_P(200, "text/html; charset=utf-8", OTA_PAGE); });\n""", "")
m = re.search(r"const char OTA_PAGE\[\] PROGMEM = R\"HTML\(.*?\)HTML\";\n", s, re.S)
if not m: sys.exit("OTA_PAGE not found")
s = s[:m.start()] + s[m.end():]
rep("""  toastText = String(RESCUE_SSID) + " " + RESCUE_PASS + "\\n192.168.4.1/ota";""",
    """  toastText = String(RESCUE_SSID) + " " + RESCUE_PASS + "\\n192.168.4.1 > Update";""")

open(SRC, 'w').write(s)
print("6.1 ok")

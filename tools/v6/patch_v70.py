#!/usr/bin/env python3
# Rafiq 7.0.0: relax for three minutes, zikr, a locked timer, and a
# contact card in Away. Applied after patch_v69.
import sys
SRC = sys.argv[1]
s = open(SRC).read()
def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count: sys.exit(f"anchor {n}x:\n{old[:220]}")
    s = s.replace(old, new)

rep('#define FW_VERSION "6.9.0"', '#define FW_VERSION "7.0.0"')

# ---------------------------------------------------------------- globals
rep("uint32_t popGlanceMs = 1000;", """uint32_t popGlanceMs = 1000;
// ---- 7.0 ----
// Who to get in touch with, shown in Away between showings of the
// message. Kept in the firmware at the owner's request: leave these
// out of anything pushed to a public repository.
#define OWNER_NAME  "Ahmed"
#define OWNER_PHONE "+918660027729"
#define OWNER_MAIL  "mahiahmad53@gmail.com"
// Relax from a Shortcut lasts this long, then the robot sleeps.
#define RELAX_RQ_MS 180000UL
uint32_t relaxUntil = 0;
// The timer. While it runs, nothing else works: a touch only keeps the
// screen on (or lets it go back to its rhythm), RAFIQ timer changes it
// and RAFIQ home stops it. Done, it flashes until touched.
bool     tmrOn = false, tmrDone = false, tmrPinned = false;
uint32_t tmrStart = 0, tmrEnd = 0, tmrDoneAt = 0;""")

# ---------------------------------------------------------------- relax
rep("""  else if (is("relax") || is("relax on")) { relaxOn = true; relaxKind = 0; relaxNext = now + 30000UL; }
  else if (is("relax off")) relaxOn = false;""",
    """  else if (is("relax") || is("relax on")) {
    relaxOn = true; relaxKind = 0; relaxNext = now + 30000UL;
    relaxUntil = now + RELAX_RQ_MS;    // three minutes, then sleep
  }
  else if (is("zikr") || is("dhikr") || is("tasbih")) {
    screen = S_FAITH; depth = 2; itemIdx = F_ZIKR; subIdx = 0;
    zikrReset();
  }""")
rep("""  if (relaxOn) {
    lastActive = now;
    if ((long)(now - relaxNext) >= 0) { relaxKind = (relaxKind + 1) % 3; relaxNext = now + 30000UL; }""",
    """  if (relaxOn && relaxUntil && (long)(now - relaxUntil) >= 0) {
    relaxOn = false; relaxUntil = 0;
    goSleep();
    return;
  }
  if (relaxOn) {
    lastActive = now;
    if ((long)(now - relaxNext) >= 0) { relaxKind = (relaxKind + 1) % 3; relaxNext = now + 30000UL; }""")
rep("""  if (relaxOn)     { relaxOn = false;    return; }""",
    """  if (relaxOn)     { relaxOn = false; relaxUntil = 0; return; }""")

# ---------------------------------------------------------------- timer
rep("static void syncBegin() {", r"""static void tmrSet(long secs) {
  uint32_t now = millis();
  if (secs <= 0) {                               // taken to nothing: it is done
    tmrOn = true; tmrDone = true; tmrDoneAt = now; tmrEnd = now;
    wake("timer");
    return;
  }
  tmrOn = true; tmrDone = false; tmrPinned = false;
  tmrStart = now; tmrEnd = now + (uint32_t)secs * 1000UL;
  popOn = false;
  wake("timer");
  Serial.printf("timer: %ld s\n", secs);
}
static long tmrLeft() {                          // seconds, never below zero
  long ms = (long)(tmrEnd - millis());
  return ms > 0 ? (ms + 999) / 1000 : 0;
}
static void tmrStop(const char* why) {
  tmrOn = false; tmrDone = false; tmrPinned = false;
  screen = S_HOME; depth = 0;
  wake("timer");
  if (why) flash(why, 1100);
}
// "timer 15", "timer +5", "timer -10". Minutes.
static bool tmrCommand(const char* c) {
  if (strncmp(c, "timer", 5) || (c[5] && c[5] != ' ')) return false;
  const char* q = c + 5;
  while (*q == ' ') q++;
  if (!*q) return false;
  char sign = (*q == '+' || *q == '-') ? *q : 0;
  if (sign) q++;
  while (*q == ' ') q++;
  if (!isdigit((unsigned char)*q)) return false;
  long m = atol(q);
  if (m > 600) m = 600;                          // ten hours is plenty
  if (!sign) { tmrSet(m * 60); return true; }
  long left = (tmrOn && !tmrDone) ? tmrLeft() : 0;
  tmrSet(sign == '+' ? left + m * 60 : left - m * 60);
  return true;
}
// The rhythm: over five minutes left, five seconds lit each minute;
// one to five minutes, five on and fifteen off; the last minute, always.
// A touch pins it on; another lets it go back to the rhythm.
static bool tmrWantsScreen() {
  if (tmrDone || tmrPinned) return true;
  long left = tmrLeft();
  if (left <= 60) return true;
  uint32_t t = (millis() - tmrStart) / 1000;
  if (left <= 300) return (t % 20) < 5;
  return (t % 60) < 5;
}
static void tmrTouch() {
  if (tmrDone) { tmrStop(nullptr); return; }     // the flash is seen: done
  tmrPinned = !tmrPinned;
}
static void tmrTick() {
  if (!tmrOn) return;
  uint32_t now = millis();
  if (!tmrDone && (long)(now - tmrEnd) >= 0) {
    tmrDone = true; tmrDoneAt = now;
    Serial.println("timer: done");
  }
  bool want = tmrWantsScreen();
  if (want && asleep)   wake("timer");
  if (!want && !asleep) goSleepQuick();
}
static void drawTimer() {
  oled.clearDisplay();
  if (tmrDone) {
    bool b = (millis() / 350) % 2;
    if (b) oled.fillRect(0, 0, SCRW, SCRH, SSD1306_WHITE);
    oled.setTextColor(b ? SSD1306_BLACK : SSD1306_WHITE);
    ctr("TIME'S UP", 18, 2);
    ctr("touch to stop", 46, 1);
    oled.setTextColor(SSD1306_WHITE);
    oled.display();
    return;
  }
  long left = tmrLeft();
  char t[12];
  if (left >= 3600) snprintf(t, sizeof(t), "%ld:%02ld", left / 3600, (left / 60) % 60);
  else              snprintf(t, sizeof(t), "%02ld:%02ld", left / 60, left % 60);
  titleBar("TIMER", tmrPinned ? "on" : "");
  ctr(t, 20, 4);
  long total = (long)((tmrEnd - tmrStart) / 1000);
  int w = total > 0 ? (int)(120L * (total - left) / total) : 120;
  oled.drawRoundRect(4, 56, 120, 6, 2, SSD1306_WHITE);
  if (w > 2) oled.fillRoundRect(4, 56, w, 6, 2, SSD1306_WHITE);
  oled.display();
}

static void syncBegin() {""")
rep("static void syncBegin();\n", "static void syncBegin();\nstatic void tmrTick();\nstatic void tmrTouch();\nstatic void drawTimer();\nstatic bool tmrCommand(const char* c);\nstatic void tmrStop(const char* why);\n")

# a running timer: only timer and home are heard
rep("""  if (awayOn) {
    char fc[16]; strcpy(fc, first);                // "Home." and "home!" count too""",
    """  if (tmrOn && !awayOn) {
    if (!fresh) return;
    char ln[48]; int k = 0;
    for (const char* q = p; *q && *q != '\\n' && k < 47; q++) {
      char ch = tolower((unsigned char)*q);
      if (ch == '.' || ch == '!' || ch == '\\r') continue;
      ln[k++] = ch;
    }
    ln[k] = 0;
    while (k && ln[k - 1] == ' ') ln[--k] = 0;
    if (!strcmp(ln, "home")) tmrStop("TIMER STOPPED");
    else tmrCommand(ln);
    return;
  }
  if (awayOn) {
    char fc[16]; strcpy(fc, first);                // "Home." and "home!" count too""")
# a timer starts from any state (but not in Away)
rep("""  if (is("sync") || is("sync now")) rqPend = RQ_SYNC;""",
    """  if (tmrCommand(c)) { }
  else if (is("sync") || is("sync now")) rqPend = RQ_SYNC;""")

# loop: the rhythm runs asleep too; the screen when awake
rep("""  // 6.4: these run asleep as well as awake.
  awayFlush(false);""", """  // 6.4: these run asleep as well as awake.
  tmrTick();
  awayFlush(false);""")
rep("""  if (awayOn && !asleep) {
    if ((int32_t)(millis() - lastActive) > (int32_t)awayShowMs) {""",
    """  if (tmrOn && !awayOn && !asleep) {
    lastActive = now;
    if (now - lastDraw >= (tmrDone ? 120UL : 250UL)) { lastDraw = now; drawTimer(); }
    delay(5); return;
  }
  if (awayOn && !asleep) {
    if ((int32_t)(millis() - lastActive) > (int32_t)awayShowMs) {""")
# no deep sleep while a timer runs
rep("""      (now - since) > wait && upState == U_OFF && !storyBusy && !awayOn && !checkWake) {""",
    """      (now - since) > wait && upState == U_OFF && !storyBusy && !awayOn && !checkWake && !tmrOn) {""")
# touches: a timer takes every press, even the one that lights the screen
rep("""          glanceUntil = 0;                       // a touch makes a glance a real wake
          awayEv(AE_TOUCH);""", """          glanceUntil = 0;                       // a touch makes a glance a real wake
          awayEv(AE_TOUCH);
          if (tmrOn && !awayOn) { tmrTouch(); touchLongDone = true; touchTaps = 0; holdShown = false; }""")
GUARD = "  if (tmrOn && !awayOn) { lastActive = millis(); return; }   // timer: nothing else\n"
rep("""  if (awayOn) { lastActive = millis(); return; }   // away: nothing but the message
""", """  if (awayOn) { lastActive = millis(); return; }   // away: nothing but the message
""" + GUARD, count=5)
rep("""static void popupShow() {
  if (awayOn) return;                  // still kept in the list""",
    """static void popupShow() {
  if (awayOn || tmrOn) return;         // still kept in the list""")
rep("""  return holdShown && touchOn && !sleepArmed && !cfgGesture && !gamePlaying() && !awayOn;""",
    """  return holdShown && touchOn && !sleepArmed && !cfgGesture && !gamePlaying() && !awayOn && !tmrOn;""")
rep("""      if (!cfgGesture && !sleepArmed && !awayOn && held >= TOUCH_HOME_MS) {""",
    """      if (!cfgGesture && !sleepArmed && !awayOn && !tmrOn && held >= TOUCH_HOME_MS) {""")
# timer lights the screen without eyes
rep("""  bool news = glance || !strcmp(why, "notification") || !strcmp(why, "shortcut") || !strcmp(why, "guard") ||""",
    """  bool news = glance || !strcmp(why, "notification") || !strcmp(why, "shortcut") || !strcmp(why, "guard") ||
              !strcmp(why, "timer") ||""")

# ---------------------------------------------------------------- away: message, then who to contact
rep("""static void drawAway() {
  oled.clearDisplay();
  awayText1(awayText.length() ? awayText.c_str() : "Away");""",
"""// Who to get in touch with: the second half of every showing.
static void drawContact() {
  oled.clearDisplay();
  titleBarC("GET IN TOUCH");
  ctr(OWNER_NAME, 16, 2);
  ctr(OWNER_PHONE, 38, 1);
  ctr(OWNER_MAIL, 50, 1);
  oled.display();
}
static void drawAway() {
  // The first half of the time it is lit, the message; the second
  // half, who to get in touch with.
  if ((int32_t)(millis() - lastActive) >= (int32_t)(awayShowMs / 2)) { drawContact(); return; }
  oled.clearDisplay();
  awayText1(awayText.length() ? awayText.c_str() : "Away");""")
rep("""    if (now - lastDraw >= 500) { lastDraw = now; drawAway(); }""",
    """    if (now - lastDraw >= 100) { lastDraw = now; drawAway(); }""")
rep("""    oled.display();
    delay(AWAY_DEEP_SHOW_MS);
    awayEyes(false);""", """    oled.display();
    delay(AWAY_DEEP_SHOW_MS / 2);
    drawContact();
    delay(AWAY_DEEP_SHOW_MS / 2);
    awayEyes(false);""")
rep("static void awayText1(const char* t);\n", "static void awayText1(const char* t);\nstatic void drawContact();\n")

open(SRC, 'w').write(s)
print("7.0 ok")

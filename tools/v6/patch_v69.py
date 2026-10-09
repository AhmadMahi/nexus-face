# Rafiq 6.9.0: Away shows only your words, in normal text; a restart
# goes straight to eyes and message; a touch in Away's sleep shows it
# at once. The start-up card describes the controls as they are now.
import sys
SRC=sys.argv[1]; s=open(SRC).read()
def rep(old,new,count=1):
    global s
    n=s.count(old)
    if n!=count: sys.exit(f"anchor {n}x:\n{old[:200]}")
    s=s.replace(old,new)
rep('#define FW_VERSION "6.8.0"', '#define FW_VERSION "6.9.0"')

# 1. "away message: ..." keeps only what comes after
rep("""    const char* t = p + n;
    while (*t == ':' || *t == ' ') t++;
    String m = String(t); m.replace("\\r", ""); m.trim();
    awaySet(true, m.c_str());""", """    const char* t = p + n;
    bool colon = false;
    while (*t == ':' || *t == ' ' || *t == '-') { if (*t == ':') colon = true; t++; }
    // "Away message: ..." and "Away msg ..." mean the same: message and
    // msg are a label, not what you want shown. Only as a whole word,
    // and only when it reads as a label: followed by a colon or a dash,
    // or straight after "away" with no colon. "Away: Messages go to
    // Sara" keeps every word.
    int L = !strncasecmp(t, "message", 7) ? 7 : !strncasecmp(t, "msg", 3) ? 3 : 0;
    if (L && (t[L] == ':' || t[L] == '-' || t[L] == ' ' || !t[L])) {
      const char* q = t + L;
      while (*q == ' ') q++;
      if (*q == ':' || *q == '-' || !colon) t = q;
    }
    while (*t == ':' || *t == ' ' || *t == '-') t++;
    String m = String(t); m.replace("\\r", ""); m.trim();
    awaySet(true, m.c_str());""")

# 2. normal-size text, wrapped and centred; long text scrolls as usual
rep("""static void drawAway() {
  oled.clearDisplay();
  fitText(awayText.length() ? awayText.c_str() : "Away", 2, 50, 0);""",
"""// The message in the ordinary font, wrapped at 21 and centred. Only
// if it is too long for the screen does it fall back to scrolling.
static void awayText1(const char* t) {
  int n = wrapInto(t, 21, REM_LN);
  if (n * 10 > 50) { fitText(t, 2, 50, 0); return; }
  int y = 2 + (50 - n * 10) / 2;
  for (int i = 0; i < n; i++) ctr(remLines[i], y + i * 10, 1);
}
static void drawAway() {
  oled.clearDisplay();
  awayText1(awayText.length() ? awayText.c_str() : "Away");""")
rep("""    oled.clearDisplay();
    fitText(awayText.c_str(), 2, 50, 0);""", """    oled.clearDisplay();
    awayText1(awayText.c_str());""")
rep("static void awayEyes(bool open);\n", "static void awayEyes(bool open);\nstatic void awayText1(const char* t);\n")

# 3. a touch in Away's sleep shows the message at once (no hold)
rep("""  // A touch must be held for the Wake hold time, as everywhere else.
  // Let go early and it is written down and nothing is shown.
  if (!moved) {
    int wi = constrain(prefs.getInt("wakeh", 2), 0, WAKE_N - 1);
    uint32_t need = WAKE_OPTS[wi], t0 = millis();
    while (need && digitalRead(TOUCH_PIN) != touchRest && millis() - t0 < need) delay(10);
    if (need && millis() - t0 < need) { LittleFS.end(); delay(50); awayDeepArm(false); }
  }""", """  // Any touch shows it, no hold: whoever finds it needs to read it,
  // and would not know to hold.""")

# 4. restarted in Away: no ceremony, eyes then the message, 2 min listening
rep("#define  AWAY_LISTEN_RESTART_MS 180000UL", "#define  AWAY_LISTEN_RESTART_MS 120000UL")
rep("""  prefs.begin("nexus", false);
  pmInit();                            // says whether light sleep is on offer""",
"""  prefs.begin("nexus", false);
  pmInit();                            // says whether light sleep is on offer
  bool awayBoot = prefs.getBool("away", false);   // Away: no start-up screens at all""")
rep("""  if (!fromDeep) animWake(1200);       // from sleep: no ceremony, straight to the face""",
    """  if (!fromDeep && !awayBoot) animWake(1200);   // from sleep or in Away: no ceremony""")
rep("""  if (!fromDeep) animSenses(700);""", """  if (!fromDeep && !awayBoot) animSenses(700);""")
rep("""  } else if (!fromDeep && !cfgOffline) {
    offlineWelcome();
  }""", """  } else if (!fromDeep && !cfgOffline && !awayBoot) {
    offlineWelcome();
  }""")
import re as _re
_m = _re.search(r"  oled\.clearDisplay\(\);\n  if \(!fromDeep\) \{\n((?:    //[^\n]*\n)*)    titleBarC\(\"HOW TO USE ME\"\);\n    at\(8,  16, \"1  next\"\);\n    at\(8,  28, \"2  back\"\);\n    at\(66, 16, \"hold  open\"\);\n    at\(66, 28, \"5s  home\"\);\n", s)
if not _m: sys.exit("how-to card not found")
s = s[:_m.start()] + "  oled.clearDisplay();\n  if (!fromDeep && !awayBoot) {\n" + _m.group(1) + """    // The controls as they are since 6.1: a tap moves on, a hold opens,
    // a longer hold goes back, four seconds switches off.
    titleBarC("HOW TO USE ME");
    ctr("tap: next  hold: open", 16, 1);
    ctr("long: back  4s: off", 28, 1);
""" + s[_m.end():]
rep("""  if (!fromDeep) {
    oled.display();
    holdCard(2000);                    // a knock ends it, and it never dawdles
  }""", """  if (!fromDeep && !awayBoot) {
    oled.display();
    holdCard(2000);                    // a knock ends it, and it never dawdles
  }""")
rep("""    awayListenUntil = millis() + AWAY_LISTEN_RESTART_MS;
    awayShowMs = AWAY_RESTART_SHOW_MS;""", """    awayListenUntil = millis() + AWAY_LISTEN_RESTART_MS;
    awayShowMs = AWAY_RESTART_SHOW_MS;
    awayEyes(true);                    // the eyes open, then the message
    lastActive = millis(); lastDraw = 0;""")
rep("""    // Switched off and on: the message for three seconds, then three
    // minutes of Bluetooth in the background for a RAFIQ home.""",
    """    // Switched off and on: eyes, the message for three seconds, then
    // two minutes of Bluetooth in the background for a RAFIQ home.""")
rep("""    Serial.println("away: restarted, listening for three minutes");""",
    """    Serial.println("away: restarted, listening for two minutes");""")
open(SRC,'w').write(s); print("6.9 ok")

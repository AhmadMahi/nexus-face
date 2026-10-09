# Rafiq 6.7.0: Away timings as asked, the wake hold respected, and no
# eyes anywhere in Away.
import sys
SRC=sys.argv[1]; s=open(SRC).read()
def rep(old,new,count=1):
    global s
    n=s.count(old)
    if n!=count: sys.exit(f"anchor {n}x:\n{old[:200]}")
    s=s.replace(old,new)
rep('#define FW_VERSION "6.6.0"', '#define FW_VERSION "6.7.0"')

# timings
rep("#define  AWAY_SHOW_MS 8000UL", "#define  AWAY_SHOW_MS 3000UL        // a touch or a move while still listening\n#define  AWAY_SENT_SHOW_MS 5000UL   // when it has just been sent")
rep("#define  AWAY_LISTEN_RESTART_MS 120000UL", "#define  AWAY_LISTEN_RESTART_MS 180000UL")
rep("#define  AWAY_RESTART_SHOW_MS     5000UL", "#define  AWAY_RESTART_SHOW_MS     3000UL")
rep("""    awayListenUntil = millis() + AWAY_LISTEN_SENT_MS;
    awayShowMs = AWAY_SHOW_MS;
    wake("away");""", """    awayListenUntil = millis() + AWAY_LISTEN_SENT_MS;
    awayShowMs = AWAY_SENT_SHOW_MS;
    wake("away");""")
rep("""    if (on) { awayListenUntil = millis() + AWAY_LISTEN_SENT_MS; wake("away"); }""",
    """    if (on) { awayListenUntil = millis() + AWAY_LISTEN_SENT_MS; awayShowMs = AWAY_SENT_SHOW_MS; wake("away"); lastActive = millis(); }""")
rep("""    // Switched off and on: the message for five seconds, then two
    // minutes of Bluetooth in the background for a RAFIQ home.""",
    """    // Switched off and on: the message for three seconds, then three
    // minutes of Bluetooth in the background for a RAFIQ home.""")
rep("""    Serial.println("away: restarted, listening for two minutes");""",
    """    Serial.println("away: restarted, listening for three minutes");""")

# no eyes in Away: not opening on a wake, not closing on a sleep
rep("""  if (!cfgGesture && !news)
    for (int i = 0; i < 8; i++) { eyesFrame(); delay(16); }""",
    """  if (!cfgGesture && !news && !awayOn)
    for (int i = 0; i < 8; i++) { eyesFrame(); delay(16); }
  if (awayOn) { lastActive = millis(); lastDraw = 0; }   // the message straight away, from now""")
rep("""static void goSleep() {
  if (asleep) return;""", """static void goSleep() {
  if (asleep) return;
  if (awayOn) { goSleepQuick(); return; }  // Away: dark at once, no closing eyes""")

# the wake hold applies to a touch in Away's deep sleep too
rep("""  uint64_t st = esp_sleep_get_gpio_wakeup_status();
  bool moved = (st & BIT(TAP_INT_PIN)) && !(st & BIT(TOUCH_PIN));
  fsOk = LittleFS.begin(false);
  tlogAdd(moved ? "Moved (away)" : "Touched (away)");""", """  uint64_t st = esp_sleep_get_gpio_wakeup_status();
  bool moved = (st & BIT(TAP_INT_PIN)) && !(st & BIT(TOUCH_PIN));
  fsOk = LittleFS.begin(false);
  tlogAdd(moved ? "Moved (away)" : "Touched (away)");
  // A touch must be held for the Wake hold time, as everywhere else.
  // Let go early and it is written down and nothing is shown.
  if (!moved) {
    int wi = constrain(prefs.getInt("wakeh", 2), 0, WAKE_N - 1);
    uint32_t need = WAKE_OPTS[wi], t0 = millis();
    while (need && digitalRead(TOUCH_PIN) != touchRest && millis() - t0 < need) delay(10);
    if (need && millis() - t0 < need) { LittleFS.end(); delay(50); awayDeepArm(false); }
  }""")
open(SRC,'w').write(s); print("6.7 ok")

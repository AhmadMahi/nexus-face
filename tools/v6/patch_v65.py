#!/usr/bin/env python3
# Rafiq 6.5.0. Away: a Bluetooth window, then deep sleep that shows the
# message on a touch or a move and goes straight back. Applied after v64.
import sys
SRC = sys.argv[1]
s = open(SRC).read()
def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count: sys.exit(f"anchor {n}x:\n{old[:220]}")
    s = s.replace(old, new)

rep('#define FW_VERSION "6.4.0"', '#define FW_VERSION "6.5.0"')

rep("""bool     awayOn = false;
String   awayText = "";
#define  AWAY_SHOW_MS 8000UL""", """bool     awayOn = false;
String   awayText = "";
#define  AWAY_SHOW_MS 8000UL
// 6.5: Away listens on Bluetooth for a while, then sleeps for real.
//   sent from the phone: 5 minutes listening for home
//   switched off and on: the message for 5 s, then 2 minutes listening
//   after that: deep sleep. A touch or a move shows the message for
//   3 s and it is straight back, Bluetooth never coming on, so nobody
//   but a restart (you) can reach it.
#define  AWAY_LISTEN_SENT_MS    300000UL
#define  AWAY_LISTEN_RESTART_MS 120000UL
#define  AWAY_RESTART_SHOW_MS     5000UL
#define  AWAY_DEEP_SHOW_MS        3000UL
uint32_t awayListenUntil = 0;
uint32_t awayShowMs = AWAY_SHOW_MS;
RTC_DATA_ATTR uint8_t rtcAwayDeep = 0;   // 1 asleep in Away, 2 pausing after a move""")

# sending away starts the five minutes
rep("""  if (on) {
    tlogAdd("Away on");
    popOn = false; pgUntil = 0; findUntil = 0; upState = U_OFF;
    wake("away");""", """  if (on) {
    tlogAdd("Away on");
    popOn = false; pgUntil = 0; findUntil = 0; upState = U_OFF;
    awayListenUntil = millis() + AWAY_LISTEN_SENT_MS;
    awayShowMs = AWAY_SHOW_MS;
    wake("away");""")
rep("""  } else {
    awayFlush(true);
    tlogAdd("Away off");""", """  } else {
    awayFlush(true);
    tlogAdd("Away off");
    awayListenUntil = 0; rtcAwayDeep = 0;""")
rep("""  if (on == awayOn) { if (on) wake("away"); return; }""",
    """  if (on == awayOn) {
    if (on) { awayListenUntil = millis() + AWAY_LISTEN_SENT_MS; wake("away"); }
    return;
  }""")

# loop: the listening window ends in Away's deep sleep
rep("""  if (awayOn && !asleep) {
    if (now - lastActive > AWAY_SHOW_MS) { goSleepQuick(); return; }""",
"""  if (awayOn && !asleep) {
    if (now - lastActive > awayShowMs) { awayShowMs = AWAY_SHOW_MS; goSleepQuick(); return; }""")
rep("""  // 6.4: these run asleep as well as awake.
  awayFlush(false);""", """  // 6.4: these run asleep as well as awake.
  awayFlush(false);
  if (awayOn && awayListenUntil && (int32_t)(now - awayListenUntil) > 0) awayDeepGo();""")

# the deep sleep itself, and the wake that never reaches the rest of setup
rep("static void syncBegin() {", r"""// Off, with only the pad and the accelerometer able to wake it, as the
// Wake by setting allows. Never returns.
static void awayDeepArm(bool pause) {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (pause) {                                   // a move just woke it: let it settle
    rtcAwayDeep = 2;
    esp_sleep_enable_timer_wakeup(20ULL * 1000000ULL);
  } else {
    rtcAwayDeep = 1;
    if (touchWakes())
      esp_deep_sleep_enable_gpio_wakeup(BIT(TOUCH_PIN), touchRest ? ESP_GPIO_WAKEUP_GPIO_LOW
                                                                  : ESP_GPIO_WAKEUP_GPIO_HIGH);
    if (motionWakes() && intWired)
      esp_deep_sleep_enable_gpio_wakeup(BIT(TAP_INT_PIN), ESP_GPIO_WAKEUP_GPIO_HIGH);
  }
  esp_deep_sleep_start();
}
static void awayDeepGo() {
  awayFlush(true);
  tlogAdd("Away asleep");
  prefs.putBool("trest", touchRest);
  const char* tz = getenv("TZ");
  snprintf(rtcTz, sizeof(rtcTz), "%s", tz ? tz : "");
  oled.clearDisplay(); oled.display();
  screenPower(false);
  bleOff();
  if (cfgNet == NET_WIFI) { WiFi.disconnect(true, false); WiFi.mode(WIFI_OFF); }
  if (intWired && adxl) {                        // the same activity watch as tamper
    wReg(adxl, 0x2E, 0x00);
    wReg(adxl, 0x24, 6);                         // 375 mg
    wReg(adxl, 0x27, 0xF0);                      // activity, AC coupled, x y z
    wReg(adxl, 0x2F, 0x00);                      // to INT1
    wReg(adxl, 0x2E, 0x10);
    rReg(adxl, 0x30);
    delay(20);
  }
  Serial.println("away: listening is over, sleeping until touched or moved");
  awayDeepArm(false);
}
// First thing in setup on a wake from Away's sleep: the message for
// three seconds, written down, and off again. No radio, nothing else.
static void awayDeepWake(bool timer) {
  if (rtcTz[0]) { setenv("TZ", rtcTz, 1); tzset(); }
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  prefs.begin("nexus", false);
  touchRest  = prefs.getBool("trest", false);
  cfgWakeBy  = constrain(prefs.getInt("wakeby", 0), 0, 2);
  cfg12h     = prefs.getBool("h12", false);
  awayText   = prefs.getString("awayt", "Away");
  cfgBright  = prefs.getInt("bri", cfgBright);
  intWired   = true;                              // it was, or this wake could not have happened
  pinMode(TOUCH_PIN, INPUT);
  pinMode(TAP_INT_PIN, INPUT_PULLDOWN);
  if (timer) {                                   // the pause after a move is over
    rReg(0x53, 0x30);
    delay(5);
    if (digitalRead(TAP_INT_PIN)) awayDeepArm(true);
    awayDeepArm(false);
  }
  uint64_t st = esp_sleep_get_gpio_wakeup_status();
  bool moved = (st & BIT(TAP_INT_PIN)) && !(st & BIT(TOUCH_PIN));
  fsOk = LittleFS.begin(false);
  tlogAdd(moved ? "Moved (away)" : "Touched (away)");
  if (oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) {
    oled.setTextWrap(false); oled.setTextColor(SSD1306_WHITE);
    applyBright();
    oled.clearDisplay();
    fitText(awayText.c_str(), 2, 50, 0);
    time_t t = time(nullptr);
    if (t > 1700000000) {
      struct tm lt; localtime_r(&t, &lt);
      int h = lt.tm_hour;
      if (cfg12h) { h %= 12; if (!h) h = 12; }
      char c[8]; snprintf(c, sizeof(c), cfg12h ? "%d:%02d" : "%02d:%02d", h, lt.tm_min);
      ctr(c, 56, 1);
    }
    oled.display();
    delay(AWAY_DEEP_SHOW_MS);
    oled.clearDisplay(); oled.display();
    oled.ssd1306_command(SSD1306_DISPLAYOFF);
  }
  LittleFS.end();
  // Do not go back to sleep with the pad still pressed, or it wakes at
  // once: wait for it to be let go, a few seconds at most.
  for (int i = 0; i < 50 && digitalRead(TOUCH_PIN) != touchRest; i++) delay(100);
  rReg(0x53, 0x30);                              // let INT1 go
  delay(5);
  if (digitalRead(TAP_INT_PIN) || digitalRead(TOUCH_PIN) != touchRest) awayDeepArm(true);
  awayDeepArm(false);
}

static void syncBegin() {""")
rep("static void syncBegin();\n", "static void syncBegin();\nstatic void awayDeepGo();\nstatic void awayDeepWake(bool timer);\n")

# setup: Away's sleep wakes go nowhere else
rep("""  if (rtcTamper && (woke_ == ESP_SLEEP_WAKEUP_GPIO || woke_ == ESP_SLEEP_WAKEUP_TIMER))
    tamperWake(woke_ == ESP_SLEEP_WAKEUP_GPIO);""", """  if (rtcTamper && (woke_ == ESP_SLEEP_WAKEUP_GPIO || woke_ == ESP_SLEEP_WAKEUP_TIMER))
    tamperWake(woke_ == ESP_SLEEP_WAKEUP_GPIO);
  // Asleep in Away: show the message, write it down, back off. Never
  // returns, and never reaches the radio.
  if (rtcAwayDeep && (woke_ == ESP_SLEEP_WAKEUP_GPIO || woke_ == ESP_SLEEP_WAKEUP_TIMER))
    awayDeepWake(woke_ == ESP_SLEEP_WAKEUP_TIMER);
  rtcAwayDeep = 0;""")

# setup: a real restart while away is the owner's way back in
rep("""  awayOn   = prefs.getBool("away", false);
  awayText = prefs.getString("awayt", "");""", """  awayOn   = prefs.getBool("away", false);
  awayText = prefs.getString("awayt", "");
  if (awayOn) {
    // Switched off and on: the message for five seconds, then two
    // minutes of Bluetooth in the background for a RAFIQ home.
    awayListenUntil = millis() + AWAY_LISTEN_RESTART_MS;
    awayShowMs = AWAY_RESTART_SHOW_MS;
    tlogAdd("Away: restarted");
    Serial.println("away: restarted, listening for two minutes");
  }""")

open(SRC, 'w').write(s)
print("6.5 ok")

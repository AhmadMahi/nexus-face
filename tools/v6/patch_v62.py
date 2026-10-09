#!/usr/bin/env python3
# Rafiq 6.2.0. Applied after patch_v61.
import re, sys
SRC = sys.argv[1]
s = open(SRC).read()

def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f"anchor matched {n} times, wanted {count}:\n{old[:220]}")
    s = s.replace(old, new)

rep('#define FW_VERSION "6.1.0"', '#define FW_VERSION "6.2.0"')

# =================================================================
# 1. THE HOLD BAR, AT THE BOTTOM, OVER WHATEVER IS ON THE SCREEN
#    Every frame goes through display(); the strip is added there, so
#    no screen has to know about it.
# =================================================================
rep("""Adafruit_SSD1306 oled(SCRW, SCRH, &Wire, -1);
RoboEyes<Adafruit_SSD1306> eyes(oled);""", """//  The panel, with one addition: while a finger is held on the pad
//  past the hold time, every frame gets the hold strip drawn along its
//  bottom just before it goes out. The screens never know.
static void drawHoldStrip();
static bool holdStripWanted();
struct OledX : public Adafruit_SSD1306 {
  using Adafruit_SSD1306::Adafruit_SSD1306;
  void display() {
    if (holdStripWanted()) drawHoldStrip();
    Adafruit_SSD1306::display();
  }
};
OledX oled(SCRW, SCRH, &Wire, -1);
RoboEyes<OledX> eyes(oled);""")

# Open, then Back. The whole bar takes as long as Open and Cancel did
# in 6.1; Cancel is gone. At four seconds, the 3 s switch-off count.
rep("""static uint32_t holdMaxMs()       { uint32_t a = holdMs() + 1500; return a > 2000 ? a : 2000; }
static uint32_t holdBackMs()      { return holdMaxMs() + 1000; }
static uint32_t holdSleepShowMs() { return holdBackMs() + 2000; }
static uint32_t holdSleepLongMs() { return holdSleepShowMs() + 3000; }""",
"""static uint32_t holdMaxMs()       { uint32_t a = holdMs() + 1500; return a > 2000 ? a : 2000; }
static uint32_t holdBackMs()      { return holdMs() + (holdMaxMs() - holdMs()) / 2; }   // Open ends, Back begins
""")

rep("""          if (!touchLongDone) {
            if      (held < holdMs())          touchGesture(TG_ONE);   // a tap, at once
            else if (held < holdMaxMs())       touchGesture(TG_LONG);  // Open
            else if (held < holdBackMs())      { }                     // Cancel
            else if (held < holdSleepShowMs()) touchGesture(TG_TWO);   // Back
            // past that it was on its way to switching off: let go, nothing
          }""", """          if (!touchLongDone) {
            if      (held < holdMs())     touchGesture(TG_ONE);    // a tap, at once
            else if (held < holdBackMs()) touchGesture(TG_LONG);   // Open
            else                          touchGesture(TG_TWO);    // Back
          }""")
rep("""      if (!cfgGesture && held >= holdSleepLongMs()) {
        touchLongDone = true; holdShown = false;
        Serial.println("held to the end of the bar: switching off");
        wantDeep = true;
      }
    }
""", """      // Held to four: home, and the three second count to switching
      // off, as before 6.1. Letting go now does nothing; the count runs.
      if (!cfgGesture && !sleepArmed && held >= TOUCH_HOME_MS) {
        sleepArmed = now;
        touchLongDone = true; holdShown = false;
        screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0; faceMode = false;
        upState = U_OFF; swOn = false; swRun = false; popOn = false;
        Serial.println("held to four: home, and switching off");
      }
    }
    if (sleepArmed) {
      lastActive = now;
      if (now - sleepArmed >= TOUCH_COUNT_MS) { sleepArmed = 0; wantDeep = true; }
    }
""")

m = re.search(r"static void drawHoldBar\(uint32_t now\) \{.*?\n\}\n", s, re.S)
if not m: sys.exit("drawHoldBar not found")
s = s[:m.start()] + r"""static bool holdStripWanted() {
  return holdShown && touchOn && !sleepArmed && !cfgGesture && !gamePlaying();
}
// Along the bottom, like C3 Buddy: a bar filling left to right with a
// mark where Open turns into Back, and on the right what letting go
// will do, in a solid label.
static void drawHoldStrip() {
  uint32_t held = millis() - touchPressAt;
  uint32_t a = holdMs(), b = holdMaxMs(), m = holdBackMs();
  float f = (float)(held - a) / (float)(b - a);
  if (f < 0) f = 0;
  if (f > 1) f = 1;
  bool back = held >= m;
  oled.fillRect(0, 51, SCRW, 13, SSD1306_BLACK);
  oled.drawFastHLine(0, 51, SCRW, SSD1306_WHITE);
  oled.drawRoundRect(2, 54, 80, 8, 3, SSD1306_WHITE);
  int w = (int)(76 * f);
  if (w > 1) oled.fillRoundRect(4, 56, w, 4, 1, SSD1306_WHITE);
  int tick = 4 + (int)(76.0f * (float)(m - a) / (float)(b - a));
  oled.drawFastVLine(tick, 52, 2, SSD1306_WHITE);
  oled.fillRoundRect(86, 53, 40, 10, 3, SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_BLACK);
  const char* lab = back ? "BACK" : "OPEN";
  oled.setCursor(86 + (40 - (int)strlen(lab) * 6) / 2, 54);
  oled.print(lab);
  oled.setTextColor(SSD1306_WHITE);
}
""" + s[m.end():]
rep("""  // 6.1: the hold bar, while a finger is on the pad past the hold time.
  // Not inside a game being played: games keep their own feel.
  if (holdShown && touchOn && !(screen == S_GAMES && gState == GS_PLAY)) {
    lastActive = now;
    if (now - lastDraw >= 33) { lastDraw = now; drawHoldBar(now); }
    delay(2); return;
  }
""", "")

# =================================================================
# 2. NEWS WHILE ASLEEP: one second, then straight back to sleep
# =================================================================
rep("""  popWoke = asleep || popWoke;
  wake("notification");
  popOn = true;
  popUntil = millis() + popupSecs() * 1000UL;""", """  popWoke = asleep || popWoke;
  wake("notification");
  popOn = true;
  // Asleep, it is a glance: a second, then dark again, unless you
  // touch it. Awake, it stays for the popup time.
  popUntil = millis() + (popWoke ? 1000UL : popupSecs() * 1000UL);""")
rep("""  if (popWoke) { popWoke = false; goSleep(); }
}""", """  if (popWoke) { popWoke = false; goSleepQuick(); }
}
// Straight to dark, no closing eyes: after a glance nobody is watching.
static void goSleepQuick() {
  if (asleep) return;
  asleep = true;
  eyes.setIdleMode(OFF); eyes.setAutoblinker(OFF);
  screenPower(false);
  sleptAt = millis();
  nSlept++;
}""")
rep("static void popupClose(bool open);\n", "static void popupClose(bool open);\nstatic void goSleepQuick();\n")
rep("static void drawHoldStrip();\n", "static void drawHoldStrip();\nstatic bool gamePlaying();\n")
rep("static void popupShow() {\n", "static bool gamePlaying() { return screen == S_GAMES && gState == GS_PLAY; }\nstatic void popupShow() {\n")
rep("""  if (popOn)         { popupClose(g == TG_LONG); return; }""",
    """  if (popOn) {
    if (g == TG_LONG) popupClose(true);                  // open it
    else if (popWoke) {                                   // a touch on a glance: stay up
      popWoke = false;
      popUntil = millis() + popupSecs() * 1000UL;
    } else popupClose(false);
    return;
  }""")

# =================================================================
# 3. IPHONE LINKED / UNLINKED: small white words on black, briefly
# =================================================================
rep("""    if (!asleep && cfgNet == NET_BT) flash(k == 1 ? "IPHONE CONNECTED" : "IPHONE DISCONNECTED", 1300);""",
    """    if (!asleep && cfgNet == NET_BT) { banText = k == 1 ? "iPhone connected" : "iPhone disconnected"; banUntil = millis() + 1100; }""")
rep("volatile uint8_t btNews = 0;", """volatile uint8_t btNews = 0;
const char* banText = "";              // a quiet line on a black screen
uint32_t    banUntil = 0;""")
rep("""  if (popOn && now > popUntil) popupClose(false);""", """  if (banUntil && now > banUntil) banUntil = 0;
  if (banUntil) {
    if (now - lastDraw >= 100) { lastDraw = now; oled.clearDisplay(); ctr(banText, 28, 1); oled.display(); }
    delay(2); return;
  }
  if (popOn && now > popUntil) popupClose(false);""")

# =================================================================
# 4. HOME IS THE WATCH FACE ON BLUETOOTH, CLOCK OR NOT YET
#    The hello and pairing screens only for a robot never paired.
# =================================================================
rep("""  if (offlineNow() && !(cfgNet == NET_BT && fb.ok)) {""",
    """  if (offlineNow() && !(cfgNet == NET_BT && (fb.ok || NimBLEDevice::getNumBonds() > 0))) {""")

# =================================================================
# 5. WAKE BY: touch and motion, touch only, motion only
# =================================================================
rep("""       C_GUARD, C_TAMPER, C_TLOG, C_HOLD, C_CLOCK, C_COUNT };""",
    """       C_GUARD, C_TAMPER, C_TLOG, C_HOLD, C_CLOCK, C_WAKEBY, C_COUNT };""")
rep("""    "Phone guard", "Tamper alarm", "Tamper log", "Hold time", "Clock" };""",
    """    "Phone guard", "Tamper alarm", "Tamper log", "Hold time", "Clock", "Wake by" };""")
rep("""  { C_HOLD,   C_KNOCK,   C_TAP,   C_SHAKE,  C_WAKEH,  C_ACCEL,  SG_END,  SG_END },""",
    """  { C_WAKEBY, C_HOLD,    C_KNOCK, C_TAP,    C_SHAKE,  C_WAKEH,  C_ACCEL, SG_END },""")
rep("""      case C_CLOCK:  snprintf(v, sizeof(v), "%s", cfg12h ? "12 hour" : "24 hour"); break;""",
    """      case C_CLOCK:  snprintf(v, sizeof(v), "%s", cfg12h ? "12 hour" : "24 hour"); break;
      case C_WAKEBY: snprintf(v, sizeof(v), "%s", WAKEBY_NAME[cfgWakeBy]); break;""")
rep("""      case C_CLOCK:
        cfg12h = !cfg12h; prefs.putBool("h12", cfg12h);
        break;""", """      case C_CLOCK:
        cfg12h = !cfg12h; prefs.putBool("h12", cfg12h);
        break;
      case C_WAKEBY:
        cfgWakeBy = (cfgWakeBy + 1) % 3; prefs.putInt("wakeby", cfgWakeBy);
        break;""")
rep("""bool cfg12h     = false;               // 12 hour clock""", """bool cfg12h     = false;               // 12 hour clock
// What wakes it from sleep: the pad and movement (knock, shake, lift),
// only the pad, or only movement.
int  cfgWakeBy  = 0;
const char* WAKEBY_NAME[3] = { "touch+move", "touch", "move" };
static bool touchWakes()  { return cfgWakeBy != 2; }
static bool motionWakes() { return cfgWakeBy != 1; }""")
rep("""  cfg12h     = prefs.getBool("h12", false);""", """  cfg12h     = prefs.getBool("h12", false);
  cfgWakeBy  = constrain(prefs.getInt("wakeby", 0), 0, 2);""")
rep("""          if (asleep) { wake("touch"); touchTaps = 0; touchLongDone = true; }
        } else {
          touchLiftAt = now;
          // What the bar said""", """          if (asleep) { if (touchWakes()) wake("touch"); touchTaps = 0; touchLongDone = true; }
        } else {
          touchLiftAt = now;
          // What the bar said""")
rep("""    if (s & INT_TAP1) {
      if (tapTesting) { tapSeen++; tapLastSeen = now; }""", """    if ((s & INT_TAP1) && asleep && !motionWakes()) { /* asleep and set to wake by touch only */ }
    else if (s & INT_TAP1) {
      if (tapTesting) { tapSeen++; tapLastSeen = now; }""")
rep("""    if (asleep) { wake("shake"); return; }""", """    if (asleep) { if (motionWakes()) wake("shake"); return; }""")
rep("""    if (asleep) wake("picked up");""", """    if (asleep) { if (!motionWakes()) return; wake("picked up"); }""")
rep("""    if (asleep) wake("moved");""", """    if (asleep) { if (!motionWakes()) return; wake("moved"); }""")

# =================================================================
# 6. FASTER STARTS
#    The eyes no longer take 2.6 s to open on every wake: 1.2 s from
#    power on, nothing at all from sleep. Senses check 0.7 s, from
#    power on only, as before.
# =================================================================
rep("""static void animWake() {
  const unsigned long TOTAL = 2600;""", """static void animWake(unsigned long TOTAL) {""")
rep("""  animWake();
  startSensors();""", """  if (!fromDeep) animWake(1200);       // from sleep: no ceremony, straight to the face
  startSensors();""")
rep("""  if (!fromDeep) animSenses(1500);""", """  if (!fromDeep) animSenses(700);""")

open(SRC, 'w').write(s)
print("6.2 ok")

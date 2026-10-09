# Rafiq 7.9.0: pocket lock. Untouched for a few minutes in light sleep, a
# short touch or a move no longer lights the screen; a hold (Wake on
# hold, 3 s by default) does, with a thin bar after the first second.
# Notifications, Shortcuts and the Mac still wake it as before.
import sys
SRC=sys.argv[1]; s=open(SRC).read()
def rep(a,b,c=1):
    global s
    n=s.count(a)
    if n!=c: sys.exit(f"anchor {n}x:\n{a[:200]}")
    s=s.replace(a,b)
rep('#define FW_VERSION "7.8.1"', '#define FW_VERSION "7.9.0"')

rep("volatile uint8_t btNews = 0;", """volatile uint8_t btNews = 0;
// ---- 7.9: pocket lock ----
int  cfgPLock = 2;                     // off, 1, 3, 10 minutes
const uint8_t PLOCK_MIN[4] = { 0, 1, 3, 10 };
const char* PLOCK_NAME[4] = { "off", "1 min", "3 min", "10 min" };
uint32_t lastUserAt = 0;               // the last real touch, not a popup or a glance
bool     unlocking = false;
uint32_t unlockAt = 0;
bool     unlockShown = false;""")
# the rule, used before it is defined
rep("static bool motionWakes() { return cfgWakeBy != 1; }",
    "static bool pocketLocked();\nstatic bool motionWakes() { return cfgWakeBy != 1 && !pocketLocked(); }   // 7.9: not while locked")

rep("static void syncBegin() {", r"""// Locked: dark, quiet for the set minutes since a real touch, and
// nothing running that wants the screen.
static bool pocketLocked() {
  if (!cfgPLock || !asleep) return false;
  if (tmrOn || relaxOn || cfgFollow || awayOn || otaOn) return false;
  return millis() - lastUserAt > (uint32_t)PLOCK_MIN[constrain(cfgPLock, 0, 3)] * 60000UL;
}
static uint32_t unlockMs() { uint32_t w = WAKE_OPTS[cfgWakeIdx]; return w ? w : 1000; }
// While a finger is down on a locked robot: dark for the first second,
// then a thin bar that fills; full, and it wakes. Let go early and it is
// dark again at once.
static void pocketTick() {
  if (!unlocking) return;
  uint32_t now = millis(), held = now - unlockAt;
  if (!touchOn) {                                   // let go before the end
    unlocking = false;
    if (unlockShown) { unlockShown = false; oled.clearDisplay(); oled.display(); screenPower(false); }
    return;
  }
  if (held >= unlockMs()) {
    unlocking = false; unlockShown = false;
    lastUserAt = now;
    wake("unlock");
    return;
  }
  if (held >= 1000) {
    if (!unlockShown) { unlockShown = true; screenPower(true); }
    uint32_t span = unlockMs() > 1000 ? unlockMs() - 1000 : 1;
    int w = (int)((uint64_t)(held - 1000) * 60 / span);
    oled.clearDisplay();
    oled.drawRoundRect(SCRW / 2 - 32, SCRH - 6, 64, 4, 2, SSD1306_WHITE);
    if (w > 0) oled.fillRoundRect(SCRW / 2 - 31, SCRH - 5, min(w, 62), 2, 1, SSD1306_WHITE);
    oled.display();
  }
}

static void syncBegin() {""")
rep("static void syncBegin();\n", "static void syncBegin();\nstatic void pocketTick();\n")
rep("""  blogTick();                          // 7.8: where the battery goes""",
    """  blogTick();                          // 7.8: where the battery goes
  pocketTick();                        // 7.9: holding to unlock""")
# a touch while locked starts the hold instead of waking
rep("""          if (asleep) { if (touchWakes()) wake("touch"); touchTaps = 0; touchLongDone = true; }""",
    """          if (asleep && pocketLocked()) {            // 7.9: locked: only a hold wakes it
            unlocking = true; unlockAt = now; unlockShown = false;
            touchTaps = 0; touchLongDone = true;
          }
          else if (asleep) { if (touchWakes()) wake("touch"); lastUserAt = now; touchTaps = 0; touchLongDone = true; }
          else lastUserAt = now;                     // awake: a real touch keeps it unlocked""")
# settings: Touch and motion gets the row
rep("""       C_BLOG, C_BVIEW, C_BRESET, C_COUNT };""", """       C_BLOG, C_BVIEW, C_BRESET, C_PLOCK, C_COUNT };""")
rep('''    "Battery log", "Battery use", "New log at" };''', '''    "Battery log", "Battery use", "New log at", "Pocket lock" };''')
rep("""  { C_WAKEBY,   C_WAKEH, C_HOLD,  C_SHAKE,  C_KNOCK,  C_TAP,    C_ACCEL, SG_END },""",
    """  { C_WAKEBY,   C_PLOCK, C_WAKEH, C_HOLD,   C_SHAKE,  C_KNOCK,  C_TAP,   C_ACCEL },""")
rep("""      case C_BLOG:   snprintf(v, sizeof(v), "%s", cfgBlog ? "on" : "off"); break;""",
    """      case C_BLOG:   snprintf(v, sizeof(v), "%s", cfgBlog ? "on" : "off"); break;
      case C_PLOCK:  snprintf(v, sizeof(v), "%s", PLOCK_NAME[constrain(cfgPLock, 0, 3)]); break;""")
rep("""      case C_BLOG:
        cfgBlog = !cfgBlog; prefs.putBool("blog", cfgBlog);""", """      case C_PLOCK:
        cfgPLock = (cfgPLock + 1) % 4; prefs.putInt("plock", cfgPLock);
        flash(cfgPLock ? "POCKET LOCK ON" : "POCKET LOCK OFF", 1100);
        break;
      case C_BLOG:
        cfgBlog = !cfgBlog; prefs.putBool("blog", cfgBlog);""")
rep("""  cfgBlog  = prefs.getBool("blog", true);""", """  cfgBlog  = prefs.getBool("blog", true);
  cfgPLock = constrain(prefs.getInt("plock", 2), 0, 3);
  lastUserAt = millis();""")
rep("""    else if (k == "blog")  {""", """    else if (k == "plock") { cfgPLock = constrain(v, 0, 3); prefs.putInt("plock", cfgPLock); }
    else if (k == "blog")  {""")
open(SRC,'w').write(s); print("7.9 ok")

#!/usr/bin/env python3
# Rafiq 7.8.0: the battery log. Where the time goes (screen on, dark but
# awake, light sleep, deep sleep, WiFi), wakes and restarts, an estimate
# of each one's share, and what the battery actually lost. One cycle,
# from a full charge (a voltage you choose) to the next. After patch_v77.
import sys
SRC = sys.argv[1]
s = open(SRC).read()
def rep(a, b, c=1):
    global s
    n = s.count(a)
    if n != c: sys.exit(f"anchor {n}x:\n{a[:220]}")
    s = s.replace(a, b)

rep('#define FW_VERSION "7.7.0"', '#define FW_VERSION "7.8.0"')

# =================================================================
# THE LOG
# =================================================================
rep("volatile uint8_t btNews = 0;", """volatile uint8_t btNews = 0;
// ---- 7.8: the battery log, one charge cycle ----
//  Kept in RTC memory through deep sleep and written to flash every half
//  hour and before deep sleep: about fifty small writes a day, and a few
//  additions per loop. It costs nothing you could measure.
#define BLOG_MAGIC 0xB10C0001UL
struct BLog {
  uint32_t magic, start;               // start: when the cycle began (epoch, 0 if unknown)
  float    v0; uint8_t pct0;           // the battery then
  uint32_t sOn, sDark, sLight, sDeep, sWifi;   // seconds in each state
  uint32_t wakes, restarts;
};
RTC_DATA_ATTR BLog blog;
RTC_DATA_ATTR uint32_t rtcDeepAt = 0;
RTC_DATA_ATTR bool blogArmed = true;   // a reset waits for the voltage to fall back first
bool cfgBlog = true;
int  cfgBlogV = 1;                     // 4.10, 4.20, 4.25, 4.30
int  cfgCap = 350;                     // mAh
const float BLOG_V[4] = { 4.10f, 4.20f, 4.25f, 4.30f };
// Typical draw in each state, for the shares (estimates; "Used" is measured).
const float BLOG_MA[5] = { 38.0f, 19.0f, 3.0f, 0.3f, 80.0f };""")

rep("static void syncBegin() {", r"""static void blogReset() {
  memset(&blog, 0, sizeof(blog));
  blog.magic = BLOG_MAGIC;
  blog.start = timeOk ? (uint32_t)time(nullptr) : 0;
  blog.v0 = isnan(battV) ? 0 : battV;
  blog.pct0 = isnan(battV) ? 0 : battPct(battV);
}
static void blogSave() {
  if (cfgBlog && blog.magic == BLOG_MAGIC) prefs.putBytes("blog", &blog, sizeof(blog));
}
// At start: after a deep sleep the RTC copy is current and the sleep is
// added; after anything else the flash copy is, and it was a restart.
static void blogBoot(bool fromDeep) {
  if (fromDeep && blog.magic == BLOG_MAGIC) {
    uint32_t now = (uint32_t)time(nullptr);
    if (rtcDeepAt && now > rtcDeepAt && now - rtcDeepAt < 7UL * 86400UL) blog.sDeep += now - rtcDeepAt;
  } else {
    BLog b;
    if (prefs.getBytes("blog", &b, sizeof(b)) == sizeof(b) && b.magic == BLOG_MAGIC) blog = b;
    else blogReset();
    if (!fromDeep) blog.restarts++;
  }
  rtcDeepAt = 0;
}
// Every pass of the loop: the time since the last one goes to the state
// it was spent in. Light sleep keeps the clock running, so the sleeping
// itself lands here on the next pass.
static void blogTick() {
  static uint32_t last = 0, ms[5] = { 0 }, saveAt = 0, vAt = 0;
  uint32_t now = millis();
  if (!last) { last = now; saveAt = now; return; }
  uint32_t dt = now - last; last = now;
  if (!cfgBlog || blog.magic != BLOG_MAGIC) return;
  int k = (cfgNet == NET_WIFI || rescueAP) ? 4 : !asleep ? 0 : (pmMode == 1 ? 2 : 1);
  ms[k] += dt;
  uint32_t* sec[5] = { &blog.sOn, &blog.sDark, &blog.sLight, &blog.sDeep, &blog.sWifi };
  if (ms[k] >= 1000) { *sec[k] += ms[k] / 1000; ms[k] %= 1000; }
  // Full: the voltage you chose. Not again until it has fallen 0.10 V,
  // so a robot left on the charger does not wipe its log over and over.
  if (now - vAt > 5000 && !isnan(battV)) {
    vAt = now;
    float thr = BLOG_V[constrain(cfgBlogV, 0, 3)];
    if (blogArmed && battV >= thr - 0.005f) { blogReset(); blogArmed = false; blogSave(); Serial.println("battery log: full, a new cycle"); }
    else if (!blogArmed && battV < thr - 0.10f) blogArmed = true;
  }
  if (now - saveAt > 30UL * 60000UL) { saveAt = now; blogSave(); }
}
static float blogMah(int k) {
  uint32_t secs[5] = { blog.sOn, blog.sDark, blog.sLight, blog.sDeep, blog.sWifi };
  return secs[k] / 3600.0f * BLOG_MA[k];
}
static void fmtHM(uint32_t s, char* b, size_t n) { snprintf(b, n, "%lu:%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60)); }

// The view: tap moves down the list, hold starts a new cycle, back leaves.
static void drawBattUse() {
  oled.clearDisplay();
  titleBar("BATTERY USE", "mAh");
  const char* lab[9] = { "Screen", "Dark", "Light", "Deep", "WiFi", "Wakes", "Restarts", "Used", "Since" };
  const int NROWS = 9;                   // not N: RoboEyes owns N
  int sel = constrain(subIdx, 0, NROWS - 1);
  int first = uiFirst(sel, NROWS);
  for (int r = 0; r < UI_ROWS && first + r < NROWS; r++) {
    int i = first + r, y = UI_ROW_Y + r * UI_ROW_H;
    char t[16] = "", m[16] = "";
    if (i < 5) {
      uint32_t secs[5] = { blog.sOn, blog.sDark, blog.sLight, blog.sDeep, blog.sWifi };
      fmtHM(secs[i], t, sizeof(t));
      snprintf(m, sizeof(m), "%d", (int)lroundf(blogMah(i)));
    } else if (i == 5) snprintf(t, sizeof(t), "%lu", (unsigned long)blog.wakes);
    else if (i == 6) snprintf(t, sizeof(t), "%lu", (unsigned long)blog.restarts);
    else if (i == 7) {
      int now = isnan(battV) ? blog.pct0 : battPct(battV);
      int d = constrain((int)blog.pct0 - now, 0, 100);   // a percentage, so at most three digits
      snprintf(t, sizeof(t), "-%d%%", d);
      snprintf(m, sizeof(m), "%d", d * cfgCap / 100);
    } else {
      if (blog.start) { time_t st = blog.start; struct tm lt; localtime_r(&st, &lt); snprintf(t, sizeof(t), "%02d:%02d", lt.tm_hour, lt.tm_min); }
      else snprintf(t, sizeof(t), "--:--");
      if (blog.v0 > 0) snprintf(m, sizeof(m), "%.2f", blog.v0);
    }
    bool on = (i == sel);
    if (on) oled.fillRoundRect(1, y - 2, SCRW - 2 - 3, UI_ROW_H, 2, SSD1306_WHITE);
    oled.setTextColor(on ? SSD1306_BLACK : SSD1306_WHITE);
    at(UI_PAD + 1, y, lab[i]);
    at(84 - (int)strlen(t) * 6, y, t);                                   // the time column ends at 84
    if (m[0]) at(SCRW - UI_PAD - 5 - (int)strlen(m) * 6, y, m);           // mAh, clear of the scrollbar
    oled.setTextColor(SSD1306_WHITE);
  }
  uiScroll(first, NROWS);
  oled.display();
}

static void syncBegin() {""")
rep("static void syncBegin();\n", "static void syncBegin();\nstatic void blogTick();\nstatic void blogSave();\nstatic void blogReset();\nstatic void blogBoot(bool fromDeep);\nstatic void drawBattUse();\n")

# the loop, a wake, a deep sleep, the start
rep("""  nightTick();                         // 7.7: bedtime""",
    """  nightTick();                         // 7.7: bedtime
  blogTick();                          // 7.8: where the battery goes""")
rep("""static void wake(const char* why) {""", """static void wake(const char* why) {
  if (asleep && blog.magic == BLOG_MAGIC) blog.wakes++;      // 7.8""")
rep("""static void sleepNow(long secs) {""", """static void sleepNow(long secs) {
  rtcDeepAt = (uint32_t)time(nullptr);   // 7.8: the deep sleep is counted on waking
  blogSave();""")
rep("""  devLoad();
  filtersLoad();                       // 7.5""", """  devLoad();
  filtersLoad();                       // 7.5
  cfgBlog  = prefs.getBool("blog", true);
  cfgBlogV = constrain(prefs.getInt("blogv", 1), 0, 3);
  cfgCap   = constrain(prefs.getInt("cap", 350), 50, 5000);
  blogBoot(fromDeep);                  // 7.8""")

# =================================================================
# SETTINGS: a Battery group
# =================================================================
rep("""       C_MULTI, C_DEV1, C_DEV2, C_AUTOAWAY, C_NIGHT, C_BED, C_COUNT };""",
    """       C_MULTI, C_DEV1, C_DEV2, C_AUTOAWAY, C_NIGHT, C_BED,
       C_BLOG, C_BVIEW, C_BRESET, C_COUNT };""")
rep('''    "Multi-link", "Primary", "Second", "Auto away", "Night sleep", "Bedtime" };''',
    '''    "Multi-link", "Primary", "Second", "Auto away", "Night sleep", "Bedtime",
    "Battery log", "Battery use", "New log at" };''')
rep("""enum { SG_DISPLAY = 0, SG_TOUCH, SG_CONN, SG_SAFE, SG_SYSTEM, SG_COUNT,""",
    """enum { SG_DISPLAY = 0, SG_TOUCH, SG_CONN, SG_SAFE, SG_BATT, SG_SYSTEM, SG_COUNT,""")
rep('''const char* SG_NAME[SG_ALL] = { "Display", "Touch and motion", "Connections",
                                "Away and safety", "System", "Prayer settings" };''',
    '''const char* SG_NAME[SG_ALL] = { "Display", "Touch and motion", "Connections",
                                "Away and safety", "Battery", "System", "Prayer settings" };''')
rep("""  { C_UPDATE,   C_DEEP,  C_BATT,  C_NIGHT,  C_BED,    C_RESET,  C_REBOOT, C_ABOUT },""",
    """  { C_BVIEW,    C_BLOG,  C_BRESET, C_BATT,  C_DEEP,   C_NIGHT,  C_BED,   SG_END },
  { C_UPDATE,   C_RESET, C_REBOOT, C_ABOUT, SG_END,   SG_END,   SG_END,  SG_END },""")
rep("""    static const uint8_t* const ic[SG_COUNT] = { IC_SUN, IC_HAND, IC_BT, IC_SHIELD, IC_CHIP };""",
    """    static const uint8_t* const ic[SG_COUNT] = { IC_SUN, IC_HAND, IC_BT, IC_SHIELD, IC_BATT, IC_CHIP };""")
rep("static const uint8_t IC_CHIP[8]  = {", "static const uint8_t IC_BATT[8]  = { 0x00, 0xFC, 0x84, 0xB6, 0xB6, 0x84, 0xFC, 0x00 };\nstatic const uint8_t IC_CHIP[8]  = {")
rep("""      case C_NIGHT:  snprintf(v, sizeof(v), "%s", cfgNight ? "on" : "off"); break;""",
    """      case C_NIGHT:  snprintf(v, sizeof(v), "%s", cfgNight ? "on" : "off"); break;
      case C_BLOG:   snprintf(v, sizeof(v), "%s", cfgBlog ? "on" : "off"); break;
      case C_BVIEW:  snprintf(v, sizeof(v), "%s", cfgBlog ? "hold" : "off"); break;
      case C_BRESET: snprintf(v, sizeof(v), "%.2f V", BLOG_V[constrain(cfgBlogV, 0, 3)]); break;""")
rep("""      case C_NIGHT:
        cfgNight = !cfgNight; prefs.putBool("night", cfgNight);""", """      case C_BLOG:
        cfgBlog = !cfgBlog; prefs.putBool("blog", cfgBlog);
        if (cfgBlog && blog.magic != BLOG_MAGIC) blogReset();
        break;
      case C_BRESET:
        cfgBlogV = (cfgBlogV + 1) % 4; prefs.putInt("blogv", cfgBlogV); blogArmed = true;
        break;
      case C_BVIEW:
        if (cfgBlog) { subIdx = 0; depth = 2; }
        break;
      case C_NIGHT:
        cfgNight = !cfgNight; prefs.putBool("night", cfgNight);""")
rep("""  if (depth == 2 && itemIdx == C_TLOG)  { drawTlog();  return; }""",
    """  if (depth == 2 && itemIdx == C_TLOG)  { drawTlog();  return; }
  if (depth == 2 && itemIdx == C_BVIEW) { drawBattUse(); return; }   // 7.8""")
rep("""  if (screen == S_SETTINGS && depth == 2 && itemIdx == C_TLOG && g == TG_LONG) {""",
    """  if (screen == S_SETTINGS && depth == 2 && itemIdx == C_BVIEW) {    // 7.8: the battery log
    if (g == TG_ONE)  { subIdx = (subIdx + 1) % 9; return; }
    if (g == TG_LONG) { blogReset(); blogSave(); subIdx = 0; flash("NEW LOG", 900); return; }
  }
  if (screen == S_SETTINGS && depth == 2 && itemIdx == C_TLOG && g == TG_LONG) {""")
# the apps: settings, and the numbers in the status
rep("""    else if (k == "night") { cfgNight = v != 0; prefs.putBool("night", cfgNight); }""",
    """    else if (k == "night") { cfgNight = v != 0; prefs.putBool("night", cfgNight); }
    else if (k == "blog")  { cfgBlog = v != 0; prefs.putBool("blog", cfgBlog); if (cfgBlog && blog.magic != BLOG_MAGIC) blogReset(); }
    else if (k == "blogv") { cfgBlogV = constrain(v, 0, 3); prefs.putInt("blogv", cfgBlogV); blogArmed = true; }
    else if (k == "cap")   { cfgCap = constrain(v, 50, 5000); prefs.putInt("cap", cfgCap); }
    else if (k == "blogreset") { blogReset(); blogSave(); }""")
rep("""    char b[240];
    long tl = (tmrOn && !tmrDone)""", """    char b[420];
    long tl = (tmrOn && !tmrDone)""")
rep("""relax=%d;follow=%d;gest=%d;knob=%d;walk=%d;last=%s",""",
    """relax=%d;follow=%d;gest=%d;knob=%d;walk=%d;v=%.2f;ls=%d;lon=%lu;ldk=%lu;lls=%lu;ldp=%lu;lwf=%lu;lwk=%lu;lrs=%lu;lst=%lu;lp0=%d;last=%s",""")
rep("""             relaxOn ? 1 : 0, cfgFollow ? 1 : 0, cfgGesture ? 1 : 0, knobOn ? 1 : 0, walkOn ? 1 : 0, appLast);""",
    """             relaxOn ? 1 : 0, cfgFollow ? 1 : 0, cfgGesture ? 1 : 0, knobOn ? 1 : 0, walkOn ? 1 : 0,
             isnan(battV) ? 0.0f : battV, pmMode == 1 ? 1 : 0,
             (unsigned long)blog.sOn, (unsigned long)blog.sDark, (unsigned long)blog.sLight,
             (unsigned long)blog.sDeep, (unsigned long)blog.sWifi, (unsigned long)blog.wakes,
             (unsigned long)blog.restarts, (unsigned long)blog.start, (int)blog.pct0, appLast);""")

open(SRC, 'w').write(s)
print("7.8 ok")

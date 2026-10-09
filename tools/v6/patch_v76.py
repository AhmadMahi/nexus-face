#!/usr/bin/env python3
# Rafiq 7.6.0: the new way around. Five stops (Home, Today, Faith, Calm,
# Settings), each hub a short menu with icons; Settings regrouped; one set
# of design rules every new screen is drawn by. After patch_v75.
import sys
SRC = sys.argv[1]
s = open(SRC).read()
def rep(a, b, c=1):
    global s
    n = s.count(a)
    if n != c: sys.exit(f"anchor {n}x:\n{a[:220]}")
    s = s.replace(a, b)

rep('#define FW_VERSION "7.5.0"', '#define FW_VERSION "7.6.0"')

# =================================================================
# SCREENS: three hubs join the list; the walk is five stops
# =================================================================
rep("""       S_FAITH, S_READS, S_GAMES, S_SETTINGS, S_SYSTEM, S_COUNT };""",
    """       S_FAITH, S_READS, S_GAMES, S_SETTINGS, S_SYSTEM,
       S_TODAY, S_FHUB, S_CALM, S_COUNT };      // 7.6: the hubs""")
rep('''    "FAITH", "SHORT READS", "GAMES", "SETTINGS", "SYSTEM" };''',
    '''    "FAITH", "SHORT READS", "GAMES", "SETTINGS", "SYSTEM",
    "TODAY", "FAITH", "CALM" };''')
rep("""const int S_ORDER[] = { S_HOME, S_MSG, S_FOCUS, S_REMIND, S_BIKE, S_WEATHER, S_PRAYER,
                        S_FAITH, S_READS, S_GAMES, S_SETTINGS, S_SYSTEM };""",
    """// 7.6: five stops. Everything else lives inside a hub, which is what
// makes it quick to find: four choices to look at, not twelve.
const int S_ORDER[] = { S_HOME, S_TODAY, S_FHUB, S_CALM, S_SETTINGS };""")
rep("""static int nextScreen(int from) {
  int at = 0;""", """static bool prayerSoon(int mins);
static int nextScreen(int from) {
  // Home looks ahead: with a prayer close, Faith comes first.
  if (from == S_HOME && prayerSoon(15)) return S_FHUB;
  int at = 0;""")

# =================================================================
# THE DESIGN RULES, ICONS AND THE DRAWING EVERY NEW SCREEN USES
# =================================================================
rep("volatile uint8_t btNews = 0;", r"""volatile uint8_t btNews = 0;
// ================================================================
//  DESIGN RULES (7.6)
// ================================================================
//  One set of measures, so every screen reads as one product.
//    panel     128 x 64
//    padding   3 px each side, everywhere
//    bar       11 px: name left, time or count right
//    rows      12 px pitch, 4 visible, first text at y 14
//    icons     8 x 8 at x 3; text after them at x 15 (icon + 4 px)
//    hub icons the same icons, twice the size: one icon language
//    selected  a rounded bar 1 px in from each edge
//    values    right-aligned to the padding; 5 px further in with a scrollbar
//    scrollbar 2 px at the right edge, only past four rows
//    hint      centred on the bottom line, y 54
#define UI_PAD     3
#define UI_BAR_H   11
#define UI_ROW_Y   14
#define UI_ROW_H   12
#define UI_ROWS    4
#define UI_ICON    8
#define UI_TEXT_X  (UI_PAD + UI_ICON + 4)
#define UI_HINT_Y  54

// Eight by eight, one byte a row, leftmost pixel the top bit.
static const uint8_t IC_BELL[8]  = { 0x18, 0x3C, 0x7E, 0x7E, 0x7E, 0xFF, 0x00, 0x18 };
static const uint8_t IC_CHECK[8] = { 0xFF, 0x81, 0x83, 0x85, 0xA9, 0x91, 0x81, 0xFF };
static const uint8_t IC_MAC[8]   = { 0x00, 0x7E, 0x42, 0x42, 0x42, 0x7E, 0xFF, 0x00 };
static const uint8_t IC_SUN[8]   = { 0x10, 0x54, 0x38, 0xFE, 0x38, 0x54, 0x10, 0x00 };
static const uint8_t IC_CAR[8]   = { 0x00, 0x3C, 0x66, 0xFF, 0xFF, 0x66, 0x00, 0x00 };
static const uint8_t IC_MOON[8]  = { 0x3C, 0x70, 0xE0, 0xE0, 0xE0, 0x70, 0x3C, 0x00 };
static const uint8_t IC_BEADS[8] = { 0x3C, 0x42, 0x81, 0x81, 0x81, 0x42, 0x3C, 0x18 };
static const uint8_t IC_DAWN[8]  = { 0x00, 0x54, 0x38, 0x7C, 0x7C, 0xFF, 0x00, 0x00 };
static const uint8_t IC_BOOK[8]  = { 0x00, 0x66, 0x99, 0x99, 0x99, 0x99, 0xE7, 0x18 };
static const uint8_t IC_STAR[8]  = { 0x10, 0x10, 0x38, 0xFE, 0x38, 0x6C, 0x44, 0x00 };
static const uint8_t IC_SLIDE[8] = { 0x70, 0xFF, 0x70, 0x00, 0x0E, 0xFF, 0x0E, 0x00 };
static const uint8_t IC_LEAF[8]  = { 0x0E, 0x3F, 0x7E, 0x7E, 0xFC, 0xF8, 0x80, 0x00 };
static const uint8_t IC_PAGE[8]  = { 0xFE, 0x82, 0xBA, 0x82, 0xBA, 0x82, 0xFE, 0x00 };
static const uint8_t IC_PAD[8]   = { 0x00, 0x7E, 0xDD, 0x8F, 0xDB, 0x7E, 0x66, 0x00 };
static const uint8_t IC_HAND[8]  = { 0x20, 0x20, 0x2C, 0x3E, 0x7E, 0x7E, 0x3C, 0x00 };
static const uint8_t IC_BT[8]    = { 0x10, 0x18, 0x54, 0x38, 0x38, 0x54, 0x18, 0x10 };
static const uint8_t IC_SHIELD[8]= { 0x7E, 0x81, 0x81, 0x81, 0x42, 0x42, 0x24, 0x18 };
static const uint8_t IC_CHIP[8]  = { 0x54, 0x7C, 0xC6, 0x44, 0xC6, 0x7C, 0x54, 0x00 };

// ---- the hubs ----
enum { HI_SCREEN = 0, HI_FAITH, HI_ADHKAR, HI_RELAX, HI_PSET };
struct HubIt { uint8_t kind; uint8_t a; const uint8_t* icon; const char* name; };
int inHub = -1, hubSel = 0, hubEntry = 0;
""")
rep("static void syncBegin() {", r"""// An icon in the current colour, at 1x or 2x.
static void uiIcon(int x, int y, const uint8_t* ic, int scale, uint16_t col) {
  for (int j = 0; j < 8; j++)
    for (int i = 0; i < 8; i++)
      if (ic[j] & (0x80 >> i)) oled.fillRect(x + i * scale, y + j * scale, scale, scale, col);
}
// The scrollbar: a 2 px track, the thumb as long as the share in view.
static void uiScroll(int first, int total) {
  if (total <= UI_ROWS) return;
  const int top = UI_BAR_H + 2, h = SCRH - top - 1;
  int th = max(6, h * UI_ROWS / total);
  int ty = top + (h - th) * first / (total - UI_ROWS);
  oled.drawFastVLine(SCRW - 1, top, h, SSD1306_WHITE);
  oled.fillRect(SCRW - 2, ty, 2, th, SSD1306_WHITE);
}
// The first row in view, keeping the chosen one inside the window.
static int uiFirst(int sel, int total) {
  int first = sel > UI_ROWS - 1 ? sel - (UI_ROWS - 1) : 0;
  if (first > total - UI_ROWS) first = total - UI_ROWS;
  return first < 0 ? 0 : first;
}
// One row: icon (or none), label, value right-aligned, chosen or not.
static void uiRow(int r, const uint8_t* ic, const char* label, const char* value, bool on, bool scrolled) {
  int y = UI_ROW_Y + r * UI_ROW_H;
  uint16_t fg = on ? SSD1306_BLACK : SSD1306_WHITE;
  if (on) oled.fillRoundRect(1, y - 2, SCRW - 2 - (scrolled ? 3 : 0), UI_ROW_H, 2, SSD1306_WHITE);
  int tx = UI_PAD + 1;
  if (ic) { uiIcon(UI_PAD + 1, y, ic, 1, fg); tx = UI_TEXT_X + 1; }
  oled.setTextColor(fg);
  int room = (SCRW - tx - UI_PAD - (scrolled ? 5 : 0)) / 6;
  int vlen = value && *value ? (int)strlen(value) : 0;
  char b[22]; snprintf(b, sizeof(b), "%.*s", max(0, room - (vlen ? vlen + 1 : 0)), label);
  at(tx, y, b);
  if (vlen) at(SCRW - UI_PAD - (scrolled ? 5 : 0) - vlen * 6, y, value);
  oled.setTextColor(SSD1306_WHITE);
}

// ---- the hubs (state and kinds are with the design rules, at the top) ----
static bool isHub(int s) { return s == S_TODAY || s == S_FHUB || s == S_CALM; }
static int hubItems(int s, HubIt* o) {
  int n = 0;
  if (s == S_TODAY) {
    o[n++] = { HI_SCREEN, (uint8_t)S_MSG, IC_BELL, "Notifications" };
    o[n++] = { HI_SCREEN, (uint8_t)S_REMIND, IC_CHECK, "Reminders" };
    if (cardsAny()) o[n++] = { HI_SCREEN, (uint8_t)S_FOCUS, IC_MAC, "Mac" };
    if (screenOn_(S_WEATHER)) o[n++] = { HI_SCREEN, (uint8_t)S_WEATHER, IC_SUN, "Weather" };
    if (cfgBike) o[n++] = { HI_SCREEN, (uint8_t)S_BIKE, IC_CAR, "Vehicle" };
  } else if (s == S_FHUB) {
    o[n++] = { HI_SCREEN, (uint8_t)S_PRAYER, IC_MOON, "Prayer times" };
    o[n++] = { HI_FAITH, F_ZIKR, IC_BEADS, "Zikr" };
    o[n++] = { HI_ADHKAR, 0, IC_DAWN, "Adhkar" };
    o[n++] = { HI_FAITH, F_QURAN, IC_BOOK, "Quran" };
    o[n++] = { HI_FAITH, F_NAMES, IC_STAR, "99 Names" };
    o[n++] = { HI_PSET, 0, IC_SLIDE, "Prayer settings" };
  } else if (s == S_CALM) {
    o[n++] = { HI_RELAX, 0, IC_LEAF, "Relax" };
    o[n++] = { HI_SCREEN, (uint8_t)S_READS, IC_PAGE, "Short reads" };
    o[n++] = { HI_SCREEN, (uint8_t)S_GAMES, IC_PAD, "Games" };
  }
  return n;
}
static int hubCount(int s) { HubIt t[8]; return hubItems(s, t); }
static const uint8_t* hubIcon(int s) {
  return s == S_TODAY ? IC_BELL : s == S_FHUB ? IC_MOON : s == S_CALM ? IC_LEAF : IC_SLIDE;
}
static bool prayerSoon(int mins) {
  if (!prayerOk) return false;
  struct tm t; if (!nowLocal(&t)) return false;
  int now = t.tm_hour * 60 + t.tm_min;
  for (int i = 0; i < 5; i++) {
    int p = prayerAt(i); if (p < 0) continue;
    int d = p - now; if (d < 0) d += 1440;
    if (d <= mins) return true;
  }
  return false;
}
// A value for a menu row: the count or the reading that matters.
static void hubValue(const HubIt& it, char* v, size_t n) {
  v[0] = 0;
  if (it.kind != HI_SCREEN) return;
  if (it.a == S_MSG && noteUnread()) snprintf(v, n, "%d", noteUnread());
  else if (it.a == S_REMIND) { int k = 0; for (int i = 0; i < remCount; i++) if (!rems[i].done) k++; if (k) snprintf(v, n, "%d", k); }
  else if (it.a == S_WEATHER && wxOk) snprintf(v, n, "%dC", (int)lroundf(wTemp));
}

// The front of a hub (and of Settings): the icon twice the size, two
// lines of what is inside, and the hint. Same places on every hub.
static void uiHubCard(const char* name, const uint8_t* ic, const char* l1, const char* l2) {
  oled.clearDisplay();
  bar(name);
  const int iy = UI_BAR_H + (UI_HINT_Y - UI_BAR_H - 16) / 2;        // centred in the space above the hint
  uiIcon(UI_PAD + 5, iy, ic, 2, SSD1306_WHITE);
  const int tx = UI_PAD + 5 + 16 + 8;
  char b[18];
  bool two = l2 && *l2;
  // the text block (8 px a line, 2 px between) centred on the 16 px icon
  int ty = two ? iy - 1 : iy + 4;
  snprintf(b, sizeof(b), "%.15s", l1); at(tx, ty, b);
  if (two) { snprintf(b, sizeof(b), "%.15s", l2); at(tx, ty + 10, b); }
  ctr("hold to open", UI_HINT_Y, 1);
  oled.display();
}
static void drawHub() {
  char l1[24] = "", l2[24] = "";
  const char* nm = S_NAME[screen];
  if (depth == 0) {
    if (screen == S_TODAY) {
      int u = noteUnread();
      if (u) snprintf(l1, sizeof(l1), "%d new", u); else snprintf(l1, sizeof(l1), "All read");
      int k = 0; for (int i = 0; i < remCount; i++) if (!rems[i].done) k++;
      if (k) snprintf(l2, sizeof(l2), "%d reminder%s", k, k == 1 ? "" : "s");
      else if (wxOk) snprintf(l2, sizeof(l2), "%dC outside", (int)lroundf(wTemp));
    } else if (screen == S_FHUB) {
      struct tm t;
      if (prayerOk && nowLocal(&t)) {
        int now = t.tm_hour * 60 + t.tm_min, i = nextPrayer(now), p = prayerAt(i);
        int d = p - now; if (d < 0) d += 1440;
        snprintf(l1, sizeof(l1), "%s %02d:%02d", PRAYERS[i], p / 60, p % 60);
        if (d >= 60) snprintf(l2, sizeof(l2), "in %dh %02dm", d / 60, d % 60);
        else         snprintf(l2, sizeof(l2), "in %d min", d);
      } else { snprintf(l1, sizeof(l1), "Zikr, Quran"); snprintf(l2, sizeof(l2), "and adhkar"); }
    } else {
      snprintf(l1, sizeof(l1), "Relax, reads"); snprintf(l2, sizeof(l2), "and %d games", G_COUNT);
    }
    uiHubCard(nm, hubIcon(screen), l1, l2);
    return;
  }
  // the menu
  HubIt it[8]; int n = hubItems(screen, it);
  if (hubSel >= n) hubSel = 0;
  oled.clearDisplay();
  char cnt[24]; snprintf(cnt, sizeof(cnt), "%d/%d", hubSel + 1, n);
  titleBar(nm, cnt);
  int first = uiFirst(hubSel, n);
  bool sc = n > UI_ROWS;
  for (int r = 0; r < UI_ROWS && first + r < n; r++) {
    char v[16]; hubValue(it[first + r], v, sizeof(v));
    uiRow(r, it[first + r].icon, it[first + r].name, v, first + r == hubSel, sc);
  }
  uiScroll(first, n);
  oled.display();
}

// Into a hub's item. hubEntry is the depth it begins at, so "back" from
// there returns to the hub's menu rather than wandering further out.
static void hubEnter(int hubScr, int sel) {
  HubIt it[8]; int n = hubItems(hubScr, it);
  if (!n) return;
  if (sel < 0 || sel >= n) sel = 0;
  hubSel = sel; inHub = hubScr;
  const HubIt& x = it[sel];
  itemIdx = 0; subIdx = 0;
  switch (x.kind) {
    case HI_SCREEN: screen = x.a; depth = 0; hubEntry = 0; break;
    case HI_FAITH:
      screen = S_FAITH; depth = 2; itemIdx = x.a; hubEntry = 2;
      if (x.a == F_ZIKR) zikrReset();
      break;
    case HI_ADHKAR: {
      struct tm t; int h = nowLocal(&t) ? t.tm_hour : 8;
      screen = S_FAITH; depth = 2; itemIdx = (h >= 3 && h < 15) ? F_MORNING : F_EVENING; hubEntry = 2;
      break;
    }
    case HI_RELAX:
      relaxOn = true; relaxKind = 0; relaxNext = millis() + 30000UL;
      relaxUntil = millis() + RELAX_RQ_MS;
      wake("relax");
      break;
    case HI_PSET:
      screen = S_SETTINGS; depth = 1; setGrp = SG_FAITHSET; itemIdx = SG_ROWS[SG_FAITHSET][0]; hubEntry = 1;
      break;
  }
}

static void syncBegin() {""")
rep("static void syncBegin();\n", "static void syncBegin();\nstatic void drawHub();\nstatic bool isHub(int s);\nstatic int hubCount(int s);\nstatic void hubEnter(int hubScr, int sel);\nstatic void uiHubCard(const char* name, const uint8_t* ic, const char* l1, const char* l2);\nstatic void uiRow(int r, const uint8_t* ic, const char* label, const char* value, bool on, bool scrolled);\nstatic void uiScroll(int first, int total);\nstatic int uiFirst(int sel, int total);\n")
rep("""      case S_SYSTEM:   drawSystem();   break;""", """      case S_SYSTEM:   drawSystem();   break;
      case S_TODAY: case S_FHUB: case S_CALM: drawHub(); break;   // 7.6""")


# =================================================================
# MOVING: next, open, back
# =================================================================
rep("""  if (depth == 0) {
    screen = nextScreen(screen);
    itemIdx = 0; subIdx = 0;
    return;
  }""", """  // 7.6: in a hub's menu a tap moves the choice; inside an item, it
  // goes on to the next item of the same hub.
  if (isHub(screen)) {
    if (depth == 1) { int n = hubCount(screen); if (n) hubSel = (hubSel + 1) % n; return; }
    screen = nextScreen(screen); return;
  }
  if (inHub >= 0 && depth == 0 && screen != S_SETTINGS) {
    int n = hubCount(inHub);
    if (n) hubEnter(inHub, (hubSel + 1) % n);
    return;
  }
  if (depth == 0) {
    screen = nextScreen(screen);
    itemIdx = 0; subIdx = 0;
    return;
  }""")
rep("""static void knockTwo() {""", """static void knockTwoInner();
static void knockTwo() {
  // 7.6: a hub opens to its menu, and the menu opens the chosen item.
  if (!awayOn && !tmrOn && isHub(screen)) {
    if (depth == 0) { depth = 1; hubSel = 0; return; }
    hubEnter(screen, hubSel);
    return;
  }
  knockTwoInner();
}
static void knockTwoInner() {""")
rep("""static void knockThree() {
  cTriple++;
  if (awayOn) { lastActive = millis(); return; }   // away: nothing but the message
  if (tmrOn && !awayOn) { lastActive = millis(); return; }   // timer: nothing else""",
    """static void knockThree() {
  cTriple++;
  if (awayOn) { lastActive = millis(); return; }   // away: nothing but the message
  if (tmrOn && !awayOn) { lastActive = millis(); return; }   // timer: nothing else
  // 7.6: back from a hub's menu to its front, from its front to Home, and
  // from where an item began straight back to its hub's menu.
  if (isHub(screen)) { if (depth == 1) depth = 0; else { screen = S_HOME; depth = 0; } return; }
  if (inHub >= 0 && depth <= hubEntry &&
      !(screen == S_SETTINGS && depth == 1 && setGrp != SG_FAITHSET)) {
    int h = inHub; inHub = -1;
    screen = h; depth = 1;
    return;
  }""")
# Home's hold: straight into Today's menu
rep("""  // The clock has nothing to open, so going in means the faces.
  if (g == TG_LONG && screen == S_HOME && depth == 0 && timeOk) {
    faceMode = true;
    clickShrink();
    return;
  }""", """  // 7.6: a hold on Home goes straight to Today's menu: what is new is
  // always one hold away. (Faces are chosen in Settings, Display.)
  if (g == TG_LONG && screen == S_HOME && depth == 0) {
    screen = S_TODAY; depth = 1; hubSel = 0; inHub = -1;
    clickShrink();
    return;
  }""")
# leaving for Home forgets the hub
rep("""  appTick();                           // 7.2: what the Rafiq app sent""",
    """  appTick();                           // 7.2: what the Rafiq app sent
  if (screen == S_HOME) inHub = -1;    // 7.6: Home forgets which hub you were in""")

# =================================================================
# SETTINGS: five groups (and Prayer settings, opened from Faith)
# =================================================================
rep("""enum { SG_DISPLAY = 0, SG_WIRELESS, SG_DEVICES, SG_CONTROLS, SG_SYSTEM, SG_COUNT };
const char* SG_NAME[SG_COUNT] = { "Display", "Wireless", "Devices", "Controls", "System" };""",
    """// 7.6: grouped by what you are trying to do. Prayer settings is not in
// the list: it is opened from Faith, next to the prayer times it changes.
enum { SG_DISPLAY = 0, SG_TOUCH, SG_CONN, SG_SAFE, SG_SYSTEM, SG_COUNT,
       SG_FAITHSET = SG_COUNT, SG_ALL,
       SG_WIRELESS = SG_CONN, SG_DEVICES = SG_CONN, SG_CONTROLS = SG_TOUCH };
const char* SG_NAME[SG_ALL] = { "Display", "Touch and motion", "Connections",
                                "Away and safety", "System", "Prayer settings" };
""")
rep("""const uint8_t SG_ROWS[SG_COUNT][SG_MAX] = {
  { C_BRIGHT, C_FACE,    C_SLEEP, C_TURN,   C_POPUP,  C_EYES,   C_HIJRI, C_BIKE },
  { C_MODE,   C_HOTSPOT, C_PAIR,  C_PRAYER, C_UPDATE, C_GUARD,  SG_END,  SG_END },
  { C_MULTI,  C_DEV1,    C_DEV2,  C_AUTOAWAY, SG_END, SG_END,  SG_END,  SG_END },
  { C_WAKEBY, C_HOLD,    C_KNOCK, C_TAP,    C_SHAKE,  C_WAKEH,  C_ACCEL, SG_END },
  { C_CLOCK,  C_DEEP,    C_BATT,  C_TAMPER, C_TLOG,   C_RESET,  C_REBOOT, C_ABOUT },
};""", """const uint8_t SG_ROWS[SG_ALL][SG_MAX] = {
  { C_BRIGHT,   C_FACE,  C_CLOCK, C_SLEEP,  C_POPUP,  C_TURN,   C_EYES,  C_BIKE },
  { C_WAKEBY,   C_WAKEH, C_HOLD,  C_SHAKE,  C_KNOCK,  C_TAP,    C_ACCEL, SG_END },
  { C_MODE,     C_MULTI, C_DEV1,  C_DEV2,   C_PAIR,   C_HOTSPOT, SG_END, SG_END },
  { C_AUTOAWAY, C_GUARD, C_TAMPER, C_TLOG,  SG_END,   SG_END,   SG_END,  SG_END },
  { C_UPDATE,   C_DEEP,  C_BATT,  C_RESET,  C_REBOOT, C_ABOUT,  SG_END,  SG_END },
  { C_PRAYER,   C_HIJRI, SG_END,  SG_END,   SG_END,   SG_END,   SG_END,  SG_END },
};""")
# Settings' front: a hub card like the others
rep("""  if (depth == 0) {
    bar("SETTINGS");
    gearIcon(SCRW / 2, 32, 11);
    ctr("Hold to open", 52, 1);
    oled.display();
    return;
  }""", """  if (depth == 0) {                      // 7.6: the same card as every hub
    char l1[20], l2[20];
    if (!isnan(battV)) snprintf(l1, sizeof(l1), "Battery %d%%", battPct(battV));
    else               snprintf(l1, sizeof(l1), "No battery");
    snprintf(l2, sizeof(l2), "%s", linkN ? (linkN > 1 ? "2 linked" : "Linked") : "Not linked");
    uiHubCard("SETTINGS", IC_SLIDE, l1, l2);
    return;
  }""")
# the group list, by the rules, with icons
rep("""  if (depth == 1 && setGrp < 0) {
    bar("SETTINGS");
    int first = grpSel > 3 ? grpSel - 3 : 0;
    for (int g = first; g < SG_COUNT && g < first + 4; g++) {
      int y = 14 + (g - first) * 12;
      bool on = (g == grpSel);
      if (on) { oled.fillRect(0, y - 2, SCRW, 12, SSD1306_WHITE); oled.setTextColor(SSD1306_BLACK); }
      else      oled.setTextColor(SSD1306_WHITE);
      at(3, y, SG_NAME[g]);
      char n[4];
      snprintf(n, sizeof(n), "%d", sgLen(g));
      at(SCRW - 3 - (int)strlen(n) * 6, y, n);
    }
    oled.setTextColor(SSD1306_WHITE);
    oled.display();
    return;
  }""", """  if (depth == 1 && setGrp < 0) {
    static const uint8_t* const ic[SG_COUNT] = { IC_SUN, IC_HAND, IC_BT, IC_SHIELD, IC_CHIP };
    bar("SETTINGS");
    int first = uiFirst(grpSel, SG_COUNT);
    for (int r = 0; r < UI_ROWS && first + r < SG_COUNT; r++) {
      int g = first + r;
      uiRow(r, ic[g], SG_NAME[g], "", g == grpSel, SG_COUNT > UI_ROWS);   // names only: they need the room
    }
    uiScroll(first, SG_COUNT);
    oled.display();
    return;
  }""")
# the rows inside a group, by the same rules
rep("""    bool on = (i == itemIdx);
    if (on) { oled.fillRect(0, y - 2, SCRW, 12, SSD1306_WHITE); oled.setTextColor(SSD1306_BLACK); }
    else      oled.setTextColor(SSD1306_WHITE);
    at(3, y, C_NAME[i]);""", """    bool on = (i == itemIdx);
    const int sc = rows > UI_ROWS ? 5 : 0;          // 7.6: room for the scrollbar
    if (on) { oled.fillRoundRect(1, y - 2, SCRW - 2 - (sc ? 3 : 0), UI_ROW_H, 2, SSD1306_WHITE); oled.setTextColor(SSD1306_BLACK); }
    else      oled.setTextColor(SSD1306_WHITE);
    at(UI_PAD + 1, y, C_NAME[i]);""")
_a = """    oled.setCursor(SCRW - 3 - (int)strlen(v) * 6, y);
    oled.print(v);
  }
  oled.setTextColor(SSD1306_WHITE);
  oled.display();
}"""
_i = s.index(_a, s.index("at(UI_PAD + 1, y, C_NAME[i]);"))      # the one in the settings list
s = s[:_i] + """    // 7.6: never into the label, never under the scrollbar
    {
      const int sc = rows > UI_ROWS ? 5 : 0;
      int room = (SCRW - UI_PAD - sc - (UI_PAD + 1) - ((int)strlen(C_NAME[i]) + 1) * 6) / 6;
      if (room < 0) room = 0;
      if ((int)strlen(v) > room) v[room] = 0;
      oled.setCursor(SCRW - UI_PAD - sc - (int)strlen(v) * 6, y);
      oled.print(v);
    }
  }
  oled.setTextColor(SSD1306_WHITE);
  uiScroll(first, rows);
  oled.display();
}""" + s[_i + len(_a):]
# The bar: a long title keeps its room; the clock gives way (the rule).
rep("""static void bar(const char* title) {
  if (!timeOk) { titleBar(title, ""); return; }""", """static void bar(const char* title) {
  // 7.6: the title and the clock with a gap of one letter, or the title alone
  if (!timeOk || UI_PAD + (int)strlen(title) * 6 + 6 + 5 * 6 > SCRW - UI_PAD) { titleBar(title, ""); return; }""")
# every title in capitals, as on every other screen
rep("""  bar(depth == 2 ? "CHANGE" : SG_NAME[grp]);""", """  {
    char up[22]; snprintf(up, sizeof(up), "%s", depth == 2 ? "CHANGE" : SG_NAME[grp]);
    for (char* q = up; *q; q++) *q = toupper((unsigned char)*q);
    bar(up);
  }""")
# a label short enough for the longest of its values
rep('  { "Brightness", "Watch face", "Sleep after",', '  { "Brightness", "Face", "Sleep after",')
open(SRC, 'w').write(s)
print("7.6 ok")

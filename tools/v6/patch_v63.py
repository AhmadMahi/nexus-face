# Rafiq 6.3.0: lift and shake give a glance; notifications show the
# sender on top; the tap and hold hints become two solid buttons.
import re, sys
SRC=sys.argv[1]; s=open(SRC).read()
def rep(old,new,count=1):
    global s
    n=s.count(old)
    if n!=count: sys.exit(f"anchor {n}x:\n{old[:200]}")
    s=s.replace(old,new)

rep('#define FW_VERSION "6.2.0"', '#define FW_VERSION "6.3.0"')
rep("static void goSleepQuick();\n", "static void goSleepQuick();\nstatic void noteHeader(const Note& n, const char* right);\nstatic void twoButtons(const char* a, const char* b);\n")

# ---- 1. glance on lift and shake ----
rep("""  bool news = !strcmp(why, "notification") || !strcmp(why, "shortcut") || !strcmp(why, "guard") ||
              !strcmp(why, "update") || !strcmp(why, "sync") || !strcmp(why, "message");""",
"""  // Lifted or shaken, it is a look at the time and no more: the face
  // at once, and dark again shortly after unless the pad is touched.
  bool glance = !strcmp(why, "shake") || !strcmp(why, "picked up") || !strcmp(why, "moved");
  bool news = glance || !strcmp(why, "notification") || !strcmp(why, "shortcut") || !strcmp(why, "guard") ||
              !strcmp(why, "update") || !strcmp(why, "sync") || !strcmp(why, "message");
  glanceUntil = glance ? millis() + GLANCE_MS : 0;""")
rep("const char* banText = \"\";", """uint32_t    glanceUntil = 0;           // a lift's look at the time ends here
#define GLANCE_MS 1500UL
const char* banText = \"\";""")
# a touch turns a glance into a normal wake
rep("""          if (asleep) { if (touchWakes()) wake("touch"); touchTaps = 0; touchLongDone = true; }""",
    """          glanceUntil = 0;                       // a touch makes a glance a real wake
          if (asleep) { if (touchWakes()) wake("touch"); touchTaps = 0; touchLongDone = true; }""")
rep("""  if (banUntil && now > banUntil) banUntil = 0;""", """  if (glanceUntil && now > glanceUntil) {
    glanceUntil = 0;
    if (!popOn && !pgUntil && !findUntil && upState == U_OFF && alertPhase == AL_NONE) goSleepQuick();
  }
  if (banUntil && now > banUntil) banUntil = 0;""")

# ---- 2 and 3. sender on top, solid buttons ----
rep("""static void drawPopup() {
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
}""", """// The bar on top says who, and from which app; the body is the
// message. Two solid buttons at the bottom, apart, so what a tap and a
// hold do can be read at a glance.
static void noteHeader(const Note& n, const char* right) {
  const char* who = noteIsCall(n) ? (n.cat == CAT_MISSED ? "Missed call" : n.cat == CAT_VOICE ? "Voicemail" : "Calling")
                                  : (n.title[0] ? n.title : appShort(n));
  int room = (SCRW - 6 - (int)strlen(right) * 6 - 6) / 6;
  char l[24]; snprintf(l, sizeof(l), "%.*s", room > 0 ? (room < 23 ? room : 23) : 0, who);
  titleBar(l, right);
}
static void twoButtons(const char* a, const char* b) {
  oled.fillRoundRect(0, 53, 62, 11, 3, SSD1306_WHITE);
  oled.fillRoundRect(66, 53, 62, 11, 3, SSD1306_WHITE);
  oled.setTextSize(1); oled.setTextColor(SSD1306_BLACK);
  oled.setCursor((62 - (int)strlen(a) * 6) / 2, 55);      oled.print(a);
  oled.setCursor(66 + (62 - (int)strlen(b) * 6) / 2, 55); oled.print(b);
  oled.setTextColor(SSD1306_WHITE);
}
static void drawPopup() {
  oled.clearDisplay();
  if (!noteN) { popOn = false; return; }
  const Note& n = notes[0];
  char app[12]; snprintf(app, sizeof(app), "%.8s", appShort(n));
  noteHeader(n, app);
  if (noteIsCall(n)) marquee(n.title[0] ? n.title : "unknown", 26, 1);
  else               fitText(n.msg[0] ? n.msg : "(no text)", 14, 50, n.at);
  twoButtons("tap:close", "hold:open");
  oled.display();
}""")
rep("""  titleBar(appShort(n), when);
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
  oled.display();""", """  noteHeader(n, when);
  {
    char app[22]; snprintf(app, sizeof(app), "%s", appShort(n));
    ctr(app, 14, 1);
  }
  if (noteIsCall(n)) marquee(n.title[0] ? n.title : "unknown", 30, 1);
  else               fitText(n.msg[0] ? n.msg : "(no text)", 24, 50, n.at);
  char pos[32];
  snprintf(pos, sizeof(pos), "tap:%d/%d", noteIdx + 1, noteN);
  twoButtons(pos, "hold:clear");
  oled.display();""")
open(SRC,'w').write(s); print("6.3 ok")

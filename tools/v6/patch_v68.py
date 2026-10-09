# Rafiq 6.8.0: Away shows its message (it was going dark at once), and
# opens and closes its eyes quickly around it.
import sys
SRC=sys.argv[1]; s=open(SRC).read()
def rep(old,new,count=1):
    global s
    n=s.count(old)
    if n!=count: sys.exit(f"anchor {n}x:\n{old[:200]}")
    s=s.replace(old,new)
rep('#define FW_VERSION "6.7.0"', '#define FW_VERSION "6.8.0"')

# 1. the bug: loop's "now" is older than the lastActive wake() just set,
#    so "now - lastActive" wrapped to forty-nine days and Away went dark
#    the moment it began. Signed, and read fresh.
rep("""    if (now - lastActive > awayShowMs) { awayShowMs = AWAY_SHOW_MS; goSleepQuick(); return; }""",
    """    if ((int32_t)(millis() - lastActive) > (int32_t)awayShowMs) {
      awayShowMs = AWAY_SHOW_MS;
      awayEyes(false);                   // eyes close, then dark
      goSleepQuick();
      return;
    }""")

# 2. quick eyes: open about 0.2 s, close about 0.2 s
rep("static void awayDeepGo() {", """// Away's eyes: open or close in six frames, about a fifth of a second.
static void awayEyes(bool open) {
  for (int i = 0; i <= 5; i++) {
    int pct = open ? i * 20 : 100 - i * 20;
    oled.clearDisplay();
    calmEyes(pct, 0, 30);
    oled.display();
    delay(30);
  }
  if (open) delay(120);                  // a beat with them open, then the message
}
static void awayDeepGo() {""")
rep("static void awayDeepGo();\n", "static void awayDeepGo();\nstatic void awayEyes(bool open);\n")

rep("""  if (awayOn) { lastActive = millis(); lastDraw = 0; }   // the message straight away, from now""",
    """  if (awayOn) { awayEyes(true); lastActive = millis(); lastDraw = 0; }   // eyes open, then the message""")
rep("""  if (awayOn) { goSleepQuick(); return; }  // Away: dark at once, no closing eyes""",
    """  if (awayOn) { awayEyes(false); goSleepQuick(); return; }  // Away: eyes close quickly, then dark""")

# 3. waking from Away's deep sleep: eyes, message, eyes, off
rep("""    applyBright();
    oled.clearDisplay();
    fitText(awayText.c_str(), 2, 50, 0);""", """    applyBright();
    awayEyes(true);
    oled.clearDisplay();
    fitText(awayText.c_str(), 2, 50, 0);""")
rep("""    delay(AWAY_DEEP_SHOW_MS);
    oled.clearDisplay(); oled.display();
    oled.ssd1306_command(SSD1306_DISPLAYOFF);""", """    delay(AWAY_DEEP_SHOW_MS);
    awayEyes(false);
    oled.clearDisplay(); oled.display();
    oled.ssd1306_command(SSD1306_DISPLAYOFF);""")
open(SRC,'w').write(s); print("6.8 ok")

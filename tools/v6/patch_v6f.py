# 6.0.1: watch face home on Bluetooth, wider RAFIQ detection with a
# visible reason when one is refused, a proper sync-failed card, iPhone
# linked/unlinked notices, and the reset reason on screen.
import sys
SRC=sys.argv[1]; s=open(SRC).read()
def rep(old,new,count=1):
    global s
    n=s.count(old)
    if n!=count: sys.exit(f"anchor {n}x:\n{old[:160]}")
    s=s.replace(old,new)

rep('#define FW_VERSION "6.0.0"', '#define FW_VERSION "6.0.1"')

# 1. home: on Bluetooth with a clock, home is the watch face
rep("""  if (offlineNow()) {
    char hi[34];""", """  // 6.0.1: on Bluetooth the phone gives it the time, and with the
  // time, home is the watch face, exactly as it was on WiFi. The hello
  // and pairing screens are only for when there is no clock yet.
  if (offlineNow() && !(cfgNet == NET_BT && fb.ok)) {
    char hi[34];""")

# 2. RAFIQ: any Shortcuts process counts, and a refusal says why
rep("""static bool rafiqIs(const Note& n) {
  bool sc = !strncmp(n.app, "com.apple.shortcuts", 19) || !strncmp(n.app, "is.workflow", 11);
  return sc && (rqTagged(n.title) || rqTagged(stSub) || rqTagged(stMsg));
}""", """// Shortcuts posts from more than one process: the app itself
// (com.apple.shortcuts), the old Workflow id, and the background runner
// that automations and widgets use (com.apple.WorkflowKit...). 6.0
// only knew the first, so commands from an automation arrived as an
// ordinary notification titled RAFIQ and did nothing.
static bool rqFromShortcuts(const char* app) {
  char l[40]; int n = 0;
  for (const char* p = app; *p && n < 39; p++) l[n++] = tolower((unsigned char)*p);
  l[n] = 0;
  return strstr(l, "shortcut") || strstr(l, "workflow");
}
static bool rafiqTagged(const Note& n) {
  return rqTagged(n.title) || rqTagged(stSub) || rqTagged(stMsg);
}
static bool rafiqIs(const Note& n) {
  return rqFromShortcuts(n.app) && rafiqTagged(n);
}""")
rep("""    if (rafiqIs(noteStage)) rafiqNote(noteStage);
    else {""", """    if (rafiqIs(noteStage)) rafiqNote(noteStage);
    else {
      // Says RAFIQ but did not come from Shortcuts. Kept as a
      // notification, with the app it came from, so a Shortcut that is
      // being refused shows you why instead of doing nothing quietly.
      if (rafiqTagged(noteStage))
        snprintf(noteStage.msg, sizeof(noteStage.msg), "Not run, from %s", noteStage.app);""")

# 3. a sync with nowhere to go says so properly
rep("""  if (kind != WS_HOTSPOT && !netCount) { flash("NO SAVED WIFI", 1500); return false; }""",
    """  if (kind != WS_HOTSPOT && !netCount) {
    wsFailCard("No WiFi saved yet", "RAFIQ config to add");
    return false;
  }""")
rep("""  if (netDown && !rescueAP) {
    bool wasUpd = upAfterJoin;
    wsEnd("NO WIFI FOUND");""", """  if (netDown && !rescueAP) {
    bool wasUpd = upAfterJoin;
    bool wasSync = syncRun;
    wsEnd("");
    if (wasSync || wsKind == WS_NONE) wsFailCard("No saved WiFi nearby", "Back on Bluetooth");""")
rep("""    if (wsKind == WS_SYNC) { wsEnd(online() ? "SYNC PARTLY DONE" : "NO WIFI FOUND"); return; }""",
    """    if (wsKind == WS_SYNC) {
      bool on = online();
      wsEnd(on ? "SYNC PARTLY DONE" : "");
      if (!on) wsFailCard("No saved WiFi nearby", "Back on Bluetooth");
      return;
    }""")
rep("static void syncBegin() {", """// A card, not a flash: it stays long enough to read, and a press
// dismisses it. Two lines of 21.
static void wsFailCard(const char* a, const char* b) {
  wake("sync");
  toastKind = "syncfail";
  toastText = String(a) + "\\n" + b;
  toastUntil = millis() + 8000; toastFlash = millis(); remShowing = -1;
}

static void syncBegin() {""")
rep("""  if (toastKind == "rafiq")  head = "RAFIQ";""", """  if (toastKind == "rafiq")  head = "RAFIQ";
  if (toastKind == "syncfail") head = "SYNC FAILED";""")
rep("static void syncBegin();\n", "static void syncBegin();\nstatic void wsFailCard(const char* a, const char* b);\nstatic bool rafiqTagged(const Note& n);\n")

# 4. iPhone linked / unlinked, said when awake, never waking it
rep("""static void btSet(int stage) {
  if (btStage == stage) return;
  btStage = stage;""", """static void btSet(int stage) {
  if (btStage == stage) return;
  // Linked and unlinked are news worth a word when someone is looking.
  // Set here, said from loop: this runs on the NimBLE task.
  if (stage == BT_BONDED) btNews = 1;
  else if (btStage == BT_BONDED && stage == BT_ADVERTISING) btNews = 2;
  btStage = stage;""")
rep("int cfgNetHome = NET_BT;\n", "int cfgNetHome = NET_BT;\nvolatile uint8_t btNews = 0;          // 1 linked, 2 unlinked, for loop to say\n")
rep("""  serviceWs();                         // WiFi sessions, syncs, commands waiting""",
    """  serviceWs();                         // WiFi sessions, syncs, commands waiting
  if (btNews) {
    uint8_t k = btNews; btNews = 0;
    if (!asleep && cfgNet == NET_BT) flash(k == 1 ? "IPHONE CONNECTED" : "IPHONE DISCONNECTED", 1300);
  }""")

# 5. why it last restarted, kept and said
rep("""    esp_reset_reason_t rr = esp_reset_reason();
    bool crashed = (rr == ESP_RST_PANIC   || rr == ESP_RST_INT_WDT ||
                    rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT);""",
"""    esp_reset_reason_t rr = esp_reset_reason();
    bool crashed = (rr == ESP_RST_PANIC   || rr == ESP_RST_INT_WDT ||
                    rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT);
    // 6.0.1: a restart nobody asked for is said on the screen at boot,
    // and kept for RAFIQ status, so "it rebooted" can be told apart
    // from "it went to sleep and woke up".
    lastReset = rr == ESP_RST_POWERON ? "power on" : rr == ESP_RST_DEEPSLEEP ? "sleep wake"
              : rr == ESP_RST_SW ? "restart" : rr == ESP_RST_PANIC ? "crash"
              : (rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT) ? "watchdog"
              : rr == ESP_RST_BROWNOUT ? "low power" : rr == ESP_RST_EXT ? "reset pin" : "other";
    if (crashed)                  bootNote = rr == ESP_RST_PANIC ? "RESTARTED: CRASH" : "RESTARTED: WATCHDOG";
    else if (rr == ESP_RST_BROWNOUT) bootNote = "RESTARTED: LOW POWER";""")
rep("int cfgNetHome = NET_BT;\n", "int cfgNetHome = NET_BT;\nconst char* lastReset = \"\";\nconst char* bootNote = nullptr;\n")
rep("""  if (tamperWas) flash("TAMPER OFF, SEE LOG", 1800);""",
    """  if (tamperWas) flash("TAMPER OFF, SEE LOG", 1800);
  if (bootNote) flash(bootNote, 2500);
  Serial.printf("last reset: %s, light sleep: %s\\n", lastReset, pmAvail ? "yes" : "no");""")

# status card: light sleep on offer, and the last reset
rep("""  snprintf(b, sizeof(b), "BT %.8s %d%%\\nsync %s%s%s",
           btShort(), isnan(battV) ? 0 : battPct(battV),
           sy, cfgPGuard ? " guard" : "", cfgQuiet ? " quiet" : "");""",
"""  snprintf(b, sizeof(b), "BT %.8s %d%% %s\\nsync %s %.10s",
           btShort(), isnan(battV) ? 0 : battPct(battV), pmAvail ? "LS" : "",
           sy, lastReset);""")
open(SRC,'w').write(s); print("part 6 (6.0.1) ok")

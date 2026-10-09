#!/usr/bin/env python3
# Rafiq 6.0.0: Bluetooth first. Applies every edit to a clean copy of
# v5.20.0 and fails loudly if any anchor does not match exactly once.
import re, sys
SRC = sys.argv[1]
s = open(SRC).read()

def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f"anchor matched {n} times, wanted {count}:\n{old[:200]}")
    s = s.replace(old, new)

def rep_re(pat, new):
    global s
    m = list(re.finditer(pat, s, re.S))
    if len(m) != 1:
        sys.exit(f"regex matched {len(m)} times:\n{pat[:200]}")
    s = s[:m[0].start()] + new + s[m[0].end():]

# ---------------------------------------------------------------- version
rep('#define FW_VERSION "5.20.0"', '#define FW_VERSION "6.0.0"')

rep(""" RAFIQ  -  ESP32-C3 desk companion
  ================================================================
""", """ RAFIQ  -  ESP32-C3 desk companion
  ================================================================
   6.0  Bluetooth first. The phone is home: its clock, its
        notifications, and RAFIQ commands sent from a Shortcut. WiFi
        is something you ask for (Settings, the page, or RAFIQ sync /
        wifi / update / config) and it goes away again by itself.
        Light sleep keeps the phone linked with the screen dark, on a
        core built with power management. See HANDOFF.md.
""")

# ---------------------------------------------------------------- includes
rep("#include <esp_ota_ops.h>\n", "#include <esp_ota_ops.h>\n#include \"esp_pm.h\"\n#include \"driver/usb_serial_jtag.h\"\n")

# ---------------------------------------------------------------- globals
rep("static bool offlineNow() { return cfgOffline || netDown; }\n",
r"""static bool offlineNow() { return cfgOffline || netDown; }

// ---------------- 6.0: Bluetooth first ----------------
//  Bluetooth is home. WiFi is something you ask for, for a while, and
//  then it goes away again by itself.
//
//  cfgNet is what the radios are doing right now, and the rest of the
//  file goes on asking it exactly as before. cfgNetHome is where it
//  comes back to, and is the only one kept in flash: Bluetooth, or
//  Off. WiFi is never kept, so a real restart always lands on
//  Bluetooth.
//
//  A WiFi session is one of four kinds:
//    MANUAL   you asked for WiFi. Ends on a real restart, or thirty
//             minutes after the page or the Mac last used it. Deep
//             sleep and waking again does not end it.
//    SYNC     RAFIQ sync. Join, fetch everything, check for an update,
//             write a read, radio off.
//    UPDATE   Check update, asked for while on Bluetooth. Ends when
//             you leave the update screen.
//    HOTSPOT  The setup hotspot, for an update from a file. Ends ten
//             minutes after the last phone leaves it.
extern bool wxOk;                      // declared with the weather, further down
int cfgNetHome = NET_BT;
enum { WS_NONE = 0, WS_MANUAL, WS_SYNC, WS_UPDATE, WS_HOTSPOT };
int      wsKind    = WS_NONE;
uint32_t wsStartMs = 0;
uint32_t wsLastUse = 0;
volatile bool wsEndWant = false;       // asked for from a web handler, done from loop
bool     wsSawUp   = false;            // the update screen has been up this session
#define WS_IDLE_S          1800UL      // thirty minutes
#define WS_SYNC_MS       150000UL      // the whole of a sync, joining included
#define WS_ASK_MS        180000UL      // an update question nobody answers
#define WS_HOTSPOT_IDLE_MS 600000UL
// Kept through deep sleep, lost on any real restart. That is the whole
// rule for when manual WiFi ends, carried by where the number lives.
RTC_DATA_ATTR uint32_t rtcWsUntil = 0;   // seconds on the system clock
RTC_DATA_ATTR uint8_t  rtcWsKind  = 0;

bool     syncRun = false, syncStarted = false, syncStoryDone = false, syncUpArmed = false;
uint32_t syncAt = 0;
bool     upQuick = false;              // a No ends the whole question
bool     upAfterJoin = false;          // Check update, waiting for a network
bool     upDirect = false;             // and go straight to the newest one
uint32_t lastSyncAt = 0;               // system clock seconds, 0 never
uint32_t askSince = 0;

// Commands that change the radios wait a moment, so the answer to the
// phone (clear that notification) goes out before the radio does.
enum { RQ_NONE = 0, RQ_SYNC, RQ_UPDATE, RQ_WIFI, RQ_HOTSPOT, RQ_TAMPER, RQ_DEEP, RQ_REBOOT };
int      rqPend = RQ_NONE;
uint32_t rqPendAt = 0;

// Weather from the phone or the last sync, kept in flash, because on
// Bluetooth deep sleep is every time the phone is not around.
uint32_t wxAt = 0;                     // system clock seconds it arrived
volatile bool wxDirty = false;

// Notifications still land in the list, but wake nothing.
bool cfgQuiet = false;

// The phone guard. Going away from the phone and the phone going away
// from you are the same event, seen from here.
bool     cfgPGuard = false;
bool     pgEver = false;               // linked at least once since Bluetooth started
uint32_t pgLostAt = 0, pgWeakSince = 0, pgRssiAt = 0, pgUntil = 0;
float    pgRssi = 0;                   // smoothed; 0 means not measured yet
bool     pgFired = false;
const char* pgWhy = "";
#define PG_LOST_MS   4000UL
#define PG_WEAK_DBM  (-88)
#define PG_OK_DBM    (-80)
#define PG_WEAK_MS   6000UL
#define PG_SHOW_MS 120000UL

// Find me: the screen calls out for twenty seconds.
uint32_t findUntil = 0;

// Tamper. Armed, it goes dark and quiet and writes down what happens
// to it. Only a real restart disarms it.
uint32_t tamperCountAt = 0;            // the countdown, 0 when not counting
#define TAMPER_COUNT_MS 10000UL
RTC_DATA_ATTR uint8_t rtcTamper = 0;   // 1 armed, 2 pausing after a move
RTC_DATA_ATTR char    rtcTz[32] = "";
#define TLOG_PATH "/tamper.txt"
#define TLOG_MAX  4096
#define TL_N 24
char tlLines[TL_N][24];
int  tlN = 0, tlSel = 0;

// Light sleep. Only on a core built with power management; a stock
// core says no to esp_pm_configure and the robot behaves exactly as
// 5.20 did, off the moment the screen goes dark on Bluetooth.
bool pmAvail = false;
int  pmMode  = -1;
volatile uint32_t btDropAt = 0;        // when the phone last went
#define BT_DEEP_GRACE_MS 60000UL       // no phone for this long, then off

static bool wsStart(int kind);
static void wsEnd(const char* why);
static void wsTouch();
static void serviceWs();
static void syncBegin();
static void pmSet(bool idle);
static bool phoneHeld();
static void pgTick();
static void tlogAdd(const char* what);
static void tamperArm();
static void serviceTamper();
static void rafiqNote(const Note& n);
static bool rafiqIs(const Note& n);
static void saveWx();
""")

# ---------------------------------------------------------------- ANCS buffers
rep("uint8_t  dsBuf[512];\n",
"""uint8_t  dsBuf[768];
//  The longer parts of whatever is being parsed. The list keeps a
//  hundred characters of a message; a RAFIQ command or a weather
//  report from a Shortcut needs the whole of it, and when it was sent.
char     stMsg[404];
char     stSub[34];
char     stDate[20];
""")
rep("#define ANCS_ATTRS 3\n", "#define ANCS_ATTRS 5\n")

rep("""  n.cat = dsCat;
  size_t p = 5;""", """  n.cat = dsCat;
  stMsg[0] = stSub[0] = stDate[0] = 0;
  size_t p = 5;""")
rep("""    else if (id == 3) { dst = n.msg;   cap = sizeof(n.msg); }
    if (dst) { size_t k = L < cap - 1 ? L : cap - 1; memcpy(dst, dsBuf + p + 3, k); dst[k] = 0; }""",
"""    else if (id == 2) { dst = stSub;   cap = sizeof(stSub); }
    else if (id == 3) { dst = stMsg;   cap = sizeof(stMsg); }
    else if (id == 5) { dst = stDate;  cap = sizeof(stDate); }
    if (dst) { size_t k = L < cap - 1 ? L : cap - 1; memcpy(dst, dsBuf + p + 3, k); dst[k] = 0; }
    if (id == 3) { strncpy(n.msg, stMsg, sizeof(n.msg) - 1); n.msg[sizeof(n.msg) - 1] = 0; }""")

rep("""    0x00,                               // app identifier   )
    0x01, (uint8_t)(sizeof(((Note*)0)->title) - 2), 0,   //  ) ANCS_ATTRS
    0x03, (uint8_t)(sizeof(((Note*)0)->msg) - 4), 0 };   //  ) of them""",
"""    0x00,                               // app identifier   )
    0x01, (uint8_t)(sizeof(((Note*)0)->title) - 2), 0,   //  )
    0x02, (uint8_t)(sizeof(stSub) - 2), 0,                //  ) ANCS_ATTRS
    0x03, (uint8_t)((sizeof(stMsg) - 4) & 0xFF),          //  ) of them
          (uint8_t)((sizeof(stMsg) - 4) >> 8),
    0x05 };                             // the date, for RAFIQ""")

# disconnect time, for the grace before deep sleep
rep("""  void onDisconnect(NimBLEServer* sv, NimBLEConnInfo& ci, int reason) override {
    btConn = 0xFFFF;""", """  void onDisconnect(NimBLEServer* sv, NimBLEConnInfo& ci, int reason) override {
    btConn = 0xFFFF;
    btDropAt = millis();""")

# notifications: RAFIQ commands first, quiet mode, and do not light the
# screen for a reconnect nobody asked to see
rep("""  if (btWokeReq) { btWokeReq = false; wake("phone"); }

  // Handed over whole, so the screen never sees half a notification.
  if (noteReady) {
    noteReady = false;
    addNote(noteStage);
    wake("notification");
    if (popupSecs()) {
      screen = S_MSG; depth = 0;
      popupUntil = millis() + popupSecs() * 1000UL;
    }
  }""", """  // A phone coming back to a robot it already knows is not news, and
  // lighting the screen for it every time it walks back into range is
  // a battery spent on nothing. Pairing for the first time still shows.
  if (btWokeReq) {
    btWokeReq = false;
    if (!asleep || NimBLEDevice::getNumBonds() == 0) wake("phone");
  }

  // Handed over whole, so the screen never sees half a notification.
  if (noteReady) {
    noteReady = false;
    if (rafiqIs(noteStage)) rafiqNote(noteStage);
    else {
      addNote(noteStage);
      if (!cfgQuiet) {
        wake("notification");
        if (popupSecs()) {
          screen = S_MSG; depth = 0;
          popupUntil = millis() + popupSecs() * 1000UL;
        }
      }
    }
  }""")

open(SRC, 'w').write(s)
print("part 1 ok")

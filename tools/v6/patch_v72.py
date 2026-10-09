#!/usr/bin/env python3
# Rafiq 7.2.0: a Rafiq Bluetooth service, so an Android app (and later
# the Mac app) can send commands, notifications and the time, and read
# status. The iPhone path is untouched. Applied after patch_v71.
import sys
SRC = sys.argv[1]
s = open(SRC).read()
def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count: sys.exit(f"anchor {n}x:\n{old[:220]}")
    s = s.replace(old, new)

rep('#define FW_VERSION "7.1.0"', '#define FW_VERSION "7.2.0"')

# ---------------------------------------------------------------- globals
rep("volatile uint8_t btNews = 0;", """volatile uint8_t btNews = 0;
// ---- 7.2: the Rafiq service ----
//  An iPhone speaks to Rafiq through Apple's own services. Everything
//  else (the Android app now, the Mac app later) speaks through this
//  one. Four characteristics, all needing a bonded, encrypted link:
//    CMD   write  a RAFIQ command, exactly as a Shortcut would send it
//    NOTE  write  a notification: category, app, title, text (0x1F apart)
//    TIME  write  8 bytes, the phone's wall clock in seconds, little end
//    STAT  read   a line of key=value pairs about the robot
#define RQ_SVC  "52a1f000-7a3e-4b5c-9d6f-0a1b2c3d4e5f"
#define RQ_CMD  "52a1f001-7a3e-4b5c-9d6f-0a1b2c3d4e5f"
#define RQ_NOTE "52a1f002-7a3e-4b5c-9d6f-0a1b2c3d4e5f"
#define RQ_TIME "52a1f003-7a3e-4b5c-9d6f-0a1b2c3d4e5f"
#define RQ_STAT "52a1f004-7a3e-4b5c-9d6f-0a1b2c3d4e5f"
struct AppMsg { uint8_t kind; uint16_t len; char data[420]; };
#define APPQ_N 4
AppMsg appQ[APPQ_N];
volatile uint8_t appHead = 0, appTail = 0;   // written by NimBLE, read by loop
volatile bool btApp = false;           // the phone on the line runs the Rafiq app
extern bool timeOk;                    // both declared with the clock, further down
extern const char* clockSrc;
uint32_t appNoteSeq = 0;""")

# the callbacks and the service, ahead of bleOn
rep("static void bleOn() {", r"""// NimBLE's task only queues; loop does the work, as for ANCS.
class RqChrCb : public NimBLECharacteristicCallbacks {
 public:
  explicit RqChrCb(uint8_t k) : kind(k) {}
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
    btApp = true;                      // only the app writes here
    uint8_t next = (appHead + 1) % APPQ_N;
    if (next == appTail) return;       // full: drop, the app will resend
    AppMsg& m = appQ[appHead];
    NimBLEAttValue v = c->getValue();
    size_t n = v.length() < sizeof(m.data) - 1 ? v.length() : sizeof(m.data) - 1;
    memcpy(m.data, v.data(), n); m.data[n] = 0;
    m.len = (uint16_t)n; m.kind = kind;
    appHead = next;
  }
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
    char b[180];
    long tl = (tmrOn && !tmrDone) ? (long)(tmrEnd - millis()) / 1000 : 0;
    if (tl < 0) tl = 0;
    snprintf(b, sizeof(b),
             "fw=%s;bat=%d;away=%d;timer=%ld;unread=%d;quiet=%d;guard=%d;wake=%d;h12=%d;clock=%d",
             FW_VERSION, isnan(battV) ? -1 : battPct(battV), awayOn ? 1 : 0, tl,
             noteUnread(), cfgQuiet ? 1 : 0, cfgPGuard ? 1 : 0, cfgWakeBy, cfg12h ? 1 : 0,
             timeOk ? 1 : 0);
    c->setValue((const uint8_t*)b, strlen(b));
  }
 private:
  uint8_t kind;
};

static void rqServiceAdd(NimBLEServer* sv) {
  NimBLEService* svc = sv->createService(RQ_SVC);
  const uint32_t WR = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC;  // not W: RoboEyes owns W
  svc->createCharacteristic(RQ_CMD,  WR)->setCallbacks(new RqChrCb(1));
  svc->createCharacteristic(RQ_NOTE, WR)->setCallbacks(new RqChrCb(2));
  svc->createCharacteristic(RQ_TIME, WR)->setCallbacks(new RqChrCb(3));
  svc->createCharacteristic(RQ_STAT, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC)
     ->setCallbacks(new RqChrCb(4));
}

// What the app sent, handled on loop.
static void appTick() {
  while (appTail != appHead) {
    AppMsg& m = appQ[appTail];
    if (m.kind == 1) {                             // a command
      Serial.printf("app: %s\n", m.data);
      if (!strcmp(m.data, "ping")) { }             // the app saying hello
      else rafiqPayload(m.data, true);             // fresh, it was sent just now
    } else if (m.kind == 2) {                      // a notification
      // cat \x1F app \x1F title \x1F text
      char* f[4] = { m.data, nullptr, nullptr, nullptr };
      int k = 1;
      for (char* p = m.data; *p && k < 4; p++) if (*p == 0x1F) { *p = 0; f[k++] = p + 1; }
      if (k == 4) {
        Note n = {};
        n.uid = 0x40000000UL | (++appNoteSeq & 0x3FFFFFFFUL);   // never an ANCS id
        n.cat = (uint8_t)atoi(f[0]);
        n.unread = true;
        n.at = millis();
        strncpy(n.app,   f[1], sizeof(n.app) - 1);
        strncpy(n.title, f[2], sizeof(n.title) - 1);
        strncpy(n.msg,   f[3], sizeof(n.msg) - 1);
        addNote(n);
        if (!cfgQuiet) popupShow();
      }
    } else if (m.kind == 3 && m.len >= 8) {        // the time
      int64_t wall = 0;
      for (int i = 7; i >= 0; i--) wall = (wall << 8) | (uint8_t)m.data[i];
      if (wall > 1700000000LL) {
        struct timeval tv = { .tv_sec = (time_t)wall, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
        setenv("TZ", "UTC0", 1); tzset();          // a wall clock, as from an iPhone
        timeOk = true; clockSrc = "your phone";
        ctsState = CTS_DONE; ctsSyncedAt = millis();
        Serial.println("clock: set by the app");
      }
    }
    appTail = (appTail + 1) % APPQ_N;
  }
}

static void bleOn() {""")
rep("static void syncBegin();\n", "static void syncBegin();\nstatic void appTick();\nstatic void rafiqPayload(const char* p, bool fresh);\n")

# add the service with the others
rep("""  if (!isnan(battV)) btHid->setBatteryLevel(battPct(battV));
  sv->start();                         // starts the services too""",
"""  if (!isnan(battV)) btHid->setBatteryLevel(battPct(battV));
  rqServiceAdd(sv);                    // 7.2: for the Android app, and the Mac app later
  sv->start();                         // starts the services too""")

# the app on the line: no Apple services to look for
rep("""  if (ancsState == ANCS_WAIT && now - ancsLastTry > ANCS_SETTLE_MS) {""",
    """  if (ancsState == ANCS_WAIT && !btApp && now - ancsLastTry > ANCS_SETTLE_MS) {""")
rep("""  if (ctsState != CTS_DONE && ctsFails < CTS_GIVE_UP &&
      now - ctsAskedAt > CTS_RETRY_MS) {""", """  if (ctsState != CTS_DONE && ctsFails < CTS_GIVE_UP && !btApp &&
      now - ctsAskedAt > CTS_RETRY_MS) {""")
rep("""    btDropAt = millis();
    btClStale = true;                    // the next link gets a fresh client""",
    """    btDropAt = millis();
    btClStale = true;                    // the next link gets a fresh client
    btApp = false;                       // the next phone says for itself""")

# the inbox line, honest for an Android phone too
rep("""              : ancsState == ANCS_READY ? "from your phone\"""",
    """              : (ancsState == ANCS_READY || btApp) ? "from your phone\"""")

# loop
rep("""  btTick();                            // the phone's clock and notifications""",
    """  btTick();                            // the phone's clock and notifications
  appTick();                           // 7.2: what the Rafiq app sent""")

open(SRC, 'w').write(s)
print("7.2 ok")

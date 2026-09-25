/*
  ================================================================
   RAFIQ  -  ESP32-C3 desk companion
  ================================================================
   Knock on it to drive it. One rule holds everywhere:

     in a list      1 next item   2 open it    3 back out   4 reload
     in a reader    1 next page   2 back       3 carousel

   SCREENS  HOME  FOCUS  WEATHER  MESSAGES  PRAYER  FAITH
            SHORT READS  SETTINGS  SYSTEM

   FAITH carries its own text, so Zikr, the 99 Names, all 114
   chapters and the morning and evening adhkar work with no network
   at all.

   WIRING   everything on one I2C bus
     SDA GPIO8   SCL GPIO9
     OLED 0x3C   ADXL345 0x53   MPU6050 0x68

   LIBRARIES  FluxGarage RoboEyes, Adafruit GFX, Adafruit SSD1306,
              ArduinoJson
   Board      ESP32-C3.  Partition: Minimal SPIFFS (1.9MB APP)
  ================================================================
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <FluxGarage_RoboEyes.h>
#include "faith_data.h"

#define SDA_PIN 8
#define SCL_PIN 9
#define OLED_ADDR 0x3C
#define SCRW 128                 // RoboEyes owns W and H, so ours differ
#define SCRH 64

#define FW_VERSION "2.2.0"
#define OTA_REPO   "AhmadMahi/nexus-face"
#define OTA_ASSET  "nexus_face.bin"

#define DEF_WIFI_SSID "__WIFI_SSID__"
#define DEF_WIFI_PASS "__WIFI_PASS__"
#define DEF_TZ        "IST-5:30"
const char* RESCUE_SSID = "RAFIQ-SETUP";
const char* RESCUE_PASS = "password";

Adafruit_SSD1306 oled(SCRW, SCRH, &Wire, -1);
RoboEyes<Adafruit_SSD1306> eyes(oled);
WebServer   web(80);
Preferences prefs;

// ---------------- sensors ----------------
#define A_DEVID 0x00
#define A_THRESH_TAP 0x1D
#define A_THRESH_FF 0x28
#define A_TIME_FF 0x29
#define A_DUR 0x21
#define A_LATENT 0x22
#define A_WINDOW 0x23
#define A_TAP_AXES 0x2A
#define A_POWER_CTL 0x2D
#define A_INT_ENABLE 0x2E
#define A_INT_SOURCE 0x30
#define A_DATA_FORMAT 0x31
#define A_DATAX0 0x32
#define INT_TAP1 0x40
#define INT_FF 0x04
#define M_WHOAMI 0x75
#define M_PWR1 0x6B
#define M_GYRO_CFG 0x1B
#define M_ACC_CFG 0x1C
#define M_ACCEL_H 0x3B

uint8_t adxl = 0, mpu = 0;
float ax, ay, az, amag = 1, mx, my, mz, gxr, gyr, gzr, mtemp;

// ---------------- screens ----------------
enum { S_HOME = 0, S_FOCUS, S_WEATHER, S_MSG, S_PRAYER,
       S_FAITH, S_READS, S_GAMES, S_SETTINGS, S_SYSTEM, S_COUNT };
const char* S_NAME[S_COUNT] =
  { "HOME", "FOCUS", "WEATHER", "MESSAGES", "PRAYER",
    "FAITH", "SHORT READS", "GAMES", "SETTINGS", "SYSTEM" };

// depth 0 the carousel, 1 a list, 2 inside it, 3 one level deeper
int screen = S_HOME;
int depth  = 0;
int itemIdx = 0;                 // what is picked at depth 1
int subIdx  = 0;                 // what is picked at depth 2

// ---------------- faith ----------------
enum { F_ZIKR = 0, F_NAMES, F_QURAN, F_MORNING, F_EVENING, F_COUNT };
const char* F_NAME[F_COUNT] =
  { "Zikr", "99 Names", "Quran", "Morning adhkar", "Evening adhkar" };

int  zikrStep = 0;               // which of the four phrases
int  zikrDone = 0;               // how many of it are counted
int  zikrTotal = 0;              // out of a hundred
unsigned long zikrNext = 0;      // when the next count lands
#define ZIKR_PACE_MS 2600
#define ZIKR_LONG_MS 9000        // the last one is long, give it room

// ---------------- settings ----------------
enum { C_BRIGHT = 0, C_FACE, C_CONTROL, C_SLEEP, C_TURN, C_POPUP, C_EYES,
       C_PRAYER, C_HOTSPOT, C_ACCEL, C_PAIR, C_UPDATE, C_REBOOT, C_ABOUT, C_COUNT };
const char* C_NAME[C_COUNT] =
  { "Brightness", "Watch face", "Control", "Sleep after", "Page turn", "Popup time",
    "Eye style", "Prayer times", "Hotspot", "Accelerometer", "Pair a Mac",
    "Check update", "Reboot", "About" };

// Zero is the dimmest the panel goes, not off: the SSD1306 still shows
// faintly at contrast zero. After that, quarters.
const int BRIGHT_OPTS[] = { 0, 64, 128, 191, 255 };
const char* BRIGHT_NAME[] = { "dim", "25%", "50%", "75%", "100%" };
const int BRIGHT_N = sizeof(BRIGHT_OPTS) / sizeof(BRIGHT_OPTS[0]);

// ---------------- watch faces ----------------
//  Six laid out by hand, two that lean with the device, and two with
//  something that pours. Only the clock screen is affected.
enum { F_CLASSIC = 0, F_STACK, F_DATEUP, F_MINIMAL, F_SIDE, F_BANNER,
       F_DRIFT, F_PARALLAX, F_WATER, F_SAND, FACE_N };
const char* FACE_NAME[FACE_N] =
  { "classic", "stacked", "date up", "minimal", "side", "banner",
    "drift", "parallax", "water", "sand" };
int cfgFace = F_CLASSIC;

// The faces read the sensor for themselves, so they work whether or not
// leaning is switched on as a way of driving the thing. The reference
// follows slowly, which is what makes everything settle back to the
// middle a couple of seconds after it is set down.
float faceRef[3] = { 0, 0, 1 };
static void faceTilt(float& tx, float& ty);

// Which way round the thing is sitting. Shared by the games, by leaning
// as a way to drive it, and by the faces that react to being tilted, so
// it is only ever learned once.
int8_t mapAxX = -1, mapSgnX = 1, mapAxY = -1, mapSgnY = 1;   // -1 until taught
float  restV[3] = { 0, 0, 0 };
int    gravAx = 2;
float  calAcc[3] = { 0, 0, 0 };
int    calN = 0;
static bool tiltTaught() { return mapAxX >= 0 && mapAxY >= 0; }

const int SLEEP_OPTS[] = { 15, 30, 45, 60, 120, 180, 300, 600, 0 };   // 0 = never
const int SLEEP_N = sizeof(SLEEP_OPTS) / sizeof(SLEEP_OPTS[0]);
const int POPUP_OPTS[] = { 0, 5, 10, 20, 30, 60 };
const int POPUP_N = sizeof(POPUP_OPTS) / sizeof(POPUP_OPTS[0]);

struct EyeStyle { const char* name; byte w, h, r; int gap; bool cyc; byte mood; };
const EyeStyle STYLES[] = {
  { "round",   36, 36, 10, 12, false, DEFAULT },
  { "square",  38, 38,  2, 10, false, DEFAULT },
  { "wide",    48, 28, 12,  8, false, DEFAULT },
  { "sleepy",  36, 14,  6, 12, false, TIRED   },
  { "joy",     36, 36, 16, 12, false, HAPPY   },
  { "cyclops", 46, 46, 14,  0, true,  DEFAULT },
};
const int STYLE_N = sizeof(STYLES) / sizeof(STYLES[0]);

int cfgBright = 160, cfgSleepIdx = 1, cfgPopupIdx = 2, cfgEyes = 0;
bool cfgAutoTurn = false;                  // pages turn themselves
// Taps always work. Tilt is something you turn on as well, never instead,
// so a misread lean can never leave you with no way back.
bool cfgTilt = false;

// Leaning it about: down for the next thing, up for the one before, left
// to go in, right to come out, and up held for three seconds for home.
enum { NC_OFF = 0, NC_HOLD, NC_UP, NC_DOWN, NC_LEFT, NC_RIGHT, NC_INFO };
int  navCal = NC_OFF;
bool navCalTeach = false;
unsigned long navCalStamp = 0;
bool navLatch = false;
unsigned long upSince = 0;
bool upConsumed = false;
uint32_t nTiltNext = 0, nTiltPrev = 0, nTiltIn = 0, nTiltOut = 0, nTiltHome = 0;
#define NAV_TILT_ON  0.30f
#define NAV_TILT_OFF 0.14f
#define NAV_HOME_MS  3000UL
String cfgSsid, cfgPass, cfgTz, cfgKey;
int sleepSecs() { return SLEEP_OPTS[cfgSleepIdx]; }
int popupSecs() { return POPUP_OPTS[cfgPopupIdx]; }
#define AUTO_TURN_MS 9000

// ---------------- content ----------------
String message = "";
unsigned long popupUntil = 0;

float wTemp = NAN, wHum = NAN, wWind = NAN;
int   wCode = -1;
String wCity = "";
bool  wxOk = false;
unsigned long nextWx = 0;
float locLat = NAN, locLon = NAN;

const char* PRAYERS[5] = { "Fajr", "Dhuhr", "Asr", "Maghrib", "Isha" };
int  prayerMin[5] = { -1, -1, -1, -1, -1 };
// Calculated times and the local masjid rarely agree. Each prayer carries
// its own correction in minutes, set on the page and kept in flash.
int  prayerAdj[5] = { 0, 0, 0, 0, 0 };
static int prayerAt(int i) {
  if (prayerMin[i] < 0) return -1;
  return (prayerMin[i] + prayerAdj[i] + 1440) % 1440;
}

// The call comes in three steps: a word ten minutes out, a reminder at
// five, and the minute itself.
enum { AL_NONE = 0, AL_TEN, AL_FIVE, AL_NOW };
int  alertPhase = AL_NONE, alertWhich = -1, alertDay = -1;
unsigned long alertUntil = 0;
uint8_t alertDone[5] = { 0, 0, 0, 0, 0 };     // bit 1 ten, 2 five, 4 now
#define ALERT_TEN_MS  16000UL
#define ALERT_FIVE_MS 12000UL
#define ALERT_NOW_MS  60000UL
bool prayerOk = false;
int  prayerDay = -1;
unsigned long nextPrayerTry = 0;
// Prayer times barely move week to week, and refetching them daily meant
// losing them whenever the network was down. They are kept in flash and
// only refreshed on request, or once every fiftieth boot.
uint32_t prayerBoot = 0;
bool     prayerWanted = false;
#define PRAYER_REFRESH_BOOTS 50

// ---------------- the reader ----------------
// One buffer serves every paged thing on the device: a short read, a
// chapter note, a dhikr. Only whatever is open is wrapped, so a shelf
// of fifteen stories costs nothing until one is picked up.
#define RD_LINES 340
#define RD_PER_PAGE 4
#define RD_COLS 21
#define RD_TITLE_MAX 15        // what fits beside the page counter
String rdLine[RD_LINES];
int    rdLines = 0, rdPage = 0;
String rdTitle = "";
unsigned long rdTurn = 0;              // when an auto page turn is due

static int rdPages() { return rdLines ? (rdLines + RD_PER_PAGE - 1) / RD_PER_PAGE : 0; }

// ---------------- the shelf of short reads ----------------
#define READS_MAX 15
#define STORY_MAX_CHARS 6000
String readTitle[READS_MAX];
int    readCount = 0;
int    readOpen  = -1;                 // which one is in the reader
bool   storyBusy = false;
String storyState = "Shelf is empty";
int    refillWant = 0;                 // how many more to write, in the background
unsigned long nextRefill = 0;
unsigned long nextStory = 0;
#define READING_SLEEP_SEC 180          // a long fuse while you are reading

// ---------------- work session ----------------
#define TASK_MAX 8
struct Task { String name; int mins; };
Task tasks[TASK_MAX];
int  taskCount = 0;
int  taskIdx   = -1;
unsigned long taskEnd = 0;
unsigned long flashUntil = 0;
const char* flashWord = "";
int  workedMin = 0;
bool breakDue = false;
#define BREAK_AFTER_MIN 30

static bool isBreak(const String& n) {
  String l = n; l.toLowerCase();
  return l.indexOf("break") >= 0 || l.indexOf("rest") >= 0 || l.indexOf("walk") >= 0;
}
static bool sessionRunning() { return taskIdx >= 0 && taskIdx < taskCount; }

// ================================================================
//  THE MAC
// ================================================================
//  Rafiq on the Mac speaks the same API the page does. The device knows
//  whether it is being spoken to and says so, but nothing here depends
//  on it: close the laptop and the clock, the prayer times, the reads
//  and every knock carry on exactly as they did before.

String   cfgTok;                       // what a paired Mac has to present
bool     cfgLock = false;              // whether anything is checked at all
int      pairCode = -1;                // six digits, on the panel, for three minutes
unsigned long pairUntil = 0;
#define PAIR_WINDOW_MS 180000UL

unsigned long macSeen = 0;             // the last call that carried the token
bool     macLinked = false;
unsigned long linkCardUntil = 0;
bool     linkCardJoin = false;
#define MAC_GONE_MS 25000UL            // two missed heartbeats, then it is gone

// ---------------- focus ----------------
//  A countdown held on an OLED for half an hour is how a panel gets
//  burned into, so focus spends most of its time dark and surfaces
//  every so often to show where it has got to.
enum { FZ_SHOW = 0, FZ_DARK, FZ_QUOTE };
int  fzPhase = FZ_SHOW;
int  fzCycle = 0;
unsigned long fzNext = 0;
const char* fzLine = "";
#define FZ_SHOW_MS  12000UL
#define FZ_DARK_MS  10000UL
#define FZ_QUOTE_MS  5000UL
const char* FZ_LINES[] = {
  "Good work", "Keep going", "Still here", "Nearly there",
  "One thing at a time", "Steady", "This is the hard part", "Stay with it" };
const int FZ_N = sizeof(FZ_LINES) / sizeof(FZ_LINES[0]);

// ---------------- relax ----------------
//  Nothing to read and nothing to do: something slow to rest your eyes
//  on. Knock once to leave.
bool relaxOn = false;
int  relaxKind = 0;
unsigned long relaxNext = 0;

// ---------------- the pointer ----------------
//  Sent over UDP rather than HTTP. Ten a second through a fresh
//  handshake each time would drown it, and a lost one costs nothing
//  because another is a tenth of a second behind.
WiFiUDP cursorUdp;
#define CURSOR_PORT 4210
float curX = 0, curY = 0;              // -1 to 1, where the pointer sits
unsigned long curUntil = 0;            // tracking lapses if the Mac goes quiet
bool cfgFollow = false;
#define CURSOR_HOLD_MS 2500UL

// ---------------- what the Mac wants shown for a moment ----------------
//  A copy, a paste, a nudge to stand up. None of it disturbs the stored
//  message, which is yours and stays where it is.
String toastText = "", toastKind = "";
unsigned long toastUntil = 0;

// ---------------- a page of pixels ----------------
//  One screenful straight from the Mac. Whatever the Mac can draw, the
//  robot can show, with no new firmware for it.
uint8_t* canvasBuf = nullptr;
unsigned long canvasUntil = 0;

// The page is the way back in when anything else fails, so it is never
// gone for good: switched off from the Mac, it returns by itself after
// a quarter of an hour with nobody home.
bool webUiOn = true;
unsigned long webOffAt = 0;
#define WEBUI_RETURN_MS 900000UL

// ---------------- runtime ----------------
bool asleep = false, screenOn = true, timeOk = false, rescueAP = false, fsOk = false;
unsigned long lastActive = 0, lastDraw = 0, lastPoll = 0, reactUntil = 0, lastShake = 0;
unsigned long nextTimeTry = 0, swStart = 0, inputMuteUntil = 0;
unsigned long lastLowG = 0, lastFallAt = 0;
float lastRawX = 0, lastRawY = 0, lastRawZ = 0;
float refAx = 0, refAy = 0, refAz = 1;
unsigned long steadySince = 0;
uint32_t cTap = 0, cDouble = 0, cTriple = 0, cQuad = 0, cFall = 0, cShake = 0, cBoot = 0;
uint8_t  burst = 0;
unsigned long burstStart = 0;
String   otaStatus = "", otaStatus2 = "";

// Updating is a little conversation now rather than one button: which
// release, then yes or no, and only then does anything get written.
enum { U_OFF = 0, U_MENU, U_ASK, U_LIST, U_NONE, U_FAIL };
int    upState = U_OFF;
int    upPick = 0;                  // 0 the latest one, 1 the older ones
bool   upYes = true;
String upTag = "", upUrl = "", upMsg = "";
#define UP_MAX 8
String relTag[UP_MAX], relUrl[UP_MAX];
int    relCount = 0, relSel = 0;
String   clockSrc = "not set";
String   wokeBy = "boot";
float    lastDirD = 0;
uint32_t nSlept = 0;
int      otaPct = -1;

static bool online() { return WiFi.status() == WL_CONNECTED; }

#define TAP_WINDOW_MS 450          // room to land four knocks, without dawdling
#define TILT 0.35f
#define SHAKE_G 0.60f

// ================================================================
//  I2C
// ================================================================
static inline void wReg(uint8_t a, uint8_t r, uint8_t v) {
  Wire.beginTransmission(a); Wire.write(r); Wire.write(v); Wire.endTransmission();
}
static uint8_t rReg(uint8_t a, uint8_t r) {
  Wire.beginTransmission(a); Wire.write(r);
  if (Wire.endTransmission(false)) return 0;
  if (Wire.requestFrom((int)a, 1) != 1) return 0;
  return Wire.read();
}
static bool rBlk(uint8_t a, uint8_t r, uint8_t* b, uint8_t n) {
  Wire.beginTransmission(a); Wire.write(r);
  if (Wire.endTransmission(false)) return false;
  if (Wire.requestFrom((int)a, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) b[i] = Wire.read();
  return true;
}
static void startSensors() {
  uint8_t t[2] = { 0x53, 0x1D };
  for (int i = 0; i < 2 && !adxl; i++) {
    if (rReg(t[i], A_DEVID) != 0xE5) continue;
    adxl = t[i];
    wReg(adxl, A_DATA_FORMAT, 0x0B);
    wReg(adxl, A_THRESH_TAP, 0x28); wReg(adxl, A_DUR, 0x10);
    wReg(adxl, A_LATENT, 0x30);     wReg(adxl, A_WINDOW, 0xC0);
    wReg(adxl, A_TAP_AXES, 0x07);
    // 375mg for 200ms. A drop from desk height is 250ms or more of real
    // free fall, so this still catches one; the old 438mg for 100ms was
    // at the sensitive end of the datasheet range and a hand turning the
    // thing over tripped it constantly.
    wReg(adxl, A_THRESH_FF, 0x06);  wReg(adxl, A_TIME_FF, 0x28);
    wReg(adxl, A_INT_ENABLE, INT_TAP1 | INT_FF);   // reset later by applyFallInt()
    wReg(adxl, A_POWER_CTL, 0x08);
    delay(20); rReg(adxl, A_INT_SOURCE);
  }
  uint8_t m[2] = { 0x68, 0x69 };
  for (int i = 0; i < 2 && !mpu; i++) {
    uint8_t who = rReg(m[i], M_WHOAMI);
    if (who != 0x68 && who != 0x69 && who != 0x70 && who != 0x71 && who != 0x98) continue;
    mpu = m[i];
    wReg(mpu, M_PWR1, 0x00); delay(10);
    wReg(mpu, M_GYRO_CFG, 0x00); wReg(mpu, M_ACC_CFG, 0x00);
  }
}
// With leaning switched on the device lives in a hand, and a hand unloads
// it constantly while turning it over. The fall animation is not worth
// the false alarms there, so it is simply not armed in that mode.
static void applyFallInt() {
  if (!adxl) return;
  wReg(adxl, A_INT_ENABLE, cfgTilt ? (uint8_t)INT_TAP1 : (uint8_t)(INT_TAP1 | INT_FF));
  rReg(adxl, A_INT_SOURCE);                  // drop anything already pending
}

static void readSensors() {
  uint8_t b[14];
  if (adxl && rBlk(adxl, A_DATAX0, b, 6)) {
    ax = (int16_t)((b[1] << 8) | b[0]) / 256.0f;
    ay = (int16_t)((b[3] << 8) | b[2]) / 256.0f;
    az = (int16_t)((b[5] << 8) | b[4]) / 256.0f;
    amag = sqrtf(ax * ax + ay * ay + az * az);
  }
  if (mpu && rBlk(mpu, M_ACCEL_H, b, 14)) {
    mx = (int16_t)((b[0] << 8) | b[1]) / 16384.0f;
    my = (int16_t)((b[2] << 8) | b[3]) / 16384.0f;
    mz = (int16_t)((b[4] << 8) | b[5]) / 16384.0f;
    mtemp = (int16_t)((b[6] << 8) | b[7]) / 340.0f + 36.53f;
    gxr = (int16_t)((b[8] << 8) | b[9]) / 131.0f;
    gyr = (int16_t)((b[10] << 8) | b[11]) / 131.0f;
    gzr = (int16_t)((b[12] << 8) | b[13]) / 131.0f;
    if (!adxl) { ax = mx; ay = my; az = mz; amag = sqrtf(mx*mx + my*my + mz*mz); }
  }
}
// ================================================================
//  DRAWING
//    y 0..10   title bar, black text knocked out of white
//    y 12..63  content
// ================================================================
static void ctr(const char* s, int y, int size) {
  int w = (int)strlen(s) * 6 * size;
  oled.setTextSize(size);
  oled.setCursor(w < SCRW ? (SCRW - w) / 2 : 0, y);
  oled.print(s);
}
static void at(int x, int y, const char* s, int size = 1) {
  oled.setTextSize(size); oled.setCursor(x, y); oled.print(s);
}
static void clockStr(char* o, size_t n, bool sec) {
  struct tm t;
  if (!timeOk || !getLocalTime(&t, 0)) { snprintf(o, n, sec ? "--:--:--" : "--:--"); return; }
  if (sec) snprintf(o, n, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  else     snprintf(o, n, "%02d:%02d", t.tm_hour, t.tm_min);
}

// The eyes are drawn as plain filled shapes by the library. A pupil and
// a glint on top is what makes them read as eyes rather than as blocks.
static void drawPupils() {
  int wl = eyes.eyeLwidthCurrent, hl = eyes.eyeLheightCurrent;
  if (hl > 12 && wl > 12) {
    int r  = min(wl, hl) / 4;
    int cx = eyes.eyeLx + wl / 2, cy = eyes.eyeLy + hl / 2;
    oled.fillCircle(cx, cy, r, SSD1306_BLACK);
    oled.fillCircle(cx + r / 2, cy - r / 2, max(1, r / 4), SSD1306_WHITE);
  }
  if (eyes.cyclops) return;
  int wr = eyes.eyeRwidthCurrent, hr = eyes.eyeRheightCurrent;
  if (hr > 12 && wr > 12) {
    int r  = min(wr, hr) / 4;
    int cx = eyes.eyeRx + wr / 2, cy = eyes.eyeRy + hr / 2;
    oled.fillCircle(cx, cy, r, SSD1306_BLACK);
    oled.fillCircle(cx + r / 2, cy - r / 2, max(1, r / 4), SSD1306_WHITE);
  }
}
// one animation frame of the face
static void eyesFrame() {
  eyes.update();                       // the library clears and draws
  drawPupils();
  oled.display();
}

// a knock, drawn as it sounds: a tap and the rings going out from it
static void knockIcon(int cx, int cy) {
  oled.fillCircle(cx, cy, 2, SSD1306_WHITE);
  oled.drawCircle(cx, cy, 5, SSD1306_WHITE);
  oled.drawCircle(cx, cy, 8, SSD1306_WHITE);
}

// ---------------------------------------------------------------
//  Boot animations, drawn by hand.
//
//  The library clears the panel and pushes it itself, so anything
//  added afterwards needed a second push and the panel showed both
//  frames. That double push is what read as a nervous flicker. These
//  build one buffer and push it once, so they sit perfectly still.
// ---------------------------------------------------------------
static void calmEyes(int openPct, int lookX, int cy) {
  const int w = 34;
  int h = 4 + (30 * constrain(openPct, 0, 100)) / 100;
  int y = cy - h / 2;
  int r = min(9, h / 2);
  int lx = 26 + lookX, rx = 68 + lookX;
  oled.fillRoundRect(lx, y, w, h, r, SSD1306_WHITE);
  oled.fillRoundRect(rx, y, w, h, r, SSD1306_WHITE);
  if (h >= 18) {
    int pr = h / 4;
    int px[2] = { lx + w / 2, rx + w / 2 };
    for (int e = 0; e < 2; e++) {
      oled.fillCircle(px[e], cy, pr, SSD1306_BLACK);
      oled.fillCircle(px[e] + pr / 2, cy - pr / 2, max(1, pr / 4), SSD1306_WHITE);
    }
  }
}

static void mosqueIcon(int cx, int cy, uint16_t c) {
  oled.drawCircleHelper(cx, cy, 7, 1 | 2, c);
  oled.drawFastHLine(cx - 7, cy, 15, c);
  oled.drawFastVLine(cx - 7, cy, 9, c);
  oled.drawFastVLine(cx + 7, cy, 9, c);
  oled.drawFastHLine(cx - 7, cy + 9, 15, c);
  oled.drawFastVLine(cx - 11, cy - 3, 12, c);
  oled.drawFastVLine(cx + 11, cy - 3, 12, c);
  oled.drawFastVLine(cx, cy - 11, 4, c);
}

static void titleBar(const char* title, const char* right) {
  oled.fillRect(0, 0, SCRW, 11, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  oled.setTextSize(1);
  oled.setCursor(3, 2);
  oled.print(title);
  if (right && *right) {
    oled.setCursor(SCRW - 3 - (int)strlen(right) * 6, 2);
    oled.print(right);
  }
  oled.setTextColor(SSD1306_WHITE);
}
// The clock is only worth showing in the bar once it is real.
static void titleBarC(const char* title) {
  oled.fillRect(0, 0, SCRW, 11, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  oled.setTextSize(1);
  oled.setCursor((SCRW - (int)strlen(title) * 6) / 2, 2);
  oled.print(title);
  oled.setTextColor(SSD1306_WHITE);
}
static void bar(const char* title) {
  if (!timeOk) { titleBar(title, ""); return; }
  char t[8];
  clockStr(t, sizeof(t), false);
  titleBar(title, t);
}

// ---- weather glyphs ----
static void wxSun(int x, int y) {
  oled.fillCircle(x + 10, y + 10, 6, SSD1306_WHITE);
  for (int i = 0; i < 8; i++) {
    float a = i * 0.7854f;
    oled.drawLine(x + 10 + cosf(a) * 8, y + 10 + sinf(a) * 8,
                  x + 10 + cosf(a) * 11, y + 10 + sinf(a) * 11, SSD1306_WHITE);
  }
}
static void wxCloud(int x, int y) {
  oled.fillCircle(x + 6, y + 13, 5, SSD1306_WHITE);
  oled.fillCircle(x + 14, y + 11, 6, SSD1306_WHITE);
  oled.fillRect(x + 6, y + 12, 9, 6, SSD1306_WHITE);
}
static void wxRain(int x, int y) {
  wxCloud(x, y - 4);
  for (int i = 0; i < 3; i++) oled.drawLine(x + 5 + i * 5, y + 14, x + 3 + i * 5, y + 19, SSD1306_WHITE);
}
static void wxSnow(int x, int y) {
  wxCloud(x, y - 4);
  for (int i = 0; i < 3; i++) oled.drawCircle(x + 5 + i * 5, y + 17, 1, SSD1306_WHITE);
}
static void wxStorm(int x, int y) {
  wxCloud(x, y - 4);
  oled.drawLine(x + 12, y + 13, x + 8, y + 19, SSD1306_WHITE);
  oled.drawLine(x + 8, y + 19, x + 13, y + 17, SSD1306_WHITE);
}
static const char* wxWord(int c) {
  if (c < 0)   return "No data";
  if (c == 0)  return "Clear";
  if (c <= 2)  return "Partly sunny";
  if (c == 3)  return "Overcast";
  if (c <= 48) return "Foggy";
  if (c <= 57) return "Drizzle";
  if (c <= 67) return "Rain";
  if (c <= 77) return "Snow";
  if (c <= 82) return "Showers";
  if (c <= 86) return "Snow showers";
  return "Thunderstorm";
}
static void wxIcon(int c, int x, int y) {
  if (c <= 2)       wxSun(x, y);
  else if (c <= 48) wxCloud(x, y);
  else if (c <= 67) wxRain(x, y);
  else if (c <= 86) wxSnow(x, y);
  else              wxStorm(x, y);
}
static void gearIcon(int cx, int cy, int r) {
  oled.drawCircle(cx, cy, r, SSD1306_WHITE);
  oled.drawCircle(cx, cy, r / 2, SSD1306_WHITE);
  for (int i = 0; i < 8; i++) {
    float a = i * 0.7854f;
    oled.drawLine(cx + cosf(a) * r, cy + sinf(a) * r,
                  cx + cosf(a) * (r + 3), cy + sinf(a) * (r + 3), SSD1306_WHITE);
  }
}
// a closed book seen head on, with a spine and a few leaves
static void bookIcon(int cx, int cy) {
  oled.drawRect(cx - 13, cy - 9, 26, 18, SSD1306_WHITE);
  oled.drawFastVLine(cx, cy - 9, 18, SSD1306_WHITE);
  for (int i = 0; i < 3; i++) {
    oled.drawFastHLine(cx - 10, cy - 4 + i * 4, 7, SSD1306_WHITE);
    oled.drawFastHLine(cx + 4,  cy - 4 + i * 4, 7, SSD1306_WHITE);
  }
}
// a string of beads for the counter
static void beadIcon(int cx, int cy) {
  for (int i = 0; i < 9; i++) {
    float a = i * 0.6981f - 1.2f;
    oled.fillCircle(cx + cosf(a) * 12, cy + sinf(a) * 12, 2, SSD1306_WHITE);
  }
  oled.drawCircle(cx, cy, 12, SSD1306_WHITE);
}

// ================================================================
//  GENERIC LIST AND READER
//    Every menu on the device is drawn by these two, so they all
//    behave the same and all fit the same way.
// ================================================================
static void drawList(const char* title, const char* right, int count, int sel,
                     const char* (*label)(int, char*, size_t)) {
  oled.clearDisplay();
  titleBar(title, right);
  if (count <= 0) { ctr("Nothing here", 30, 1); oled.display(); return; }

  int first = sel > 3 ? sel - 3 : 0;                 // keep the pick in view
  if (first > count - 4) first = count - 4;
  if (first < 0) first = 0;

  char buf[26];
  for (int r = 0; r < 4 && first + r < count; r++) {
    int i = first + r, y = 14 + r * 12;
    bool on = (i == sel);
    if (on) { oled.fillRect(0, y - 2, SCRW, 12, SSD1306_WHITE); oled.setTextColor(SSD1306_BLACK); }
    else      oled.setTextColor(SSD1306_WHITE);
    const char* s = label(i, buf, sizeof(buf));
    oled.setTextSize(1);
    oled.setCursor(3, y);
    for (int k = 0; k < 20 && s[k]; k++) oled.write(s[k]);
    oled.setTextColor(SSD1306_WHITE);
  }
  // a slim rail on the right showing where you are in a long list
  if (count > 4) {
    int h = max(4, 48 * 4 / count);
    int y = 14 + (48 - h) * sel / max(1, count - 1);
    oled.drawFastVLine(126, 14, 48, SSD1306_WHITE);
    oled.fillRect(125, y, 3, h, SSD1306_WHITE);
  }
  oled.display();
}

static void drawReader(const char* title) {
  oled.clearDisplay();
  int pages = rdPages();
  char r[12];
  snprintf(r, sizeof(r), "%d/%d", rdPage + 1, pages ? pages : 1);
  titleBar(title, r);
  int start = rdPage * RD_PER_PAGE;
  for (int i = 0; i < RD_PER_PAGE && start + i < rdLines; i++)
    at(2, 15 + i * 12, rdLine[start + i].c_str());
  int w = pages > 1 ? (SCRW - 4) * (rdPage + 1) / pages : SCRW - 4;
  oled.drawFastHLine(2, 63, w, SSD1306_WHITE);
  oled.display();
}

// ================================================================
//  SCREENS
// ================================================================
static void swStr(char* o, size_t n) {
  unsigned long s = (millis() - swStart) / 1000UL;
  if (s >= 3600UL) snprintf(o, n, "%lu:%02lu:%02lu", s / 3600UL, (s / 60UL) % 60UL, s % 60UL);
  else             snprintf(o, n, "%lu:%02lu", s / 60UL, s % 60UL);
}

// ================================================================
//  WATCH FACES
//    Ten ways to show the same few things. No frames and no boxes:
//    the panel is small enough that a border is only lost pixels.
// ================================================================
struct Bits { char hm[8], hh[4], mm[4], ss[4], day[12], dlong[20], dshort[14],
                   dmon[10], dyear[6]; bool ok; };

static Bits fb;                         // what the faces draw from
static void loadBits() {
  Bits& b = fb;
  struct tm t;
  fb.ok = timeOk && getLocalTime(&t, 0);
  if (!fb.ok) {
    strcpy(fb.hm, "--:--"); strcpy(fb.hh, "--"); strcpy(fb.mm, "--"); strcpy(fb.ss, "--");
    strcpy(fb.day, "waiting"); strcpy(fb.dlong, "for the clock"); strcpy(fb.dshort, "--");
    return;
  }
  snprintf(fb.hm, sizeof(fb.hm), "%02d:%02d", t.tm_hour, t.tm_min);
  snprintf(fb.hh, sizeof(fb.hh), "%02d", t.tm_hour);
  snprintf(fb.mm, sizeof(fb.mm), "%02d", t.tm_min);
  snprintf(fb.ss, sizeof(fb.ss), "%02d", t.tm_sec);
  strftime(fb.day,    sizeof(fb.day),    "%A", &t);
  strftime(fb.dlong,  sizeof(fb.dlong),  "%d %B %Y", &t);
  strftime(fb.dshort, sizeof(fb.dshort), "%d %b %Y", &t);
}

// A level that follows slowly, so however the thing happens to be
// sitting counts as flat, and everything drifts back to the middle a
// couple of seconds after it is set down. A face needs no calibration
// of its own, and works whether or not leaning drives the device.
static void faceTilt(float& tx, float& ty) {
  float v[3] = { ax, ay, az };
  for (int i = 0; i < 3; i++) faceRef[i] += (v[i] - faceRef[i]) * 0.05f;
  if (tiltTaught()) {
    tx = mapSgnX * (v[mapAxX] - faceRef[mapAxX]);
    ty = mapSgnY * (v[mapAxY] - faceRef[mapAxY]);
  } else {
    tx = v[0] - faceRef[0];
    ty = v[1] - faceRef[1];
  }
  tx = constrain(tx, -0.6f, 0.6f);
  ty = constrain(ty, -0.6f, 0.6f);
}

// Flip every pixel under a line that moves per column, so whatever the
// fill covers stays readable instead of disappearing into it.
static void invertUnder(const int* topAt) {
  uint8_t* buf = oled.getBuffer();
  for (int x = 0; x < SCRW; x++) {
    int top = constrain(topAt[x], 0, SCRH);
    for (int y = top; y < SCRH; y++) buf[x + (y >> 3) * SCRW] ^= (1 << (y & 7));
  }
}

static void faceClassic() {
  int bw = 5 * 18;
  int x0 = (SCRW - bw - 14) / 2;
  oled.setTextSize(3); oled.setCursor(x0, 8); oled.print(fb.hm);
  at(x0 + bw + 5, 18, fb.ss);
  oled.drawFastHLine(22, 36, SCRW - 44, SSD1306_WHITE);
  ctr(fb.day, 41, 1);
  ctr(fb.dlong, 53, 1);
}
static void faceStack() {
  ctr(fb.hh, 3, 3);
  ctr(fb.mm, 28, 3);
  ctr(fb.dshort, 55, 1);
}
static void faceDateUp() {
  ctr(fb.day, 3, 1);
  ctr(fb.dshort, 14, 1);
  ctr(fb.hm, 27, 3);
  ctr(fb.ss, 54, 1);
}
static void faceMinimal() {
  ctr(fb.hm, 16, 4);
}
static void faceSide() {
  // the date goes on two short lines, because "30 September 2026" beside
  // a size two clock does not fit and never will
  at(4, 20, fb.hm, 2);
  at(4, 40, fb.ss, 1);
  oled.drawFastVLine(64, 14, 38, SSD1306_WHITE);
  at(68, 16, fb.day);
  at(68, 30, fb.dmon);
  at(68, 42, fb.dyear);
}
static void faceBanner() {
  ctr(fb.day, 4, 1);
  oled.fillRect(0, 16, SCRW, 28, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  ctr(fb.hm, 20, 3);
  oled.setTextColor(SSD1306_WHITE);
  ctr(fb.dshort, 50, 1);
}
// everything leans the way you do, and rights itself when you stop
static void faceDrift() {
  float tx, ty;
  faceTilt(tx, ty);
  int dx = constrain((int)(tx * 40.0f), -22, 22);
  int dy = constrain((int)(-ty * 18.0f), -10, 10);
  int w = 5 * 18;
  oled.setTextSize(3);
  oled.setCursor((SCRW - w) / 2 + dx, 14 + dy);
  oled.print(fb.hm);
  oled.setTextSize(1);
  int dw = (int)strlen(fb.dshort) * 6;
  oled.setCursor((SCRW - dw) / 2 + dx, 46 + dy);
  oled.print(fb.dshort);
}
// the same idea, except the lines lean by different amounts
static void faceParallax() {
  float tx, ty;
  faceTilt(tx, ty);
  int d1 = constrain((int)(tx * 46.0f), -24, 24);
  int d2 = constrain((int)(tx * -20.0f), -12, 12);
  int w = 5 * 18;
  oled.setTextSize(3);
  oled.setCursor((SCRW - w) / 2 + d1, 10);
  oled.print(fb.hm);
  oled.setTextSize(1);
  int dw = (int)strlen(fb.day) * 6;
  oled.setCursor((SCRW - dw) / 2 + d2, 40);
  oled.print(fb.day);
  dw = (int)strlen(fb.dshort) * 6;
  oled.setCursor((SCRW - dw) / 2 + d1 / 2, 52);
  oled.print(fb.dshort);
}
// one that sloshes, and one that simply tips
static void faceFill(bool wavy) {
  ctr(fb.day, 2, 1);
  ctr(fb.dshort, 13, 1);
  ctr(fb.hm, 26, 2);
  float tx, ty;
  faceTilt(tx, ty);
  static int topAt[SCRW];
  float slope = tx * 30.0f;
  float ph = millis() * 0.004f;
  for (int x = 0; x < SCRW; x++) {
    float w = wavy ? sinf(x * 0.13f + ph) * 2.4f + sinf(x * 0.05f - ph * 0.7f) * 1.6f : 0.0f;
    // lean right and it should pool on the right, so the surface sits
    // higher up the screen on that side
    topAt[x] = 46 - (int)(slope * (x - SCRW / 2) / (SCRW / 2)) + (int)w;
  }
  invertUnder(topAt);
}

static void drawHome() {
  oled.clearDisplay();
  loadBits();

  if (!fb.ok) {                          // no clock yet, so it counts instead
    char e[14];
    swStr(e, sizeof(e));
    ctr("STOPWATCH", 6, 1);
    int sz = strlen(e) > 5 ? 2 : 3;
    oled.setTextSize(sz);
    oled.setCursor((SCRW - (int)strlen(e) * 6 * sz) / 2, sz == 3 ? 20 : 24);
    oled.print(e);
    oled.drawFastHLine(22, 46, SCRW - 44, SSD1306_WHITE);
    ctr("Two knocks to reset", 52, 1);
    oled.display();
    return;
  }

  switch (cfgFace) {
    case F_STACK:    faceStack();       break;
    case F_DATEUP:    faceDateUp();      break;
    case F_MINIMAL:    faceMinimal();     break;
    case F_SIDE:    faceSide();        break;
    case F_BANNER:    faceBanner();      break;
    case F_DRIFT:    faceDrift();       break;
    case F_PARALLAX:    faceParallax();    break;
    case F_WATER:    faceFill(true);  break;
    case F_SAND:    faceFill(false); break;
    default:         faceClassic();     break;
  }
  oled.display();
}


static void drawWeather() {
  oled.clearDisplay();
  bar("WEATHER");
  if (!wxOk) {
    ctr(online() ? "Fetching" : "No network", 28, 1);
    ctr(wCity.length() ? wCity.c_str() : "Offline for now", 44, 1);
    oled.display();
    return;
  }
  wxIcon(wCode, 2, 16);

  char l[22];
  snprintf(l, sizeof(l), "%d", (int)roundf(wTemp));
  int tw = strlen(l) * 18;
  oled.setTextSize(3);
  oled.setCursor(30, 16);
  oled.print(l);
  oled.drawCircle(30 + tw + 5, 19, 2, SSD1306_WHITE);
  at(30 + tw + 10, 16, "C");
  snprintf(l, sizeof(l), "%d%%", (int)roundf(wHum));
  at(30 + tw + 10, 30, l);

  ctr(wxWord(wCode), 44, 1);
  snprintf(l, sizeof(l), "%s  %.0f km/h", wCity.c_str(), wWind);
  ctr(l, 54, 1);
  oled.display();
}

static void fmt12(char* o, size_t n, int mins) {
  int h = mins / 60, m = mins % 60;
  int d = h % 12; if (!d) d = 12;
  snprintf(o, n, "%2d:%02d%s", d, m, h >= 12 ? "pm" : "am");
}
static bool isFriday() {
  struct tm t;
  return timeOk && getLocalTime(&t, 0) && t.tm_wday == 5;
}
static int nextPrayer(int nowMin) {
  for (int i = 0; i < 5; i++) if (prayerAt(i) > nowMin) return i;
  return 0;
}
static void drawPrayer() {
  oled.clearDisplay();
  bar("PRAYER");
  if (!prayerOk) {
    ctr(online() ? "Fetching times" : "No network", 26, 1);
    if (!online()) ctr("Saved times will show", 42, 1);
    oled.display();
    return;
  }
  struct tm t;
  int nowMin = (timeOk && getLocalTime(&t, 0)) ? t.tm_hour * 60 + t.tm_min : -1;
  int nx = nowMin >= 0 ? nextPrayer(nowMin) : -1;

  char v[12];
  for (int i = 0; i < 5; i++) {
    int y = 14 + i * 10;
    if (i == nx) {
      oled.fillRect(0, y - 1, SCRW, 10, SSD1306_WHITE);
      oled.setTextColor(SSD1306_BLACK);
    } else oled.setTextColor(SSD1306_WHITE);
    // on a Friday the midday prayer goes by its own name
    at(4, y, (i == 1 && isFriday()) ? "Jumuah" : PRAYERS[i]);
    fmt12(v, sizeof(v), prayerAt(i));
    oled.setCursor(SCRW - 3 - (int)strlen(v) * 6, y);
    oled.print(v);
  }
  oled.setTextColor(SSD1306_WHITE);
  oled.display();
}

static void drawPrayerAlert() {
  bool flash = false;
  if (alertPhase == AL_FIVE) flash = ((millis() / 450) % 2) == 0;
  if (alertPhase == AL_NOW)  flash = ((millis() / 300) % 2) == 0;

  oled.clearDisplay();
  if (flash) {
    oled.fillRect(0, 0, SCRW, SCRH, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
  } else oled.setTextColor(SSD1306_WHITE);
  uint16_t ink = flash ? SSD1306_BLACK : SSD1306_WHITE;

  const char* nm = (alertWhich == 1 && isFriday()) ? "Jumuah"
                 : (alertWhich >= 0 && alertWhich < 5) ? PRAYERS[alertWhich] : "Prayer";
  if (alertPhase == AL_NOW) {
    ctr(nm, 12, 2);
    oled.drawFastHLine(24, 34, SCRW - 48, ink);
    ctr("It is time", 40, 1);
    ctr("for prayer", 52, 1);
  } else {
    char l[26];
    mosqueIcon(SCRW / 2, 22, ink);
    snprintf(l, sizeof(l), "%s in %d min", nm, alertPhase == AL_TEN ? 10 : 5);
    ctr(l, 38, 1);
    ctr("Time to get ready", 50, 1);
  }
  oled.setTextColor(SSD1306_WHITE);
  oled.display();
}

static void drawMessage() {
  oled.clearDisplay();
  bar("MESSAGE");
  if (!message.length()) {
    ctr("Nothing yet", 28, 1);
    ctr("Send one on the page", 44, 1);
    oled.display();
    return;
  }
  const int PER = 21, MAXL = 4;
  String line[MAXL];
  int n = 0;
  for (int i = 0; n < MAXL && i < (int)message.length(); ) {
    int take = min(PER, (int)message.length() - i);
    if (take == PER) { int sp = message.lastIndexOf(' ', i + take); if (sp > i + 5) take = sp - i; }
    line[n++] = message.substring(i, i + take);
    i += take;
    while (i < (int)message.length() && message.charAt(i) == ' ') i++;
  }
  int top = 16 + (48 - n * 11) / 2;
  for (int k = 0; k < n; k++) ctr(line[k].c_str(), top + k * 11, 1);
  oled.display();
}

// ---- text that is too wide simply travels ----
static void marquee(const char* s, int y, int size) {
  int w = (int)strlen(s) * 6 * size;
  if (w <= SCRW - 4) { ctr(s, y, size); return; }
  int span = w + 30;
  int off = (millis() / 45) % span;
  oled.setTextSize(size);
  oled.setCursor(2 - off, y);        oled.print(s);
  oled.setCursor(2 - off + span, y); oled.print(s);
  oled.fillRect(0, y - 1, 2, 8 * size + 2, SSD1306_BLACK);
  oled.fillRect(SCRW - 2, y - 1, 2, 8 * size + 2, SSD1306_BLACK);
}
// two centred lines, broken on a space
static void ctr2(const char* s, int y1, int y2) {
  int n = strlen(s);
  if (n <= RD_COLS) { ctr(s, (y1 + y2) / 2, 1); return; }
  int cut = RD_COLS;
  while (cut > 4 && s[cut] != ' ') cut--;
  if (cut <= 4) cut = RD_COLS;
  char a[RD_COLS + 2];
  int k = min(cut, RD_COLS);
  memcpy(a, s, k); a[k] = 0;
  ctr(a, y1, 1);
  ctr(s + (s[cut] == ' ' ? cut + 1 : cut), y2, 1);
}

static void spinner(int cy) {
  int a = (millis() / 110) % 8;
  for (int i = 0; i < 8; i++) {
    float th = i * 0.7854f;
    int rr = (i == a) ? 9 : 5;
    oled.drawPixel(SCRW / 2 + cosf(th) * rr, cy + sinf(th) * rr, SSD1306_WHITE);
  }
}

// ================================================================
//  FAITH
// ================================================================
static const char* faithLabel(int i, char* b, size_t n) { snprintf(b, n, "%s", F_NAME[i]); return b; }
static const char* surahLabel(int i, char* b, size_t n) { snprintf(b, n, "%d %s", i + 1, SURAH[i].name); return b; }
static const char* mornLabel(int i, char* b, size_t n)  { snprintf(b, n, "%s", MORNING[i].title); return b; }
static const char* eveLabel(int i, char* b, size_t n)   { snprintf(b, n, "%s", EVENING[i].title); return b; }

static void drawZikr() {
  oled.clearDisplay();
  const ZikrStep& z = ZIKR[zikrStep];
  char r[12];
  snprintf(r, sizeof(r), "%d/100", zikrTotal);
  titleBar("ZIKR", r);

  if (zikrTotal >= 100) {
    ctr("COMPLETE", 22, 2);
    ctr("One hundred", 44, 1);
    ctr("Four knocks to reset", 54, 1);
    oled.display();
    return;
  }

  int w2 = (int)strlen(z.text) * 12;
  if (w2 <= SCRW - 4) ctr(z.text, 18, 2);
  else                marquee(z.text, 20, 1);

  marquee(z.meaning, 34, 1);

  char c[16];
  snprintf(c, sizeof(c), "%d of %d", zikrDone, z.count);
  ctr(c, 46, 1);

  int fw = (SCRW - 8) * zikrTotal / 100;
  oled.drawRect(4, 56, SCRW - 8, 6, SSD1306_WHITE);
  if (fw > 2) oled.fillRect(5, 57, fw - 2, 4, SSD1306_WHITE);
  oled.display();
}

static void drawNames() {
  oled.clearDisplay();
  char r[12];
  snprintf(r, sizeof(r), "%d/99", subIdx + 1);
  titleBar("99 NAMES", r);
  const DivineName& d = NAME99[subIdx];
  if ((int)strlen(d.name) <= 10) ctr(d.name, 20, 2);
  else                           ctr(d.name, 24, 1);
  oled.drawFastHLine(20, 40, SCRW - 40, SSD1306_WHITE);
  ctr2(d.meaning, 46, 55);
  oled.display();
}

// the card in the carousel
static void drawFaithCard() {
  oled.clearDisplay();
  bar("FAITH");
  bookIcon(SCRW / 2, 32);
  ctr("Two knocks to open", 52, 1);
  oled.display();
}

static void drawFaith() {
  if (depth == 0) { drawFaithCard(); return; }
  if (depth == 1) {
    char r[10]; snprintf(r, sizeof(r), "%d/%d", itemIdx + 1, F_COUNT);
    drawList("FAITH", r, F_COUNT, itemIdx, faithLabel);
    return;
  }
  if (depth == 2) {
    char r[12];
    switch (itemIdx) {
      case F_ZIKR:  drawZikr();  break;
      case F_NAMES: drawNames(); break;
      case F_QURAN:
        snprintf(r, sizeof(r), "%d/114", subIdx + 1);
        drawList("QURAN", r, 114, subIdx, surahLabel);
        break;
      case F_MORNING:
        snprintf(r, sizeof(r), "%d/%d", subIdx + 1, MORNING_N);
        drawList("MORNING", r, MORNING_N, subIdx, mornLabel);
        break;
      default:
        snprintf(r, sizeof(r), "%d/%d", subIdx + 1, EVENING_N);
        drawList("EVENING", r, EVENING_N, subIdx, eveLabel);
        break;
    }
    return;
  }
  drawReader(rdTitle.c_str());                      // depth 3
}

// ================================================================
//  SHORT READS
// ================================================================
static const char* readLabel(int i, char* b, size_t n) { snprintf(b, n, "%s", readTitle[i].c_str()); return b; }

static void drawReads() {
  if (storyBusy) {
    oled.clearDisplay();
    titleBar("SHORT READS", "");
    ctr("Writing one for you", 24, 1);
    spinner(46);
    oled.display();
    return;
  }

  if (depth == 0) {
    oled.clearDisplay();
    bar("SHORT READS");
    bookIcon(SCRW / 2, 30);
    char l[26];
    if (readCount) snprintf(l, sizeof(l), "%d on the shelf", readCount);
    else           snprintf(l, sizeof(l), "%s", storyState.c_str());
    ctr(l, 45, 1);
    ctr(readCount ? "Two knocks to open" : "Four knocks to write", 55, 1);
    oled.display();
    return;
  }

  if (depth == 1) {
    if (!readCount) {
      oled.clearDisplay();
      titleBar("SHORT READS", "");
      ctr(storyState.c_str(), 24, 1);
      ctr(cfgKey.length() ? "Knock four times" : "Add a key on the page", 40, 1);
      ctr(cfgKey.length() ? "to write a new one" : "", 50, 1);
      oled.display();
      return;
    }
    char r[10]; snprintf(r, sizeof(r), "%d/%d", itemIdx + 1, readCount);
    drawList("SHORT READS", r, readCount, itemIdx, readLabel);
    return;
  }
  drawReader(rdTitle.c_str());
}

// ================================================================
//  SYSTEM
//    Four facts, generously spaced. No gauge, no crowding.
// ================================================================
static void drawSystem() {
  oled.clearDisplay();
  titleBar("SYSTEM", FW_VERSION);

  char v[22];
  const int LY[3] = { 16, 28, 40 };
  const char* LB[3] = { "Uptime", "Network", "Memory" };

  unsigned long s = millis() / 1000UL;
  if (s >= 3600UL) snprintf(v, sizeof(v), "%luh %lum", s / 3600UL, (s / 60UL) % 60UL);
  else             snprintf(v, sizeof(v), "%lum", s / 60UL);
  String vals[3];
  vals[0] = v;
  if (online())      snprintf(v, sizeof(v), "%d dBm", (int)WiFi.RSSI());
  else if (rescueAP) snprintf(v, sizeof(v), "hotspot");
  else               snprintf(v, sizeof(v), "offline");
  vals[1] = v;
  snprintf(v, sizeof(v), "%u kB", (unsigned)(ESP.getFreeHeap() / 1024));
  vals[2] = v;

  for (int i = 0; i < 3; i++) {
    at(4, LY[i], LB[i]);
    oled.setCursor(SCRW - 4 - (int)vals[i].length() * 6, LY[i]);
    oled.print(vals[i]);
  }

  oled.drawFastHLine(4, 51, SCRW - 8, SSD1306_WHITE);
  String ip = online() ? WiFi.localIP().toString()
            : rescueAP ? WiFi.softAPIP().toString()
                       : String("no address");
  ctr(ip.c_str(), 55, 1);
  oled.display();
}

// ---- the work session ----
static void drawFocus() {
  oled.clearDisplay();

  if (millis() < flashUntil) {
    oled.fillRect(0, 0, SCRW, SCRH, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    int n = strlen(flashWord);
    int size = n * 12 <= SCRW - 8 ? 2 : 1;
    ctr(flashWord, size == 2 ? 20 : 26, size);
    if (breakDue) ctr("Walk for a minute", 42, 1);
    oled.setTextColor(SSD1306_WHITE);
    oled.display();
    return;
  }

  if (!sessionRunning()) {
    bar("FOCUS");
    if (taskCount) {
      char l[26];
      snprintf(l, sizeof(l), "%d ready to run", taskCount);
      ctr(l, 24, 1);
      ctr("Start it on the page", 42, 1);
    } else {
      ctr("Nothing planned", 24, 1);
      ctr("A clear desk is a", 40, 1);
      ctr("good place to begin", 50, 1);
    }
    oled.display();
    return;
  }

  // Between showings it says something instead of counting. Same
  // screen, different thing to look at, and the panel is spared a
  // countdown burned into one place.
  if (fzPhase == FZ_QUOTE) {
    long left = (long)(taskEnd - millis()) / 1000L;
    if (left < 0) left = 0;
    char m[20];
    snprintf(m, sizeof(m), "%ld min left", (left + 59) / 60);
    bar("FOCUS");
    ctr(fzLine, 26, 1);
    ctr(m, 44, 1);
    oled.display();
    return;
  }

  const Task& t = tasks[taskIdx];
  bool brk = isBreak(t.name);
  char r[12];
  snprintf(r, sizeof(r), "%d/%d", taskIdx + 1, taskCount);
  titleBar(brk ? "BREAK" : "FOCUS", r);

  long left = (long)(taskEnd - millis()) / 1000L;
  if (left < 0) left = 0;
  char big[10];
  snprintf(big, sizeof(big), "%ld:%02ld", left / 60, left % 60);
  oled.setTextSize(3);
  oled.setCursor((SCRW - (int)strlen(big) * 18) / 2, 16);
  oled.print(big);

  long total = (long)t.mins * 60;
  int bw = SCRW - 16;
  int fill = total > 0 ? (int)((bw - 2) * (total - left) / total) : 0;
  oled.drawRect(8, 40, bw, 6, SSD1306_WHITE);
  if (fill > 0) oled.fillRect(9, 41, fill, 4, SSD1306_WHITE);

  marquee(t.name.c_str(), 54, 1);
  oled.display();
}

static void drawAccel() {
  oled.clearDisplay();
  titleBar("ACCELEROMETER", "");
  // a live dot for where it is leaning
  oled.drawRect(2, 14, 30, 30, SSD1306_WHITE);
  oled.drawFastHLine(14, 29, 7, SSD1306_WHITE);
  oled.drawFastVLine(17, 26, 7, SSD1306_WHITE);
  float tx, ty;
  faceTilt(tx, ty);
  int px = 17 + (int)constrain(tx * 22.0f, -12.0f, 12.0f);
  int py = 29 - (int)constrain(ty * 22.0f, -12.0f, 12.0f);
  oled.fillCircle(px, py, 2, SSD1306_WHITE);

  char l[24];
  snprintf(l, sizeof(l), "x %+.2f", ax); at(38, 15, l);
  snprintf(l, sizeof(l), "y %+.2f", ay); at(38, 26, l);
  snprintf(l, sizeof(l), "z %+.2f", az); at(38, 37, l);

  snprintf(l, sizeof(l), "taps %lu %lu %lu %lu",
           (unsigned long)min(99UL, (unsigned long)cTap),
           (unsigned long)min(99UL, (unsigned long)cDouble),
           (unsigned long)min(99UL, (unsigned long)cTriple),
           (unsigned long)min(99UL, (unsigned long)cQuad));
  ctr(l, 46, 1);
  snprintf(l, sizeof(l), "falls %lu  shakes %lu",
           (unsigned long)min(99UL, (unsigned long)cFall),
           (unsigned long)min(99UL, (unsigned long)cShake));
  ctr(l, 55, 1);
  oled.display();
}

static void drawAbout() {
  oled.clearDisplay();
  titleBar("ABOUT", FW_VERSION);
  ctr("Developed by Ahmed", 16, 1);
  oled.drawFastHLine(14, 28, SCRW - 28, SSD1306_WHITE);
  ctr("linkedin.com/in/", 33, 1);
  ctr("ahmed-al-imaad", 43, 1);
  ctr("www.iotcart.in", 54, 1);
  oled.display();
}

// ================================================================
//  THE MAC
// ================================================================
//  A robot head, because that is what is on the menu bar at the other
//  end. The eyes are what carry the state: two of them when the link
//  is up, closed when it goes.
static void robotHead(int cx, int cy, bool linked) {
  oled.drawRoundRect(cx - 19, cy - 15, 38, 30, 6, SSD1306_WHITE);
  oled.drawFastVLine(cx, cy - 21, 6, SSD1306_WHITE);
  oled.fillCircle(cx, cy - 23, 2, SSD1306_WHITE);
  oled.drawFastHLine(cx - 23, cy - 2, 4, SSD1306_WHITE);   // ears
  oled.drawFastHLine(cx + 19, cy - 2, 4, SSD1306_WHITE);
  if (linked) {
    oled.fillCircle(cx - 8, cy - 3, 4, SSD1306_WHITE);
    oled.fillCircle(cx + 8, cy - 3, 4, SSD1306_WHITE);
  } else {                                     // asleep, not broken
    oled.drawFastHLine(cx - 12, cy - 3, 8, SSD1306_WHITE);
    oled.drawFastHLine(cx + 4, cy - 3, 8, SSD1306_WHITE);
  }
  oled.drawFastHLine(cx - 6, cy + 7, 12, SSD1306_WHITE);
}

//  Said once when the Mac arrives and once when it goes. Both are
//  short, because neither is news you have to act on.
static void drawLinkCard() {
  oled.clearDisplay();
  long since = (long)(linkCardUntil - millis());
  robotHead(SCRW / 2, 26, linkCardJoin);
  if (linkCardJoin) {
    int step = (int)((1600 - since) / 200);   // arcs go out as it settles
    for (int i = 0; i < 3; i++)
      if (step > i) {
        oled.drawCircle(SCRW / 2, 26, 26 + i * 5, SSD1306_WHITE);
        oled.fillRect(0, 0, SCRW, 10, SSD1306_BLACK);
        oled.fillRect(0, 48, SCRW, 16, SSD1306_BLACK);
      }
    ctr("Connected to Mac", 54, 1);
  } else ctr("Mac disconnected", 54, 1);
  oled.display();
}

//  Six digits, big enough to read across a desk. They live for three
//  minutes and are never the same twice.
static void drawPair() {
  oled.clearDisplay();
  bar("PAIR");
  if (pairCode < 0 || millis() > pairUntil) {
    ctr("Two knocks for a code", 26, 1);
    ctr(cfgLock ? "A Mac is paired" : "Open to any Mac", 44, 1);
    oled.display();
    return;
  }
  char c[8];
  snprintf(c, sizeof(c), "%06d", pairCode);
  ctr(c, 22, 2);
  long left = ((long)(pairUntil - millis())) / 1000L;
  char t[24];
  snprintf(t, sizeof(t), "Type it in, %lds", left < 0 ? 0L : left);
  ctr(t, 48, 1);
  oled.display();
}

//  What the Mac copied, what it pasted, or a word about standing up.
//  Held for a few seconds and then gone, leaving the message alone.
static void drawToast() {
  oled.clearDisplay();
  const char* head = "FROM YOUR MAC";
  if (toastKind == "copy")  head = "COPIED";
  if (toastKind == "paste") head = "PASTED";
  if (toastKind == "break") head = "TAKE A BREAK";
  bar(head);
  if (toastKind == "break") {
    long m = (long)(toastUntil - millis()) / 1000L;
    ctr(toastText.length() ? toastText.c_str() : "Stand up, look away", 24, 1);
    ctr("Knock twice to snooze", 40, 1);
    int bw = SCRW - 30;
    oled.drawRect(15, 52, bw, 5, SSD1306_WHITE);
    if (m > 0) oled.fillRect(16, 53, (bw - 2) * constrain((int)m, 0, 20) / 20, 3, SSD1306_WHITE);
  } else {
    // two lines of it, and no more: this is a glance, not a read
    String t = toastText;
    if (t.length() <= 21) ctr(t.c_str(), 28, 1);
    else {
      int cut = 21;
      for (int i = 21; i > 8; i--) if (t[i] == ' ') { cut = i; break; }
      String a = t.substring(0, cut); a.trim();
      String b = t.substring(cut);    b.trim();
      if (b.length() > 21) { b = b.substring(0, 20); b += "…"; }
      ctr(a.c_str(), 22, 1);
      ctr(b.c_str(), 34, 1);
    }
  }
  oled.display();
}

//  Where the pointer is on the Mac, drawn as somewhere to look. The
//  eyes are hand drawn here rather than left to the library, because
//  the library moves them on its own schedule and this has to follow
//  the hand exactly.
static void drawFollow() {
  oled.clearDisplay();
  bool live = millis() < curUntil;
  float fx = live ? curX : 0, fy = live ? curY : 0;
  for (int e = 0; e < 2; e++) {
    int cx = e ? 86 : 42, cy = 32;
    oled.fillRoundRect(cx - 21, cy - 21, 42, 42, 10, SSD1306_WHITE);
    int px = cx + (int)(fx * 11.0f);
    int py = cy + (int)(fy * 11.0f);
    oled.fillCircle(px, py, 8, SSD1306_BLACK);
    oled.fillCircle(px + 3, py - 3, 2, SSD1306_WHITE);
  }
  if (!live) ctr("waiting for the Mac", 56, 1);
  oled.display();
}

//  Something slow to rest on. Three of them, and it moves to the next
//  one every half minute so no single pattern sits on the panel.
static void drawRelax() {
  unsigned long t = millis();
  oled.clearDisplay();
  if (relaxKind == 0) {
    // a circle that breathes: four seconds out, four back, which is
    // roughly the pace you would want to be breathing at
    float ph = (t % 8000UL) / 8000.0f;
    float k = ph < 0.5f ? ph * 2.0f : (1.0f - ph) * 2.0f;
    int r = 6 + (int)(k * 20.0f);
    oled.drawCircle(SCRW / 2, 32, r, SSD1306_WHITE);
    oled.drawCircle(SCRW / 2, 32, r / 2, SSD1306_WHITE);
    oled.fillCircle(SCRW / 2, 32, 2, SSD1306_WHITE);
    ctr(ph < 0.5f ? "in" : "out", 56, 1);
  } else if (relaxKind == 1) {
    // specks drifting past, each at its own pace
    for (int i = 0; i < 28; i++) {
      int sp = 1 + (i % 4);
      int x = (int)((i * 37 + t / (60 / sp)) % SCRW);
      int y = (i * 23) % SCRH;
      if (sp > 2) oled.fillCircle(x, y, 1, SSD1306_WHITE);
      else        oled.drawPixel(x, y, SSD1306_WHITE);
    }
  } else {
    // two slow waves crossing, which never quite repeat
    for (int x = 0; x < SCRW; x++) {
      float a = sinf(x * 0.09f + t * 0.0011f) * 13.0f;
      float b = sinf(x * 0.05f - t * 0.0007f) * 9.0f;
      oled.drawPixel(x, 32 + (int)a, SSD1306_WHITE);
      oled.drawPixel(x, 32 + (int)b, SSD1306_WHITE);
    }
  }
  oled.display();
}

//  A screenful the Mac drew. One knock clears it early.
static void drawCanvas() {
  if (!canvasBuf) return;
  oled.clearDisplay();
  memcpy(oled.getBuffer(), canvasBuf, SCRW * SCRH / 8);
  oled.display();
}

static void drawSettings() {
  oled.clearDisplay();
  if (depth == 0) {
    bar("SETTINGS");
    gearIcon(SCRW / 2, 32, 11);
    ctr("Two knocks to open", 52, 1);
    oled.display();
    return;
  }
  if (depth == 2 && itemIdx == C_ABOUT) { drawAbout(); return; }
  if (depth == 2 && itemIdx == C_ACCEL) { drawAccel(); return; }
  if (depth == 2 && itemIdx == C_PAIR)  { drawPair();  return; }
  bar(depth == 2 ? "CHANGE" : "SETTINGS");

  char v[18];
  int first = itemIdx > 3 ? itemIdx - 3 : 0;
  if (first > C_COUNT - 4) first = C_COUNT - 4;
  if (first < 0) first = 0;
  for (int r = 0; r < 4 && first + r < C_COUNT; r++) {
    int i = first + r, y = 14 + r * 12;
    bool on = (i == itemIdx);
    if (on) { oled.fillRect(0, y - 2, SCRW, 12, SSD1306_WHITE); oled.setTextColor(SSD1306_BLACK); }
    else      oled.setTextColor(SSD1306_WHITE);
    at(3, y, C_NAME[i]);
    switch (i) {
      case C_BRIGHT: { int k = 0;
                       for (int j = 0; j < BRIGHT_N; j++) if (BRIGHT_OPTS[j] == cfgBright) k = j;
                       snprintf(v, sizeof(v), "%s", BRIGHT_NAME[k]); break; }
      case C_FACE:   snprintf(v, sizeof(v), "%s", FACE_NAME[cfgFace]); break;
      case C_PRAYER: snprintf(v, sizeof(v), "%s", prayerOk ? "saved" : "none"); break;
      case C_ACCEL:  snprintf(v, sizeof(v), "x2"); break;
      case C_PAIR:   snprintf(v, sizeof(v), "%s", cfgLock ? "paired" : "x2"); break;
      case C_CONTROL:snprintf(v, sizeof(v), "%s", cfgTilt ? "tilt" : "taps"); break;
      case C_ABOUT:  snprintf(v, sizeof(v), "x2"); break;
      case C_SLEEP:  if (!sleepSecs())         snprintf(v, sizeof(v), "never");
                     else if (sleepSecs() < 60) snprintf(v, sizeof(v), "%ds", sleepSecs());
                     else                       snprintf(v, sizeof(v), "%dm", sleepSecs() / 60); break;
      case C_TURN:   snprintf(v, sizeof(v), "%s", cfgAutoTurn ? "auto" : "knock"); break;
      case C_POPUP:  if (!popupSecs()) snprintf(v, sizeof(v), "off");
                     else snprintf(v, sizeof(v), "%ds", popupSecs()); break;
      case C_EYES:   snprintf(v, sizeof(v), "%s", STYLES[cfgEyes].name); break;
      case C_HOTSPOT:snprintf(v, sizeof(v), "%s", rescueAP ? "on" : "x2"); break;
      case C_UPDATE: snprintf(v, sizeof(v), "%s", online() ? "x2" : "offline"); break;
      default:       snprintf(v, sizeof(v), "x2"); break;
    }
    oled.setCursor(SCRW - 3 - (int)strlen(v) * 6, y);
    oled.print(v);
  }
  oled.setTextColor(SSD1306_WHITE);
  oled.display();
}

static void drawOta() {
  oled.clearDisplay();
  bar("UPDATE");
  if (otaStatus2.length()) {                 // a failure worth explaining
    ctr(otaStatus.c_str(), 16, 1);
    ctr(otaStatus2.c_str(), 27, 1);
  } else ctr(otaStatus.c_str(), 22, 1);
  if (otaPct >= 0) {
    int bw = SCRW - 24;
    oled.drawRect(12, 36, bw, 9, SSD1306_WHITE);
    int f = (bw - 4) * constrain(otaPct, 0, 100) / 100;
    if (f > 0) oled.fillRect(14, 38, f, 5, SSD1306_WHITE);
    char p[8];
    snprintf(p, sizeof(p), "%d%%", otaPct);
    ctr(p, 50, 1);
  } else spinner(otaStatus2.length() ? 48 : 44);
  oled.display();
}

// ================================================================
//  CHOOSING AN UPDATE
// ================================================================
static void dlIcon(int cx, int cy) {
  oled.drawFastVLine(cx, cy - 8, 9, SSD1306_WHITE);
  oled.drawLine(cx - 4, cy - 3, cx, cy + 2, SSD1306_WHITE);
  oled.drawLine(cx + 4, cy - 3, cx, cy + 2, SSD1306_WHITE);
  oled.drawFastHLine(cx - 8, cy + 6, 17, SSD1306_WHITE);
  oled.drawFastVLine(cx - 8, cy + 2, 5, SSD1306_WHITE);
  oled.drawFastVLine(cx + 8, cy + 2, 5, SSD1306_WHITE);
}
static void histIcon(int cx, int cy) {
  oled.drawCircle(cx, cy, 9, SSD1306_WHITE);
  oled.drawFastVLine(cx, cy - 6, 7, SSD1306_WHITE);
  oled.drawFastHLine(cx - 5, cy, 6, SSD1306_WHITE);
  oled.drawLine(cx - 9, cy - 5, cx - 5, cy - 9, SSD1306_WHITE);
  oled.drawLine(cx - 9, cy - 5, cx - 4, cy - 2, SSD1306_WHITE);
}
static void yesNo(bool yes) {
  // Yes on the left, No on the right, whichever is picked filled in
  // RoboEyes already owns N, NE, E, SE, S, SW, W and NW as direction
  // macros, so nothing here gets a two letter capital name.
  const int yesX = 29, yesW = 32, noX = 73, noW = 26, boxY = 36, boxH = 14;
  if (yes) { oled.fillRect(yesX, boxY, yesW, boxH, SSD1306_WHITE);
             oled.drawRect(noX, boxY, noW, boxH, SSD1306_WHITE); }
  else     { oled.drawRect(yesX, boxY, yesW, boxH, SSD1306_WHITE);
             oled.fillRect(noX, boxY, noW, boxH, SSD1306_WHITE); }
  oled.setTextSize(1);
  oled.setTextColor(yes ? SSD1306_BLACK : SSD1306_WHITE);
  oled.setCursor(yesX + 7, boxY + 4); oled.print("Yes");
  oled.setTextColor(yes ? SSD1306_WHITE : SSD1306_BLACK);
  oled.setCursor(noX + 7, boxY + 4); oled.print("No");
  oled.setTextColor(SSD1306_WHITE);
}
static void drawUpdateUI() {
  oled.clearDisplay();
  char r[12];
  switch (upState) {
    case U_MENU: {
      snprintf(r, sizeof(r), "%d/2", upPick + 1);
      titleBar("UPDATE", r);
      const int TX[2] = { 14, 74 };
      for (int i = 0; i < 2; i++) {
        if (i == upPick) oled.drawRoundRect(TX[i] - 2, 13, 42, 28, 4, SSD1306_WHITE);
        if (i == 0) dlIcon(TX[i] + 19, 27);
        else        histIcon(TX[i] + 19, 27);
      }
      ctr(upPick == 0 ? "Newest release" : "Earlier releases", 45, 1);
      ctr("1 next   2 open", 55, 1);
      break;
    }
    case U_ASK:
      titleBar("INSTALL", "");
      ctr(upTag.length() ? upTag.c_str() : "unknown", 15, 1);
      ctr("Put this one on?", 26, 1);
      yesNo(upYes);
      ctr("1 moves  2 confirms", 55, 1);
      break;
    case U_LIST: {
      snprintf(r, sizeof(r), "%d/%d", relSel + 1, relCount);
      titleBar("RELEASES", r);
      int first = relSel > 3 ? relSel - 3 : 0;
      if (first > relCount - 4) first = relCount - 4;
      if (first < 0) first = 0;
      for (int k = 0; k < 4 && first + k < relCount; k++) {
        int i = first + k, y = 14 + k * 12;
        bool on = (i == relSel);
        if (on) { oled.fillRect(0, y - 2, SCRW, 12, SSD1306_WHITE); oled.setTextColor(SSD1306_BLACK); }
        else      oled.setTextColor(SSD1306_WHITE);
        String t = relTag[i];
        if (t == String("v" FW_VERSION) || t == String(FW_VERSION)) t += " (on now)";
        oled.setTextSize(1);
        oled.setCursor(3, y);
        for (int c = 0; c < 20 && c < (int)t.length(); c++) oled.write(t[c]);
        oled.setTextColor(SSD1306_WHITE);
      }
      if (relCount > 4) {
        int h = max(4, 48 * 4 / relCount);
        int yy = 14 + (48 - h) * relSel / max(1, relCount - 1);
        oled.drawFastVLine(126, 14, 48, SSD1306_WHITE);
        oled.fillRect(125, yy, 3, h, SSD1306_WHITE);
      }
      break;
    }
    case U_NONE:
      titleBar("UPDATE", "");
      ctr("Already current", 20, 1);
      ctr(upTag.c_str(), 32, 1);
      ctr("Knock to go back", 50, 1);
      break;
    default:
      titleBar("UPDATE", "");
      ctr(upMsg.length() ? upMsg.c_str() : "Something went wrong", 24, 1);
      ctr("Knock to go back", 46, 1);
      break;
  }
  oled.display();
}
// ================================================================
//  GAMES
// ================================================================
//  Both are played by tilting. The trouble with tilt is that it only
//  means anything relative to how the thing happens to be sitting,
//  and this one could be upright on a desk or lying flat. So rather
//  than guess, it asks once: hold still, tilt right, tilt away. That
//  mapping is kept in flash and never asked for again. The resting
//  position is re-measured every time a game starts, because that is
//  the part that drifts when you pick it up and put it down.
// ================================================================
enum { G_SNAKE = 0, G_BRICK, G_CAR, G_CATCH, G_PONG, G_ROLL, G_COUNT };
const char* G_NAME[G_COUNT] = { "Snake", "Brick", "Car", "Catch", "Pong", "Roll" };
#define G_PER_PAGE 2
#define G_PAGES ((G_COUNT + G_PER_PAGE - 1) / G_PER_PAGE)

enum { GS_CAL_STILL = 0, GS_CAL_RIGHT, GS_CAL_AWAY, GS_READY, GS_PLAY, GS_PAUSE, GS_OVER };
int  gState = GS_READY;
int  gamePending = -1;
int  gScore = 0, gBest[G_COUNT] = { 0, 0, 0, 0, 0, 0 };
unsigned long gNext = 0, gStamp = 0;


static void saveTiltMap() {
  char b[24];
  snprintf(b, sizeof(b), "%d,%d,%d,%d", mapAxX, mapSgnX, mapAxY, mapSgnY);
  prefs.putString("tiltmap", b);
}
static void loadTiltMap() {
  String s = prefs.getString("tiltmap", "");
  if (!s.length()) return;
  int v[4] = { -1, 1, -1, 1 }, i = 0, n = 0;
  while (n < 4 && i <= (int)s.length()) {
    int c = s.indexOf(',', i); if (c < 0) c = s.length();
    v[n++] = s.substring(i, c).toInt();
    i = c + 1;
  }
  if (v[0] >= 0 && v[0] < 3 && v[2] >= 0 && v[2] < 3 && v[0] != v[2]) {
    mapAxX = v[0]; mapSgnX = v[1]; mapAxY = v[2]; mapSgnY = v[3];
  }
}

static void tiltRead(float& tx, float& ty) {
  float v[3] = { ax, ay, az };
  tx = (mapAxX >= 0) ? mapSgnX * (v[mapAxX] - restV[mapAxX]) : 0;
  ty = (mapAxY >= 0) ? mapSgnY * (v[mapAxY] - restV[mapAxY]) : 0;
}

// ---------------- Snake ----------------
#define SN_CELL 4
#define SN_COLS 32
#define SN_TOP  12
#define SN_ROWS 13
#define SN_MAX  170
uint8_t snX[SN_MAX], snY[SN_MAX];
int  snLen = 3, snDir = 0, snPend = 0;
uint8_t foodX = 20, foodY = 6;
unsigned long snPace = 220;

static void snPlaceFood() {
  for (int tries = 0; tries < 200; tries++) {
    uint8_t fx = random(SN_COLS), fy = random(SN_ROWS);
    bool hit = false;
    for (int i = 0; i < snLen; i++) if (snX[i] == fx && snY[i] == fy) { hit = true; break; }
    if (!hit) { foodX = fx; foodY = fy; return; }
  }
}
static void snReset() {
  snLen = 3; snDir = 0; snPend = 0; snPace = 220;
  for (int i = 0; i < snLen; i++) { snX[i] = 6 - i; snY[i] = SN_ROWS / 2; }
  gScore = 0;
  snPlaceFood();
}
static void snStep() {
  float tx, ty;
  tiltRead(tx, ty);
  const float T = 0.20f;
  int want = -1;
  if (fabsf(tx) > fabsf(ty)) { if (tx > T) want = 0; else if (tx < -T) want = 2; }
  else                       { if (ty > T) want = 3; else if (ty < -T) want = 1; }
  if (want >= 0 && (want + 2) % 4 != snDir) snDir = want;   // no doubling back

  int nx = snX[0] + (snDir == 0 ? 1 : snDir == 2 ? -1 : 0);
  int ny = snY[0] + (snDir == 1 ? 1 : snDir == 3 ? -1 : 0);
  if (nx < 0 || nx >= SN_COLS || ny < 0 || ny >= SN_ROWS) { gState = GS_OVER; return; }
  for (int i = 0; i < snLen; i++) if (snX[i] == nx && snY[i] == ny) { gState = GS_OVER; return; }

  bool ate = (nx == foodX && ny == foodY);
  if (ate && snLen < SN_MAX) snLen++;
  for (int i = snLen - 1; i > 0; i--) { snX[i] = snX[i - 1]; snY[i] = snY[i - 1]; }
  snX[0] = nx; snY[0] = ny;
  if (ate) {
    gScore++;
    if (snPace > 120) snPace -= 5;
    snPlaceFood();
  }
}
static void snDraw() {
  for (int i = 0; i < snLen; i++)
    oled.fillRect(snX[i] * SN_CELL, SN_TOP + snY[i] * SN_CELL, 3, 3, SSD1306_WHITE);
  oled.drawRect(foodX * SN_CELL, SN_TOP + foodY * SN_CELL, 3, 3, SSD1306_WHITE);
  oled.drawPixel(foodX * SN_CELL + 1, SN_TOP + foodY * SN_CELL + 1, SSD1306_WHITE);
}


// ---------------- Brick ----------------
#define BR_COLS 8
#define BR_ROWS 3
#define BR_TOP  14
#define BR_W    16
#define BR_H    7
#define PAD_W   22
#define PAD_Y   59
bool  brick[BR_ROWS][BR_COLS];
float bx, by, bvx, bvy, padX;
int   brLives = 3, brLeft = 0, brLevel = 1;
bool  brStuck = true;
unsigned long brStuckAt = 0;
#define BR_AUTO_LAUNCH 1300UL      // so it never just sits there looking broken

// Each level lays the wall out differently, so it is not only the speed
// that changes as you climb.
static void brFill() {
  brLeft = 0;
  int shape = (brLevel - 1) % 5;
  for (int r = 0; r < BR_ROWS; r++)
    for (int c = 0; c < BR_COLS; c++) {
      bool on = true;
      switch (shape) {
        case 1: on = ((r + c) % 2) == 0;                 break;   // chequered
        case 2: on = (c >= r) && (c < BR_COLS - r);      break;   // a pyramid
        case 3: on = (r != 1) || (c % 3 != 1);           break;   // gaps in the middle
        case 4: on = (c < 2) || (c >= BR_COLS - 2) || r == 0; break; // a bowl
        default: on = true;                              break;
      }
      brick[r][c] = on;
      if (on) brLeft++;
    }
  if (!brLeft) { for (int c = 0; c < BR_COLS; c++) { brick[0][c] = true; brLeft++; } }
}
static void brServe() {
  brStuck = true;
  brStuckAt = millis();
  padX = (SCRW - PAD_W) / 2;
  bx = padX + PAD_W / 2; by = PAD_Y - 3;
  float sp = 0.85f + 0.10f * (brLevel - 1);
  if (sp > 2.0f) sp = 2.0f;
  bvx = (random(2) ? sp : -sp) * 0.75f;
  bvy = -sp;
}
static void brReset() {
  brLives = 3; brLevel = 1; gScore = 0;
  brFill(); brServe();
}
static void brStepGame() {
  float tx, ty;
  tiltRead(tx, ty);
  padX += tx * 26.0f;
  padX = constrain(padX, 0.0f, (float)(SCRW - PAD_W));

  // Waiting on a knock forever is how a game looks broken, so it lets
  // go by itself after a moment. A knock still launches it early.
  if (brStuck) {
    bx = padX + PAD_W / 2; by = PAD_Y - 3;
    if (millis() - brStuckAt > BR_AUTO_LAUNCH) brStuck = false;
    return;
  }

  bx += bvx; by += bvy;
  if (bx < 1)        { bx = 1;        bvx = -bvx; }
  if (bx > SCRW - 2) { bx = SCRW - 2; bvx = -bvx; }
  if (by < 12)       { by = 12;       bvy = -bvy; }

  if (bvy > 0 && by >= PAD_Y - 2 && by <= PAD_Y + 2 &&
      bx >= padX - 1 && bx <= padX + PAD_W + 1) {
    by = PAD_Y - 2;
    bvy = -fabsf(bvy);
    float off = ((bx - padX) / PAD_W) - 0.5f;
    bvx += off * 1.1f;
    bvx = constrain(bvx, -1.7f, 1.7f);
  }

  if (by >= BR_TOP && by < BR_TOP + BR_ROWS * BR_H) {
    int c = (int)bx / BR_W;
    int r = (int)(by - BR_TOP) / BR_H;
    if (c >= 0 && c < BR_COLS && r >= 0 && r < BR_ROWS && brick[r][c]) {
      brick[r][c] = false;
      brLeft--;
      gScore += 10;
      bvy = -bvy;
      if (!brLeft) { brLevel++; gScore += 50; brFill(); brServe(); return; }
    }
  }

  if (by > 63) {
    brLives--;
    if (brLives <= 0) { gState = GS_OVER; return; }
    brServe();
  }
}
static void brDraw() {
  for (int r = 0; r < BR_ROWS; r++)
    for (int c = 0; c < BR_COLS; c++)
      if (brick[r][c]) oled.fillRect(c * BR_W, BR_TOP + r * BR_H, BR_W - 1, BR_H - 2, SSD1306_WHITE);
  oled.fillRect((int)padX, PAD_Y, PAD_W, 3, SSD1306_WHITE);
  oled.fillRect((int)bx - 1, (int)by - 1, 2, 2, SSD1306_WHITE);
  for (int i = 0; i < brLives; i++) oled.fillRect(122 - i * 4, 12, 2, 2, SSD1306_WHITE);
}

// ---------------- Car ----------------
//  Three lanes. Tilt picks one, and it will not slide two across in a
//  single lean: you have to come back to level before it moves again.
#define CAR_LANES 3
#define CAR_LEFT  6
#define CAR_LANEW 38
#define CAR_W     14
#define CAR_H     11
#define CAR_Y     47
#define CAR_OBS   5
int   carLane = 1;
bool  carLatch = false;
float carSpeed = 1.0f, carScroll = 0;
struct CarObs { int8_t lane; float y; bool live; };
CarObs carObs[CAR_OBS];
int   carPassed = 0;

static int carLaneX(int l) { return CAR_LEFT + l * CAR_LANEW + (CAR_LANEW - CAR_W) / 2; }
static void carReset() {
  carLane = 1; carLatch = false; carSpeed = 1.0f; carScroll = 0; carPassed = 0; gScore = 0;
  for (int i = 0; i < CAR_OBS; i++) carObs[i].live = false;
  carObs[0].live = true; carObs[0].lane = random(CAR_LANES); carObs[0].y = -12;
}
static void carStep() {
  float tx, ty;
  tiltRead(tx, ty);
  if (!carLatch && tx > 0.28f)  { if (carLane < CAR_LANES - 1) carLane++; carLatch = true; }
  if (!carLatch && tx < -0.28f) { if (carLane > 0) carLane--;             carLatch = true; }
  if (fabsf(tx) < 0.14f) carLatch = false;

  carScroll += carSpeed;
  if (carScroll > 1000) carScroll -= 1000;

  int live = 0;
  for (int i = 0; i < CAR_OBS; i++) {
    if (!carObs[i].live) continue;
    live++;
    carObs[i].y += carSpeed;
    if (carObs[i].y > 64) {
      carObs[i].live = false;
      carPassed++;
      gScore += 10;
      if (carPassed % 5 == 0 && carSpeed < 3.2f) carSpeed += 0.18f;
      continue;
    }
    // hit?
    if (carObs[i].lane == carLane &&
        carObs[i].y + CAR_H > CAR_Y && carObs[i].y < CAR_Y + CAR_H) { gState = GS_OVER; return; }
  }
  // Feed in the next one only once the most recent has come far enough
  // down. Gating on the lowest one instead let them pile up together at
  // the top, and three abreast is a wall, not a challenge. The gap grows
  // with speed, so there is always time to read it.
  float highest = 1000;
  for (int i = 0; i < CAR_OBS; i++) if (carObs[i].live && carObs[i].y < highest) highest = carObs[i].y;
  if (live < CAR_OBS && highest > 20.0f + carSpeed * 9.0f) {
    // Whatever is still near the top is what you have to thread. Spawn
    // only into a lane that is clear there, and never take the last one:
    // a wall across all three would be unavoidable, not difficult.
    bool taken[CAR_LANES];
    for (int l = 0; l < CAR_LANES; l++) taken[l] = false;
    int nTaken = 0;
    for (int k = 0; k < CAR_OBS; k++)
      if (carObs[k].live && carObs[k].y < 30 && !taken[carObs[k].lane]) { taken[carObs[k].lane] = true; nTaken++; }
    if (nTaken >= CAR_LANES - 1) return;
    int freeLane[CAR_LANES], nf = 0;
    for (int l = 0; l < CAR_LANES; l++) if (!taken[l]) freeLane[nf++] = l;
    for (int i = 0; i < CAR_OBS; i++) if (!carObs[i].live) {
      carObs[i].live = true;
      carObs[i].y = -12;
      carObs[i].lane = freeLane[random(nf)];
      break;
    }
  }
}
static void carBody(int x, int y, bool mine) {
  // four wheels and a roof, so it reads as a car rather than a block
  oled.fillRect(x, y + 1, 2, 3, SSD1306_WHITE);
  oled.fillRect(x + CAR_W - 2, y + 1, 2, 3, SSD1306_WHITE);
  oled.fillRect(x, y + CAR_H - 4, 2, 3, SSD1306_WHITE);
  oled.fillRect(x + CAR_W - 2, y + CAR_H - 4, 2, 3, SSD1306_WHITE);
  oled.drawRoundRect(x + 2, y, CAR_W - 4, CAR_H, 3, SSD1306_WHITE);
  if (mine) oled.fillRect(x + 4, y + 3, CAR_W - 8, CAR_H - 6, SSD1306_WHITE);
  else      oled.drawFastHLine(x + 4, y + CAR_H / 2, CAR_W - 8, SSD1306_WHITE);
}
static void carDraw() {
  oled.drawFastVLine(CAR_LEFT - 2, 12, 52, SSD1306_WHITE);
  oled.drawFastVLine(CAR_LEFT + CAR_LANES * CAR_LANEW + 1, 12, 52, SSD1306_WHITE);
  int off = ((int)carScroll) % 12;
  for (int l = 1; l < CAR_LANES; l++) {
    int x = CAR_LEFT + l * CAR_LANEW;
    for (int y = 12 - off; y < 64; y += 12) oled.drawFastVLine(x, max(12, y), min(6, 64 - y), SSD1306_WHITE);
  }
  for (int i = 0; i < CAR_OBS; i++)
    if (carObs[i].live) carBody(carLaneX(carObs[i].lane), (int)carObs[i].y, false);
  carBody(carLaneX(carLane), CAR_Y, true);
}

// ---------------- Catch ----------------
#define CT_BASK 22
#define CT_Y    56
#define CT_MAX  6
float ctX;
struct CtItem { float x, y, v; bool live, bad; };
CtItem ctIt[CT_MAX];
int   ctLives = 3;
unsigned long ctNext = 0;

static void ctReset() {
  ctX = (SCRW - CT_BASK) / 2; ctLives = 3; gScore = 0;
  for (int i = 0; i < CT_MAX; i++) ctIt[i].live = false;
  ctNext = millis() + 400;
}
static void ctStep() {
  float tx, ty;
  tiltRead(tx, ty);
  ctX = constrain(ctX + tx * 26.0f, 0.0f, (float)(SCRW - CT_BASK));

  if (millis() > ctNext) {
    for (int i = 0; i < CT_MAX; i++) if (!ctIt[i].live) {
      ctIt[i].live = true;
      ctIt[i].bad = random(100) < 28;
      ctIt[i].x = 4 + random(SCRW - 12);
      ctIt[i].y = 12;
      ctIt[i].v = 0.55f + (gScore / 400.0f);
      if (ctIt[i].v > 2.0f) ctIt[i].v = 2.0f;
      break;
    }
    unsigned long gap = 900 - min(500, gScore);
    ctNext = millis() + gap + random(300);
  }

  for (int i = 0; i < CT_MAX; i++) {
    if (!ctIt[i].live) continue;
    ctIt[i].y += ctIt[i].v;
    if (ctIt[i].y >= CT_Y - 1 && ctIt[i].y <= CT_Y + 4 &&
        ctIt[i].x >= ctX - 2 && ctIt[i].x <= ctX + CT_BASK + 2) {
      ctIt[i].live = false;
      if (ctIt[i].bad) { ctLives--; if (ctLives <= 0) { gState = GS_OVER; return; } }
      else gScore += 10;
      continue;
    }
    if (ctIt[i].y > 64) ctIt[i].live = false;
  }
}
static void ctDraw() {
  oled.drawFastHLine((int)ctX, CT_Y + 3, CT_BASK, SSD1306_WHITE);
  oled.drawFastVLine((int)ctX, CT_Y, 4, SSD1306_WHITE);
  oled.drawFastVLine((int)ctX + CT_BASK - 1, CT_Y, 4, SSD1306_WHITE);
  for (int i = 0; i < CT_MAX; i++) {
    if (!ctIt[i].live) continue;
    int x = (int)ctIt[i].x, y = (int)ctIt[i].y;
    if (ctIt[i].bad) {                       // the ones to let through
      oled.drawLine(x - 2, y - 2, x + 2, y + 2, SSD1306_WHITE);
      oled.drawLine(x + 2, y - 2, x - 2, y + 2, SSD1306_WHITE);
    } else oled.fillRect(x - 1, y - 1, 3, 3, SSD1306_WHITE);
  }
  for (int i = 0; i < ctLives; i++) oled.fillRect(122 - i * 4, 12, 2, 2, SSD1306_WHITE);
}

// ---------------- Pong ----------------
#define PG_PAD 22
#define PG_MY  59
#define PG_AI  13
float pgMy, pgAi, pgX, pgY, pgVx, pgVy;
int   pgMe = 0, pgThem = 0;
#define PG_WIN 7

static void pgServe(bool toMe) {
  pgX = SCRW / 2; pgY = 36;
  pgVx = (random(2) ? 0.8f : -0.8f);
  pgVy = toMe ? 1.0f : -1.0f;
}
static void pgReset() {
  pgMy = pgAi = (SCRW - PG_PAD) / 2;
  pgMe = pgThem = 0; gScore = 0;
  pgServe(random(2));
}
static void pgStep() {
  float tx, ty;
  tiltRead(tx, ty);
  pgMy = constrain(pgMy + tx * 26.0f, 0.0f, (float)(SCRW - PG_PAD));

  // the other side is beatable on purpose: it cannot quite keep up
  float want = pgX - PG_PAD / 2;
  float d = want - pgAi;
  float lim = 1.02f + 0.04f * (pgMe + pgThem);      // keeps up, but not perfectly
  if (lim > 1.5f) lim = 1.5f;
  pgAi += constrain(d, -lim, lim);
  pgAi = constrain(pgAi, 0.0f, (float)(SCRW - PG_PAD));

  pgX += pgVx; pgY += pgVy;
  if (pgX < 1)        { pgX = 1;        pgVx = -pgVx; }
  if (pgX > SCRW - 2) { pgX = SCRW - 2; pgVx = -pgVx; }

  if (pgVy > 0 && pgY >= PG_MY - 2 && pgY <= PG_MY + 2 && pgX >= pgMy - 1 && pgX <= pgMy + PG_PAD + 1) {
    pgY = PG_MY - 2; pgVy = -fabsf(pgVy) * 1.04f;      // no endless rallies
    if (pgVy < -2.1f) pgVy = -2.1f;
    pgVx = constrain(pgVx + (((pgX - pgMy) / PG_PAD) - 0.5f) * 1.0f, -1.6f, 1.6f);
  }
  if (pgVy < 0 && pgY <= PG_AI + 3 && pgY >= PG_AI - 1 && pgX >= pgAi - 1 && pgX <= pgAi + PG_PAD + 1) {
    pgY = PG_AI + 3; pgVy = fabsf(pgVy) * 1.04f;
    if (pgVy > 2.1f) pgVy = 2.1f;
    pgVx = constrain(pgVx + (((pgX - pgAi) / PG_PAD) - 0.5f) * 1.0f, -1.6f, 1.6f);
  }

  if (pgY > 63) { pgThem++; if (pgThem >= PG_WIN) { gState = GS_OVER; return; } pgServe(false); }
  if (pgY < 12) { pgMe++;   gScore = pgMe * 10;
                  if (pgMe >= PG_WIN) { gState = GS_OVER; return; } pgServe(true); }
}
static void pgDraw() {
  for (int x = 2; x < SCRW - 2; x += 6) oled.drawFastHLine(x, 36, 3, SSD1306_WHITE);
  oled.fillRect((int)pgMy, PG_MY, PG_PAD, 3, SSD1306_WHITE);
  oled.fillRect((int)pgAi, PG_AI, PG_PAD, 3, SSD1306_WHITE);
  oled.fillRect((int)pgX - 1, (int)pgY - 1, 2, 2, SSD1306_WHITE);
}

// ---------------- Roll ----------------
//  The one that leans hardest on the sensor: the ball carries momentum,
//  so you have to lead it and then catch it again.
#define RL_HOLES 7
float rlX, rlY, rlVx, rlVy;
int   rlLives = 3, rlLevel = 1, rlN = 0;
struct Hole { int8_t x, y; };
Hole rlH[RL_HOLES];
int8_t rlGx, rlGy;

#define RL_GW 30
#define RL_GH 12
static bool rlBlockedAt(int px, int py) {
  for (int i = 0; i < rlN; i++) {
    int dx = px - rlH[i].x, dy = py - rlH[i].y;
    if (dx * dx + dy * dy < 64) return true;        // keep 8px clear of a hole
  }
  return false;
}
// A level where the holes happen to fence the goal off is not hard, it
// is broken. Flood fill a coarse grid from the start and only keep a
// layout the ball can actually get through.
static bool rlReachable() {
  static uint8_t seen[RL_GW * RL_GH];
  static uint16_t q[RL_GW * RL_GH];
  memset(seen, 0, sizeof(seen));
  int head = 0, tail = 0;
  int sx = constrain((12 - 3) / 4, 0, RL_GW - 1),  sy = constrain((20 - 15) / 4, 0, RL_GH - 1);
  int gx = constrain((rlGx - 3) / 4, 0, RL_GW - 1), gy = constrain((rlGy - 15) / 4, 0, RL_GH - 1);
  seen[sy * RL_GW + sx] = 1;
  q[tail++] = sy * RL_GW + sx;
  const int8_t D[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
  while (head < tail) {
    int c = q[head++];
    int cx = c % RL_GW, cy = c / RL_GW;
    if (cx == gx && cy == gy) return true;
    for (int d = 0; d < 4; d++) {
      int nx = cx + D[d][0], ny = cy + D[d][1];
      if (nx < 0 || ny < 0 || nx >= RL_GW || ny >= RL_GH) continue;
      int n = ny * RL_GW + nx;
      if (seen[n]) continue;
      if (rlBlockedAt(3 + nx * 4 + 2, 15 + ny * 4 + 2)) continue;
      seen[n] = 1;
      q[tail++] = n;
    }
  }
  return false;
}

static void rlPlace() {
  rlN = min(RL_HOLES, 2 + rlLevel);
  for (int attempt = 0; attempt < 16; attempt++) {
    for (int i = 0; i < rlN; i++) {
      rlH[i].x = 64; rlH[i].y = 40;                 // a sane spot if placing fails
      for (int tries = 0; tries < 40; tries++) {
        int8_t hx = 12 + random(SCRW - 24), hy = 20 + random(38);
        if (abs(hx - 12) + abs(hy - 20) < 26) continue;      // not on the start
        bool clash = false;
        for (int k = 0; k < i; k++)
          if (abs(rlH[k].x - hx) < 14 && abs(rlH[k].y - hy) < 14) clash = true;
        if (clash) continue;
        rlH[i].x = hx; rlH[i].y = hy;
        break;
      }
    }
    // A goal sitting on a hole can never be reached, so keep it clear.
    for (int tries = 0; tries < 60; tries++) {
      rlGx = 20 + random(SCRW - 40);
      rlGy = 20 + random(36);
      if (abs(rlGx - 12) + abs(rlGy - 20) < 40) continue;
      bool clear = true;
      for (int i = 0; i < rlN; i++)
        if (abs(rlH[i].x - rlGx) < 15 && abs(rlH[i].y - rlGy) < 15) clear = false;
      if (clear) break;
    }
    if (rlReachable()) return;
  }
}
static void rlServe() { rlX = 12; rlY = 20; rlVx = rlVy = 0; }
static void rlReset() { rlLives = 3; rlLevel = 1; gScore = 0; rlPlace(); rlServe(); }
static void rlStep() {
  float tx, ty;
  tiltRead(tx, ty);
  rlVx += tx * 0.42f;
  rlVy -= ty * 0.42f;                    // tilting away rolls it up the screen
  rlVx *= 0.93f; rlVy *= 0.93f;
  rlVx = constrain(rlVx, -2.4f, 2.4f);
  rlVy = constrain(rlVy, -2.4f, 2.4f);
  rlX += rlVx; rlY += rlVy;

  if (rlX < 3)        { rlX = 3;        rlVx = -rlVx * 0.5f; }
  if (rlX > SCRW - 4) { rlX = SCRW - 4; rlVx = -rlVx * 0.5f; }
  if (rlY < 15)       { rlY = 15;       rlVy = -rlVy * 0.5f; }
  if (rlY > 60)       { rlY = 60;       rlVy = -rlVy * 0.5f; }

  for (int i = 0; i < rlN; i++) {
    float dx = rlX - rlH[i].x, dy = rlY - rlH[i].y;
    if (dx * dx + dy * dy < 20) {
      rlLives--;
      if (rlLives <= 0) { gState = GS_OVER; return; }
      rlServe();
      return;
    }
  }
  float gx = rlX - rlGx, gy = rlY - rlGy;
  if (gx * gx + gy * gy < 26) {
    gScore += 50;
    rlLevel++;
    rlPlace();
    rlServe();
  }
}
static void rlDraw() {
  for (int i = 0; i < rlN; i++) {
    oled.drawCircle(rlH[i].x, rlH[i].y, 4, SSD1306_WHITE);
    oled.drawCircle(rlH[i].x, rlH[i].y, 2, SSD1306_WHITE);
  }
  oled.drawRect(rlGx - 4, rlGy - 4, 9, 9, SSD1306_WHITE);
  oled.fillRect(rlGx - 2, rlGy - 2, 5, 5, SSD1306_WHITE);
  oled.fillCircle((int)rlX, (int)rlY, 2, SSD1306_WHITE);
  for (int i = 0; i < rlLives; i++) oled.fillRect(122 - i * 4, 12, 2, 2, SSD1306_WHITE);
}

// ================================================================
//  SHARED
// ================================================================
static const char* bestKey(int g) {
  switch (g) {
    case G_SNAKE: return "bSnake";
    case G_BRICK: return "bBrick";
    case G_CAR:   return "bCar";
    case G_CATCH: return "bCatch";
    case G_PONG:  return "bPong";
    default:      return "bRoll";
  }
}
static void gameReset(int g) {
  switch (g) {
    case G_SNAKE: snReset(); break;
    case G_BRICK: brReset(); break;
    case G_CAR:   carReset(); break;
    case G_CATCH: ctReset(); break;
    case G_PONG:  pgReset(); break;
    default:      rlReset(); break;
  }
}
static void gameStart(int which) {
  gState = GS_READY;                     // the leans were just checked on the way in
  gStamp = millis();
  gameReset(which);
}
static const char* gameHint(int g) {
  switch (g) {
    case G_SNAKE: return "Tilt to steer";
    case G_BRICK: return "Tilt to slide";
    case G_CAR:   return "Tilt to change lane";
    case G_CATCH: return "Tilt to catch";
    case G_PONG:  return "Tilt to return it";
    default:      return "Tilt to roll it";
  }
}

// the teaching steps, and the short hold that finds the rest position
static void gameCalibrate() {
  readSensors();
  float v[3] = { ax, ay, az };

  if (gState == GS_CAL_STILL) {
    for (int i = 0; i < 3; i++) calAcc[i] += v[i];
    calN++;
    if (millis() - gStamp > 900 && calN > 4) {
      for (int i = 0; i < 3; i++) restV[i] = calAcc[i] / calN;
      gravAx = 0;
      for (int i = 1; i < 3; i++) if (fabsf(restV[i]) > fabsf(restV[gravAx])) gravAx = i;
      if (tiltTaught() && mapAxX != gravAx && mapAxY != gravAx) {
        gState = GS_READY; gStamp = millis();
      } else {
        mapAxX = mapAxY = -1;
        gState = GS_CAL_RIGHT;
      }
    }
    return;
  }
  if (gState == GS_CAL_RIGHT) {
    int best = -1; float bd = 0.30f;
    for (int i = 0; i < 3; i++) {
      if (i == gravAx) continue;
      float d = v[i] - restV[i];
      if (fabsf(d) > bd) { bd = fabsf(d); best = i; }
    }
    if (best >= 0) {
      mapAxX = best;
      mapSgnX = (v[best] - restV[best]) > 0 ? 1 : -1;
      gState = GS_CAL_AWAY;
      gStamp = millis();
    }
    return;
  }
  if (gState == GS_CAL_AWAY) {
    if (millis() - gStamp < 700) return;
    for (int i = 0; i < 3; i++) {
      if (i == gravAx || i == mapAxX) continue;
      float d = v[i] - restV[i];
      if (fabsf(d) > 0.30f) {
        mapAxY = i;
        mapSgnY = d > 0 ? 1 : -1;
        saveTiltMap();
        gState = GS_READY;
        gStamp = millis();
        return;
      }
    }
  }
}

static void serviceGame() {
  int which = itemIdx;
  unsigned long now = millis();
  readSensors();                       // the tick is faster than the input poll

  if (gState <= GS_CAL_AWAY) { gameCalibrate(); return; }
  if (gState == GS_READY) {
    if (now - gStamp > 1800) { gState = GS_PLAY; gNext = now; }
    return;
  }
  if (gState != GS_PLAY) return;
  if ((long)(now - gNext) < 0) return;

  switch (which) {
    case G_SNAKE: gNext = now + snPace; snStep();   break;
    case G_BRICK: gNext = now + 28;     brStepGame(); break;
    case G_CAR:   gNext = now + 28;     carStep();  break;
    case G_CATCH: gNext = now + 28;     ctStep();   break;
    case G_PONG:  gNext = now + 26;     pgStep();   break;
    default:      gNext = now + 26;     rlStep();   break;
  }

  if (gState == GS_OVER && gScore > gBest[which]) {
    gBest[which] = gScore;
    prefs.putInt(bestKey(which), gScore);
  }
}

// ---------------- the icons ----------------
static void snakeIcon(int x, int y) {
  const int8_t P[8][2] = { {2,4},{6,4},{10,4},{14,4},{14,8},{14,12},{18,12},{22,12} };
  for (int i = 0; i < 8; i++) oled.fillRect(x + P[i][0], y + P[i][1], 3, 3, SSD1306_WHITE);
  oled.drawRect(x + 26, y + 12, 3, 3, SSD1306_WHITE);
}
static void brickIcon(int x, int y) {
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 4; c++)
      oled.fillRect(x + 2 + c * 8, y + 2 + r * 5, 7, 3, SSD1306_WHITE);
  oled.fillRect(x + 9, y + 22, 12, 2, SSD1306_WHITE);
  oled.fillRect(x + 20, y + 18, 2, 2, SSD1306_WHITE);
}
static void carIcon(int x, int y) {
  // seen from above: wheels either side, a roof, and a windscreen
  int cx = x + 15, cy = y + 12;
  oled.fillRect(cx - 8, cy - 8, 3, 5, SSD1306_WHITE);
  oled.fillRect(cx + 5, cy - 8, 3, 5, SSD1306_WHITE);
  oled.fillRect(cx - 8, cy + 3, 3, 5, SSD1306_WHITE);
  oled.fillRect(cx + 5, cy + 3, 3, 5, SSD1306_WHITE);
  oled.drawRoundRect(cx - 6, cy - 11, 13, 23, 4, SSD1306_WHITE);
  oled.fillRect(cx - 3, cy - 4, 7, 8, SSD1306_WHITE);
  oled.drawFastHLine(cx - 4, cy - 6, 9, SSD1306_WHITE);
  oled.drawFastHLine(cx - 4, cy + 6, 9, SSD1306_WHITE);
}
static void catchIcon(int x, int y) {
  oled.drawFastHLine(x + 8, y + 21, 14, SSD1306_WHITE);
  oled.drawFastVLine(x + 8, y + 16, 6, SSD1306_WHITE);
  oled.drawFastVLine(x + 21, y + 16, 6, SSD1306_WHITE);
  oled.fillRect(x + 13, y + 3, 3, 3, SSD1306_WHITE);
  oled.fillRect(x + 19, y + 10, 3, 3, SSD1306_WHITE);
  oled.drawLine(x + 5, y + 6, x + 9, y + 10, SSD1306_WHITE);
  oled.drawLine(x + 9, y + 6, x + 5, y + 10, SSD1306_WHITE);
}
static void pongIcon(int x, int y) {
  oled.fillRect(x + 6, y + 3, 12, 2, SSD1306_WHITE);
  oled.fillRect(x + 12, y + 20, 12, 2, SSD1306_WHITE);
  for (int yy = y + 9; yy < y + 17; yy += 4) oled.drawFastHLine(x + 4, yy, 3, SSD1306_WHITE);
  oled.fillRect(x + 16, y + 11, 3, 3, SSD1306_WHITE);
}
static void rollIcon(int x, int y) {
  oled.drawCircle(x + 7, y + 6, 4, SSD1306_WHITE);
  oled.drawCircle(x + 7, y + 6, 2, SSD1306_WHITE);
  oled.fillCircle(x + 20, y + 9, 3, SSD1306_WHITE);
  oled.drawRect(x + 17, y + 17, 8, 8, SSD1306_WHITE);
  oled.fillRect(x + 19, y + 19, 4, 4, SSD1306_WHITE);
}
static void gameIcon(int g, int x, int y) {
  switch (g) {
    case G_SNAKE: snakeIcon(x, y); break;
    case G_BRICK: brickIcon(x, y); break;
    case G_CAR:   carIcon(x, y);   break;
    case G_CATCH: catchIcon(x, y); break;
    case G_PONG:  pongIcon(x, y);  break;
    default:      rollIcon(x, y);  break;
  }
}

// ---------------- the screens ----------------
static void padIcon(int cx, int cy) {
  oled.drawRoundRect(cx - 15, cy - 8, 30, 16, 5, SSD1306_WHITE);
  oled.drawFastHLine(cx - 11, cy, 7, SSD1306_WHITE);
  oled.drawFastVLine(cx - 8, cy - 3, 7, SSD1306_WHITE);
  oled.fillCircle(cx + 7, cy - 2, 2, SSD1306_WHITE);
  oled.fillCircle(cx + 11, cy + 3, 2, SSD1306_WHITE);
}

// Two to a page, and the page turns itself as the pick walks past the
// end of it. The dots along the bottom say how many pages there are.
static void drawGameList() {
  oled.clearDisplay();
  char r[10];
  snprintf(r, sizeof(r), "%d/%d", itemIdx + 1, G_COUNT);
  titleBar("GAMES", r);
  int page = itemIdx / G_PER_PAGE;
  const int TX[2] = { 10, 76 }, TY = 14, TW = 42, TH = 30;
  for (int k = 0; k < G_PER_PAGE; k++) {
    int g = page * G_PER_PAGE + k;
    if (g >= G_COUNT) break;
    if (g == itemIdx) oled.drawRoundRect(TX[k] - 2, TY - 2, TW, TH, 4, SSD1306_WHITE);
    gameIcon(g, TX[k] + 5, TY + 3);
  }
  char l[26];
  snprintf(l, sizeof(l), "%s   best %d", G_NAME[itemIdx], gBest[itemIdx]);
  ctr(l, 48, 1);
  int dx = SCRW / 2 - (G_PAGES * 8) / 2 + 4;
  for (int p = 0; p < G_PAGES; p++) {
    if (p == page) oled.fillCircle(dx + p * 8, 60, 2, SSD1306_WHITE);
    else           oled.drawCircle(dx + p * 8, 60, 1, SSD1306_WHITE);
  }
  oled.display();
}

static void drawGamePlay() {
  int which = itemIdx;
  oled.clearDisplay();

  if (gState <= GS_CAL_AWAY) {
    titleBar("HOLD ON", "");
    if (gState == GS_CAL_STILL) {
      ctr("Hold it still", 24, 1);
      int n = ((millis() - gStamp) / 220) % 4;
      for (int i = 0; i < 3; i++)
        oled.fillCircle(52 + i * 12, 44, i < n ? 3 : 1, SSD1306_WHITE);
    } else if (gState == GS_CAL_RIGHT) {
      ctr("Now tilt it right", 22, 1);
      int a = (millis() / 300) % 3;
      for (int i = 0; i <= a; i++) {
        int x = 52 + i * 8;
        oled.drawLine(x, 44, x + 5, 48, SSD1306_WHITE);
        oled.drawLine(x, 52, x + 5, 48, SSD1306_WHITE);
      }
    } else {
      ctr("Now tilt it away", 22, 1);
      int a = (millis() / 300) % 3;
      for (int i = 0; i <= a; i++) {
        int y = 52 - i * 7;
        oled.drawLine(60, y, 64, y - 5, SSD1306_WHITE);
        oled.drawLine(68, y, 64, y - 5, SSD1306_WHITE);
      }
    }
    oled.display();
    return;
  }

  char t[14], r[14];
  switch (which) {
    case G_BRICK: snprintf(t, sizeof(t), "BRICK L%d", brLevel); break;
    case G_ROLL:  snprintf(t, sizeof(t), "ROLL L%d", rlLevel);  break;
    case G_CAR:   snprintf(t, sizeof(t), "CAR");                break;
    case G_CATCH: snprintf(t, sizeof(t), "CATCH");              break;
    case G_PONG:  snprintf(t, sizeof(t), "PONG");               break;
    default:      snprintf(t, sizeof(t), "SNAKE");              break;
  }
  if (which == G_PONG) snprintf(r, sizeof(r), "%d-%d", pgMe, pgThem);
  else                 snprintf(r, sizeof(r), "%d", gScore);

  if (gState == GS_READY) {
    titleBar(t, "");
    ctr(gameHint(which), 22, 1);
    long left = 1800 - (long)(millis() - gStamp);
    char c[4];
    snprintf(c, sizeof(c), "%ld", left / 600 + 1);
    ctr(c, 36, 2);
    oled.display();
    return;
  }

  titleBar(t, r);
  switch (which) {
    case G_SNAKE: snDraw();  break;
    case G_BRICK: brDraw();  break;
    case G_CAR:   carDraw(); break;
    case G_CATCH: ctDraw();  break;
    case G_PONG:  pgDraw();  break;
    default:      rlDraw();  break;
  }
  if (which == G_BRICK && brStuck && gState == GS_PLAY) ctr("Knock to launch", 44, 1);

  if (gState == GS_PAUSE) {
    oled.fillRect(10, 22, 108, 26, SSD1306_BLACK);
    oled.drawRect(10, 22, 108, 26, SSD1306_WHITE);
    ctr("PAUSED", 27, 1);
    ctr("2 go on  3 leave", 38, 1);
  }
  if (gState == GS_OVER) {
    oled.fillRect(4, 13, 120, 46, SSD1306_BLACK);
    oled.drawRect(4, 13, 120, 46, SSD1306_WHITE);
    ctr(which == G_PONG && pgMe >= PG_WIN ? "YOU WIN" : "GAME OVER", 17, 1);
    char l[20];
    snprintf(l, sizeof(l), "Score %d", gScore);       ctr(l, 28, 1);
    snprintf(l, sizeof(l), "Best %d", gBest[which]);  ctr(l, 38, 1);
    ctr("2 again  3 leave", 49, 1);
  }
  oled.display();
}

static void drawGames() {
  if (depth == 0) {
    oled.clearDisplay();
    bar("GAMES");
    padIcon(SCRW / 2, 30);
    ctr("Two knocks to open", 50, 1);
    oled.display();
    return;
  }
  if (depth == 1) { drawGameList(); return; }
  drawGamePlay();
}

// ================================================================
//  LEARNING THE LEANS
// ================================================================
//  The same mapping the games use, so it is only ever asked for once.
//  What changes here is the resting position, which moves every time the
//  thing is picked up and put down, so that part is measured afresh.
static void navCalBegin(bool teach) {
  navCal = NC_HOLD;
  navCalStamp = millis();
  calAcc[0] = calAcc[1] = calAcc[2] = 0; calN = 0;
  navCalTeach = teach;
  if (teach) { mapAxX = mapAxY = -1; }      // asked for, so ask for all four
}
static void navCalService() {
  readSensors();
  float v[3] = { ax, ay, az };

  switch (navCal) {
    case NC_HOLD:
      for (int i = 0; i < 3; i++) calAcc[i] += v[i];
      calN++;
      if (millis() - navCalStamp > 1300 && calN > 6) {
        for (int i = 0; i < 3; i++) restV[i] = calAcc[i] / calN;
        gravAx = 0;
        for (int i = 1; i < 3; i++) if (fabsf(restV[i]) > fabsf(restV[gravAx])) gravAx = i;
        if (!navCalTeach && tiltTaught()) {
          navCal = NC_OFF;                  // a boot check: keep the taught axes
          navLatch = true;
          steadySince = 0;
        } else {
          mapAxX = mapAxY = -1;
          navCal = NC_UP;
        }
        navCalStamp = millis();
      }
      break;

    case NC_UP: {
      int best = -1; float bd = 0.30f;
      for (int i = 0; i < 3; i++) {
        if (i == gravAx) continue;
        if (fabsf(v[i] - restV[i]) > bd) { bd = fabsf(v[i] - restV[i]); best = i; }
      }
      if (best >= 0) {
        mapAxY = best;
        mapSgnY = (v[best] - restV[best]) > 0 ? 1 : -1;
        navCal = NC_DOWN; navCalStamp = millis();
      }
      break;
    }
    case NC_DOWN: {
      if (millis() - navCalStamp < 600) break;
      float tx, ty; tiltRead(tx, ty);
      if (ty < -NAV_TILT_ON) { navCal = NC_LEFT; navCalStamp = millis(); }
      break;
    }
    case NC_LEFT: {
      if (millis() - navCalStamp < 600) break;
      for (int i = 0; i < 3; i++) {
        if (i == gravAx || i == mapAxY) continue;
        float d = v[i] - restV[i];
        if (fabsf(d) > 0.30f) {
          mapAxX = i;
          mapSgnX = d > 0 ? -1 : 1;          // leaning left reads negative
          navCal = NC_RIGHT; navCalStamp = millis();
        }
      }
      break;
    }
    case NC_RIGHT: {
      if (millis() - navCalStamp < 600) break;
      float tx, ty; tiltRead(tx, ty);
      if (tx > NAV_TILT_ON) {
        saveTiltMap();
        navCal = NC_INFO; navCalStamp = millis();
      }
      break;
    }
    case NC_INFO:
      if (millis() - navCalStamp > 4200) {
        navCal = NC_OFF;
        navLatch = true;                     // do not act on the lean you finished with
        upSince = 0; upConsumed = false;
      }
      break;
  }
}
static void drawNavCal() {
  oled.clearDisplay();
  if (navCal == NC_INFO) {
    titleBarC("THIS IS THE WAY");
    at(6, 15, "Down");  at(52, 15, "next");
    at(6, 26, "Up");    at(52, 26, "back one");
    at(6, 37, "Left");  at(52, 37, "go in");
    at(6, 48, "Right"); at(52, 48, "come out");
    oled.drawFastVLine(46, 14, 42, SSD1306_WHITE);
    oled.display();
    return;
  }
  titleBarC(navCal == NC_HOLD ? "HOLD IT FIRMLY" : "SHOW ME HOW YOU LEAN");
  const char* ask = "";
  switch (navCal) {
    case NC_HOLD:  ask = "Keep it steady"; break;
    case NC_UP:    ask = "Tilt the top away"; break;
    case NC_DOWN:  ask = "Now tilt it back"; break;
    case NC_LEFT:  ask = "Now lean it left"; break;
    default:       ask = "And now right"; break;
  }
  ctr(ask, 18, 1);
  if (navCal == NC_HOLD) {
    int n = ((millis() - navCalStamp) / 300) % 4;
    for (int i = 0; i < 3; i++) oled.fillCircle(52 + i * 12, 40, i < n ? 3 : 1, SSD1306_WHITE);
  } else {
    int a = (millis() / 300) % 3;
    for (int i = 0; i <= a; i++) {
      if (navCal == NC_UP)        { oled.drawLine(58, 46 - i * 6, 64, 40 - i * 6, SSD1306_WHITE); oled.drawLine(70, 46 - i * 6, 64, 40 - i * 6, SSD1306_WHITE); }
      else if (navCal == NC_DOWN) { oled.drawLine(58, 34 + i * 6, 64, 40 + i * 6, SSD1306_WHITE); oled.drawLine(70, 34 + i * 6, 64, 40 + i * 6, SSD1306_WHITE); }
      else if (navCal == NC_LEFT) { oled.drawLine(70 - i * 8, 34, 64 - i * 8, 40, SSD1306_WHITE); oled.drawLine(70 - i * 8, 46, 64 - i * 8, 40, SSD1306_WHITE); }
      else                        { oled.drawLine(58 + i * 8, 34, 64 + i * 8, 40, SSD1306_WHITE); oled.drawLine(58 + i * 8, 46, 64 + i * 8, 40, SSD1306_WHITE); }
    }
  }
  const bool done[4] = { navCal > NC_UP, navCal > NC_DOWN, navCal > NC_LEFT, navCal > NC_RIGHT };
  for (int i = 0; i < 4; i++) {
    int x = 40 + i * 13;
    if (done[i]) oled.fillCircle(x, 57, 3, SSD1306_WHITE);
    else         oled.drawCircle(x, 57, 3, SSD1306_WHITE);
  }
  oled.display();
}
// ================================================================
//  NETWORK
// ================================================================
static bool httpGetTo(const String& url, bool tls, String& out, int ms) {
  if (!online()) return false;
  HTTPClient h;
  h.setConnectTimeout(ms); h.setTimeout(ms);
  h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  h.setUserAgent("rafiq");
  bool ok = false;
  if (tls) { WiFiClientSecure c; c.setInsecure();
             if (h.begin(c, url) && h.GET() == 200) { out = h.getString(); ok = true; } }
  else     { WiFiClient c;
             if (h.begin(c, url) && h.GET() == 200) { out = h.getString(); ok = true; } }
  h.end();
  return ok;
}

// ================================================================
//  THE CLOCK
// ================================================================
// Two ways in. SNTP first, because it is precise. When UDP port 123
// is blocked, which many home routers and captive networks do quietly,
// SNTP simply never answers and the old code sat there waiting. So the
// second way is an ordinary web request: every HTTP server stamps its
// reply with a Date header, and that is accurate to the second.
static bool parseHttpDate(const String& d, struct tm& t) {
  static const char* MON = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[8] = { 0 };
  int dd = 0, yy = 0, hh = 0, mi = 0, ss = 0;
  int c = d.indexOf(' ');                       // step over "Wed,"
  if (c < 0) return false;
  if (sscanf(d.c_str() + c + 1, "%d %7s %d %d:%d:%d", &dd, mon, &yy, &hh, &mi, &ss) != 6) return false;
  const char* p = strstr(MON, mon);
  if (!p || yy < 2024 || dd < 1 || dd > 31 || hh > 23) return false;
  memset(&t, 0, sizeof(t));
  t.tm_mday = dd; t.tm_mon = (int)(p - MON) / 3; t.tm_year = yy - 1900;
  t.tm_hour = hh; t.tm_min = mi; t.tm_sec = ss;
  t.tm_isdst = 0;
  return true;
}

static bool timeFromHttp() {
  if (!online()) return false;
  // Two tiny endpoints that answer fast and always carry a Date header.
  const char* URLS[2] = { "http://www.google.com/generate_204",
                          "http://detectportal.firefox.com/success.txt" };
  for (int i = 0; i < 2; i++) {
    WiFiClient c;
    HTTPClient h;
    h.setConnectTimeout(5000); h.setTimeout(5000);
    h.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    h.setUserAgent("rafiq");
    if (!h.begin(c, URLS[i])) continue;
    const char* want[] = { "Date" };
    h.collectHeaders(want, 1);
    int code = h.GET();
    String d = h.header("Date");
    h.end();
    if (code <= 0 || !d.length()) continue;

    struct tm t;
    if (!parseHttpDate(d, t)) continue;

    // The header is UTC, so turn it into an epoch under UTC rules and
    // only then put our own zone back on.
    setenv("TZ", "UTC0", 1); tzset();
    time_t e = mktime(&t);
    setenv("TZ", cfgTz.c_str(), 1); tzset();
    if (e < 1735689600L) continue;                      // sanity: after 2025
    struct timeval tv = { .tv_sec = e, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    clockSrc = "http date";
    Serial.println("clock set from http date");
    return true;
  }
  return false;
}

// Called at boot and then from the loop until it lands. Re-arming SNTP
// on each attempt matters: a single failed round leaves it idle.
static bool trySyncTime(int sntpWaitMs) {
  if (!online()) return false;
  configTzTime(cfgTz.c_str(), "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  struct tm t;
  unsigned long t0 = millis();
  while (millis() - t0 < (unsigned long)sntpWaitMs) {
    if (getLocalTime(&t, 120) && t.tm_year > 120) { timeOk = true; clockSrc = "ntp"; Serial.println("clock set from ntp"); return true; }
    web.handleClient();
    delay(30);
  }
  if (timeFromHttp() && getLocalTime(&t, 200) && t.tm_year > 120) { timeOk = true; return true; }
  return false;
}

// Open-Meteo lists every field twice, once in "current_units" with the
// unit as a string. Scope the search or you read "C" and get zero.
static float curNum(const String& b, const char* k, float def) {
  int c = b.indexOf("\"current\":{");
  if (c < 0) return def;
  int i = b.indexOf(String("\"") + k + "\":", c);
  return i < 0 ? def : b.substring(i + strlen(k) + 3).toFloat();
}

static bool locate() {
  if (!isnan(locLat) && locLat != 0) return true;
  String b;
  if (!httpGetTo("http://ip-api.com/json/?fields=status,city,lat,lon", false, b, 6000)) return false;
  int i = b.indexOf("\"lat\":"); if (i >= 0) locLat = b.substring(i + 6).toFloat();
  i = b.indexOf("\"lon\":");     if (i >= 0) locLon = b.substring(i + 6).toFloat();
  i = b.indexOf("\"city\":\"");
  if (i >= 0) { int e = b.indexOf('"', i + 8); wCity = b.substring(i + 8, e); }
  if (wCity.length() > 13) wCity = wCity.substring(0, 13);
  if (!isnan(locLat) && locLat != 0) {
    prefs.putFloat("lat", locLat); prefs.putFloat("lon", locLon);
    prefs.putString("city", wCity);
    return true;
  }
  return false;
}

static void fetchWeather() {
  if (!locate()) return;
  String b;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(locLat, 3) +
               "&longitude=" + String(locLon, 3) +
               "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m";
  if (!httpGetTo(url, true, b, 8000)) return;
  wTemp = curNum(b, "temperature_2m", NAN);
  wHum  = curNum(b, "relative_humidity_2m", NAN);
  wWind = curNum(b, "wind_speed_10m", NAN);
  wCode = (int)curNum(b, "weather_code", -1);
  wxOk  = !isnan(wTemp);
}

static int parseHHMM(const char* s) {
  int h = 0, m = 0;
  if (sscanf(s, "%d:%d", &h, &m) != 2) return -1;
  if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}
// Kept in flash once fetched, so the times are still there with no
// network. They drift by a minute or two across a week, which is fine
// for knowing what is next.
static void savePrayer() {
  String s;
  for (int i = 0; i < 5; i++) { s += String(prayerMin[i]); if (i < 4) s += ","; }
  prefs.putString("pray", s);
  prefs.putInt("prayd", prayerDay);
  prefs.putUInt("prayb", prayerBoot);
}
static void loadPrayer() {
  String s = prefs.getString("pray", "");
  if (!s.length()) return;
  int i = 0, n = 0, tmp[5];
  while (n < 5 && i <= (int)s.length()) {
    int c = s.indexOf(',', i); if (c < 0) c = s.length();
    tmp[n++] = s.substring(i, c).toInt();
    i = c + 1;
  }
  if (n != 5) return;
  for (int k = 0; k < 5; k++) { if (tmp[k] < 0 || tmp[k] > 1439) return; prayerMin[k] = tmp[k]; }
  prayerOk = true;
  prayerDay = prefs.getInt("prayd", -1);
  prayerBoot = prefs.getUInt("prayb", 0);
}
static void saveAdj() {
  String o;
  for (int i = 0; i < 5; i++) { o += String(prayerAdj[i]); if (i < 4) o += ","; }
  prefs.putString("adj", o);
}
static void loadAdj() {
  String in = prefs.getString("adj", "");
  if (!in.length()) return;
  int i = 0, n = 0;
  while (n < 5 && i <= (int)in.length()) {
    int c = in.indexOf(',', i); if (c < 0) c = in.length();
    prayerAdj[n++] = constrain(in.substring(i, c).toInt(), -90, 90);
    i = c + 1;
  }
}

static void fetchPrayer() {
  struct tm t;
  if (!timeOk || !getLocalTime(&t, 0)) return;
  if (!locate()) return;

  char d[16];
  strftime(d, sizeof(d), "%d-%m-%Y", &t);
  String url = "https://api.aladhan.com/v1/timings/" + String(d) +
               "?latitude=" + String(locLat, 4) + "&longitude=" + String(locLon, 4) +
               "&method=1&school=0";
  String b;
  if (!httpGetTo(url, true, b, 9000)) return;

  JsonDocument filter;
  filter["data"]["timings"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, b, DeserializationOption::Filter(filter))) return;
  JsonObject tm_ = doc["data"]["timings"];
  if (tm_.isNull()) return;

  int tmp[5];
  for (int i = 0; i < 5; i++) {
    tmp[i] = parseHHMM(tm_[PRAYERS[i]] | "");
    if (tmp[i] < 0) return;
  }
  for (int i = 0; i < 5; i++) prayerMin[i] = tmp[i];
  prayerOk = true;
  prayerDay = t.tm_yday;
  prayerBoot = cBoot;
  prayerWanted = false;
  savePrayer();
  Serial.println("prayer times updated");
}

// ================================================================
//  THE READER
//    Wrapping happens once, when something is opened. After that,
//    turning a page is only an index change.
// ================================================================
static void wrapInto(const String& text) {
  rdLines = 0;
  rdPage = 0;
  int i = 0, n = text.length();
  while (i < n && rdLines < RD_LINES) {
    while (i < n && (text.charAt(i) == ' ' || text.charAt(i) == '\n')) i++;
    if (i >= n) break;
    int take = min(RD_COLS, n - i);
    int nl = text.indexOf('\n', i);
    if (nl >= 0 && nl < i + take) take = nl - i;
    else if (take == RD_COLS) {
      int sp = text.lastIndexOf(' ', i + take);
      if (sp > i + 4) take = sp - i;
    }
    rdLine[rdLines++] = text.substring(i, i + take);
    i += take;
  }
  for (int k = rdLines; k < RD_LINES; k++) rdLine[k] = "";
  rdTurn = millis() + AUTO_TURN_MS;
}

static void openSurah(int i) {
  const Surah& s = SURAH[i];
  String body = String(s.meaning) + "\n" +
                (s.makki ? "Revealed in Makkah" : "Revealed in Madinah") + "\n" +
                String(s.ayat) + " verses\n\n" + s.theme;
  rdTitle = s.name;                      // the number is already in the list
  wrapInto(body);
}
static void openDhikr(const Dhikr* set, int i) {
  rdTitle = set[i].title;
  wrapInto(set[i].body);
}

// ================================================================
//  THE SHELF
//    Short reads live as files, not as settings entries. Fifteen of
//    them would not come close to fitting in the settings area, and
//    only the one being read is ever held in memory.
// ================================================================
static String readPath(int i) { return "/r" + String(i) + ".txt"; }

static String titleOf(int i) {
  File f = LittleFS.open(readPath(i), "r");
  if (!f) return String("untitled");
  char b[72];
  size_t n = f.readBytes(b, 64);
  b[n] = 0;
  f.close();
  String t(b);
  t.replace("\n", " "); t.replace("\r", " ");
  t.trim();
  if (t.length() > RD_TITLE_MAX) {
    int sp = t.lastIndexOf(' ', RD_TITLE_MAX);
    t = t.substring(0, sp > 6 ? sp : RD_TITLE_MAX);
  }
  return t.length() ? t : String("untitled");
}

static void loadShelf() {
  readCount = 0;
  if (!fsOk) return;
  for (int i = 0; i < READS_MAX; i++) {
    if (!LittleFS.exists(readPath(i))) break;
    readTitle[readCount++] = titleOf(i);
  }
  if (readCount) storyState = "Ready";
  Serial.printf("shelf: %d reads\n", readCount);
}

static void openRead(int i) {
  if (!fsOk || i < 0 || i >= readCount) return;
  File f = LittleFS.open(readPath(i), "r");
  if (!f) return;
  String text = f.readString();
  f.close();
  readOpen = i;
  rdTitle = readTitle[i];
  wrapInto(text);
}

// Make space by letting the oldest go. The partition is 128 kB, so the
// shelf settles at however many actually fit rather than a guessed count.
static bool ensureRoom(size_t need) {
  for (int guard = 0; guard <= READS_MAX; guard++) {
    size_t freeB = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (freeB > need + 8192) return true;
    if (readCount <= 1) return false;
    LittleFS.remove(readPath(readCount - 1));
    readCount--;
  }
  return false;
}

// Newest first. Files shuffle down by name, which in LittleFS is a
// rename and costs nothing.
static void addRead(const String& textIn) {
  if (!fsOk) { storyState = "No storage"; return; }
  String text = textIn.substring(0, STORY_MAX_CHARS);
  if (!ensureRoom(text.length())) { storyState = "Shelf is full"; return; }

  if (readCount >= READS_MAX) { LittleFS.remove(readPath(READS_MAX - 1)); readCount = READS_MAX - 1; }
  for (int i = readCount; i > 0; i--) {
    if (LittleFS.exists(readPath(i))) LittleFS.remove(readPath(i));
    LittleFS.rename(readPath(i - 1), readPath(i));
    readTitle[i] = readTitle[i - 1];
  }
  File f = LittleFS.open(readPath(0), "w");
  if (!f) { storyState = "Cannot write"; loadShelf(); return; }
  f.print(text);
  f.close();
  readCount = min(readCount + 1, READS_MAX);
  readTitle[0] = titleOf(0);
  storyState = "Ready";
  Serial.printf("stored a read, shelf now %d\n", readCount);
}

// ================================================================
//  WRITING A NEW ONE  (OpenAI, gpt-4o-mini)
// ================================================================
static const char* STORY_PROMPT =
  "Write a warm, romantic short story of about 900 words in simple English. "
  "Give the characters Muslim names such as Ayaan, Zaynab, Bilal, Maryam, Idris, "
  "Safiya, Yusuf, Aisha, Hamza or Khadija. Keep it tender and respectful, the kind "
  "of story that ends happily. Set it somewhere ordinary and real. Begin with a "
  "short line of five or six words that works as a title, then a blank line, then "
  "the story. Plain prose only: no headings, no markdown, no lists.";

static bool fetchStory(bool showProgress) {
  if (!cfgKey.length()) { storyState = "No API key"; return false; }
  if (!online())        { storyState = "No network"; return false; }

  storyBusy = true;
  if (showProgress) drawReads();

  WiFiClientSecure c; c.setInsecure();
  HTTPClient h;
  h.setConnectTimeout(12000); h.setTimeout(30000);
  if (!h.begin(c, "https://api.openai.com/v1/chat/completions")) {
    storyBusy = false; storyState = "Cannot reach OpenAI"; return false;
  }
  h.addHeader("Content-Type", "application/json");
  h.addHeader("Authorization", "Bearer " + cfgKey);

  JsonDocument req;
  req["model"] = "gpt-4o-mini";
  req["max_tokens"] = 1800;
  req["temperature"] = 1.0;
  JsonObject m = req["messages"].add<JsonObject>();
  m["role"] = "user";
  m["content"] = STORY_PROMPT;
  String body;
  serializeJson(req, body);

  int code = h.POST(body);
  if (code != 200) {
    String err = h.getString();
    h.end();
    storyBusy = false;
    storyState = (code == 401) ? "Key rejected"
               : (code == 429) ? "rate limited"
               : ("openai " + String(code));
    Serial.println("story failed " + String(code) + " " + err.substring(0, 200));
    return false;
  }

  // Read the body through getString(), which unpicks chunked transfer
  // encoding for us. Handing getStream() straight to ArduinoJson feeds it
  // the raw chunk length markers, and it fails every time.
  String reply = h.getString();
  h.end();

  JsonDocument filter;
  filter["choices"][0]["message"]["content"] = true;
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, reply, DeserializationOption::Filter(filter));
  if (e) {
    storyBusy = false;
    storyState = String("Parse: ") + e.c_str();
    Serial.println("story parse failed: " + String(e.c_str()));
    return false;
  }

  String text = doc["choices"][0]["message"]["content"] | "";
  if (text.length() < 200) { storyBusy = false; storyState = "Reply was too short"; return false; }

  addRead(text);
  storyBusy = false;
  return true;
}

// Four knocks on the shelf: write one now and show it, then keep
// filling the rest quietly while you read.
static void refillShelf() {
  if (!cfgKey.length()) { storyState = "No API key"; return; }
  if (!online())        { storyState = "No network"; return; }
  if (fetchStory(true)) {
    openRead(0);
    itemIdx = 0;
    refillWant = READS_MAX - readCount;
    nextRefill = millis() + 4000;
  }
}

static void setStory(const String& text) {
  addRead(text);
  itemIdx = 0;
}

// ================================================================
//  WORK SESSION
// ================================================================
static void saveTasks() {
  String out;
  for (int i = 0; i < taskCount; i++) {
    String n = tasks[i].name;
    n.replace("|", " "); n.replace(";", " ");
    out += n + "|" + String(tasks[i].mins);
    if (i < taskCount - 1) out += ";";
  }
  prefs.putString("plan", out);
}
static void loadTasks() {
  taskCount = 0;
  String in = prefs.getString("plan", "");
  int i = 0;
  while (i < (int)in.length() && taskCount < TASK_MAX) {
    int semi = in.indexOf(';', i); if (semi < 0) semi = in.length();
    String part = in.substring(i, semi);
    int barp = part.indexOf('|');
    if (barp > 0) {
      tasks[taskCount].name = part.substring(0, barp);
      tasks[taskCount].mins = constrain(part.substring(barp + 1).toInt(), 1, 240);
      taskCount++;
    }
    i = semi + 1;
  }
}

static void flash(const char* word, unsigned long ms) {
  flashWord = word;
  flashUntil = millis() + ms;
}
static void startTask(int i) {
  if (i < 0 || i >= taskCount) {
    taskIdx = -1;
    workedMin = 0; breakDue = false;
    flash("ALL DONE", 3000);
    return;
  }
  taskIdx = i;
  taskEnd = millis() + (unsigned long)tasks[i].mins * 60000UL;
  if (isBreak(tasks[i].name)) { workedMin = 0; breakDue = false; }
}
static void startSession() {
  if (!taskCount) return;
  workedMin = 0; breakDue = false;
  screen = S_FOCUS; depth = 0;
  startTask(0);
}
static void stopSession() {
  taskIdx = -1;
  flashUntil = 0;
  workedMin = 0; breakDue = false;
}
static void finishTask() {
  const Task& t = tasks[taskIdx];
  bool wasBreak = isBreak(t.name);
  if (!wasBreak) workedMin += t.mins;
  if (!wasBreak && workedMin >= BREAK_AFTER_MIN) {
    breakDue = true;
    flash("TAKE A BREAK", 5000);
    workedMin = 0;
  } else {
    breakDue = false;
    flash(wasBreak ? "BREAK OVER" : "COMPLETED", 2600);
  }
  startTask(taskIdx + 1);
}
static void serviceSession() {
  if (!sessionRunning()) return;
  if (millis() < flashUntil) return;
  if ((long)(millis() - taskEnd) >= 0) finishTask();
}

// ================================================================
//  OTA
// ================================================================
// Two things used to go wrong here.
//
// Update.writeStream() is one blocking call that returns only when the
// whole image has been pulled, so the bar could not move off zero no
// matter how well the download was going. It now reads the body in
// chunks and repaints as it goes, which is both honest and useful.
//
// And redirects: the asset sits behind a hop to another host, and
// reusing one TLS client across two hosts is what produced a silent
// "download failed" after a long pause. Each hop now gets its own
// client, and a stall is detected rather than waited out.
static long verNum(const String& v) {
  int a = 0, b = 0, c = 0;
  const char* p = v.c_str();
  while (*p && !isdigit((unsigned char)*p)) p++;
  sscanf(p, "%d.%d.%d", &a, &b, &c);
  return (long)a * 1000000L + b * 1000L + c;
}

static void otaFail(const char* why, const char* detail = "") {
  otaStatus = why; otaStatus2 = detail; otaPct = -1;
  drawOta(); delay(detail[0] ? 4200 : 2400);
  otaStatus2 = "";
  Update.abort();
}

// GitHub returns compact JSON to the device, because the device sends no
// Accept header, and pretty printed JSON to anything that sends one. The
// whole update path used to rest on that staying true. It reads either
// way now, which costs four lines and removes the assumption.
static String jsonStr(const String& b, const char* key, int from = 0) {
  int k = b.indexOf(key, from);
  if (k < 0) return "";
  int i = k + (int)strlen(key);
  while (i < (int)b.length() &&
         (b[i] == ' ' || b[i] == ':' || b[i] == '\t' || b[i] == '\n' || b[i] == '\r')) i++;
  if (i >= (int)b.length() || b[i] != '"') return "";
  i++;
  int e = b.indexOf('"', i);
  return e < 0 ? String("") : b.substring(i, e);
}

// Ask GitHub what the newest release is. Only fills in the answer; it
// does not install anything.
static bool otaFetchLatest() {
  if (!online()) { upMsg = "No network"; return false; }
  String b;
  if (!httpGetTo("https://api.github.com/repos/" OTA_REPO "/releases/latest", true, b, 12000)) {
    upMsg = "GitHub unreachable"; return false;
  }
  upTag = jsonStr(b, "\"tag_name\"");
  int a = b.indexOf(OTA_ASSET);
  upUrl = a < 0 ? "" : jsonStr(b, "\"browser_download_url\"", a);
  b = String();
  if (!upTag.length() || !upUrl.length()) { upMsg = "No release found"; return false; }
  return true;
}

// The last few releases, so an older one can be put back deliberately.
static bool otaFetchList() {
  if (!online()) { upMsg = "No network"; return false; }
  String b;
  if (!httpGetTo("https://api.github.com/repos/" OTA_REPO
                 "/releases?per_page=8", true, b, 15000)) {
    upMsg = "GitHub unreachable"; return false;
  }
  relCount = 0; relSel = 0;
  int i = 0;
  while (relCount < UP_MAX) {
    int t = b.indexOf("\"tag_name\"", i);
    if (t < 0) break;
    String tag = jsonStr(b, "\"tag_name\"", t);
    int nextT = b.indexOf("\"tag_name\"", t + 10);
    int limit = nextT < 0 ? (int)b.length() : nextT;
    int a = b.indexOf(OTA_ASSET, t);
    String url = "";
    if (a >= 0 && a < limit) {
      int u = b.indexOf("\"browser_download_url\"", a);
      if (u >= 0 && u < limit) url = jsonStr(b, "\"browser_download_url\"", a);
    }
    if (tag.length() && url.length()) {
      relTag[relCount] = tag; relUrl[relCount] = url; relCount++;
    }
    i = t + 10;
  }
  b = String();
  if (!relCount) { upMsg = "No releases found"; return false; }
  return true;
}

// Writes whatever was chosen. No version comparison lives here on
// purpose: putting an older build back is a thing you are allowed to
// want, and the asking has already happened by this point.
static void otaInstall() {
  if (!online() || !upUrl.length()) { otaFail("No network"); return; }
  otaStatus = upTag; otaPct = 0; drawOta();

  String url = upUrl;
  WiFiClientSecure* sec = nullptr;
  HTTPClient*       h   = nullptr;
  int  len = 0;
  bool open = false;

  for (int hop = 0; hop < 5 && !open; hop++) {
    sec = new WiFiClientSecure();
    if (!sec) { otaFail("Out of memory"); return; }
    sec->setInsecure();
    sec->setTimeout(30);
    h = new HTTPClient();
    h->setReuse(false);
    h->setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    h->setConnectTimeout(15000);
    h->setTimeout(30000);
    h->setUserAgent("rafiq");

    if (!h->begin(*sec, url)) { delete h; delete sec; otaFail("Cannot connect"); return; }
    const char* want[] = { "Location" };
    h->collectHeaders(want, 1);

    int code = h->GET();
    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
      String loc = h->header("Location");
      h->end(); delete h; delete sec; h = nullptr; sec = nullptr;
      if (!loc.length()) { otaFail("Bad redirect"); return; }
      url = loc;
      otaStatus = "Connecting"; drawOta();
      continue;
    }
    if (code != 200) {
      h->end(); delete h; delete sec;
      otaFail((String("HTTP ") + code).c_str());
      return;
    }
    len = h->getSize();
    open = true;
  }
  if (!open) { otaFail("Too many redirects"); return; }

  const esp_partition_t* slot = esp_ota_get_next_update_partition(NULL);
  if (len <= 0) { h->end(); delete h; delete sec; otaFail("No content length"); return; }
  if (!slot)    { h->end(); delete h; delete sec; otaFail("No OTA slot", "Needs min SPIFFS"); return; }
  if ((size_t)len > slot->size || !Update.begin((size_t)len)) {
    char d[24];
    snprintf(d, sizeof(d), "%dk into %uk slot", len / 1024, (unsigned)(slot->size / 1024));
    h->end(); delete h; delete sec;
    otaFail("Will not fit", d);
    return;
  }

  WiFiClient* st = h->getStreamPtr();
  static uint8_t buf[2048];
  size_t done = 0;
  unsigned long lastByte = millis();
  bool bad = false;

  otaStatus = "Downloading"; drawOta();
  while (done < (size_t)len) {
    size_t avail = st->available();
    if (avail) {
      int want2 = (int)min(avail, sizeof(buf));
      int got = st->readBytes(buf, want2);
      if (got > 0) {
        if (Update.write(buf, got) != (size_t)got) { bad = true; break; }
        done += got;
        lastByte = millis();
        int p = (int)((done * 100ULL) / (size_t)len);
        if (p != otaPct) { otaPct = p; drawOta(); }
      }
    } else {
      if (!st->connected() && !st->available()) break;
      if (millis() - lastByte > 20000UL) { bad = true; break; }
      delay(2);
    }
  }
  h->end(); delete h; delete sec;

  if (bad || done != (size_t)len) {
    otaFail(bad ? "Download stalled" : "Download cut short");
    return;
  }
  otaStatus = "Installing"; drawOta();
  if (!Update.end(true)) { otaFail("Install failed"); return; }

  otaStatus = "Installed"; otaPct = 100; drawOta();
  delay(1400);
  ESP.restart();
}

// what the page's button still does: newest, straight away
static void runUpdate() {
  otaStatus = "Checking"; otaPct = -1; drawOta();
  if (!otaFetchLatest()) { otaFail(upMsg.c_str()); return; }
  if (upTag == FW_VERSION || upTag == String("v" FW_VERSION)) {
    otaStatus = "Already current"; otaStatus2 = upTag; otaPct = -1;
    drawOta(); delay(2400); otaStatus2 = ""; return;
  }
  otaInstall();
}

// ================================================================
//  SLEEP AND EYES
// ================================================================
static void screenPower(bool on) {
  if (on == screenOn) return;
  screenOn = on;
  oled.ssd1306_command(on ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
}
static void applyBright() {
  oled.ssd1306_command(SSD1306_SETCONTRAST);
  oled.ssd1306_command(cfgBright);
}
static void applyEyes(int i) {
  cfgEyes = (i + STYLE_N) % STYLE_N;
  const EyeStyle& e = STYLES[cfgEyes];
  eyes.setCyclops(e.cyc);
  eyes.setWidth(e.w, e.w); eyes.setHeight(e.h, e.h);
  eyes.setBorderradius(e.r, e.r); eyes.setSpacebetween(e.gap);
  eyes.setMood(e.mood);
}
static void goSleep() {
  if (asleep) return;
  asleep = true;
  applyEyes(cfgEyes);
  eyes.setIdleMode(OFF); eyes.setAutoblinker(OFF);
  eyes.setMood(TIRED); eyes.close();
  for (int i = 0; i < 26; i++) { eyesFrame(); delay(16); }
  screenPower(false);
  nSlept++;
  // The clock used to drop to 80MHz here. That saves almost nothing on a
  // desk and changes the bus timing at exactly the moment the sensors
  // have to stay readable, because noticing a lean means reading them
  // continuously while asleep. Not worth the risk.
  if (!cfgTilt) setCpuFrequencyMhz(80);
}
static void wake(const char* why) {
  lastActive = millis();
  wokeBy = why;
  if (!asleep) return;
  asleep = false;
  setCpuFrequencyMhz(160);
  screenPower(true);
  eyes.setAutoblinker(ON, 7, 5); eyes.setIdleMode(ON, 5, 4);
  applyEyes(cfgEyes); eyes.open();
  for (int i = 0; i < 18; i++) { eyesFrame(); delay(16); }
  screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0;
  navLatch = true;                     // the lean that woke it is not also a command
  upSince = 0; upConsumed = false;
  steadySince = 0;
  Serial.printf("awake (%s)\n", why);
}

// ================================================================
//  THE CALL TO PRAYER
// ================================================================
//  Ten minutes out it says a word, five minutes out it says it again
//  and starts to flash, and on the minute itself it flashes for a
//  minute and then leaves you alone. Each step fires once a day.
static void servicePrayerAlert() {
  struct tm t;
  if (!prayerOk || !timeOk || !getLocalTime(&t, 0)) return;

  if (alertDay != t.tm_yday) {                 // a new day, a clean slate
    alertDay = t.tm_yday;
    for (int i = 0; i < 5; i++) alertDone[i] = 0;
  }

  if (alertPhase != AL_NONE) {                 // let the one running finish
    if ((long)(millis() - alertUntil) >= 0) { alertPhase = AL_NONE; alertWhich = -1; }
    else lastActive = millis();
    return;
  }

  int nowMin = t.tm_hour * 60 + t.tm_min;
  for (int i = 0; i < 5; i++) {
    int p = prayerAt(i);
    if (p < 0) continue;
    int d = p - nowMin;
    if (d < 0) d += 1440;
    uint8_t bit = (d == 10) ? 1 : (d == 5) ? 2 : (d == 0) ? 4 : 0;
    if (!bit || (alertDone[i] & bit)) continue;
    alertDone[i] |= bit;
    alertWhich = i;
    alertPhase = (d == 10) ? AL_TEN : (d == 5) ? AL_FIVE : AL_NOW;
    alertUntil = millis() + (d == 10 ? ALERT_TEN_MS : d == 5 ? ALERT_FIVE_MS : ALERT_NOW_MS);
    wake("prayer");
    Serial.printf("prayer alert: %s in %d min\n", PRAYERS[i], d);
    return;
  }
}
// ================================================================
//  WHO IS ALLOWED TO ASK
// ================================================================
//  Until a Mac is paired the API is open, exactly as it has always
//  been. Pair one and everything has to carry its token, which closes
//  a door that was standing open: anyone on the same network could
//  post to this device before.
//
//  The code is shown on the panel and nowhere else. Someone on your
//  network can ask for one, but they cannot read it without standing
//  in front of the thing, and that is the whole point of it.

static bool authed() {
  if (!cfgLock || !cfgTok.length()) return true;
  String t = web.arg("t");
  if (!t.length()) t = web.header("X-Rafiq-Token");
  return t.length() && t == cfgTok;
}
// Every call that carries the token is also a heartbeat.
static void sawMac() {
  if (!cfgLock) return;
  macSeen = millis();
  if (!macLinked) {
    macLinked = true;
    linkCardJoin = true;
    linkCardUntil = millis() + 1600;
    wake("mac");
  }
}
static bool guard() {
  if (!authed()) { web.send(401, "application/json", "{\"ok\":false,\"err\":\"pair first\"}"); return false; }
  sawMac();
  return true;
}
static void okJson() { web.send(200, "application/json", "{\"ok\":true}"); }

static void newPairCode() {
  // A client that keeps asking must not keep changing the digits under
  // your fingers. While one is still good, that is the one you get.
  if (pairCode >= 0 && millis() < pairUntil) {
    screen = S_SETTINGS; depth = 2; itemIdx = C_PAIR;
    wake("pairing");
    return;
  }
  pairCode = (int)random(0, 1000000);
  pairUntil = millis() + PAIR_WINDOW_MS;
  screen = S_SETTINGS; depth = 2; itemIdx = C_PAIR;
  wake("pairing");
}
// Thirty-two hex characters out of the hardware generator, which is a
// real one on this chip and not the Arduino pseudo random.
static String newToken() {
  String t;
  for (int i = 0; i < 4; i++) { char b[9]; snprintf(b, sizeof(b), "%08x", (unsigned)esp_random()); t += b; }
  return t;
}

// ---------------------------------------------------------------
//  base64, for the screenful of pixels. Small enough to spell out
//  and it saves pulling mbedtls in for one call.
static int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}
static int b64decode(const String& in, uint8_t* out, int cap) {
  int bits = 0, acc = 0, n = 0;
  for (int i = 0; i < (int)in.length(); i++) {
    int v = b64val(in[i]);
    if (v < 0) continue;                       // whitespace, padding, anything else
    acc = (acc << 6) | v; bits += 6;
    if (bits >= 8) { bits -= 8; if (n < cap) out[n++] = (uint8_t)((acc >> bits) & 0xFF); }
  }
  return n;
}

// ---------------------------------------------------------------
//  Focus, driven from the Mac. One task, one length, and the panel
//  spends most of it dark.
static void focusBegin(int mins) {
  if (mins <= 0) { stopSession(); fzPhase = FZ_SHOW; return; }
  taskCount = 1;
  tasks[0].name = "Focus";
  tasks[0].mins = mins;
  workedMin = 0; breakDue = false;
  screen = S_FOCUS; depth = 0;
  startTask(0);
  fzPhase = FZ_SHOW; fzCycle = 0;
  fzNext = millis() + FZ_SHOW_MS;
  wake("focus");
}

// The pointer arrives over UDP, ten or so a second, as two numbers in
// thousandths. Nothing is acknowledged and nothing is retried.
static void serviceCursor() {
  if (!cfgFollow) return;
  int n = cursorUdp.parsePacket();
  while (n > 0) {
    char b[32];
    int got = cursorUdp.read(b, sizeof(b) - 1);
    if (got > 0) {
      b[got] = 0;
      int x = 0, y = 0;
      if (sscanf(b, "%d %d", &x, &y) == 2) {
        curX = constrain(x / 1000.0f, -1.0f, 1.0f);
        curY = constrain(y / 1000.0f, -1.0f, 1.0f);
        curUntil = millis() + CURSOR_HOLD_MS;
      }
    }
    n = cursorUdp.parsePacket();
  }
}

// ================================================================
//  KNOCKS
//    One rule, everywhere on the device:
//
//      in a list     1 next item   2 open it   3 back out   4 reload
//      in a reader   1 next page   2 back      3 carousel
//      on a card     1 next screen 2 go in     3 home
//
//    Reload only means anything on the shelf of short reads and on
//    the zikr counter, where it starts the hundred again.
// ================================================================
static void react(unsigned long ms) { reactUntil = millis() + ms; }

static bool inReader() {
  if (screen == S_READS) return depth == 2;
  if (screen == S_FAITH) return depth == 3;
  return false;
}
static void prevPage() {
  int p = rdPages();
  if (p > 0) rdPage = (rdPage + p - 1) % p;
  rdTurn = millis() + AUTO_TURN_MS;
}
static void nextPage() {
  int p = rdPages();
  if (p > 0) rdPage = (rdPage + 1) % p;
  rdTurn = millis() + AUTO_TURN_MS;
}

static void zikrReset() {
  zikrStep = 0; zikrDone = 0; zikrTotal = 0;
  zikrNext = millis() + ZIKR_PACE_MS;
}
static void zikrTick() {
  if (zikrTotal >= 100) return;
  zikrDone++; zikrTotal++;
  if (zikrDone >= ZIKR[zikrStep].count && zikrStep < ZIKR_N - 1) {
    zikrStep++; zikrDone = 0;
  }
  zikrNext = millis() + (zikrStep == ZIKR_N - 1 ? ZIKR_LONG_MS : ZIKR_PACE_MS);
}

static void startHotspot() {
  if (rescueAP) return;
  WiFi.mode(online() ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(RESCUE_SSID, RESCUE_PASS);
  rescueAP = true;
  Serial.printf("hotspot up: %s at %s\n", RESCUE_SSID, WiFi.softAPIP().toString().c_str());
}

static void knockOne() {
  cTap++;
  // Anything the Mac put on the screen goes away on one knock. It is
  // the Mac's idea of what you want to see, and this is the desk.
  if (relaxOn)     { relaxOn = false;    return; }
  if (canvasUntil) { canvasUntil = 0;    return; }
  if (toastUntil)  { toastUntil = 0; toastText = ""; toastKind = ""; return; }
  if (depth == 0) {
    screen = (screen + 1) % S_COUNT;
    itemIdx = 0; subIdx = 0;
    return;
  }
  if (screen == S_READS) {
    if (depth == 1) { if (readCount) itemIdx = (itemIdx + 1) % readCount; return; }
    nextPage();
    return;
  }
  if (screen == S_FAITH) {
    if (depth == 1) { itemIdx = (itemIdx + 1) % F_COUNT; subIdx = 0; return; }
    if (depth == 2) {
      switch (itemIdx) {
        case F_ZIKR:    zikrTick(); break;
        case F_NAMES:   subIdx = (subIdx + 1) % 99; break;
        case F_QURAN:   subIdx = (subIdx + 1) % 114; break;
        case F_MORNING: subIdx = (subIdx + 1) % MORNING_N; break;
        default:        subIdx = (subIdx + 1) % EVENING_N; break;
      }
      return;
    }
    nextPage();
    return;
  }
  if (screen == S_GAMES) {
    if (depth == 1) { itemIdx = (itemIdx + 1) % G_COUNT; return; }
    if (itemIdx == G_BRICK && gState == GS_PLAY) brStuck = false;   // let it go
    return;
  }
  if (screen == S_SETTINGS) {
    if (depth == 1) { itemIdx = (itemIdx + 1) % C_COUNT; return; }
    switch (itemIdx) {
      case C_BRIGHT: { int i = 0;
                       for (int k = 0; k < BRIGHT_N; k++) if (BRIGHT_OPTS[k] == cfgBright) i = k;
                       cfgBright = BRIGHT_OPTS[(i + 1) % BRIGHT_N];
                       applyBright(); prefs.putInt("bri", cfgBright); break; }
      case C_FACE:   cfgFace = (cfgFace + 1) % FACE_N;
                     prefs.putInt("face", cfgFace); break;
      case C_CONTROL: cfgTilt = !cfgTilt;
                     prefs.putBool("ctrl", cfgTilt);
                     applyFallInt();
                     if (cfgTilt) navCalBegin(true);        // show how, there and then
                     break;
      case C_SLEEP:  cfgSleepIdx = (cfgSleepIdx + 1) % SLEEP_N;
                     prefs.putInt("slpi", cfgSleepIdx); break;
      case C_TURN:   cfgAutoTurn = !cfgAutoTurn;
                     prefs.putBool("turn", cfgAutoTurn); break;
      case C_POPUP:  cfgPopupIdx = (cfgPopupIdx + 1) % POPUP_N;
                     prefs.putInt("popi", cfgPopupIdx); break;
      case C_EYES:   applyEyes(cfgEyes + 1); prefs.putInt("eye", cfgEyes); break;
      default: break;
    }
  }
}

// The other way round the carousel, and back up a list. Leaning gives
// this for nothing; knocking never had it.
static void knockPrev() {
  if (depth == 0) { screen = (screen + S_COUNT - 1) % S_COUNT; itemIdx = 0; subIdx = 0; return; }
  if (screen == S_READS) {
    if (depth == 1) { if (readCount) itemIdx = (itemIdx + readCount - 1) % readCount; return; }
    prevPage(); return;
  }
  if (screen == S_FAITH) {
    if (depth == 1) { itemIdx = (itemIdx + F_COUNT - 1) % F_COUNT; subIdx = 0; return; }
    if (depth == 2) {
      switch (itemIdx) {
        case F_ZIKR:    return;                                     // it paces itself
        case F_NAMES:   subIdx = (subIdx + 98) % 99; return;
        case F_QURAN:   subIdx = (subIdx + 113) % 114; return;
        case F_MORNING: subIdx = (subIdx + MORNING_N - 1) % MORNING_N; return;
        default:        subIdx = (subIdx + EVENING_N - 1) % EVENING_N; return;
      }
    }
    prevPage(); return;
  }
  if (screen == S_GAMES) {
    if (depth == 1) itemIdx = (itemIdx + G_COUNT - 1) % G_COUNT;
    return;
  }
  if (screen == S_SETTINGS) {
    if (depth == 1) { itemIdx = (itemIdx + C_COUNT - 1) % C_COUNT; return; }
    switch (itemIdx) {
      case C_BRIGHT: { int i = 0;
                       for (int k = 0; k < BRIGHT_N; k++) if (BRIGHT_OPTS[k] == cfgBright) i = k;
                       cfgBright = BRIGHT_OPTS[(i + BRIGHT_N - 1) % BRIGHT_N];
                       applyBright(); prefs.putInt("bri", cfgBright); break; }
      case C_FACE:   cfgFace = (cfgFace + FACE_N - 1) % FACE_N;
                     prefs.putInt("face", cfgFace); break;
      case C_SLEEP:  cfgSleepIdx = (cfgSleepIdx + SLEEP_N - 1) % SLEEP_N;
                     prefs.putInt("slpi", cfgSleepIdx); break;
      case C_POPUP:  cfgPopupIdx = (cfgPopupIdx + POPUP_N - 1) % POPUP_N;
                     prefs.putInt("popi", cfgPopupIdx); break;
      case C_EYES:   applyEyes(cfgEyes - 1); prefs.putInt("eye", cfgEyes); break;
      case C_TURN:   cfgAutoTurn = !cfgAutoTurn; prefs.putBool("turn", cfgAutoTurn); break;
      default: break;
    }
  }
}

static void knockTwo() {
  cDouble++;
  // Told to stand up and not able to just yet. Ten minutes and it asks
  // again, which is the difference between a reminder and a nag.
  if (toastUntil && toastKind == "break") {
    toastUntil = 0; toastKind = ""; toastText = "";
    flash("SNOOZED", 1200);
    return;
  }
  if (depth == 0) {
    switch (screen) {
      case S_FAITH:    depth = 1; itemIdx = 0; subIdx = 0; break;
      case S_READS:    if (readCount) { depth = 1; itemIdx = 0; } else refillShelf(); break;
      case S_GAMES:    depth = 1; itemIdx = 0; navCalBegin(false); break;
      case S_SETTINGS: depth = 1; itemIdx = 0; break;
      case S_HOME:     if (!timeOk) swStart = millis(); break;   // restart the stopwatch
      case S_WEATHER:  nextWx = 0; break;
      case S_PRAYER:   nextPrayerTry = 0; break;
      default: break;
    }
    return;
  }
  if (screen == S_READS) {
    if (depth == 1) { openRead(itemIdx); depth = 2; return; }
    depth = 1;                                   // out of the reader, back to the shelf
    return;
  }
  if (screen == S_FAITH) {
    if (depth == 1) {
      subIdx = 0;
      if (itemIdx == F_ZIKR) zikrReset();
      depth = 2;
      return;
    }
    if (depth == 2) {
      switch (itemIdx) {
        case F_QURAN:   openSurah(subIdx);           depth = 3; break;
        case F_MORNING: openDhikr(MORNING, subIdx);  depth = 3; break;
        case F_EVENING: openDhikr(EVENING, subIdx);  depth = 3; break;
        default:        depth = 1; break;            // zikr and the names step back
      }
      return;
    }
    depth = 2;                                   // out of a chapter, back to the list
    return;
  }
  if (screen == S_GAMES) {
    if (depth == 1) { gamePending = itemIdx; navCalBegin(true); return; }
    if (gState == GS_OVER)  { gameStart(itemIdx); return; }
    if (gState == GS_PLAY)  { gState = GS_PAUSE;  return; }
    if (gState == GS_PAUSE) { gState = GS_PLAY; gNext = millis(); return; }
    return;
  }
  if (screen == S_SETTINGS && depth == 1) {
    switch (itemIdx) {
      case C_REBOOT:  delay(150); ESP.restart(); break;
      case C_UPDATE:  if (online()) { upState = U_MENU; upPick = 0; upMsg = ""; }
                      else { otaStatus = "No network"; otaPct = -1; drawOta(); delay(1600); }
                      break;
      case C_HOTSPOT: startHotspot(); break;
      case C_PRAYER:  prayerWanted = true; nextPrayerTry = 0; break;
      case C_ACCEL:   depth = 2; break;
      case C_PAIR:    depth = 2; newPairCode(); break;
      default:        depth = 2; break;
    }
  }
}

static void knockThree() {
  cTriple++;
  if (inReader()) { depth = 0; itemIdx = 0; subIdx = 0; return; }
  if (depth > 0) {
    depth--;
    if (depth == 0) { itemIdx = 0; subIdx = 0; }
    return;
  }
  screen = S_HOME; itemIdx = 0; subIdx = 0;
}

static void knockFour() {
  cQuad++;
  if (screen == S_GAMES && depth == 2) { gameStart(itemIdx); return; }
  if (screen == S_GAMES && depth == 1) {          // forget how it was taught to tilt
    mapAxX = mapAxY = -1;
    prefs.remove("tiltmap");
    return;
  }
  if (screen == S_READS && depth >= 1) { refillShelf(); depth = readCount ? 2 : 1; return; }
  if (screen == S_FAITH && depth == 2 && itemIdx == F_ZIKR) { zikrReset(); return; }
  screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0;
}

static void updateKnock(uint8_t n) {
  switch (upState) {
    case U_MENU:
      if (n == 1) { upPick = (upPick + 1) % 2; return; }
      if (n == 2) {
        otaStatus = "Checking"; otaStatus2 = ""; otaPct = -1; drawOta();
        if (upPick == 0) {
          if (!otaFetchLatest()) { upState = U_FAIL; return; }
          if (upTag == String("v" FW_VERSION) || upTag == String(FW_VERSION)) {
            upState = U_NONE; return;
          }
          upYes = true; upState = U_ASK; return;
        }
        if (!otaFetchList()) { upState = U_FAIL; return; }
        upState = U_LIST; return;
      }
      upState = U_OFF;
      return;

    case U_ASK:
      if (n == 1) { upYes = !upYes; return; }
      if (n == 2) {
        if (upYes) { upState = U_OFF; otaInstall(); upState = U_MENU; }  // returns only if it failed
        else upState = U_MENU;
        return;
      }
      upState = U_MENU;
      return;

    case U_LIST:
      if (n == 1) { if (relCount) relSel = (relSel + 1) % relCount; return; }
      if (n == 2) {
        upTag = relTag[relSel]; upUrl = relUrl[relSel];
        upYes = true; upState = U_ASK;
        return;
      }
      upState = U_MENU;
      return;

    default:                                   // nothing to do, or it went wrong
      upState = U_MENU;
      return;
  }
}

static void navHome() { upState = U_OFF; screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0; nTiltHome++; }

// While the update conversation is up it takes the leans too, so there is
// never a screen that only answers to one of the two.
static void navAct(uint8_t n) {
  if (upState != U_OFF) { updateKnock(n); return; }
  if (n == 1) knockOne();
  else if (n == 2) knockTwo();
  else knockThree();
}
static void navPrevAct() {
  switch (upState) {
    case U_OFF:  knockPrev(); return;
    case U_LIST: if (relCount) relSel = (relSel + relCount - 1) % relCount; return;
    case U_MENU: upPick = (upPick + 1) % 2; return;
    case U_ASK:  upYes = !upYes; return;
    default: return;
  }
}

// Down for the next thing, up for the one before, left to go in, right to
// come out, and up held for three seconds for home. Up does double duty,
// so it only counts as "the one before" once you let it go: holding it
// past three seconds means you wanted home instead.
static void tiltNav() {
  if (!cfgTilt || !tiltTaught()) return;
  if (navCal != NC_OFF) return;
  if (alertPhase != AL_NONE) return;
  if (screen == S_GAMES && depth == 2) return;      // in there, tilt is playing

  float tx, ty;
  tiltRead(tx, ty);
  unsigned long now = millis();

  // Gravity has the same magnitude whichever way up the thing is, so
  // leaning it never shows up in the movement check that keeps it awake:
  // it would doze off under your hand mid gesture. Watch the lean itself
  // change instead. One left standing at an angle still settles down.
  // Judge stillness from the sensor itself, not from the lean. The lean
  // is measured against rest, and the correction below moves rest, so a
  // lean based test would read its own correction as movement, stop
  // itself, and crawl. Raw cannot fight itself.
  float rawD = fabsf(ax - lastRawX) + fabsf(ay - lastRawY) + fabsf(az - lastRawZ);
  lastRawX = ax; lastRawY = ay; lastRawZ = az;

  // Gravity has the same magnitude whichever way up it is, so leaning
  // never showed up in the movement check that keeps it awake.
  if (rawD > 0.03f) lastActive = now;

  // Rest is measured in your hand, because that is where it asks you to
  // hold it. Put the thing down afterwards and every lean reads as
  // already held over: the latch never clears, no gesture fires again,
  // and nothing refreshes the idle timer, so it dozes and does the same
  // after every shake. Once it has sat still a while, let rest settle to
  // wherever it is actually sitting.
  if (rawD > 0.02f || !steadySince) steadySince = now;
  else if (!upSince && now - steadySince > 5000) {
    float v[3] = { ax, ay, az };
    for (int i = 0; i < 3; i++) restV[i] += (v[i] - restV[i]) * 0.08f;
  }

  if (ty > NAV_TILT_ON) {
    if (!upSince) { upSince = now; upConsumed = false; }
    else if (!upConsumed && now - upSince >= NAV_HOME_MS) {
      navHome(); upConsumed = true; lastActive = now;
    }
  } else if (upSince && fabsf(ty) < NAV_TILT_OFF) {
    if (!upConsumed) { navPrevAct(); nTiltPrev++; lastActive = now; }
    upSince = 0; upConsumed = false;
  }

  if (!navLatch) {
    if (ty < -NAV_TILT_ON)      { navAct(1); nTiltNext++; navLatch = true; lastActive = now; }
    else if (tx < -NAV_TILT_ON) { navAct(2); nTiltIn++;   navLatch = true; lastActive = now; }
    else if (tx >  NAV_TILT_ON) { navAct(3); nTiltOut++;  navLatch = true; lastActive = now; }
  }
  if (fabsf(tx) < NAV_TILT_OFF && fabsf(ty) < NAV_TILT_OFF) navLatch = false;
}

static void onFall() {
  cFall++;
  int keep = screen;
  applyEyes(cfgEyes);
  eyes.setMood(DEFAULT); eyes.setPosition(N); eyes.setVFlicker(ON, 6);
  for (int i = 0; i < 12; i++) { eyesFrame(); delay(16); }
  eyes.setPosition(S);
  for (int i = 0; i < 12; i++) { eyesFrame(); delay(16); }
  eyes.setVFlicker(OFF);
  eyes.setHeight(6, 6);
  for (int i = 0; i < 40; i++) { eyesFrame(); delay(16); }
  applyEyes(cfgEyes);
  screen = keep;
  react(300);
}

static void settleBurst() {
  if (!burst) return;
  if (burst < 4 && millis() - burstStart < TAP_WINDOW_MS) return;   // four is all there is
  uint8_t n = burst;
  burst = 0;
  if (upState != U_OFF) {
    updateKnock(n);
    lastActive = millis();
    Serial.printf("knock x%u -> update state %d\n", n, upState);
    return;
  }
  if      (n == 1) knockOne();
  else if (n == 2) knockTwo();
  else if (n == 3) knockThree();
  else             knockFour();
  Serial.printf("knock x%u -> %s depth %d item %d sub %d\n",
                n, S_NAME[screen], depth, itemIdx, subIdx);
}

static void input() {
  readSensors();
  unsigned long now = millis();

  // A knock that dismissed a card has already been acted on. Swallow the
  // rest of that burst so it does not also step the carousel.
  if (now < inputMuteUntil) {
    if (adxl) rReg(adxl, A_INT_SOURCE);
    burst = 0;
    lastActive = now;
    return;
  }

  if (amag < 0.60f) lastLowG = now;

  if (adxl) {
    uint8_t s = rReg(adxl, A_INT_SOURCE);
    // The latch trips on any brief unloading, and a hand turning the
    // device over produces those constantly. Believe it only if the low
    // reading is one we saw ourselves, and never twice in a few seconds.
    if (!cfgTilt && (s & INT_FF) && lastLowG && now - lastLowG < 400 &&
        (!lastFallAt || now - lastFallAt > 4000)) {
      lastFallAt = now;
      wake("fall"); onFall(); return;
    }
    if (s & INT_TAP1) {
      wake("knock");
      if (!burst) burstStart = now;
      if (burst < 4) burst++;
      lastActive = now;
    }
  }
  settleBurst();

  if (fabsf(amag - 1.0f) > SHAKE_G && now - lastShake > 600) {
    lastShake = now; cShake++;
    if (asleep) { wake("shake"); return; }
    lastActive = now;
  }
  if (fabsf(amag - 1.0f) > 0.12f || fabsf(gxr) + fabsf(gyr) + fabsf(gzr) > 25.0f) {
    if (asleep) wake("picked up");
    lastActive = now;
  }

  // Everything above watches the MAGNITUDE of the acceleration, and
  // gravity has the same magnitude whichever way the thing is turned:
  // leaning it changes the direction, not the size. A forty five degree
  // lean moves |a| by about a hundredth, so none of those checks can see
  // one at all. The gyro term was the only one that could, and it reads
  // zero unless you are actually rotating, and zero altogether if the
  // gyro is not answering. That is why shaking woke it, leaning never
  // did, and it dozed off under your hand and did it again every time.
  //
  // Compare the direction against a reference rather than against the
  // previous reading: a slow lean arrives in steps too small to notice
  // one at a time, but it still adds up against a fixed mark. Noise is
  // even handed, so it never accumulates. The mark creeps very slowly so
  // that thermal drift is absorbed without masking a real lean.
  float dirD = fabsf(ax - refAx) + fabsf(ay - refAy) + fabsf(az - refAz);
  lastDirD = dirD;
  if (dirD > 0.12f) {   // three times what a still device wanders
    refAx = ax; refAy = ay; refAz = az;
    if (asleep) wake("moved");
    lastActive = now;
  } else {
    refAx += (ax - refAx) * 0.005f;
    refAy += (ay - refAy) * 0.005f;
    refAz += (az - refAz) * 0.005f;
  }

  if (asleep) return;

  tiltNav();

  // Reading needs a long fuse. Two minutes on a page is normal, and
  // dozing off mid sentence would be maddening.
  unsigned long fuse = inReader() ? (unsigned long)READING_SLEEP_SEC
                                  : (unsigned long)sleepSecs();
  // Five seconds before it drops off, come back to the clock, so it is
  // always the clock you find when you glance at it. Not while you are
  // reading or mid game: that would lose your place.
  if (fuse && fuse > 6 && !inReader() && !(screen == S_GAMES && depth == 2) &&
      now - lastActive > (fuse - 5) * 1000UL && screen != S_HOME) {
    screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0;
  }
  if (fuse && now - lastActive > fuse * 1000UL) goSleep();   // 0 means never
}

// ================================================================
//  WEB PAGE  (one page, on your own network)
// ================================================================
const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Rafiq</title><style>
:root{--bg:#070d13;--card:#101c27;--fg:#e6eef5;--mut:#7d93a6;--line:#1e2f3d;--acc:#2dd4bf;--warn:#fbbf24}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;text-align:center}
.wrap{max-width:460px;margin:0 auto;padding:18px}
h1{font-size:12px;letter-spacing:.34em;color:var(--acc);margin:0}
.clock{font-size:42px;font-weight:200;margin:2px 0 0;font-variant-numeric:tabular-nums}
.sub{color:var(--mut);font-size:12px;letter-spacing:.1em;text-transform:uppercase}
.tabs{display:flex;gap:6px;margin:18px 0 14px;background:var(--card);border:1px solid var(--line);border-radius:12px;padding:5px}
.tabs button{flex:1;margin:0;padding:10px;border-radius:9px;background:transparent;color:var(--mut);font-weight:600;font-size:13px}
.tabs button.on{background:var(--acc);color:#04201c}
h2{font-size:11px;letter-spacing:.2em;color:var(--mut);margin:20px 0 8px;text-transform:uppercase}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px;margin-bottom:8px}
.g4{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}
.tile{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:11px 4px}
.tile b{display:block;font-size:18px;font-weight:500;font-variant-numeric:tabular-nums}
.tile span{font-size:10px;color:var(--mut)}
textarea{width:100%;padding:11px;border-radius:10px;border:1px solid var(--line);background:#0b141c;color:var(--fg);font:inherit;font-size:14px;margin-top:8px;resize:vertical}
input,select{width:100%;padding:11px;border-radius:10px;border:1px solid var(--line);background:#0b141c;color:var(--fg);font:inherit;text-align:center}
button{font:inherit;font-weight:600;padding:11px;border:0;border-radius:10px;background:var(--acc);color:#04201c;cursor:pointer;width:100%;margin-top:8px}
button.g{background:transparent;color:var(--fg);border:1px solid var(--line)}
button.d{background:transparent;color:var(--warn);border:1px solid var(--line)}
.row{display:flex;gap:8px}.row button{margin-top:0}
table{width:100%;font-size:13px;font-variant-numeric:tabular-nums}
td{padding:3px 0}td:first-child{color:var(--mut);text-align:left}td:last-child{text-align:right}
.task{display:flex;align-items:center;gap:8px;background:#0b141c;border:1px solid var(--line);border-radius:10px;padding:9px 11px;margin-bottom:6px;text-align:left}
.task b{flex:1;font-weight:500;font-size:14px}
.task i{color:var(--mut);font-style:normal;font-size:13px}
.task.now{border-color:var(--acc)}
.task.brk b{color:var(--warn)}
.task button{width:auto;margin:0;padding:5px 9px;font-size:12px}
.plan{display:grid;grid-template-columns:1fr 78px;gap:8px}
.big{font-size:34px;font-weight:200;font-variant-numeric:tabular-nums;margin:4px 0}
.bar{height:6px;background:#0b141c;border-radius:3px;overflow:hidden;margin-top:8px}
.bar i{display:block;height:100%;background:var(--acc)}
.adj{display:grid;grid-template-columns:repeat(5,1fr);gap:6px;margin-top:8px}
.adj label{display:block;font-size:10px;color:var(--mut);margin-bottom:3px}
.adj input{padding:9px 2px;font-size:14px}
#t{margin-top:10px;font-size:13px;color:var(--acc);min-height:18px}
</style></head><body><div class="wrap">
<h1>R A F I Q</h1>
<div class="clock" id="clk">--:--</div>
<div class="sub" id="sub">connecting</div>

<div class="tabs">
  <button id="tabA" class="on" onclick="tab('work')">Let's work together</button>
  <button id="tabB" onclick="tab('cfg')">Configuration</button>
</div>

<!-- ============ WORK ============ -->
<div id="work">
  <div class="card" id="now">
    <div class="sub" id="nowLabel">nothing running</div>
    <div class="big" id="nowTime">--:--</div>
    <div class="bar"><i id="nowBar" style="width:0%"></i></div>
    <div class="row" style="margin-top:10px">
      <button onclick="act('/api/session?go=1')">Start</button>
      <button class="g" onclick="act('/api/session?go=2')">Skip</button>
      <button class="d" onclick="act('/api/session?go=0')">Stop</button>
    </div>
  </div>

  <h2>The plan</h2>
  <div id="plan"></div>
  <div class="card">
    <div class="plan">
      <input id="tn" maxlength="40" placeholder="what are you doing?">
      <input id="tm" type="number" min="1" max="240" value="10" placeholder="min">
    </div>
    <div class="row" style="margin-top:8px">
      <button onclick="addTask()">Add</button>
      <button class="g" onclick="addBreak()">Add a break</button>
    </div>
    <button class="d" onclick="if(confirm('Clear the whole plan?'))act('/api/plan?clear=1')">Clear the plan</button>
  </div>

  <h2>Send a message</h2>
  <div class="card">
    <input id="m" maxlength="84" placeholder="it will pop up on the face">
    <button onclick="send()">Send</button>
  </div>

  <h2>The shelf</h2>
  <div class="card">
    <div class="sub" id="shelfMeta" style="margin-bottom:8px">empty</div>
    <div id="shelf"></div>
    <button class="g" onclick="act('/api/story')">Write a new one</button>
    <textarea id="paste" rows="5" placeholder="or paste a story of your own here"></textarea>
    <button class="g" onclick="pasteStory()">Put it on the shelf</button>
  </div>
</div>

<!-- ============ CONFIGURATION ============ -->
<div id="cfg" style="display:none">
  <h2>Screens</h2>
  <div class="card"><div class="row">
    <button class="g" onclick="go(0)">Home</button>
    <button class="g" onclick="go(1)">Focus</button>
    <button class="g" onclick="go(2)">Weather</button>
    <button class="g" onclick="go(3)">Msg</button>
  </div><div class="row" style="margin-top:8px">
    <button class="g" onclick="go(4)">Prayer</button>
    <button class="g" onclick="go(5)">Faith</button>
    <button class="g" onclick="go(6)">Reads</button>
    <button class="g" onclick="go(7)">Games</button>
  </div><div class="row" style="margin-top:8px">
    <button class="g" onclick="go(8)">Settings</button>
    <button class="g" onclick="go(9)">System</button>
  </div></div>

  <h2>Control</h2><div class="card">
    <table><tr><td>Driven by</td><td id="ctrlNow">taps</td></tr></table>
    <div class="row" style="margin-top:8px">
      <button class="g" onclick="setCtrl(0)">Taps only</button>
      <button class="g" onclick="setCtrl(1)">Taps and tilt</button>
    </div>
    <div style="font-size:12px;color:var(--mut);margin-top:8px;text-align:left">
      Switching tilt on here keeps the leans it already learned, and turns
      the fall animation off, which a hand sets off constantly. To teach
      the leans again, use Control on the device itself.</div>
  </div>

  <h2>Reading</h2><div class="card">
    <table><tr><td>Pages turn</td><td id="turnNow">by knock</td></tr></table>
    <div class="row" style="margin-top:8px">
      <button class="g" onclick="post('/api/turn',{a:0}).then(load)">By knock</button>
      <button class="g" onclick="post('/api/turn',{a:1}).then(load)">Automatically</button>
    </div>
  </div>

  <h2>Why it sleeps</h2><div class="card"><table id="diag"></table>
    <div style="font-size:12px;color:var(--mut);margin-top:8px;text-align:left">
      Live. If it dozes off while you are using it, look at <b>idle</b>
      climbing and at <b>lean seen</b>: that number has to cross its
      threshold for a lean to count as you being there.</div>
  </div>

  <h2>Knocks</h2>
  <div class="g4">
    <div class="tile"><b id="k1">0</b><span>ONE</span></div>
    <div class="tile"><b id="k2">0</b><span>TWO</span></div>
    <div class="tile"><b id="k3">0</b><span>THREE</span></div>
    <div class="tile"><b id="k4">0</b><span>FOUR</span></div>
  </div>

  <h2>Weather</h2><div class="card"><table id="wx"></table>
    <button class="g" onclick="act('/api/weather')">Refresh</button></div>
  <h2>Prayer times</h2><div class="card"><table id="pr"></table>
    <div class="sub" style="margin-top:12px">Adjust each one, in minutes</div>
    <div class="adj" id="adj"></div>
    <button onclick="saveAdj()">Save the adjustments</button>
  </div>

  <h2>Update</h2><div class="card">
    <button onclick="if(confirm('Install the newest release?'))act('/api/update')">Install the newest release</button>
    <button class="g" onclick="listRel()">List earlier releases</button>
    <div id="rel"></div>
  </div>

  <h2>System</h2><div class="card"><table id="sys"></table>
    <div class="row" style="margin-top:8px">
      <button class="g" onclick="if(confirm('Reboot?'))act('/api/reboot')">Reboot</button>
      <button class="g" onclick="act('/api/hotspot')">Hotspot</button>
    </div></div>

  <h2>OpenAI</h2><div class="card">
    <div class="sub" id="keyState" style="margin-bottom:8px">no key</div>
    <input id="key" type="password" placeholder="paste the API key">
    <button onclick="saveKey()">Save the key</button>
  </div>

  <h2>Network</h2><div class="card">
    <input id="ssid" placeholder="wifi network"><div style="height:8px"></div>
    <input id="pass" type="password" placeholder="wifi password (blank keeps it)">
    <div style="height:8px"></div>
    <input id="tz" placeholder="timezone eg IST-5:30">
    <button onclick="saveNet()">Save and reboot</button>
  </div>
</div>
<div id="t"></div>
</div><script>
const $=i=>document.getElementById(i);
window.esc=function(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
window.rows=function(el,o){$(el).innerHTML=Object.entries(o).map(([k,v])=>'<tr><td>'+k+'</td><td>'+esc(v)+'</td></tr>').join('')}
window.tok=function(){return localStorage.getItem('rtok')||''}
window.hdr=function(){const h={'Content-Type':'application/x-www-form-urlencoded'};const t=tok();if(t)h['X-Rafiq-Token']=t;return h}
window.post=async function(u,d){const r=await fetch(u,{method:'POST',headers:hdr(),body:new URLSearchParams(d||{})});if(r.status==401||r.status==403)askPair();return r}
window.locked=false;
window.askPair=async function(){
  if(locked)return;                       // one prompt, one code
  locked=true;
  await fetch('/api/paircode',{method:'POST'});
  const c=prompt('This one is paired to a Mac. Six digits are on its screen now:');
  if(c){
    const r=await fetch('/api/pair',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({c:c.trim()})});
    if(r.ok){const j=await r.json();
      if(j.token){localStorage.setItem('rtok',j.token);locked=false;load();return}}
  }
  // Left locked on purpose: polling on would mint a fresh code every
  // second and the digits on the panel would never sit still.
  const t=$('t');
  if(t){t.textContent='Locked to a Mac. Click here to pair.';t.style.cursor='pointer';
        t.onclick=function(){locked=false;askPair()}}
}
window.act=async function(u){$('t').textContent='working';await post(u,{});$('t').textContent='done';load()}
window.go=async function(n){await post('/api/screen',{n:n});$('t').textContent='opened'}
window.tab=function(w){
  const work=w==='work';
  $('work').style.display=work?'':'none';
  $('cfg').style.display=work?'none':'';
  $('tabA').className=work?'on':'';
  $('tabB').className=work?'':'on';
}
window.addTask=async function(){
  const n=$('tn').value.trim(), m=parseInt($('tm').value||'10',10);
  if(!n){$('t').textContent='name it first';return}
  await post('/api/plan',{name:n,mins:m});$('tn').value='';$('t').textContent='added';load()}
window.addBreak=async function(){
  await post('/api/plan',{name:'Break',mins:parseInt($('tm').value||'5',10)});
  $('t').textContent='break added';load()}
window.del=async function(i){await post('/api/plan',{del:i});load()}
window.openRead=async function(i){await post('/api/read',{open:i});$('t').textContent='opened on the face'}
window.dropRead=async function(i){if(!confirm('Remove this read?'))return;await post('/api/read',{del:i});load()}
window.send=async function(){const m=$('m').value.trim();if(!m){$('t').textContent='type something';return}
  await post('/api/msg',{m:m});$('m').value='';$('t').textContent='sent';load()}
window.pasteStory=async function(){
  const t=$('paste').value.trim();
  if(t.length<40){$('t').textContent='paste a bit more than that';return}
  $('t').textContent='storing';
  const r=await post('/api/paste',{text:t});
  const j=await r.json().catch(()=>({}));
  $('paste').value='';
  $('t').textContent='stored, '+(j.count||0)+' on the shelf';
  load();
}
window.setCtrl=async function(t){
  if(t===1&&!confirm('Switch to taps and tilt?'))return;
  await post('/api/control',{t:t}); $('t').textContent='control set'; load()}
window.listRel=async function(){
  $('t').textContent='asking github';
  const r=await post('/api/releases',{});
  const j=await r.json().catch(()=>({tags:[]}));
  const tags=j.tags||[];
  $('rel').innerHTML=tags.length?tags.map((t,i)=>
    '<div class="task"><b>'+esc(t)+'</b>'
    +'<button class="g" onclick="inst('+i+',\''+esc(t)+'\')">install</button></div>').join('')
    :'<div style="color:var(--mut);font-size:13px;padding:6px 0">nothing came back</div>';
  $('t').textContent=tags.length+' releases listed'}
window.inst=async function(i,t){
  if(!confirm('Put '+t+' on? It will restart when it is done.'))return;
  $('t').textContent='installing '+t;
  await post('/api/install',{i:i})}
window.saveAdj=async function(){
  const d={}; for(let i=0;i<5;i++) d['a'+i]=$('a'+i).value||'0';
  await post('/api/adj',d); $('t').textContent='adjustments saved'; load()}
window.saveKey=async function(){
  const k=$('key').value.trim(); if(!k){$('t').textContent='paste a key first';return}
  await post('/api/key',{key:k});$('key').value='';$('t').textContent='key saved';load()}
window.saveNet=async function(){
  const d={ssid:$('ssid').value,tz:$('tz').value};
  if($('pass').value)d.pass=$('pass').value;
  await post('/api/cfg',d);$('t').textContent='saved, rebooting'}
window.pushTime=async function(){const d=new Date();
  await post('/api/time',{e:Math.floor(d.getTime()/1000),o:-d.getTimezoneOffset()})}
let filled=false, adjFilled=false;
window.load=async function(){
  if(locked)return;
  const rr=await fetch('/api/state',{cache:'no-store',headers:hdr()});
  if(rr.status==401||rr.status==403){askPair();return}
  const s=await rr.json();
  if(!s.timeOk) await pushTime();
  $('clk').textContent=s.time;
  $('sub').textContent=(s.asleep?'asleep':'awake')+' · '+s.screen+' · fw '+s.fw;
  $('k1').textContent=s.k1;$('k2').textContent=s.k2;$('k3').textContent=s.k3;$('k4').textContent=s.k4;
  $('turnNow').textContent=s.autoTurn?'automatically':'by knock';
  $('ctrlNow').textContent=s.control;
  $('nowLabel').textContent=s.running?(s.taskName+'  ·  '+(s.taskIdx+1)+' of '+s.plan.length):'nothing running';
  $('nowTime').textContent=s.running?s.left:'--:--';
  $('nowBar').style.width=(s.running?s.taskPct:0)+'%';
  $('plan').innerHTML=s.plan.length?s.plan.map((p,i)=>
    '<div class="task'+(s.running&&i===s.taskIdx?' now':'')+(p.brk?' brk':'')+'">'
    +'<b>'+esc(p.name)+'</b><i>'+p.mins+' min</i>'
    +'<button class="d" onclick="del('+i+')">x</button></div>').join('')
    :'<div class="card" style="color:var(--mut);font-size:13px">nothing planned yet</div>';
  $('shelf').innerHTML=s.reads.length?s.reads.map((r,i)=>
    '<div class="task"><b>'+esc(r)+'</b>'
    +'<button class="g" onclick="openRead('+i+')">read</button>'
    +'<button class="d" onclick="dropRead('+i+')">x</button></div>').join('')
    :'<div style="color:var(--mut);font-size:13px;padding:6px 0">nothing on the shelf yet</div>';
  $('shelfMeta').textContent=s.reads.length?(s.reads.length+' stored · '+s.storyState):s.storyState;
  rows('diag',{'State':s.asleep?'asleep':'awake','Idle':s.idle+' s of '+s.sleepAfter,
               'Woke by':s.wokeBy,'Times slept':s.slept,
               'Sensors':s.sensors,'Gravity':s.accel,
               'Lean seen':s.dirD+' (needs 0.12)','Lean now':s.tilt});
  rows('wx',{'City':s.city,'Temperature':s.temp,'Humidity':s.hum,'Wind':s.wind,'Conditions':s.cond});
  rows('pr',s.prayer);
  if(!adjFilled){
    $('adj').innerHTML=Object.keys(s.prayer).map((n,i)=>
      '<div><label>'+n+'</label><input id="a'+i+'" type="number" min="-90" max="90" value="'+s.adj[i]+'"></div>').join('');
    adjFilled=true;
  }
  $('keyState').textContent=s.hasKey?('key saved · '+s.storyState):'no key yet';
  rows('sys',{'Signal':s.rssi,'Address':s.ip,'Hotspot':s.ap,'Control':s.control,'Tilts':s.tilts,'Free ram':s.heap+' B','OTA room':s.ota,
              'Storage used':s.fsUsed,'Uptime':s.up+' s','Boots':s.boots,'Falls':s.fall,
              'Chip':s.chip,'Firmware':s.fw,'Clock source':s.clockSrc});
  if(!filled){$('ssid').value=s.ssid;$('tz').value=s.tz;filled=true}
}
load();setInterval(load,1000);
</script></body></html>
)HTML";

static void apiState() {
  char t[10];
  clockStr(t, sizeof(t), true);
  String o = "{";
  o += "\"time\":\"" + String(t) + "\",\"screen\":\"" + String(S_NAME[screen]) + "\",";
  o += "\"timeOk\":" + String(timeOk ? "true" : "false") + ",";
  o += "\"clockSrc\":\"" + clockSrc + "\",";
  o += "\"asleep\":" + String(asleep ? "true" : "false") + ",\"fw\":\"" FW_VERSION "\",";
  o += "\"k1\":" + String(cTap) + ",\"k2\":" + String(cDouble) + ",\"k3\":" + String(cTriple) +
       ",\"k4\":" + String(cQuad) + ",\"fall\":" + String(cFall) + ",\"boots\":" + String(cBoot) + ",";
  o += "\"autoTurn\":" + String(cfgAutoTurn ? "true" : "false") + ",";
  o += "\"control\":\"" + String(cfgTilt ? "taps and tilt" : "taps") + "\",";
  o += "\"tilts\":\"" + String(nTiltNext) + " next, " + String(nTiltPrev) + " back, " +
       String(nTiltIn) + " in, " + String(nTiltOut) + " out, " + String(nTiltHome) + " home\",";
  o += "\"city\":\"" + wCity + "\",";
  o += "\"temp\":\"" + String(wxOk ? String(wTemp, 1) + " C" : String("--")) + "\",";
  o += "\"hum\":\"" + String(wxOk ? String(wHum, 0) + " %" : String("--")) + "\",";
  o += "\"wind\":\"" + String(wxOk ? String(wWind, 1) + " km/h" : String("--")) + "\",";
  o += "\"cond\":\"" + String(wxWord(wCode)) + "\",";

  o += "\"running\":" + String(sessionRunning() ? "true" : "false") + ",";
  o += "\"taskIdx\":" + String(taskIdx) + ",";
  if (sessionRunning()) {
    long left = (long)(taskEnd - millis()) / 1000L;
    if (left < 0) left = 0;
    char lt[12];
    snprintf(lt, sizeof(lt), "%ld:%02ld", left / 60, left % 60);
    long total = (long)tasks[taskIdx].mins * 60;
    String nm = tasks[taskIdx].name; nm.replace("\"", "'");
    o += "\"left\":\"" + String(lt) + "\",\"taskName\":\"" + nm + "\",";
    o += "\"taskPct\":" + String(total > 0 ? (int)(100 * (total - left) / total) : 0) + ",";
  } else {
    o += "\"left\":\"--:--\",\"taskName\":\"\",\"taskPct\":0,";
  }
  o += "\"plan\":[";
  for (int i = 0; i < taskCount; i++) {
    String nm = tasks[i].name; nm.replace("\\", " "); nm.replace("\"", "'");
    o += "{\"name\":\"" + nm + "\",\"mins\":" + String(tasks[i].mins) +
         ",\"brk\":" + String(isBreak(tasks[i].name) ? "true" : "false") + "}";
    if (i < taskCount - 1) o += ",";
  }
  o += "],";
  o += "\"reads\":[";
  for (int i = 0; i < readCount; i++) {
    String nm = readTitle[i]; nm.replace("\\", " "); nm.replace("\"", "'");
    o += "\"" + nm + "\"";
    if (i < readCount - 1) o += ",";
  }
  o += "],";
  o += "\"hasKey\":" + String(cfgKey.length() ? "true" : "false") + ",";
  o += "\"storyState\":\"" + storyState + "\",";
  o += "\"prayer\":{";
  for (int i = 0; i < 5; i++) {
    char v[12];
    if (prayerOk) fmt12(v, sizeof(v), prayerAt(i)); else strcpy(v, "--");
    o += "\"" + String(PRAYERS[i]) + "\":\"" + String(v) + "\"";
    if (i < 4) o += ",";
  }
  o += "},";
  const esp_partition_t* slot_ = esp_ota_get_next_update_partition(NULL);
  o += "\"ota\":\"" + String(slot_ ? String(slot_->size / 1024) + " kB slot, this build " +
                                      String(ESP.getSketchSize() / 1024) + " kB"
                                    : String("No OTA slot")) + "\",";
  o += "\"idle\":" + String((millis() - lastActive) / 1000UL) + ",";
  o += "\"sleepAfter\":\"" + String(sleepSecs() ? String(sleepSecs()) + " s" : String("never")) + "\",";
  o += "\"wokeBy\":\"" + wokeBy + "\",\"slept\":" + String(nSlept) + ",";
  o += "\"sensors\":\"" + String(adxl ? "adxl ok" : "NO ADXL") + ", " +
       String(mpu ? "mpu ok" : "no mpu") + "\",";
  {
    char ab[48];
    snprintf(ab, sizeof(ab), "%.2f %.2f %.2f  |a| %.2f", ax, ay, az, amag);
    o += "\"accel\":\"" + String(ab) + "\",";
    snprintf(ab, sizeof(ab), "%.3f", lastDirD);
    o += "\"dirD\":\"" + String(ab) + "\",";
    float tx = 0, ty = 0;
    if (tiltTaught()) tiltRead(tx, ty);
    snprintf(ab, sizeof(ab), "%+.2f across  %+.2f up", tx, ty);
    o += "\"tilt\":\"" + String(tiltTaught() ? ab : "not taught") + "\",";
  }
  o += "\"adj\":[";
  for (int i = 0; i < 5; i++) { o += String(prayerAdj[i]); if (i < 4) o += ","; }
  o += "],";
  o += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
  o += "\"fsUsed\":\"" + String(fsOk ? String(LittleFS.usedBytes() / 1024) + " / " +
                                       String(LittleFS.totalBytes() / 1024) + " kB"
                                     : String("No storage")) + "\",";
  o += "\"up\":" + String(millis() / 1000UL) + ",";
  o += "\"rssi\":\"" + String(online() ? String(WiFi.RSSI()) + " dBm" : String("offline")) + "\",";
  o += "\"ap\":\"" + String(rescueAP ? String(RESCUE_SSID) + " / " + WiFi.softAPIP().toString()
                                     : String("off")) + "\",";
  o += "\"chip\":\"" + String(ESP.getChipModel()) + " @" + String(ESP.getCpuFreqMHz()) + "MHz\",";
  o += "\"ip\":\"" + String(online() ? WiFi.localIP().toString() : String("not on a network")) + "\",";
  o += "\"paired\":" + String(cfgLock ? "true" : "false") + ",";
  o += "\"linked\":" + String(macLinked ? "true" : "false") + ",";
  o += "\"follow\":" + String(cfgFollow ? "true" : "false") + ",";
  o += "\"relax\":" + String(relaxOn ? "true" : "false") + ",";
  o += "\"webui\":" + String(webUiOn ? "true" : "false") + ",";
  o += "\"focusLeft\":" + String(sessionRunning()
          ? (long)((taskEnd - millis()) / 1000UL) : 0L) + ",";
  o += "\"ssid\":\"" + cfgSsid + "\",\"tz\":\"" + cfgTz + "\"}";
  web.send(200, "application/json", o);
}

static void setupWeb() {
  web.on("/", HTTP_GET, []() {
    if (!webUiOn) {
      web.send(200, "text/html; charset=utf-8",
               "<meta name=viewport content='width=device-width'>"
               "<body style='background:#111;color:#eee;font:16px system-ui;padding:2em'>"
               "<h2>Driven from your Mac</h2><p>This page comes back on its own "
               "a quarter of an hour after the Mac stops talking, so there is "
               "always a way in.</p></body>");
      return;
    }
    web.send_P(200, "text/html; charset=utf-8", PAGE);
  });
  web.on("/api/state", HTTP_GET, []() { if (!authed()) { web.send(401, "application/json", "{\"ok\":false}"); return; } sawMac(); apiState(); });

  // ---- pairing ----
  // Asking for a code is the one thing that needs no token, because
  // otherwise a device whose token you have lost could never be paired
  // again. Seeing the code still means standing in front of it.
  web.on("/api/paircode", HTTP_POST, []() { newPairCode(); okJson(); });
  web.on("/api/pair", HTTP_POST, []() {
    if (pairCode < 0 || millis() > pairUntil) {
      web.send(403, "application/json", "{\"ok\":false,\"err\":\"no code showing\"}");
      return;
    }
    if (web.arg("c").toInt() != pairCode) {
      pairCode = -1;                                   // one wrong guess spends it
      web.send(403, "application/json", "{\"ok\":false,\"err\":\"wrong code\"}");
      return;
    }
    // One device, one secret. A second client that can read the code off
    // the panel is standing in front of the thing, which is the whole
    // proof we ever wanted, so it gets the same token rather than a new
    // one that would lock the first client out. Forgetting clears it, and
    // the next pairing then mints a fresh one, which is how a token that
    // has got out is revoked.
    if (!cfgTok.length()) cfgTok = newToken();
    cfgLock = true;
    prefs.putString("tok", cfgTok);
    prefs.putBool("lock", true);
    pairCode = -1;
    screen = S_HOME; depth = 0;
    macSeen = millis(); macLinked = true;
    linkCardJoin = true; linkCardUntil = millis() + 1600;
    web.send(200, "application/json", "{\"ok\":true,\"token\":\"" + cfgTok + "\"}");
  });
  web.on("/api/unpair", HTTP_POST, []() {
    if (!guard()) return;
    cfgTok = ""; cfgLock = false;
    prefs.remove("tok"); prefs.putBool("lock", false);
    macLinked = false; webUiOn = true;
    okJson();
  });

  // ---- what the Mac drives ----
  web.on("/api/focus", HTTP_POST, []() {
    if (!guard()) return;
    focusBegin(constrain((int)web.arg("m").toInt(), 0, 240));
    okJson();
  });
  web.on("/api/toast", HTTP_POST, []() {
    if (!guard()) return;
    String m = web.arg("m"); m.trim();
    toastKind = web.arg("k");
    toastText = m.substring(0, 84);
    int secs = web.arg("s").toInt(); if (secs <= 0) secs = 4;
    toastUntil = millis() + (unsigned long)constrain(secs, 1, 60) * 1000UL;
    wake("mac");
    okJson();
  });
  web.on("/api/relax", HTTP_POST, []() {
    if (!guard()) return;
    relaxOn = web.arg("a").toInt() != 0;
    if (relaxOn) { relaxKind = 0; relaxNext = millis() + 30000UL; wake("relax"); }
    okJson();
  });
  web.on("/api/follow", HTTP_POST, []() {
    if (!guard()) return;
    cfgFollow = web.arg("a").toInt() != 0;
    prefs.putBool("follow", cfgFollow);
    if (cfgFollow) { cursorUdp.begin(CURSOR_PORT); wake("follow"); }
    else           { cursorUdp.stop(); curUntil = 0; }
    okJson();
  });
  // Nothing wakes it from this but the power. Say so, and mean it.
  web.on("/api/deepsleep", HTTP_POST, []() {
    if (!guard()) return;
    okJson();
    delay(200);
    oled.clearDisplay();
    robotHead(SCRW / 2, 26, false);
    ctr("Good night", 50, 1);
    oled.display();
    delay(2200);
    screenPower(false);
    esp_deep_sleep_start();
  });
  // One screenful of pixels, base64 of 1024 bytes, top row first.
  web.on("/api/canvas", HTTP_POST, []() {
    if (!guard()) return;
    if (!canvasBuf) canvasBuf = (uint8_t*)malloc(SCRW * SCRH / 8);
    if (!canvasBuf) { web.send(507, "application/json", "{\"ok\":false,\"err\":\"no room\"}"); return; }
    int n = b64decode(web.arg("b"), canvasBuf, SCRW * SCRH / 8);
    if (n < SCRW * SCRH / 8) {
      web.send(400, "application/json", "{\"ok\":false,\"err\":\"want 1024 bytes\"}");
      return;
    }
    int secs = web.arg("s").toInt(); if (secs <= 0) secs = 8;
    canvasUntil = millis() + (unsigned long)constrain(secs, 1, 300) * 1000UL;
    wake("canvas");
    okJson();
  });
  web.on("/api/webui", HTTP_POST, []() {
    if (!guard()) return;
    webUiOn = web.arg("a").toInt() != 0;
    webOffAt = webUiOn ? 0 : millis();
    okJson();
  });
  web.on("/api/msg", HTTP_POST, []() {
    if (!guard()) return;
    String m = web.arg("m"); m.trim();
    if (m.length()) {
      message = m.substring(0, 84);
      prefs.putString("msg", message);
      wake("message");
      if (popupSecs()) { screen = S_MSG; depth = 0; popupUntil = millis() + popupSecs() * 1000UL; }
    }
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/screen", HTTP_POST, []() {
    if (!guard()) return;
    screen = constrain((int)web.arg("n").toInt(), 0, S_COUNT - 1);
    depth = 0; itemIdx = 0; subIdx = 0; wake("panel");
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/weather", HTTP_POST, []() { if (!guard()) return; nextWx = 0; web.send(200, "application/json", "{\"ok\":true}"); });
  web.on("/api/turn", HTTP_POST, []() {
    if (!guard()) return;
    cfgAutoTurn = web.arg("a").toInt() != 0;
    prefs.putBool("turn", cfgAutoTurn);
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/adj", HTTP_POST, []() {
    if (!guard()) return;
    for (int i = 0; i < 5; i++) {
      String k = "a" + String(i);
      if (web.hasArg(k)) prayerAdj[i] = constrain((int)web.arg(k).toInt(), -90, 90);
    }
    saveAdj();
    for (int i = 0; i < 5; i++) alertDone[i] = 0;   // the times moved, so let today ring again
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/control", HTTP_POST, []() {
    if (!guard()) return;
    bool want = web.arg("t").toInt() != 0;
    if (want != cfgTilt) {
      cfgTilt = want;
      prefs.putBool("ctrl", cfgTilt);
      applyFallInt();
      // Never taught the leans? Ask on the device. Otherwise just find
      // where it is resting now, so the first lean is measured from there.
      if (cfgTilt) navCalBegin(!tiltTaught());
    }
    web.send(200, "application/json", "{\"ok\":true}");
  });

  // The list first, then install by its position in that list. The device
  // only ever fetches a URL it read from its own releases, never one
  // handed to it.
  web.on("/api/releases", HTTP_POST, []() {
    if (!guard()) return;
    String o = "{\"tags\":[";
    if (otaFetchList())
      for (int i = 0; i < relCount; i++) { o += "\"" + relTag[i] + "\""; if (i < relCount - 1) o += ","; }
    o += "]}";
    web.send(200, "application/json", o);
  });
  web.on("/api/install", HTTP_POST, []() {
    if (!guard()) return;
    int i = web.arg("i").toInt();
    if (i < 0 || i >= relCount) { web.send(400, "application/json", "{\"ok\":false}"); return; }
    upTag = relTag[i]; upUrl = relUrl[i];
    web.send(200, "application/json", "{\"ok\":true}");
    delay(250);
    otaInstall();
  });

  web.on("/api/hotspot", HTTP_POST, []() {
    if (!guard()) return;
    startHotspot();
    web.send(200, "application/json", "{\"ok\":true}");
  });

  web.on("/api/plan", HTTP_POST, []() {
    if (!guard()) return;
    if (web.hasArg("Clear")) {
      taskCount = 0; stopSession(); saveTasks();
    } else if (web.hasArg("del")) {
      int d = web.arg("del").toInt();
      if (d >= 0 && d < taskCount) {
        for (int i = d; i < taskCount - 1; i++) tasks[i] = tasks[i + 1];
        taskCount--;
        if (taskIdx >= taskCount) stopSession();
        saveTasks();
      }
    } else {
      String n = web.arg("name"); n.trim();
      int m = constrain((int)web.arg("mins").toInt(), 1, 240);
      if (n.length() && taskCount < TASK_MAX) {
        tasks[taskCount].name = n.substring(0, 40);
        tasks[taskCount].mins = m;
        taskCount++;
        saveTasks();
      }
    }
    web.send(200, "application/json", "{\"ok\":true}");
  });

  web.on("/api/session", HTTP_POST, []() {
    if (!guard()) return;
    int go = web.arg("go").toInt();
    if (go == 1)      { startSession(); wake("session"); }
    else if (go == 2) { if (sessionRunning()) { flashUntil = 0; startTask(taskIdx + 1); } }
    else              { stopSession(); }
    web.send(200, "application/json", "{\"ok\":true}");
  });

  // open one on the face, or take it off the shelf
  web.on("/api/read", HTTP_POST, []() {
    if (!guard()) return;
    if (web.hasArg("open")) {
      int i = web.arg("open").toInt();
      if (i >= 0 && i < readCount) {
        openRead(i);
        screen = S_READS; depth = 2; itemIdx = i;
        wake("read");
      }
    } else if (web.hasArg("del")) {
      int d = web.arg("del").toInt();
      if (fsOk && d >= 0 && d < readCount) {
        LittleFS.remove(readPath(d));
        for (int i = d; i < readCount - 1; i++) LittleFS.rename(readPath(i + 1), readPath(i));
        readCount--;
        loadShelf();
        if (itemIdx >= readCount) itemIdx = readCount ? readCount - 1 : 0;
        if (screen == S_READS && depth == 2) depth = 1;
      }
    }
    web.send(200, "application/json", "{\"ok\":true}");
  });

  // the key on its own, so saving it does not force a reboot
  web.on("/api/key", HTTP_POST, []() {
    if (!guard()) return;
    String k = web.arg("key"); k.trim();
    if (k.length()) {
      cfgKey = k;
      prefs.putString("key", cfgKey);
      storyState = readCount ? "Ready" : "Key saved";
      nextStory = 0;
      Serial.printf("openai key saved, %d chars\n", cfgKey.length());
    }
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/story", HTTP_POST, []() { if (!guard()) return; nextStory = 0; web.send(200, "application/json", "{\"ok\":true}"); });

  // paste your own: it goes on the shelf exactly like a written one
  web.on("/api/paste", HTTP_POST, []() {
    if (!guard()) return;
    String t = web.arg("text");
    t.trim();
    if (t.length()) {
      setStory(t.substring(0, STORY_MAX_CHARS));
      openRead(0);
      screen = S_READS; depth = 2;
      wake("read");
    }
    web.send(200, "application/json",
             String("{\"ok\":true,\"count\":") + String(readCount) + "}");
  });
  web.on("/api/time", HTTP_POST, []() {
    if (!guard()) return;
    long e = web.arg("e").toInt();
    int  z = web.arg("o").toInt();
    if (e > 1735689600L) {
      struct timeval tv = { .tv_sec = (time_t)e, .tv_usec = 0 };
      settimeofday(&tv, nullptr);
      char tz[24];
      snprintf(tz, sizeof(tz), "UTC%+d:%02d", -z / 60, abs(z) % 60);
      setenv("TZ", tz, 1); tzset();
      timeOk = true;
      clockSrc = "this browser";
    }
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/cfg", HTTP_POST, []() {
    if (!guard()) return;
    String s = web.arg("ssid"); s.trim();
    if (s.length()) prefs.putString("ssid", s);
    if (web.arg("pass").length()) prefs.putString("pass", web.arg("pass"));
    String z = web.arg("tz"); z.trim();
    if (z.length()) prefs.putString("tz", z);
    web.send(200, "application/json", "{\"ok\":true}");
    delay(300); ESP.restart();
  });
  web.on("/api/update", HTTP_POST, []() {
    if (!guard()) return;
    web.send(200, "application/json", "{\"ok\":true}");
    delay(200); runUpdate();
  });
  web.on("/api/reboot", HTTP_POST, []() {
    if (!guard()) return;
    web.send(200, "application/json", "{\"ok\":true}");
    delay(300); ESP.restart();
  });
  {
    const char* keep[] = { "X-Rafiq-Token" };
    web.collectHeaders(keep, 1);
  }
  web.onNotFound([]() { web.send(404, "text/plain", "not found"); });
  web.begin();
}
// ================================================================
//  BOOT
//    Each stage looks like what it is doing, and each frame is built
//    once and pushed once, so nothing twitches.
// ================================================================

// A knock is worth noticing even when nothing is polling for one, so
// look at the tap latch and at a firm nudge as well.
static bool knockPending() {
  if (adxl && (rReg(adxl, A_INT_SOURCE) & INT_TAP1)) return true;
  readSensors();
  return fabsf(amag - 1.0f) > 0.50f;
}

// eyes opening slowly, a look either way, one unhurried blink
static void animWake() {
  const unsigned long TOTAL = 2600;
  unsigned long t0 = millis();
  while (millis() - t0 < TOTAL) {
    float p = (float)(millis() - t0) / (float)TOTAL;
    int open = (p < 0.30f) ? (int)(100 * (p / 0.30f))
             : (p > 0.86f && p < 0.93f) ? 12          // the blink
             : 100;
    oled.clearDisplay();
    calmEyes(open, (int)(7 * sinf(p * 5.0f)), 30);
    oled.display();
    delay(33);
  }
}

// a sweep across the panel, lighting each sense as it passes over it
static void animSenses(unsigned long ms) {
  const int SX[2] = { 36, 90 };
  const char* SN[2] = { "Motion", "Tilt" };
  bool has[2] = { adxl != 0, mpu != 0 };
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    float p = (float)(millis() - t0) / (float)ms;
    int x = 6 + (int)((SCRW - 12) * p);
    oled.clearDisplay();
    titleBarC("CHECKING SENSES");
    oled.drawFastVLine(x, 14, 32, SSD1306_WHITE);
    for (int i = 0; i < 2; i++) {
      bool lit = x >= SX[i];
      if (lit && has[i]) oled.fillCircle(SX[i], 24, 4, SSD1306_WHITE);
      else               oled.drawCircle(SX[i], 24, 4, SSD1306_WHITE);
      if (lit) {
        oled.setTextSize(1);
        oled.setCursor(SX[i] - (int)strlen(SN[i]) * 3, 34);
        oled.print(SN[i]);
      }
    }
    if (p > 0.92f) {
      char l[20];
      snprintf(l, sizeof(l), "%d of 2 ready", (has[0] ? 1 : 0) + (has[1] ? 1 : 0));
      ctr(l, 50, 1);
    }
    oled.display();
    delay(30);
  }
}

// waves going out, the way a signal is always drawn
static void animWifiFrame() {
  oled.clearDisplay();
  titleBarC("JOINING WIFI");
  int cx = SCRW / 2, cy = 50;
  oled.fillCircle(cx, cy, 2, SSD1306_WHITE);
  int step = (millis() / 380) % 4;
  for (int k = 1; k <= 3; k++)
    if (step >= k) oled.drawCircleHelper(cx, cy, k * 8, 1 | 2, SSD1306_WHITE);
  oled.display();
}

// a face with a hand going round, because that is what it is waiting for
static void animClockFrame() {
  oled.clearDisplay();
  titleBarC("SETTING THE CLOCK");
  const int cx = SCRW / 2, cy = 38, r = 16;
  oled.drawCircle(cx, cy, r, SSD1306_WHITE);
  for (int i = 0; i < 12; i++) {
    float a = i * 0.5236f;
    oled.drawPixel(cx + cosf(a) * (r - 3), cy + sinf(a) * (r - 3), SSD1306_WHITE);
  }
  float a = (float)((millis() / 4) % 360) * 0.01745f - 1.5708f;
  oled.drawLine(cx, cy, cx + cosf(a) * (r - 5), cy + sinf(a) * (r - 5), SSD1306_WHITE);
  oled.fillCircle(cx, cy, 2, SSD1306_WHITE);
  oled.display();
}

// a calm settled face, held for a moment
static void restingFace(const char* caption, unsigned long ms) {
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    oled.clearDisplay();
    calmEyes(100, 0, 26);
    if (caption) ctr(caption, 54, 1);
    oled.display();
    delay(40);
  }
}

// Cards used to run out a fixed delay, so a knock during one did nothing
// and then arrived late. Now a knock ends the card at once, and the rest
// of that burst is muted so it does not also move the carousel.
static bool holdCard(unsigned long ms) {
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    if (knockPending()) {
      inputMuteUntil = millis() + TAP_WINDOW_MS + 150;
      burst = 0;
      lastActive = millis();
      return true;
    }
    web.handleClient();
    delay(8);
  }
  return false;
}

static void nameCard() {
  oled.clearDisplay();
  oled.setTextSize(3);
  oled.setCursor((SCRW - 5 * 18) / 2, 18);
  oled.print("RAFIQ");
  oled.drawFastHLine(24, 44, SCRW - 48, SSD1306_WHITE);
  ctr("Your companion", 50, 1);
  oled.display();
  holdCard(900);
}

// ---------------------------------------------------------------
//  No network is not an error, so it does not read like one. A
//  greeting, then the honest list of what still works without one.
// ---------------------------------------------------------------
static void offlineWelcome() {
  restingFace(nullptr, 900);

  // the greeting slides its underline open
  for (int f = 0; f <= 16; f++) {
    oled.clearDisplay();
    ctr("HELLO", 14, 3);
    int w = (SCRW - 48) * f / 16;
    oled.drawFastHLine((SCRW - w) / 2, 42, w, SSD1306_WHITE);
    if (f > 10) ctr("Good to see you", 50, 1);
    oled.display();
    delay(28);
  }
  delay(900);

  // what is still here, one line at a time
  const char* AVAIL[4] = { "saved messages", "short reads", "faith", "a stopwatch" };
  for (int n = 1; n <= 4; n++) {
    oled.clearDisplay();
    titleBar("READY OFFLINE", "");
    for (int i = 0; i < n; i++) {
      int y = 16 + i * 11;
      oled.fillCircle(8, y + 3, 2, SSD1306_WHITE);
      at(16, y, AVAIL[i]);
    }
    oled.display();
    delay(420);
  }
  holdCard(1500);

  oled.clearDisplay();
  titleBar("WHEN YOU WANT WIFI", "");
  ctr("Open SETTINGS", 20, 1);
  ctr("and knock twice on", 32, 1);
  ctr("Hotspot", 44, 1);
  oled.drawFastHLine(30, 56, SCRW - 60, SSD1306_WHITE);
  oled.display();
  holdCard(2600);
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  prefs.begin("nexus", false);
  cBoot = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", cBoot);
  cfgBright   = constrain(prefs.getInt("bri", 160), 0, 255);
  cfgSleepIdx = constrain(prefs.getInt("slpi", 1), 0, SLEEP_N - 1);
  cfgPopupIdx = constrain(prefs.getInt("popi", 2), 0, POPUP_N - 1);
  cfgEyes     = constrain(prefs.getInt("eye", 0), 0, STYLE_N - 1);
  cfgAutoTurn = prefs.getBool("turn", false);
  cfgFace     = constrain(prefs.getInt("face", F_CLASSIC), 0, FACE_N - 1);
  cfgTilt     = prefs.getBool("ctrl", false);
  // A paired Mac survives a reflash, because the token lives in NVS and
  // OTA never touches that. Losing it would mean walking over to the
  // device after every update, which nobody would put up with.
  cfgTok      = prefs.getString("tok", "");
  cfgLock     = prefs.getBool("lock", false) && cfgTok.length();
  cfgFollow   = prefs.getBool("follow", false);
  cfgTz       = prefs.getString("tz", DEF_TZ);
  cfgSsid     = prefs.getString("ssid", "");
  cfgPass     = prefs.getString("pass", "");
  cfgKey      = prefs.getString("key", "");
  message     = prefs.getString("msg", "");
  locLat      = prefs.getFloat("lat", NAN);
  locLon      = prefs.getFloat("lon", NAN);
  wCity       = prefs.getString("city", "");
  loadTasks();
  loadPrayer();
  loadAdj();
  loadTiltMap();
  for (int g = 0; g < G_COUNT; g++) gBest[g] = prefs.getInt(bestKey(g), 0);
  randomSeed(esp_random());
  swStart = millis();

  // First run: copy what is compiled in into flash. After that flash
  // wins, so an update can never take the network away.
  if (!cfgSsid.length() && strcmp(DEF_WIFI_SSID, "__WIFI_SSID__") != 0) {
    cfgSsid = DEF_WIFI_SSID; cfgPass = DEF_WIFI_PASS;
    prefs.putString("ssid", cfgSsid);
    prefs.putString("pass", cfgPass);
  }

  fsOk = LittleFS.begin(true);                 // format it once if it is blank
  if (!fsOk) Serial.println("no filesystem");
  loadShelf();

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) { Serial.println("no OLED"); return; }
  oled.setTextWrap(false);
  oled.setTextColor(SSD1306_WHITE);
  applyBright();

  eyes.begin(SCRW, SCRH, 50);
  applyEyes(cfgEyes);

  animWake();
  startSensors();
  applyFallInt();
  animSenses(1500);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (cfgSsid.length()) {
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 14000) {
      animWifiFrame();
      delay(40);
    }
  }

  setupWeb();

  if (online()) {
    restingFace("Connected", 700);
    // A few seconds here, then the loop keeps trying. Boot should not
    // hang on a clock that may take a minute to arrive.
    for (int f = 0; f < 6 && !timeOk; f++) {
      animClockFrame();
      if (trySyncTime(700)) break;
    }
    nextTimeTry = millis() + 15000;
    restingFace(timeOk ? "Clock set" : "Clock still coming", 700);
    nameCard();
  } else {
    offlineWelcome();
  }

  eyes.setAutoblinker(ON, 7, 5);      // a blink now and then, not a flutter
  eyes.setIdleMode(ON, 5, 4);
  eyes.setMood(STYLES[cfgEyes].mood);

  // Leaning only means anything against how it is sitting now, and that
  // changes every time it is picked up and put down. So it takes a moment
  // to settle before handing control over.
  if (cfgTilt) {
    navCalBegin(false);
    while (navCal != NC_OFF) { navCalService(); drawNavCal(); web.handleClient(); delay(40); }
  }

  // Two columns, numbers on a common left edge so the words line up.
  oled.clearDisplay();
  if (cfgTilt) {
    titleBarC("KNOCK OR LEAN");
    at(8,  16, "1 next");   at(66, 16, "dn next");
    at(8,  28, "2 open");   at(66, 28, "lt open");
    oled.drawFastHLine(8, 40, 112, SSD1306_WHITE);
    ctr("up 3s for home", 48, 1);
  } else {
    titleBarC("HOW TO KNOCK");
    at(8,  16, "1  next");
    at(8,  28, "2  open");
    at(66, 16, "3  back");
    at(66, 28, "4  reload");
    oled.drawFastHLine(8, 40, 112, SSD1306_WHITE);
    knockIcon(19, 51);
    at(32, 48, "Knock to begin");
  }
  oled.display();
  holdCard(2000);                      // a knock ends it, and it never dawdles

  screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0;
  lastActive = millis();
  drawHome();                          // on screen before anything can block

  // These all started at zero, so the first pass through loop() fired
  // every one of them back to back: weather, then prayer, then a story.
  // Between them that is the better part of half a minute of blocking
  // network calls, during which nothing redraws and no knock is acted
  // on. The card stayed up and the device looked wedged. Stagger them.
  if (cfgFollow && online()) cursorUdp.begin(CURSOR_PORT);

  nextWx        = millis() + 3000;
  nextPrayerTry = millis() + 8000;
  nextStory     = millis() + 25000;
  Serial.printf("up. fw %s boot #%lu  shelf %d  fs %s\n",
                FW_VERSION, (unsigned long)cBoot, readCount, fsOk ? "ok" : "none");
}

// ================================================================
//  LOOP
// ================================================================
void loop() {
  web.handleClient();
  unsigned long now = millis();

  if (now - lastPoll >= 45) { lastPoll = now; input(); }
  serviceSession();
  servicePrayerAlert();
  serviceCursor();

  // The Mac stops talking for all sorts of ordinary reasons: a lid
  // closed, a network changed, a laptop carried to another room. None
  // of them are a fault, so this is quiet about it and everything
  // carries on without it.
  if (macLinked && (now - macSeen) > MAC_GONE_MS) {
    macLinked = false;
    linkCardJoin = false;
    linkCardUntil = now + 1400;
    relaxOn = false; curUntil = 0; canvasUntil = 0;
  }
  // The page is never gone for good. A quarter of an hour after the Mac
  // goes quiet it is back, which is what keeps a broken app from being
  // a device you cannot reach.
  if (!webUiOn && (now - (macSeen > webOffAt ? macSeen : webOffAt)) > WEBUI_RETURN_MS)
    webUiOn = true;

  // Fetching blocks for seconds at a time, so it waits for a lull rather
  // than freezing the screen under someone's hand.
  bool idle = (now - lastActive) > 2500;

  if (online()) {
    // keep trying for a clock until one lands, then leave it alone
    if (!timeOk && idle && (long)(now - nextTimeTry) >= 0) {
      nextTimeTry = now + 20000;
      trySyncTime(1500);
    }
    if (!asleep && idle && (long)(now - nextWx) >= 0) { nextWx = now + 900000UL; fetchWeather(); }

    struct tm t;
    bool haveDay = timeOk && getLocalTime(&t, 0);
    // They barely move week to week, and refetching daily meant losing
    // them whenever the network was down. Keep what is in flash; refresh
    // when asked, or once every fiftieth boot.
    bool dueByBoot = (cBoot >= prayerBoot + PRAYER_REFRESH_BOOTS);
    if (idle && (long)(now - nextPrayerTry) >= 0 && haveDay &&
        (!prayerOk || prayerWanted || dueByBoot)) {
      nextPrayerTry = now + 300000UL;
      fetchPrayer();
    }
    // a fresh read every six hours, and the queue left over from a reload
    if (idle && (long)(now - nextStory) >= 0 && cfgKey.length() && !storyBusy && readCount < READS_MAX) {
      nextStory = now + 21600000UL;
      fetchStory(!asleep && screen == S_READS);
    }
    if (refillWant > 0 && !storyBusy && cfgKey.length() &&
        (long)(now - nextRefill) >= 0 && (asleep || screen != S_READS)) {
      refillWant--;
      nextRefill = now + 5000;
      fetchStory(false);                       // quietly, while you are elsewhere
    }
  }

  if (popupUntil && now > popupUntil) { popupUntil = 0; screen = S_HOME; depth = 0; }

  // Focus used to hold the panel lit for the whole run, which is the
  // one reliable way to burn a countdown into an OLED. It now shows
  // for twelve seconds, goes dark for ten, and every third time comes
  // back with a word instead of the clock. The device is never asleep
  // through any of it: the timer keeps running and one knock brings it
  // straight back.
  if (sessionRunning() || millis() < flashUntil) {
    lastActive = now;
    screen = S_FOCUS; depth = 0;
    if (millis() < flashUntil) {
      fzPhase = FZ_SHOW; screenPower(true);      // the end is worth looking at
    } else if ((long)(now - fzNext) >= 0) {
      if (fzPhase == FZ_DARK) {
        fzCycle++;
        if (fzCycle % 3 == 0) {
          fzPhase = FZ_QUOTE;
          fzLine = FZ_LINES[random(FZ_N)];
          fzNext = now + FZ_QUOTE_MS;
        } else {
          fzPhase = FZ_SHOW;
          fzNext = now + FZ_SHOW_MS;
        }
        screenPower(true);
      } else {
        fzPhase = FZ_DARK;
        fzNext = now + FZ_DARK_MS;
        screenPower(false);
      }
    }
    if (fzPhase == FZ_DARK) { delay(6); return; }
  }

  // depth only means something on the three screens that have one
  if (screen != S_FAITH && screen != S_READS && screen != S_GAMES &&
      screen != S_SETTINGS && depth) {
    depth = 0; itemIdx = 0; subIdx = 0;
  }

  if (asleep) { delay(6); return; }

  // the counter paces itself, and holds the screen while it runs
  if (screen == S_FAITH && depth == 2 && itemIdx == F_ZIKR) {
    if (zikrTotal < 100) {
      lastActive = now;
      if ((long)(now - zikrNext) >= 0) zikrTick();
    }
  }
  // pages turn themselves when you asked them to
  if (cfgAutoTurn && inReader() && (long)(now - rdTurn) >= 0) nextPage();

  if (upState != U_OFF) {                      // choosing what to install
    lastActive = now;
    if (now - lastDraw >= 90) { lastDraw = now; drawUpdateUI(); }
    delay(2);
    return;
  }

  if (navCal != NC_OFF) {                      // learning the leans
    lastActive = now;
    navCalService();
    if (now - lastDraw >= 60) { lastDraw = now; drawNavCal(); }
    delay(2);
    return;
  }
  if (gamePending >= 0) {                      // leans checked, on with the game
    itemIdx = gamePending;
    gamePending = -1;
    gameStart(itemIdx);
    depth = 2;
  }

  if (alertPhase != AL_NONE) {                 // the call takes the screen
    lastActive = now;
    if (now - lastDraw >= 60) { lastDraw = now; drawPrayerAlert(); }
    delay(2);
    return;
  }

  // What the Mac asked for. The call to prayer is checked above this
  // and returns first, so nothing sent from a laptop can ever sit on
  // top of the adhan.
  if (now < linkCardUntil) {
    lastActive = now;
    if (now - lastDraw >= 60) { lastDraw = now; drawLinkCard(); }
    delay(2); return;
  }
  if (toastUntil && now < toastUntil) {
    lastActive = now;
    if (now - lastDraw >= 90) { lastDraw = now; drawToast(); }
    delay(2); return;
  }
  if (toastUntil && now >= toastUntil) { toastUntil = 0; toastText = ""; toastKind = ""; }
  if (canvasUntil && now < canvasUntil) {
    lastActive = now;
    if (now - lastDraw >= 120) { lastDraw = now; drawCanvas(); }
    delay(2); return;
  }
  if (canvasUntil && now >= canvasUntil) canvasUntil = 0;
  if (relaxOn) {
    lastActive = now;
    if ((long)(now - relaxNext) >= 0) { relaxKind = (relaxKind + 1) % 3; relaxNext = now + 30000UL; }
    if (now - lastDraw >= 40) { lastDraw = now; drawRelax(); }
    delay(2); return;
  }
  // Following the pointer holds the screen only while the pointer is
  // actually moving. Stop touching the mouse and it lets go, and the
  // usual sleep takes over as if nothing had happened.
  if (cfgFollow && now < curUntil) {
    lastActive = now;
    if (now - lastDraw >= 45) { lastDraw = now; drawFollow(); }
    delay(2); return;
  }

  // a game runs its own clock, and holds the screen while it does
  if (screen == S_GAMES && depth == 2) {
    lastActive = now;
    serviceGame();
    if (now - lastDraw >= 33) { lastDraw = now; drawGames(); }
    delay(2);
    return;
  }

  if (now - lastDraw >= 110) {
    lastDraw = now;
    switch (screen) {
      case S_FOCUS:    drawFocus();    break;
      case S_WEATHER:  drawWeather();  break;
      case S_MSG:      drawMessage();  break;
      case S_PRAYER:   drawPrayer();   break;
      case S_FAITH:    drawFaith();    break;
      case S_READS:    drawReads();    break;
      case S_GAMES:    drawGames();    break;
      case S_SETTINGS: drawSettings(); break;
      case S_SYSTEM:   drawSystem();   break;
      default:         drawHome();     break;
    }
  }
  delay(2);
}

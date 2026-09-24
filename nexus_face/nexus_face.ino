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

#define FW_VERSION "1.6.0"
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
enum { C_BRIGHT = 0, C_SLEEP, C_TURN, C_POPUP, C_EYES, C_HOTSPOT,
       C_UPDATE, C_REBOOT, C_COUNT };
const char* C_NAME[C_COUNT] =
  { "Brightness", "Sleep after", "Page turn", "Popup time", "Eye style",
    "Hotspot", "Check update", "Reboot" };

const int SLEEP_OPTS[] = { 15, 30, 45, 60, 120, 180, 300, 600 };
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

// ---------------- runtime ----------------
bool asleep = false, screenOn = true, timeOk = false, rescueAP = false, fsOk = false;
unsigned long lastActive = 0, lastDraw = 0, lastPoll = 0, reactUntil = 0, lastShake = 0;
unsigned long nextTimeTry = 0, swStart = 0, inputMuteUntil = 0;
uint32_t cTap = 0, cDouble = 0, cTriple = 0, cQuad = 0, cFall = 0, cShake = 0, cBoot = 0;
uint8_t  burst = 0;
unsigned long burstStart = 0;
String   otaStatus = "", otaStatus2 = "";
String   clockSrc = "not set";
int      otaPct = -1;

static bool online() { return WiFi.status() == WL_CONNECTED; }

#define TAP_WINDOW_MS 560          // room to land four knocks
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
    wReg(adxl, A_THRESH_FF, 0x07);  wReg(adxl, A_TIME_FF, 0x14);
    wReg(adxl, A_INT_ENABLE, INT_TAP1 | INT_FF);   // single taps only, we count them
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
  if (!timeOk || !getLocalTime(&t, 5)) { snprintf(o, n, sec ? "--:--:--" : "--:--"); return; }
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

// The clock when there is one. Without a network there is no clock to
// show, so it runs a stopwatch instead of four dashes.
static void drawHome() {
  oled.clearDisplay();
  struct tm t;

  if (!timeOk || !getLocalTime(&t, 5)) {
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

  char big[8], sec[4], day[14], date[20];
  snprintf(big, sizeof(big), "%02d:%02d", t.tm_hour, t.tm_min);
  snprintf(sec, sizeof(sec), "%02d", t.tm_sec);
  strftime(day, sizeof(day), "%A", &t);
  strftime(date, sizeof(date), "%d %B %Y", &t);

  int bw = 5 * 18;
  int x0 = (SCRW - bw - 14) / 2;
  oled.setTextSize(3);
  oled.setCursor(x0, 8);
  oled.print(big);
  at(x0 + bw + 5, 18, sec);

  oled.drawFastHLine(22, 36, SCRW - 44, SSD1306_WHITE);
  ctr(day, 41, 1);
  ctr(date, 53, 1);
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
  int nowMin = (timeOk && getLocalTime(&t, 5)) ? t.tm_hour * 60 + t.tm_min : -1;
  int nx = nowMin >= 0 ? nextPrayer(nowMin) : -1;

  char v[12];
  for (int i = 0; i < 5; i++) {
    int y = 14 + i * 10;
    if (i == nx) {
      oled.fillRect(0, y - 1, SCRW, 10, SSD1306_WHITE);
      oled.setTextColor(SSD1306_BLACK);
    } else oled.setTextColor(SSD1306_WHITE);
    at(4, y, PRAYERS[i]);
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

  const char* nm = (alertWhich >= 0 && alertWhich < 5) ? PRAYERS[alertWhich] : "Prayer";
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

static void drawSettings() {
  oled.clearDisplay();
  if (depth == 0) {
    bar("SETTINGS");
    gearIcon(SCRW / 2, 32, 11);
    ctr("Two knocks to open", 52, 1);
    oled.display();
    return;
  }
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
      case C_BRIGHT: snprintf(v, sizeof(v), "%d", cfgBright); break;
      case C_SLEEP:  if (sleepSecs() < 60) snprintf(v, sizeof(v), "%ds", sleepSecs());
                     else snprintf(v, sizeof(v), "%dm", sleepSecs() / 60); break;
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
enum { G_SNAKE = 0, G_BRICK, G_COUNT };
const char* G_NAME[G_COUNT] = { "Snake", "Brick" };

enum { GS_CAL_STILL = 0, GS_CAL_RIGHT, GS_CAL_AWAY, GS_READY, GS_PLAY, GS_PAUSE, GS_OVER };
int  gState = GS_READY;
int  gScore = 0, gBest[G_COUNT] = { 0, 0 };
unsigned long gNext = 0, gStamp = 0;

int8_t mapAxX = -1, mapSgnX = 1, mapAxY = -1, mapSgnY = 1;   // -1 until taught
float  restV[3] = { 0, 0, 0 };
int    gravAx = 2;
float  calAcc[3] = { 0, 0, 0 };
int    calN = 0;

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
static bool tiltTaught() { return mapAxX >= 0 && mapAxY >= 0; }

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

static void brFill() {
  brLeft = 0;
  for (int r = 0; r < BR_ROWS; r++)
    for (int c = 0; c < BR_COLS; c++) { brick[r][c] = true; brLeft++; }
}
static void brServe() {
  brStuck = true;
  brStuckAt = millis();
  padX = (SCRW - PAD_W) / 2;
  bx = padX + PAD_W / 2; by = PAD_Y - 3;
  float sp = 0.85f + 0.12f * (brLevel - 1);
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
  padX += tx * 26.0f;                       // tilt slides it, proportionally
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

  // the paddle, which also steers: the edges send it away at an angle
  if (bvy > 0 && by >= PAD_Y - 2 && by <= PAD_Y + 2 &&
      bx >= padX - 1 && bx <= padX + PAD_W + 1) {
    by = PAD_Y - 2;
    bvy = -fabsf(bvy);
    float off = ((bx - padX) / PAD_W) - 0.5f;          // -0.5 .. 0.5
    bvx += off * 1.1f;
    bvx = constrain(bvx, -1.7f, 1.7f);
  }

  // bricks
  if (by >= BR_TOP && by < BR_TOP + BR_ROWS * BR_H) {
    int c = (int)bx / BR_W;
    int r = (int)(by - BR_TOP) / BR_H;
    if (c >= 0 && c < BR_COLS && r >= 0 && r < BR_ROWS && brick[r][c]) {
      brick[r][c] = false;
      brLeft--;
      gScore += 10;
      bvy = -bvy;
      if (!brLeft) { brLevel++; brFill(); brServe(); return; }
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
}

// ---------------- shared ----------------
static void gameStart(int which) {
  gState = tiltTaught() ? GS_CAL_STILL : GS_CAL_STILL;   // rest is measured every time
  calAcc[0] = calAcc[1] = calAcc[2] = 0; calN = 0;
  gStamp = millis();
  if (which == G_SNAKE) snReset(); else brReset();
}

// the teaching steps, and the short hold that finds the rest position
static void gameCalibrate(int which) {
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
    if (millis() - gStamp < 700) return;          // let the hand settle first
    for (int i = 0; i < 3; i++) {
      if (i == gravAx || i == mapAxX) continue;
      float d = v[i] - restV[i];
      if (fabsf(d) > 0.30f) {
        mapAxY = i;
        mapSgnY = d > 0 ? 1 : -1;
        saveTiltMap();
        gState = GS_READY;
        gStamp = millis();
        Serial.printf("tilt taught: x=axis%d(%+d) y=axis%d(%+d) grav=axis%d\n",
                      mapAxX, mapSgnX, mapAxY, mapSgnY, gravAx);
        return;
      }
    }
  }
}

static void serviceGame() {
  int which = itemIdx;
  unsigned long now = millis();
  readSensors();                       // the tick is faster than the input poll

  if (gState <= GS_CAL_AWAY) { gameCalibrate(which); return; }
  if (gState == GS_READY) {
    if (now - gStamp > 1800) { gState = GS_PLAY; gNext = now; }
    return;
  }
  if (gState != GS_PLAY) return;

  if (which == G_SNAKE) {
    if ((long)(now - gNext) < 0) return;
    gNext = now + snPace;
    snStep();
  } else {
    if ((long)(now - gNext) < 0) return;
    gNext = now + 28;
    brStepGame();
  }

  if (gState == GS_OVER && gScore > gBest[which]) {
    gBest[which] = gScore;
    prefs.putInt(which == G_SNAKE ? "bestSnake" : "bestBrick", gScore);
  }
}

// ---------------- the screens ----------------
static void padIcon(int cx, int cy) {
  oled.drawRoundRect(cx - 15, cy - 8, 30, 16, 5, SSD1306_WHITE);
  oled.drawFastHLine(cx - 11, cy, 7, SSD1306_WHITE);      // the cross
  oled.drawFastVLine(cx - 8, cy - 3, 7, SSD1306_WHITE);
  oled.fillCircle(cx + 7, cy - 2, 2, SSD1306_WHITE);      // and the buttons
  oled.fillCircle(cx + 11, cy + 3, 2, SSD1306_WHITE);
}
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

static void drawGameList() {
  oled.clearDisplay();
  char r[10];
  snprintf(r, sizeof(r), "%d/%d", itemIdx + 1, G_COUNT);
  titleBar("GAMES", r);
  const int TX[2] = { 10, 76 }, TY = 15, TW = 42, TH = 30;
  for (int i = 0; i < G_COUNT; i++) {
    if (i == itemIdx) oled.drawRoundRect(TX[i] - 2, TY - 2, TW, TH, 4, SSD1306_WHITE);
    if (i == G_SNAKE) snakeIcon(TX[i] + 4, TY + 4);
    else              brickIcon(TX[i] + 4, TY + 3);
  }
  char l[26];
  snprintf(l, sizeof(l), "%s   best %d", G_NAME[itemIdx], gBest[itemIdx]);
  ctr(l, 50, 1);
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

  if (gState == GS_READY) {
    titleBar(which == G_SNAKE ? "SNAKE" : "BRICK", "");
    ctr(which == G_SNAKE ? "Tilt to steer" : "Tilt to slide", 22, 1);
    long left = 1800 - (long)(millis() - gStamp);
    char c[4];
    snprintf(c, sizeof(c), "%ld", left / 600 + 1);
    ctr(c, 36, 2);
    oled.display();
    return;
  }

  char r[14];
  if (which == G_SNAKE) snprintf(r, sizeof(r), "%d", gScore);
  else                  snprintf(r, sizeof(r), "L%d %d", brLives, gScore);
  titleBar(which == G_SNAKE ? "SNAKE" : "BRICK", r);

  if (which == G_SNAKE) snDraw(); else brDraw();
  if (which == G_BRICK && brStuck && gState == GS_PLAY) ctr("Knock to launch", 44, 1);

  if (gState == GS_PAUSE) {
    oled.fillRect(10, 22, 108, 26, SSD1306_BLACK);
    oled.drawRect(10, 22, 108, 26, SSD1306_WHITE);
    ctr("PAUSED", 27, 1);
    ctr("2 go on  3 leave", 38, 1);
  }
  if (gState == GS_OVER) {
    // Brick scores reach four digits, so the two numbers get a line each
    // rather than sharing one and running out of the box.
    oled.fillRect(4, 13, 120, 46, SSD1306_BLACK);
    oled.drawRect(4, 13, 120, 46, SSD1306_WHITE);
    ctr("GAME OVER", 17, 1);
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
  if (!timeOk || !getLocalTime(&t, 5)) return;
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

static void runUpdate() {
  if (!online()) { otaStatus = "No network"; otaPct = -1; drawOta(); delay(1800); return; }
  otaStatus = "Checking"; otaPct = -1; drawOta();

  String b;
  if (!httpGetTo("https://api.github.com/repos/" OTA_REPO "/releases/latest", true, b, 12000)) {
    otaStatus = "GitHub unreachable"; drawOta(); delay(2200); return;
  }
  int i = b.indexOf("\"tag_name\":\"");
  String tag = i < 0 ? "" : b.substring(i + 12, b.indexOf('"', i + 12));
  int a = b.indexOf(OTA_ASSET);
  int u = a < 0 ? -1 : b.indexOf("\"browser_download_url\":\"", a);
  String url = u < 0 ? "" : b.substring(u + 24, b.indexOf('"', u + 24));
  b = String();                                  // let the reply go before we need the room
  if (!tag.length() || !url.length()) { otaStatus = "No release"; drawOta(); delay(2200); return; }
  if (verNum(tag) <= verNum(FW_VERSION)) { otaStatus = "Already newest"; drawOta(); delay(1800); return; }

  otaStatus = tag; otaPct = 0; drawOta();

  // ---- follow the redirects by hand, a fresh client for each host ----
  WiFiClientSecure* sec = nullptr;
  HTTPClient*       h   = nullptr;
  int  len = 0;
  bool open = false;

  for (int hop = 0; hop < 5 && !open; hop++) {
    sec = new WiFiClientSecure();
    if (!sec) { otaFail("Out of memory"); return; }
    sec->setInsecure();
    sec->setTimeout(30);                         // seconds, for the socket itself
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

  // "no room" used to be the whole story, which told you nothing. The
  // slot an update lands in is fixed by the partition table that was
  // written over USB, and only USB can change it, so say the numbers.
  const esp_partition_t* slot = esp_ota_get_next_update_partition(NULL);
  if (len <= 0) { h->end(); delete h; delete sec; otaFail("No content length"); return; }
  if (!slot) {
    h->end(); delete h; delete sec;
    otaFail("No OTA slot", "Needs min SPIFFS");
    return;
  }
  if ((size_t)len > slot->size || !Update.begin((size_t)len)) {
    char d[24];
    snprintf(d, sizeof(d), "%dk into %uk slot",
             len / 1024, (unsigned)(slot->size / 1024));
    h->end(); delete h; delete sec;
    otaFail("Will not fit", d);
    return;
  }

  // ---- pull it down in pieces, repainting as we go ----
  WiFiClient* st = h->getStreamPtr();
  static uint8_t buf[2048];
  size_t done = 0;
  unsigned long lastByte = millis();
  bool bad = false;

  otaStatus = "Downloading"; drawOta();

  while (done < (size_t)len) {
    size_t avail = st->available();
    if (avail) {
      int want = (int)min(avail, sizeof(buf));
      int got = st->readBytes(buf, want);
      if (got > 0) {
        if (Update.write(buf, got) != (size_t)got) { bad = true; break; }
        done += got;
        lastByte = millis();
        int p = (int)((done * 100ULL) / (size_t)len);
        if (p != otaPct) { otaPct = p; drawOta(); }
      }
    } else {
      if (!st->connected() && !st->available()) break;
      if (millis() - lastByte > 20000UL) { bad = true; break; }   // stalled, not slow
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
  setCpuFrequencyMhz(80);
}
static void wake(const char* why) {
  lastActive = millis();
  if (!asleep) return;
  asleep = false;
  setCpuFrequencyMhz(160);
  screenPower(true);
  eyes.setAutoblinker(ON, 7, 5); eyes.setIdleMode(ON, 5, 4);
  applyEyes(cfgEyes); eyes.open();
  for (int i = 0; i < 18; i++) { eyesFrame(); delay(16); }
  screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0;
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
  if (!prayerOk || !timeOk || !getLocalTime(&t, 5)) return;

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
      case C_BRIGHT: cfgBright += 45; if (cfgBright > 255) cfgBright = 25;
                     applyBright(); prefs.putInt("bri", cfgBright); break;
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

static void knockTwo() {
  cDouble++;
  if (depth == 0) {
    switch (screen) {
      case S_FAITH:    depth = 1; itemIdx = 0; subIdx = 0; break;
      case S_READS:    if (readCount) { depth = 1; itemIdx = 0; } else refillShelf(); break;
      case S_GAMES:    depth = 1; itemIdx = 0; break;
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
    if (depth == 1) { gameStart(itemIdx); depth = 2; return; }
    if (gState == GS_OVER)  { gameStart(itemIdx); return; }
    if (gState == GS_PLAY)  { gState = GS_PAUSE;  return; }
    if (gState == GS_PAUSE) { gState = GS_PLAY; gNext = millis(); return; }
    return;
  }
  if (screen == S_SETTINGS && depth == 1) {
    switch (itemIdx) {
      case C_REBOOT:  delay(150); ESP.restart(); break;
      case C_UPDATE:  if (online()) runUpdate();
                      else { otaStatus = "No network"; otaPct = -1; drawOta(); delay(1600); }
                      break;
      case C_HOTSPOT: startHotspot(); break;
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
  if (!burst || millis() - burstStart < TAP_WINDOW_MS) return;
  uint8_t n = burst;
  burst = 0;
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

  if (adxl) {
    uint8_t s = rReg(adxl, A_INT_SOURCE);
    if (s & INT_FF) { wake("fall"); onFall(); return; }
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
  if (asleep) return;

  // Reading needs a long fuse. Two minutes on a page is normal, and
  // dozing off mid sentence would be maddening.
  unsigned long fuse = inReader() ? (unsigned long)READING_SLEEP_SEC
                                  : (unsigned long)sleepSecs();
  if (now - lastActive > fuse * 1000UL) goSleep();
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

  <h2>Reading</h2><div class="card">
    <table><tr><td>Pages turn</td><td id="turnNow">by knock</td></tr></table>
    <div class="row" style="margin-top:8px">
      <button class="g" onclick="post('/api/turn',{a:0}).then(load)">By knock</button>
      <button class="g" onclick="post('/api/turn',{a:1}).then(load)">Automatically</button>
    </div>
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

  <h2>System</h2><div class="card"><table id="sys"></table>
    <div class="row" style="margin-top:8px">
      <button class="g" onclick="act('/api/update')">Check update</button>
      <button class="g" onclick="if(confirm('Reboot?'))act('/api/reboot')">Reboot</button>
    </div>
    <button class="g" onclick="act('/api/hotspot')">Turn on the hotspot</button></div>

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
window.post=async function(u,d){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(d||{})})}
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
  const s=await(await fetch('/api/state',{cache:'no-store'})).json();
  if(!s.timeOk) await pushTime();
  $('clk').textContent=s.time;
  $('sub').textContent=(s.asleep?'asleep':'awake')+' · '+s.screen+' · fw '+s.fw;
  $('k1').textContent=s.k1;$('k2').textContent=s.k2;$('k3').textContent=s.k3;$('k4').textContent=s.k4;
  $('turnNow').textContent=s.autoTurn?'automatically':'by knock';
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
  rows('wx',{'City':s.city,'Temperature':s.temp,'Humidity':s.hum,'Wind':s.wind,'Conditions':s.cond});
  rows('pr',s.prayer);
  if(!adjFilled){
    $('adj').innerHTML=Object.keys(s.prayer).map((n,i)=>
      '<div><label>'+n+'</label><input id="a'+i+'" type="number" min="-90" max="90" value="'+s.adj[i]+'"></div>').join('');
    adjFilled=true;
  }
  $('keyState').textContent=s.hasKey?('key saved · '+s.storyState):'no key yet';
  rows('sys',{'Signal':s.rssi,'Address':s.ip,'Hotspot':s.ap,'Free ram':s.heap+' B','OTA room':s.ota,
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
  o += "\"ssid\":\"" + cfgSsid + "\",\"tz\":\"" + cfgTz + "\"}";
  web.send(200, "application/json", o);
}

static void setupWeb() {
  web.on("/", HTTP_GET, []() { web.send_P(200, "text/html; charset=utf-8", PAGE); });
  web.on("/api/state", HTTP_GET, apiState);
  web.on("/api/msg", HTTP_POST, []() {
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
    screen = constrain((int)web.arg("n").toInt(), 0, S_COUNT - 1);
    depth = 0; itemIdx = 0; subIdx = 0; wake("panel");
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/weather", HTTP_POST, []() { nextWx = 0; web.send(200, "application/json", "{\"ok\":true}"); });
  web.on("/api/turn", HTTP_POST, []() {
    cfgAutoTurn = web.arg("a").toInt() != 0;
    prefs.putBool("turn", cfgAutoTurn);
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/adj", HTTP_POST, []() {
    for (int i = 0; i < 5; i++) {
      String k = "a" + String(i);
      if (web.hasArg(k)) prayerAdj[i] = constrain((int)web.arg(k).toInt(), -90, 90);
    }
    saveAdj();
    for (int i = 0; i < 5; i++) alertDone[i] = 0;   // the times moved, so let today ring again
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/hotspot", HTTP_POST, []() {
    startHotspot();
    web.send(200, "application/json", "{\"ok\":true}");
  });

  web.on("/api/plan", HTTP_POST, []() {
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
    int go = web.arg("go").toInt();
    if (go == 1)      { startSession(); wake("session"); }
    else if (go == 2) { if (sessionRunning()) { flashUntil = 0; startTask(taskIdx + 1); } }
    else              { stopSession(); }
    web.send(200, "application/json", "{\"ok\":true}");
  });

  // open one on the face, or take it off the shelf
  web.on("/api/read", HTTP_POST, []() {
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
  web.on("/api/story", HTTP_POST, []() { nextStory = 0; web.send(200, "application/json", "{\"ok\":true}"); });

  // paste your own: it goes on the shelf exactly like a written one
  web.on("/api/paste", HTTP_POST, []() {
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
    String s = web.arg("ssid"); s.trim();
    if (s.length()) prefs.putString("ssid", s);
    if (web.arg("pass").length()) prefs.putString("pass", web.arg("pass"));
    String z = web.arg("tz"); z.trim();
    if (z.length()) prefs.putString("tz", z);
    web.send(200, "application/json", "{\"ok\":true}");
    delay(300); ESP.restart();
  });
  web.on("/api/update", HTTP_POST, []() {
    web.send(200, "application/json", "{\"ok\":true}");
    delay(200); runUpdate();
  });
  web.on("/api/reboot", HTTP_POST, []() {
    web.send(200, "application/json", "{\"ok\":true}");
    delay(300); ESP.restart();
  });
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
  holdCard(1200);
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
  cfgBright   = constrain(prefs.getInt("bri", 160), 10, 255);
  cfgSleepIdx = constrain(prefs.getInt("slpi", 1), 0, SLEEP_N - 1);
  cfgPopupIdx = constrain(prefs.getInt("popi", 2), 0, POPUP_N - 1);
  cfgEyes     = constrain(prefs.getInt("eye", 0), 0, STYLE_N - 1);
  cfgAutoTurn = prefs.getBool("turn", false);
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
  gBest[G_SNAKE] = prefs.getInt("bestSnake", 0);
  gBest[G_BRICK] = prefs.getInt("bestBrick", 0);
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

  // Two columns, numbers on a common left edge so the words line up.
  oled.clearDisplay();
  titleBarC("HOW TO KNOCK");
  at(8,  16, "1  next");
  at(8,  28, "2  open");
  at(66, 16, "3  back");
  at(66, 28, "4  reload");
  oled.drawFastHLine(8, 40, 112, SSD1306_WHITE);
  knockIcon(19, 51);
  at(32, 48, "Knock to begin");
  oled.display();
  holdCard(2500);                      // a knock ends it, and it never dawdles

  screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0;
  lastActive = millis();
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

  if (online()) {
    // keep trying for a clock until one lands, then leave it alone
    if (!timeOk && (long)(now - nextTimeTry) >= 0) {
      nextTimeTry = now + 20000;
      trySyncTime(1500);
    }
    if (!asleep && (long)(now - nextWx) >= 0) { nextWx = now + 900000UL; fetchWeather(); }

    struct tm t;
    bool haveDay = timeOk && getLocalTime(&t, 5);
    if ((long)(now - nextPrayerTry) >= 0 && haveDay &&
        (!prayerOk || prayerDay != t.tm_yday)) {
      nextPrayerTry = now + 300000UL;
      fetchPrayer();
    }
    // a fresh read every six hours, and the queue left over from a reload
    if ((long)(now - nextStory) >= 0 && cfgKey.length() && !storyBusy && readCount < READS_MAX) {
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

  if (sessionRunning() || millis() < flashUntil) { lastActive = now; screen = S_FOCUS; depth = 0; }

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

  if (alertPhase != AL_NONE) {                 // the call takes the screen
    lastActive = now;
    if (now - lastDraw >= 60) { lastDraw = now; drawPrayerAlert(); }
    delay(2);
    return;
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

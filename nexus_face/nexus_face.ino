/*
  ================================================================
   NEXUS  -  ESP32-C3 desk companion
  ================================================================
   Knock on it to drive it.

     1 knock ...... next screen        (or next item inside a menu,
                                        or change the value you opened)
     2 knocks ..... go in
     3 knocks ..... come back out

   SCREENS   HOME, WEATHER, PRAYER, MESSAGES, STORY, SYSTEM, SETTINGS

   It joins your WiFi and serves one page at its own address. If it
   ever cannot get on, it raises a rescue hotspot so you can still
   reach the page and fix the credentials.

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
#include <Preferences.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <FluxGarage_RoboEyes.h>

#define SDA_PIN 8
#define SCL_PIN 9
#define OLED_ADDR 0x3C
#define SCRW 128                 // RoboEyes owns W and H, so ours differ
#define SCRH 64

#define FW_VERSION "1.2.0"
#define OTA_REPO   "AhmadMahi/nexus-face"
#define OTA_ASSET  "nexus_face.bin"

#define DEF_WIFI_SSID "YOUR_WIFI_NAME"
#define DEF_WIFI_PASS "YOUR_WIFI_PASSWORD"
#define DEF_TZ        "IST-5:30"
const char* RESCUE_SSID = "NEXUS-RESCUE";
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
enum { S_HOME = 0, S_FOCUS, S_WEATHER, S_PRAYER, S_MSG, S_STORY, S_SETTINGS, S_SYSTEM, S_COUNT };
const char* S_NAME[S_COUNT] =
  { "HOME", "FOCUS", "WEATHER", "PRAYER", "MESSAGES", "STORY", "SETTINGS", "SYSTEM" };

int screen = S_HOME;
int depth  = 0;                  // 0 screens, 1 settings list, 2 editing
int itemIdx = 0;

// ---------------- settings ----------------
enum { C_BRIGHT = 0, C_SLEEP, C_POPUP, C_EYES, C_STORY, C_UPDATE, C_REBOOT, C_COUNT };
const char* C_NAME[C_COUNT] =
  { "Brightness", "Sleep after", "Popup time", "Eye style", "New story", "Check update", "Reboot" };

// 0 to a minute in steps, then the longer ones, then round again
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
String cfgSsid, cfgPass, cfgTz, cfgKey;
int sleepSecs() { return SLEEP_OPTS[cfgSleepIdx]; }
int popupSecs() { return POPUP_OPTS[cfgPopupIdx]; }

// ---------------- content ----------------
String message = "";                       // just the latest one
unsigned long popupUntil = 0;

float wTemp = NAN, wHum = NAN, wWind = NAN;
int   wCode = -1;
String wCity = "";
bool  wxOk = false;
unsigned long nextWx = 0;
float locLat = NAN, locLon = NAN;

const char* PRAYERS[5] = { "Fajr", "Dhuhr", "Asr", "Maghrib", "Isha" };
int  prayerMin[5] = { -1, -1, -1, -1, -1 };
bool prayerOk = false;
int  prayerDay = -1;
unsigned long nextPrayerTry = 0;

// The story is wrapped into lines once, when it arrives, so turning a
// page later costs nothing.
#define STORY_LINES 72
#define LINES_PER_PAGE 4
String storyLine[STORY_LINES];
int    storyLines = 0, storyPage = 0;
bool   storyBusy = false;
String storyState = "no story yet";
unsigned long nextStory = 0, lastPageTurn = 0;

// ---------------- work session ----------------
//  A list of stretches to work through. Each one runs, flashes when it
//  is done, and hands over to the next.
#define TASK_MAX 8
struct Task { String name; int mins; };
Task tasks[TASK_MAX];
int  taskCount = 0;
int  taskIdx   = -1;               // -1 when nothing is running
unsigned long taskEnd = 0;
unsigned long flashUntil = 0;
const char* flashWord = "";
int  workedMin = 0;                // minutes since the last break
bool breakDue = false;
#define BREAK_AFTER_MIN 30

static bool isBreak(const String& n) {
  String l = n; l.toLowerCase();
  return l.indexOf("break") >= 0 || l.indexOf("rest") >= 0 || l.indexOf("walk") >= 0;
}
static bool sessionRunning() { return taskIdx >= 0 && taskIdx < taskCount; }

// ---------------- runtime ----------------
bool asleep = false, screenOn = true, timeOk = false, rescueAP = false;
unsigned long lastActive = 0, lastDraw = 0, lastPoll = 0, reactUntil = 0, lastShake = 0;
uint32_t cTap = 0, cDouble = 0, cTriple = 0, cFall = 0, cShake = 0, cBoot = 0;
uint8_t  burst = 0;
unsigned long burstStart = 0;
String   otaStatus = "";
int      otaPct = -1;

#define TAP_WINDOW_MS 520
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
//    y 0..9    title bar, white on black inverted
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
static void rightAt(int y, const char* s) {
  oled.setTextSize(1);
  oled.setCursor(SCRW - 2 - (int)strlen(s) * 6, y);
  oled.print(s);
}
static void clockStr(char* o, size_t n, bool sec) {
  struct tm t;
  if (!timeOk || !getLocalTime(&t, 5)) { snprintf(o, n, sec ? "--:--:--" : "--:--"); return; }
  if (sec) snprintf(o, n, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  else     snprintf(o, n, "%02d:%02d", t.tm_hour, t.tm_min);
}

// The bar is a filled block with the text knocked out of it, which reads
// far better than a hairline rule.
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
static void bar(const char* title) {
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
  if (c < 0)   return "no data";
  if (c == 0)  return "clear";
  if (c <= 2)  return "partly sunny";
  if (c == 3)  return "overcast";
  if (c <= 48) return "foggy";
  if (c <= 57) return "drizzle";
  if (c <= 67) return "rain";
  if (c <= 77) return "snow";
  if (c <= 82) return "showers";
  if (c <= 86) return "snow showers";
  return "thunderstorm";
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

// ================================================================
//  SCREENS
// ================================================================
// No signal, no clutter. Just what time it is and what day it is.
static void drawHome() {
  oled.clearDisplay();
  struct tm t;
  bool ok = timeOk && getLocalTime(&t, 5);

  char big[8], sec[4], day[14], date[20];
  if (ok) {
    snprintf(big, sizeof(big), "%02d:%02d", t.tm_hour, t.tm_min);
    snprintf(sec, sizeof(sec), "%02d", t.tm_sec);
    strftime(day, sizeof(day), "%A", &t);
    strftime(date, sizeof(date), "%d %B %Y", &t);
  } else {
    strcpy(big, "--:--"); strcpy(sec, "--");
    strcpy(day, "waiting"); strcpy(date, "for the clock");
  }

  // size 3 rather than 4: the bigger one ran right to both edges
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
    ctr(WiFi.status() == WL_CONNECTED ? "fetching" : "no network", 28, 1);
    ctr(wCity.length() ? wCity.c_str() : "locating", 44, 1);
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
  for (int i = 0; i < 5; i++) if (prayerMin[i] > nowMin) return i;
  return 0;
}
static void drawPrayer() {
  oled.clearDisplay();
  bar("PRAYER");
  if (!prayerOk) {
    ctr(WiFi.status() == WL_CONNECTED ? "fetching times" : "no network", 30, 1);
    oled.display();
    return;
  }
  struct tm t;
  int nowMin = (timeOk && getLocalTime(&t, 5)) ? t.tm_hour * 60 + t.tm_min : -1;
  int nx = nowMin >= 0 ? nextPrayer(nowMin) : -1;

  char v[12];
  for (int i = 0; i < 5; i++) {
    int y = 14 + i * 10;
    if (i == nx) {                                   // the one coming up, picked out
      oled.fillRect(0, y - 1, SCRW, 10, SSD1306_WHITE);
      oled.setTextColor(SSD1306_BLACK);
    } else oled.setTextColor(SSD1306_WHITE);
    at(4, y, PRAYERS[i]);
    fmt12(v, sizeof(v), prayerMin[i]);
    oled.setCursor(SCRW - 3 - (int)strlen(v) * 6, y);
    oled.print(v);
  }
  oled.setTextColor(SSD1306_WHITE);
  oled.display();
}

static void drawMessage() {
  oled.clearDisplay();
  bar("MESSAGE");
  if (!message.length()) {
    ctr("nothing yet", 28, 1);
    ctr("send one from the app", 44, 1);
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

static void drawStory() {
  oled.clearDisplay();
  char r[12];
  if (storyLines) {
    int pages = (storyLines + LINES_PER_PAGE - 1) / LINES_PER_PAGE;
    snprintf(r, sizeof(r), "%d/%d", storyPage + 1, pages);
  } else strcpy(r, "");
  titleBar("STORY", r);

  if (storyBusy) {
    ctr("writing", 26, 1);
    int a = (millis() / 110) % 8;
    for (int i = 0; i < 8; i++) {
      float th = i * 0.7854f;
      int rr = (i == a) ? 9 : 5;
      oled.drawPixel(SCRW / 2 + cosf(th) * rr, 46 + sinf(th) * rr, SSD1306_WHITE);
    }
    oled.display();
    return;
  }
  if (!storyLines) {
    ctr(storyState.c_str(), 28, 1);
    // only blame the key when the key really is the problem
    ctr(cfgKey.length() ? "two knocks to retry" : "add a key on the page", 44, 1);
    oled.display();
    return;
  }
  int start = storyPage * LINES_PER_PAGE;
  for (int i = 0; i < LINES_PER_PAGE && start + i < storyLines; i++)
    at(2, 16 + i * 12, storyLine[start + i].c_str());
  oled.display();
}

static void drawSystem() {
  oled.clearDisplay();
  bar("SYSTEM");

  char l[20];
  uint32_t heap = ESP.getFreeHeap(), tot = ESP.getHeapSize();

  // left: one headline number and a gauge under it
  snprintf(l, sizeof(l), "%u", (unsigned)(heap / 1024));
  oled.setTextSize(3);
  oled.setCursor(4, 15);
  oled.print(l);
  at(4, 39, "kB free");

  int bw = 56, fill = bw * heap / tot;
  oled.drawRect(4, 48, bw, 6, SSD1306_WHITE);
  if (fill > 2) oled.fillRect(5, 49, fill - 2, 4, SSD1306_WHITE);

  // right: one fact per line, all short enough to never reach the edge
  oled.drawFastVLine(64, 14, 42, SSD1306_WHITE);
  snprintf(l, sizeof(l), "up %lum", (unsigned long)(millis() / 60000UL));  at(69, 14, l);
  snprintf(l, sizeof(l), "boot %lu", (unsigned long)cBoot);                at(69, 23, l);
  snprintf(l, sizeof(l), "tap %lu", (unsigned long)(cTap + cDouble + cTriple)); at(69, 32, l);
  snprintf(l, sizeof(l), "fall %lu", (unsigned long)cFall);                at(69, 41, l);
  if (WiFi.status() == WL_CONNECTED) snprintf(l, sizeof(l), "%ddBm", WiFi.RSSI());
  else                               snprintf(l, sizeof(l), "no wifi");
  at(69, 50, l);

  // and the address on its own line, where it has the whole width
  if (WiFi.status() == WL_CONNECTED) at(4, 56, WiFi.localIP().toString().c_str());
  else at(4, 56, rescueAP ? "rescue 192.168.4.1" : "offline");
  oled.display();
}

// ---- the work session ----
// A countdown filling the screen, with the task itself scrolling along
// the bottom so a long name still reads.
static void drawFocus() {
  oled.clearDisplay();

  if (millis() < flashUntil) {                       // a stretch just ended
    oled.fillRect(0, 0, SCRW, SCRH, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    int n = strlen(flashWord);
    int size = n * 12 <= SCRW - 8 ? 2 : 1;
    ctr(flashWord, size == 2 ? 20 : 26, size);
    if (breakDue) ctr("walk for a minute", 42, 1);
    oled.setTextColor(SSD1306_WHITE);
    oled.display();
    return;
  }

  if (!sessionRunning()) {
    bar("FOCUS");
    ctr("nothing planned", 26, 1);
    ctr("add tasks on the page", 42, 1);
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

  // how far through this stretch we are
  long total = (long)t.mins * 60;
  int bw = SCRW - 16;
  int fill = total > 0 ? (int)((bw - 2) * (total - left) / total) : 0;
  oled.drawRect(8, 40, bw, 6, SSD1306_WHITE);
  if (fill > 0) oled.fillRect(9, 41, fill, 4, SSD1306_WHITE);

  // the task name scrolls if it does not fit
  int w = t.name.length() * 6;
  if (w <= SCRW - 4) {
    ctr(t.name.c_str(), 54, 1);
  } else {
    int span = w + 24;
    int off = (millis() / 55) % span;
    oled.setTextSize(1);
    oled.setCursor(2 - off, 54);          oled.print(t.name);
    oled.setCursor(2 - off + span, 54);   oled.print(t.name);
    oled.fillRect(0, 52, 2, 10, SSD1306_BLACK);
    oled.fillRect(SCRW - 2, 52, 2, 10, SSD1306_BLACK);
  }
  oled.display();
}

static void drawSettings() {
  oled.clearDisplay();
  if (depth == 0) {                                  // the signpost
    bar("SETTINGS");
    gearIcon(SCRW / 2, 32, 11);
    ctr("knock twice to open", 52, 1);
    oled.display();
    return;
  }
  bar(depth == 2 ? "CHANGE" : "SETTINGS");

  char v[18];
  int first = itemIdx > 3 ? itemIdx - 3 : 0;         // scroll, four rows fit
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
      case C_POPUP:  if (!popupSecs()) snprintf(v, sizeof(v), "off");
                     else snprintf(v, sizeof(v), "%ds", popupSecs()); break;
      case C_EYES:   snprintf(v, sizeof(v), "%s", STYLES[cfgEyes].name); break;
      case C_STORY:  snprintf(v, sizeof(v), "%s", cfgKey.length() ? "ready" : "no key"); break;
      case C_UPDATE: snprintf(v, sizeof(v), "%s", FW_VERSION); break;
      default:       snprintf(v, sizeof(v), "x2"); break;
    }
    oled.setCursor(SCRW - 3 - (int)strlen(v) * 6, y);
    oled.print(v);
    if (on && depth == 2) { oled.drawRect(0, y - 2, SCRW, 12, SSD1306_BLACK); }
  }
  oled.setTextColor(SSD1306_WHITE);
  oled.display();
}

static void drawOta() {
  oled.clearDisplay();
  bar("UPDATE");
  ctr(otaStatus.c_str(), 22, 1);
  if (otaPct >= 0) {
    int bw = SCRW - 24;
    oled.drawRect(12, 36, bw, 9, SSD1306_WHITE);
    int f = (bw - 4) * constrain(otaPct, 0, 100) / 100;
    if (f > 0) oled.fillRect(14, 38, f, 5, SSD1306_WHITE);
    char p[8];
    snprintf(p, sizeof(p), "%d%%", otaPct);
    ctr(p, 50, 1);
  } else {
    int a = (millis() / 120) % 8;
    for (int i = 0; i < 8; i++) {
      float th = i * 0.7854f;
      int r = (i == a) ? 9 : 5;
      oled.drawPixel(SCRW / 2 + cosf(th) * r, 44 + sinf(th) * r, SSD1306_WHITE);
    }
  }
  oled.display();
}

// ================================================================
//  NETWORK
// ================================================================
static bool httpGetTo(const String& url, bool tls, String& out, int ms) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient h;
  h.setConnectTimeout(ms); h.setTimeout(ms);
  h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  h.setUserAgent("nexus");
  bool ok = false;
  if (tls) { WiFiClientSecure c; c.setInsecure();
             if (h.begin(c, url) && h.GET() == 200) { out = h.getString(); ok = true; } }
  else     { WiFiClient c;
             if (h.begin(c, url) && h.GET() == 200) { out = h.getString(); ok = true; } }
  h.end();
  return ok;
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
  return !isnan(locLat) && locLat != 0;
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
  Serial.println("prayer times updated");
}

// ================================================================
//  STORY  (OpenAI, gpt-4o-mini)
// ================================================================
// The text is wrapped into lines the moment it arrives, so paging
// through it later is free.
static void layoutStory(const String& text) {
  storyLines = 0;
  storyPage = 0;
  const int PER = 21;
  int i = 0, n = text.length();
  while (i < n && storyLines < STORY_LINES) {
    while (i < n && (text.charAt(i) == ' ' || text.charAt(i) == '\n')) i++;
    if (i >= n) break;
    int take = min(PER, n - i);
    int nl = text.indexOf('\n', i);
    if (nl >= 0 && nl < i + take) take = nl - i;
    else if (take == PER) {
      int sp = text.lastIndexOf(' ', i + take);
      if (sp > i + 4) take = sp - i;
    }
    storyLine[storyLines++] = text.substring(i, i + take);
    i += take;
  }
}

static void fetchStory() {
  if (!cfgKey.length()) { storyState = "no api key"; return; }
  if (WiFi.status() != WL_CONNECTED) { storyState = "no network"; return; }

  storyBusy = true;
  storyState = "writing";
  drawStory();

  WiFiClientSecure c; c.setInsecure();
  HTTPClient h;
  h.setConnectTimeout(12000); h.setTimeout(25000);
  if (!h.begin(c, "https://api.openai.com/v1/chat/completions")) {
    storyBusy = false; storyState = "cannot reach openai"; return;
  }
  h.addHeader("Content-Type", "application/json");
  h.addHeader("Authorization", "Bearer " + cfgKey);

  String body = "{\"model\":\"gpt-4o-mini\",\"max_tokens\":700,\"temperature\":0.9,"
                "\"messages\":[{\"role\":\"user\",\"content\":"
                "\"Write a gentle love story of about 400 words in simple, warm English. "
                "Plain prose only: no title, no headings, no markdown, no quotation marks.\"}]}";

  int code = h.POST(body);
  if (code != 200) {
    String err = h.getString();
    h.end();
    storyBusy = false;
    storyState = (code == 401) ? "key rejected"
               : (code == 429) ? "rate limited"
               : ("openai " + String(code));
    Serial.println("story failed " + String(code) + " " + err.substring(0, 200));
    return;
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
    storyState = String("parse: ") + e.c_str();
    Serial.println("story parse failed: " + String(e.c_str()));
    Serial.println(reply.substring(0, 300));
    return;
  }

  String text = doc["choices"][0]["message"]["content"] | "";
  if (!text.length()) { storyBusy = false; storyState = "empty reply"; return; }

  layoutStory(text);
  storyBusy = false;
  storyState = "ready";
  lastPageTurn = millis();
  Serial.printf("story ready, %d lines\n", storyLines);
}

// ================================================================
//  WORK SESSION
// ================================================================
// Stored as  name|minutes;name|minutes;...  so the whole plan is one
// preference entry and survives a reboot.
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
    int bar = part.indexOf('|');
    if (bar > 0) {
      tasks[taskCount].name = part.substring(0, bar);
      tasks[taskCount].mins = constrain(part.substring(bar + 1).toInt(), 1, 240);
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
  if (i < 0 || i >= taskCount) {                 // the plan is finished
    taskIdx = -1;
    workedMin = 0; breakDue = false;
    flash("ALL DONE", 3000);
    Serial.println("session complete");
    return;
  }
  taskIdx = i;
  taskEnd = millis() + (unsigned long)tasks[i].mins * 60000UL;
  if (isBreak(tasks[i].name)) { workedMin = 0; breakDue = false; }
  Serial.printf("task %d/%d: %s for %d min\n", i + 1, taskCount,
                tasks[i].name.c_str(), tasks[i].mins);
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

// called once a stretch runs out
static void finishTask() {
  const Task& t = tasks[taskIdx];
  bool wasBreak = isBreak(t.name);
  if (!wasBreak) workedMin += t.mins;

  // an honest nudge once half an hour has gone by without a pause
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
  if (millis() < flashUntil) return;             // let the flash finish first
  if ((long)(millis() - taskEnd) >= 0) finishTask();
}

// ================================================================
//  OTA
// ================================================================
static long verNum(const String& v) {
  int a = 0, b = 0, c = 0;
  const char* p = v.c_str();
  while (*p && !isdigit((unsigned char)*p)) p++;
  sscanf(p, "%d.%d.%d", &a, &b, &c);
  return (long)a * 1000000L + b * 1000L + c;
}
static void otaProgress(size_t done, size_t total) {
  int p = total ? (int)((done * 100ULL) / total) : 0;
  if (p == otaPct) return;
  otaPct = p;
  drawOta();
}
static void runUpdate() {
  if (WiFi.status() != WL_CONNECTED) { otaStatus = "no network"; otaPct = -1; drawOta(); delay(1800); return; }
  otaStatus = "checking"; otaPct = -1; drawOta();

  String b;
  if (!httpGetTo("https://api.github.com/repos/" OTA_REPO "/releases/latest", true, b, 9000)) {
    otaStatus = "github unreachable"; drawOta(); delay(2000); return;
  }
  int i = b.indexOf("\"tag_name\":\"");
  String tag = i < 0 ? "" : b.substring(i + 12, b.indexOf('"', i + 12));
  int a = b.indexOf(OTA_ASSET);
  int u = a < 0 ? -1 : b.indexOf("\"browser_download_url\":\"", a);
  String url = u < 0 ? "" : b.substring(u + 24, b.indexOf('"', u + 24));
  if (!tag.length() || !url.length()) { otaStatus = "no release"; drawOta(); delay(2000); return; }
  if (verNum(tag) <= verNum(FW_VERSION)) { otaStatus = "already newest"; drawOta(); delay(1800); return; }

  otaStatus = tag; otaPct = 0; drawOta();
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h;
  h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  h.setConnectTimeout(15000); h.setTimeout(20000);
  h.setUserAgent("nexus");
  if (!h.begin(c, url) || h.GET() != 200) { otaStatus = "download failed"; drawOta(); delay(2000); h.end(); return; }
  int len = h.getSize();
  if (len <= 0 || !Update.begin(len)) { otaStatus = "no room"; drawOta(); delay(2000); h.end(); return; }
  Update.onProgress(otaProgress);
  size_t wrote = Update.writeStream(*h.getStreamPtr());
  h.end();
  if (wrote != (size_t)len || !Update.end(true)) { otaStatus = "install failed"; otaPct = -1; drawOta(); delay(2200); return; }
  otaStatus = "installed"; otaPct = 100; drawOta();
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
  for (int i = 0; i < 26; i++) { eyes.update(); delay(16); }
  screenPower(false);
  setCpuFrequencyMhz(80);
}
static void wake(const char* why) {
  lastActive = millis();
  if (!asleep) return;
  asleep = false;
  setCpuFrequencyMhz(160);
  screenPower(true);
  eyes.setAutoblinker(ON, 3, 2); eyes.setIdleMode(ON, 2, 2);
  applyEyes(cfgEyes); eyes.open();
  for (int i = 0; i < 18; i++) { eyes.update(); delay(16); }
  screen = S_HOME; depth = 0; itemIdx = 0;
  Serial.printf("awake (%s)\n", why);
}

// ================================================================
//  KNOCKS
//    depth 0   1 next screen   2 go in      3 nothing
//    depth 1   1 next item     2 open it    3 back to screens
//    depth 2   1 change value  2 nothing    3 back to the list
// ================================================================
static void react(unsigned long ms) { reactUntil = millis() + ms; }

static void knockOne() {
  cTap++;
  if (depth == 0) { screen = (screen + 1) % S_COUNT; itemIdx = 0; return; }
  if (depth == 1) { itemIdx = (itemIdx + 1) % C_COUNT; return; }
  switch (itemIdx) {                                  // depth 2: change it
    case C_BRIGHT: cfgBright += 45; if (cfgBright > 255) cfgBright = 25;
                   applyBright(); prefs.putInt("bri", cfgBright); break;
    case C_SLEEP:  cfgSleepIdx = (cfgSleepIdx + 1) % SLEEP_N;
                   prefs.putInt("slpi", cfgSleepIdx); break;
    case C_POPUP:  cfgPopupIdx = (cfgPopupIdx + 1) % POPUP_N;
                   prefs.putInt("popi", cfgPopupIdx); break;
    case C_EYES:   applyEyes(cfgEyes + 1); prefs.putInt("eye", cfgEyes); break;
    default: break;
  }
}

static void knockTwo() {
  cDouble++;
  if (depth == 0) {
    if (screen == S_SETTINGS) { depth = 1; itemIdx = 0; }
    else if (screen == S_WEATHER) nextWx = 0;         // refresh now
    else if (screen == S_PRAYER)  nextPrayerTry = 0;
    else if (screen == S_STORY)   nextStory = 0;
    return;
  }
  if (depth == 1) {
    if (itemIdx == C_REBOOT) { delay(150); ESP.restart(); }
    if (itemIdx == C_UPDATE) { runUpdate(); return; }
    if (itemIdx == C_STORY)  { nextStory = 0; screen = S_STORY; depth = 0; return; }
    depth = 2;
  }
}

static void knockThree() {
  cTriple++;
  if (depth > 0) depth--;
  else { screen = S_HOME; itemIdx = 0; }
}

static void onFall() {
  cFall++;
  int keep = screen;
  applyEyes(cfgEyes);
  eyes.setMood(DEFAULT); eyes.setPosition(N); eyes.setVFlicker(ON, 6);
  for (int i = 0; i < 12; i++) { eyes.update(); delay(16); }
  eyes.setPosition(S);
  for (int i = 0; i < 12; i++) { eyes.update(); delay(16); }
  eyes.setVFlicker(OFF);
  eyes.setHeight(6, 6);                               // flat out
  for (int i = 0; i < 40; i++) { eyes.update(); delay(16); }
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
  else             knockThree();
  Serial.printf("knock x%u -> %s depth %d item %d\n", n, S_NAME[screen], depth, itemIdx);
}

static void input() {
  readSensors();
  unsigned long now = millis();

  if (adxl) {
    uint8_t s = rReg(adxl, A_INT_SOURCE);
    if (s & INT_FF) { wake("fall"); onFall(); return; }
    if (s & INT_TAP1) {
      wake("knock");
      if (!burst) burstStart = now;
      if (burst < 3) burst++;
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
  if (now - lastActive > (unsigned long)sleepSecs() * 1000UL) goSleep();
}

// ================================================================
//  WEB PAGE  (one page, on your own network)
// ================================================================
const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Nexus</title><style>
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
.story{text-align:left;font-size:14px;line-height:1.55;color:#cfe0ee;max-height:200px;overflow:auto}
.bar{height:6px;background:#0b141c;border-radius:3px;overflow:hidden;margin-top:8px}
.bar i{display:block;height:100%;background:var(--acc)}
#t{margin-top:10px;font-size:13px;color:var(--acc);min-height:18px}
</style></head><body><div class="wrap">
<h1>N E X U S</h1>
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

  <h2>Story</h2><div class="card">
    <div class="story" id="st">nothing yet</div>
    <button class="g" onclick="act('/api/story')">Write a new one</button>
  </div>
</div>

<!-- ============ CONFIGURATION ============ -->
<div id="cfg" style="display:none">
  <h2>Screens</h2>
  <div class="card"><div class="row">
    <button class="g" onclick="go(0)">Home</button>
    <button class="g" onclick="go(1)">Focus</button>
    <button class="g" onclick="go(2)">Weather</button>
    <button class="g" onclick="go(3)">Prayer</button>
  </div><div class="row" style="margin-top:8px">
    <button class="g" onclick="go(4)">Msg</button>
    <button class="g" onclick="go(5)">Story</button>
    <button class="g" onclick="go(6)">Settings</button>
    <button class="g" onclick="go(7)">System</button>
  </div></div>

  <h2>Knocks</h2>
  <div class="g4">
    <div class="tile"><b id="k1">0</b><span>ONE</span></div>
    <div class="tile"><b id="k2">0</b><span>TWO</span></div>
    <div class="tile"><b id="k3">0</b><span>THREE</span></div>
    <div class="tile"><b id="k4">0</b><span>FALLS</span></div>
  </div>

  <h2>Weather</h2><div class="card"><table id="wx"></table>
    <button class="g" onclick="act('/api/weather')">Refresh</button></div>
  <h2>Prayer times</h2><div class="card"><table id="pr"></table></div>

  <h2>System</h2><div class="card"><table id="sys"></table><div class="bar"><i id="hb"></i></div>
    <div class="row" style="margin-top:8px">
      <button class="g" onclick="act('/api/update')">Check update</button>
      <button class="g" onclick="if(confirm('Reboot?'))act('/api/reboot')">Reboot</button>
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
window.send=async function(){const m=$('m').value.trim();if(!m){$('t').textContent='type something';return}
  await post('/api/msg',{m:m});$('m').value='';$('t').textContent='sent';load()}
window.saveKey=async function(){
  const k=$('key').value.trim(); if(!k){$('t').textContent='paste a key first';return}
  await post('/api/key',{key:k});$('key').value='';$('t').textContent='key saved';load()}
window.saveNet=async function(){
  const d={ssid:$('ssid').value,tz:$('tz').value};
  if($('pass').value)d.pass=$('pass').value;
  await post('/api/cfg',d);$('t').textContent='saved, rebooting'}
window.pushTime=async function(){const d=new Date();
  await post('/api/time',{e:Math.floor(d.getTime()/1000),o:-d.getTimezoneOffset()})}
let filled=false;
window.load=async function(){
  const s=await(await fetch('/api/state',{cache:'no-store'})).json();
  if(!s.timeOk) await pushTime();
  $('clk').textContent=s.time;
  $('sub').textContent=(s.asleep?'asleep':'awake')+' · '+s.screen+' · fw '+s.fw;
  $('k1').textContent=s.k1;$('k2').textContent=s.k2;$('k3').textContent=s.k3;$('k4').textContent=s.fall;
  $('nowLabel').textContent=s.running?(s.taskName+'  ·  '+(s.taskIdx+1)+' of '+s.plan.length):'nothing running';
  $('nowTime').textContent=s.running?s.left:'--:--';
  $('nowBar').style.width=(s.running?s.taskPct:0)+'%';
  $('plan').innerHTML=s.plan.length?s.plan.map((p,i)=>
    '<div class="task'+(s.running&&i===s.taskIdx?' now':'')+(p.brk?' brk':'')+'">'
    +'<b>'+esc(p.name)+'</b><i>'+p.mins+' min</i>'
    +'<button class="d" onclick="del('+i+')">x</button></div>').join('')
    :'<div class="card" style="color:var(--mut);font-size:13px">nothing planned yet</div>';
  rows('wx',{'city':s.city,'temperature':s.temp,'humidity':s.hum,'wind':s.wind,'conditions':s.cond});
  rows('pr',s.prayer);
  $('st').textContent=s.story;
  $('keyState').textContent=s.hasKey?('key saved · '+s.storyState):'no key yet';
  rows('sys',{'signal':s.rssi,'address':s.ip,'free ram':s.heap+' B','uptime':s.up+' s',
              'boots':s.boots,'chip':s.chip,'firmware':s.fw});
  $('hb').style.width=s.pct+'%';
  if(!filled){$('ssid').value=s.ssid;$('tz').value=s.tz;filled=true}
}
load();setInterval(load,1000);
</script></body></html>
)HTML";

static String f2(float v, int d) { return isnan(v) ? String("null") : String(v, d); }

static void apiState() {
  char t[10];
  clockStr(t, sizeof(t), true);
  uint32_t heap = ESP.getFreeHeap(), tot = ESP.getHeapSize();
  String o = "{";
  o += "\"time\":\"" + String(t) + "\",\"screen\":\"" + String(S_NAME[screen]) + "\",";
  o += "\"timeOk\":" + String(timeOk ? "true" : "false") + ",";
  o += "\"asleep\":" + String(asleep ? "true" : "false") + ",\"fw\":\"" FW_VERSION "\",";
  o += "\"k1\":" + String(cTap) + ",\"k2\":" + String(cDouble) + ",\"k3\":" + String(cTriple) +
       ",\"fall\":" + String(cFall) + ",\"boots\":" + String(cBoot) + ",";
  o += "\"city\":\"" + wCity + "\",";
  o += "\"temp\":\"" + String(wxOk ? String(wTemp, 1) + " C" : String("--")) + "\",";
  o += "\"hum\":\"" + String(wxOk ? String(wHum, 0) + " %" : String("--")) + "\",";
  o += "\"wind\":\"" + String(wxOk ? String(wWind, 1) + " km/h" : String("--")) + "\",";
  o += "\"cond\":\"" + String(wxWord(wCode)) + "\",";

  // the work session
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
  o += "\"hasKey\":" + String(cfgKey.length() ? "true" : "false") + ",";
  o += "\"storyState\":\"" + storyState + "\",";
  o += "\"prayer\":{";
  for (int i = 0; i < 5; i++) {
    char v[12];
    if (prayerOk) fmt12(v, sizeof(v), prayerMin[i]); else strcpy(v, "--");
    o += "\"" + String(PRAYERS[i]) + "\":\"" + String(v) + "\"";
    if (i < 4) o += ",";
  }
  o += "},";
  String st = "";
  for (int i = 0; i < storyLines; i++) { st += storyLine[i]; st += " "; }
  st.replace("\\", " "); st.replace("\"", "'");
  o += "\"story\":\"" + String(st.length() ? st : storyState) + "\",";
  o += "\"heap\":" + String(heap) + ",\"pct\":" + String(100 - heap * 100 / tot) + ",";
  o += "\"up\":" + String(millis() / 1000UL) + ",";
  o += "\"rssi\":\"" + String(WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) + " dBm" : String("offline")) + "\",";
  o += "\"chip\":\"" + String(ESP.getChipModel()) + " @" + String(ESP.getCpuFreqMHz()) + "MHz\",";
  o += "\"ip\":\"" + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "\",";
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
    depth = 0; itemIdx = 0; wake("panel");
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/weather", HTTP_POST, []() { nextWx = 0; web.send(200, "application/json", "{\"ok\":true}"); });

  // the plan: add one, delete one, or wipe it
  web.on("/api/plan", HTTP_POST, []() {
    if (web.hasArg("clear")) {
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

  // the key on its own, so saving it does not force a reboot
  web.on("/api/key", HTTP_POST, []() {
    String k = web.arg("key"); k.trim();
    if (k.length()) {
      cfgKey = k;
      prefs.putString("key", cfgKey);
      storyState = "key saved";
      nextStory = 0;                       // have a go straight away
      Serial.printf("openai key saved, %d chars\n", cfgKey.length());
    }
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/story",   HTTP_POST, []() { nextStory = 0; web.send(200, "application/json", "{\"ok\":true}"); });
  web.on("/api/time", HTTP_POST, []() {
    long e = web.arg("e").toInt();
    int  z = web.arg("o").toInt();
    if (e > 1700000000L) {
      struct timeval tv = { .tv_sec = (time_t)e, .tv_usec = 0 };
      settimeofday(&tv, nullptr);
      char tz[24];
      snprintf(tz, sizeof(tz), "UTC%+d:%02d", -z / 60, abs(z) % 60);
      setenv("TZ", tz, 1); tzset();
      timeOk = true;
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
//  BOOT ANIMATION
// ================================================================
// A caption under the eyes, so the eyes carry the boot rather than a
// wall of text doing it.
static void bootFrame(const char* caption, int dots) {
  eyes.update();                       // draws the eyes into the buffer
  if (caption) {
    oled.fillRect(0, 52, SCRW, 12, SSD1306_BLACK);
    char l[26];
    snprintf(l, sizeof(l), "%s%.*s", caption, dots, "...");
    ctr(l, 55, 1);
  }
  oled.display();
}
static void bootStage(const char* caption, unsigned long ms) {
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    bootFrame(caption, (int)((millis() / 350) % 4));
    delay(24);
  }
}

// eyes closed, then a slow blink open and a look around
static void wakeUpAnimation() {
  eyes.setAutoblinker(OFF); eyes.setIdleMode(OFF);
  eyes.setMood(TIRED);
  eyes.close();
  for (int i = 0; i < 20; i++) { eyes.update(); delay(22); }

  eyes.open();
  eyes.setMood(DEFAULT);
  for (int i = 0; i < 18; i++) { eyes.update(); delay(22); }

  eyes.setPosition(W); for (int i = 0; i < 12; i++) { eyes.update(); delay(20); }
  eyes.setPosition(E); for (int i = 0; i < 12; i++) { eyes.update(); delay(20); }
  eyes.setPosition(DEFAULT);
  eyes.blink();
  for (int i = 0; i < 10; i++) { eyes.update(); delay(20); }
}

static void nameCard() {
  oled.clearDisplay();
  oled.setTextSize(3);
  oled.setCursor((SCRW - 5 * 18) / 2, 18);
  oled.print("NEXUS");
  oled.drawFastHLine(24, 44, SCRW - 48, SSD1306_WHITE);
  ctr("desk companion", 50, 1);
  oled.display();
  delay(1300);
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
  cfgTz       = prefs.getString("tz", DEF_TZ);
  cfgSsid     = prefs.getString("ssid", "");
  cfgPass     = prefs.getString("pass", "");
  cfgKey      = prefs.getString("key", "");
  message     = prefs.getString("msg", "");
  loadTasks();

  // First run: copy what is compiled in into flash. After that flash
  // wins, so an update can never take the network away.
  if (!cfgSsid.length() && strcmp(DEF_WIFI_SSID, "YOUR_WIFI_NAME") != 0) {
    cfgSsid = DEF_WIFI_SSID; cfgPass = DEF_WIFI_PASS;
    prefs.putString("ssid", cfgSsid);
    prefs.putString("pass", cfgPass);
  }

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  // last argument stops Adafruit calling Wire.begin() on the default pins
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) { Serial.println("no OLED"); return; }
  oled.setTextWrap(false);
  oled.setTextColor(SSD1306_WHITE);
  applyBright();

  eyes.begin(SCRW, SCRH, 50);
  applyEyes(cfgEyes);

  wakeUpAnimation();                                 // good morning
  startSensors();
  bootStage("checking senses", 700);

  // ---- station only. The hotspot is a rescue door, not a network.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (cfgSsid.length()) {
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 14000) {
      bootFrame("joining wifi", (int)((millis() / 350) % 4));
      delay(24);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    eyes.setMood(HAPPY);
    bootStage("connected", 700);
    configTzTime(cfgTz.c_str(), "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    struct tm tm0;
    unsigned long t0 = millis();
    while (!getLocalTime(&tm0, 200) && millis() - t0 < 8000) {
      bootFrame("getting the time", (int)((millis() / 350) % 4));
      delay(24);
    }
    timeOk = getLocalTime(&tm0, 200);
    eyes.setMood(timeOk ? HAPPY : DEFAULT);
    bootStage(timeOk ? "clock set" : "no clock yet", 700);
  } else {
    // could not get on: raise the rescue hotspot so the page is still
    // reachable and the credentials can be fixed without a cable
    WiFi.mode(WIFI_AP);
    WiFi.softAP(RESCUE_SSID, RESCUE_PASS);
    rescueAP = true;
    eyes.setMood(TIRED);
    bootStage("no wifi", 900);
    oled.clearDisplay();
    ctr("RESCUE HOTSPOT", 14, 1);
    ctr(RESCUE_SSID, 28, 1);
    ctr("pass: password", 40, 1);
    ctr("192.168.4.1", 52, 1);
    oled.display();
    delay(3500);
  }

  setupWeb();
  nameCard();

  eyes.setAutoblinker(ON, 3, 2);
  eyes.setIdleMode(ON, 2, 2);
  eyes.setMood(STYLES[cfgEyes].mood);

  oled.clearDisplay();
  titleBar("READY", "");
  ctr("1 next", 20, 1);
  ctr("2 go in", 34, 1);
  ctr("3 back", 48, 1);
  oled.display();
  delay(1900);

  screen = S_HOME; depth = 0; itemIdx = 0;
  lastActive = millis();
  Serial.printf("up. fw %s boot #%lu\n", FW_VERSION, (unsigned long)cBoot);
}

// ================================================================
//  LOOP
// ================================================================
void loop() {
  web.handleClient();
  unsigned long now = millis();

  if (now - lastPoll >= 45) { lastPoll = now; input(); }
  serviceSession();

  if (!asleep && WiFi.status() == WL_CONNECTED) {
    if ((long)(now - nextWx) >= 0) { nextWx = now + 900000UL; fetchWeather(); }

    struct tm t;
    bool haveDay = timeOk && getLocalTime(&t, 5);
    if ((long)(now - nextPrayerTry) >= 0 && haveDay &&
        (!prayerOk || prayerDay != t.tm_yday)) {
      nextPrayerTry = now + 300000UL;
      fetchPrayer();
    }
    if ((long)(now - nextStory) >= 0 && cfgKey.length() && !storyBusy) {
      nextStory = now + 21600000UL;          // a fresh one every six hours
      fetchStory();
    }
  }

  // a message that just arrived holds the screen for a moment
  if (popupUntil && now > popupUntil) { popupUntil = 0; screen = S_HOME; }

  // a running session keeps itself awake and on screen
  if (sessionRunning() || millis() < flashUntil) { lastActive = now; screen = S_FOCUS; }

  if (asleep) { delay(6); return; }

  // the story turns its own pages, since one knock means next screen
  if (screen == S_STORY && storyLines > LINES_PER_PAGE && now - lastPageTurn > 5000) {
    lastPageTurn = now;
    int pages = (storyLines + LINES_PER_PAGE - 1) / LINES_PER_PAGE;
    storyPage = (storyPage + 1) % pages;
  }

  if (now - lastDraw >= 110) {
    lastDraw = now;
    switch (screen) {
      case S_FOCUS:    drawFocus();    break;
      case S_WEATHER:  drawWeather();  break;
      case S_PRAYER:   drawPrayer();   break;
      case S_MSG:      drawMessage();  break;
      case S_STORY:    drawStory();    break;
      case S_SYSTEM:   drawSystem();   break;
      case S_SETTINGS: drawSettings(); break;
      default:         drawHome();     break;
    }
  }
  delay(2);
}

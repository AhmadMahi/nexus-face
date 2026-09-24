/*
  ================================================================
   NEXUS FACE  -  ESP32-C3 companion
  ================================================================
   Everything is driven by knocking on it.

     1 knock ...... next thing on this screen
     2 knocks ..... next screen
     3 knocks ..... do the thing you have selected
     4 knocks ..... straight back to HOME

   SCREENS   HOME, CLOCK, WEATHER, MESSAGES, FACE, SENSORS,
             SETTINGS, SYSTEM

   Leave it 30 seconds and the panel powers down and the processor
   throttles back. Pick it up, shake it or knock it to bring it back.

   WIRING   everything on one I2C bus
     SDA GPIO8   SCL GPIO9
     OLED 0x3C   ADXL345 0x53   MPU6050 0x68
   Both sensors are optional; it uses whichever answers.

   NETWORK  joins your WiFi and runs its own hotspot at the same
            time, so the panel is always reachable.
              hotspot  NEXUS-ROBOT / password
              panel    http://192.168.4.1

   UPDATES  SETTINGS > Check for update pulls the newest release
            from GitHub and installs it over the air.

   LIBRARIES  FluxGarage RoboEyes, Adafruit GFX, Adafruit SSD1306
   Board      ESP32-C3.  Partition: Minimal SPIFFS (1.9MB APP)
  ================================================================
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <Preferences.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <FluxGarage_RoboEyes.h>

#define SDA_PIN 8
#define SCL_PIN 9
#define OLED_ADDR 0x3C
// RoboEyes already uses W and H, so the screen size lives under other names
#define SCRW 128
#define SCRH 64

#define FW_VERSION "1.0.0"
#define OTA_REPO   "AhmadMahi/nexus-face"
#define OTA_ASSET  "nexus_face.bin"

#define DEF_WIFI_SSID "YOUR_WIFI_NAME"
#define DEF_WIFI_PASS "YOUR_WIFI_PASSWORD"
#define DEF_TZ        "IST-5:30"

const char* AP_SSID = "NEXUS-ROBOT";
const char* AP_PASS = "password";

Adafruit_SSD1306 oled(SCRW, SCRH, &Wire, -1);
RoboEyes<Adafruit_SSD1306> eyes(oled);
WebServer   web(80);
Preferences prefs;

// ---------------- sensor registers ----------------
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
enum { S_HOME = 0, S_CLOCK, S_WEATHER, S_MSG, S_FACE, S_SENSORS, S_SETTINGS, S_SYSTEM, S_COUNT };
const char* S_NAME[S_COUNT] =
  { "HOME", "CLOCK", "WEATHER", "MESSAGES", "FACE", "SENSORS", "SETTINGS", "SYSTEM" };

int screen = S_HOME;
int itemIdx = 0;                  // which row is selected on this screen

// ---------------- settings menu ----------------
enum { CFG_BRIGHT = 0, CFG_SLEEP, CFG_EYES, CFG_UPDATE, CFG_REBOOT, CFG_COUNT };
const char* CFG_NAME[CFG_COUNT] =
  { "Brightness", "Sleep after", "Eye style", "Check update", "Reboot" };

struct EyeStyle { const char* name; byte w, h, r; int gap; bool cyc; byte mood; };
const EyeStyle STYLES[] = {
  { "round",   36, 36, 10, 12, false, DEFAULT },
  { "square",  38, 38,  2, 10, false, DEFAULT },
  { "wide",    48, 28, 12,  8, false, DEFAULT },
  { "sleepy",  36, 14,  6, 12, false, TIRED   },
  { "cross",   34, 34,  8, 14, false, ANGRY   },
  { "joy",     36, 36, 16, 12, false, HAPPY   },
  { "cyclops", 46, 46, 14,  0, true,  DEFAULT },
};
const int STYLE_COUNT = sizeof(STYLES) / sizeof(STYLES[0]);

int cfgBright = 160, cfgSleepSec = 30, cfgEyes = 0;
String cfgSsid, cfgPass, cfgTz;

// ---------------- messages, kept in flash ----------------
#define MSG_MAX 8
String msgs[MSG_MAX];
int    msgCount = 0;

// ---------------- weather ----------------
float wTemp = NAN, wHum = NAN, wWind = NAN;
int   wCode = -1;
String wCity = "";
bool  wxOk = false;
unsigned long nextWx = 0;
float locLat = NAN, locLon = NAN;

// ---------------- runtime ----------------
bool asleep = false, screenOn = true, timeOk = false;
unsigned long lastActive = 0, lastDraw = 0, lastPoll = 0, reactUntil = 0, lastShake = 0;
uint32_t cTap = 0, cScreen = 0, cFall = 0, cShake = 0, cBoot = 0;
uint8_t  burst = 0;
unsigned long burstStart = 0;
String   otaStatus = "";
int      otaPct = -1;

#define TAP_WINDOW_MS 520
#define TILT 0.35f
#define SHAKE_G 0.60f

const char* const QUIPS[] = {
  "waiting for the clock", "counting electrons", "time is a construct",
  "ask me again shortly", "no signal, no idea",
};
const int QUIP_COUNT = sizeof(QUIPS) / sizeof(QUIPS[0]);

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
    // single taps only: we count them ourselves, which is the only way
    // to tell three knocks from two
    wReg(adxl, A_INT_ENABLE, INT_TAP1 | INT_FF);
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
  Serial.printf("ADXL345 %s   MPU6050 %s\n", adxl ? "ok" : "absent", mpu ? "ok" : "absent");
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
//  MESSAGES, kept across reboots
// ================================================================
static void loadMessages() {
  msgCount = prefs.getInt("mn", 0);
  if (msgCount > MSG_MAX) msgCount = MSG_MAX;
  char k[6];
  for (int i = 0; i < msgCount; i++) {
    snprintf(k, sizeof(k), "m%d", i);
    msgs[i] = prefs.getString(k, "");
  }
}
static void saveMessages() {
  prefs.putInt("mn", msgCount);
  char k[6];
  for (int i = 0; i < msgCount; i++) {
    snprintf(k, sizeof(k), "m%d", i);
    prefs.putString(k, msgs[i]);
  }
}
static void addMessage(const String& m) {
  for (int i = MSG_MAX - 1; i > 0; i--) msgs[i] = msgs[i - 1];
  msgs[0] = m;
  if (msgCount < MSG_MAX) msgCount++;
  saveMessages();
}

// ================================================================
//  DRAWING  -  one layout for every screen, so nothing jumps about
//    y 0..8    header: screen name left, clock and signal right
//    y 10      rule
//    y 14..63  content
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

static void header(const char* title) {
  at(2, 0, title);
  int bars = 0;
  if (WiFi.status() == WL_CONNECTED) {
    int r = WiFi.RSSI();
    bars = r > -55 ? 4 : r > -67 ? 3 : r > -78 ? 2 : 1;
  }
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2, x = SCRW - 14 + i * 3;
    if (i < bars) oled.fillRect(x, 8 - h, 2, h, SSD1306_WHITE);
    else          oled.drawPixel(x, 7, SSD1306_WHITE);
  }
  char t[8];
  clockStr(t, sizeof(t), false);
  oled.setTextSize(1);
  oled.setCursor(SCRW - 18 - 5 * 6, 0);
  oled.print(t);
  oled.drawFastHLine(0, 10, SCRW, SSD1306_WHITE);
}

// ---- weather glyphs ----
static void wxSun(int x, int y) {
  oled.fillCircle(x + 9, y + 9, 5, SSD1306_WHITE);
  for (int i = 0; i < 8; i++) {
    float a = i * 0.7854f;
    oled.drawLine(x + 9 + cosf(a) * 7, y + 9 + sinf(a) * 7,
                  x + 9 + cosf(a) * 9, y + 9 + sinf(a) * 9, SSD1306_WHITE);
  }
}
static void wxCloud(int x, int y) {
  oled.fillCircle(x + 6, y + 11, 4, SSD1306_WHITE);
  oled.fillCircle(x + 12, y + 9, 5, SSD1306_WHITE);
  oled.fillRect(x + 6, y + 10, 8, 5, SSD1306_WHITE);
}
static void wxRain(int x, int y) {
  wxCloud(x, y - 3);
  for (int i = 0; i < 3; i++) oled.drawLine(x + 4 + i * 4, y + 13, x + 3 + i * 4, y + 17, SSD1306_WHITE);
}
static void wxSnow(int x, int y) {
  wxCloud(x, y - 3);
  for (int i = 0; i < 3; i++) oled.drawCircle(x + 4 + i * 4, y + 15, 1, SSD1306_WHITE);
}
static void wxStorm(int x, int y) {
  wxCloud(x, y - 3);
  oled.drawLine(x + 10, y + 12, x + 7, y + 17, SSD1306_WHITE);
  oled.drawLine(x + 7, y + 17, x + 11, y + 16, SSD1306_WHITE);
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

// ================================================================
//  SCREENS
// ================================================================
static void drawHome() {
  oled.clearDisplay();
  header("HOME");
  char t[10], l[24];
  clockStr(t, sizeof(t), false);
  oled.setTextSize(3);
  oled.setCursor((SCRW - (int)strlen(t) * 18) / 2, 16);
  oled.print(t);

  struct tm tm0;
  if (timeOk && getLocalTime(&tm0, 5)) strftime(l, sizeof(l), "%a %d %b", &tm0);
  else snprintf(l, sizeof(l), "%s", QUIPS[(millis() / 4000) % QUIP_COUNT]);
  ctr(l, 42, 1);

  if (wxOk) snprintf(l, sizeof(l), "%d C  %s", (int)roundf(wTemp), wxWord(wCode));
  else      snprintf(l, sizeof(l), "%d msg  %lu taps", msgCount, (unsigned long)cTap);
  ctr(l, 54, 1);
  oled.display();
}

static void drawClock() {
  struct tm t;
  bool ok = timeOk && getLocalTime(&t, 5);
  oled.clearDisplay();
  header("CLOCK");

  char big[8], sec[4], date[22];
  if (ok) {
    snprintf(big, sizeof(big), "%02d:%02d", t.tm_hour, t.tm_min);
    snprintf(sec, sizeof(sec), "%02d", t.tm_sec);
    strftime(date, sizeof(date), "%A %d %B", &t);
  } else {
    strcpy(big, "--:--"); strcpy(sec, "--");
    snprintf(date, sizeof(date), "%s", QUIPS[(millis() / 4000) % QUIP_COUNT]);
  }
  int bw = 5 * 18;
  oled.setTextSize(3);
  oled.setCursor((SCRW - bw - 16) / 2, 18);
  oled.print(big);
  at((SCRW - bw - 16) / 2 + bw + 5, 34, sec);
  ctr(date, 50, 1);
  oled.display();
}

static void drawWeather() {
  oled.clearDisplay();
  header("WEATHER");
  if (!wxOk) {
    ctr(WiFi.status() == WL_CONNECTED ? "fetching..." : "needs the internet", 28, 1);
    ctr(wCity.length() ? wCity.c_str() : "locating", 44, 1);
    oled.display();
    return;
  }
  wxIcon(wCode, 4, 16);
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

static void drawMessages() {
  oled.clearDisplay();
  header("MESSAGES");
  if (!msgCount) {
    ctr("nothing yet", 28, 1);
    ctr("send one from the app", 44, 1);
    oled.display();
    return;
  }
  char c[10];
  snprintf(c, sizeof(c), "%d/%d", itemIdx + 1, msgCount);
  rightAt(0, c);

  // tilt steers the text, exactly as before
  int align = ax > TILT ? 1 : (ax < -TILT ? -1 : 0);
  int drop  = ay > TILT ? 1 : (ay < -TILT ? -1 : 0);

  const int PER = 20, MAXL = 3;
  String m = msgs[itemIdx], line[MAXL];
  int n = 0;
  for (int i = 0; n < MAXL && i < (int)m.length(); ) {
    int take = min(PER, (int)m.length() - i);
    if (take == PER) { int sp = m.lastIndexOf(' ', i + take); if (sp > i + 5) take = sp - i; }
    line[n++] = m.substring(i, i + take);
    i += take;
    while (i < (int)m.length() && m.charAt(i) == ' ') i++;
  }
  int lowest = SCRH - 2 - (n - 1) * 11 - 7;
  int top = constrain(18 + drop * 10, 14, max(14, lowest));
  for (int k = 0; k < n; k++) {
    int lw = line[k].length() * 6;
    int x = (SCRW - lw) / 2;
    if (align < 0) x = 4;
    if (align > 0) x = SCRW - lw - 4;
    at(x, top + k * 11, line[k].c_str());
  }
  oled.display();
}

static void drawSensors() {
  oled.clearDisplay();
  header("SENSORS");
  const int BX = 4, BY = 15, BW = 44, BH = 44;
  oled.drawRect(BX, BY, BW, BH, SSD1306_WHITE);
  int cx = BX + BW / 2, cy = BY + BH / 2;
  oled.drawFastHLine(cx - 3, cy, 7, SSD1306_WHITE);
  oled.drawFastVLine(cx, cy - 3, 7, SSD1306_WHITE);
  int dx = constrain((int)(ax * (BW / 2 - 5)), -(BW / 2 - 5), BW / 2 - 5);
  int dy = constrain((int)(ay * (BH / 2 - 5)), -(BH / 2 - 5), BH / 2 - 5);
  oled.fillCircle(cx + dx, cy + dy, 4, SSD1306_WHITE);

  char l[18];
  snprintf(l, sizeof(l), "X%+5.2f", ax); at(54, 16, l);
  snprintf(l, sizeof(l), "Y%+5.2f", ay); at(54, 26, l);
  snprintf(l, sizeof(l), "Z%+5.2f", az); at(54, 36, l);
  snprintf(l, sizeof(l), "%.2f g", amag); at(54, 46, l);
  snprintf(l, sizeof(l), "%s", mpu ? "2 sensors" : "1 sensor"); at(54, 56, l);
  oled.display();
}

static void drawSystem() {
  oled.clearDisplay();
  header("SYSTEM");
  char l[26];
  uint32_t heap = ESP.getFreeHeap(), tot = ESP.getHeapSize();
  snprintf(l, sizeof(l), "fw %s", FW_VERSION);      at(4, 15, l);
  snprintf(l, sizeof(l), "ram %uk/%uk", (unsigned)(heap / 1024), (unsigned)(tot / 1024));
  at(4, 25, l);
  int bw = SCRW - 8, fill = bw * (tot - heap) / tot;
  oled.drawRect(4, 34, bw, 5, SSD1306_WHITE);
  if (fill > 2) oled.fillRect(5, 35, fill - 2, 3, SSD1306_WHITE);
  snprintf(l, sizeof(l), "up %lus  boots %lu",
           (unsigned long)(millis() / 1000UL), (unsigned long)cBoot);
  at(4, 43, l);
  snprintf(l, sizeof(l), "%s", WiFi.status() == WL_CONNECTED
           ? WiFi.localIP().toString().c_str() : "192.168.4.1");
  at(4, 53, l);
  oled.display();
}

// a proper list, one row per setting, value on the right
static void drawSettings() {
  oled.clearDisplay();
  header("SETTINGS");
  char v[18];
  for (int i = 0; i < CFG_COUNT; i++) {
    int y = 14 + i * 10;
    if (i == itemIdx) {
      oled.fillRect(0, y - 1, SCRW, 10, SSD1306_WHITE);
      oled.setTextColor(SSD1306_BLACK);
    } else {
      oled.setTextColor(SSD1306_WHITE);
    }
    at(4, y, CFG_NAME[i]);
    switch (i) {
      case CFG_BRIGHT: snprintf(v, sizeof(v), "%d", cfgBright); break;
      case CFG_SLEEP:  snprintf(v, sizeof(v), "%ds", cfgSleepSec); break;
      case CFG_EYES:   snprintf(v, sizeof(v), "%s", STYLES[cfgEyes].name); break;
      case CFG_UPDATE: snprintf(v, sizeof(v), "%s", FW_VERSION); break;
      default:         snprintf(v, sizeof(v), "x3"); break;
    }
    oled.setCursor(SCRW - 3 - (int)strlen(v) * 6, y);
    oled.print(v);
  }
  oled.setTextColor(SSD1306_WHITE);
  oled.display();
}

// shown while an update is running
static void drawOta() {
  oled.clearDisplay();
  header("UPDATE");
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
    // a little spinner while it is still thinking
    int a = (millis() / 120) % 8;
    for (int i = 0; i < 8; i++) {
      float th = i * 0.7854f;
      int r = (i == a) ? 8 : 5;
      oled.drawPixel(SCRW / 2 + cosf(th) * r, 42 + sinf(th) * r, SSD1306_WHITE);
    }
  }
  oled.display();
}

// ================================================================
//  NETWORK HELPERS
// ================================================================
static bool httpGetTo(const String& url, bool tls, String& out, int ms) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient h;
  h.setConnectTimeout(ms); h.setTimeout(ms);
  h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  h.setUserAgent("nexus-face");
  bool ok = false;
  if (tls) { WiFiClientSecure c; c.setInsecure();
             if (h.begin(c, url) && h.GET() == 200) { out = h.getString(); ok = true; } }
  else     { WiFiClient c;
             if (h.begin(c, url) && h.GET() == 200) { out = h.getString(); ok = true; } }
  h.end();
  return ok;
}

// Open-Meteo repeats every field name inside "current_units" with the unit
// as a string, so a naive search finds "C" instead of the number. Scope the
// search to the "current" object and the values come out right.
static float curNum(const String& body, const char* key, float def) {
  int c = body.indexOf("\"current\":{");
  if (c < 0) return def;
  int i = body.indexOf(String("\"") + key + "\":", c);
  if (i < 0) return def;
  return body.substring(i + strlen(key) + 3).toFloat();
}

static void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  String body;
  if (isnan(locLat)) {
    if (httpGetTo("http://ip-api.com/json/?fields=status,city,lat,lon", false, body, 6000)) {
      int i = body.indexOf("\"lat\":");
      if (i >= 0) locLat = body.substring(i + 6).toFloat();
      i = body.indexOf("\"lon\":");
      if (i >= 0) locLon = body.substring(i + 6).toFloat();
      i = body.indexOf("\"city\":\"");
      if (i >= 0) { int e = body.indexOf('"', i + 8); wCity = body.substring(i + 8, e); }
      if (wCity.length() > 13) wCity = wCity.substring(0, 13);
      Serial.printf("located %s  %.3f %.3f\n", wCity.c_str(), locLat, locLon);
    }
    if (isnan(locLat) || locLat == 0) return;
  }
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(locLat, 3) +
               "&longitude=" + String(locLon, 3) +
               "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m";
  if (!httpGetTo(url, true, body, 8000)) return;
  wTemp = curNum(body, "temperature_2m", NAN);
  wHum  = curNum(body, "relative_humidity_2m", NAN);
  wWind = curNum(body, "wind_speed_10m", NAN);
  wCode = (int)curNum(body, "weather_code", -1);
  wxOk  = !isnan(wTemp);
  Serial.printf("weather %.1fC %.0f%% wind %.1f code %d\n", wTemp, wHum, wWind, wCode);
}

// ================================================================
//  OVER THE AIR UPDATE
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
  if (WiFi.status() != WL_CONNECTED) { otaStatus = "no network"; drawOta(); delay(1800); return; }

  otaStatus = "checking"; otaPct = -1; drawOta();

  String body;
  if (!httpGetTo("https://api.github.com/repos/" OTA_REPO "/releases/latest", true, body, 8000)) {
    otaStatus = "github unreachable"; drawOta(); delay(2000); return;
  }
  int i = body.indexOf("\"tag_name\":\"");
  String tag = i < 0 ? "" : body.substring(i + 12, body.indexOf('"', i + 12));
  int a = body.indexOf(OTA_ASSET);
  int u = a < 0 ? -1 : body.indexOf("\"browser_download_url\":\"", a);
  String url = u < 0 ? "" : body.substring(u + 24, body.indexOf('"', u + 24));

  if (!tag.length() || !url.length()) { otaStatus = "no release yet"; drawOta(); delay(2000); return; }
  if (verNum(tag) <= verNum(FW_VERSION)) {
    otaStatus = "already newest"; drawOta(); delay(1800); return;
  }

  otaStatus = tag; otaPct = 0; drawOta();

  WiFiClientSecure c; c.setInsecure();
  HTTPClient h;
  h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  h.setConnectTimeout(15000); h.setTimeout(20000);
  h.setUserAgent("nexus-face");
  if (!h.begin(c, url) || h.GET() != 200) { otaStatus = "download failed"; drawOta(); delay(2000); h.end(); return; }
  int len = h.getSize();
  if (len <= 0 || !Update.begin(len)) { otaStatus = "no room"; drawOta(); delay(2000); h.end(); return; }
  Update.onProgress(otaProgress);
  size_t wrote = Update.writeStream(*h.getStreamPtr());
  h.end();
  if (wrote != (size_t)len || !Update.end(true)) {
    otaStatus = "install failed"; otaPct = -1; drawOta(); delay(2200); return;
  }
  otaStatus = "installed"; otaPct = 100; drawOta();
  delay(1400);
  ESP.restart();
}

// ================================================================
//  SLEEP
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
  cfgEyes = (i + STYLE_COUNT) % STYLE_COUNT;
  const EyeStyle& e = STYLES[cfgEyes];
  eyes.setCyclops(e.cyc);
  eyes.setWidth(e.w, e.w); eyes.setHeight(e.h, e.h);
  eyes.setBorderradius(e.r, e.r); eyes.setSpacebetween(e.gap);
  eyes.setMood(e.mood);
}
static void goSleep() {
  if (asleep) return;
  asleep = true;
  eyes.setIdleMode(OFF); eyes.setAutoblinker(OFF);
  eyes.setMood(TIRED); eyes.close();
  for (int i = 0; i < 26; i++) { eyes.update(); delay(16); }
  screenPower(false);
  setCpuFrequencyMhz(80);
  Serial.println("asleep");
}
static void wake(const char* why) {
  lastActive = millis();
  if (!asleep) return;
  asleep = false;
  setCpuFrequencyMhz(160);
  screenPower(true);
  eyes.setAutoblinker(ON, 3, 2); eyes.setIdleMode(ON, 2, 2);
  applyEyes(cfgEyes); eyes.open();
  screen = S_HOME; itemIdx = 0;
  Serial.printf("awake (%s)\n", why);
}

// ================================================================
//  KNOCKS
//    1 next item   2 next screen   3 activate   4 home
// ================================================================
static void react(unsigned long ms) { reactUntil = millis() + ms; }

static int itemsOn(int s) {
  if (s == S_SETTINGS) return CFG_COUNT;
  if (s == S_MSG)      return max(1, msgCount);
  return 1;
}

static void knockNext() {                       // 1
  cTap++;
  itemIdx = (itemIdx + 1) % itemsOn(screen);
}

static void knockScreen() {                     // 2
  cScreen++;
  screen = (screen + 1) % S_COUNT;
  itemIdx = 0;
  if (screen == S_FACE) { applyEyes(cfgEyes); eyes.open(); }
}

static void knockDo() {                         // 3
  if (screen == S_SETTINGS) {
    switch (itemIdx) {
      case CFG_BRIGHT:
        cfgBright += 45; if (cfgBright > 255) cfgBright = 25;
        applyBright(); prefs.putInt("bri", cfgBright); break;
      case CFG_SLEEP:
        cfgSleepSec = cfgSleepSec >= 120 ? 15 : cfgSleepSec * 2;
        prefs.putInt("slp", cfgSleepSec); break;
      case CFG_EYES:
        applyEyes(cfgEyes + 1); prefs.putInt("eye", cfgEyes); break;
      case CFG_UPDATE:
        runUpdate(); break;
      default:
        delay(150); ESP.restart();
    }
  } else if (screen == S_FACE) {
    eyes.anim_laugh(); react(1500);
  } else if (screen == S_WEATHER) {
    nextWx = 0;                                  // refresh now
  } else if (screen == S_MSG && msgCount) {
    msgCount = 0; itemIdx = 0; saveMessages();   // clear the lot
  }
}

static void knockHome() {                       // 4
  screen = S_HOME; itemIdx = 0;
}

static void onFall() {
  cFall++;
  screen = S_FACE; itemIdx = 0;
  eyes.setMood(DEFAULT); eyes.setPosition(N); eyes.setVFlicker(ON, 6);
  for (int i = 0; i < 10; i++) { eyes.update(); delay(16); }
  eyes.setPosition(S);
  for (int i = 0; i < 10; i++) { eyes.update(); delay(16); }
  eyes.setVFlicker(OFF); eyes.setHeight(6, 6);
  react(2000);
}

static void settleBurst() {
  if (!burst || millis() - burstStart < TAP_WINDOW_MS) return;
  uint8_t n = burst;
  burst = 0;
  switch (n) {
    case 1: knockNext();   break;
    case 2: knockScreen(); break;
    case 3: knockDo();     break;
    default: knockHome();  break;
  }
  Serial.printf("knock x%u -> %s [%d]\n", n, S_NAME[screen], itemIdx);
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
      if (burst < 4) burst++;
      lastActive = now;
    }
  }
  settleBurst();

  if (fabsf(amag - 1.0f) > SHAKE_G && now - lastShake > 600) {
    lastShake = now; cShake++;
    if (asleep) { wake("shake"); return; }
    if (screen == S_FACE) {
      eyes.setMood(ANGRY); eyes.setHFlicker(ON, 4); eyes.anim_confused(); react(1600);
    }
    lastActive = now;
    return;
  }
  if (fabsf(amag - 1.0f) > 0.12f || fabsf(gxr) + fabsf(gyr) + fabsf(gzr) > 25.0f) {
    if (asleep) wake("picked up");
    lastActive = now;
  }
  if (asleep) return;
  if (now - lastActive > (unsigned long)cfgSleepSec * 1000UL) { goSleep(); return; }
  if (now < reactUntil) return;

  if (screen == S_FACE) {
    if      (ax >  TILT && ay >  TILT) eyes.setPosition(SE);
    else if (ax >  TILT && ay < -TILT) eyes.setPosition(NE);
    else if (ax < -TILT && ay >  TILT) eyes.setPosition(SW);
    else if (ax < -TILT && ay < -TILT) eyes.setPosition(NW);
    else if (ax >  TILT)               eyes.setPosition(E);
    else if (ax < -TILT)               eyes.setPosition(W);
    else if (ay >  TILT)               eyes.setPosition(S);
    else if (ay < -TILT)               eyes.setPosition(N);
    else                               eyes.setPosition(DEFAULT);
  }
}

// ================================================================
//  WEB PANEL
// ================================================================
const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Nexus</title><style>
:root{--bg:#070d13;--card:#101c27;--fg:#e6eef5;--mut:#7d93a6;--line:#1e2f3d;--acc:#2dd4bf}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;text-align:center}
.wrap{max-width:440px;margin:0 auto;padding:20px}
h1{font-size:12px;letter-spacing:.34em;color:var(--acc);margin:0}
.clock{font-size:44px;font-weight:200;margin:2px 0 0;font-variant-numeric:tabular-nums}
.sub{color:var(--mut);font-size:12px;letter-spacing:.12em;text-transform:uppercase}
h2{font-size:11px;letter-spacing:.2em;color:var(--mut);margin:22px 0 8px;text-transform:uppercase}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px;margin-bottom:8px}
.g4{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}
.tile{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:12px 4px}
.tile b{display:block;font-size:19px;font-weight:500;font-variant-numeric:tabular-nums}
.tile span{font-size:10px;color:var(--mut);letter-spacing:.06em}
input{width:100%;padding:12px;border-radius:10px;border:1px solid var(--line);background:#0b141c;color:var(--fg);font:inherit;text-align:center}
button{font:inherit;font-weight:600;padding:11px;border:0;border-radius:10px;background:var(--acc);color:#04201c;cursor:pointer;width:100%;margin-top:8px}
button.g{background:transparent;color:var(--fg);border:1px solid var(--line)}
.row{display:flex;gap:8px}.row button{margin-top:0}
table{width:100%;font-size:13px;font-variant-numeric:tabular-nums}
td{padding:3px 0}td:first-child{color:var(--mut);text-align:left}td:last-child{text-align:right}
.msg{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:9px 12px;margin-bottom:6px;text-align:left;font-size:14px;word-break:break-word}
.bar{height:6px;background:#0b141c;border-radius:3px;overflow:hidden;margin-top:8px}
.bar i{display:block;height:100%;background:var(--acc)}
#t{margin-top:10px;font-size:13px;color:var(--acc);min-height:18px}
</style></head><body><div class="wrap">
<h1>N E X U S</h1>
<div class="clock" id="clk">--:--</div>
<div class="sub" id="sub">connecting</div>

<h2>Say something</h2>
<div class="card">
  <input id="m" maxlength="72" placeholder="type a message">
  <button onclick="send()">Send</button>
</div>

<h2>Screens</h2>
<div class="card"><div class="row">
  <button class="g" onclick="go(0)">Home</button>
  <button class="g" onclick="go(1)">Clock</button>
  <button class="g" onclick="go(2)">Weather</button>
  <button class="g" onclick="go(3)">Msgs</button>
</div><div class="row" style="margin-top:8px">
  <button class="g" onclick="go(4)">Face</button>
  <button class="g" onclick="go(5)">Sensors</button>
  <button class="g" onclick="go(6)">Settings</button>
  <button class="g" onclick="go(7)">System</button>
</div></div>

<h2>Activity</h2>
<div class="g4">
  <div class="tile"><b id="c1">0</b><span>KNOCKS</span></div>
  <div class="tile"><b id="c2">0</b><span>SCREENS</span></div>
  <div class="tile"><b id="c3">0</b><span>FALLS</span></div>
  <div class="tile"><b id="c4">0</b><span>SHAKES</span></div>
</div>

<h2>Messages</h2><div class="card" id="ml"></div>
<div class="row"><button class="g" onclick="act('/api/clear')">Clear all</button></div>

<h2>Weather</h2><div class="card"><table id="wx"></table>
  <button class="g" onclick="act('/api/weather')">Refresh</button></div>
<h2>Motion</h2><div class="card"><table id="mot"></table></div>
<h2>System</h2><div class="card"><table id="sys"></table><div class="bar"><i id="hb"></i></div>
  <div class="row" style="margin-top:8px">
    <button class="g" onclick="act('/api/update')">Check update</button>
    <button class="g" onclick="if(confirm('Reboot?'))act('/api/reboot')">Reboot</button>
  </div></div>

<h2>WiFi</h2><div class="card">
  <input id="ssid" placeholder="network"><div style="height:8px"></div>
  <input id="pass" type="password" placeholder="password (blank keeps it)">
  <button onclick="savewifi()">Save and reboot</button>
</div>
<div id="t"></div>
</div><script>
const $=i=>document.getElementById(i);
window.esc=function(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
window.rows=function(el,o){$(el).innerHTML=Object.entries(o).map(([k,v])=>'<tr><td>'+k+'</td><td>'+esc(v)+'</td></tr>').join('')}
window.post=async function(u,d){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(d||{})})}
window.act=async function(u){await post(u,{});$('t').textContent='done';load()}
window.go=async function(n){await post('/api/screen',{n:n});$('t').textContent='opened'}
window.send=async function(){const m=$('m').value.trim();if(!m){$('t').textContent='type something';return}
  await post('/api/msg',{m:m});$('m').value='';$('t').textContent='sent';load()}
window.savewifi=async function(){
  const d={ssid:$('ssid').value};if($('pass').value)d.pass=$('pass').value;
  await post('/api/wifi',d);$('t').textContent='saved, rebooting'}
window.pushTime=async function(){const d=new Date();
  await post('/api/time',{e:Math.floor(d.getTime()/1000),o:-d.getTimezoneOffset()})}
let filled=false;
window.load=async function(){
  const s=await(await fetch('/api/state',{cache:'no-store'})).json();
  if(!s.timeOk) await pushTime();
  $('clk').textContent=s.time;
  $('sub').textContent=(s.asleep?'asleep':'awake')+' · '+s.screen+' · fw '+s.fw;
  $('c1').textContent=s.tap;$('c2').textContent=s.scr;$('c3').textContent=s.fall;$('c4').textContent=s.shake;
  $('ml').innerHTML=s.msgs.length?s.msgs.map(m=>'<div class="msg">'+esc(m)+'</div>').join(''):'<span style="color:var(--mut);font-size:13px">nothing stored</span>';
  rows('wx',{'city':s.city,'temperature':s.temp,'humidity':s.hum,'wind':s.wind,'conditions':s.cond});
  rows('mot',{'ADXL X':s.ax,'ADXL Y':s.ay,'ADXL Z':s.az,'gravity':s.amag+' g','MPU temp':s.mtemp+' C','gyro':s.gyro});
  rows('sys',{'free ram':s.heap+' B','used':s.used+' B','uptime':s.up+' s','boots':s.boots,
              'chip':s.chip,'address':s.ip,'clients':s.cl,'firmware':s.fw});
  $('hb').style.width=s.pct+'%';
  if(!filled){$('ssid').value=s.ssid;filled=true}
}
load();setInterval(load,1200);
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
  o += "\"ax\":" + f2(ax, 2) + ",\"ay\":" + f2(ay, 2) + ",\"az\":" + f2(az, 2) + ",\"amag\":" + f2(amag, 2) + ",";
  o += "\"mtemp\":" + f2(mtemp, 1) + ",\"gyro\":\"" + f2(gxr, 0) + " / " + f2(gyr, 0) + " / " + f2(gzr, 0) + "\",";
  o += "\"city\":\"" + wCity + "\",";
  o += "\"temp\":\"" + String(wxOk ? String(wTemp, 1) + " C" : String("--")) + "\",";
  o += "\"hum\":\"" + String(wxOk ? String(wHum, 0) + " %" : String("--")) + "\",";
  o += "\"wind\":\"" + String(wxOk ? String(wWind, 1) + " km/h" : String("--")) + "\",";
  o += "\"cond\":\"" + String(wxWord(wCode)) + "\",";
  o += "\"tap\":" + String(cTap) + ",\"scr\":" + String(cScreen) + ",\"fall\":" + String(cFall) +
       ",\"shake\":" + String(cShake) + ",\"boots\":" + String(cBoot) + ",";
  o += "\"heap\":" + String(heap) + ",\"used\":" + String(tot - heap) +
       ",\"pct\":" + String(100 - heap * 100 / tot) + ",";
  o += "\"up\":" + String(millis() / 1000UL) + ",\"cl\":" + String(WiFi.softAPgetStationNum()) + ",";
  o += "\"chip\":\"" + String(ESP.getChipModel()) + " @" + String(ESP.getCpuFreqMHz()) + "MHz\",";
  o += "\"ip\":\"" + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "\",";
  o += "\"ssid\":\"" + cfgSsid + "\",\"msgs\":[";
  for (int i = 0; i < msgCount; i++) {
    String m = msgs[i]; m.replace("\\", " "); m.replace("\"", "'");
    o += "\"" + m + "\"";
    if (i < msgCount - 1) o += ",";
  }
  o += "]}";
  web.send(200, "application/json", o);
}

static void setupWeb() {
  web.on("/", HTTP_GET, []() { web.send_P(200, "text/html; charset=utf-8", PAGE); });
  web.on("/api/state", HTTP_GET, apiState);
  web.on("/api/msg", HTTP_POST, []() {
    String m = web.arg("m"); m.trim();
    if (m.length()) { addMessage(m.substring(0, 72)); screen = S_MSG; itemIdx = 0; wake("message"); }
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/screen", HTTP_POST, []() {
    screen = constrain((int)web.arg("n").toInt(), 0, S_COUNT - 1);
    itemIdx = 0; wake("panel");
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/clear", HTTP_POST, []() {
    msgCount = 0; itemIdx = 0; saveMessages();
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/weather", HTTP_POST, []() {
    nextWx = 0;
    web.send(200, "application/json", "{\"ok\":true}");
  });
  // your phone knows the time even when we cannot reach a time server
  web.on("/api/time", HTTP_POST, []() {
    long e = web.arg("e").toInt();
    int  o = web.arg("o").toInt();
    if (e > 1700000000L) {
      struct timeval tv = { .tv_sec = (time_t)e, .tv_usec = 0 };
      settimeofday(&tv, nullptr);
      char tz[24];
      snprintf(tz, sizeof(tz), "UTC%+d:%02d", -o / 60, abs(o) % 60);
      setenv("TZ", tz, 1); tzset();
      timeOk = true;
    }
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/wifi", HTTP_POST, []() {
    String s = web.arg("ssid"); s.trim();
    if (s.length()) prefs.putString("ssid", s);
    if (web.arg("pass").length()) prefs.putString("pass", web.arg("pass"));
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
  web.onNotFound([]() {
    web.sendHeader("Location", "http://192.168.4.1/", true);
    web.send(302, "text/plain", "");
  });
  web.begin();
}

// ================================================================
//  BOOT
// ================================================================
static void splash(const char* a, const char* b) {
  oled.clearDisplay();
  oled.setTextSize(2);
  oled.setCursor((SCRW - 60) / 2, 14);
  oled.print("NEXUS");
  ctr(a, 36, 1);
  if (b) ctr(b, 48, 1);
  oled.display();
}

void setup() {
  Serial.begin(115200);
  delay(300);

  prefs.begin("nexus", false);
  cBoot = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", cBoot);
  cfgBright   = constrain(prefs.getInt("bri", 160), 10, 255);
  cfgSleepSec = constrain(prefs.getInt("slp", 30), 10, 3600);
  cfgEyes     = constrain(prefs.getInt("eye", 0), 0, STYLE_COUNT - 1);
  cfgTz       = prefs.getString("tz", DEF_TZ);
  cfgSsid     = prefs.getString("ssid", "");
  cfgPass     = prefs.getString("pass", "");

  // First run: take the credentials compiled in and write them to flash.
  // From then on they live in flash, so an update can never wipe them.
  if (!cfgSsid.length() && strcmp(DEF_WIFI_SSID, "YOUR_WIFI_NAME") != 0) {
    cfgSsid = DEF_WIFI_SSID; cfgPass = DEF_WIFI_PASS;
    prefs.putString("ssid", cfgSsid);
    prefs.putString("pass", cfgPass);
    Serial.println("wifi seeded from the sketch into flash");
  }
  loadMessages();

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  // last argument stops Adafruit calling Wire.begin() on the default pins
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) { Serial.println("no OLED"); return; }
  oled.setTextWrap(false);
  oled.setTextColor(SSD1306_WHITE);
  applyBright();

  splash("starting", nullptr);
  startSensors();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  splash(AP_SSID, "pass: password");
  delay(1300);

  if (cfgSsid.length()) {
    splash("joining", cfgSsid.c_str());
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(120);
  }

  if (WiFi.status() == WL_CONNECTED) {
    splash("getting the time", WiFi.localIP().toString().c_str());
    configTzTime(cfgTz.c_str(), "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    struct tm tm0;
    unsigned long t0 = millis();
    while (!getLocalTime(&tm0, 200) && millis() - t0 < 8000) delay(100);
    timeOk = getLocalTime(&tm0, 200);
  } else {
    splash("no network", "hotspot only");
    delay(1200);
  }

  setupWeb();

  eyes.begin(SCRW, SCRH, 50);
  applyEyes(cfgEyes);
  eyes.setAutoblinker(ON, 3, 2);
  eyes.setIdleMode(ON, 2, 2);

  splash("1 next  2 screen", "3 do    4 home");
  delay(1800);

  screen = S_HOME; itemIdx = 0;
  lastActive = millis();
  Serial.printf("up. fw %s boot #%lu\n", FW_VERSION, (unsigned long)cBoot);
}

void loop() {
  web.handleClient();
  unsigned long now = millis();

  if (now - lastPoll >= 45) { lastPoll = now; input(); }

  if (!asleep && WiFi.status() == WL_CONNECTED && (long)(now - nextWx) >= 0) {
    nextWx = now + 900000UL;
    fetchWeather();
  }

  if (asleep) { delay(6); return; }

  if (screen == S_FACE) {
    eyes.update();
  } else if (now - lastDraw >= 110) {
    lastDraw = now;
    switch (screen) {
      case S_CLOCK:    drawClock();    break;
      case S_WEATHER:  drawWeather();  break;
      case S_MSG:      drawMessages(); break;
      case S_SENSORS:  drawSensors();  break;
      case S_SETTINGS: drawSettings(); break;
      case S_SYSTEM:   drawSystem();   break;
      default:         drawHome();     break;
    }
  }
  delay(2);
}

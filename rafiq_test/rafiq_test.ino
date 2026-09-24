/*
  ================================================================
   RAFIQ  -  input test build
  ================================================================
   This is a scratch build, not the real firmware. It carries only
   what is needed to try three ways of driving the device and to get
   an update back down afterwards:

     WiFi, the clock, the web page, and OTA.

   Everything else is deliberately gone.

   IT WILL ALWAYS INSTALL THE LATEST RELEASE, even one numbered
   lower than itself. That is on purpose: it is the way back to the
   real firmware, and a version check would otherwise refuse it.

   Board ESP32-C3.  Partition: Minimal SPIFFS (1.9MB APP with OTA)
  ================================================================
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <Preferences.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SDA_PIN 8
#define SCL_PIN 9
#define OLED_ADDR 0x3C
#define SCRW 128
#define SCRH 64

#define FW_VERSION "1.7.1-test"
#define OTA_REPO   "AhmadMahi/nexus-face"
#define OTA_ASSET  "nexus_face.bin"
#define DEF_TZ     "IST-5:30"

Adafruit_SSD1306 oled(SCRW, SCRH, &Wire, -1);
WebServer   web(80);
Preferences prefs;

// ---------------- sensors ----------------
#define A_DEVID 0x00
#define A_THRESH_TAP 0x1D
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
#define M_WHOAMI 0x75
#define M_PWR1 0x6B
#define M_GYRO_CFG 0x1B
#define M_ACC_CFG 0x1C
#define M_ACCEL_H 0x3B

uint8_t adxl = 0, mpu = 0;
float ax, ay, az, amag = 1, gx = 0, gy = 0, gz = 0, gmag = 0;

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
    wReg(adxl, A_INT_ENABLE, INT_TAP1);
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
    gx = (int16_t)((b[8] << 8) | b[9]) / 131.0f;
    gy = (int16_t)((b[10] << 8) | b[11]) / 131.0f;
    gz = (int16_t)((b[12] << 8) | b[13]) / 131.0f;
    gmag = fabsf(gx) + fabsf(gy) + fabsf(gz);
    if (!adxl) {
      ax = (int16_t)((b[0] << 8) | b[1]) / 16384.0f;
      ay = (int16_t)((b[2] << 8) | b[3]) / 16384.0f;
      az = (int16_t)((b[4] << 8) | b[5]) / 16384.0f;
      amag = sqrtf(ax * ax + ay * ay + az * az);
    }
  }
}

// ---------------- drawing ----------------
static void ctr(const char* s, int y, int size) {
  int w = (int)strlen(s) * 6 * size;
  oled.setTextSize(size);
  oled.setCursor(w < SCRW ? (SCRW - w) / 2 : 0, y);
  oled.print(s);
}
static void at(int x, int y, const char* s, int size = 1) {
  oled.setTextSize(size); oled.setCursor(x, y); oled.print(s);
}
static void bar(const char* title, const char* right) {
  oled.fillRect(0, 0, SCRW, 11, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  oled.setTextSize(1);
  oled.setCursor(3, 2); oled.print(title);
  if (right && *right) { oled.setCursor(SCRW - 3 - (int)strlen(right) * 6, 2); oled.print(right); }
  oled.setTextColor(SSD1306_WHITE);
}
static void barC(const char* title) {
  oled.fillRect(0, 0, SCRW, 11, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  oled.setTextSize(1);
  oled.setCursor((SCRW - (int)strlen(title) * 6) / 2, 2); oled.print(title);
  oled.setTextColor(SSD1306_WHITE);
}

// ---------------- state ----------------
enum { M_CAL = 0, M_MENU, M_TILT, M_SQUEEZE, M_TAPS, M_RESULT };
int mode = M_CAL;

enum { CAL_HOLD = 0, CAL_UP, CAL_DOWN, CAL_LEFT, CAL_RIGHT, CAL_SHOW, CAL_DONE };
int calStep = CAL_HOLD;
unsigned long calStamp = 0;
float calAcc[3] = { 0, 0, 0 };
int   calN = 0, gravAx = 2;
int8_t axUD = -1, sgUD = 1, axLR = -1, sgLR = 1;
float restV[3] = { 0, 0, 0 };
bool  calOkUp = false, calOkDown = false, calOkLeft = false, calOkRight = false;

String cfgSsid, cfgPass, cfgTz;
bool timeOk = false;
String otaStatus = "", otaStatus2 = "";
int otaPct = -1;

// counters, so you can judge how each one behaves
uint32_t nTapOne = 0, nTapTwo = 0, nTapThree = 0, nTapFour = 0;
uint32_t nUp = 0, nDown = 0, nLeft = 0, nRight = 0, nSqueeze = 0, nSqFalse = 0;
const char* lastGesture = "-";
unsigned long lastGestureAt = 0;

#define TAP_WINDOW_MS 520
uint8_t burst = 0;
unsigned long burstStart = 0;

int menuIdx = 0;
#define MENU_N 4
const char* MENU_NAME[MENU_N] = { "Tilt", "Squeeze", "Taps", "Update" };

int demoSel = 0;
#define DEMO_N 5
const char* DEMO_ITEM[DEMO_N] = { "Brightness", "Sleep after", "Eye style", "Weather", "Reboot" };
String demoPicked = "";
unsigned long demoPickedAt = 0;

static void tiltRead(float& tx, float& ty) {
  float v[3] = { ax, ay, az };
  tx = (axLR >= 0) ? sgLR * (v[axLR] - restV[axLR]) : 0;
  ty = (axUD >= 0) ? sgUD * (v[axUD] - restV[axUD]) : 0;
}
static void note(const char* g) { lastGesture = g; lastGestureAt = millis(); }

// ================================================================
//  NETWORK, CLOCK AND OTA
// ================================================================
static bool online() { return WiFi.status() == WL_CONNECTED; }

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
static bool parseHttpDate(const String& d, struct tm& t) {
  static const char* MON = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[8] = { 0 };
  int dd = 0, yy = 0, hh = 0, mi = 0, ss = 0;
  int c = d.indexOf(' ');
  if (c < 0) return false;
  if (sscanf(d.c_str() + c + 1, "%d %7s %d %d:%d:%d", &dd, mon, &yy, &hh, &mi, &ss) != 6) return false;
  const char* p = strstr(MON, mon);
  if (!p || yy < 2024) return false;
  memset(&t, 0, sizeof(t));
  t.tm_mday = dd; t.tm_mon = (int)(p - MON) / 3; t.tm_year = yy - 1900;
  t.tm_hour = hh; t.tm_min = mi; t.tm_sec = ss;
  return true;
}
static bool timeFromHttp() {
  if (!online()) return false;
  const char* URLS[2] = { "http://www.google.com/generate_204",
                          "http://detectportal.firefox.com/success.txt" };
  for (int i = 0; i < 2; i++) {
    WiFiClient c; HTTPClient h;
    h.setConnectTimeout(5000); h.setTimeout(5000);
    h.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!h.begin(c, URLS[i])) continue;
    const char* want[] = { "Date" };
    h.collectHeaders(want, 1);
    int code = h.GET();
    String d = h.header("Date");
    h.end();
    if (code <= 0 || !d.length()) continue;
    struct tm t;
    if (!parseHttpDate(d, t)) continue;
    setenv("TZ", "UTC0", 1); tzset();
    time_t e = mktime(&t);
    setenv("TZ", cfgTz.c_str(), 1); tzset();
    if (e < 1735689600L) continue;
    struct timeval tv = { .tv_sec = e, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    return true;
  }
  return false;
}
static bool trySyncTime(int waitMs) {
  if (!online()) return false;
  configTzTime(cfgTz.c_str(), "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  struct tm t;
  unsigned long t0 = millis();
  while (millis() - t0 < (unsigned long)waitMs) {
    if (getLocalTime(&t, 120) && t.tm_year > 120) { timeOk = true; return true; }
    delay(30);
  }
  if (timeFromHttp() && getLocalTime(&t, 200) && t.tm_year > 120) { timeOk = true; return true; }
  return false;
}

static void drawOta() {
  oled.clearDisplay();
  barC("UPDATE");
  if (otaStatus2.length()) { ctr(otaStatus.c_str(), 16, 1); ctr(otaStatus2.c_str(), 27, 1); }
  else                       ctr(otaStatus.c_str(), 22, 1);
  if (otaPct >= 0) {
    int bw = SCRW - 24;
    oled.drawRect(12, 38, bw, 9, SSD1306_WHITE);
    int f = (bw - 4) * constrain(otaPct, 0, 100) / 100;
    if (f > 0) oled.fillRect(14, 40, f, 5, SSD1306_WHITE);
    char p[8]; snprintf(p, sizeof(p), "%d%%", otaPct);
    ctr(p, 52, 1);
  } else {
    int a = (millis() / 120) % 8;
    for (int i = 0; i < 8; i++) {
      float th = i * 0.7854f;
      int r = (i == a) ? 9 : 5;
      oled.drawPixel(SCRW / 2 + cosf(th) * r, 46 + sinf(th) * r, SSD1306_WHITE);
    }
  }
  oled.display();
}
static void otaFail(const char* why, const char* detail = "") {
  otaStatus = why; otaStatus2 = detail; otaPct = -1;
  drawOta(); delay(detail[0] ? 4200 : 2400);
  otaStatus2 = "";
  Update.abort();
}

// Deliberately no version comparison. This build is a detour, and the
// way back is whatever release is current, which may well be numbered
// below it. Refusing that would strand the device here.
static void runUpdate() {
  if (!online()) { otaStatus = "No network"; otaPct = -1; drawOta(); delay(1800); return; }
  otaStatus = "Checking"; otaPct = -1; drawOta();

  String b;
  if (!httpGetTo("https://api.github.com/repos/" OTA_REPO "/releases/latest", true, b, 12000)) {
    otaFail("GitHub unreachable"); return;
  }
  int i = b.indexOf("\"tag_name\":\"");
  String tag = i < 0 ? "" : b.substring(i + 12, b.indexOf('"', i + 12));
  int a = b.indexOf(OTA_ASSET);
  int u = a < 0 ? -1 : b.indexOf("\"browser_download_url\":\"", a);
  String url = u < 0 ? "" : b.substring(u + 24, b.indexOf('"', u + 24));
  b = String();
  if (!tag.length() || !url.length()) { otaFail("No release"); return; }
  if (tag == FW_VERSION || tag == String("v" FW_VERSION)) {
    otaStatus = "Already current"; otaStatus2 = tag; otaPct = -1;
    drawOta(); delay(2400); otaStatus2 = ""; return;
  }

  otaStatus = tag; otaPct = 0; drawOta();

  WiFiClientSecure* sec = nullptr;
  HTTPClient* h = nullptr;
  int len = 0; bool open = false;
  for (int hop = 0; hop < 5 && !open; hop++) {
    sec = new WiFiClientSecure();
    if (!sec) { otaFail("Out of memory"); return; }
    sec->setInsecure(); sec->setTimeout(30);
    h = new HTTPClient();
    h->setReuse(false);
    h->setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    h->setConnectTimeout(15000); h->setTimeout(30000);
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
    if (code != 200) { h->end(); delete h; delete sec; otaFail((String("HTTP ") + code).c_str()); return; }
    len = h->getSize();
    open = true;
  }
  if (!open) { otaFail("Too many redirects"); return; }

  const esp_partition_t* slot = esp_ota_get_next_update_partition(NULL);
  if (len <= 0) { h->end(); delete h; delete sec; otaFail("No content length"); return; }
  if (!slot) { h->end(); delete h; delete sec; otaFail("No OTA slot", "Needs min SPIFFS"); return; }
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
        done += got; lastByte = millis();
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
  if (bad || done != (size_t)len) { otaFail(bad ? "Download stalled" : "Download cut short"); return; }
  otaStatus = "Installing"; drawOta();
  if (!Update.end(true)) { otaFail("Install failed"); return; }
  otaStatus = "Installed"; otaPct = 100; drawOta();
  delay(1400);
  ESP.restart();
}

// ================================================================
//  WEB PAGE  (a second way to reach the update, if the knocks fail)
// ================================================================
const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Rafiq test</title><style>
body{margin:0;background:#070d13;color:#e6eef5;font:16px/1.6 system-ui,sans-serif;text-align:center}
.w{max-width:420px;margin:0 auto;padding:20px}
h1{font-size:12px;letter-spacing:.3em;color:#2dd4bf;margin:0}
.v{color:#fbbf24;font-size:13px;letter-spacing:.1em;margin-top:4px}
.c{background:#101c27;border:1px solid #1e2f3d;border-radius:14px;padding:14px;margin-top:14px;text-align:left}
table{width:100%;font-size:13px}td:last-child{text-align:right;color:#7d93a6}
button{font:inherit;font-weight:600;padding:12px;border:0;border-radius:10px;background:#2dd4bf;color:#04201c;width:100%;margin-top:10px;cursor:pointer}
.n{font-size:12px;color:#7d93a6;margin-top:14px;line-height:1.5}
</style></head><body><div class="w">
<h1>R A F I Q</h1><div class="v">INPUT TEST BUILD</div>
<div class="c"><table id="s"></table></div>
<button onclick="up()">Install the latest release</button>
<div class="n">This build always installs whatever release is current,
even one numbered below itself. That is how you get back to the real
firmware.</div>
</div><script>
const $=i=>document.getElementById(i);
window.up=async function(){ if(!confirm('Install the latest release now?'))return;
  await fetch('/api/update',{method:'POST'}); }
window.load=async function(){
  const s=await(await fetch('/api/state',{cache:'no-store'})).json();
  $('s').innerHTML=Object.entries(s).map(([k,v])=>'<tr><td>'+k+'</td><td>'+v+'</td></tr>').join('');
}
load();setInterval(load,1000);
</script></body></html>
)HTML";

static void apiState() {
  char t[10];
  struct tm tmv;
  if (timeOk && getLocalTime(&tmv, 5)) snprintf(t, sizeof(t), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  else strcpy(t, "--:--:--");
  String o = "{";
  o += "\"Firmware\":\"" FW_VERSION "\",";
  o += "\"Clock\":\"" + String(t) + "\",";
  o += "\"Screen\":\"" + String(mode == M_CAL ? "Calibrating" : MENU_NAME[menuIdx]) + "\",";
  o += "\"Address\":\"" + String(online() ? WiFi.localIP().toString() : String("offline")) + "\",";
  o += "\"Signal\":\"" + String(online() ? String(WiFi.RSSI()) + " dBm" : String("-")) + "\",";
  o += "\"Taps 1/2/3/4\":\"" + String(nTapOne) + " / " + String(nTapTwo) + " / " +
                               String(nTapThree) + " / " + String(nTapFour) + "\",";
  o += "\"Tilts U/D/L/R\":\"" + String(nUp) + " / " + String(nDown) + " / " +
                                String(nLeft) + " / " + String(nRight) + "\",";
  o += "\"Squeezes\":\"" + String(nSqueeze) + "\",";
  o += "\"Free ram\":\"" + String(ESP.getFreeHeap() / 1024) + " kB\"}";
  web.send(200, "application/json", o);
}
static void setupWeb() {
  web.on("/", HTTP_GET, []() { web.send_P(200, "text/html; charset=utf-8", PAGE); });
  web.on("/api/state", HTTP_GET, apiState);
  web.on("/api/update", HTTP_POST, []() {
    web.send(200, "application/json", "{\"ok\":true}");
    delay(200); runUpdate();
  });
  web.onNotFound([]() { web.send(404, "text/plain", "not found"); });
  web.begin();
}

// ================================================================
//  THE THREE WAYS IN
// ================================================================

// ---- tilt ----
#define TILT_ON  0.30f
#define TILT_OFF 0.14f
bool tiltLatch = false;
static int tiltGesture() {            // 0 none, 1 up, 2 down, 3 left, 4 right
  float tx, ty;
  tiltRead(tx, ty);
  if (tiltLatch) {
    if (fabsf(tx) < TILT_OFF && fabsf(ty) < TILT_OFF) tiltLatch = false;
    return 0;
  }
  if (fabsf(ty) > fabsf(tx)) {
    if (ty >  TILT_ON) { tiltLatch = true; return 1; }
    if (ty < -TILT_ON) { tiltLatch = true; return 2; }
  } else {
    if (tx < -TILT_ON) { tiltLatch = true; return 3; }
    if (tx >  TILT_ON) { tiltLatch = true; return 4; }
  }
  return 0;
}

// ---- squeeze ----
//  Worth being plain about: an accelerometer cannot feel a squeeze.
//  There is no force sensor on this board, and unlike the older ESP32
//  the C3 has no touch peripheral at all. What a hand tightening round
//  it does leave behind is a patch of low frequency wobble, longer than
//  a knock and gentler than a shake. That is what this watches for. It
//  is an approximation, and the counter for short bursts is there so
//  you can see how often it is fooled.
float sqEnergy = 0, sqThresh = 14.0f;
unsigned long sqAbove = 0;
#define SQ_MIN_MS 220
#define SQ_MAX_MS 1600
static bool squeezeTick() {
  float inst = fabsf(amag - 1.0f) * 100.0f + gmag * 0.35f;
  sqEnergy += (inst - sqEnergy) * 0.25f;
  if (sqEnergy > sqThresh) {
    if (!sqAbove) sqAbove = millis();
    return false;
  }
  if (sqAbove) {
    unsigned long dur = millis() - sqAbove;
    sqAbove = 0;
    if (dur >= SQ_MIN_MS && dur <= SQ_MAX_MS) { nSqueeze++; note("SQUEEZE"); return true; }
    if (dur < SQ_MIN_MS) nSqFalse++;
  }
  return false;
}

// ---- taps ----
static int tapTick() {                // returns 1..4 when a burst settles
  if (adxl && (rReg(adxl, A_INT_SOURCE) & INT_TAP1)) {
    if (!burst) burstStart = millis();
    if (burst < 4) burst++;
  }
  if (burst && millis() - burstStart >= TAP_WINDOW_MS) {
    int n = burst; burst = 0;
    if (n == 1) { nTapOne++;   note("ONE TAP"); }
    if (n == 2) { nTapTwo++;   note("TWO TAPS"); }
    if (n == 3) { nTapThree++; note("THREE TAPS"); }
    if (n == 4) { nTapFour++;  note("FOUR TAPS"); }
    return n;
  }
  return 0;
}

// ================================================================
//  CALIBRATION
// ================================================================
static void calBegin() {
  calStep = CAL_HOLD; calStamp = millis();
  calAcc[0] = calAcc[1] = calAcc[2] = 0; calN = 0;
  axUD = axLR = -1;
  calOkUp = calOkDown = calOkLeft = calOkRight = false;
}
static void calService() {
  readSensors();
  float v[3] = { ax, ay, az };

  switch (calStep) {
    case CAL_HOLD:
      for (int i = 0; i < 3; i++) calAcc[i] += v[i];
      calN++;
      if (millis() - calStamp > 1400 && calN > 6) {
        for (int i = 0; i < 3; i++) restV[i] = calAcc[i] / calN;
        gravAx = 0;
        for (int i = 1; i < 3; i++) if (fabsf(restV[i]) > fabsf(restV[gravAx])) gravAx = i;
        calStep = CAL_UP; calStamp = millis();
      }
      break;

    case CAL_UP: {
      int best = -1; float bd = 0.30f;
      for (int i = 0; i < 3; i++) {
        if (i == gravAx) continue;
        if (fabsf(v[i] - restV[i]) > bd) { bd = fabsf(v[i] - restV[i]); best = i; }
      }
      if (best >= 0) {
        axUD = best;
        sgUD = (v[best] - restV[best]) > 0 ? 1 : -1;   // away from you reads positive
        calOkUp = true;
        calStep = CAL_DOWN; calStamp = millis();
      }
      break;
    }
    case CAL_DOWN: {
      if (millis() - calStamp < 600) break;
      float tx, ty; tiltRead(tx, ty);
      if (ty < -TILT_ON) { calOkDown = true; calStep = CAL_LEFT; calStamp = millis(); }
      break;
    }
    case CAL_LEFT: {
      if (millis() - calStamp < 600) break;
      for (int i = 0; i < 3; i++) {
        if (i == gravAx || i == axUD) continue;
        float d = v[i] - restV[i];
        if (fabsf(d) > 0.30f) {
          axLR = i;
          sgLR = d > 0 ? -1 : 1;                        // leaning left reads negative
          calOkLeft = true;
          calStep = CAL_RIGHT; calStamp = millis();
        }
      }
      break;
    }
    case CAL_RIGHT: {
      if (millis() - calStamp < 600) break;
      float tx, ty; tiltRead(tx, ty);
      if (tx > TILT_ON) { calOkRight = true; calStep = CAL_SHOW; calStamp = millis(); }
      break;
    }
    case CAL_SHOW:
      if (millis() - calStamp > 3400) { calStep = CAL_DONE; mode = M_MENU; }
      break;
  }
}
static void drawTiltBox(int bx, int by, int bw) {
  oled.drawRect(bx, by, bw, bw, SSD1306_WHITE);
  int c = bw / 2;
  oled.drawFastHLine(bx + c - 3, by + c, 7, SSD1306_WHITE);
  oled.drawFastVLine(bx + c, by + c - 3, 7, SSD1306_WHITE);
  float tx, ty;
  tiltRead(tx, ty);
  int px = bx + c + (int)constrain(tx * (bw / 2 - 4) / 0.45f, -(float)(bw / 2 - 4), (float)(bw / 2 - 4));
  int py = by + c - (int)constrain(ty * (bw / 2 - 4) / 0.45f, -(float)(bw / 2 - 4), (float)(bw / 2 - 4));
  oled.fillCircle(px, py, 3, SSD1306_WHITE);
}
static void drawCal() {
  oled.clearDisplay();
  barC("HOLD ME IN YOUR HAND");
  const char* ask = "";
  switch (calStep) {
    case CAL_HOLD:  ask = "Keep it still"; break;
    case CAL_UP:    ask = "Tilt the top away"; break;
    case CAL_DOWN:  ask = "Now tilt it back"; break;
    case CAL_LEFT:  ask = "Now lean it left"; break;
    case CAL_RIGHT: ask = "And now right"; break;
    default:        ask = "This is how it reads"; break;
  }
  if (calStep == CAL_SHOW) {
    ctr(ask, 14, 1);
    drawTiltBox(46, 24, 36);
    oled.display();
    return;
  }
  ctr(ask, 18, 1);
  if (calStep == CAL_HOLD) {
    int n = ((millis() - calStamp) / 300) % 4;
    for (int i = 0; i < 3; i++) oled.fillCircle(52 + i * 12, 38, i < n ? 3 : 1, SSD1306_WHITE);
  } else {
    int a = (millis() / 300) % 3;
    for (int i = 0; i <= a; i++) {
      int s = 6 + i * 6;
      if (calStep == CAL_UP)         { oled.drawLine(58, 44 - i * 6, 64, 38 - i * 6, SSD1306_WHITE); oled.drawLine(70, 44 - i * 6, 64, 38 - i * 6, SSD1306_WHITE); }
      else if (calStep == CAL_DOWN)  { oled.drawLine(58, 34 + i * 6, 64, 40 + i * 6, SSD1306_WHITE); oled.drawLine(70, 34 + i * 6, 64, 40 + i * 6, SSD1306_WHITE); }
      else if (calStep == CAL_LEFT)  { oled.drawLine(70 - i * 8, 34, 64 - i * 8, 40, SSD1306_WHITE); oled.drawLine(70 - i * 8, 46, 64 - i * 8, 40, SSD1306_WHITE); }
      else                            { oled.drawLine(58 + i * 8, 34, 64 + i * 8, 40, SSD1306_WHITE); oled.drawLine(58 + i * 8, 46, 64 + i * 8, 40, SSD1306_WHITE); }
      (void)s;
    }
  }
  // a row of ticks showing how far through it is
  const bool done[4] = { calOkUp, calOkDown, calOkLeft, calOkRight };
  for (int i = 0; i < 4; i++) {
    int x = 40 + i * 13;
    if (done[i]) { oled.fillCircle(x, 57, 3, SSD1306_WHITE); }
    else           oled.drawCircle(x, 57, 3, SSD1306_WHITE);
  }
  oled.display();
}

// ================================================================
//  MENU AND DEMOS
// ================================================================
static void iconTilt(int x, int y) {
  oled.drawRoundRect(x + 5, y + 2, 12, 18, 3, SSD1306_WHITE);
  oled.fillRect(x + 8, y + 6, 6, 9, SSD1306_WHITE);
  oled.drawLine(x + 19, y + 6, x + 22, y + 9, SSD1306_WHITE);
  oled.drawLine(x + 19, y + 12, x + 22, y + 9, SSD1306_WHITE);
  oled.drawLine(x + 3, y + 6, x, y + 9, SSD1306_WHITE);
  oled.drawLine(x + 3, y + 12, x, y + 9, SSD1306_WHITE);
}
static void iconSqueeze(int x, int y) {
  oled.drawRoundRect(x + 7, y + 4, 9, 14, 3, SSD1306_WHITE);
  for (int i = 0; i < 2; i++) {
    int o = i * 3;
    oled.drawLine(x + 2 - o, y + 7, x + 5 - o, y + 11, SSD1306_WHITE);
    oled.drawLine(x + 2 - o, y + 15, x + 5 - o, y + 11, SSD1306_WHITE);
    oled.drawLine(x + 21 + o, y + 7, x + 18 + o, y + 11, SSD1306_WHITE);
    oled.drawLine(x + 21 + o, y + 15, x + 18 + o, y + 11, SSD1306_WHITE);
  }
}
static void iconTaps(int x, int y) {
  oled.fillCircle(x + 11, y + 11, 2, SSD1306_WHITE);
  oled.drawCircle(x + 11, y + 11, 5, SSD1306_WHITE);
  oled.drawCircle(x + 11, y + 11, 9, SSD1306_WHITE);
}
static void iconUpdate(int x, int y) {
  oled.drawFastVLine(x + 11, y + 2, 9, SSD1306_WHITE);
  oled.drawLine(x + 7, y + 7, x + 11, y + 12, SSD1306_WHITE);
  oled.drawLine(x + 15, y + 7, x + 11, y + 12, SSD1306_WHITE);
  oled.drawFastHLine(x + 3, y + 16, 17, SSD1306_WHITE);
  oled.drawFastVLine(x + 3, y + 12, 5, SSD1306_WHITE);
  oled.drawFastVLine(x + 19, y + 12, 5, SSD1306_WHITE);
}
static void menuIcon(int i, int x, int y) {
  if (i == 0) iconTilt(x, y);
  else if (i == 1) iconSqueeze(x, y);
  else if (i == 2) iconTaps(x, y);
  else iconUpdate(x, y);
}
static void drawMenu() {
  oled.clearDisplay();
  char r[8];
  snprintf(r, sizeof(r), "%d/%d", menuIdx + 1, MENU_N);
  bar("TRY A WAY IN", r);
  const int TX[MENU_N] = { 4, 34, 64, 94 };
  for (int i = 0; i < MENU_N; i++) {
    if (i == menuIdx) oled.drawRoundRect(TX[i] - 1, 13, 28, 26, 4, SSD1306_WHITE);
    menuIcon(i, TX[i] + 2, 16);
  }
  ctr(MENU_NAME[menuIdx], 43, 1);
  ctr("1 tap next   2 open", 54, 1);
  oled.display();
}

static void drawDemoList(int x, int y) {
  for (int i = 0; i < 3; i++) {
    int k = (demoSel + i) % DEMO_N;
    if (i == 0) { oled.fillRect(x - 2, y - 2, 128 - x, 11, SSD1306_WHITE); oled.setTextColor(SSD1306_BLACK); }
    else oled.setTextColor(SSD1306_WHITE);
    at(x, y + i * 11, DEMO_ITEM[k]);
    oled.setTextColor(SSD1306_WHITE);
  }
}

static void drawTilt() {
  oled.clearDisplay();
  char r[14];
  snprintf(r, sizeof(r), "%lu", (unsigned long)(nUp + nDown + nLeft + nRight));
  bar("TILT", r);
  drawTiltBox(2, 14, 36);
  drawDemoList(44, 16);
  char l[26];
  snprintf(l, sizeof(l), "U%lu D%lu L%lu R%lu", (unsigned long)min(999UL, (unsigned long)nUp),
           (unsigned long)min(999UL, (unsigned long)nDown),
           (unsigned long)min(999UL, (unsigned long)nLeft),
           (unsigned long)min(999UL, (unsigned long)nRight));
  ctr(l, 54, 1);
  if (demoPicked.length() && millis() - demoPickedAt < 1200) {
    oled.fillRect(12, 20, 104, 22, SSD1306_BLACK);
    oled.drawRect(12, 20, 104, 22, SSD1306_WHITE);
    ctr("PICKED", 24, 1);
    ctr(demoPicked.c_str(), 33, 1);
  }
  oled.display();
}

static void drawSqueeze() {
  oled.clearDisplay();
  char r[14];
  snprintf(r, sizeof(r), "%lu", (unsigned long)nSqueeze);
  bar("SQUEEZE", r);
  int w = (int)constrain(sqEnergy * 1.7f, 0.0f, 118.0f);
  oled.drawRect(4, 14, 120, 9, SSD1306_WHITE);
  if (w > 2) oled.fillRect(5, 15, w - 2, 7, SSD1306_WHITE);
  int tx = 5 + (int)constrain(sqThresh * 1.7f, 0.0f, 116.0f);
  oled.drawFastVLine(tx, 11, 15, SSD1306_WHITE);
  char l[26];
  snprintf(l, sizeof(l), "Level %d   limit %d", (int)sqEnergy, (int)sqThresh);
  ctr(l, 27, 1);
  snprintf(l, sizeof(l), "Took %lu  missed %lu", (unsigned long)min(999UL, (unsigned long)nSqueeze),
           (unsigned long)min(999UL, (unsigned long)nSqFalse));
  ctr(l, 38, 1);
  ctr(DEMO_ITEM[demoSel], 47, 1);
  ctr("Tilt up/down = limit", 56, 1);
  oled.display();
}

static void drawTaps() {
  oled.clearDisplay();
  char r[14];
  snprintf(r, sizeof(r), "%lu", (unsigned long)(nTapOne + nTapTwo + nTapThree + nTapFour));
  bar("TAPS", r);
  ctr(millis() - lastGestureAt < 1400 ? lastGesture : "knock it", 16, 1);
  char l[26];
  snprintf(l, sizeof(l), "1:%lu  2:%lu", (unsigned long)min(999UL, (unsigned long)nTapOne),
           (unsigned long)min(999UL, (unsigned long)nTapTwo));
  ctr(l, 28, 1);
  snprintf(l, sizeof(l), "3:%lu  4:%lu", (unsigned long)min(999UL, (unsigned long)nTapThree),
           (unsigned long)min(999UL, (unsigned long)nTapFour));
  ctr(l, 38, 1);
  ctr(DEMO_ITEM[demoSel], 48, 1);
  ctr("3 taps to leave", 56, 1);
  oled.display();
}

static void pick() { demoPicked = DEMO_ITEM[demoSel]; demoPickedAt = millis(); }

// ================================================================
//  SETUP AND LOOP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  prefs.begin("nexus", true);                 // read only: the real settings stay untouched
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  cfgTz   = prefs.getString("tz", DEF_TZ);
  prefs.end();

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) { Serial.println("no OLED"); return; }
  oled.setTextWrap(false);
  oled.setTextColor(SSD1306_WHITE);
  startSensors();

  oled.clearDisplay();
  ctr("RAFIQ", 12, 3);
  oled.drawFastHLine(20, 38, SCRW - 40, SSD1306_WHITE);
  ctr("input test build", 44, 1);
  ctr(FW_VERSION, 55, 1);
  oled.display();
  delay(1600);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (cfgSsid.length()) {
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
      oled.clearDisplay();
      barC("JOINING WIFI");
      int step = (millis() / 380) % 4;
      oled.fillCircle(64, 50, 2, SSD1306_WHITE);
      for (int k = 1; k <= 3; k++) if (step >= k) oled.drawCircleHelper(64, 50, k * 8, 1 | 2, SSD1306_WHITE);
      oled.display();
      delay(40);
    }
  }
  setupWeb();
  if (online()) trySyncTime(2500);

  oled.clearDisplay();
  barC(online() ? "READY" : "NO WIFI");
  ctr(online() ? WiFi.localIP().toString().c_str() : "OTA needs a network", 20, 1);
  ctr(timeOk ? "Clock set" : "No clock", 32, 1);
  ctr("Three ways to try", 46, 1);
  oled.display();
  delay(2200);

  calBegin();
  mode = M_CAL;
  Serial.printf("test build %s up, adxl %s mpu %s\n", FW_VERSION,
                adxl ? "yes" : "no", mpu ? "yes" : "no");
}

void loop() {
  web.handleClient();
  static unsigned long lastPoll = 0, lastDraw = 0;
  unsigned long now = millis();

  if (now - lastPoll < 20) { delay(2); return; }
  lastPoll = now;
  readSensors();

  if (mode == M_CAL) {
    calService();
    if (now - lastDraw >= 60) { lastDraw = now; drawCal(); }
    return;
  }

  int taps = tapTick();                       // taps are read everywhere, as the way out

  switch (mode) {
    case M_MENU:
      if (taps == 1) menuIdx = (menuIdx + 1) % MENU_N;
      if (taps == 2) {
        if (menuIdx == 3) { runUpdate(); }
        else { mode = M_TILT + menuIdx; demoSel = 0; demoPicked = ""; }
      }
      if (taps == 4) { calBegin(); mode = M_CAL; }
      break;

    case M_TILT: {
      int g = tiltGesture();
      if (g == 1) { nUp++;   note("UP");    demoSel = (demoSel + DEMO_N - 1) % DEMO_N; }
      if (g == 2) { nDown++; note("DOWN");  demoSel = (demoSel + 1) % DEMO_N; }
      if (g == 3) { nLeft++; note("LEFT"); }
      if (g == 4) { nRight++; note("RIGHT"); pick(); }
      if (taps == 3) mode = M_MENU;
      break;
    }
    case M_SQUEEZE: {
      if (squeezeTick()) { demoSel = (demoSel + 1) % DEMO_N; pick(); }
      int g = tiltGesture();                  // tilt tunes the limit while you are here
      if (g == 1 && sqThresh < 60) sqThresh += 2;
      if (g == 2 && sqThresh > 4)  sqThresh -= 2;
      if (taps == 3) mode = M_MENU;
      break;
    }
    case M_TAPS:
      if (taps == 1) demoSel = (demoSel + 1) % DEMO_N;
      if (taps == 2) pick();
      if (taps == 3) mode = M_MENU;
      break;
  }

  if (now - lastDraw >= 55) {
    lastDraw = now;
    switch (mode) {
      case M_TILT:    drawTilt();    break;
      case M_SQUEEZE: drawSqueeze(); break;
      case M_TAPS:    drawTaps();    break;
      default:        drawMenu();    break;
    }
  }
}

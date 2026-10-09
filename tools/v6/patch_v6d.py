#!/usr/bin/env python3
import sys
SRC = sys.argv[1]
s = open(SRC).read()

def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f"anchor matched {n} times, wanted {count}:\n{old[:200]}")
    s = s.replace(old, new)

# ---------------------------------------------------------------- OTA from a file
rep("static void setupWeb() {\n  if (webUp) return;", r'''// ================================================================
//  INSTALLING FROM A FILE  (6.0)
// ================================================================
//  GitHub needs the internet. This needs only the hotspot: join it,
//  open 192.168.4.1/ota, pick the APP file. A FULL image is refused
//  by its size, because it would not fit the slot.
const char OTA_PAGE[] PROGMEM = R"HTML(<!doctype html><meta charset=utf-8>
<meta name=viewport content='width=device-width,initial-scale=1'><title>Rafiq update</title>
<body style='font:16px system-ui;background:#111;color:#eee;padding:1.5em;max-width:30em'>
<h2>Install a firmware file</h2>
<p>Pick the <b>APP</b> file (Rafiq_vX_APP_wireless_update.bin), not the FULL one.
Keep this page open until it says it is restarting.</p>
<input type=file id=f accept='.bin'><br><br>
<button id=b style='font-size:18px;padding:.5em 1.2em'>Install</button>
<p id=s></p>
<script>
b.onclick=function(){var x=f.files[0];if(!x){s.textContent='Pick a file first';return}
var d=new FormData();d.append('f',x,x.name);var r=new XMLHttpRequest();r.open('POST','/ota');
r.upload.onprogress=function(e){if(e.lengthComputable)s.textContent='Sending '+Math.round(e.loaded*100/e.total)+'%'};
r.onload=function(){s.textContent=r.responseText};
r.onerror=function(){s.textContent='The connection dropped. If Rafiq restarted, it worked.'};
b.disabled=true;s.textContent='Sending';r.send(d)};
</script>)HTML";

bool   otaUpOk = false;
String otaUpErr = "";
size_t otaUpGot = 0;

static void otaUpload() {
  HTTPUpload& u = web.upload();
  if (u.status == UPLOAD_FILE_START) {
    otaUpOk = false; otaUpErr = ""; otaUpGot = 0;
    if (!rescueAP && !authed()) { otaUpErr = "join the Rafiq hotspot first"; return; }
    wake("update");
    wsLastUse = millis();
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { otaUpErr = Update.errorString(); return; }
    Serial.printf("installing %s from a file\n", u.filename.c_str());
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (otaUpErr.length()) return;
    // Every ESP image starts with 0xE9. Anything else is not firmware.
    if (otaUpGot == 0 && u.currentSize && u.buf[0] != 0xE9) {
      otaUpErr = "that is not a firmware file"; Update.abort(); return;
    }
    if (Update.write(u.buf, u.currentSize) != u.currentSize) {
      otaUpErr = Update.errorString(); Update.abort(); return;
    }
    otaUpGot += u.currentSize;
    wsLastUse = millis();
    if ((otaUpGot & 0xFFFF) < u.currentSize) {        // every 64 kB
      oled.clearDisplay(); bar("UPDATING");
      char l[22]; snprintf(l, sizeof(l), "%u kB", (unsigned)(otaUpGot / 1024));
      ctr(l, 28, 1); ctr("from a file", 44, 1);
      oled.display();
    }
  } else if (u.status == UPLOAD_FILE_END) {
    if (otaUpErr.length()) return;
    if (Update.end(true)) { otaUpOk = true; Serial.printf("file installed, %u bytes\n", (unsigned)otaUpGot); }
    else otaUpErr = Update.errorString();
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    Update.abort(); otaUpErr = "the upload stopped";
  }
}

static void setupWeb() {
  if (webUp) return;''')

# ---------------------------------------------------------------- the 6.0 functions
BLOCK = r'''// ================================================================
//  6.0: BLUETOOTH FIRST
// ================================================================

// ---- light sleep ----
static void pmInit() {
  esp_pm_config_t c = {};
  c.max_freq_mhz = 160; c.min_freq_mhz = 160; c.light_sleep_enable = false;
  pmAvail = (esp_pm_configure(&c) == ESP_OK);
  pmMode = 0;
  Serial.printf("power: %s\n", pmAvail ? "light sleep on offer" : "stock core, no light sleep");
}
static void pmSet(bool idle) {
  if (!pmAvail) return;
  int m = idle ? 1 : 0;
  if (m == pmMode) return;
  esp_pm_config_t c = {};
  c.max_freq_mhz = 160;
  c.min_freq_mhz = idle ? 40 : 160;
  c.light_sleep_enable = idle;
  if (esp_pm_configure(&c) == ESP_OK) pmMode = m;
}
// Linked to the phone, on a core that can stay linked in the dark.
static bool phoneHeld() {
  return pmAvail && cfgNet == NET_BT && btUp && btConn != 0xFFFF;
}

// ---- WiFi sessions ----
static void wsTouch() {
  wsLastUse = millis();
  if (wsKind == WS_MANUAL) rtcWsUntil = (uint32_t)time(nullptr) + WS_IDLE_S;
}

static bool wsStart(int kind) {
  if (cfgNet == NET_WIFI) {              // already up: only the reason can change
    if (kind == WS_MANUAL && wsKind != WS_MANUAL) { wsKind = WS_MANUAL; rtcWsKind = WS_MANUAL; }
    wsTouch();
    return true;
  }
  if (kind != WS_HOTSPOT && !netCount) { flash("NO SAVED WIFI", 1500); return false; }
  bleOff();
  cfgNet = NET_WIFI;
  wsKind = kind; wsStartMs = millis(); wsSawUp = false;
  rtcWsKind = (kind == WS_MANUAL) ? WS_MANUAL : 0;
  if (kind != WS_MANUAL) rtcWsUntil = 0;
  netDown = false; netMisses = 0; netNextTry = 0; hadNet = false; netUsing = -1;
  WiFi.persistent(false);
  WiFi.mode(kind == WS_HOTSPOT ? WIFI_AP : WIFI_STA);
  WiFi.setSleep(false);
  setupWeb();
  wsTouch();
  Serial.printf("wifi session: %s\n", kind == WS_MANUAL ? "manual" : kind == WS_SYNC ? "sync" :
                                      kind == WS_UPDATE ? "update" : "hotspot");
  return true;
}

static void wsEnd(const char* why) {
  if (cfgNet != NET_WIFI) return;
  wantTime = false; wantWx = false; wantPrayerNow = false;
  wantOtaLatest = false; wantOtaList = false;
  if (upState != U_OFF) upState = U_OFF;
  if (rescueAP) WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  rescueAP = false;
  syncRun = false; syncStarted = false; syncUpArmed = false;
  upAfterJoin = false; upDirect = false;
  wsKind = WS_NONE; rtcWsUntil = 0; rtcWsKind = 0;
  netDown = false; netMisses = 0; netUsing = -1;
  macLinked = false;                   // the Mac was on the network that just went
  cfgNet = cfgNetHome;
  if (cfgNet == NET_BT) bleOn();
  if (screen == S_WEATHER && !wxOk) screen = S_HOME;
  if (why && *why) flash(why, 1400);
  Serial.printf("wifi session over: %s\n", why ? why : "");
}

static void syncBegin() {
  if (!wsStart(WS_SYNC)) return;
  syncRun = true; syncStarted = false; syncStoryDone = false; syncUpArmed = false;
  syncAt = millis();
  flash("SYNCING", 1500);
}

static void rqUpdate() {
  if (!wsStart(WS_UPDATE)) return;
  upAfterJoin = true; upDirect = true;
  upState = U_LOOK; upMsg = "";
}

// Everything a session needs, once a pass.
static void serviceWs() {
  uint32_t now = millis();

  // Commands from the phone that move the radios, a moment after they
  // arrived so the reply to the phone has gone out first.
  if (rqPend && now - rqPendAt > 700) {
    int c = rqPend; rqPend = RQ_NONE;
    switch (c) {
      case RQ_SYNC:    syncBegin(); break;
      case RQ_UPDATE:  rqUpdate(); break;
      case RQ_WIFI:    if (wsStart(WS_MANUAL)) flash("WIFI UNTIL RESTART", 1500); break;
      case RQ_HOTSPOT: startHotspot(); break;
      case RQ_TAMPER:  tamperArm(); break;
      case RQ_DEEP:    wantDeep = true; break;
      case RQ_REBOOT:  ESP.restart(); break;
    }
  }

  if (wsEndWant) { wsEndWant = false; wsEnd("BLUETOOTH"); return; }
  if (cfgNet != NET_WIFI || wsKind == WS_NONE) return;

  // Nothing to join. Say so and go home.
  if (netDown && !rescueAP) {
    bool wasUpd = upAfterJoin;
    wsEnd("NO WIFI FOUND");
    if (wasUpd) { upState = U_FAIL; upMsg = "No WiFi found"; upQuick = true; }
    return;
  }

  if (online()) {
    if (upAfterJoin) {
      upAfterJoin = false;
      if (upDirect) { upDirect = false; upQuick = true; wantOtaLatest = true; upState = U_LOOK; }
      else          { upQuick = false; upState = U_MENU; upPick = 0; upMsg = ""; }
      wake("update");
    }
    if (syncRun && !syncStarted) {
      syncStarted = true;
      wantTime = true; wantWx = true; wantPrayerNow = true;
      if (upState == U_OFF && !wantOtaLatest) { wantOtaLatest = true; syncUpArmed = true; }
    }
  }

  // The sync's look for an update: a newer one is asked about, the
  // same one is said nothing about.
  if (syncUpArmed && !wantOtaLatest) {
    if (upState == U_ASK) {
      syncUpArmed = false; upQuick = true; askSince = now;
      wake("update");
    } else if (upState == U_NONE || upState == U_FAIL) {
      syncUpArmed = false; upState = U_OFF;
    }
  }
  if (upState == U_ASK && askSince && now - askSince > WS_ASK_MS) { upState = U_OFF; askSince = 0; }
  if (upState != U_ASK) askSince = 0;

  if (syncRun && syncStarted && !wantTime && !wantWx && !wantPrayerNow &&
      !wantOtaLatest && !syncUpArmed && upState == U_OFF && !storyBusy) {
    if (!syncStoryDone) {
      syncStoryDone = true;
      // A new short read, if there is a key to write one with. This
      // holds the loop for the length of the call, which is why it is
      // the last thing and says so on the screen.
      if (cfgKey.length()) {
        oled.clearDisplay(); bar("SYNC"); ctr("Writing a read", 30, 1); oled.display();
        fetchStory(false);
      }
      return;
    }
    syncRun = false;
    lastSyncAt = (uint32_t)time(nullptr);
    prefs.putUInt("lsync", lastSyncAt);
    if (wsKind == WS_SYNC) wsEnd("SYNC DONE");
    else flash("SYNC DONE", 1200);
    return;
  }
  if (syncRun && now - syncAt > WS_SYNC_MS && upState != U_ASK) {
    syncRun = false;
    if (wsKind == WS_SYNC) { wsEnd(online() ? "SYNC PARTLY DONE" : "NO WIFI FOUND"); return; }
  }

  if (wsKind == WS_UPDATE) {
    if (upState != U_OFF) wsSawUp = true;
    else if (wsSawUp) { wsEnd(""); return; }
    else if (now - wsStartMs > 20000) { wsEnd(""); return; }
  }
  if (wsKind == WS_MANUAL && rtcWsUntil && !syncRun && upState == U_OFF &&
      (uint32_t)time(nullptr) > rtcWsUntil) {
    wsEnd("WIFI OFF");
    return;
  }
  if (wsKind == WS_HOTSPOT) {
    if (WiFi.softAPgetStationNum() > 0) wsLastUse = now;
    if (now - wsLastUse > WS_HOTSPOT_IDLE_MS) { wsEnd("HOTSPOT OFF"); return; }
  }
}

// ---- weather, kept ----
static void saveWx() {
  char b[96];
  snprintf(b, sizeof(b), "%.1f|%.0f|%.1f|%d|%lu|%s", wTemp, wHum, wWind, wCode,
           (unsigned long)wxAt, wCity);
  prefs.putString("wx", b);
}
static void loadWx() {
  String s = prefs.getString("wx", "");
  if (!s.length()) return;
  float t = NAN, h = NAN, w = NAN; int c = -1; unsigned long a = 0; char city[16] = "";
  int got = sscanf(s.c_str(), "%f|%f|%f|%d|%lu|%15[^\n]", &t, &h, &w, &c, &a, city);
  if (got < 5) return;
  wTemp = t; wHum = h; wWind = w; wCode = c; wxAt = a;
  if (city[0]) { strncpy(wCity, city, sizeof(wCity) - 1); wCity[sizeof(wCity) - 1] = 0; }
  wxOk = !isnan(wTemp);
}

// ================================================================
//  RAFIQ, FROM A SHORTCUT ON THE PHONE
// ================================================================
//  A Shortcut posts a notification. If it comes from the Shortcuts app
//  and says RAFIQ in its title, its subtitle or the start of its text,
//  the rest is read as commands, one to a line, or as a weather report.
//  Only the Shortcuts app counts, because Rafiq is also somebody's
//  name, and a message from him must never be taken as an order.
//
//  Commands run only if the notification is fresh and has never run
//  before, because iOS hands old notifications over again after a
//  restart and a reconnect. Weather is always taken: it is harmless.
static const char* rqSkip(const char* s) {
  while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
  return s;
}
static bool rqTagged(const char* s) {
  s = rqSkip(s);
  if (strncasecmp(s, "RAFIQ", 5)) return false;
  char c = s[5];
  return c == 0 || c == ':' || c == ' ' || c == '\n' || c == '\r' || c == '-';
}
static const char* rqAfterTag(const char* s) {
  s = rqSkip(s);
  if (!strncasecmp(s, "RAFIQ", 5)) {
    s += 5;
    while (*s == ':' || *s == ' ' || *s == '-' || *s == '\n' || *s == '\r') s++;
  }
  return s;
}
static bool rafiqIs(const Note& n) {
  bool sc = !strncmp(n.app, "com.apple.shortcuts", 19) || !strncmp(n.app, "is.workflow", 11);
  return sc && (rqTagged(n.title) || rqTagged(stSub) || rqTagged(stMsg));
}

// "yyyyMMddTHHmmSS", the phone's own wall clock. Compared with ours as
// two wall clocks, which is right in either time model.
static bool rqFresh(const char* d) {
  if (strlen(d) < 15 || d[8] != 'T' || !timeOk) return true;   // run once decides alone
  for (int i = 0; i < 15; i++) if (i != 8 && (d[i] < '0' || d[i] > '9')) return true;
  auto num = [&](int at, int len) { int v = 0; for (int i = 0; i < len; i++) v = v * 10 + (d[at + i] - '0'); return v; };
  struct tm t = {};
  t.tm_year = num(0, 4) - 1900; t.tm_mon = num(4, 2) - 1; t.tm_mday = num(6, 2);
  t.tm_hour = num(9, 2); t.tm_min = num(11, 2); t.tm_sec = num(13, 2);
  time_t now = time(nullptr); struct tm lt; localtime_r(&now, &lt);
  long age = (long)(utcFromTm(&lt) - utcFromTm(&t));
  return age > -300 && age <= 120;                 // a little clock skew allowed
}

uint32_t rqSeen[8];
bool     rqSeenLoaded = false;
static uint32_t rqFnv(const char* s, uint32_t h) {
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h;
}
static bool rqRanBefore(uint32_t h) {
  if (!rqSeenLoaded) {
    memset(rqSeen, 0, sizeof(rqSeen));
    if (prefs.isKey("rqseen")) prefs.getBytes("rqseen", rqSeen, sizeof(rqSeen));
    rqSeenLoaded = true;
  }
  for (int i = 0; i < 8; i++) if (rqSeen[i] == h) return true;
  return false;
}
static void rqRemember(uint32_t h) {
  memmove(rqSeen + 1, rqSeen, sizeof(uint32_t) * 7);
  rqSeen[0] = h;
  prefs.putBytes("rqseen", rqSeen, sizeof(rqSeen));
}

// Weather, in whatever words the Shortcut used:
//   temp=24;cond=Sunny;hum=60
//   Temp: 21°C (H: 25°C, L: 18°C)  Condition: Mostly Cloudy  Humidity: 88%
static int wxCodeFromWords(const char* w) {
  char l[48]; int n = 0;
  for (const char* p = w; *p && n < 47; p++) l[n++] = tolower((unsigned char)*p);
  l[n] = 0;
  if (strstr(l, "thunder") || strstr(l, "storm")) return 95;
  if (strstr(l, "snow") || strstr(l, "sleet") || strstr(l, "flurr") || strstr(l, "hail")) return 71;
  if (strstr(l, "drizzle")) return 51;
  if (strstr(l, "shower")) return 80;
  if (strstr(l, "rain")) return 61;
  if (strstr(l, "fog") || strstr(l, "mist") || strstr(l, "haze") || strstr(l, "smok") || strstr(l, "dust")) return 45;
  if (strstr(l, "overcast") || strstr(l, "mostly cloudy")) return 3;
  if (strstr(l, "partly") || strstr(l, "mostly sunny") || strstr(l, "mostly clear")) return 2;
  if (strstr(l, "cloud")) return 3;
  if (strstr(l, "clear") || strstr(l, "sunny") || strstr(l, "fair")) return 0;
  return -1;
}
static bool rqNum(const char* v, float& out) {
  while (*v && !isdigit((unsigned char)*v) && *v != '-' && *v != '.') v++;
  if (!*v) return false;
  char* e; out = strtof(v, &e);
  return e != v;
}
static int rqWeather(const char* text) {
  int got = 0;
  float t = NAN, h = NAN, w = NAN; int code = -9; char city[16] = "";
  const char* s = text;
  while (*s) {
    const char* e = s;
    while (*e && !strchr("\n;,()|", *e)) e++;
    char seg[64]; int L = e - s; if (L > 63) L = 63;
    memcpy(seg, s, L); seg[L] = 0;
    s = *e ? e + 1 : e;
    char* sep = strpbrk(seg, ":=");
    if (!sep) continue;
    *sep = 0;
    char key[24]; int k = 0;
    for (char* p = seg; *p && k < 23; p++) {
      char c = tolower((unsigned char)*p);
      if (c == ' ' && (k == 0 || key[k - 1] == ' ')) continue;
      key[k++] = c;
    }
    while (k && key[k - 1] == ' ') k--;
    key[k] = 0;
    const char* val = sep + 1;
    while (*val == ' ') val++;
    float f;
    bool fahr = strstr(val, "F") && !strstr(val, "C");
    if (!strcmp(key, "temp") || !strcmp(key, "temperature")) {
      if (rqNum(val, f)) { t = fahr ? (f - 32) * 5 / 9 : f; got++; }
    } else if (!strcmp(key, "hum") || !strcmp(key, "humidity")) {
      if (rqNum(val, f)) { h = f; got++; }
    } else if (!strcmp(key, "wind") || !strcmp(key, "wind speed")) {
      if (rqNum(val, f)) { w = strstr(val, "mph") ? f * 1.609f : f; got++; }
    } else if (!strcmp(key, "cond") || !strcmp(key, "condition") || !strcmp(key, "weather")) {
      int c = wxCodeFromWords(val);
      if (c >= 0) { code = c; got++; }
    } else if (!strcmp(key, "loc") || !strcmp(key, "location") || !strcmp(key, "city")) {
      snprintf(city, sizeof(city), "%s", val);
      for (char* p = city; *p; p++) if (*p == '\r') *p = 0;
      got++;
    }
  }
  if (!got) return 0;
  // A report with no temperature keeps the one we had; anything it
  // does say replaces what was there, and anything it does not say
  // is cleared rather than left looking current.
  if (!isnan(t)) { wTemp = t; wHum = h; wWind = w; }
  if (code != -9) wCode = code;
  if (city[0]) { strncpy(wCity, city, sizeof(wCity) - 1); wCity[sizeof(wCity) - 1] = 0; }
  wxOk = !isnan(wTemp);
  wxAt = (uint32_t)time(nullptr);
  wxDirty = true;
  Serial.printf("weather from the phone: %.1fC code %d %s\n", wTemp, wCode, wCity);
  return got;
}

static void rqStatus() {
  char sy[16];
  uint32_t tnow = (uint32_t)time(nullptr);
  if (lastSyncAt && tnow >= lastSyncAt) {
    uint32_t a = (tnow - lastSyncAt) / 60;
    if (a < 60)        snprintf(sy, sizeof(sy), "%lum ago", (unsigned long)a);
    else if (a < 2880) snprintf(sy, sizeof(sy), "%luh ago", (unsigned long)(a / 60));
    else               snprintf(sy, sizeof(sy), "%lud ago", (unsigned long)(a / 1440));
  } else snprintf(sy, sizeof(sy), "never");
  // Two lines of 21, which is what the card shows.
  char b[64];
  // "BT no radio 100%" is the longest first line; the second only
  // runs past 21 with both flags on and a two digit age, and then it
  // loses the end of "quiet", not anything that matters.
  snprintf(b, sizeof(b), "BT %.8s %d%%\nsync %s%s%s",
           btShort(), isnan(battV) ? 0 : battPct(battV),
           sy, cfgPGuard ? " guard" : "", cfgQuiet ? " quiet" : "");
  toastKind = "rafiq"; toastText = b;
  toastUntil = millis() + 9000; toastFlash = millis(); remShowing = -1;
}

static void rqRemind(const char* s) {
  while (*s == ' ' || *s == ':') s++;
  int h, m;
  if (!timeOk) { flash("NO CLOCK YET", 1500); return; }
  if (sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
    flash("REMIND HH:MM WHAT", 1600); return;
  }
  while (*s && *s != ' ') s++;
  while (*s == ' ') s++;
  if (!*s) s = "Reminder";
  time_t now = time(nullptr);
  struct tm lt; localtime_r(&now, &lt);
  lt.tm_hour = h; lt.tm_min = m; lt.tm_sec = 0;
  time_t at = mktime(&lt);
  if (at <= now) at += 86400;                       // already gone today
  if (addRem(s, (uint32_t)at)) { sortRems(); saveRems(); remAddedCard(1, (uint32_t)at); }
  else flash("REMINDERS FULL", 1300);
}

// One line, one command. 1 if it was one, 0 if not.
static int rqCommand(const char* raw) {
  char c[48]; int n = 0;
  for (const char* p = raw; *p && n < 47; p++) {
    char ch = tolower((unsigned char)*p);
    if (ch == '.' || ch == '!' || ch == '\r') continue;
    if (ch == ' ' && (n == 0 || c[n - 1] == ' ')) continue;
    c[n++] = ch;
  }
  while (n && c[n - 1] == ' ') n--;
  c[n] = 0;
  if (!n) return 0;
  uint32_t now = millis();
  auto is  = [&](const char* w) { return !strcmp(c, w); };
  auto pre = [&](const char* w) { size_t L = strlen(w); return !strncmp(c, w, L) && (c[L] == ' ' || c[L] == 0); };

  if (is("sync") || is("sync now")) rqPend = RQ_SYNC;
  else if (is("update") || is("check update") || is("software update")) rqPend = RQ_UPDATE;
  else if (is("wifi") || is("wi-fi") || is("wifi on")) rqPend = RQ_WIFI;
  else if (is("bluetooth") || is("bt")) flash("ON BLUETOOTH", 1200);
  else if (is("config") || is("config-robo") || is("config robo") || is("hotspot")) rqPend = RQ_HOTSPOT;
  else if (is("guard on") || is("guard off")) {
    cfgPGuard = is("guard on"); prefs.putBool("guard", cfgPGuard);
    pgFired = false; pgUntil = 0;
    flash(cfgPGuard ? "GUARD ON" : "GUARD OFF", 1200);
  }
  else if (is("tamper on") || is("tamper")) rqPend = RQ_TAMPER;
  else if (is("find") || is("find me") || is("where are you")) findUntil = now + 20000;
  else if (is("relax") || is("relax on")) { relaxOn = true; relaxKind = 0; relaxNext = now + 30000UL; }
  else if (is("relax off")) relaxOn = false;
  else if (is("focus stop") || is("focus off")) { stopSession(); flash("FOCUS STOPPED", 1000); }
  else if (pre("focus")) { int m = atoi(c + 5); focusBegin(m > 0 ? constrain(m, 1, 240) : 25); }
  else if (pre("bright") || pre("brightness")) {
    const char* q = strchr(c, ' ');
    int pct = q ? atoi(q) : 0;
    if (pct <= 0) return 0;
    cfgBright = constrain(pct * 255 / 100, 26, 255);
    prefs.putInt("bri", cfgBright); applyBright();
    flash("BRIGHTNESS SET", 900);
  }
  else if (pre("face")) {
    int f = atoi(c + 4);
    if (f < 1) return 0;
    cfgFace = constrain(f - 1, 0, FACE_N - 1); prefs.putInt("face", cfgFace);
    screen = S_HOME; depth = 0;
  }
  else if (is("notifications on") || is("notification on") || is("popups on") || is("quiet off")) {
    cfgQuiet = false; prefs.putBool("quiet", false); flash("NOTIFICATIONS ON", 1200);
  }
  else if (is("notifications off") || is("notification off") || is("popups off") ||
           is("quiet") || is("quiet on") || is("silent")) {
    cfgQuiet = true; prefs.putBool("quiet", true); flash("NOTIFICATIONS QUIET", 1200);
  }
  else if (is("home")) { screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0; }
  else if (is("sleep")) goSleep();
  else if (is("off") || is("deep sleep") || is("power off")) rqPend = RQ_DEEP;
  else if (is("reboot") || is("restart")) rqPend = RQ_REBOOT;
  else if (is("status")) rqStatus();
  else if (is("prayer refresh") || is("prayers refresh") || is("refresh prayer")) {
    prayerWanted = true; rqPend = RQ_SYNC;
  }
  else if (pre("prayer")) {
    const char* q = c + 7;
    const char* names[5] = { "fajr", "dhuhr", "asr", "maghrib", "isha" };
    int which = -1, L = 0;
    for (int i = 0; i < 5 && which < 0; i++) {
      L = strlen(names[i]);
      if (!strncmp(q, names[i], L)) which = i;
    }
    if (which < 0 && !strncmp(q, "zuhr", 4)) { which = 1; L = 4; }
    if (which < 0) return 0;
    prayerAdj[which] = constrain(atoi(q + L), -60, 60);
    saveAdj();
    char m[22]; snprintf(m, sizeof(m), "%s %+d MIN", PRAYERS[which], prayerAdj[which]);
    flash(m, 1400);
  }
  else return 0;
  if (rqPend) rqPendAt = now;
  return 1;
}

static void rafiqPayload(const char* p, bool fresh) {
  // The first word decides whether the rest belongs to it.
  char first[16]; int n = 0;
  for (const char* q = p; *q && *q != '\n' && *q != ':' && *q != ' ' && n < 15; q++)
    first[n++] = tolower((unsigned char)*q);
  first[n] = 0;

  if (!strcmp(first, "msg") || !strcmp(first, "message") || !strcmp(first, "say")) {
    if (!fresh) return;
    const char* t = p + n;
    while (*t == ':' || *t == ' ') t++;
    String m = String(t); m.replace("\n", " "); m.replace("\r", ""); m.trim();
    if (!m.length()) return;
    message = m.substring(0, 84);
    prefs.putString("msg", message);
    Note pn = {};
    pn.uid = (uint32_t)millis() | 0x80000000UL;    // never an ANCS id
    pn.unread = true;
    strncpy(pn.app,   "Rafiq.shortcut", sizeof(pn.app) - 1);
    strncpy(pn.title, "From your phone", sizeof(pn.title) - 1);
    strncpy(pn.msg,   message.c_str(), sizeof(pn.msg) - 1);
    addNote(pn);
    wake("message");
    screen = S_MSG; depth = 0;
    popupUntil = millis() + (unsigned long)max(popupSecs(), 10) * 1000UL;
    return;
  }
  if (!strcmp(first, "remind")) {
    if (!fresh) return;
    wake("reminder");
    rqRemind(p + n);
    return;
  }

  // Line by line. A line with no colon or equals sign is a command, so
  // anything with one in it is waking the screen; a report alone is not.
  bool cmdish = false;
  for (const char* q = p; *q; ) {
    const char* e = q; while (*e && *e != '\n' && *e != ';') e++;
    bool has = false, any = false;
    for (const char* r = q; r < e; r++) { if (*r == ':' || *r == '=') has = true; if (*r > ' ') any = true; }
    if (any && !has) cmdish = true;
    q = *e ? e + 1 : e;
  }
  if (fresh && cmdish) wake("shortcut");

  String wx;
  bool did = false;
  for (const char* q = p; *q; ) {
    const char* e = q; while (*e && *e != '\n' && *e != ';') e++;
    char ln[96]; int L = e - q; if (L > 95) L = 95;
    memcpy(ln, q, L); ln[L] = 0;
    q = *e ? e + 1 : e;
    if (fresh && rqCommand(ln)) { did = true; continue; }
    wx += ln; wx += '\n';
  }
  if (wx.length() && rqWeather(wx.c_str())) {
    did = true;
    if (!asleep) flash("WEATHER UPDATED", 1000);
  }
  if (!did && fresh) { wake("shortcut"); flash("RAFIQ?", 1200); }
}

static void rafiqNote(const Note& n) {
  // Shortcuts notifications usually cannot be cleared by an accessory,
  // but asking costs nothing and some versions of iOS allow it.
  ancsAction(n.uid, 1);
  const char* p = rqAfterTag(stMsg);
  if (!*p) p = rqAfterTag(stSub);
  bool fresh = rqFresh(stDate);
  if (fresh) {
    uint32_t h = stDate[0] ? rqFnv(p, rqFnv("|", rqFnv(stDate, 2166136261u)))
                           : rqFnv(p, 2166136261u ^ n.uid);
    if (rqRanBefore(h)) fresh = false;
    else rqRemember(h);
  }
  Serial.printf("rafiq%s: %s\n", fresh ? "" : " (old, data only)", p);
  rafiqPayload(p, fresh);
}

// ================================================================
//  THE PHONE GUARD
// ================================================================
//  Linked and strong is fine. Weak for six seconds, or gone for four,
//  and it calls out. It does not call again until the phone is back.
static void pgFire(const char* why) {
  pgFired = true; pgWhy = why;
  pgUntil = millis() + PG_SHOW_MS;
  wake("guard");
  Serial.printf("guard: %s\n", why);
}
static void pgTick() {
  uint32_t now = millis();
  if (!cfgPGuard || cfgNet != NET_BT || !btUp) {
    pgEver = false; pgLostAt = 0; pgWeakSince = 0; pgRssi = 0;
    return;
  }
  bool linked = (btConn != 0xFFFF && btStage == BT_BONDED);
  if (linked) {
    pgEver = true; pgLostAt = 0;
    if (now - pgRssiAt >= 2000) {
      pgRssiAt = now;
      int8_t r = 0;
      if (ble_gap_conn_rssi(btConn, &r) == 0 && r < 0)
        pgRssi = (pgRssi == 0) ? r : pgRssi * 0.7f + r * 0.3f;
    }
    if (pgRssi != 0 && pgRssi < PG_WEAK_DBM) { if (!pgWeakSince) pgWeakSince = now; }
    else pgWeakSince = 0;
    if (pgFired && (pgRssi == 0 || pgRssi > PG_OK_DBM)) {
      pgFired = false;
      if (pgUntil) { pgUntil = 0; flash("THERE YOU ARE", 1200); }
    }
    if (!pgFired && pgWeakSince && now - pgWeakSince > PG_WEAK_MS) pgFire("Phone is far away");
  } else if (pgEver) {
    pgRssi = 0; pgWeakSince = 0;
    if (!pgLostAt) pgLostAt = now;
    if (!pgFired && now - pgLostAt > PG_LOST_MS) pgFire("Phone is gone");
  }
}

static void drawGuard() {
  oled.clearDisplay();
  bool b = (millis() / 450) % 2;
  uint16_t fg = b ? SSD1306_BLACK : SSD1306_WHITE;
  if (b) oled.fillRect(0, 0, SCRW, SCRH, SSD1306_WHITE);
  // two eyes, brows down at the middle: worried
  oled.fillRoundRect(30, 8, 24, 18, 6, fg);
  oled.fillRoundRect(74, 8, 24, 18, 6, fg);
  uint16_t bg = b ? SSD1306_WHITE : SSD1306_BLACK;
  oled.fillTriangle(30, 6, 54, 6, 30, 14, bg);
  oled.fillTriangle(98, 6, 74, 6, 98, 14, bg);
  oled.setTextColor(fg);
  ctr("WAIT FOR ME", 34, 1);
  ctr(pgWhy, 48, 1);
  oled.setTextColor(SSD1306_WHITE);
  oled.display();
}

static void drawFind() {
  oled.clearDisplay();
  bool b = (millis() / 300) % 2;
  if (b) oled.fillRect(0, 0, SCRW, SCRH, SSD1306_WHITE);
  oled.setTextColor(b ? SSD1306_BLACK : SSD1306_WHITE);
  ctr("I'M HERE", 18, 2);
  ctr("press to stop", 46, 1);
  oled.setTextColor(SSD1306_WHITE);
  oled.display();
}

// ================================================================
//  TAMPER
// ================================================================
//  Ten seconds to change your mind, then dark and quiet: no screen, no
//  radio, and every move written down with the time. With the INT1
//  wire it lies in deep sleep and the accelerometer wakes it; without
//  it, it watches from light sleep five times a second. Only a real
//  restart (power, or the reset button) ends it.
static void tlogAdd(const char* what) {
  if (!fsOk) return;
  char ln[40];
  time_t t = time(nullptr);
  struct tm lt; localtime_r(&t, &lt);
  if (t > 1700000000) snprintf(ln, sizeof(ln), "%02d/%02d %02d:%02d %s\n",
                               lt.tm_mday, lt.tm_mon + 1, lt.tm_hour, lt.tm_min, what);
  else                snprintf(ln, sizeof(ln), "--/-- --:-- %s\n", what);
  File f = LittleFS.open(TLOG_PATH, "a");
  if (!f) return;
  size_t sz = f.size();
  f.print(ln);
  f.close();
  if (sz > TLOG_MAX) {                         // keep the newer half
    File r = LittleFS.open(TLOG_PATH, "r");
    if (!r) return;
    String all = r.readString(); r.close();
    int cut = all.indexOf('\n', all.length() / 2);
    if (cut > 0) {
      File w = LittleFS.open(TLOG_PATH, "w");
      if (w) { w.print(all.substring(cut + 1)); w.close(); }
    }
  }
}
static void tlogLoad() {
  tlN = 0; tlSel = 0;
  if (!fsOk) return;
  File f = LittleFS.open(TLOG_PATH, "r");
  if (!f) return;
  // A ring of the last TL_N lines, then turned round, newest first.
  char ring[TL_N][24]; int cnt = 0;
  while (f.available()) {
    String l = f.readStringUntil('\n'); l.trim();
    if (!l.length()) continue;
    snprintf(ring[cnt % TL_N], 24, "%s", l.c_str());
    cnt++;
  }
  f.close();
  int have = cnt < TL_N ? cnt : TL_N;
  for (int i = 0; i < have; i++) snprintf(tlLines[i], 24, "%s", ring[(cnt - 1 - i) % TL_N]);
  tlN = have;
}
static const char* tlLabel(int i, char* b, size_t n) { snprintf(b, n, "%s", tlLines[i]); return b; }
static void drawTlog() {
  char r[8]; snprintf(r, sizeof(r), "%d", tlN);
  drawList("TAMPER LOG", r, tlN, tlSel, tlLabel);
}
static void drawTamperCount() {
  oled.clearDisplay();
  titleBarC("TAMPER ALARM");
  long left = ((long)TAMPER_COUNT_MS - (long)(millis() - tamperCountAt) + 999) / 1000;
  if (left < 0) left = 0;
  char b[8]; snprintf(b, sizeof(b), "%ld", left);
  ctr(b, 18, 3);
  ctr("press to cancel", 52, 1);
  oled.display();
}
static void tamperArm() {
  if (!fsOk) { flash("NO FILESYSTEM", 1200); return; }
  wake("tamper");
  tamperCountAt = millis();
  if (!tamperCountAt) tamperCountAt = 1;
}

#define ADXL_A 0x53
static void tamperDeep(bool pause) {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (pause) {                                  // let a move finish before listening again
    rtcTamper = 2;
    esp_sleep_enable_timer_wakeup(30ULL * 1000000ULL);
  } else {
    rtcTamper = 1;
    esp_deep_sleep_enable_gpio_wakeup(BIT(TAP_INT_PIN), ESP_GPIO_WAKEUP_GPIO_HIGH);
  }
  esp_deep_sleep_start();
}
// Called first thing in setup, armed and woken. Never returns.
static void tamperWake(bool moved) {
  if (rtcTz[0]) { setenv("TZ", rtcTz, 1); tzset(); }
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  if (moved) {
    fsOk = LittleFS.begin(false);
    tlogAdd("Moved");
    LittleFS.end();
    rReg(ADXL_A, 0x30);
    tamperDeep(true);
  }
  pinMode(TAP_INT_PIN, INPUT_PULLDOWN);
  rReg(ADXL_A, 0x30);                           // reading INT_SOURCE lets INT1 go
  delay(5);
  if (digitalRead(TAP_INT_PIN)) tamperDeep(true);   // still moving
  tamperDeep(false);
}
static void tamperGo() {
  tlogAdd("Armed");
  prefs.putBool("tamper", true);
  const char* tz = getenv("TZ");
  snprintf(rtcTz, sizeof(rtcTz), "%s", tz ? tz : "");
  oled.clearDisplay(); oled.display();
  screenPower(false);
  bleOff();
  if (cfgNet == NET_WIFI) { WiFi.disconnect(true, false); WiFi.mode(WIFI_OFF); }
  cfgNet = NET_OFF;
  Serial.println("tamper: armed");
  if (intWired && adxl) {
    wReg(adxl, 0x2E, 0x00);                     // nothing while it changes
    wReg(adxl, 0x24, 6);                        // THRESH_ACT, 375 mg
    wReg(adxl, 0x27, 0xF0);                     // activity, AC coupled, x y z
    wReg(adxl, 0x2F, 0x00);                     // everything to INT1
    wReg(adxl, 0x2E, 0x10);                     // activity only
    rReg(adxl, 0x30);
    delay(20);
    tamperDeep(false);                          // never returns
  }
  // No wire: watch from here, five times a second, in light sleep.
  if (pmAvail) {
    esp_pm_config_t c = {};
    c.max_freq_mhz = 80; c.min_freq_mhz = 40; c.light_sleep_enable = true;
    esp_pm_configure(&c);
  }
  readSensors();
  float bx = ax, by = ay, bz = az;
  uint32_t lastMove = 0, lastTouch = 0;
  rtcTamper = 0;                                // nothing to resume from deep sleep
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(200));
    readSensors();
    uint32_t now = millis();
    float d = fabsf(ax - bx) + fabsf(ay - by) + fabsf(az - bz);
    if (d > 0.25f) {
      if (!lastMove || now - lastMove > 30000) { tlogAdd("Moved"); lastMove = now; }
      bx = ax; by = ay; bz = az;
    }
    if (digitalRead(TOUCH_PIN) != touchRest) {
      if (!lastTouch || now - lastTouch > 30000) { tlogAdd("Touched"); lastTouch = now; }
    }
  }
}
static void serviceTamper() {
  if (tamperCountAt && millis() - tamperCountAt >= TAMPER_COUNT_MS) {
    tamperCountAt = 0;
    tamperGo();
  }
}

'''
rep("void setup() {\n", BLOCK + "void setup() {\n")

open(SRC, 'w').write(s)
print("part 4 ok")

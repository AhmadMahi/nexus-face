// Host tests for the RAFIQ Shortcut bridge. The functions under test are
// copied verbatim out of nexus_face.ino by extract.py; only the things
// they call are stubbed here, and every stub records what it was asked.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <map>
#include <strings.h>
#include <algorithm>
using std::isnan;

// ---- Arduino-ish String, just what the code uses ----
struct String {
  std::string v;
  String() {}
  String(const char* s) : v(s ? s : "") {}
  String(const std::string& s) : v(s) {}
  const char* c_str() const { return v.c_str(); }
  size_t length() const { return v.size(); }
  String substring(size_t a, size_t b) const { if (a > v.size()) return String(); return String(v.substr(a, std::min(b, v.size()) - a)); }
  void replace(const char* a, const char* b) { std::string f(a), t(b); size_t p = 0; while ((p = v.find(f, p)) != std::string::npos) { v.replace(p, f.size(), t); p += t.size(); } }
  void trim() { size_t a = v.find_first_not_of(" \t\r\n"); if (a == std::string::npos) { v.clear(); return; } size_t b = v.find_last_not_of(" \t\r\n"); v = v.substr(a, b - a + 1); }
  String& operator+=(const char* s) { v += s; return *this; }
  String& operator+=(char c) { v += c; return *this; }
  bool operator==(const char* s) const { return v == s; }
};
template<class A, class B> auto max(A a, B b) { return a > b ? a : b; }
template<class T> T constrain(T x, T lo, T hi) { return x < lo ? lo : x > hi ? hi : x; }

// ---- recorded calls ----
std::vector<std::string> calls;
static void rec(const std::string& s) { calls.push_back(s); }
static bool called(const char* prefix) { for (auto& c : calls) if (c.rfind(prefix, 0) == 0) return true; return false; }

struct Prefs {
  std::map<std::string, std::string> kv;
  bool isKey(const char* k) { return kv.count(k); }
  void getBytes(const char* k, void* b, size_t n) { auto& s = kv[k]; memcpy(b, s.data(), std::min(n, s.size())); }
  void putBytes(const char* k, const void* b, size_t n) { kv[k] = std::string((const char*)b, n); }
  void putBool(const char* k, bool v) { kv[k] = v ? "1" : "0"; rec(std::string("putBool ") + k + "=" + (v ? "1" : "0")); }
  void putInt(const char* k, int v) { kv[k] = std::to_string(v); rec(std::string("putInt ") + k + "=" + std::to_string(v)); }
  void putString(const char* k, const String& v) { kv[k] = v.v; rec(std::string("putString ") + k + "=" + v.v); }
} prefs;

struct Note { uint32_t uid; uint8_t cat; bool unread; uint32_t at; char app[40]; char title[34]; char msg[100]; };

// ---- globals the functions read and write ----
bool timeOk = true, asleep = false;
char stMsg[404], stSub[34], stDate[20];
uint32_t rqSeen[8]; bool rqSeenLoaded = false;
float wTemp = NAN, wHum = NAN, wWind = NAN; int wCode = -1; char wCity[16] = "";
bool wxOk = false; uint32_t wxAt = 0; volatile bool wxDirty = false;
String message;
enum { RQ_NONE = 0, RQ_SYNC, RQ_UPDATE, RQ_WIFI, RQ_HOTSPOT, RQ_TAMPER, RQ_DEEP, RQ_REBOOT };
int rqPend = RQ_NONE; uint32_t rqPendAt = 0;
bool cfgPGuard = false, pgFired = false, cfgQuiet = false, relaxOn = false, prayerWanted = false;
uint32_t pgUntil = 0, findUntil = 0, relaxNext = 0, popupUntil = 0; int relaxKind = 0;
int cfgBright = 160, cfgFace = 0, screen = 0, depth = 0, itemIdx = 0, subIdx = 0, noteIdx = 0;
#define FACE_N 21   // 7.14: arabic and crescent went
#define S_HOME 0
#define S_MSG 5
const char* PRAYERS[5] = { "Fajr", "Dhuhr", "Asr", "Maghrib", "Isha" };
int prayerAdj[5] = { 0, 0, 0, 0, 0 };
uint32_t fakeNow = 0;
static uint32_t millis() { return fakeNow ? fakeNow : 123456; }

static void flash(const char* w, unsigned long) { rec(std::string("flash ") + w); }
static void wake(const char* w) { rec(std::string("wake ") + w); }
static void addNote(const Note& n) { rec(std::string("addNote ") + n.msg); }
static void ancsAction(uint32_t uid, uint8_t a) { rec("ancsAction " + std::to_string(a)); }
static void stopSession() { rec("stopSession"); }
static void focusBegin(int m) { rec("focusBegin " + std::to_string(m)); }
static void applyBright() { rec("applyBright"); }
static void goSleep() { rec("goSleep"); }
static void rqStatus() { rec("rqStatus"); }
bool awayOn = false, awayAuto = false; int cfgWakeBy = 0; uint32_t popGlanceMs = 1000;
// 7.13: the radio mode and keyring mode, which rqCommand now touches.
enum { BTM_ALWAYS = 0, BTM_SMART, BTM_N };
int cfgBtMode = BTM_ALWAYS;
bool cfgKeyring = false, keyOut = false;
uint32_t keyCardUntil = 0, keyBootAt = 0;
int keyPokes = 0; bool keyDizzy = false;
static void applyBtMode(bool) {}
static void applyCpu() {}
static void applyARate() {}
// 7.14: keyring mode's own state. The RTC_DATA_ATTR ones are the whole
// point of the rebuild, so the test keeps them apart from the rest and
// wipes only the others to model a wake from deep sleep.
#define KEY_NOTICE_MS   10000UL
#define KEY_LISTEN_MS  120000UL
#define KEY_DIZZY_S          8
#define KEY_DIZZY_N          3
#define KEY_CARD_MS      2400UL
#define KEYCARD_N            6
int cfgKeyCard = 0, cfgNet = 1, cfgCpu = 0;
#define NET_BT 1
uint8_t rtcKeyShut = 0, rtcKeyCard = 0, rtcKeyPokes = 0;
uint32_t rtcKeyPokeAt = 0;
bool keyDark = false, keyLit = false;
uint32_t lastDraw = 0, keyLastPoke = 0;
static bool keyringNow() { return cfgKeyring && !keyOut; }
static void drawKeyNotice(uint32_t s) { rec("notice " + std::to_string(s)); }
static void drawKeyCard() { rec("card"); }
static void screenPower(bool on) { rec(on ? "screen on" : "screen off"); }
// 7.15: a clock the test can move. Left at nought it is the real
// one, so every test written before this still sees what it saw.
static time_t fakeEpoch = 0;
static time_t fwTime(time_t* p) {
  if (!fakeEpoch) return ::time(p);
  if (p) *p = fakeEpoch;
  return fakeEpoch;
}
#define time fwTime

// 7.15: the radio that is off unless the clock is due.
#define SMART_WINDOW_MS 45000UL
#define SMART_EVERY_S   86400L
#define SMART_RETRY_S     3600L
#define SMART_RETRY_MS   3600000UL
#define CLOCK_SANE     1735689600L
enum { CTS_IDLE = 0, CTS_DONE, CTS_NONE };
int ctsState = CTS_IDLE;
bool btUp = false;
uint8_t fakeBonds = 1;
static uint8_t bondsKnown() { return fakeBonds; }
static void bleOn()  { if (btUp) return; btUp = true;  rec("bleOn"); }
static void bleOff() { btUp = false; rec("bleOff"); }
uint32_t smartOnAt = 0, smartRetryMs = 0;
bool     smartSettled = false;
time_t   smartNextS = 0, smartRetryS = 0;
static void setCpuFrequencyMhz(int m) { rec("cpu " + std::to_string(m)); }
static void keyringDeep() { rtcKeyShut = 1; rec("deep sleep"); }
struct { void clearDisplay() {} void display() {} } oled;
struct { void disconnect(bool, bool) { rec("wifi off"); } void mode(int) {} } WiFi;
#define WIFI_OFF 0
int nightPush = 0; uint32_t nightCardUntil = 0;
bool tmrOn = false, tmrDone = false; long tmrSetTo = -999, tmrLeftNow = 0;
uint32_t relaxUntil = 0;
#define RELAX_RQ_MS 180000UL
#define S_FAITH 7
#define F_ZIKR 0
static void zikrReset() { rec("zikrReset"); }
static void tmrSet(long secs) { tmrSetTo = secs; tmrOn = true; tmrDone = secs <= 0; rec("tmrSet " + std::to_string(secs)); }
static long tmrLeft() { return tmrLeftNow; }
static void tmrStop(const char* why) { tmrOn = false; rec("tmrStop"); }
static void awaySet(bool on, const char* t) { awayOn = on; rec(std::string("awaySet ") + (on ? "on " : "off ") + (t ? t : "")); }
static bool rafiqTagged(const Note& n);
static void saveAdj() { rec("saveAdj"); }
static void rqRemind(const char* s) { rec(std::string("rqRemind ") + s); }
static int popupSecs() { return 5; }
static void popupShow() { rec("popupShow"); }
struct { void printf(const char*, ...) {} void println(const char*) {} } Serial;

#include "fw_funcs.inc"

// ---- the tests ----
int fails = 0, passes = 0;
#define CHECK(c, msg) do { if (c) passes++; else { fails++; printf("FAIL: %s  (line %d)\n", msg, __LINE__); } } while (0)

static void reset() {
  calls.clear(); rqPend = RQ_NONE; stMsg[0] = stSub[0] = stDate[0] = 0;
}
// the phone's wall clock, offset seconds from now, as ANCS sends it
static void dateIn(int offset) {
  time_t t = time(nullptr) + offset; struct tm lt; localtime_r(&t, &lt);
  snprintf(stDate, sizeof(stDate), "%04d%02d%02dT%02d%02d%02d", lt.tm_year + 1900, lt.tm_mon + 1,
           lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
}
static Note note(const char* app, const char* title) {
  Note n = {}; n.uid = 77; strncpy(n.app, app, 39); strncpy(n.title, title, 33); return n;
}

int main() {
  setenv("TZ", "UTC0", 1); tzset();

  // -- who counts as RAFIQ --
  reset(); strcpy(stMsg, "sync");
  CHECK(rafiqIs(note("com.apple.shortcuts", "RAFIQ")), "Shortcut titled RAFIQ is a command");
  CHECK(rafiqIs(note("com.apple.shortcuts", "rafiq")), "case does not matter");
  CHECK(!rafiqIs(note("net.whatsapp.WhatsApp", "Rafiq")), "a WhatsApp from a person named Rafiq is NOT a command");
  CHECK(!rafiqIs(note("com.apple.MobileSMS", "RAFIQ")), "SMS titled RAFIQ is not a command");
  CHECK(!rafiqIs(note("com.apple.shortcuts", "Rafiqa")), "Rafiqa is not RAFIQ");
  CHECK(!rafiqIs(note("com.apple.shortcuts", "Morning routine")), "other shortcuts are not commands");
  strcpy(stSub, "RAFIQ"); CHECK(rafiqIs(note("com.apple.shortcuts", "My shortcut")), "RAFIQ in the subtitle counts");
  stSub[0] = 0; strcpy(stMsg, "RAFIQ: sync"); CHECK(rafiqIs(note("com.apple.shortcuts", "Run")), "RAFIQ at the start of the text counts");
  CHECK(!strcmp(rqAfterTag("RAFIQ: sync"), "sync"), "tag stripped with colon");
  CHECK(!strcmp(rqAfterTag("  rafiq - focus 25"), "focus 25"), "tag stripped with dash and spaces");
  CHECK(!strcmp(rqAfterTag("sync"), "sync"), "untagged text unchanged");
  // 6.0.1: every Shortcuts process counts, nothing else does
  stSub[0] = 0; strcpy(stMsg, "sync");
  CHECK(rafiqIs(note("com.apple.WorkflowKit.BackgroundShortcutRunner", "RAFIQ")), "6.0.1: automation runner counts");
  CHECK(rafiqIs(note("is.workflow.my.app", "RAFIQ")), "6.0.1: old Workflow id counts");
  CHECK(rafiqIs(note("com.apple.ShortcutsActions", "RAFIQ")), "6.0.1: any shortcuts process counts");
  CHECK(!rafiqIs(note("net.whatsapp.WhatsApp", "Rafiq")), "6.0.1: WhatsApp from Rafiq still refused");
  CHECK(!rafiqIs(note("ph.telegra.Telegraph", "RAFIQ")), "6.0.1: Telegram still refused");
  CHECK(rafiqTagged(note("net.whatsapp.WhatsApp", "Rafiq")), "6.0.1: refused one is still recognised as tagged, so it can say why");

  // -- freshness --
  reset(); dateIn(-30);  CHECK(rqFresh(stDate), "30 s old is fresh");
  reset(); dateIn(-119); CHECK(rqFresh(stDate), "119 s old is fresh");
  reset(); dateIn(-200); CHECK(!rqFresh(stDate), "200 s old is stale");
  reset(); dateIn(-86400); CHECK(!rqFresh(stDate), "yesterday is stale");
  reset(); dateIn(240);  CHECK(rqFresh(stDate), "4 min ahead allowed (clock skew)");
  reset(); dateIn(600);  CHECK(!rqFresh(stDate), "10 min ahead is not");
  CHECK(rqFresh(""), "no date: run-once decides alone");
  CHECK(rqFresh("garbageXgarbage"), "malformed date: run-once decides alone");
  timeOk = false; reset(); dateIn(-86400); CHECK(rqFresh(stDate), "no clock yet: run-once decides alone"); timeOk = true;

  // -- run once --
  prefs.kv.clear(); rqSeenLoaded = false;
  reset(); dateIn(-5); strcpy(stMsg, "find");
  rafiqNote(note("com.apple.shortcuts", "RAFIQ"));
  CHECK(findUntil > 0, "find ran the first time");
  CHECK(called("ancsAction 1"), "asked the phone to clear it");
  findUntil = 0; calls.clear();
  rafiqNote(note("com.apple.shortcuts", "RAFIQ"));            // same date, same text: re-delivered
  CHECK(findUntil == 0, "the same notification delivered again does not run again");
  rqSeenLoaded = false;                                        // as after a reboot: read back from flash
  rafiqNote(note("com.apple.shortcuts", "RAFIQ"));
  CHECK(findUntil == 0, "nor after a restart (seen list is in flash)");
  reset(); dateIn(-5); strcpy(stMsg, "find me");
  rafiqNote(note("com.apple.shortcuts", "RAFIQ"));
  CHECK(findUntil > 0, "a different command in the same second does run");
  // stale command ignored, stale weather still applied
  findUntil = 0; reset(); dateIn(-3600); strcpy(stMsg, "find");
  rafiqNote(note("com.apple.shortcuts", "RAFIQ"));
  CHECK(findUntil == 0, "an hour-old command is ignored");
  reset(); dateIn(-3600); strcpy(stMsg, "Temp: 19C\nCondition: Rain");
  rafiqNote(note("com.apple.shortcuts", "RAFIQ"));
  CHECK(fabsf(wTemp - 19) < 0.01 && wCode == 61, "an hour-old weather report is still applied");

  // -- commands --
  struct { const char* in; int pend; } radio[] = {
    { "sync", RQ_SYNC }, { "Sync.", RQ_SYNC }, { "SYNC!", RQ_SYNC }, { "update", RQ_UPDATE },
    { "wifi", RQ_WIFI }, { "Wi-Fi", RQ_WIFI }, { "config", RQ_HOTSPOT }, { "config-robo", RQ_HOTSPOT },
    { "hotspot", RQ_HOTSPOT }, { "tamper on", RQ_TAMPER }, { "deep sleep", RQ_DEEP }, { "reboot", RQ_REBOOT },
    { "prayer refresh", RQ_SYNC } };
  for (auto& r : radio) { reset(); CHECK(rqCommand(r.in) == 1 && rqPend == r.pend, r.in); }
  reset(); CHECK(rqCommand("guard on") && cfgPGuard, "guard on");
  reset(); CHECK(rqCommand("Guard  Off") && !cfgPGuard, "guard off, odd spacing");
  reset(); CHECK(rqCommand("focus 40") == 0 && !called("focusBegin"), "6.1: focus is gone");
  reset(); CHECK(rqCommand("focus stop") == 0, "6.1: focus stop is gone");
  reset(); CHECK(rqCommand("bright 50") && cfgBright == 127, "bright 50 -> contrast 127");
  reset(); CHECK(rqCommand("bright 1") && cfgBright == 26, "bright floor is 10%");
  reset(); CHECK(rqCommand("bright") == 0, "bright with no number is not a command");
  reset(); CHECK(rqCommand("face 3") && cfgFace == 2, "face is 1-based");
  reset(); CHECK(rqCommand("face 99") && cfgFace == FACE_N - 1, "face capped");
  reset(); CHECK(rqCommand("notifications off") && cfgQuiet, "notifications off");
  reset(); CHECK(rqCommand("notifications on") && !cfgQuiet, "notifications on");

  // 7.13: a robot told not to show notifications has no reason to hold
  // a link open, and one told to show them has every reason to.
  reset(); cfgBtMode = BTM_ALWAYS;
  CHECK(rqCommand("notifications off") && cfgBtMode == BTM_SMART,
        "notifications off puts the radio in smart mode");
  reset(); cfgBtMode = BTM_SMART;
  CHECK(rqCommand("notifications on") && cfgBtMode == BTM_ALWAYS,
        "notifications on puts the radio back on");
  reset(); cfgBtMode = BTM_SMART; cfgQuiet = true;
  CHECK(rqCommand("quiet off") && cfgBtMode == BTM_ALWAYS,
        "quiet off is the same thing by another name");

  // The way out of keyring mode that does not need your hands on it.
  reset(); cfgKeyring = true; keyOut = false;
  CHECK(rqCommand("home") && !cfgKeyring && keyOut,
        "home lets it out of keyring mode");
  reset(); cfgKeyring = false;
  CHECK(rqCommand("home") && !cfgKeyring,
        "home on a normal robot is just home");
  reset(); CHECK(rqCommand("prayer fajr +2") && prayerAdj[0] == 2, "prayer fajr +2");
  reset(); CHECK(rqCommand("prayer maghrib -3") && prayerAdj[3] == -3, "prayer maghrib -3");
  reset(); CHECK(rqCommand("prayer zuhr 5") && prayerAdj[1] == 5, "zuhr is dhuhr");
  reset(); CHECK(rqCommand("prayer isha +500") && prayerAdj[4] == 60, "prayer offset capped at 60");
  reset(); CHECK(rqCommand("prayer lunch +2") == 0, "unknown prayer is not a command");
  reset(); CHECK(rqCommand("status") && called("rqStatus"), "status");
  reset(); CHECK(rqCommand("hello there") == 0, "unknown words are not a command");
  reset(); CHECK(rqCommand("") == 0, "empty line");

  // -- payloads --
  reset(); rafiqPayload("msg: Running late, 10 min", true);
  CHECK(message == "Running late, 10 min" && called("addNote Running late") && called("popupShow"), "msg lands in Notifications and pops up");
  reset(); rafiqPayload("message Line one\nline two", true);
  CHECK(message == "Line one line two", "a multi-line message is joined");
  reset(); message = "old"; rafiqPayload("msg: stale", false);
  CHECK(message == "old", "a stale msg is ignored");
  reset(); rafiqPayload("remind 18:30 Call mom", true);
  CHECK(called("rqRemind  18:30 Call mom") || called("rqRemind 18:30 Call mom"), "remind hands over time and text");
  reset(); cfgPGuard = false; rafiqPayload("guard on\nfind", true);
  CHECK(cfgPGuard && findUntil > 0 && called("wake shortcut"), "two commands on two lines, screen woken");
  reset(); rafiqPayload("relax;notifications off", true);
  CHECK(relaxOn && cfgQuiet, "two commands split by a semicolon");
  reset(); asleep = true; wTemp = NAN;
  rafiqPayload("temp=24;cond=Sunny;hum=60;wind=12;city=Bengaluru", true);
  CHECK(fabsf(wTemp - 24) < .01 && fabsf(wHum - 60) < .01 && fabsf(wWind - 12) < .01 && wCode == 0 && !strcmp(wCity, "Bengaluru"),
        "key=value weather");
  CHECK(!called("wake"), "a weather report alone does not wake the screen");
  CHECK(wxOk && wxDirty, "weather marked for saving");
  asleep = false;
  reset(); rafiqPayload("Temp: 21°C (H: 25°C, L: 18°C)\nCondition: Mostly Cloudy\nHumidity: 88%\nRain Chance: 20%\n"
                        "Wind: 12 km/h\nUV Index: 5\nSunrise: 5 Oct 2026 at 6:12 AM", true);
  CHECK(fabsf(wTemp - 21) < .01 && fabsf(wHum - 88) < .01 && wCode == 3 && fabsf(wWind - 12) < .01,
        "C3Buddy's long-form weather works unchanged");
  reset(); rafiqPayload("Temperature: 77°F\nCondition: Thunderstorms", true);
  CHECK(fabsf(wTemp - 25) < .01 && wCode == 95, "Fahrenheit converted, thunder mapped");
  reset(); rafiqPayload("Wind: 10 mph\nTemp: 10", true);
  CHECK(fabsf(wWind - 16.09) < .05, "mph converted");
  reset(); rafiqPayload("what is this", true);
  CHECK(called("flash RAFIQ?"), "nonsense says so");

  // -- 6.4 away and wake by --
  reset(); awayOn = false; rafiqPayload("away: Back at 3. Call Ahmed", true);
  CHECK(awayOn && called("awaySet on Back at 3. Call Ahmed"), "away: text turns it on with the text");
  reset(); findUntil = 0; rafiqPayload("find", true);
  CHECK(findUntil == 0 && awayOn, "while away, other commands are ignored");
  reset(); rafiqPayload("sync", true);
  CHECK(rqPend == RQ_NONE && awayOn, "while away, sync is ignored");
  reset(); awayOn = false; rafiqPayload("Away message: Back at 3", true);
  CHECK(called("awaySet on Back at 3"), "6.9: the word message is not shown");
  reset(); awayOn = false; rafiqPayload("away msg: Gone home", true);
  CHECK(called("awaySet on Gone home"), "6.9: nor msg");
  reset(); awayOn = false; rafiqPayload("away - message - Call Ahmed", true);
  CHECK(called("awaySet on Call Ahmed"), "6.9: dashes and label stripped");
  reset(); awayOn = false; rafiqPayload("away: Messages go to Sara", true);
  CHECK(called("awaySet on Messages go to Sara"), "6.9: a message that starts with Messages keeps it");
  reset(); awayOn = false; rafiqPayload("away message Out for lunch", true);
  CHECK(called("awaySet on Out for lunch"), "6.9: away message text, no colon");
  reset(); rafiqPayload("away: new text", true);
  CHECK(called("awaySet on new text"), "away: updates the message while away");
  reset(); rafiqPayload("home", false);
  CHECK(awayOn, "a stale home does not end away");
  reset(); rafiqPayload("Home.", true);
  CHECK(!awayOn && called("awaySet off"), "home ends away");
  reset(); rafiqPayload("away", true);
  CHECK(awayOn && called("awaySet on "), "away alone turns it on with the last text");
  awayOn = false;
  reset(); CHECK(rqCommand("wake touch") && cfgWakeBy == 1, "wake touch");
  reset(); CHECK(rqCommand("wake shake") && cfgWakeBy == 2, "wake shake");
  reset(); CHECK(rqCommand("wake both") && cfgWakeBy == 0, "wake both");
  reset(); CHECK(rqCommand("wake by") && cfgWakeBy == 1, "wake by cycles");

  // -- 7.0 timer, zikr, relax --
  tmrOn = false;
  reset(); CHECK(rqCommand("timer 15") && tmrSetTo == 900, "timer 15 is 15 minutes");
  reset(); CHECK(rqCommand("Timer 10.") && tmrSetTo == 600, "Timer 10. with capital and dot");
  reset(); CHECK(rqCommand("timer") == 0, "timer with no number is not a command");
  reset(); CHECK(rqCommand("timers 5") == 0, "timers is not timer");
  reset(); tmrOn = true; tmrDone = false; tmrLeftNow = 300;
  rafiqPayload("timer +5", true); CHECK(tmrSetTo == 600, "timer +5 adds to what is left");
  reset(); tmrOn = true; tmrLeftNow = 900;
  rafiqPayload("timer -10", true); CHECK(tmrSetTo == 300, "timer -10 takes off");
  reset(); tmrOn = true; tmrLeftNow = 120;
  rafiqPayload("timer -10", true); CHECK(tmrSetTo <= 0, "taking off more than is left ends it");
  reset(); tmrOn = true; tmrDone = false; tmrLeftNow = 500;
  rafiqPayload("timer 20", true); CHECK(tmrSetTo == 1200, "a new timer replaces the running one");
  reset(); tmrOn = true; findUntil = 0; rafiqPayload("find", true);
  CHECK(findUntil == 0 && tmrOn, "while a timer runs, other commands are ignored");
  reset(); tmrOn = true; rafiqPayload("Home.", true);
  CHECK(!tmrOn && called("tmrStop"), "home stops the timer");
  reset(); tmrOn = false; tmrSetTo = -999; rafiqPayload("timer +5", true);
  CHECK(tmrSetTo == 300, "timer +5 with nothing running starts five minutes");
  reset(); tmrOn = false; CHECK(rqCommand("zikr") && called("zikrReset") && screen == S_FAITH, "zikr opens zikr");
  reset(); CHECK(rqCommand("relax") && relaxOn && relaxUntil > 0, "relax runs with a three minute end");
  // 7.7: night sleep, an hour later, from a Shortcut; twice is two hours
  nightPush = 0; reset(); CHECK(rqCommand("night later") && nightPush == 60, "night later pushes bedtime an hour");
  reset(); CHECK(rqCommand("night +1") && nightPush == 120, "night +1 pushes it another hour");
  nightPush = 300; reset(); rqCommand("night later"); rqCommand("night later"); CHECK(nightPush == 360, "the push stops at six hours");
  nightPush = 0;
  reset(); CHECK(rqCommand("relax off") == 0, "relax off is gone");

  // ================================================================
  //  7.14: notifications own the radio
  // ================================================================
  reset(); cfgQuiet = false; cfgBtMode = BTM_ALWAYS;
  setNotes(false, false);
  CHECK(cfgQuiet && cfgBtMode == BTM_SMART, "notifications off puts the radio on the daily clock");
  reset(); setNotes(true, false);
  CHECK(!cfgQuiet && cfgBtMode == BTM_ALWAYS, "notifications on keeps the link up");
  reset(); cfgQuiet = true; cfgBtMode = BTM_ALWAYS;   // disagreeing, as 7.13 allowed
  setNotes(true, false);
  CHECK(!cfgQuiet && cfgBtMode == BTM_ALWAYS, "the two can no longer disagree");

  // ================================================================
  //  7.14: keyring mode, the state machine
  // ================================================================
  //  The thing this replaces: in 7.13 keyShut and keyBootAt were plain
  //  globals, so every wake from deep sleep came back with keyShut
  //  false and handed out another thirty seconds of the ordinary
  //  robot, radios and all. These tests model a wake by wiping exactly
  //  what RAM loses and keeping exactly what RTC memory keeps.
  auto keyFresh = [&]() {                       // power on, in keyring mode
    cfgKeyring = true; keyOut = false;
    rtcKeyShut = 0; rtcKeyPokes = 0; rtcKeyPokeAt = 0; rtcKeyCard = 0;
    keyDark = keyLit = false; keyBootAt = 0; keyCardUntil = 0;
    keyPokes = 0; keyDizzy = false; lastDraw = 0; fakeNow = 1000;
  };
  auto keyWake = [&]() {                        // what a deep sleep wake leaves behind
    keyDark = keyLit = false; keyBootAt = 0; keyCardUntil = 0;
    keyPokes = 0; keyDizzy = false; lastDraw = 0; fakeNow = 1000;
  };

  reset(); keyFresh();
  CHECK(keyTick() && called("notice"), "power on shows the notice");
  reset(); fakeNow = 1000 + 4000;
  keyTick();
  CHECK(called("notice 6"), "the notice counts down");

  reset(); fakeNow = 1000 + KEY_NOTICE_MS;
  CHECK(keyTick() && keyDark && called("screen off"), "ten seconds later the screen goes out");

  reset(); fakeNow = 1000 + KEY_NOTICE_MS + 5000; keyCardUntil = fakeNow + 1000;
  CHECK(keyTick() && called("card") && called("screen on"), "a poke inside the window still shows a card");
  reset(); fakeNow += 2000;                     // the card has run out
  keyTick();
  CHECK(called("screen off") && !keyLit, "and the screen goes out again after it");

  reset(); fakeNow = 1000 + KEY_LISTEN_MS;
  CHECK(keyTick() && called("deep sleep") && called("bleOff") && called("wifi off"),
        "two minutes with no home and both radios go");
  CHECK(called("cpu 80") && rtcKeyShut == 1, "it drops to 80 MHz and latches shut");

  //  The regression. A wake must not reopen anything.
  reset(); keyWake();
  CHECK(keyTick() && called("deep sleep"), "a wake goes straight back to sleep");
  CHECK(!called("notice") && !called("screen on"), "a wake shows no notice and no menu");
  int winds = 0;
  for (int i = 0; i < 20; i++) { reset(); keyWake(); keyTick(); if (called("notice")) winds++; }
  CHECK(winds == 0, "twenty wakes, and not one of them hands out a window");

  //  Let out, and it is an ordinary robot again.
  reset(); keyOut = true; cfgKeyring = false;
  CHECK(!keyTick(), "once it is out, keyring mode takes nothing");

  //  The card a wake will show is kept where a wake can read it.
  reset(); cfgKeyCard = 3; keyArm();
  CHECK(rtcKeyCard == 3, "arming remembers the card in RTC memory");

  // ================================================================
  //  7.15: the radio is off unless the clock is due
  // ================================================================
  //  What this replaces: smartNext and smartRetryAt were ordinary
  //  globals holding millis() readings. A deep sleep wake clears the
  //  first and restarts the second at nought, so every wake came back
  //  believing the clock had never been fetched and opened a forty
  //  five second radio window. The radio was never off for longer than
  //  one wake, which is the whole of "it keeps saying connecting".
  //
  //  As with keyring mode, a wake is modelled by wiping exactly what
  //  RAM loses and keeping exactly what RTC memory keeps.
  auto smartFresh = [&]() {                     // power on
    cfgNet = NET_BT; cfgBtMode = BTM_SMART; cfgKeyring = false; keyOut = false;
    btUp = false; ctsState = CTS_IDLE; fakeBonds = 1;
    smartNextS = 0; smartRetryS = 0;            // RTC memory, cleared by a power cut
    smartOnAt = 0; smartRetryMs = 0; smartSettled = false;
    fakeNow = 1000; fakeEpoch = 1760000000L;
  };
  auto smartWake = [&]() {                      // what a deep sleep wake leaves behind
    btUp = false; ctsState = CTS_IDLE;
    smartOnAt = 0; smartRetryMs = 0; smartSettled = false;
    fakeNow = 1000;                             // millis() starts again at nought
  };

  reset(); smartFresh();
  CHECK(smartDue(), "a power on is always owed a clock");
  smartTick();
  CHECK(called("bleOn") && btUp, "and brings the radio up for it");
  ctsState = CTS_DONE; fakeNow += 2000;
  reset(); smartTick();
  CHECK(called("bleOff") && !btUp, "the clock lands and the radio goes");
  CHECK(smartNextS == fakeEpoch + SMART_EVERY_S, "the next one is a day away");
  CHECK(smartSettled, "and this wake is finished with it");

  //  The regression, and the reason for all of it.
  time_t keptNext = smartNextS;
  reset(); smartWake();
  CHECK(!smartDue(), "a wake an instant later is not owed a clock");
  smartTick();
  CHECK(!called("bleOn") && !btUp, "so a wake does not bring the radio up");
  CHECK(smartNextS == keptNext, "and the day's appointment is still the same one");
  int ups = 0;
  for (int i = 0; i < 20; i++) { reset(); smartWake(); smartTick(); if (called("bleOn")) ups++; }
  CHECK(ups == 0, "twenty wakes, and the radio stays off for every one");

  //  A day later it is owed one again.
  reset(); smartWake(); fakeEpoch = keptNext + 1;
  CHECK(smartDue(), "a day later the clock is due again");
  smartTick();
  CHECK(called("bleOn"), "and the radio comes up, once");

  //  Nobody answered.
  reset(); smartFresh(); smartTick();
  reset(); fakeNow += SMART_WINDOW_MS + 1;
  smartTick();
  CHECK(called("bleOff") && !btUp, "nobody answered, and the radio goes anyway");
  CHECK(smartRetryS == fakeEpoch + SMART_RETRY_S, "it waits an hour before asking again");
  reset(); smartWake();
  CHECK(!smartDue(), "and the hour survives a wake, as the day does");

  //  Never paired. There is nothing to be quiet about.
  reset(); smartFresh(); fakeBonds = 0;
  smartTick();
  reset(); fakeNow += SMART_WINDOW_MS + 1;
  smartTick();
  CHECK(!called("bleOff") && btUp, "with nothing ever paired it stays up to be found");

  //  Notifications on is the other mode, and this one keeps out of it.
  reset(); smartFresh(); cfgBtMode = BTM_ALWAYS;
  smartTick();
  CHECK(!called("bleOn"), "with notifications on the daily clock has no say");
  fakeEpoch = 0;                                // back to the real clock

  // -- weather words --
  struct { const char* w; int c; } wx[] = { { "Sunny", 0 }, { "Clear", 0 }, { "Partly Cloudy", 2 },
    { "Mostly Cloudy", 3 }, { "Cloudy", 3 }, { "Overcast", 3 }, { "Fog", 45 }, { "Haze", 45 },
    { "Drizzle", 51 }, { "Light Rain", 61 }, { "Rain Showers", 80 }, { "Snow", 71 },
    { "Thunderstorms", 95 }, { "Something new", -1 } };
  for (auto& w : wx) CHECK(wxCodeFromWords(w.w) == w.c, w.w);

  printf("\n%d passed, %d failed\n", passes, fails);
  return fails ? 1 : 0;
}

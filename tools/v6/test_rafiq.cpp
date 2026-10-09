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
#define FACE_N 20
#define S_HOME 0
#define S_MSG 5
const char* PRAYERS[5] = { "Fajr", "Dhuhr", "Asr", "Maghrib", "Isha" };
int prayerAdj[5] = { 0, 0, 0, 0, 0 };
static uint32_t millis() { return 123456; }

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
struct { void printf(const char*, ...) {} } Serial;

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

  // -- weather words --
  struct { const char* w; int c; } wx[] = { { "Sunny", 0 }, { "Clear", 0 }, { "Partly Cloudy", 2 },
    { "Mostly Cloudy", 3 }, { "Cloudy", 3 }, { "Overcast", 3 }, { "Fog", 45 }, { "Haze", 45 },
    { "Drizzle", 51 }, { "Light Rain", 61 }, { "Rain Showers", 80 }, { "Snow", 71 },
    { "Thunderstorms", 95 }, { "Something new", -1 } };
  for (auto& w : wx) CHECK(wxCodeFromWords(w.w) == w.c, w.w);

  printf("\n%d passed, %d failed\n", passes, fails);
  return fails ? 1 : 0;
}

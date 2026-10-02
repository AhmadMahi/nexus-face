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
#include "arabic_glyphs.h"

#define SDA_PIN 8
#define SCL_PIN 9

// The accelerometer's INT1 line. Wire it to this pin and the robot can
// sleep properly: the chip watches for movement by itself and pulls the
// line high, which is the only thing that can wake a processor that has
// been switched off. Leave it unwired and everything still works, it
// simply never goes deeper than a dark screen. It is found at boot
// rather than assumed, so one unit having the wire and another not is
// not something you have to keep track of.
//
// GPIO4 because the C3 can only wake on 0 to 5, and 2 is a strapping pin.
#define TAP_INT_PIN 4

// A touch pad on GPIO5, which is free: 8 and 9 are the I2C lines, 4 is
// the accelerometer interrupt, and 2, 8 and 9 are the strapping pins.
//
// Read as an ordinary digital input, not with touchRead. The C3 has no
// capacitive touch peripheral at all, unlike the original ESP32 and the
// S2 and S3, so a pad on this chip means a little board that does its
// own sensing and hands over a level.
//
// On a TTP223 the two solder pads on the back decide whether the output
// is high or low when touched, and whether it is momentary or latching.
// Four combinations, and the board does not say which it is in.
//
// So nothing is assumed. Resting is whatever level the pin has held for
// the last minute, and a touch is anything else. A pad spends almost
// all its life untouched, so the level it sits at longest is the level
// it sits at. That is true of all four combinations and needs nothing
// to be configured.
//
// It also digs itself out of the one thing that can go wrong. The
// TTP223 calibrates itself in the first half second of power, so a
// finger on the pad while the robot starts makes the chip treat a
// touched pad as its baseline. The level read at boot is then the wrong
// one, and a minute later this notices and takes the other one.
//
// v2.15.0 did this by taking a sample at boot and flipping it if a
// touch lasted thirty seconds. That is wrong for a TTP223 wired to
// latch: its output stays put until the next touch, quite legitimately,
// and the flip would have fought it.
// ---------------- the battery ----------------
//  Two 200K resistors from the pack to ground with GPIO1 in the middle,
//  so the pin sees exactly half the pack voltage and a full cell lands
//  near 2.1V, inside what the ADC can read.
//
//  analogReadMilliVolts rather than analogRead: it applies the
//  calibration burned into the chip at the factory, which is worth a
//  good hundred millivolts of accuracy, and a hundred millivolts is
//  most of the difference between half full and nearly flat.
//
//  Worth knowing: two 200K resistors put 100K in front of the ADC, and
//  this one would rather see something under about 10K. It charges a
//  small capacitor each sample and 100K refills it slowly, so the first
//  read after a gap comes back low. Reading several times in a row and
//  throwing the first away is most of the fix. A 100nF capacitor from
//  GPIO1 to ground would be the rest of it, if the numbers look low.
#define BATT_PIN  1
#define BATT_MUL  2              // 200K over 200K, so half of the pack
float    battV = NAN;
uint32_t battNext = 0;
// What this pack reads when it is full. Measured at 4.09V it was being
// called 89%, which is exactly where 4.09 lands on a curve drawn for a
// cell that charges to 4.20. It does not; most do not quite. The whole
// curve is scaled to whatever this says, so the top of the charge is
// the top of the scale.
float    battFull = 4.10f;

#define TOUCH_PIN 5
bool     touchRest  = false;     // the level it sits at with nobody near
bool     touchOn    = false;     // a finger is on it right now
uint32_t touchCount = 0;
uint32_t touchEdge  = 0;         // when the level last disagreed with us
bool     touchLvl   = false;     // what the pin read last time
uint32_t touchLvlAt = 0;         // when it last changed
#define TOUCH_REST_MS 60000UL    // held this long and it is the resting level

// ---------------- what a touch means ----------------
//  The pad is the way this is driven now, and knocking is the thing you
//  switch on if you want it. Four gestures:
//
//    one     next. The next screen, the next thing in a list
//    long    in. Open the thing you are looking at
//    two     back, out one level
//    three   straight home from wherever you are
//
//  Press and hold to go in, which is how holding works nearly
//  everywhere else, and it leaves a double free to mean back.
//
//  Somewhere there is nothing left to open, that rule has nothing to
//  say, so a leaf reads differently: in a reader, in zikr and while
//  walking the faces, one is the next, two is the one before, and a
//  long press is the way out. Both are natural where they are.
//
//  A single cannot be acted on the moment you lift, because a second
//  might be coming: it waits TOUCH_GAP_MS first. A double waits the
//  same again in case a third is coming. A triple does not wait at all,
//  since nothing longer can follow it, and a long press fires while
//  your finger is still down, which is why it feels the most immediate
//  of the four.
#define TOUCH_LONG_MS 700UL      // held this long and it is a long press
// Keep holding and it keeps meaning more. Nothing is shown for the
// first five seconds, because a bar on screen during ordinary use was
// worse than the thing it explained; nobody holds for five seconds by
// accident, so after that it is safe to say what is about to happen.
#define TOUCH_HOME_MS  5000UL
#define TOUCH_SLEEP_MS 10000UL
#define TOUCH_COUNT_MS  3000UL   // and then it counts three and goes
#define TOUCH_GAP_MS  300UL      // quiet for this long and the count is final
#define TOUCH_DEBOUNCE 40UL
enum { TG_ONE = 1, TG_TWO, TG_THREE, TG_LONG };
uint32_t touchPressAt = 0;       // when the finger went down
uint32_t touchLiftAt  = 0;       // when it last came up
uint8_t  touchTaps    = 0;       // lifts so far in this run
bool     touchLongDone = false;  // the long press already fired this press
uint8_t  touchHold    = 0;       // 0 nothing, 1 past home, 2 past sleep
bool     wantDeep     = false;   // asked for, by holding or by the Mac letting go
// It came back from being switched off because a reminder or a prayer
// was due, and that is the only reason it is on. Waking that way is a
// boot, so `asleep` is false by the time the reminder fires and
// nothing downstream could tell this from someone picking it up.
bool     wokeForAlarm = false;


// Changing the watch face from the clock, with the pad. A long press on
// the clock goes in, the screen blinks once to say so, single presses
// walk the faces and a long press leaves. It is a mode rather than a
// gesture because there are twenty faces and you want to sit in it.
bool     faceMode = false;

// Knocking. Off unless you ask for it, and when it is on it keeps every
// meaning it has always had rather than learning the pad's.
bool cfgKnock = false;
// A shake steps back one. On by default, because with the pad doing
// everything a second way out is worth having, and a shake is the one
// gesture you can make without looking at the thing.
// How you go back a level. Two presses, a shake, or either.
//
// It was a plain on/off for the shake, which left no way to say "the
// shake only": a double press always went back whatever you set. Three
// values, walked by holding on the row the way every other setting is
// walked, and shown as the value so you can see where you are without
// opening anything.
//
// A long press still goes home from anywhere and three presses still
// go home, so there is no setting here that can strand you.
enum { BACK_TOUCH = 0, BACK_SHAKE, BACK_BOTH, BACK_N };
int cfgBack = BACK_BOTH;
const char* BACK_NAME[BACK_N] = { "touch", "shake", "both" };
static bool backByTouch() { return cfgBack != BACK_SHAKE; }
static bool backByShake() { return cfgBack != BACK_TOUCH; }
// Kept so the Mac can still send and read shake=0/1 without knowing
// about any of this.
#define cfgShake (backByShake())

// How long with nothing happening before it switches off properly,
// rather than just turning the screen off. Separate from the screen
// timeout on purpose: the screen can go dark after fifteen seconds
// without it mattering, but powering down drops the network, and
// coming back costs a few seconds of reconnecting before anything
// works. Two minutes is long enough not to be in the way.
const uint16_t DEEP_OPTS[] = { 60, 120, 300, 600, 1800, 0 };
const char* DEEP_NAME[] = { "1m", "2m", "5m", "10m", "30m", "never" };
#define DEEP_N 6
int cfgDeepIdx = 1;
static uint32_t deepAfterMs() { return (uint32_t)DEEP_OPTS[cfgDeepIdx] * 1000UL; }
#define OLED_ADDR 0x3C
#define SCRW 128                 // RoboEyes owns W and H, so ours differ
#define SCRH 64

#define FW_VERSION "5.4.0"
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
#define A_THRESH_ACT 0x24
#define A_ACT_INACT_CTL 0x27
#define A_INT_MAP 0x2F
#define INT_ACT 0x10
#define INT_DATARDY 0x80
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
enum { S_HOME = 0, S_BIKE, S_REMIND, S_FOCUS, S_WEATHER, S_MSG, S_PRAYER,
       S_FAITH, S_READS, S_GAMES, S_SETTINGS, S_SYSTEM, S_COUNT };
const char* S_NAME[S_COUNT] =
  { "HOME", "VEHICLE", "REMINDERS", "FOCUS", "WEATHER", "MESSAGES", "PRAYER",
    "FAITH", "SHORT READS", "GAMES", "SETTINGS", "SYSTEM" };

// ---------------- online, or not ----------------
//  Two different things, and keeping them apart matters.
//
//  cfgOffline is a decision: you said stay off the network. The radio
//  goes off and stays off until you say otherwise.
//
//  netDown is a fact: it tried and there was nothing there. The radio
//  goes off too, because a chip scanning for a network that is not
//  coming back is just a way of spending the battery, but it tries
//  again on the next wake.
//
//  Either way the robot works. Offline is a mode, not a fault.
bool cfgOffline = false;
bool netDown = false;
int  netMisses = 0;                // failed joins since the last success
bool hadNet = false;               // it has been online at least once this time up
static bool offlineNow() { return cfgOffline || netDown; }

// The vehicle screen, off until you ask for it.
//
//  Make and model are two fields rather than one because
//  "Royal Enfield Meteor 350" is twenty four characters and the screen
//  is twenty one wide. Split at the source and every template can put
//  them where it has the room, instead of each one guessing where to
//  break a string someone typed.
bool cfgBike = false;
int  cfgBikeTpl = 0;
#define BIKE_TPL_N 6
char bikePlate[20] = "KA 50 HJ 5683";
char bikeMake[20]  = "Royal Enfield";
char bikeModel[20] = "Meteor 350";
char bikeOwner[20] = "Ahmed";
// Picking a layout is a mode, not a side effect of a press: hold to
// get in, press to walk the six, hold to keep the one you are looking
// at. Until you keep it, nothing is written and nothing is kept.
bool bikeEdit = false;
int  bikeTry  = 0;

// Which screens have anything inside them.
//
// loop() clears depth on every pass for any screen that does not, so a
// screen missing from here cannot be entered at all: the press sets
// depth, the animation plays, and two milliseconds later the next pass
// through the loop puts it back to zero. That is exactly what happened
// to the reminders. They were given a depth and this list was not told,
// so holding to read them opened them and closed them faster than the
// screen could draw, and all you ever saw was the zoom and the summary
// again.
//
// It was a bare condition in the middle of loop() before, five screens
// written out by name. Nothing pointed at it from the screens
// themselves and nothing complained when a sixth was added. Here, at
// least, it is next to the enum it is about, and sim_depth.py now
// fails the build if a screen sets a depth without being in it.
static bool screenHasDepth(int s) {
  return s == S_FAITH || s == S_READS || s == S_GAMES ||
         s == S_FOCUS || s == S_SETTINGS || s == S_REMIND;
}

// Which screens are worth offering at all. Weather needs the network
// and the vehicle screen is something you asked for; neither should sit
// in the carousel as a dead end.
static bool screenOn_(int s) {
  if (s == S_BIKE)    return cfgBike;
  if (s == S_WEATHER) return !offlineNow();
  return true;
}
static int nextScreen(int from) {
  for (int i = 1; i <= S_COUNT; i++) {
    int s = (from + i) % S_COUNT;
    if (screenOn_(s)) return s;
  }
  return S_HOME;
}

// ---------------- the Hijri date ----------------
//  The arithmetic Islamic calendar, which is a rule rather than an
//  observation. It agrees with the announced date most of the time and
//  can be a day out either way, because the real one depends on
//  somebody seeing the moon. Checked against four known first-of-the-
//  months and it landed on all four; there is an offset in settings for
//  the days it does not.
int cfgHijriAdj = 0;              // -2 to +2 days

static long gregToJdn(int y, int m, int d) {
  long a = (14 - m) / 12, yy = y + 4800 - a, mm = m + 12 * a - 3;
  return d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
}
static void hijriFromJdn(long jd, int& hy, int& hm, int& hd) {
  long l = jd - 1948440 + 10632;
  long n = (l - 1) / 10631;
  l = l - 10631 * n + 354;
  long j = ((10985 - l) / 5316) * ((50 * l) / 17719)
         + (l / 5670) * ((43 * l) / 15238);
  l = l - ((30 - j) / 15) * ((17719 * j) / 50)
        - (j / 16) * ((15238 * j) / 43) + 29;
  hm = (int)((24 * l) / 709);
  hd = (int)(l - (709 * hm) / 24);
  hy = (int)(30 * n + j - 30);
}
// Latin, for the faces that are otherwise in English. The Arabic ones
// are bitmaps and live in arabic_glyphs.h.
const char* HIJRI_LATIN[12] = {
  "Muharram", "Safar", "Rabi I", "Rabi II", "Jumada I", "Jumada II",
  "Rajab", "Shaban", "Ramadan", "Shawwal", "Dhul Qadah", "Dhul Hijjah" };

// ---------------- reminders ----------------
//  Kept here rather than on the Mac, which is where they used to live.
//  The Mac knew the time and the robot did not, so the Mac held the
//  list and said the word when one came due. That stops working the
//  moment the robot is asleep with the lid shut somewhere else: it
//  cannot be told about nine o'clock by a laptop that is not there.
//
//  So the robot keeps them, and works out for itself when to wake. The
//  clock keeps running through deep sleep, so a reminder set tonight
//  still lands at nine tomorrow with no network and no Mac.
// Twenty. Twelve was a number picked when a reminder was a short
// label; the app keeps twenty and the reading view walks them one at
// a time, so the robot may as well hold the same list the app shows.
#define REM_MAX 20
// Sixty three characters was not a reminder, it was a label. At
// ninety five a sentence fits, and the reader has a crawl for the
// ones that still do not.
#define REM_TEXT 96
//  at    when it is next due, which moves every time it is put off
//  first the day it was for, so a thing from Tuesday stops asking on
//        Wednesday rather than following you around for ever
//  done  you held the pad on it, and it is finished
// id is what the Mac holds on to. Matching on the words and the
// minute worked for spotting a duplicate but not for editing one: the
// moment the app changed a time, the thing it was changing stopped
// matching what it had sent. The id is assigned here, never reused,
// and survives a save.
struct Rem { char text[REM_TEXT]; uint32_t id; uint32_t at; uint32_t first;
             uint8_t tries; bool done; };

// Ignoring it gets you asked again, and the gaps grow. Three quick
// ones in case you were simply looking the other way, then it backs
// off, and after the last of them it stops rather than following you
// round the house all evening.
const uint16_t REM_LADDER[] = { 5, 5, 5, 30, 30, 60, 60 };
#define REM_STEPS (sizeof(REM_LADDER) / sizeof(REM_LADDER[0]))
// A press is different. It means "I have seen it, not now", so it comes
// back sooner and does not count against the ladder: you can keep
// saying not now for as long as you like.
#define REM_WAVED_MIN 15
Rem  rems[REM_MAX];
int  remCount = 0;
int  remIdx = 0;                   // which one is being read
uint32_t remCheck = 0;
int  remShowing = -1;              // the one on screen, or none
uint32_t remNextId = 1;            // never goes backwards, never reused
bool remWokeIt = false;            // it was asleep, and this is why it is not
bool remConfirm = false;           // on the clear-them-all question
bool remYes = false;

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
enum { C_BRIGHT = 0, C_FACE, C_SLEEP, C_TURN, C_POPUP, C_EYES,
       C_PRAYER, C_HIJRI, C_MODE, C_BIKE, C_HOTSPOT, C_ACCEL, C_KNOCK, C_TAP,
       C_SHAKE, C_DEEP, C_BATT,
       C_PAIR, C_UPDATE,
       C_AUTOUP, C_RESET, C_REBOOT, C_ABOUT, C_COUNT };
const char* C_NAME[C_COUNT] =
  { "Brightness", "Watch face", "Sleep after", "Page turn", "Popup time",
    "Eye style", "Prayer times", "Hijri shift", "Network", "Vehicle",
    "Hotspot", "Accelerometer", "Knocks", "Tap strength",
    "Go back by", "Power down", "Battery full",
    "Pair a Mac", "Check update", "Auto update",
    "Reset settings", "Reboot", "About" };

// Zero is the dimmest the panel goes, not off: the SSD1306 still shows
// faintly at contrast zero. After that, quarters.
// Contrast, not backlight: an OLED lights its own pixels, so 0 is the
// faintest it will go rather than off. That is the 1% step, and there
// is nothing below it to offer. 10% sits between it and a quarter,
// because the gap from barely-there to a quarter was the big one.
// The 1% step is gone: 0 contrast is so faint it reads as a fault
// rather than a setting. 10% is the bottom now.
const int BRIGHT_OPTS[] = { 26, 64, 128, 191, 255 };
const char* BRIGHT_NAME[] = { "10%", "25%", "50%", "75%", "100%" };
const int BRIGHT_N = sizeof(BRIGHT_OPTS) / sizeof(BRIGHT_OPTS[0]);

// ---------------- how hard a knock has to be ----------------
//  THRESH_TAP on the ADXL345 counts in 62.5mg steps, so these are the
//  force a knock has to reach before the chip calls it one. Lower means
//  a lighter touch is enough. Medium is what every version so far has
//  used, so it stays the default and nothing changes unless you say so.
enum { TAP_ULTRA = 0, TAP_LIGHT, TAP_MED, TAP_HARD, TAP_N };
const char*   TAP_NAME[TAP_N]   = { "ultra light", "light", "medium", "hard" };
const char* TAP_SHORT[TAP_N] = { "ultra", "light", "medium", "hard" };
const uint8_t TAP_THRESH[TAP_N] = { 0x14, 0x1C, 0x28, 0x3C };   // 1.25g .. 3.75g
int  cfgTap  = TAP_MED;
int  tapPick = TAP_MED;              // what is highlighted while choosing
bool tapTesting = false;
bool tapChosen = false;              // past the list, actually knocking at one
// Trying a strength is a window with an end, not a screen you have to
// knock your way out of. Knocking IS the test in there, so no knock can
// be a command as well; the window closing is the only way back.
#define TAP_TRY_MS 10000UL
uint32_t tapTestEnds = 0;
// A little rolling trace of how hard the last few shoves were, so you
// can see the one that did not count as well as the ones that did.
#define TAP_TRACE 24
uint8_t tapTrace[TAP_TRACE];
int  tapTraceAt = 0;
unsigned long tapTraceNext = 0;
uint32_t tapSeen = 0;                // knocks counted while testing
unsigned long tapLastSeen = 0;

// ---------------- watch faces ----------------
//  Six laid out by hand, two that lean with the device, and two with
//  something that pours. Only the clock screen is affected.
enum { F_CLASSIC = 0, F_STACK, F_DATEUP, F_MINIMAL, F_SIDE, F_BANNER,
       F_DRIFT, F_PARALLAX, F_WATER, F_SAND,
       // Ten more, borrowed from watches that cost rather more. Three
       // are hands, one is both, five are the numbers a desk robot
       // actually knows about itself, and one is for showing off.
       F_DIAL, F_BAUHAUS, F_REGULATOR, F_RINGS, F_INFOGRAPH,
       F_STATUS, F_VITALS, F_BARS, F_TERMINAL, F_BINARY,
       // and three that know what month it is in the other calendar
       F_ARABIC, F_HIJRI, F_CRESCENT, FACE_N };
const char* FACE_NAME[FACE_N] =
  { "classic", "stacked", "date up", "minimal", "side", "banner",
    "drift", "parallax", "water", "sand",
    "dial", "bauhaus", "regulator", "rings", "infograph",
    "status", "vitals", "bars", "terminal", "binary",
    "arabic", "hijri", "crescent" };
int cfgFace = F_CLASSIC;

// Changing the face from the clock itself rather than walking into
// settings for it. A double knock on the clock is the next face, kept
// straight away, and that is the whole of it.
//
// It was a mode before this: a double knock went in, single knocks
// walked the faces, another double kept one and looking away for eight
// seconds put the old one back. That made a single knock mean two
// different things depending on a state you could not see from across
// the desk, and it threw away a face you had chosen if you got
// distracted. Nothing to enter and nothing to confirm is better.

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

const int SLEEP_OPTS[] = { 5, 10, 15, 30, 45, 60, 120, 180, 300, 600, 0 };  // 0 = never
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

// Knocking is the only way to drive this. Leaning it about was offered as
// a second way for four versions and never worked properly: a lean does
// not change how hard gravity pulls, so the check that kept the thing
// awake could not see one, and it dozed off mid gesture. Four attempts at
// it were each a real fix for a real bug and none of them cured the
// symptom. The sensor still reads leans for the games and for the watch
// faces that react to being tilted; nothing navigates by them.
enum { NC_OFF = 0, NC_HOLD, NC_UP, NC_DOWN, NC_LEFT, NC_RIGHT, NC_INFO };
// How far counts as a lean. Only the games use this now, to learn which
// way round the thing is sitting before one starts.
#define NAV_TILT_ON 0.30f
int  navCal = NC_OFF;
bool navCalTeach = false;
unsigned long navCalStamp = 0;
bool navLatch = false;
unsigned long upSince = 0;
bool upConsumed = false;
String cfgSsid, cfgPass, cfgTz, cfgKey;

// ---------------- the networks it knows ----------------
//  Home, a phone hotspot, wherever else. It works down the list until
//  one answers and stays there, and starts down it again if that one
//  goes away.
//
//  Fixed buffers rather than Strings, because the network task reads
//  these while the panel and the app can be writing them. A String moves
//  in memory when it is reassigned and the reader follows the stale
//  pointer, which is the same trap wCity was in. 32 and 63 characters
//  are the most a name and a passphrase can be anyway.
#define NET_MAX 5
char netSsid[NET_MAX][33];
char netPass[NET_MAX][65];
int  netCount  = 0;
int  netUsing  = -1;                 // which one answered
int  netTrying = 0;                  // where the walk has got to
unsigned long netNextTry = 0;
volatile bool netReload = false;     // the app changed the list
int sleepSecs() { return SLEEP_OPTS[cfgSleepIdx]; }
int popupSecs() { return POPUP_OPTS[cfgPopupIdx]; }
#define AUTO_TURN_MS 9000

// ---------------- content ----------------
String message = "";
unsigned long popupUntil = 0;

float wTemp = NAN, wHum = NAN, wWind = NAN;
int   wCode = -1;
// A plain array rather than a String, because the network task writes
// this and the screen reads it. A String can move in memory as it is
// reassigned, and the reader would follow the old pointer. It was never
// longer than thirteen characters anyway.
char wCity[16] = "";
bool  wxOk = false;

// ---------------- the network task ----------------
// What the loop asks for and the task gets on with. Plain flags: only
// the loop ever sets them and only the task ever clears them, so there
// is nothing here for two tasks to disagree about.
TaskHandle_t netTask = nullptr;
volatile bool wantTime = false, wantWx = false, wantPrayerNow = false;
// Looking for an update is the slowest thing it does over the network,
// so it happens on that task too and the panel keeps answering.
volatile bool wantOtaLatest = false, wantOtaList = false;
bool cfgAutoUp = false;                  // install what it finds, without asking
unsigned long nextAutoUp = 0;
bool autoUpArmed = false;         // this check was started by the timer
#define AUTOUP_EVERY_MS (24UL * 60UL * 60UL * 1000UL)
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

// ---------------- the clock that is not set yet ----------------
//  Home used to turn into a stopwatch whenever the time was unknown,
//  which is how the stopwatch ended up running whether or not anyone
//  had asked for one. Home stays home now, and says something rather
//  than showing a clock with nothing in it.
// Said to you, not at you. The name is yours and settable, and the
// greeting changes so the same screen is not the same screen all week.
char cfgName[16] = "Ahmed";
const char* GREET[] = { "Salam", "Hello", "Assalam", "Hi", "Good to see you" };
const int GREET_N = sizeof(GREET) / sizeof(GREET[0]);
const char* IDLE_LINES[] = {
  "touch to begin",   "everything still works", "reads, faith, games",
  "no network needed", "hold to go in",         "still here" };
const int IDLE_N = sizeof(IDLE_LINES) / sizeof(IDLE_LINES[0]);

// ---------------- the stopwatch ----------------
//  Something you pick now, rather than something home became.
//
//  It counts, it stops where it is, and it goes again from there. It
//  used to do neither: the only thing a press did was throw the time
//  away and start over, which is not a stopwatch, it is a reset
//  button that happens to show a number.
//
//    one      start, and stop where it is
//    hold     back to zero
//    two      back to the list
//    shake    back to the clock
bool     swOn  = false;          // the stopwatch has the screen
bool     swRun = false;          // and it is counting
uint32_t swAcc = 0;              // milliseconds banked by earlier runs

// ---------------- work session ----------------
#define TASK_MAX 12
struct Task { String name; int mins; bool done; };
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
unsigned long toastUntil = 0, toastFlash = 0;

// ---------------- on a break ----------------
//  The Mac locks itself and the robot holds the sign, so anyone walking
//  past knows without having to ask.
unsigned long dndUntil = 0;
const char* DND_LINES[] = {
  "Back shortly", "Stretching", "Away from the desk", "Tea", "Walking" };
const int DND_N = sizeof(DND_LINES) / sizeof(DND_LINES[0]);
int dndLine = 0;

// ---------------- camera and microphone ----------------
//  Nothing is listened to or looked at here. The Mac reads one flag per
//  device from the system and forwards it, and the robot holds the panel
//  lit while either is live so you can see it across the desk.
bool busyCam = false, busyMic = false;
unsigned long busyAt = 0;

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

// ---------------- sleeping properly ----------------
//  A dark screen is not sleep: the processor is still running flat out
//  with the radio up. After a while with nobody about and no Mac
//  listening, it can switch off altogether and wait to be picked up.
bool intWired = false;               // is INT1 actually connected?
bool deepOff = false;                // switched off in settings
unsigned long sleptAt = 0;           // when the screen went dark
#define DEEP_AFTER_MS (7UL * 60000UL)

// ---------------- runtime ----------------
bool asleep = false, screenOn = true, timeOk = false, rescueAP = false, fsOk = false;
// The web server is not started at boot when the radio is meant to be
// down, so whether it has been started is a separate question from
// whether the board has booted.
bool webUp = false;
static void setupWeb();
unsigned long lastActive = 0, lastDraw = 0, lastPoll = 0, reactUntil = 0, lastShake = 0;
unsigned long nextTimeTry = 0, swStart = 0, inputMuteUntil = 0;
unsigned long lastLowG = 0, lastFallAt = 0;
float refAx = 0, refAy = 0, refAz = 1;
unsigned long steadySince = 0;
uint32_t cTap = 0, cDouble = 0, cTriple = 0, cQuad = 0, cFall = 0, cShake = 0, cBoot = 0;
uint8_t  burst = 0;
unsigned long burstStart = 0;
String   otaStatus = "", otaStatus2 = "";

// Updating is a little conversation rather than one button: which
// release, then yes or no, and only then does anything get written.
// U_MENU is the two icons you pick from, newest or earlier, and it is
// the room every other state comes back to.
//
// The freeze was never the menu. Picking an icon ran the GitHub call
// on the display loop, where it held everything still for anything up
// to half a minute, which is what looked like dropping off to sleep.
// Picking an icon now only raises a flag; the network task does the
// talking and U_LOOK is what you watch while it does.
//
// upSeq is how a check gets called off. Knocking out of U_LOOK bumps
// it, and a reply carrying a stale number is dropped on the floor
// rather than dragging you back into a conversation you just left.
enum { U_OFF = 0, U_MENU, U_LOOK, U_ASK, U_LIST, U_NONE, U_FAIL };
int    upState = U_OFF;
volatile uint8_t upSeq = 0;
int    upPick = 0;                  // 0 the latest one, 1 the older ones
bool   upYes = true;
String upTag = "", upUrl = "", upMsg = "";
#define UP_MAX 8
String relTag[UP_MAX], relUrl[UP_MAX];
int    relCount = 0, relSel = 0;
// A pointer to a literal rather than a String: the network task sets
// this and the page reads it, and swapping a pointer is one instruction
// that cannot be caught half done. The literals never move.
const char* clockSrc = "not set";
String   wokeBy = "boot";
float    lastDirD = 0;
uint32_t nSlept = 0;
int      otaPct = -1;

static bool online() { return WiFi.status() == WL_CONNECTED; }

#define TAP_WINDOW_MS 450          // room to land four knocks, without dawdling
#define TILT 0.35f
#define SHAKE_G 0.60f
// How long after a press a shake is assumed to be the press.
#define SHAKE_AFTER_MS 800UL
// Measured from the later of the press and the lift. The jolt that
// matters is the one from letting go, which lands after a long press
// has already been counted, and the old guard had always expired by
// then: you held to go into the reminders, it went in, you lifted, the
// lift shook it, and the shake took you straight back out. That is why
// holding looked like nothing but an animation.

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
// Taps and the drop alarm, both always on. This used to disarm the drop
// alarm while leaning was switched on, because a hand turning the thing
// over unloads it constantly and tripped it. With leaning gone it sits on
// a desk, where a genuine unloading means it is falling.
// Writing the threshold is all it takes; the chip does the rest.
// Is the interrupt line actually there?
//
// Asked rather than assumed, so a unit built without the wire behaves
// sensibly instead of going to sleep and never waking up. Data ready
// fires continuously at the output rate, so routing it to INT1 and
// looking at the pin is a reliable question: connected, it sits high;
// unconnected, the pull down holds it low.
// A single lithium cell does not fall evenly, so a straight line from
// 3.0 to 4.2 would read 50% for most of an afternoon and then drop off
// a cliff. This is the usual discharge shape, in steps.
static int battPct(float v) {
  // The curve below is drawn for a cell that tops out at 4.20, so a
  // pack that stops lower is scaled up to meet it rather than being
  // told it is never quite full.
  if (battFull > 3.5f) v *= (4.20f / battFull);
  static const float V[] = { 3.00f, 3.45f, 3.68f, 3.74f, 3.77f, 3.79f,
                             3.82f, 3.87f, 3.93f, 4.00f, 4.10f, 4.20f };
  static const int   P[] = {     0,     5,    10,    20,    30,    40,
                                50,    60,    70,    80,    90,   100 };
  if (v <= V[0]) return 0;
  for (int i = 1; i < 12; i++) {
    if (v <= V[i]) {
      float f = (v - V[i-1]) / (V[i] - V[i-1]);
      return P[i-1] + (int)(f * (P[i] - P[i-1]) + 0.5f);
    }
  }
  return 100;
}

// Read it rarely: the pack does not move quickly and every sample
// costs a little of it through the divider.
static void readBattery() {
  uint32_t now = millis();
  if (battNext && (int32_t)(now - battNext) < 0) return;
  battNext = now + 20000;
  analogReadMilliVolts(BATT_PIN);              // thrown away: see above
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(BATT_PIN);
  float v = (sum / 8.0f) * BATT_MUL / 1000.0f;
  // Nothing plugged in reads as noise near zero rather than as a cell.
  battV = (v > 2.5f) ? v : NAN;
}

static bool probeIntPin() {
  if (!adxl) return false;
  pinMode(TAP_INT_PIN, INPUT_PULLDOWN);
  uint8_t keepEn = rReg(adxl, A_INT_ENABLE);
  uint8_t keepMap = rReg(adxl, A_INT_MAP);
  wReg(adxl, A_INT_MAP, 0x00);            // everything out of INT1
  wReg(adxl, A_INT_ENABLE, INT_DATARDY);
  delay(30);
  int high = 0;
  for (int i = 0; i < 12; i++) { if (digitalRead(TAP_INT_PIN)) high++; delay(4); }
  wReg(adxl, A_INT_ENABLE, keepEn);
  wReg(adxl, A_INT_MAP, keepMap);
  rReg(adxl, A_INT_SOURCE);
  // a floating pin would not read high this consistently
  return high >= 9;
}

static void applyTapLevel(int lvl) {
  if (!adxl) return;
  wReg(adxl, A_THRESH_TAP, TAP_THRESH[constrain(lvl, 0, TAP_N - 1)]);
  rReg(adxl, A_INT_SOURCE);            // drop anything the change stirred up
}
static void applyTap() { applyTapLevel(cfgTap); }

static void applyFallInt() {
  if (!adxl) return;
  wReg(adxl, A_INT_ENABLE, (uint8_t)(INT_TAP1 | INT_FF));
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
// However long it has been counting, banked plus whatever this run has
// added. Reading it while stopped gives the same answer every time,
// which is the whole point of stopping it.
static uint32_t swMs() {
  return swAcc + (swRun ? (uint32_t)(millis() - swStart) : 0);
}
static void swGo()    { if (!swRun) { swRun = true; swStart = millis(); } }
static void swStop()  { if (swRun)  { swAcc = swMs(); swRun = false; } }
static void swZero()  { swAcc = 0; swStart = millis(); }
static void swStr(char* o, size_t n) {
  uint32_t ms = swMs();
  unsigned long s = ms / 1000UL;
  // Tenths under the hour, because a stopwatch that only moves once a
  // second looks stopped when it is running.
  if (s >= 3600UL) snprintf(o, n, "%lu:%02lu:%02lu", s / 3600UL, (s / 60UL) % 60UL, s % 60UL);
  else             snprintf(o, n, "%lu:%02lu.%lu", s / 60UL, s % 60UL, (ms / 100UL) % 10UL);
}

// ================================================================
//  WATCH FACES
//    Twenty ways to show the same few things. No frames and no boxes:
//    the panel is small enough that a border is only lost pixels.
//
//    The second half borrows from wristwatches: three with hands, four
//    that carry information the robot already knows and was only ever
//    showing buried in SYSTEM, and three that are just nice to look at.
//    Anything it does not know yet shows dashes rather than a gap.
// ================================================================
struct Bits { char hm[8], hh[4], mm[4], ss[4], day[12], dlong[20], dshort[14],
                   dmon[10], dyear[6]; bool ok;
              // The hands need an angle, not a string, and Rings needs
              // to know how far through the day it is.
              //
              // Not S for seconds: RoboEyes has "#define S 5" for the
              // south gaze direction, so a member called S expands to a
              // number and takes the rest of the line down with it. The
              // same goes for N, E, W and the four corners.
              int H, M, sec, mday, yday; };

static Bits fb;                         // what the faces draw from
static void loadBits() {
  Bits& b = fb;
  struct tm t;
  fb.ok = timeOk && getLocalTime(&t, 0);
  if (!fb.ok) {
    strcpy(fb.hm, "--:--"); strcpy(fb.hh, "--"); strcpy(fb.mm, "--"); strcpy(fb.ss, "--");
    strcpy(fb.day, "waiting"); strcpy(fb.dlong, "for the clock"); strcpy(fb.dshort, "--");
    fb.H = fb.M = fb.sec = 0; fb.mday = 1; fb.yday = 0;
    return;
  }
  snprintf(fb.hm, sizeof(fb.hm), "%02d:%02d", t.tm_hour, t.tm_min);
  snprintf(fb.hh, sizeof(fb.hh), "%02d", t.tm_hour);
  snprintf(fb.mm, sizeof(fb.mm), "%02d", t.tm_min);
  snprintf(fb.ss, sizeof(fb.ss), "%02d", t.tm_sec);
  strftime(fb.day,    sizeof(fb.day),    "%A", &t);
  strftime(fb.dlong,  sizeof(fb.dlong),  "%d %B %Y", &t);
  strftime(fb.dshort, sizeof(fb.dshort), "%d %b %Y", &t);
  fb.H = t.tm_hour; fb.M = t.tm_min; fb.sec = t.tm_sec;
  fb.mday = t.tm_mday; fb.yday = t.tm_yday;
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

// A little mast with its signal struck through: enough to read as
// "no network" without a word for it.
static void offlineIcon(int x, int y) {
  oled.drawFastVLine(x + 3, y + 1, 6, SSD1306_WHITE);
  oled.drawFastHLine(x + 1, y + 7, 5, SSD1306_WHITE);
  oled.drawPixel(x + 1, y + 2, SSD1306_WHITE);
  oled.drawPixel(x + 5, y + 2, SSD1306_WHITE);
  for (int i = 0; i < 8; i++) oled.drawPixel(x - 1 + i, y + i, SSD1306_WHITE);
}


// ================================================================
//  TEN MORE FACES
// ================================================================
//  Hands, rings and readouts. Everything here is drawn from fb, which
//  is only ever filled when the clock is known, and from numbers the
//  robot already keeps about itself: how loud the network is, how many
//  times it has started up, how long it has been awake.
//
//  The panel redraws about nine times a second, so a second hand ticks
//  once per second and lands cleanly. There is no sweep and there was
//  never going to be one.
// ================================================================

// How strong the signal is, nought to four. Anything still connected
// gets at least one bar: nought means no network at all, which is a
// different thing from a weak one and should not look the same.
static int sigBars() {
  if (!online()) return 0;
  int r = (int)WiFi.RSSI();
  if (r >= -55) return 4;
  if (r >= -65) return 3;
  if (r >= -75) return 2;
  return 1;
}
static void drawSig(int x, int y, int bars) {
  for (int i = 0; i < 4; i++) {
    int h = 3 + i * 2, bx = x + i * 4, by = y + 9 - h;
    if (i < bars) oled.fillRect(bx, by, 3, h, SSD1306_WHITE);
    else          oled.drawRect(bx, by, 3, h, SSD1306_WHITE);
  }
}

// A number that cannot grow wider than four characters. The knock
// count passes ten thousand on a busy desk and a boot count climbs
// forever; either one printed in full runs off the right of a face
// that has sixty pixels for it.
static void numStr(char* out, size_t n, uint32_t v) {
  if (v < 1000)        snprintf(out, n, "%lu", (unsigned long)v);
  else if (v < 100000) snprintf(out, n, "%luk", (unsigned long)(v / 1000));
  else                 snprintf(out, n, "%luM", (unsigned long)(v / 1000000));
}

// Awake for how long, in the largest unit that still says something.
static void upStr(char* out, size_t n) {
  uint32_t s = millis() / 1000UL;
  if (s < 3600)  snprintf(out, n, "%lum", (unsigned long)(s / 60));
  else if (s < 86400) snprintf(out, n, "%luh%02lum",
                               (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60));
  else snprintf(out, n, "%lud%luh", (unsigned long)(s / 86400),
                (unsigned long)((s % 86400) / 3600));
}

// The next prayer, or empty if the times have not arrived. Wraps to
// tomorrow's first one after the last has passed, so it is never blank
// for the rest of the evening.
static bool nextPrayer(char* name, size_t nn, char* when, size_t wn) {
  int nowMin = fb.H * 60 + fb.M, best = -1, bi = -1;
  for (int i = 0; i < 5; i++) {
    int mins = prayerAt(i);
    if (mins < 0) continue;
    if (mins >= nowMin && (best < 0 || mins < best)) { best = mins; bi = i; }
  }
  if (bi < 0) for (int i = 0; i < 5; i++) {     // all gone; tomorrow's first
    int mins = prayerAt(i);
    if (mins >= 0) { best = mins; bi = i; break; }
  }
  if (bi < 0) return false;
  snprintf(name, nn, "%s", PRAYERS[bi]);
  snprintf(when, wn, "%02d:%02d", best / 60, best % 60);
  return true;
}

// A hand, from the middle out. Angles run clockwise from twelve, which
// is not how sin and cos run, hence the quarter turn.
static void hand(int cx, int cy, float turns, int len, bool thick) {
  float a = turns * 6.28318f - 1.5708f;
  int x = cx + (int)(cosf(a) * len), y = cy + (int)(sinf(a) * len);
  oled.drawLine(cx, cy, x, y, SSD1306_WHITE);
  if (thick) {                      // a second line beside it, so it reads as heavier
    oled.drawLine(cx, cy + 1, x, y + 1, SSD1306_WHITE);
    oled.drawLine(cx + 1, cy, x + 1, y, SSD1306_WHITE);
  }
}
// Part of a circle, from twelve, clockwise. Stepped finely enough that
// it does not come out as a dotted line.
static void arc(int cx, int cy, int r, float part) {
  int steps = (int)(part * 220);
  for (int i = 0; i <= steps; i++) {
    float a = (i / 220.0f) * 6.28318f - 1.5708f;
    oled.drawPixel(cx + (int)(cosf(a) * r), cy + (int)(sinf(a) * r), SSD1306_WHITE);
  }
}

static bool hijriNow(int& hy, int& hm, int& hd) {
  struct tm t;
  if (!timeOk || !getLocalTime(&t, 0)) return false;
  long jd = gregToJdn(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday) + cfgHijriAdj;
  hijriFromJdn(jd, hy, hm, hd);
  if (hm < 1) hm = 1; if (hm > 12) hm = 12;
  return true;
}

// ---- Arabic, out of the baked bitmaps ----
static void arDigit(int x, int y, int d, bool big) {
  if (d < 0 || d > 9) return;
  if (big) oled.drawBitmap(x, y, AR_BIG[d],   AR_BIG_W,   AR_BIG_H,   SSD1306_WHITE);
  else     oled.drawBitmap(x, y, AR_SMALL[d], AR_SMALL_W, AR_SMALL_H, SSD1306_WHITE);
}
// Western order on purpose. A clock is read left to right whichever
// digits it is wearing, and an Arabic reader reads ١٢:٣٠ the same way
// round as 12:30.
static void arNum(int x, int y, int v, int digits, bool big) {
  int w = big ? AR_BIG_W : AR_SMALL_W;
  for (int i = digits - 1; i >= 0; i--) { arDigit(x + i * w, y, v % 10, big); v /= 10; }
}
static int arNumW(int digits, bool big) { return digits * (big ? AR_BIG_W : AR_SMALL_W); }
static void arMonth(int x, int y, int m) {
  if (m < 1 || m > 12) return;
  oled.drawBitmap(x, y, AR_MONTH[m - 1], AR_MONTH_W, AR_MONTH_H, SSD1306_WHITE);
}

// A band that shows one line, then the other, sliding the old one up
// and out as the new one comes up behind it. Two seconds each.
//
// There is no clipping in the library, so the lines going past the top
// and bottom of the band have to be wiped afterwards. Anything using
// this has to keep the rows a band's height above and below it empty,
// which the three faces below do.
static void slideBand(int y, const char* a, const char* b) {
  const int H = 9, PERIOD = 2000, SLIDE = 320;
  uint32_t t = millis() % (PERIOD * 2);
  bool second = t >= (uint32_t)PERIOD;
  uint32_t into = second ? t - PERIOD : t;
  const char* now_ = second ? b : a;
  const char* was  = second ? a : b;
  int off = into < (uint32_t)SLIDE ? (int)(into * H / SLIDE) : H;
  if (off < H) {
    ctr(was,  y - off, 1);
    ctr(now_, y + H - off, 1);
  } else {
    ctr(now_, y, 1);
  }
  oled.fillRect(0, y - H, SCRW, H, SSD1306_BLACK);        // what slid off the top
  oled.fillRect(0, y + 8, SCRW, H, SSD1306_BLACK);        // and off the bottom
}

// ---- 1. dial: a whole watch, ticks and three hands ----
static void faceDial() {
  const int CX = 64, CY = 32, R = 30;
  oled.drawCircle(CX, CY, R, SSD1306_WHITE);
  for (int i = 0; i < 12; i++) {
    float a = i * 0.5236f - 1.5708f;
    int len = (i % 3 == 0) ? 6 : 3;
    oled.drawLine(CX + (int)(cosf(a) * (R - 2)), CY + (int)(sinf(a) * (R - 2)),
                  CX + (int)(cosf(a) * (R - 2 - len)), CY + (int)(sinf(a) * (R - 2 - len)),
                  SSD1306_WHITE);
  }
  hand(CX, CY, ((fb.H % 12) + fb.M / 60.0f) / 12.0f, 14, true);
  hand(CX, CY, (fb.M + fb.sec / 60.0f) / 60.0f, 21, true);
  hand(CX, CY, fb.sec / 60.0f, 25, false);
  oled.fillCircle(CX, CY, 2, SSD1306_WHITE);
}

// ---- 2. bauhaus: no case, four marks, two hands ----
static void faceBauhaus() {
  const int CX = 64, CY = 32;
  for (int i = 0; i < 12; i++) {
    float a = i * 0.5236f - 1.5708f;
    if (i % 3 == 0) {
      oled.drawLine(CX + (int)(cosf(a) * 30), CY + (int)(sinf(a) * 30),
                    CX + (int)(cosf(a) * 23), CY + (int)(sinf(a) * 23), SSD1306_WHITE);
      oled.drawLine(CX + (int)(cosf(a) * 30) + 1, CY + (int)(sinf(a) * 30),
                    CX + (int)(cosf(a) * 23) + 1, CY + (int)(sinf(a) * 23), SSD1306_WHITE);
    } else {
      oled.drawPixel(CX + (int)(cosf(a) * 29), CY + (int)(sinf(a) * 29), SSD1306_WHITE);
    }
  }
  hand(CX, CY, ((fb.H % 12) + fb.M / 60.0f) / 12.0f, 15, true);
  hand(CX, CY, fb.M / 60.0f, 24, false);
  oled.fillCircle(CX, CY, 2, SSD1306_WHITE);
}

// ---- 3. regulator: hours and minutes apart from the seconds ----
static void faceRegulator() {
  const int CX = 40, CY = 33, R = 27;
  oled.drawCircle(CX, CY, R, SSD1306_WHITE);
  for (int i = 0; i < 12; i += 3) {
    float a = i * 0.5236f - 1.5708f;
    oled.drawLine(CX + (int)(cosf(a) * (R - 2)), CY + (int)(sinf(a) * (R - 2)),
                  CX + (int)(cosf(a) * (R - 7)), CY + (int)(sinf(a) * (R - 7)), SSD1306_WHITE);
  }
  hand(CX, CY, ((fb.H % 12) + fb.M / 60.0f) / 12.0f, 13, true);
  hand(CX, CY, fb.M / 60.0f, 20, false);
  oled.fillCircle(CX, CY, 2, SSD1306_WHITE);

  const int SX = 99, SY = 22, SR = 15;         // the seconds, on their own
  oled.drawCircle(SX, SY, SR, SSD1306_WHITE);
  for (int i = 0; i < 4; i++) {
    float a = i * 1.5708f - 1.5708f;
    oled.drawPixel(SX + (int)(cosf(a) * (SR - 3)), SY + (int)(sinf(a) * (SR - 3)), SSD1306_WHITE);
  }
  hand(SX, SY, fb.sec / 60.0f, 11, false);
  oled.fillCircle(SX, SY, 1, SSD1306_WHITE);

  char d[8];
  snprintf(d, sizeof(d), "%02d", fb.mday);
  oled.drawRect(88, 46, 23, 13, SSD1306_WHITE);
  at(95, 49, d);
}

// ---- 4. rings: an hour, a minute and a second, closing ----
static void faceRings() {
  const int CX = 64, CY = 33;
  // Faint tracks first so an almost empty ring still reads as a ring,
  // then the filled part over the top. Every fourth pixel: enough to
  // see where the ring goes, not enough to be mistaken for progress.
  for (int r = 20; r <= 30; r += 5)
    for (int i = 0; i < 220; i += 4) {
      float a = (i / 220.0f) * 6.28318f - 1.5708f;
      oled.drawPixel(CX + (int)(cosf(a) * r), CY + (int)(sinf(a) * r), SSD1306_WHITE);
    }
  arc(CX, CY, 30, ((fb.H % 12) + fb.M / 60.0f) / 12.0f);
  arc(CX, CY, 25, (fb.M + fb.sec / 60.0f) / 60.0f);
  arc(CX, CY, 20, fb.sec / 60.0f);
  at(CX - 15, CY - 9, fb.hm);
  at(CX - 6, CY + 2, fb.ss);
}

// ---- 5. infograph: the time small, and one fact in each corner ----
static void faceInfograph() {
  drawSig(3, 2, sigBars());

  char t[10];
  if (wxOk) snprintf(t, sizeof(t), "%dC", (int)lroundf(wTemp));
  else      snprintf(t, sizeof(t), "--");
  at(SCRW - 2 - (int)strlen(t) * 6, 3, t);

  oled.setTextSize(2);
  oled.setCursor(64 - 5 * 6, 24);
  oled.print(fb.hm);
  oled.setTextSize(1);
  at(64 + 5 * 6 + 2, 31, fb.ss);

  char dd[14];
  if (isnan(battV)) snprintf(dd, sizeof(dd), "%02d %.3s", fb.mday, fb.dshort + 3);
  else              snprintf(dd, sizeof(dd), "%02d %.3s %d%%", fb.mday, fb.dshort + 3,
                             battPct(battV));
  at(3, 54, dd);

  char pn[10], pw[8];
  if (nextPrayer(pn, sizeof(pn), pw, sizeof(pw))) {
    char p[20];
    snprintf(p, sizeof(p), "%.4s %s", pn, pw);
    at(SCRW - 2 - (int)strlen(p) * 6, 54, p);
  }
  oled.drawFastHLine(0, 14, SCRW, SSD1306_WHITE);
  oled.drawFastHLine(0, 50, SCRW, SSD1306_WHITE);
}

// ---- 6. status: the time, and how the thing is doing under it ----
static void faceStatus() {
  oled.setTextSize(3);
  oled.setCursor(64 - (5 * 18) / 2 - 7, 6);
  oled.print(fb.hm);
  oled.setTextSize(1);
  at(64 + (5 * 18) / 2 - 5, 22, fb.ss);

  oled.drawFastHLine(0, 36, SCRW, SSD1306_WHITE);
  drawSig(3, 41, sigBars());

  char r[12];
  if (online()) snprintf(r, sizeof(r), "%ddBm", (int)WiFi.RSSI());
  else          snprintf(r, sizeof(r), "no wifi");
  at(23, 42, r);

  // Two pieces pinned to their own ends rather than one centred line:
  // a long uptime and a long boot count together came to more than the
  // screen is wide.
  char u[12]; upStr(u, sizeof(u));
  char l[16], r2[16];
  snprintf(l,  sizeof(l),  "up %s", u);
  if (isnan(battV)) snprintf(r2, sizeof(r2), "no pack");
  else              snprintf(r2, sizeof(r2), "batt %d%%", battPct(battV));
  at(3, 53, l);
  at(SCRW - 2 - (int)strlen(r2) * 6, 53, r2);
}

// ---- 7. vitals: everything it knows about itself ----
static void faceVitals() {
  at(3, 2, fb.hm);
  at(SCRW - 2 - (int)strlen(fb.dshort) * 6, 2, fb.dshort);
  oled.drawFastHLine(0, 12, SCRW, SSD1306_WHITE);

  // The right hand column starts at 68 and the screen ends at 128, so
  // it has ten characters and not one more. Every number here is one
  // that keeps growing, so every one of them is shortened.
  char u[12]; upStr(u, sizeof(u));
  char nb[8], ns[8], nt[8];
  numStr(nb, sizeof(nb), cBoot);
  numStr(ns, sizeof(ns), nSlept);
  numStr(nt, sizeof(nt), cTap);
  char l[6][14];
  snprintf(l[0], 14, "boots %s", nb);
  snprintf(l[1], 14, "up %s",    u);
  snprintf(l[2], 14, "slept %s", ns);
  snprintf(l[3], 14, "taps %s",  nt);
  if (isnan(battV)) snprintf(l[4], 14, "batt none");
  else              snprintf(l[4], 14, "batt %d%%", battPct(battV));
  if (online()) snprintf(l[5], 14, "rssi %d", (int)WiFi.RSSI());
  else          snprintf(l[5], 14, "offline");
  for (int i = 0; i < 6; i++) at(i % 2 ? 68 : 4, 17 + (i / 2) * 15, l[i]);
}

// ---- 8. bars: read it like a level meter ----
static void faceBars() {
  const char* L[4] = { "H", "M", "S", "@" };
  float f[4] = { fb.H / 24.0f, fb.M / 60.0f, fb.sec / 60.0f, sigBars() / 4.0f };
  char  v[4][6];
  snprintf(v[0], 6, "%02d", fb.H);
  snprintf(v[1], 6, "%02d", fb.M);
  snprintf(v[2], 6, "%02d", fb.sec);
  snprintf(v[3], 6, "%d", sigBars());
  for (int i = 0; i < 4; i++) {
    int y = 4 + i * 15;
    at(1, y + 1, L[i]);
    oled.drawRect(10, y, 100, 10, SSD1306_WHITE);
    int w = (int)(f[i] * 96);
    if (w > 0) oled.fillRect(12, y + 2, w, 6, SSD1306_WHITE);
    at(113, y + 1, v[i]);
  }
}

// ---- 9. terminal: for when you want it to look like a machine ----
static void faceTerminal() {
  char l[6][24];
  snprintf(l[0], 24, "time  %s:%s", fb.hm, fb.ss);
  snprintf(l[1], 24, "date  %s", fb.dshort);
  if (online()) {
    String s = WiFi.SSID();
    snprintf(l[2], 24, "net   %.13s", s.c_str());
    snprintf(l[3], 24, "rssi  %d dBm", (int)WiFi.RSSI());
  } else {
    snprintf(l[2], 24, "net   none");
    snprintf(l[3], 24, "rssi  --");
  }
  if (isnan(battV)) snprintf(l[4], 24, "batt  no pack");
  else              snprintf(l[4], 24, "batt  %d%% %.2fV", battPct(battV), battV);
  char u[12]; upStr(u, sizeof(u));
  snprintf(l[5], 24, "up    %s", u);
  for (int i = 0; i < 6; i++) {
    at(2, 2 + i * 10, ">");
    at(10, 2 + i * 10, l[i]);
  }
}

// ---- 21. arabic: the time in Arabic numerals, the month in Arabic ----
static void faceArabic() {
  // HH:MM across the middle. Four glyphs and a colon of two dots,
  // centred as one block so it does not shift as the digits change.
  const int DW = AR_BIG_W, GAP = 7;
  int w = DW * 4 + GAP;
  int x = (SCRW - w) / 2, y = 4;
  arNum(x, y, fb.H, 2, true);
  arNum(x + DW * 2 + GAP, y, fb.M, 2, true);
  int cx = x + DW * 2 + GAP / 2;
  oled.fillRect(cx - 1, y + 7,  2, 2, SSD1306_WHITE);
  oled.fillRect(cx - 1, y + 15, 2, 2, SSD1306_WHITE);

  int hy, hm, hd;
  if (!hijriNow(hy, hm, hd)) { ctr("--", 40, 1); return; }
  // the day on the left, the year on the right, the month between them
  arNum(3, 30, hd, 2, false);
  arNum(SCRW - 3 - arNumW(4, false), 30, hy, 4, false);
  arMonth((SCRW - AR_MONTH_W) / 2, 45, hm);
}

// ---- 22. hijri: an English clock whose date keeps changing its mind ----
static void faceHijri() {
  oled.setTextSize(3);
  oled.setCursor((SCRW - 5 * 18) / 2, 4);
  oled.print(fb.hm);
  oled.setTextSize(1);

  int hy, hm, hd;
  char g[24], h[26];
  snprintf(g, sizeof(g), "%.11s", fb.dshort);
  if (hijriNow(hy, hm, hd)) snprintf(h, sizeof(h), "%d %s %d", hd, HIJRI_LATIN[hm - 1], hy);
  else                      snprintf(h, sizeof(h), "no date yet");
  // 44 keeps the band and its wipe clear of the clock above it
  slideBand(44, g, h);
}

// ---- 23. crescent: the moon, and the month it belongs to ----
static void faceCrescent() {
  // A crescent is two circles, one eating the other.
  const int CX = 22, CY = 20, R = 13;
  oled.fillCircle(CX, CY, R, SSD1306_WHITE);
  oled.fillCircle(CX + 6, CY - 2, R, SSD1306_BLACK);

  oled.setTextSize(2);
  oled.setCursor(46, 12);
  oled.print(fb.hm);
  oled.setTextSize(1);
  at(46 + 5 * 12 + 2, 19, fb.ss);

  // The month name is 104 wide and 16 tall, which is most of the lower
  // half, so everything else is placed round it rather than near it.
  // At 40 it ran straight through the line underneath.
  int hy, hm, hd;
  if (hijriNow(hy, hm, hd)) {
    arNum(3, 39, hd, 2, false);                       // 39..49
    arMonth(SCRW - 2 - AR_MONTH_W, 37, hm);           // 37..52
    char y[12]; snprintf(y, sizeof(y), "%d", hy);
    at(3, 55, y);                                     // 55..61
    at(SCRW - 3 - (int)strlen(fb.dshort) * 6, 55, fb.dshort);
  }
}

// ---- 10. binary: hours, minutes and seconds, in dots ----
//  One column per digit, most significant at the top, and only the dots
//  that can ever light are drawn: there is no eight in the tens of an
//  hour, so there is no lamp for one.
static void faceBinary() {
  const int digit[6] = { fb.H / 10, fb.H % 10, fb.M / 10, fb.M % 10,
                         fb.sec / 10, fb.sec % 10 };
  const int bits[6]  = { 2, 4, 3, 4, 3, 4 };     // how many can ever light
  const int cx[6]    = { 16, 32, 56, 72, 96, 112 };
  at(20, 3, "H"); at(60, 3, "M"); at(100, 3, "S");
  for (int c = 0; c < 6; c++) {
    for (int b = 0; b < bits[c]; b++) {
      int weight = 1 << (bits[c] - 1 - b);
      int y = 20 + b * 12 - (4 - bits[c]) * 0;
      y = 20 + (4 - bits[c] + b) * 12;           // bottom aligned, so rows line up
      if (digit[c] & weight) oled.fillCircle(cx[c], y, 4, SSD1306_WHITE);
      else                   oled.drawCircle(cx[c], y, 4, SSD1306_WHITE);
    }
  }
}

static void drawHome() {
  oled.clearDisplay();
  loadBits();

  // Off the network, the clock is not the point any more: nothing on
  // this screen was going to be fetched. It says hello instead.
  //
  // The time in the corner is there only when there is a time. The
  // clock runs through deep sleep and comes back with the hour still
  // right, so most of the time there is one and it belongs on screen.
  // When it has been off the mains and lost it, the corner stays
  // empty: a robot that invents the time is worse than one that does
  // not know it. This comes before the no-clock screen because off
  // the network with no clock is still the offline home, not a board
  // sitting there waiting for something that is not coming.
  if (offlineNow()) {
    char hi[34];
    snprintf(hi, sizeof(hi), "%s, %s",
             GREET[(millis() / 11000UL) % GREET_N], cfgName);
    offlineIcon(4, 2);
    if (fb.ok) at(15, 2, fb.hm);
    if (!isnan(battV)) {
      char b[8]; snprintf(b, sizeof(b), "%d%%", battPct(battV));
      at(SCRW - 2 - (int)strlen(b) * 6, 2, b);
    }
    robotHead(SCRW / 2, 27, false);
    ctr(hi, 44, 1);
    ctr("touch to begin", 55, 1);
    oled.display();
    return;
  }

  // On a network but the clock has not landed yet. This used to become
  // a stopwatch, which is why one was always running whether or not
  // anyone wanted it. Home stays home, and says what it is waiting for
  // rather than showing an empty clock.
  if (!fb.ok) {
    ctr("WAITING FOR THE CLOCK", 4, 1);
    robotHead(SCRW / 2, 37, true);        // 23px of aerial clears the title
    // a different line every eight seconds, so it is never a dead panel
    ctr(IDLE_LINES[(millis() / 8000UL) % IDLE_N], 55, 1);
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
    case F_SAND:     faceFill(false); break;
    case F_DIAL:      faceDial();       break;
    case F_BAUHAUS:   faceBauhaus();    break;
    case F_REGULATOR: faceRegulator();  break;
    case F_RINGS:     faceRings();      break;
    case F_INFOGRAPH: faceInfograph();  break;
    case F_STATUS:    faceStatus();     break;
    case F_VITALS:    faceVitals();     break;
    case F_BARS:      faceBars();       break;
    case F_TERMINAL:  faceTerminal();   break;
    case F_BINARY:    faceBinary();     break;
    case F_ARABIC:    faceArabic();     break;
    case F_HIJRI:     faceHijri();      break;
    case F_CRESCENT:  faceCrescent();   break;
    default:         faceClassic();     break;
  }
  // Last, over whichever face just drew itself, or a face that fills
  // the screen would paint straight over it. A single press means
  // something different in here and nothing else would say so.
  if (faceMode) {
    oled.fillCircle(SCRW - 5, 4, 3, SSD1306_BLACK);
    oled.fillCircle(SCRW - 5, 4, 2, SSD1306_WHITE);
  }
  oled.display();
}


static void drawWeather() {
  oled.clearDisplay();
  bar("WEATHER");
  if (!wxOk) {
    ctr(online() ? "Fetching" : "No network", 28, 1);
    ctr(wCity[0] ? wCity : "Offline for now", 44, 1);
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
  snprintf(l, sizeof(l), "%s  %.0f km/h", wCity, wWind);
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
    ctr("Hold to reset", 54, 1);
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
  ctr("Hold to open", 52, 1);
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
    ctr(readCount ? "Hold to open" : "Hold to fetch some", 55, 1);
    oled.display();
    return;
  }

  if (depth == 1) {
    if (!readCount) {
      oled.clearDisplay();
      titleBar("SHORT READS", "");
      ctr(storyState.c_str(), 24, 1);
      ctr(cfgKey.length() ? "Hold to write" : "Add a key on the page", 40, 1);
      ctr(cfgKey.length() ? "a new one" : "", 50, 1);
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
// A reminder, the way a watch shows a message: the time it is for
// along the top and the whole of it underneath, wrapped, with nothing
// else competing for the room.
// A house, for the overlay that says letting go goes home.
static void houseGlyph(int cx, int cy) {
  oled.drawLine(cx - 7, cy, cx, cy - 7, SSD1306_WHITE);
  oled.drawLine(cx, cy - 7, cx + 7, cy, SSD1306_WHITE);
  oled.drawRect(cx - 5, cy, 11, 8, SSD1306_WHITE);
  oled.fillRect(cx - 1, cy + 4, 3, 4, SSD1306_WHITE);
}

// A ring drawn as a fraction of itself, clockwise from twelve.
static void ringArc(int cx, int cy, int r, float frac) {
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  const int SEG = 140;                  // not N: RoboEyes #defines that
  int steps = (int)(frac * SEG);
  for (int i = 0; i < steps; i++) {
    float a = -1.5708f + i * (6.2832f / SEG);
    float c = cosf(a), sn = sinf(a);
    for (int rr = r - 2; rr <= r; rr++)
      oled.drawPixel(cx + (int)(rr * c), cy + (int)(rr * sn), SSD1306_WHITE);
  }
}

// What a long hold looks like while it is happening.
//
//  Five to ten seconds is a ring filling towards sleep with a house in
//  the middle, because what letting go does right now is go home. Past
//  ten it stops being a hint and becomes a countdown, and the one
//  thing it needs to say is that the finger has to come off.
//
//  It used to flash the word HOME in a box, which said nothing about
//  how far through you were and read like a fault.
static void drawHoldTier(uint32_t held) {
  // Both tiers take the whole screen. A ring punched into the middle
  // of a settings list left the list showing round the edges and the
  // title band sliced in half, which looked like a glitch rather than
  // a thing the robot meant to do.
  if (held < TOUCH_SLEEP_MS) {
    float frac = (float)(held - TOUCH_HOME_MS) / (float)(TOUCH_SLEEP_MS - TOUCH_HOME_MS);
    oled.clearDisplay();
    oled.fillRect(0, 0, SCRW, 11, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    ctr("LET GO FOR HOME", 2, 1);
    oled.setTextColor(SSD1306_WHITE);

    const int CX = SCRW / 2, CY = 31, R = 16;
    oled.drawCircle(CX, CY, R, SSD1306_WHITE);
    ringArc(CX, CY, R, frac);
    houseGlyph(CX, CY - 1);

    oled.fillRect(0, 53, SCRW, 11, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    ctr("hold on for sleep", 55, 1);
    oled.setTextColor(SSD1306_WHITE);
    return;
  }

  uint32_t gone = held - TOUCH_SLEEP_MS;
  if (gone > TOUCH_COUNT_MS) gone = TOUCH_COUNT_MS;
  int left = (int)((TOUCH_COUNT_MS - gone + 999) / 1000);
  if (left < 1) left = 1;
  char n[2] = { (char)('0' + left), 0 };

  oled.clearDisplay();                       // this one takes the screen
  oled.fillRect(0, 0, SCRW, 11, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  ctr("GOING TO SLEEP", 2, 1);
  oled.setTextColor(SSD1306_WHITE);

  const int CX = SCRW / 2, CY = 31, R = 16;
  oled.drawCircle(CX, CY, R, SSD1306_WHITE);
  ringArc(CX, CY, R, 1.0f - (float)gone / (float)TOUCH_COUNT_MS);
  ctr(n, 19, 3);

  oled.fillRect(0, 53, SCRW, 11, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  ctr("lift your finger", 55, 1);
  oled.setTextColor(SSD1306_WHITE);
}

// A bell, drawn to the same weight as the gear so the carousel looks
// like one thing rather than a collection of drawings.
static void bellIcon(int cx, int cy, int r) {
  const float dr = r * 0.78f;
  for (int d = 180; d <= 360; d += 2)
    oled.drawPixel(cx + (int)(dr * cosf(d * 0.01745f)),
                   cy - 2 + (int)(dr * sinf(d * 0.01745f)), SSD1306_WHITE);
  oled.drawFastVLine(cx - (int)dr, cy - 2, r - 1, SSD1306_WHITE);
  oled.drawFastVLine(cx + (int)dr, cy - 2, r - 1, SSD1306_WHITE);
  oled.drawFastHLine(cx - r, cy + r - 3, r * 2 + 1, SSD1306_WHITE);   // the lip
  oled.fillCircle(cx, cy + r, 2, SSD1306_WHITE);                      // the clapper
  oled.drawPixel(cx, cy - 3 - (int)dr, SSD1306_WHITE);                // the loop
}

// Break text on spaces into lines of at most cols characters. Returns
// how many lines it made, which may be fewer than it wanted if the
// buffer ran out, and never splits a word that will fit on its own.
#define REM_LN   10
#define REM_COLS 32
static char remLines[REM_LN][REM_COLS];
static int wrapInto(const char* t, int cols, int maxLines) {
  if (cols > REM_COLS - 1) cols = REM_COLS - 1;
  int len = (int)strlen(t), pos = 0, n = 0;
  while (pos < len && n < maxLines) {
    int take = len - pos;
    if (take > cols) take = cols;
    if (pos + take < len) {                 // more to come: break on a space
      int sp = take;
      while (sp > 0 && t[pos + sp] != ' ') sp--;
      if (sp > 0) take = sp;
    }
    memcpy(remLines[n], t + pos, take);
    remLines[n][take] = 0;
    pos += take;
    while (pos < len && t[pos] == ' ') pos++;
    n++;
  }
  return n;
}

// When the one on screen last changed, so a crawl starts from the top
// every time you turn the page instead of continuing mid-sentence.
static uint32_t remShownAt = 0;
static int      remShownIdx = -1;

static void drawReminders() {
  oled.clearDisplay();

  if (depth == 0) {                              // the summary
    bar("REMINDERS");
    bellIcon(SCRW / 2, 30, 11);
    int p = remPending();
    char c[24];
    if (!remCount)   snprintf(c, sizeof(c), "nothing waiting");
    else if (p == 1) snprintf(c, sizeof(c), "1 reminder");
    else if (p)      snprintf(c, sizeof(c), "%d reminders", p);
    else             snprintf(c, sizeof(c), "all done");
    ctr(c, 46, 1);
    ctr(remCount ? "hold to read" : "Rafiq puts them here", 55, 1);
    oled.display();
    return;
  }

  // Past the last one: the offer to be rid of them.
  if (remIdx >= remCount) {
    if (remConfirm) {
      bar("CLEAR THEM ALL");
      ctr("Throw away every", 20, 1);
      ctr("reminder?", 31, 1);
      yesNo(remYes);
      ctr("1 moves   hold yes", 55, 1);
    } else {
      bar("REMINDERS");
      ctr("that is all of them", 22, 1);
      ctr("hold to clear them", 38, 1);
      ctr("2 to go back", 54, 1);
    }
    oled.display();
    return;
  }

  const Rem& r = rems[remIdx];
  if (remShownIdx != remIdx) { remShownIdx = remIdx; remShownAt = millis(); }

  // A plain page. No title band and no rules: the reminder is the
  // screen, the way a watch shows a message. A header band across the
  // top of a short sentence makes the sentence look like a caption.
  //
  // The stored time only goes up when the clock is actually running.
  // Off the mains and off the network it can come back not knowing
  // what day it is, and a reminder stamped with a time the board
  // invented is worse than one with no time on it.
  char when[24]; when[0] = 0;
  bool hasWhen = r.at && timeOk;
  if (hasWhen) {
    time_t tt = (time_t)r.at;
    struct tm lt; localtime_r(&tt, &lt);
    strftime(when, sizeof(when), "%H:%M  %a %d %b", &lt);
  }
  const int top = hasWhen ? 13 : 3, bottom = 52;
  const int h = bottom - top + 1;

  // Fit the words to the room. Two big lines if they will go, small
  // ones if not, and a slow crawl when even small will not fit.
  int size = 2, n = wrapInto(r.text, 10, REM_LN);
  if (n * 18 > h) { size = 1; n = wrapInto(r.text, 21, REM_LN); }
  const int lh = size == 2 ? 18 : 10;
  const int blockH = n * lh;
  int off;
  if (blockH > h) {
    uint32_t travel = (uint32_t)(blockH - h);
    uint32_t climb  = travel * 1000UL / 9;       // nine pixels a second
    uint32_t cycle  = 1800 + climb + 1800;       // read, climb, read, again
    uint32_t t      = (millis() - remShownAt) % cycle;
    if      (t < 1800)         off = 0;
    else if (t < 1800 + climb) off = (int)((t - 1800) * travel / climb);
    else                       off = (int)travel;
  } else {
    off = -(h - blockH) / 2;                     // centred when it fits
  }

  const bool mid = (size == 2) || n <= 2;
  for (int i = 0; i < n; i++) {
    int y = top + i * lh - off;
    if (y > bottom || y + 8 * size < top) continue;
    if (mid) ctr(remLines[i], y, size);
    else     at(2, y, remLines[i], size);
  }

  // Hard edges. Adafruit's text has no clip, so a line halfway out of
  // the window would paint straight over the bands; this cuts it off
  // instead, and everything that belongs in the bands is drawn after.
  oled.fillRect(0, 0, SCRW, top, SSD1306_BLACK);
  oled.fillRect(0, bottom + 1, SCRW, SCRH - bottom - 1, SSD1306_BLACK);

  if (hasWhen) at(2, 2, when);
  if (r.done)  at(SCRW - 2 - 4 * 6, 2, "done");
  char ofN[14];
  snprintf(ofN, sizeof(ofN), "%d of %d", remIdx + 1, remCount);
  ctr(ofN, 56, 1);
  oled.display();
}

// A motorcycle, side on, in about 34 by 16.
static void bikeIcon(int x, int y) {
  oled.drawCircle(x + 5,  y + 11, 4, SSD1306_WHITE);
  oled.drawCircle(x + 27, y + 11, 4, SSD1306_WHITE);
  oled.drawPixel(x + 5,  y + 11, SSD1306_WHITE);
  oled.drawPixel(x + 27, y + 11, SSD1306_WHITE);
  oled.drawLine(x + 5,  y + 11, x + 13, y + 4,  SSD1306_WHITE);
  oled.drawLine(x + 13, y + 4,  x + 22, y + 5,  SSD1306_WHITE);
  oled.drawLine(x + 22, y + 5,  x + 27, y + 11, SSD1306_WHITE);
  oled.drawLine(x + 13, y + 4,  x + 16, y + 11, SSD1306_WHITE);
  oled.drawLine(x + 16, y + 11, x + 27, y + 11, SSD1306_WHITE);
  oled.fillRect(x + 10, y + 2, 7, 2, SSD1306_WHITE);
  oled.fillRect(x + 20, y + 1, 2, 4, SSD1306_WHITE);
  oled.fillRect(x + 17, y + 6, 5, 3, SSD1306_WHITE);
}
// A plate with the corners knocked off, the way a real one looks.
static void plateBox(int x, int y, int w, int h) {
  oled.drawRect(x, y, w, h, SSD1306_WHITE);
  oled.drawPixel(x, y, SSD1306_BLACK);
  oled.drawPixel(x + w - 1, y, SSD1306_BLACK);
  oled.drawPixel(x, y + h - 1, SSD1306_BLACK);
  oled.drawPixel(x + w - 1, y + h - 1, SSD1306_BLACK);
}

// A plate breaks where a real Indian plate breaks: state and district
// on top, series and number under. Thirteen characters will not cross
// 128 pixels at double size, and a small big-plate is no plate at all.
static void plateSplit(char* top, int tn, char* bot, int bn) {
  top[0] = bot[0] = 0;
  const char* sp  = strchr(bikePlate, ' ');
  const char* sp2 = sp ? strchr(sp + 1, ' ') : nullptr;
  if (sp2) {
    int n = (int)(sp2 - bikePlate);
    if (n > tn - 1) n = tn - 1;
    memcpy(top, bikePlate, n); top[n] = 0;
    snprintf(bot, bn, "%s", sp2 + 1);
  } else {
    snprintf(top, tn, "%s", bikePlate);
  }
}

// A dotted rule, for the templates that want a leader rather than a line.
static void dots(int x0, int x1, int y) {
  for (int x = x0; x < x1; x += 3) oled.drawPixel(x, y, SSD1306_WHITE);
}

// The torn edge of a ticket: little bites taken out of a white band.
static void tornEdge(int y) {
  for (int x = 4; x < SCRW; x += 9) oled.fillCircle(x, y, 3, SSD1306_BLACK);
}

static void drawBike() {
  oled.clearDisplay();
  char top[14], bot[14];
  plateSplit(top, sizeof(top), bot, sizeof(bot));
  char line[34];

  switch (bikeEdit ? bikeTry : cfgBikeTpl) {

    case 1: {                                    // badge: the bike itself
      ctr(bikePlate, 2, 1);
      oled.drawFastHLine(0, 12, SCRW, SSD1306_WHITE);
      bikeIcon(3, 20);
      oled.drawFastVLine(40, 15, 47, SSD1306_WHITE);
      at(44, 17, bikeMake);
      at(44, 29, bikeModel);
      at(44, 43, bikeOwner);
      break; }

    case 2: {                                    // garage board: the paperwork
      bar("VEHICLE");
      const char* K[4] = { "PLATE", "MAKE", "MODEL", "OWNER" };
      const char* V[4] = { bikePlate, bikeMake, bikeModel, bikeOwner };
      for (int i = 0; i < 4; i++) {
        int y = 14 + i * 10;
        at(4, y, K[i]);
        int vw = (int)strlen(V[i]) * 6;
        at(SCRW - 4 - vw, y, V[i]);
        if (i < 3) dots(6 + (int)strlen(K[i]) * 6, SCRW - 6 - vw, y + 4);
      }
      break; }

    case 3: {                                    // ticket stub: make on the tab
      oled.fillRect(0, 0, SCRW, 13, SSD1306_WHITE);
      oled.setTextColor(SSD1306_BLACK);
      ctr(bikeMake, 3, 1);
      oled.setTextColor(SSD1306_WHITE);
      tornEdge(13);
      if (bot[0]) { ctr(top, 19, 2); ctr(bot, 36, 2); }
      else        { ctr(top, 28, 2); }
      snprintf(line, sizeof(line), "%s / %s", bikeModel, bikeOwner);
      ctr(line, 55, 1);
      break; }

    case 4: {                                    // speedo: for the look of it
      const int CX = 64, CY = 50, R = 36;
      for (int d = 180; d <= 360; d += 2)
        oled.drawPixel(CX + (int)(R * cosf(d * 0.01745f)),
                       CY + (int)(R * sinf(d * 0.01745f)), SSD1306_WHITE);
      for (int i = 0; i <= 6; i++) {             // seven ticks, long every other
        float a = (180 + 30 * i) * 0.01745f;
        int len = (i & 1) ? 3 : 6;
        for (int r = R - len; r <= R; r++)
          oled.drawPixel(CX + (int)(r * cosf(a)), CY + (int)(r * sinf(a)), SSD1306_WHITE);
      }
      float a = (180 + 30 * 4.2f) * 0.01745f;    // parked somewhere believable
      oled.drawLine(CX, CY, CX + (int)(20 * cosf(a)), CY + (int)(20 * sinf(a)),
                    SSD1306_WHITE);
      oled.fillCircle(CX, CY, 2, SSD1306_WHITE);
      ctr(bikeModel, 2, 1);
      plateBox(18, 51, 92, 13);
      ctr(bikePlate, 54, 1);
      break; }

    case 5: {                                    // minimal: the plate, nothing else
      if (bot[0]) { ctr(top, 16, 2); ctr(bot, 36, 2); }
      else        { ctr(top, 24, 2); }
      break; }

    default: {                                   // plate card: the plate, framed
      plateBox(6, 2, SCRW - 12, bot[0] ? 38 : 24);
      if (bot[0]) { ctr(top, 5, 2); ctr(bot, 22, 2); }
      else        { ctr(top, 9, 2); }
      ctr(bikeMake, 43, 1);
      snprintf(line, sizeof(line), "%s  %s", bikeModel, bikeOwner);
      ctr(line, 53, 1);
      break; }
  }

  // Choosing says so, over whatever is underneath, because half of
  // these fill the screen and a hint drawn politely into a gap would
  // land on top of a number plate.
  if (bikeEdit) {
    char w[24];
    snprintf(w, sizeof(w), "%d of %d  hold to keep", bikeTry + 1, BIKE_TPL_N);
    oled.fillRect(0, 53, SCRW, 11, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    ctr(w, 56, 1);
    oled.setTextColor(SSD1306_WHITE);
  }
  oled.display();
}

static void drawSystem() {
  oled.clearDisplay();
  titleBar("SYSTEM", FW_VERSION);

  // Five rows. A glyph is seven high in an eight high cell, so at nine
  // apart the last paints 49 to 55 and the address below it 56 to 62,
  // with the screen ending at 63. The rule that used to sit between
  // them is what paid for the fifth row.
  char v[22];
  const int LY[5] = { 13, 22, 31, 40, 49 };
  const char* LB[5] = { "Uptime", "Network", "Memory", "Touch", "Battery" };

  unsigned long s = millis() / 1000UL;
  if (s >= 3600UL) snprintf(v, sizeof(v), "%luh %lum", s / 3600UL, (s / 60UL) % 60UL);
  else             snprintf(v, sizeof(v), "%lum", s / 60UL);
  String vals[5];
  vals[0] = v;
  if (online())      snprintf(v, sizeof(v), "%d dBm", (int)WiFi.RSSI());
  else if (rescueAP) snprintf(v, sizeof(v), "hotspot");
  else               snprintf(v, sizeof(v), "offline");
  vals[1] = v;
  snprintf(v, sizeof(v), "%u kB", (unsigned)(ESP.getFreeHeap() / 1024));
  vals[2] = v;
  // Live, so you can watch it change with a finger on the pad, and a
  // count so a touch that happened while the screen was elsewhere still
  // shows. "rest hi" or "rest lo" says which way round it decided the
  // board drives the pin.
  { char tc[8]; numStr(tc, sizeof(tc), touchCount);
    snprintf(v, sizeof(v), "%s %s%s", touchOn ? "ON" : "--",
             tc, cfgKnock ? " +k" : ""); }
  vals[3] = v;
  // What is left in the pack, with the volts beside it: a percentage
  // with nothing behind it is hard to argue with when it looks wrong.
  if (isnan(battV)) snprintf(v, sizeof(v), "none");
  else              snprintf(v, sizeof(v), "%d%% %.2fV", battPct(battV), battV);
  vals[4] = v;

  for (int i = 0; i < 5; i++) {
    at(4, LY[i], LB[i]);
    oled.setCursor(SCRW - 4 - (int)vals[i].length() * 6, LY[i]);
    oled.print(vals[i]);
  }

  String ip = online() ? WiFi.localIP().toString()
            : rescueAP ? WiFi.softAPIP().toString()
                       : String("no address");
  ctr(ip.c_str(), 57, 1);
  oled.display();
}

// ---- the work session ----
// A word, held for a moment, over whatever happens to be on screen.
//
// This used to live inside the focus screen, and the loop forced the
// screen to focus for as long as one was up. So saying SET in the tap
// settings, or DONE on a list, threw you onto the focus screen and left
// you there. A word about what just happened should never move you.
static void drawFlash() {
  oled.clearDisplay();
  oled.fillRect(0, 0, SCRW, SCRH, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  int n = strlen(flashWord);
  int size = n * 12 <= SCRW - 8 ? 2 : 1;
  ctr(flashWord, size == 2 ? 20 : 26, size);
  if (breakDue) ctr("Walk for a minute", 42, 1);
  oled.setTextColor(SSD1306_WHITE);
  oled.display();
}

static void drawFocus() {
  if (swOn)      { drawStopwatch(); return; }
  if (depth == 1) { drawFocusList(); return; }
  oled.clearDisplay();

  if (!sessionRunning()) {
    bar("FOCUS");
    if (taskCount) {
      char l[26];
      int left = 0;
      for (int i = 0; i < taskCount; i++) if (!tasks[i].done) left++;
      snprintf(l, sizeof(l), "%d still to do", left);
      ctr(l, 24, 1);
      ctr("Hold to see", 42, 1);
    } else {
      ctr("Nothing planned", 22, 1);
      ctr("Hold for the", 38, 1);
      ctr("stopwatch", 48, 1);
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
    at(6, 4, "FOCUS");
    ctr(fzLine, 27, 1);
    ctr(m, 45, 1);
    oled.display();
    return;
  }

  // Rebuilt to stop it looking like a form. The inverted bar, the boxed
  // progress rail and the scrolling name were three hard edges stacked
  // on a panel this small. What is left is a word, the time, and a line
  // showing how far in you are, with room around all three.
  const Task& t = tasks[taskIdx];
  bool brk = isBreak(t.name);

  long left = (long)(taskEnd - millis()) / 1000L;
  if (left < 0) left = 0;

  at(6, 4, brk ? "BREAK" : "FOCUS");
  if (taskCount > 1) {                       // only worth saying when there is a list
    char r[12];
    snprintf(r, sizeof(r), "%d/%d", taskIdx + 1, taskCount);
    at(SCRW - 6 - (int)strlen(r) * 6, 4, r);
  }

  // Hours only appear once there are hours, so a short run is not padded
  // out with a leading zero that never changes.
  char big[12];
  if (left >= 3600) snprintf(big, sizeof(big), "%ld:%02ld", left / 3600, (left % 3600) / 60);
  else              snprintf(big, sizeof(big), "%ld:%02ld", left / 60, left % 60);
  int bw = (int)strlen(big) * 18;
  oled.setTextSize(3);
  oled.setCursor((SCRW - bw) / 2, 22);
  oled.print(big);
  oled.setTextSize(1);

  // A thin rail with a thicker run along it, and no box around either.
  long total = (long)t.mins * 60;
  const int RX = 14, RW = SCRW - 28;
  int fill = total > 0 ? (int)((long)RW * (total - left) / total) : 0;
  fill = constrain(fill, 0, RW);
  oled.drawFastHLine(RX, 57, RW, SSD1306_WHITE);
  if (fill > 0) oled.fillRect(RX, 55, fill, 4, SSD1306_WHITE);
  oled.display();
}

// ================================================================
//  WHAT THERE IS TO DO
// ================================================================
//  The list lives here rather than on its own screen, because this is
//  where you come to start something. The stopwatch sits at the end of
//  it: it used to be what home turned into when the clock was unknown,
//  which meant one was always running whether or not you wanted it.
static int focusRows() { return taskCount + 1; }        // the tasks, then the stopwatch

static void drawFocusList() {
  oled.clearDisplay();
  char r[12];
  snprintf(r, sizeof(r), "%d/%d", itemIdx + 1, focusRows());
  titleBar("TO DO", r);

  int first = itemIdx > 3 ? itemIdx - 3 : 0;
  if (first > focusRows() - 4) first = focusRows() - 4;
  if (first < 0) first = 0;

  for (int k = 0; k < 4 && first + k < focusRows(); k++) {
    int i = first + k, y = 14 + k * 12;
    bool on = (i == itemIdx);
    if (on) { oled.fillRect(0, y - 2, SCRW, 12, SSD1306_WHITE); oled.setTextColor(SSD1306_BLACK); }
    else      oled.setTextColor(SSD1306_WHITE);

    if (i == taskCount) {                       // the stopwatch, always last
      int c = on ? SSD1306_BLACK : SSD1306_WHITE;
      // A watch face with a hand and a crown, in nine pixels.
      oled.drawCircle(8, y + 3, 4, c);
      oled.drawFastVLine(8, y, 4, c);
      oled.drawFastHLine(6, y - 2, 5, c);
      at(16, y, "Stopwatch");
      if (swMs()) {                             // it has something on it
        char e[14]; swStr(e, sizeof(e));
        at(SCRW - 4 - (int)strlen(e) * 6, y, e);
      }
    } else {
      // a tick in front of anything already finished
      if (tasks[i].done) {
        int c = on ? SSD1306_BLACK : SSD1306_WHITE;
        oled.drawLine(4, y + 4, 6, y + 6, c);
        oled.drawLine(6, y + 6, 10, y + 1, c);
      }
      String nm = tasks[i].name;
      if (nm.length() > 14) nm = nm.substring(0, 14);
      at(13, y, nm.c_str());
      char m[8];
      snprintf(m, sizeof(m), "%dm", tasks[i].mins);
      at(SCRW - 4 - (int)strlen(m) * 6, y, m);
    }
  }
  oled.setTextColor(SSD1306_WHITE);
  oled.display();
}

//  Counts up rather than down, and only when you have asked it to.
static void drawStopwatch() {
  oled.clearDisplay();
  char e[14];
  swStr(e, sizeof(e));
  bar("STOPWATCH");

  // Big enough to read across a desk, and it steps down a size rather
  // than running off the edge once it has been going an hour.
  int sz = strlen(e) > 7 ? 2 : (strlen(e) > 5 ? 2 : 3);
  oled.setTextSize(sz);
  oled.setCursor((SCRW - (int)strlen(e) * 6 * sz) / 2, sz == 3 ? 20 : 24);
  oled.print(e);
  oled.setTextSize(1);

  // Running or not, said without a word: two bars for held, one
  // triangle for going.
  if (swRun) {
    oled.fillRect(58, 44, 3, 8, SSD1306_WHITE);
    oled.fillRect(64, 44, 3, 8, SSD1306_WHITE);
  } else {
    for (int i = 0; i < 8; i++)
      oled.drawFastVLine(60 + i, 44 + i / 2, 8 - (i / 2) * 2, SSD1306_WHITE);
  }
  ctr(swRun ? "1 stop   hold zero" : (swMs() ? "1 go   hold zero" : "1 to start"), 56, 1);
  oled.display();
}

// ================================================================
//  ON A BREAK
// ================================================================
//  The Mac locks itself the moment this starts. The panel holds the
//  sign so anyone at the desk can see it without waking anything.
static void drawDnd() {
  oled.clearDisplay();
  long left = (long)(dndUntil - millis()) / 1000L;
  if (left < 0) left = 0;

  // a mug, because it reads at a glance from across a room
  int cx = 22, cy = 20;
  oled.drawRoundRect(cx - 9, cy - 6, 18, 14, 3, SSD1306_WHITE);
  oled.drawCircle(cx + 12, cy + 1, 4, SSD1306_WHITE);
  for (int i = 0; i < 3; i++)
    oled.drawFastVLine(cx - 5 + i * 5, cy - 12, 4, SSD1306_WHITE);

  at(44, 10, "ON A", 2);
  at(44, 27, "BREAK", 2);

  char m[24];
  if (left >= 60) snprintf(m, sizeof(m), "%ld min left", (left + 59) / 60);
  else            snprintf(m, sizeof(m), "%ld sec left", left);
  ctr(m, 48, 1);
  ctr(DND_LINES[dndLine], 57, 1);
  oled.display();
}

// ================================================================
//  CAMERA AND MICROPHONE
// ================================================================
//  Shown large and held awake, because the whole value of it is being
//  readable from wherever you are sitting.
static void drawBusy() {
  oled.clearDisplay();
  bool both = busyCam && busyMic;
  int cx = both ? 36 : 64;

  if (busyCam) {                             // a camera body with a lens
    oled.fillRoundRect(cx - 22, 14, 36, 26, 4, SSD1306_WHITE);
    oled.fillCircle(cx - 4, 27, 9, SSD1306_BLACK);
    oled.fillCircle(cx - 4, 27, 4, SSD1306_WHITE);
    oled.fillRect(cx + 14, 20, 8, 14, SSD1306_WHITE);
  }
  if (busyMic) {                             // a capsule on a stand
    int mx = both ? 94 : cx;
    oled.fillRoundRect(mx - 6, 12, 12, 20, 6, SSD1306_WHITE);
    oled.drawCircle(mx, 28, 11, SSD1306_WHITE);
    oled.drawFastVLine(mx, 39, 5, SSD1306_WHITE);
    oled.drawFastHLine(mx - 7, 44, 15, SSD1306_WHITE);
  }

  const char* w = both ? "CAMERA AND MIC ON" : (busyCam ? "CAMERA ON" : "MIC ON");
  int tw = (int)strlen(w) * 6;
  oled.fillRect((SCRW - tw) / 2 - 3, 52, tw + 6, 11, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  ctr(w, 54, 1);
  oled.setTextColor(SSD1306_WHITE);
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
    ctr("Hold for a code", 26, 1);
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
  if (toastKind == "copy")   head = "COPIED";
  if (toastKind == "paste")  head = "PASTED";
  if (toastKind == "break")  head = "TAKE A BREAK";
  if (toastKind == "remind") head = "REMINDER";
  if (toastKind == "note")   head = "REMINDERS";
  bar(head);

  // A reminder you did not see is not a reminder, so the first half
  // second of one is inverted. It catches the eye from across a desk
  // the way a quiet change of screen never does.
  bool loud = (toastKind == "remind" || toastKind == "break");
  if (loud && toastFlash && (long)(millis() - toastFlash) < 500) {
    bool on = ((millis() - toastFlash) / 125) % 2 == 0;
    if (on) {
      oled.fillRect(0, 0, SCRW, SCRH, SSD1306_WHITE);
      oled.display();
      return;
    }
  }
  if (toastKind == "break" || toastKind == "remind") {
    long m = (long)(toastUntil - millis()) / 1000L;
    ctr(toastText.length() ? toastText.c_str()
                           : "Stand up, look away", 24, 1);
    ctr("Touch twice to snooze", 40, 1);
    int bw = SCRW - 30;
    oled.drawRect(15, 52, bw, 5, SSD1306_WHITE);
    if (m > 0) oled.fillRect(16, 53, (bw - 2) * constrain((int)m, 0, 20) / 20, 3, SSD1306_WHITE);
  } else {
    // Two lines of it, and no more: this is a glance, not a read.
    //
    // A newline in the text is where the writer wanted the break. The
    // card that says how many reminders landed is written as two
    // lines and was being wrapped on width instead, so the newline
    // came out as a glyph in the middle of a sentence and neither
    // line sat where it should.
    String t = toastText;
    int nl = t.indexOf('\n');
    String a, b;
    if (nl >= 0)                { a = t.substring(0, nl); b = t.substring(nl + 1); }
    else if (t.length() <= 21)  { a = t; }
    else {
      int cut = 21;
      for (int i = 21; i > 8; i--) if (t[i] == ' ') { cut = i; break; }
      a = t.substring(0, cut);
      b = t.substring(cut);
    }
    a.trim(); b.trim();
    if (a.length() > 21) { a = a.substring(0, 19); a += ".."; }
    if (b.length() > 21) { b = b.substring(0, 19); b += ".."; }
    bool bell = (toastKind == "note");
    if (b.length()) {
      if (bell) bellIcon(SCRW / 2, 24, 7);
      ctr(a.c_str(), bell ? 38 : 22, 1);
      ctr(b.c_str(), bell ? 50 : 34, 1);
    } else {
      if (bell) bellIcon(SCRW / 2, 26, 8);
      ctr(a.c_str(), bell ? 44 : 28, 1);
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

// ================================================================
//  HOW HARD A KNOCK HAS TO BE
// ================================================================
//  Two things you can do here: try the strengths out, or pick one.
//  Trying one out is the point, because how hard your desk needs to be
//  hit depends on your desk, not on a number.
static void drawTapMenu() {
  oled.clearDisplay();
  bar("TAP STRENGTH");
  for (int i = 0; i < 2; i++) {
    int cx = i ? 92 : 36;
    bool on = (subIdx == i);
    if (on) oled.drawRoundRect(cx - 27, 16, 54, 34, 5, SSD1306_WHITE);
    if (i == 0) knockIcon(cx, 28);               // try one out
    else        gearIcon(cx, 28, 9);             // pick one
    at(cx - (i ? 9 : 9), 40, i ? "set" : "try");
  }
  char v[22];
  snprintf(v, sizeof(v), "now: %s", TAP_NAME[cfgTap]);
  ctr(v, 54, 1);
  oled.display();
}

//  Knock at it and watch. The count is what the chip reported, and the
//  number beside it is how hard the last shove actually was, so a
//  strength that never registers is obvious rather than mysterious.
//  Which one to try. Knock through them, two knocks to go and hit it.
static void drawTapTry() {
  oled.clearDisplay();
  bar("TRY WHICH ONE");
  for (int i = 0; i < TAP_N; i++) {
    int y = 14 + i * 11;
    bool on = (i == tapPick);
    if (on) { oled.fillRect(0, y - 2, SCRW, 11, SSD1306_WHITE); oled.setTextColor(SSD1306_BLACK); }
    else      oled.setTextColor(SSD1306_WHITE);
    at(4, y, TAP_NAME[i]);
    if (i == cfgTap) at(SCRW - 4 - 3 * 6, y, "now");
  }
  oled.setTextColor(SSD1306_WHITE);
  oled.display();
}

//  Knock at it and watch. Single knocks are the thing being tested and
//  do nothing else; two knocks is the way back, so a strength you can
//  barely register is still one you can leave.
static void drawTapTest() {
  oled.clearDisplay();
  titleBar("TRYING", TAP_NAME[tapPick]);

  char c[10];
  snprintf(c, sizeof(c), "%lu", (unsigned long)tapSeen);
  oled.setTextSize(3);
  oled.setCursor(6, 22);
  oled.print(c);
  oled.setTextSize(1);

  // The trace lives in its own column on the right, and is kept clear of
  // both the label above it and the line along the bottom. Every bar is
  // a moment, and its height is how hard that shove was, whether or not
  // the chip counted it. The short ones are the point.
  const int TX = 74, BASE = 50, TOP = 22;
  oled.drawFastHLine(TX, BASE, TAP_TRACE * 2, SSD1306_WHITE);
  for (int i = 0; i < TAP_TRACE; i++) {
    int k = (tapTraceAt + i) % TAP_TRACE;
    int h = tapTrace[k] * (BASE - TOP) / 255;
    if (h > 0) oled.drawFastVLine(TX + i * 2, BASE - h, h, SSD1306_WHITE);
  }
  at(TX, 13, "jolt");

  // No way out is offered because there is none to offer: every knock
  // in here is being measured, so none of them can also be a command.
  // What it shows instead is how much of the window is left.
  uint32_t left = (int32_t)(tapTestEnds - millis());
  if ((int32_t)left < 0) left = 0;
  char t[20];
  snprintf(t, sizeof(t), "%lus left", (unsigned long)((left + 999) / 1000));
  ctr(t, 56, 1);
  oled.display();
}

static void drawTapSet() {
  oled.clearDisplay();
  bar("SET STRENGTH");
  for (int i = 0; i < TAP_N; i++) {
    int y = 14 + i * 11;
    bool on = (i == tapPick);
    if (on) { oled.fillRect(0, y - 2, SCRW, 11, SSD1306_WHITE); oled.setTextColor(SSD1306_BLACK); }
    else      oled.setTextColor(SSD1306_WHITE);
    at(4, y, TAP_NAME[i]);
    if (i == cfgTap) at(SCRW - 4 - 3 * 6, y, "now");
  }
  oled.setTextColor(SSD1306_WHITE);
  oled.display();
}

static void drawSettings() {
  oled.clearDisplay();
  if (depth == 0) {
    bar("SETTINGS");
    gearIcon(SCRW / 2, 32, 11);
    ctr("Hold to open", 52, 1);
    oled.display();
    return;
  }
  if (depth == 2 && itemIdx == C_ABOUT) { drawAbout(); return; }
  if (depth == 2 && itemIdx == C_ACCEL) { drawAccel(); return; }
  if (depth == 2 && itemIdx == C_PAIR)  { drawPair();  return; }
  if (depth == 2 && itemIdx == C_RESET) {
    oled.clearDisplay();
    bar("RESET SETTINGS");
    ctr("Put everything back", 18, 1);
    ctr("the way it came?", 28, 1);
    ctr("Networks stay", 40, 1);
    ctr("1 moves   hold yes", 54, 1);
    oled.display();
    return;
  }
  if (itemIdx == C_TAP && depth >= 2) {
    if (depth == 2) { drawTapMenu(); return; }
    if (subIdx == 1) { drawTapSet(); return; }
    if (tapChosen)  { drawTapTest(); return; }
    drawTapTry(); return;
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
      case C_BRIGHT: { int k = 0;
                       for (int j = 0; j < BRIGHT_N; j++) if (BRIGHT_OPTS[j] == cfgBright) k = j;
                       snprintf(v, sizeof(v), "%s", BRIGHT_NAME[k]); break; }
      case C_FACE:   snprintf(v, sizeof(v), "%s", FACE_NAME[cfgFace]); break;
      case C_PRAYER: snprintf(v, sizeof(v), "%s", prayerOk ? "saved" : "none"); break;
      case C_ACCEL:  snprintf(v, sizeof(v), "hold"); break;
      case C_KNOCK:  snprintf(v, sizeof(v), "%s", cfgKnock ? "on" : "off"); break;
      case C_HIJRI:  snprintf(v, sizeof(v), "%+d d", cfgHijriAdj); break;
      case C_MODE:   snprintf(v, sizeof(v), "%s", cfgOffline ? "off" :
                              (netDown ? "no signal" : "on")); break;
      case C_BIKE:   snprintf(v, sizeof(v), "%s", cfgBike ? "on" : "off"); break;
      case C_SHAKE:  snprintf(v, sizeof(v), "%s", BACK_NAME[cfgBack]); break;
      case C_DEEP:   snprintf(v, sizeof(v), "%s", DEEP_NAME[cfgDeepIdx]); break;
      case C_BATT:   snprintf(v, sizeof(v), "%.2fV", battFull); break;
      case C_PAIR:   snprintf(v, sizeof(v), "%s", cfgLock ? "paired" : "hold"); break;
      // Only means anything with knocking switched on, and says so
      // rather than offering a setting that does nothing.
      case C_TAP:    snprintf(v, sizeof(v), "%s",
                              cfgKnock ? TAP_SHORT[cfgTap] : "off"); break;
      case C_AUTOUP: snprintf(v, sizeof(v), "%s", cfgAutoUp ? "on" : "off"); break;
      case C_ABOUT:  snprintf(v, sizeof(v), "hold"); break;
      case C_SLEEP:  if (!sleepSecs())         snprintf(v, sizeof(v), "never");
                     else if (sleepSecs() < 60) snprintf(v, sizeof(v), "%ds", sleepSecs());
                     else                       snprintf(v, sizeof(v), "%dm", sleepSecs() / 60); break;
      case C_TURN:   snprintf(v, sizeof(v), "%s", cfgAutoTurn ? "auto" : "touch"); break;
      case C_POPUP:  if (!popupSecs()) snprintf(v, sizeof(v), "off");
                     else snprintf(v, sizeof(v), "%ds", popupSecs()); break;
      case C_EYES:   snprintf(v, sizeof(v), "%s", STYLES[cfgEyes].name); break;
      case C_HOTSPOT:snprintf(v, sizeof(v), "%s", rescueAP ? "on" : "hold"); break;
      case C_UPDATE: snprintf(v, sizeof(v), "%s", online() ? "hold" : "offline"); break;
      // Everything left is something you open rather than something
      // with a value. "x2" meant knock twice, from when knocking was
      // the only way in; holding is how you open anything now.
      default:       snprintf(v, sizeof(v), "hold"); break;
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
      ctr("1 next   hold opens", 55, 1);
      break;
    }

    case U_LOOK: {
      // Something to watch while it asks GitHub, instead of a panel that
      // has apparently dropped off to sleep.
      bar("LOOKING");
      ctr("Asking GitHub", 22, 1);
      int dots = (int)((millis() / 400) % 4);
      for (int i = 0; i < 3; i++) {
        int x = SCRW / 2 - 8 + i * 8;
        if (i < dots) oled.fillCircle(x, 40, 2, SSD1306_WHITE);
        else          oled.drawCircle(x, 40, 2, SSD1306_WHITE);
      }
      ctr("3 touches to stop", 54, 1);
      break;
    }

    case U_ASK:
      titleBar("INSTALL", "");
      ctr(upTag.length() ? upTag.c_str() : "unknown", 15, 1);
      ctr("Put this one on?", 26, 1);
      yesNo(upYes);
      ctr("1 moves   hold yes", 55, 1);
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
      ctr("Touch to go back", 50, 1);
      break;
    default:
      titleBar("UPDATE", "");
      ctr(upMsg.length() ? upMsg.c_str() : "Something went wrong", 24, 1);
      ctr("Touch to go back", 46, 1);
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
enum { G_SNAKE = 0, G_BRICK, G_CAR, G_CATCH, G_PONG, G_ROLL,
       G_ECHO, G_MAZE, G_DIZZY, G_BALLOON, G_COUNT };
const char* G_NAME[G_COUNT] =
  { "Snake", "Brick", "Car", "Catch", "Pong", "Roll", "Echo", "Maze",
    "Dizzy", "Balloon" };

// The last three are for a small person, and none of them asks for the
// tilt lesson first: being made to hold a thing still, lean it right and
// lean it away before anything happens is a reasonable price for Snake
// and no price a four year old will pay. Copy Me is knocked, and the
// other two are a shake and a knock.
static bool gameUsesTilt(int g) {
  return !(g == G_ECHO || g == G_DIZZY || g == G_BALLOON);
}
#define G_PER_PAGE 2
#define G_PAGES ((G_COUNT + G_PER_PAGE - 1) / G_PER_PAGE)

enum { GS_CAL_STILL = 0, GS_CAL_RIGHT, GS_CAL_AWAY, GS_READY, GS_PLAY, GS_PAUSE, GS_OVER };
int  gState = GS_READY;
int  gamePending = -1;
int  gScore = 0, gBest[G_COUNT] = {};
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

// ---------------- Echo ----------------
//  For someone small. Nothing to dodge, nothing that speeds up and no
//  tilting: the robot taps a little pattern and you touch it back,
//  and every round it adds one more. Beats are one, two or three knocks
//  and never four, because four is the top of what the chip counts and
//  the one most likely to come out as three on a small hand.
#define EC_MAX 16
uint8_t ecPat[EC_MAX];
int  ecLen = 1, ecAt = 0, ecPip = 0;
bool ecShow = true;                  // true while the robot is doing the knocking
unsigned long ecNext = 0;

static void ecReset() {
  ecLen = 1;
  for (int i = 0; i < EC_MAX; i++) ecPat[i] = 1 + random(3);
  ecAt = 0; ecPip = 0; ecShow = true;
  ecNext = millis() + 700;
  gScore = 0;
}
static void ecStep() {
  if (!ecShow) return;                            // your turn; nothing ticks
  unsigned long now = millis();
  if ((long)(now - ecNext) < 0) return;
  if (ecPip < ecPat[ecAt]) { ecPip++; ecNext = now + 330; return; }
  ecAt++; ecPip = 0;                              // a gap, so beats do not run together
  if (ecAt >= ecLen) { ecShow = false; ecAt = 0; return; }
  ecNext = now + 520;
}
static void ecKnock(uint8_t n) {
  if (ecShow) return;                             // still showing you; wait
  if (n != ecPat[ecAt]) { gState = GS_OVER; return; }
  ecAt++;
  if (ecAt < ecLen) return;
  gScore = ecLen;                                 // a whole pattern back
  if (ecLen < EC_MAX) ecLen++;
  ecAt = 0; ecPip = 0; ecShow = true;
  ecNext = millis() + 800;
}
static void ecDraw() {
  ctr(ecShow ? "listen" : "your turn", 15, 1);
  if (ecShow) {
    int n = ecPat[ecAt];
    int x0 = SCRW / 2 - (n * 18) / 2 + 9;
    for (int i = 0; i < n; i++) {
      if (i < ecPip) oled.fillCircle(x0 + i * 18, 33, 7, SSD1306_WHITE);
      else           oled.drawCircle(x0 + i * 18, 33, 7, SSD1306_WHITE);
    }
  } else {
    ctr("touch it back", 30, 1);
  }
  // one block per beat, filled for the ones already knocked back
  int w = ecLen > 12 ? 5 : 7;
  int x0 = SCRW / 2 - (ecLen * w) / 2;
  for (int i = 0; i < ecLen; i++) {
    if (!ecShow && i < ecAt) oled.fillRect(x0 + i * w, 48, w - 2, 6, SSD1306_WHITE);
    else                     oled.drawRect(x0 + i * w, 48, w - 2, 6, SSD1306_WHITE);
  }
}

// ---------------- Maze ----------------
//  The other one for someone small. No timer, nothing chasing anybody
//  and no way to lose: you lean the dot to the star and it draws a new
//  one. A wall simply does not let you through.
#define MZ_COLS 15
#define MZ_ROWS 6
#define MZ_CELL 8
#define MZ_X0   4
#define MZ_Y0   14
#define MZ_N    (MZ_COLS * MZ_ROWS)
uint8_t mzW[MZ_N];                // bit 0 north, 1 east, 2 south, 3 west; set means wall
int mzX = 0, mzY = 0, mzLevel = 1;
unsigned long mzMove = 0;

static void mzCarve() {
  for (int i = 0; i < MZ_N; i++) mzW[i] = 0x0F;
  bool seen[MZ_N];
  for (int i = 0; i < MZ_N; i++) seen[i] = false;
  uint8_t stk[MZ_N];
  int sp = 0;
  seen[0] = true; stk[sp++] = 0;
  while (sp) {
    int cur = stk[sp - 1];
    int cx = cur % MZ_COLS, cy = cur / MZ_COLS;
    int cand[4], dir[4], nc = 0;
    if (cy > 0           && !seen[cur - MZ_COLS]) { cand[nc] = cur - MZ_COLS; dir[nc++] = 0; }
    if (cx < MZ_COLS - 1 && !seen[cur + 1])       { cand[nc] = cur + 1;       dir[nc++] = 1; }
    if (cy < MZ_ROWS - 1 && !seen[cur + MZ_COLS]) { cand[nc] = cur + MZ_COLS; dir[nc++] = 2; }
    if (cx > 0           && !seen[cur - 1])       { cand[nc] = cur - 1;       dir[nc++] = 3; }
    if (!nc) { sp--; continue; }
    int k = random(nc), nx = cand[k], d = dir[k];
    mzW[cur] &= ~(1 << d);
    mzW[nx]  &= ~(1 << ((d + 2) & 3));
    seen[nx] = true;
    stk[sp++] = nx;
  }
}
static void mzReset() {
  mzLevel = 1; gScore = 0;
  mzCarve(); mzX = 0; mzY = 0; mzMove = 0;
}
static void mzStep() {
  unsigned long now = millis();
  if ((long)(now - mzMove) < 0) return;
  float tx, ty;
  tiltRead(tx, ty);
  const float T = 0.22f;
  int d = -1;
  if (fabsf(tx) > fabsf(ty)) { if (tx > T) d = 1; else if (tx < -T) d = 3; }
  else                       { if (ty > T) d = 0; else if (ty < -T) d = 2; }
  if (d < 0) return;
  int cur = mzY * MZ_COLS + mzX;
  if (mzW[cur] & (1 << d)) { mzMove = now + 150; return; }   // a wall, and that is that
  if      (d == 0) mzY--;
  else if (d == 1) mzX++;
  else if (d == 2) mzY++;
  else             mzX--;
  mzMove = now + 170;
  if (mzX == MZ_COLS - 1 && mzY == MZ_ROWS - 1) {
    gScore = mzLevel;
    mzLevel++;
    mzCarve(); mzX = 0; mzY = 0;
    mzMove = now + 450;
  }
}
static void mzDraw() {
  for (int y = 0; y < MZ_ROWS; y++)
    for (int x = 0; x < MZ_COLS; x++) {
      int c = y * MZ_COLS + x;
      int px = MZ_X0 + x * MZ_CELL, py = MZ_Y0 + y * MZ_CELL;
      if (mzW[c] & 1) oled.drawFastHLine(px, py, MZ_CELL + 1, SSD1306_WHITE);
      if (mzW[c] & 2) oled.drawFastVLine(px + MZ_CELL, py, MZ_CELL + 1, SSD1306_WHITE);
      if (mzW[c] & 4) oled.drawFastHLine(px, py + MZ_CELL, MZ_CELL + 1, SSD1306_WHITE);
      if (mzW[c] & 8) oled.drawFastVLine(px, py, MZ_CELL + 1, SSD1306_WHITE);
    }
  int sx = MZ_X0 + (MZ_COLS - 1) * MZ_CELL + MZ_CELL / 2;
  int sy = MZ_Y0 + (MZ_ROWS - 1) * MZ_CELL + MZ_CELL / 2;
  oled.drawFastHLine(sx - 3, sy, 7, SSD1306_WHITE);
  oled.drawFastVLine(sx, sy - 3, 7, SSD1306_WHITE);
  oled.fillCircle(MZ_X0 + mzX * MZ_CELL + MZ_CELL / 2,
                  MZ_Y0 + mzY * MZ_CELL + MZ_CELL / 2, 2, SSD1306_WHITE);
}

// ================================================================
//  SHARED
// ================================================================
// ---------------- Dizzy ----------------
//  Shake it and it gets giddy. The eyes spin, stars come out, and when
//  it cannot take any more it flops over and picks itself back up.
//  There is nothing to lose and nothing to be good at.
//
//  No knock does anything in here, which is the same rule as trying a
//  tap strength and for the same reason: a good shake throws off knocks
//  by the handful, and any one of them being a command would end the
//  game every time it got going. The round has an end instead.
#define DZ_MS 20000UL
float    dzSpin = 0, dzAng = 0;
uint32_t dzFlop = 0, dzEnds = 0;

static void dzReset() {
  dzSpin = 0; dzAng = 0; dzFlop = 0; gScore = 0;
  dzEnds = millis() + DZ_MS;
}
static void dzStep() {
  uint32_t now = millis();
  float jolt = fabsf(amag - 1.0f);
  if (jolt > 0.22f) dzSpin += jolt * 9.0f;
  dzSpin -= 0.55f;                                   // it always calms down
  dzSpin = constrain(dzSpin, 0.0f, 100.0f);
  dzAng += 0.02f + dzSpin * 0.012f;
  if (dzAng > 6.2832f) dzAng -= 6.2832f;
  if ((int)dzSpin > gScore) gScore = (int)dzSpin;

  if (!dzFlop && dzSpin >= 99.0f) dzFlop = now;
  if (dzFlop && now - dzFlop > 2400) { gState = GS_OVER; return; }
  if (!dzFlop && (int32_t)(now - dzEnds) >= 0) gState = GS_OVER;
}
static void dzDraw() {
  // The screen is 64 high and the title bar owns the top eleven, so
  // everything below has to be placed rather than centred and hoped
  // for. Words 11 to 19, stars 21 to 25, head 29 to 55, meter 57 to 62.
  // Drawn at full size the first time round, the head ran through the
  // meter and the stars ran through the words.
  bool over = dzFlop != 0;
  const int cy = 42, R = 13;
  // A wobble that grows with the giddiness, so the head sways harder
  // the more it has been shaken. Sideways only: dropping it down the
  // screen when it flops is what put it through the meter.
  int cx = 64 + (int)(sinf(dzAng * 2.4f) * (dzSpin / 16.0f));
  cx = constrain(cx, 64 - 8, 64 + 8);

  oled.drawCircle(cx, cy, R, SSD1306_WHITE);
  if (over) {
    // crosses for eyes, and a mouth that has given up
    for (int s = -1; s <= 1; s += 2) {
      int ex = cx + s * 6;
      oled.drawLine(ex - 3, cy - 7, ex + 3, cy - 1, SSD1306_WHITE);
      oled.drawLine(ex + 3, cy - 7, ex - 3, cy - 1, SSD1306_WHITE);
    }
    oled.drawCircle(cx, cy + 5, 4, SSD1306_WHITE);
  } else {
    for (int s = -1; s <= 1; s += 2) {
      int ex = cx + s * 6, ey = cy - 4;
      oled.drawCircle(ex, ey, 4, SSD1306_WHITE);
      // the pupil goes round and round, faster the giddier it is
      int px = ex + (int)(cosf(dzAng + (s > 0 ? 1.6f : 0)) * 2.0f);
      int py = ey + (int)(sinf(dzAng + (s > 0 ? 1.6f : 0)) * 2.0f);
      oled.fillCircle(px, py, 1, SSD1306_WHITE);
    }
    int m = 2 + (int)(dzSpin / 28);            // the mouth opens as it goes
    oled.drawCircle(cx, cy + 6, m, SSD1306_WHITE);
  }

  // Stars come out once it is properly giddy, and go round above the
  // head. On their own band at y 21 to 25, so they cannot cross either
  // the words above them or the head below.
  int stars = (int)(dzSpin / 26);
  for (int i = 0; i < stars; i++) {
    float a = dzAng * 1.7f + i * 2.1f;
    int sx = 64 + (int)(cosf(a) * 26);
    if (sx < 4 || sx > SCRW - 5) continue;     // round the back, out of sight
    oled.drawFastHLine(sx - 2, 23, 5, SSD1306_WHITE);
    oled.drawFastVLine(sx, 21, 5, SSD1306_WHITE);
  }

  if (over) ctr("whooooa", 12, 1);
  else      ctr(dzSpin < 30 ? "shake me!" : dzSpin < 75 ? "more!" : "too much!", 12, 1);

  // how giddy, along the bottom, on its own band below everything
  oled.drawRect(14, 57, 100, 6, SSD1306_WHITE);
  int w = (int)(dzSpin);
  if (w > 0) oled.fillRect(15, 58, w, 4, SSD1306_WHITE);
}

// ---------------- Balloon ----------------
//  Touch to puff it up. It bursts somewhere, and nobody knows where.
//  Every burst is one puff whatever its size, so a heavy knock that
//  bounces into three cannot cost three puffs, and no knock walks out
//  of the game either. The bang is the end of the round.
float    blSize = 6, blPop = 24;
int      blPuffs = 0;
uint32_t blBang = 0;

static void blReset() {
  blSize = 6; blPuffs = 0; blBang = 0; gScore = 0;
  // Between 15 and 20 across. Bigger than that and it runs into the
  // title bar at the top and its own string at the bottom, which on a
  // screen this size means it bursts off the edge rather than on it.
  blPop = 15 + random(6);
}
static void blPuff() {
  if (blBang) return;
  blPuffs++; gScore = blPuffs;
  blSize += 0.85f + random(7) / 10.0f;        // eight to fourteen puffs, usually
  if (blSize >= blPop) blBang = millis();
}
static void blStep() {
  if (blBang && millis() - blBang > 1800) gState = GS_OVER;
}
static void blDraw() {
  const int CX = 64, CY = 33;
  if (blBang) {
    // Spikes round the middle and the tally underneath, rather than a
    // big word in the middle with the spikes drawn through it.
    for (int i = 0; i < 12; i++) {
      float a = i * 0.5236f;
      oled.drawLine(CX + (int)(cosf(a) * 8),  30 + (int)(sinf(a) * 8),
                    CX + (int)(cosf(a) * 20), 30 + (int)(sinf(a) * 18), SSD1306_WHITE);
    }
    char m[24];
    snprintf(m, sizeof(m), "POP!  %d puff%s", blPuffs, blPuffs == 1 ? "" : "s");
    ctr(m, 54, 1);
    return;
  }
  int r = (int)blSize;
  oled.drawCircle(CX, CY, r, SSD1306_WHITE);
  oled.drawCircle(CX, CY, r - 1, SSD1306_WHITE);
  oled.fillTriangle(CX - 3, CY + r, CX + 3, CY + r, CX, CY + r + 4, SSD1306_WHITE);
  for (int y = CY + r + 4; y < 63; y += 2)   // a string that wiggles
    oled.drawPixel(CX + ((y / 2) % 2 ? 2 : -2), y, SSD1306_WHITE);
  oled.fillCircle(CX - r / 3, CY - r / 3, 2, SSD1306_WHITE);   // a shine

  // No hint over the top of it: the balloon fills this space once it
  // gets going, and the ready screen has already said to touch. The
  // tally sits in the corner, clear of the widest it can get.
  char m[10];
  snprintf(m, sizeof(m), "%d", blPuffs);
  at(3, 13, m);
}

static const char* bestKey(int g) {
  switch (g) {
    case G_SNAKE: return "bSnake";
    case G_BRICK: return "bBrick";
    case G_CAR:   return "bCar";
    case G_CATCH: return "bCatch";
    case G_PONG:  return "bPong";
    case G_ECHO:  return "bEcho";
    case G_MAZE:  return "bMaze";
    case G_DIZZY: return "bDizzy";
    case G_BALLOON: return "bBalloon";
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
    case G_ECHO:  ecReset(); break;
    case G_MAZE:  mzReset(); break;
    case G_DIZZY: dzReset(); break;
    case G_BALLOON: blReset(); break;
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
    case G_ECHO:  return "Touch it back";
    case G_MAZE:  return "Tilt to the star";
    case G_DIZZY: return "Shake me about";
    case G_BALLOON: return "Touch to puff";
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
    case G_ECHO:  gNext = now + 40;     ecStep();   break;
    case G_MAZE:  gNext = now + 30;     mzStep();   break;
    case G_DIZZY: gNext = now + 33;     dzStep();   break;
    case G_BALLOON: gNext = now + 40;   blStep();   break;
    default:      gNext = now + 26;     rlStep();   break;
  }

  // Maze never ends, so waiting for game over would mean its best was
  // never written down. A level takes long enough that saving one per
  // level costs nothing.
  if ((gState == GS_OVER || which == G_MAZE) && gScore > gBest[which]) {
    gBest[which] = gScore;
    prefs.putInt(bestKey(which), gScore);
  }
}

// ---------------- the icons ----------------
static void dizzyIcon(int x, int y) {
  oled.drawCircle(x + 15, y + 9, 7, SSD1306_WHITE);
  oled.drawPixel(x + 13, y + 8, SSD1306_WHITE);
  oled.drawPixel(x + 17, y + 8, SSD1306_WHITE);
  oled.drawCircle(x + 15, y + 12, 2, SSD1306_WHITE);
  for (int i = 0; i < 3; i++) {                  // stars going round
    int sx = x + 5 + i * 10, sy = y + (i == 1 ? 0 : 2);
    oled.drawFastHLine(sx - 1, sy, 3, SSD1306_WHITE);
    oled.drawFastVLine(sx, sy - 1, 3, SSD1306_WHITE);
  }
}
static void balloonIcon(int x, int y) {
  oled.drawCircle(x + 15, y + 8, 7, SSD1306_WHITE);
  oled.fillTriangle(x + 13, y + 15, x + 17, y + 15, x + 15, y + 18, SSD1306_WHITE);
  for (int yy = y + 18; yy < y + 24; yy += 2)
    oled.drawPixel(x + 15 + ((yy / 2) % 2 ? 1 : -1), yy, SSD1306_WHITE);
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
static void echoIcon(int x, int y) {
  for (int i = 0; i < 3; i++) {
    int cx = x + 6 + i * 9;
    if (i == 1) oled.fillCircle(cx, y + 8, 4, SSD1306_WHITE);
    else        oled.drawCircle(cx, y + 8, 4, SSD1306_WHITE);
  }
  for (int i = 0; i < 3; i++) oled.fillRect(x + 4 + i * 9, y + 18, 6, 5, SSD1306_WHITE);
}
static void mazeIcon(int x, int y) {
  oled.drawRect(x + 2, y + 3, 27, 21, SSD1306_WHITE);
  oled.drawFastVLine(x + 11, y + 3, 13, SSD1306_WHITE);
  oled.drawFastVLine(x + 20, y + 11, 13, SSD1306_WHITE);
  oled.drawFastHLine(x + 11, y + 11, 6, SSD1306_WHITE);
  oled.fillCircle(x + 6, y + 8, 2, SSD1306_WHITE);
  oled.drawFastHLine(x + 22, y + 7, 5, SSD1306_WHITE);
  oled.drawFastVLine(x + 24, y + 5, 5, SSD1306_WHITE);
}
static void gameIcon(int g, int x, int y) {
  switch (g) {
    case G_SNAKE: snakeIcon(x, y); break;
    case G_BRICK: brickIcon(x, y); break;
    case G_CAR:   carIcon(x, y);   break;
    case G_CATCH: catchIcon(x, y); break;
    case G_PONG:  pongIcon(x, y);  break;
    case G_ECHO:  echoIcon(x, y);  break;
    case G_MAZE:  mazeIcon(x, y);  break;
    case G_DIZZY: dizzyIcon(x, y); break;
    case G_BALLOON: balloonIcon(x, y); break;
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
    case G_ECHO:  snprintf(t, sizeof(t), "ECHO");               break;
    case G_MAZE:  snprintf(t, sizeof(t), "MAZE L%d", mzLevel); break;
    case G_DIZZY: snprintf(t, sizeof(t), "DIZZY");              break;
    case G_BALLOON: snprintf(t, sizeof(t), "BALLOON");          break;
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
    case G_ECHO:  ecDraw();  break;
    case G_MAZE:  mzDraw();  break;
    case G_DIZZY: dzDraw();  break;
    case G_BALLOON: blDraw(); break;
    default:      rlDraw();  break;
  }
  if (which == G_BRICK && brStuck && gState == GS_PLAY) ctr("Touch to launch", 44, 1);

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
    ctr("Hold to open", 50, 1);
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
    if (getLocalTime(&t, 120) && t.tm_year > 120) {
      timeOk = true; clockSrc = "ntp";
      Serial.println("clock set from ntp");
      return true;
    }
    // No web.handleClient() here. This runs on the network task now, and
    // the web server belongs to the loop: two tasks inside the same
    // request state machine would be a far worse bug than the pause this
    // was added to paper over. The loop keeps the page alive by itself.
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
  if (i >= 0) {
    int e = b.indexOf('"', i + 8);
    String c = b.substring(i + 8, e);
    if (c.length() > 13) c = c.substring(0, 13);
    strncpy(wCity, c.c_str(), sizeof(wCity) - 1);
    wCity[sizeof(wCity) - 1] = 0;
  }
  if (!isnan(locLat) && locLat != 0) {
    prefs.putFloat("lat", locLat); prefs.putFloat("lon", locLon);
    prefs.putString("city", String(wCity));
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
  filter["data"]["date"]["hijri"]["day"] = true;
  filter["data"]["date"]["hijri"]["month"]["number"] = true;
  filter["data"]["date"]["hijri"]["year"] = true;
  filter["data"]["date"]["gregorian"]["date"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, b, DeserializationOption::Filter(filter))) return;
  JsonObject tm_ = doc["data"]["timings"];
  if (tm_.isNull()) return;

  // The same answer carries the Hijri date, announced rather than
  // worked out, so take it while we are here. It is kept as a day
  // offset from the arithmetic one rather than as a date, because an
  // offset stays right while the robot is off the network and a date
  // would go stale overnight.
  {
    int hd = doc["data"]["date"]["hijri"]["day"] | 0;
    int hm = doc["data"]["date"]["hijri"]["month"]["number"] | 0;
    int hy = atoi(doc["data"]["date"]["hijri"]["year"] | "0");
    if (hd && hm && hy) {
      // No inverse conversion. Writing one by hand put it 385 days out
      // and the round trip caught it, so instead this tries each shift
      // worth having and keeps the one that reproduces the announced
      // date. Only the forward conversion is trusted, and that one is
      // checked against four known first-of-the-months.
      long have = gregToJdn(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
      int shift = 99;
      for (int s = -3; s <= 3; s++) {
        int ay, am, ad;
        hijriFromJdn(have + s, ay, am, ad);
        if (ay == hy && am == hm && ad == hd) { shift = s; break; }
      }
      // Further out than three days is not a shift, it is a
      // disagreement about which calendar, and guessing would be worse
      // than leaving it where the user put it.
      if (shift != 99 && shift != cfgHijriAdj) {
        cfgHijriAdj = shift;
        prefs.putInt("hadj", cfgHijriAdj);
        Serial.printf("hijri shifted %+d day to match the announced date\n", shift);
      }
      prefs.putUInt("hsync", (uint32_t)time(nullptr));
    }
  }

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
// One string, pipe separated, the way the tasks are kept. A dozen
// short lines is not worth a filesystem.
// Version 2 of the row format, marked as such on the front. Version 1
// had no id and five fields; guessing which one you are holding from
// the field count is the kind of thing that works until a reminder
// contains a separator, so it says which it is instead.
#define REM_FMT "2\x1e"
static void saveRems() {
  String s = REM_FMT;
  for (int i = 0; i < remCount; i++) {
    s += String(rems[i].id); s += '\x1f';
    s += String(rems[i].at); s += '\x1f';
    s += String(rems[i].first); s += '\x1f';
    s += String(rems[i].tries); s += '\x1f';
    s += (rems[i].done ? '1' : '0'); s += '\x1f';
    s += rems[i].text;
    if (i < remCount - 1) s += '\x1e';
  }
  prefs.putString("rems", s);
  prefs.putUInt("remid", remNextId);
}
static void loadRems() {
  remCount = 0;
  String s = prefs.getString("rems", "");
  remNextId = prefs.getUInt("remid", 1);
  int i = 0;
  bool v2 = s.startsWith(REM_FMT);
  if (v2) i = (int)strlen(REM_FMT);
  while (i < (int)s.length() && remCount < REM_MAX) {
    int e = s.indexOf('\x1e', i); if (e < 0) e = s.length();
    String row = s.substring(i, e);
    Rem& r = rems[remCount];
    int f[5] = { -1, -1, -1, -1, -1 };          // where each separator is
    int want = v2 ? 5 : 4, at = -1, ok = 1;
    for (int k = 0; k < want; k++) {
      at = row.indexOf('\x1f', at + 1);
      if (at < 0) { ok = 0; break; }
      f[k] = at;
    }
    if (ok && f[0] > 0) {
      int k = 0;
      if (v2) r.id = (uint32_t)strtoul(row.substring(0, f[k++]).c_str(), nullptr, 10);
      else    r.id = remNextId++;               // a version 1 row, given one now
      int p0 = v2 ? f[0] + 1 : 0;
      r.at    = (uint32_t)strtoul(row.substring(p0, f[k]).c_str(), nullptr, 10);
      r.first = (uint32_t)strtoul(row.substring(f[k] + 1, f[k + 1]).c_str(), nullptr, 10);
      r.tries = (uint8_t)row.substring(f[k + 1] + 1, f[k + 2]).toInt();
      r.done  = row.substring(f[k + 2] + 1, f[k + 3]) == "1";
      snprintf(r.text, REM_TEXT, "%s", row.substring(f[k + 3] + 1).c_str());
      if (r.id >= remNextId) remNextId = r.id + 1;
      remCount++;
    }
    i = e + 1;
  }
  Serial.printf("%d reminders remembered (format %d, next id %lu)\n",
                remCount, v2 ? 2 : 1, (unsigned long)remNextId);
}
// Quotes, backslashes and anything below a space, so a reminder with
// an apostrophe or a quote mark in it cannot break the response it is
// being sent in.
static String jstr(const char* t) {
  String o = "\"";
  for (const char* c = t; *c; c++) {
    if (*c == '"' || *c == '\\') { o += '\\'; o += *c; }
    else if ((uint8_t)*c < 0x20)   { o += ' '; }
    else                            o += *c;
  }
  o += '"';
  return o;
}

// The one with this id, or -1.
static int remById(uint32_t id) {
  if (!id) return -1;
  for (int i = 0; i < remCount; i++) if (rems[i].id == id) return i;
  return -1;
}
// Soonest first, so walking them is walking time.
//
// remShowing is an index into this array, so sorting it moves the
// reminder out from under whatever is pointing at it: answering the
// card would have marked a different reminder done. The id goes in
// and comes back out the other side.
static void sortRems() {
  uint32_t showId = (remShowing >= 0 && remShowing < remCount) ? rems[remShowing].id : 0;
  for (int i = 1; i < remCount; i++) {
    Rem k = rems[i]; int j = i - 1;
    while (j >= 0 && rems[j].at > k.at) { rems[j + 1] = rems[j]; j--; }
    rems[j + 1] = k;
  }
  if (showId) remShowing = remById(showId);
}
// Returns false when there is no room. The oldest finished one is
// dropped first, so a full list of done things never blocks a new one.
static bool addRem(const char* text, uint32_t when) {
  if (remCount >= REM_MAX) {
    int drop = -1;
    for (int i = 0; i < remCount; i++)
      if (rems[i].done && (drop < 0 || rems[i].at < rems[drop].at)) drop = i;
    if (drop < 0) return false;
    for (int i = drop; i < remCount - 1; i++) rems[i] = rems[i + 1];
    remCount--;
  }
  snprintf(rems[remCount].text, REM_TEXT, "%s", text);
  // The separators are the save format, so a reminder is not allowed
  // to contain one. Nothing types these; a bad request could send one.
  for (char* c = rems[remCount].text; *c; c++)
    if (*c == '\x1e' || *c == '\x1f' || *c == '\n' || *c == '\r') *c = ' ';
  rems[remCount].id = remNextId++;
  rems[remCount].at = when;
  rems[remCount].first = when;
  rems[remCount].tries = 0;
  rems[remCount].done = false;
  remCount++;
  return true;
}

static int remPending() {
  int n = 0;
  for (int i = 0; i < remCount; i++) if (!rems[i].done) n++;
  return n;
}

static void saveTasks() {
  String out;
  for (int i = 0; i < taskCount; i++) {
    String n = tasks[i].name;
    n.replace("|", " "); n.replace(";", " ");
    out += n + "|" + String(tasks[i].mins) + "|" + (tasks[i].done ? "1" : "0");
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
      String rest = part.substring(barp + 1);
      int bar2 = rest.indexOf('|');
      // A plan saved before ticking off existed has no third field, and
      // has to keep loading rather than vanishing.
      tasks[taskCount].mins = constrain((bar2 > 0 ? rest.substring(0, bar2) : rest).toInt(), 1, 240);
      tasks[taskCount].done = bar2 > 0 && rest.substring(bar2 + 1).toInt() != 0;
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
// Deliberately NOT /releases/latest.
//
// That endpoint means "most recently published", and this repository
// also carries the Mac and Windows apps. Asking for the latest worked
// only while a firmware release happened to be the newest thing in it:
// publish a Rafiq update and the next check would be handed a zip full
// of Windows with no firmware in it at all, and report that there was
// no release to be found. So walk the list and take the newest one that
// actually carries a firmware image.
//
// Twelve is measured rather than guessed. It is enough to fill the
// earlier releases list even with app releases interleaved, and small
// enough that the reply still fits in memory comfortably.
static bool otaFetchLatest() {
  if (!online()) { upMsg = "No network"; return false; }
  String b;
  if (!httpGetTo("https://api.github.com/repos/" OTA_REPO
                 "/releases?per_page=12", true, b, 15000)) {
    upMsg = "GitHub unreachable"; return false;
  }
  upTag = ""; upUrl = "";
  int i = 0;
  while (true) {
    int t = b.indexOf("\"tag_name\"", i);
    if (t < 0) break;
    String tag = jsonStr(b, "\"tag_name\"", t);
    // only this release's own assets, never the next one's
    int nextT = b.indexOf("\"tag_name\"", t + 10);
    int limit = nextT < 0 ? (int)b.length() : nextT;
    int a = b.indexOf(OTA_ASSET, t);
    if (a >= 0 && a < limit) {
      String url = jsonStr(b, "\"browser_download_url\"", a);
      if (url.length()) { upTag = tag; upUrl = url; break; }
    }
    i = t + 10;
  }
  b = String();
  if (!upTag.length() || !upUrl.length()) { upMsg = "No release found"; return false; }
  return true;
}

// The last few releases, so an older one can be put back deliberately.
static bool otaFetchList() {
  if (!online()) { upMsg = "No network"; return false; }
  String b;
  if (!httpGetTo("https://api.github.com/repos/" OTA_REPO
                 "/releases?per_page=12", true, b, 15000)) {
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
  sleptAt = millis();
  nSlept++;
  // The clock used to drop to 80MHz here. It saves almost nothing on a
  // desk and changes the bus timing at exactly the moment the sensors
  // have to stay readable, because noticing that it has been moved means
  // reading them continuously while asleep. Not worth the risk.
}
// Put back the way it was found. A reminder answered is a reminder
// over with, and standing there afterwards waiting out a screen
// timeout is the robot ignoring what just happened. Deep if it came
// from deep or there is no Mac to keep hold of, otherwise the screen
// goes dark again and the Mac keeps its connection.
static void backToSleep() {
  if (wokeForAlarm || !macLinked) { wokeForAlarm = false; wantDeep = true; }
  else                            goSleep();
}

// How long until the next prayer wants saying something about, so a
// processor that is switched off can set an alarm and still speak up.
// The times are already in flash and the clock survives being switched
// off, so none of this needs the network.
// Slot zero keeps the old ssid and pass keys, so a device that has been
// running for months comes back up on the network it already knows and
// only then notices it can hold four more.
static void loadNets() {
  netCount = 0;
  for (int i = 0; i < NET_MAX; i++) {
    String sk = i ? ("ssid" + String(i)) : String("ssid");
    String pk = i ? ("pass" + String(i)) : String("pass");
    String sv = prefs.getString(sk.c_str(), "");
    if (!sv.length()) continue;
    strncpy(netSsid[netCount], sv.c_str(), 32);          netSsid[netCount][32] = 0;
    String pv = prefs.getString(pk.c_str(), "");
    strncpy(netPass[netCount], pv.c_str(), 64);          netPass[netCount][64] = 0;
    netCount++;
  }
}

static void saveNets() {
  for (int i = 0; i < NET_MAX; i++) {
    String sk = i ? ("ssid" + String(i)) : String("ssid");
    String pk = i ? ("pass" + String(i)) : String("pass");
    if (i < netCount) {
      prefs.putString(sk.c_str(), String(netSsid[i]));
      prefs.putString(pk.c_str(), String(netPass[i]));
    } else {
      prefs.remove(sk.c_str());
      prefs.remove(pk.c_str());
    }
  }
}

// One attempt at one network. Called from the network task, so the wait
// costs the screen nothing.
static bool joinOne(int i, int ms) {
  if (i < 0 || i >= netCount || !netSsid[i][0]) return false;
  WiFi.begin(netSsid[i], netPass[i]);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < (unsigned long)ms)
    vTaskDelay(pdMS_TO_TICKS(100));
  if (WiFi.status() != WL_CONNECTED) return false;
  netUsing = i;
  cfgSsid = String(netSsid[i]);
  Serial.printf("on %s\n", netSsid[i]);
  return true;
}

// How long until the next reminder is due, in seconds, or -1.
static long secsToNextRem() {
  if (!timeOk) return -1;
  time_t now = time(nullptr);
  if (now < 1700000000) return -1;             // the clock is not set yet
  long best = -1;
  for (int i = 0; i < remCount; i++) {
    if (rems[i].done) continue;
    long d = (long)rems[i].at - (long)now;
    if (d < 0) continue;
    if (best < 0 || d < best) best = d;
  }
  return best;
}

static long secsToNextAlert() {
  struct tm t;
  if (!prayerOk || !timeOk || !getLocalTime(&t, 0)) return -1;
  int nowMin = t.tm_hour * 60 + t.tm_min;
  long best = -1;
  for (int i = 0; i < 5; i++) {
    int at = prayerAt(i);
    if (at < 0) continue;
    // ten minutes before is the first thing it says
    int want = (at - 10 + 1440) % 1440;
    long d = (long)want - nowMin;
    if (d <= 0) d += 1440;                       // tomorrow, then
    if (best < 0 || d < best) best = d;
  }
  return best < 0 ? -1 : best * 60 - t.tm_sec;
}

// Switch off properly and wait to be lifted.
//
// The accelerometer keeps watch on its own while the processor is off:
// it is told to pull INT1 high the moment it feels movement, and that
// line is the only thing that can bring the processor back. A timer is
// set alongside it for the next prayer, so being switched off never
// means missing one.
// Everything back to how it arrived, except the things that would leave
// you unable to reach it afterwards. The networks stay, the pairing
// stays, and the shelf of reads stays, because none of those are
// settings and losing them is not what anyone means by reset.
static void resetSettings() {
  cfgBright = 160;   prefs.putInt("bri", cfgBright);   applyBright();
  cfgFace = F_CLASSIC; prefs.putInt("face", cfgFace);
  cfgSleepIdx = 1;   prefs.putInt("slpi", cfgSleepIdx);
  cfgPopupIdx = 2;   prefs.putInt("popi", cfgPopupIdx);
  cfgEyes = 0;       prefs.putInt("eye", cfgEyes);     applyEyes(cfgEyes);
  cfgAutoTurn = false; prefs.putBool("turn", cfgAutoTurn);
  cfgTap = TAP_MED;  prefs.putInt("tap", cfgTap);      applyTap();
  cfgAutoUp = false; prefs.putBool("autoup", cfgAutoUp);
  cfgKnock = false;  prefs.putBool("knock", cfgKnock);
  cfgHijriAdj = 0;   prefs.putInt("hadj", cfgHijriAdj);
  cfgOffline = false; prefs.putBool("offl", cfgOffline);
  cfgBike = false;   prefs.putBool("bike", cfgBike);
  cfgBikeTpl = 0;    prefs.putInt("btpl", cfgBikeTpl);
  bikeEdit = false;  bikeTry = 0;
  cfgBack = BACK_BOTH; prefs.putInt("back", cfgBack);
  cfgDeepIdx = 1;    prefs.putInt("deepi", cfgDeepIdx);
  battFull = 4.10f;  prefs.putFloat("bfull", battFull);
  deepOff = false;   prefs.putBool("nodeep", deepOff);
  for (int i = 0; i < 5; i++) prayerAdj[i] = 0;
  saveAdj();
  Serial.println("settings back to how they came");
}

// Said properly rather than just going dark, because a robot that
// vanishes mid sentence looks broken and one that says goodnight looks
// asleep. Three breaths out, then the eyes close.
static void sleepCard() {
  for (int f = 0; f < 22; f++) {
    oled.clearDisplay();
    int r = 14 - f / 2;
    if (r > 1) {
      oled.drawCircle(40, 34, r, SSD1306_WHITE);
      oled.drawCircle(88, 34, r, SSD1306_WHITE);
    } else {
      oled.drawFastHLine(40 - 12, 34, 24, SSD1306_WHITE);
      oled.drawFastHLine(88 - 12, 34, 24, SSD1306_WHITE);
    }
    ctr("going to sleep", 8, 1);
    // zzz drifting up from the right eye
    for (int z = 0; z < 3; z++) {
      int zy = 24 - z * 7 - (f / 3);
      if (zy > 10 && z * 7 < f) at(100 + z * 5, zy, "z");
    }
    ctr("touch to wake me", 54, 1);
    oled.display();
    delay(55);
  }
  oled.clearDisplay(); oled.display();
}

static void goDeep() {
  // The pad can wake it now, so this no longer needs INT1 soldered.
  // Until this version deep sleep simply never happened on a board
  // without that wire, which is every board this has ever run on.
  //
  // One catch worth knowing about: the wake call takes a single level
  // for every pin in the mask. INT1 is active high, so it can only come
  // along when the pad is active high too. On a pad wired the other way
  // round the pad wakes it and the accelerometer does not.
  // The pad, and only the pad. The accelerometer used to be able to
  // wake it and no longer is: a bag being carried, a desk being leaned
  // on and a door closing are all enough to trip it, and every one of
  // those cost a wake, a WiFi reconnect and a slice of the battery for
  // nothing. Waking should be something you did on purpose.
  bool wakeHigh = !touchRest;                  // the level that means touched
  if (deepOff) return;
  Serial.println("switching off until touched");
  sleepCard();


  screenPower(false);
  prefs.end();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);

  uint64_t mask = BIT(TOUCH_PIN);
  esp_deep_sleep_enable_gpio_wakeup(mask, wakeHigh ? ESP_GPIO_WAKEUP_GPIO_HIGH
                                                   : ESP_GPIO_WAKEUP_GPIO_LOW);
  // Whichever is sooner, a prayer or a reminder. Worked out here, from
  // the clock, so it holds with no network: the chip keeps counting
  // through deep sleep and comes back on its own at the right minute.
  long pray = secsToNextAlert();
  long rem  = secsToNextRem();
  long secs = -1;
  if (pray > 0 && rem > 0) secs = pray < rem ? pray : rem;
  else if (pray > 0)       secs = pray;
  else if (rem > 0)        secs = rem;
  if (secs > 0) {
    Serial.printf("next wake in %ld s (%s)\n", secs,
                  (rem > 0 && (pray <= 0 || rem <= pray)) ? "a reminder" : "a prayer");
    esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
  }
  esp_deep_sleep_start();
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
// Rafiq says who it is on every call, so the panel can tell the Mac apart
// from a browser tab left open on the page. Without that marker a tab
// polling once a second would look exactly like a laptop arriving.
static void sawMac() {
  if (web.header("X-Rafiq-App") != "1") return;
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
  // Asking for the hotspot is asking for the radio, so it says so and
  // turns the mode back rather than quietly contradicting a setting
  // you chose. There is no third state where the radio is both off and
  // serving an access point.
  if (cfgOffline) {
    cfgOffline = false;
    prefs.putBool("offl", cfgOffline);
    netDown = false; netMisses = 0; netNextTry = 0;
    flash("NETWORK BACK ON", 1100);
  }
  WiFi.mode(online() ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(RESCUE_SSID, RESCUE_PASS);
  rescueAP = true;
  Serial.printf("hotspot up: %s at %s\n", RESCUE_SSID, WiFi.softAPIP().toString().c_str());
}

static void knockOne() {
  cTap++;
  // Anything the Mac put on the screen goes away on one knock. It is
  // the Mac's idea of what you want to see, and this is the desk.
  if (dndUntil)    { dndUntil = 0;      return; }
  if (relaxOn)     { relaxOn = false;    return; }
  if (canvasUntil) { canvasUntil = 0;    return; }
  if (toastUntil)  {
    // Waved away. That is "not now" rather than "done", so it comes
    // back in a quarter of an hour, and it does not count against the
    // ladder: you may keep saying not now for as long as you like.
    //
    // And if this is what woke it up, it goes straight back. You were
    // asleep, it asked, you said later; there is nothing else here for
    // it to be awake for.
    if (remShowing >= 0 && remShowing < remCount && !rems[remShowing].done) {
      rems[remShowing].at = (uint32_t)time(nullptr) + REM_WAVED_MIN * 60UL;
      saveRems();
      Serial.println("reminder waved away, back in fifteen minutes");
    }
    bool back = remWokeIt;
    remShowing = -1; remWokeIt = false;
    toastUntil = 0; toastText = ""; toastKind = "";
    if (back) backToSleep();
    return;
  }
  if (depth == 0) {
    screen = nextScreen(screen);
    itemIdx = 0; subIdx = 0;
    return;
  }
  if (screen == S_FOCUS) {
    if (swOn) return;                          // it is just counting; leave it be
    if (depth == 1) { itemIdx = (itemIdx + 1) % focusRows(); return; }
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
  if (screen == S_SETTINGS && itemIdx == C_TAP && depth >= 2) {
    if (depth == 2) { subIdx = (subIdx + 1) % 2; return; }
    // Inside the test, a single knock IS the thing being tested. It is
    // counted where the interrupt is read and must do nothing here, or
    // trying one out would walk you through the menu at the same time.
    if (subIdx == 0 && tapChosen) return;
    tapPick = (tapPick + 1) % TAP_N;
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
  // Told to stand up and not able to just yet. Ten minutes and it asks
  // again, which is the difference between a reminder and a nag.
  if (toastUntil && (toastKind == "break" || toastKind == "remind")) {
    toastUntil = 0; toastKind = ""; toastText = "";
    flash("SNOOZED", 1200);
    return;
  }
  if (depth == 0) {
    switch (screen) {
      case S_FOCUS:
        if (swOn) { if (swRun) swStop(); else swGo(); break; }   // same as a press
        depth = 1; itemIdx = 0;
        break;
      case S_FAITH:    depth = 1; itemIdx = 0; subIdx = 0; break;
      case S_READS:    if (readCount) { depth = 1; itemIdx = 0; } else refillShelf(); break;
      case S_GAMES:    depth = 1; itemIdx = 0; navCalBegin(false); break;
      case S_SETTINGS: depth = 1; itemIdx = 0; break;
      case S_REMIND:   if (remCount) { depth = 1; remIdx = 0; remConfirm = false; } break;
      case S_HOME:
        // With no clock this screen is a stopwatch, and restarting it is
        // the only useful thing a double knock can mean there.
        if (!timeOk) { swZero(); break; }
        // The next face, kept as you go. Written every time rather
        // than on the way out of something, because there is no way
        // out of this: whatever is on the screen is what it will be
        // wearing next time it wakes up.
        cfgFace = (cfgFace + 1) % FACE_N;
        prefs.putInt("face", cfgFace);
        break;
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
    if (depth == 1) {
      // Knock and shake games have nothing to calibrate, and making a
      // small person hold it still, lean it right and lean it away
      // before a balloon appears is how you lose them.
      if (!gameUsesTilt(itemIdx)) { gameStart(itemIdx); depth = 2; return; }
      gamePending = itemIdx; navCalBegin(true); return;
    }
    if (gState == GS_OVER)  { gameStart(itemIdx); return; }
    if (gState == GS_PLAY)  { gState = GS_PAUSE;  return; }
    if (gState == GS_PAUSE) { gState = GS_PLAY; gNext = millis(); return; }
    return;
  }
  if (screen == S_FOCUS && depth == 1) {
    if (itemIdx == taskCount) {                 // the stopwatch
      // Opened, not started. A stopwatch that is already running by
      // the time you are looking at it has already lost you the bit
      // you wanted to measure.
      swOn = true; swRun = false; swZero(); depth = 0;
      return;
    }
    if (itemIdx < taskCount) {                  // run just this one
      startTask(itemIdx);
      depth = 0;
    }
    return;
  }
  if (screen == S_SETTINGS && itemIdx == C_RESET && depth == 2) {
    resetSettings();
    depth = 1;
    flash("RESET", 1200);
    return;
  }
  if (screen == S_SETTINGS && itemIdx == C_TAP && depth >= 2) {
    if (depth == 2) {
      depth = 3;
      tapPick = cfgTap;
      tapChosen = false;
      tapSeen = 0; tapLastSeen = millis();
      return;
    }
    if (subIdx == 0) {                       // trying one out
      // Only ever a way in. There is no knock that leaves this screen,
      // because a light tap bounces into two or three of them and one
      // of those was being read as the way out, which is what walked
      // you off the screen the instant you touched it. The window
      // closing is the way back, and nothing else is.
      if (!tapChosen) {
        tapChosen = true; tapTesting = true;
        tapSeen = 0; tapLastSeen = millis();
        tapTestEnds = millis() + TAP_TRY_MS;
        for (int i = 0; i < TAP_TRACE; i++) tapTrace[i] = 0;
        applyTapLevel(tapPick);
      }
      return;
    }
    if (subIdx == 1) {                       // choosing, so this commits
      cfgTap = tapPick;
      prefs.putInt("tap", cfgTap);
      applyTap();
      flash("SET", 900);
      depth = 2;
    }
    return;
  }
  if (screen == S_SETTINGS && depth == 1) {
    switch (itemIdx) {
      case C_REBOOT:  delay(150); ESP.restart(); break;
      case C_UPDATE:
        if (online()) { upState = U_MENU; upPick = 0; upMsg = ""; }
        else { upState = U_FAIL; upMsg = "No network"; }
        break;
      case C_AUTOUP:
        cfgAutoUp = !cfgAutoUp;
        prefs.putBool("autoup", cfgAutoUp);
        nextAutoUp = millis() + 30000;
        flash(cfgAutoUp ? "AUTO ON" : "AUTO OFF", 1000);
        break;
      case C_RESET:
        depth = 2;                       // it asks first
        break;
      case C_HOTSPOT: startHotspot(); break;
      case C_PRAYER:  prayerWanted = true; nextPrayerTry = 0; break;
      case C_ACCEL:   depth = 2; break;
      case C_MODE:
        cfgOffline = !cfgOffline;
        prefs.putBool("offl", cfgOffline);
        if (cfgOffline) {
          // Asked for, so it goes off and stays off. Nothing scans,
          // nothing retries, and the carousel loses the weather.
          WiFi.disconnect(true, false);
          WiFi.mode(WIFI_OFF);
          netDown = false;
          if (screen == S_WEATHER) screen = S_HOME;
          flash("NETWORK OFF", 1200);
        } else {
          netDown = false;
          netNextTry = 0;                   // the task picks it up at once
          WiFi.mode(WIFI_STA);
          WiFi.setSleep(false);
          setupWeb();                       // never started if it booted offline
          flash("LOOKING", 1200);
        }
        break;
      case C_BIKE:
        cfgBike = !cfgBike;
        prefs.putBool("bike", cfgBike);
        if (!cfgBike && screen == S_BIKE) screen = S_HOME;
        flash(cfgBike ? "VEHICLE ON" : "VEHICLE OFF", 1000);
        break;
      case C_HIJRI: {
        // The arithmetic calendar is a rule and the real one is a
        // sighting, so they disagree by a day now and then. Two either
        // way covers every disagreement worth having.
        cfgHijriAdj++;
        if (cfgHijriAdj > 2) cfgHijriAdj = -2;
        prefs.putInt("hadj", cfgHijriAdj);
        char m[10]; snprintf(m, sizeof(m), "%+d day", cfgHijriAdj);
        flash(m, 900);
        break; }
      case C_SHAKE:
        cfgBack = (cfgBack + 1) % BACK_N;
        prefs.putInt("back", cfgBack);
        flash(cfgBack == BACK_TOUCH ? "BACK BY TOUCH"
            : cfgBack == BACK_SHAKE ? "BACK BY SHAKE" : "BACK BY EITHER", 1000);
        break;
      case C_DEEP:
        cfgDeepIdx = (cfgDeepIdx + 1) % DEEP_N;
        prefs.putInt("deepi", cfgDeepIdx);
        flash(DEEP_NAME[cfgDeepIdx], 900);
        break;
      case C_BATT: {
        // Steps a tenth at a time round the range a single cell charges
        // to, so a pack that tops out at 4.10 can be told so.
        battFull += 0.05f;
        if (battFull > 4.251f) battFull = 3.95f;
        prefs.putFloat("bfull", battFull);
        char m[12]; snprintf(m, sizeof(m), "%.2fV", battFull);
        flash(m, 900);
        break; }
      case C_KNOCK:
        cfgKnock = !cfgKnock;
        prefs.putBool("knock", cfgKnock);
        flash(cfgKnock ? "KNOCKS ON" : "KNOCKS OFF", 1100);
        break;
      case C_PAIR:    depth = 2; newPairCode(); break;
      default:        depth = 2; break;
    }
  }
}

static void knockThree() {
  cTriple++;
  if (screen == S_FOCUS && (swOn || depth == 1)) {
    if (swOn) { swOn = false; swRun = false; depth = 0; return; }
    depth = 0; itemIdx = 0;
    return;
  }
  if (screen == S_SETTINGS && itemIdx == C_RESET && depth == 2) { depth = 1; return; }
  if (screen == S_SETTINGS && itemIdx == C_TAP && depth >= 2) {
    if (depth == 3) {
      // Never reached while a window is open: settleBurst eats every
      // knock in there. This is the way out of the list behind it.
      if (tapTesting) return;
      tapChosen = false;
      applyTap();
      depth = 2;
    } else depth = 1;
    return;
  }
  // Twenty faces is a long way round on double knocks alone, so three
  // knocks steps back one. Only on the clock, and only at the top
  // level, where three knocks already meant "go home" and going home
  // from home does nothing at all.
  if (screen == S_HOME && depth == 0 && timeOk) {
    cfgFace = (cfgFace + FACE_N - 1) % FACE_N;
    prefs.putInt("face", cfgFace);
    return;
  }
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
  // Ticking one off at the device, so the robot can change the list and
  // not only show it. Rafiq sees it on its next look.
  if (screen == S_FOCUS && depth == 1 && itemIdx < taskCount) {
    tasks[itemIdx].done = !tasks[itemIdx].done;
    saveTasks();
    flash(tasks[itemIdx].done ? "DONE" : "BACK ON", 900);
    return;
  }
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

// Picking Check update starts the check. There is no menu to get past
// first, and the check itself runs on the network task, so the panel
// keeps drawing while it waits instead of sitting there looking asleep.
static void updateKnock(uint8_t n) {
  switch (upState) {
    case U_MENU:
      if (n == 1) { upPick = (upPick + 1) % 2; return; }
      if (n == 2) {
        // All this does is ask. The network task picks the flag up and
        // sets the answer; the panel stays awake and answering the whole
        // time it takes, which is the whole point of the change.
        upMsg = "";
        if (upPick == 0) wantOtaLatest = true;
        else             wantOtaList = true;
        upState = U_LOOK;
        return;
      }
      upState = U_OFF;
      return;

    case U_LOOK:
      // Nothing to do but wait. Three knocks calls it off, and bumping
      // the sequence means a reply already on its way is ignored rather
      // than yanking you back in a few seconds later.
      if (n >= 3) {
        wantOtaLatest = false; wantOtaList = false;
        upSeq++;
        upState = U_MENU;
      }
      return;

    case U_ASK:
      if (n == 1) { upYes = !upYes; return; }
      if (n == 2) {
        if (upYes) { upState = U_OFF; otaInstall(); upState = U_FAIL; }  // returns only if it failed
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

    default:                                   // it said its piece
      upState = U_MENU;
      return;
  }
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

// A gesture off the pad. Knocking, when it is switched on, keeps every
// meaning it ever had; this is a second vocabulary beside it rather
// than a translation of it.
//
//   one    next          two   open        three  home        long  back
//
// Most of it is the knock handlers, because "next" and "open" are the
// same jobs whichever way you asked for them. What is not shared is the
// clock, where a long press opens the faces instead of going back,
// since there is nowhere back to go from the clock.
// Whatever is on the screen, pulled in and let go again. The panel
// buffer is a real thing we can read, so this scales the actual screen
// rather than drawing a rectangle over it and hoping.
//
// Four frames: in, in further, most of the way back, home. About two
// hundred milliseconds all in, which is long enough to see and short
// enough not to be in the way. It replaces the inverse blink, which
// said "something happened" without saying it felt like a press.
static void clickShrink() {
  uint8_t* buf = oled.getBuffer();
  if (!buf) return;
  static uint8_t snap[SCRW * SCRH / 8];
  memcpy(snap, buf, sizeof(snap));

  const uint8_t pct[4] = { 90, 82, 94, 100 };
  for (int s = 0; s < 4; s++) {
    if (pct[s] >= 100) { memcpy(buf, snap, sizeof(snap)); oled.display(); break; }
    int w = SCRW * pct[s] / 100, h = SCRH * pct[s] / 100;
    int ox = (SCRW - w) / 2, oy = (SCRH - h) / 2;
    oled.clearDisplay();
    for (int y = 0; y < h; y++) {
      int sy = y * SCRH / h;
      const uint8_t* row = &snap[(sy >> 3) * SCRW];
      uint8_t bit = 1 << (sy & 7);
      for (int x = 0; x < w; x++) {
        if (row[x * SCRW / w] & bit) oled.drawPixel(ox + x, oy + y, SSD1306_WHITE);
      }
    }
    oled.display();
    delay(26);
  }
}

// Is there anything a second press could mean from here? At the top of
// the carousel there is not: back goes home and a triple goes home, so
// waiting to find out costs a third of a second and buys nothing.
// Anywhere with somewhere to go back to, and in the places where a
// double turns a page, there is.
static bool doubleMeansSomething() {
  if (depth != 0) return true;                      // back one level
  if (faceMode || inReader()) return true;          // the page before
  if (screen == S_GAMES && gState == GS_PLAY) return true;
  if (dndUntil || relaxOn || canvasUntil || toastUntil) return true;
  return false;
}

// A card when one lands, saying how many and when the next is due.
static void remAddedCard(int n, uint32_t when) {
  if (n <= 0) return;
  wake("reminder added");
  char a[26], b[26];
  snprintf(a, sizeof(a), n == 1 ? "%d reminder added" : "%d reminders added", n);
  if (when) {
    time_t tt = (time_t)when;
    struct tm lt; localtime_r(&tt, &lt);
    strftime(b, sizeof(b), "first at %H:%M", &lt);
  } else {
    snprintf(b, sizeof(b), "%d waiting", remPending());
  }
  toastKind = "note";
  toastText = String(a) + "\n" + b;
  toastUntil = millis() + 3500;
  toastFlash = millis();
}

static void touchGesture(uint8_t g) {
  lastActive = millis();

  // A reminder on screen takes the press before anything else does.
  // Holding is how you say it is done, which is the only way a thing
  // ever stops asking.
  if (toastUntil && toastKind == "remind" && remShowing >= 0) {
    if (g == TG_LONG) {
      if (remShowing < remCount) { rems[remShowing].done = true; saveRems(); }
      bool back = remWokeIt;
      remShowing = -1; remWokeIt = false;
      toastUntil = 0; toastText = ""; toastKind = "";
      flash("DONE", 900);
      // Same again: if this is the only reason it is awake, finishing
      // it is the only reason it needed to be.
      if (back) backToSleep();
      return;
    }
    // anything else falls through, and a single press waves it away
  }
  Serial.printf("touch gesture %u (screen %s depth %d)\n", g, S_NAME[screen], depth);

  // Trying a tap strength is the one screen where knocking is the
  // thing being measured. The pad still works in there, so there is
  // always a way out of it.
  if (tapTesting && tapChosen && g == TG_LONG) { tapTesting = false; return; }

  // The vehicle screen picks its layout the way the clock picks its
  // time: hold to get into it, press to walk the six, hold again to
  // keep the one in front of you. Walking them used to happen on a
  // long press with nothing asked and nothing confirmed, so a layout
  // changed and stayed changed before you had decided anything.
  if (screen == S_BIKE && depth == 0) {
    if (!bikeEdit) {
      if (g == TG_LONG) { bikeEdit = true; bikeTry = cfgBikeTpl; clickShrink(); return; }
    } else {
      switch (g) {
        case TG_ONE:  bikeTry = (bikeTry + 1) % BIKE_TPL_N; return;
        case TG_LONG: cfgBikeTpl = bikeTry; prefs.putInt("btpl", cfgBikeTpl);
                      bikeEdit = false; flash("KEPT", 800); return;
        default:      bikeEdit = false; return;    // two, three: leave it alone
      }
    }
  }

  // Reminders read like a watch does: one goes to the next, two comes
  // back out, and holding is how you get in and how you clear them.
  if (screen == S_REMIND && depth > 0) {
    if (remConfirm) {
      if (g == TG_ONE) { remYes = !remYes; return; }
      if (g == TG_LONG) {
        if (remYes) { remCount = 0; remIdx = 0; saveRems(); flash("CLEARED", 1100); }
        remConfirm = false; depth = 0;
        return;
      }
      if (g == TG_TWO) { remConfirm = false; return; }
      return;
    }
    switch (g) {
      case TG_ONE:  if (remIdx < remCount) remIdx++; return;   // past the end is the offer
      case TG_TWO:  if (remIdx > 0) remIdx--; else depth = 0; return;
      case TG_LONG:
        if (remIdx >= remCount && remCount) { remConfirm = true; remYes = false; }
        else if (remIdx < remCount) {        // mark it read and move on
          rems[remIdx].done = true; saveRems(); remIdx++;
        }
        return;
      default: depth = 0; return;
    }
  }
  if (g == TG_LONG && screen == S_REMIND && depth == 0 && remCount) {
    depth = 1; remIdx = 0; remConfirm = false;
    clickShrink();
    return;
  }

  // The stopwatch, while it has the screen. One starts it and stops
  // it where it is, holding puts it back to zero, two goes back to
  // the list. A shake is handled with every other shake and leaves
  // altogether.
  if (screen == S_FOCUS && swOn) {
    switch (g) {
      case TG_ONE:  if (swRun) swStop(); else swGo(); return;
      case TG_LONG: swStop(); swZero(); flash("ZERO", 600); return;
      case TG_TWO:  swOn = false; swRun = false; depth = 1; itemIdx = taskCount; return;
      default:      swOn = false; swRun = false; depth = 0; itemIdx = 0; return;
    }
  }

  // An empty shelf has nothing to read and nothing to leave, so
  // holding writes one. Reloading used to be four knocks, which is a
  // gesture the pad does not have, so with knocks off there was no way
  // to ask for a story at all.
  if (g == TG_LONG && screen == S_READS && depth == 1 && !readCount && cfgKey.length()) {
    refillShelf();
    depth = readCount ? 2 : 1;
    return;
  }

  // A leaf: nothing here to open, so one goes on, two goes back a page
  // and a long press is the way out.
  if (inReader()) {
    switch (g) {
      case TG_ONE:   nextPage(); return;
      case TG_TWO:   prevPage(); return;
      case TG_LONG:  depth = 0; itemIdx = 0; subIdx = 0; return;
      default:       break;                 // three still goes home
    }
  }

  // Games played on the pad. The gesture is the move, not a command.
  if (screen == S_GAMES && depth == 2 && gState == GS_PLAY) {
    if (itemIdx == G_ECHO && !ecShow && g <= TG_THREE) { ecKnock(g); return; }
    if (itemIdx == G_BALLOON && !blBang && g <= TG_THREE) { blPuff(); return; }
    if (itemIdx == G_DIZZY && g <= TG_THREE) return;     // shaking is the game
  }

  // The faces are a leaf too: walking them is all there is to do.
  if (faceMode) {
    switch (g) {
      case TG_ONE:   cfgFace = (cfgFace + 1) % FACE_N;           prefs.putInt("face", cfgFace); break;
      case TG_TWO:   cfgFace = (cfgFace + FACE_N - 1) % FACE_N;  prefs.putInt("face", cfgFace); break;
      default:       faceMode = false; break;
    }
    return;
  }
  // The clock has nothing to open, so going in means the faces.
  if (g == TG_LONG && screen == S_HOME && depth == 0 && timeOk) {
    faceMode = true;
    clickShrink();
    return;
  }

  switch (g) {
    case TG_ONE:   knockOne();   break;
    // Back, out one level, unless you have said a shake is the only
    // way back. This is the generic one; the doubles that turn a page
    // inside a reader or walk back through the reminders are page
    // moves rather than commands and are left alone, so setting this
    // to the shake can never strand you inside something.
    case TG_TWO:   if (backByTouch()) knockThree(); break;
    case TG_LONG:  clickShrink(); knockTwo(); break;   // in
    case TG_THREE:                            // straight home from anywhere
      if (dndUntil || relaxOn || canvasUntil || toastUntil) { knockOne(); break; }
      screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0;
      break;
  }
}

static void settleBurst() {
  if (!burst) return;
  // Knocking is off unless you switch it on. The burst is still counted
  // up to here so that turning it on takes effect at once, and so the
  // counters on the vitals face keep telling the truth.
  if (!cfgKnock && !tapTesting) { burst = 0; return; }
  if (burst < 4 && millis() - burstStart < TAP_WINDOW_MS) return;   // four is all there is
  uint8_t n = burst;
  burst = 0;
  if (upState != U_OFF) {
    updateKnock(n);
    lastActive = millis();
    Serial.printf("knock x%u -> update state %d\n", n, upState);
    return;
  }
  // While a strength is being tried out, knocking IS the test, so no
  // knock is a command as well. Letting the clean double through was
  // the whole bug: at the light end one real tap lands as a double as
  // often as not, and the double was the way out, so touching the
  // screen left it. Everything is counted, nothing is obeyed, and the
  // window closing is what ends it.
  if (tapTesting && tapChosen) {
    Serial.printf("knock x%u counted, not obeyed (trying a strength)\n", n);
    return;
  }
  // Some games are played by knocking, and in those a burst is the move
  // and not a command. Taken here for the same reason as the line above:
  // once a burst has been turned into next, open or back, the count is
  // gone and the game never sees it.
  if (screen == S_GAMES && depth == 2 && gState == GS_PLAY) {
    // Echo and Balloon are played on the pad now, so a knock in there
    // is nothing at all rather than a second way to play: a stray knock
    // counting as part of an answer is how you lose a round you had.
    if (itemIdx == G_ECHO || itemIdx == G_BALLOON) { lastActive = millis(); return; }
    if (itemIdx == G_DIZZY) {
      // Shaking throws off knocks by the handful. Any of them being a
      // command would end the game the moment it got going, so none of
      // them is; the round has an end of its own.
      lastActive = millis();
      return;
    }
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

  // The screen goes inverse while the pad is held. That is the whole of
  // the test: nothing to find in a menu and nothing to remember.
  // invertDisplay is a panel command rather than anything we draw, so it
  // survives every redraw and costs one message, only on a change.
  {
    bool lvl = digitalRead(TOUCH_PIN);
    if (lvl != touchLvl) { touchLvl = lvl; touchLvlAt = now; }
    else if (lvl != touchRest && now - touchLvlAt > TOUCH_REST_MS) {
      // A whole minute at one level. Whatever it is, that is resting.
      touchRest = lvl;
      prefs.putBool("trest", touchRest);     // so the next wake starts right
      Serial.printf("pad resting level is now %s\n", touchRest ? "high" : "low");
    }

    // The pad settles before anything believes it, so a noisy edge is
    // not a press and a bounce on the way up is not a lift.
    bool want = (lvl != touchRest);
    if (want != touchOn) {
      if (!touchEdge) touchEdge = now;
      if (now - touchEdge >= TOUCH_DEBOUNCE) {
        touchOn = want; touchEdge = 0;
        lastActive = now;
        if (touchOn) {
          touchCount++;
          touchPressAt = now;
          touchLongDone = false;
          if (asleep) { wake("touch"); touchTaps = 0; touchLongDone = true; }
        } else {
          // Every lift is a jolt, so every lift is recorded, including
          // the one that ends a long press. Only the ones that were
          // not already spent on a long press count towards a tap.
          touchLiftAt = now;
          if (!touchLongDone) {
            if (touchTaps < 3) touchTaps++;
            if (touchTaps == 3) { touchGesture(TG_THREE); touchTaps = 0; }
          }
        }
      }
    } else touchEdge = 0;

    // Going in fires under your finger rather than after it, which is
    // what makes it feel like the quickest of the four. Holding on past
    // that is a different question, answered when you let go.
    if (touchOn && !touchLongDone && now - touchPressAt >= TOUCH_LONG_MS) {
      touchLongDone = true;
      touchTaps = 0;
      touchGesture(TG_LONG);
    }
    if (touchOn && touchLongDone) {
      uint32_t held = now - touchPressAt;
      touchHold = held >= TOUCH_SLEEP_MS ? 2 : held >= TOUCH_HOME_MS ? 1 : 0;
      // Past ten seconds it counts itself down and goes, held or not.
      // Waiting for a finger that is clearly not coming off is a way
      // of looking broken; the five seconds are there to be read and
      // to give you time to change your mind by letting go early.
      if (touchHold == 2 && held >= TOUCH_SLEEP_MS + TOUCH_COUNT_MS) {
        touchHold = 0; touchLongDone = true; touchTaps = 0;
        wantDeep = true;
      }
    } else if (!touchOn && touchHold) {
      uint8_t h = touchHold; touchHold = 0;
      if (h == 2) { wantDeep = true; }
      else { screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0; faceMode = false; }
    }
    // Quiet long enough that nothing more is coming.
    //
    // Except where nothing can. At the top of the carousel a double
    // means back, back from there means home, and a triple already
    // means home, so a second press buys nothing at all. With nothing
    // to wait for it does not wait, and flicking through the screens
    // lands the instant you lift. Everywhere a double does something,
    // the window still has to pass.
    if (!touchOn && touchTaps == 1 && !doubleMeansSomething()) {
      touchTaps = 0;
      touchGesture(TG_ONE);
    }
    else if (!touchOn && touchTaps && now - touchLiftAt >= TOUCH_GAP_MS) {
      uint8_t n = touchTaps; touchTaps = 0;
      touchGesture(n == 1 ? TG_ONE : TG_TWO);
    }
    if (touchOn) lastActive = now;             // a finger on it is not idle
  }

  // A knock that dismissed a card has already been acted on. Swallow the
  // rest of that burst so it does not also step the carousel.
  if (now < inputMuteUntil) {
    if (adxl) rReg(adxl, A_INT_SOURCE);
    burst = 0;
    lastActive = now;
    return;
  }

  if (amag < 0.60f) lastLowG = now;

  // While trying a strength out, keep a rolling picture of how hard the
  // last few shoves were. Every jolt goes in, registered or not, which
  // is the whole point: it shows the ones that fell short.
  if (tapTesting && tapChosen && now >= tapTraceNext) {
    tapTraceNext = now + 60;
    float j = fabsf(amag - 1.0f) * 255.0f / 1.2f;    // 1.2g reaches the top
    tapTrace[tapTraceAt] = (uint8_t)constrain((int)j, 0, 255);
    tapTraceAt = (tapTraceAt + 1) % TAP_TRACE;
  }

  if (adxl) {
    uint8_t s = rReg(adxl, A_INT_SOURCE);
    // The latch trips on any brief unloading, and a hand turning the
    // device over produces those constantly. Believe it only if the low
    // reading is one we saw ourselves, and never twice in a few seconds.
    if ((s & INT_FF) && lastLowG && now - lastLowG < 400 &&
        (!lastFallAt || now - lastFallAt > 4000)) {
      lastFallAt = now;
      wake("fall"); onFall(); return;
    }
    if (s & INT_TAP1) {
      if (tapTesting) { tapSeen++; tapLastSeen = now; }
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
    // One step back per shake, however long you keep shaking: the 600ms
    // above is what stops a good rattle counting as six. It never goes
    // past the clock, so shaking at an empty desk cannot do anything
    // except leave you at home.
    // Not from your own finger. Pressing a pad glued to a small light
    // robot shakes the small light robot, and at six tenths of a g a
    // firm press clears it easily. That is why holding to go into the
    // reminders looked like nothing happening: it went in, the press
    // registered as a shake, and the shake took it straight back out.
    //
    // So a shake counts only when nothing is touching the pad and
    // nothing has been for a moment.
    uint32_t sinceHand = now - (touchLiftAt > touchPressAt ? touchLiftAt : touchPressAt);
    bool byHand = touchOn || sinceHand < SHAKE_AFTER_MS;
    if (cfgShake && !byHand && !(screen == S_GAMES && depth == 2) && !tapTesting) {
      if (bikeEdit) bikeEdit = false;           // out of the chooser, nothing kept
      // The stopwatch holds the screen on its own, outside depth, so
      // stepping back has to put it down first or the shake does
      // nothing you can see.
      else if (swOn) { swOn = false; swRun = false; depth = 0; itemIdx = 0; }
      else if (faceMode) faceMode = false;
      else if (depth > 0) { depth--; if (!depth) { itemIdx = 0; subIdx = 0; } }
      else if (screen != S_HOME) { screen = S_HOME; itemIdx = 0; subIdx = 0; }
      Serial.println("shake -> back");
    }
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
      climbing and at <b>movement</b>: that number has to cross its
      threshold for the device to count you as still there.</div>
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
      <button class="g" onclick="if(confirm('Let go of the Mac? It will switch itself off shortly after.'))act('/api/bye')">Disconnect from the Mac</button>
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
               'Movement':s.dirD+' (needs 0.12)','Tilt, for the games':s.tilt});
  rows('wx',{'City':s.city,'Temperature':s.temp,'Humidity':s.hum,'Wind':s.wind,'Conditions':s.cond});
  rows('pr',s.prayer);
  if(!adjFilled){
    $('adj').innerHTML=Object.keys(s.prayer).map((n,i)=>
      '<div><label>'+n+'</label><input id="a'+i+'" type="number" min="-90" max="90" value="'+s.adj[i]+'"></div>').join('');
    adjFilled=true;
  }
  $('keyState').textContent=s.hasKey?('key saved · '+s.storyState):'no key yet';
  rows('sys',{'Signal':s.rssi,'Address':s.ip,'Hotspot':s.ap,'Deep sleep':(s.intWired?(s.deepOff?'off in settings':'ready'):'INT1 not wired'),'Free ram':s.heap+' B','OTA room':s.ota,
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
  o += "\"clockSrc\":\"" + String(clockSrc) + "\",";
  o += "\"asleep\":" + String(asleep ? "true" : "false") + ",\"fw\":\"" FW_VERSION "\",";
  o += "\"knock\":" + String(cfgKnock ? "true" : "false") + ",";
  o += "\"shake\":" + String(cfgShake ? "true" : "false") + ",";
  o += "\"back\":" + String(cfgBack) + ",";
  o += "\"backName\":\"" + String(BACK_NAME[cfgBack]) + "\",";
  o += "\"hadj\":" + String(cfgHijriAdj) + ",";
  o += "\"offline\":" + String(cfgOffline ? "true" : "false") + ",";
  o += "\"netDown\":" + String(netDown ? "true" : "false") + ",";
  o += "\"bike\":" + String(cfgBike ? "true" : "false") + ",";
  o += "\"btpl\":" + String(cfgBikeTpl) + ",";
  o += "\"plate\":\"" + String(bikePlate) + "\",";
  o += "\"make\":\"" + String(bikeMake) + "\",";
  o += "\"model\":\"" + String(bikeModel) + "\",";
  o += "\"owner\":\"" + String(bikeOwner) + "\",";
  o += "\"name\":\"" + String(cfgName) + "\",";
  { int hy, hm, hd;
    if (hijriNow(hy, hm, hd)) {
      o += "\"hijri\":\"" + String(hd) + " " + HIJRI_LATIN[hm - 1] + " " + String(hy) + "\","; } }
  o += "\"deepi\":" + String(cfgDeepIdx) + ",";
  o += "\"battFull\":" + String(battFull, 2) + ",";
  o += "\"battV\":" + String(isnan(battV) ? 0.0f : battV, 2) + ",";
  o += "\"battPct\":" + String(isnan(battV) ? -1 : battPct(battV)) + ",";
  o += "\"touches\":" + String((unsigned long)touchCount) + ",";
  o += "\"k1\":" + String(cTap) + ",\"k2\":" + String(cDouble) + ",\"k3\":" + String(cTriple) +
       ",\"k4\":" + String(cQuad) + ",\"fall\":" + String(cFall) + ",\"boots\":" + String(cBoot) + ",";
  o += "\"autoTurn\":" + String(cfgAutoTurn ? "true" : "false") + ",";
  o += "\"city\":\"" + String(wCity) + "\",";
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
         ",\"brk\":" + String(isBreak(tasks[i].name) ? "true" : "false") +
         ",\"done\":" + String(tasks[i].done ? "true" : "false") + "}";
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
  o += "\"tap\":" + String(cfgTap) + ",";
  o += "\"swOn\":" + String(swOn ? "true" : "false") + ",";
  o += "\"intWired\":" + String(intWired ? "true" : "false") + ",";
  o += "\"nets\":[";
  for (int i = 0; i < netCount; i++) {
    String nm = String(netSsid[i]); nm.replace("\\", " "); nm.replace("\"", "'");
    o += "{\"ssid\":\"" + nm + "\",\"on\":" + String(i == netUsing ? "true" : "false") + "}";
    if (i < netCount - 1) o += ",";
  }
  o += "],";
  o += "\"netMax\":" + String(NET_MAX) + ",";
  o += "\"bri\":" + String(cfgBright) + ",\"face\":" + String(cfgFace) +
       ",\"slpi\":" + String(cfgSleepIdx) + ",\"popi\":" + String(cfgPopupIdx) +
       ",\"eye\":" + String(cfgEyes) + ",\"turn\":" + String(cfgAutoTurn ? "true" : "false") + ",";
  o += "\"deepOff\":" + String(deepOff ? "true" : "false") + ",";
  o += "\"autoUp\":" + String(cfgAutoUp ? "true" : "false") + ",";
  o += "\"tapName\":\"" + String(TAP_NAME[cfgTap]) + "\",";
  o += "\"relax\":" + String(relaxOn ? "true" : "false") + ",";
  o += "\"webui\":" + String(webUiOn ? "true" : "false") + ",";
  o += "\"dndLeft\":" + String(dndUntil && millis() < dndUntil
          ? (long)((dndUntil - millis()) / 1000UL) : 0L) + ",";
  o += "\"cam\":" + String(busyCam ? "true" : "false") + ",";
  o += "\"mic\":" + String(busyMic ? "true" : "false") + ",";
  o += "\"focusLeft\":" + String(sessionRunning()
          ? (long)((taskEnd - millis()) / 1000UL) : 0L) + ",";
  o += "\"ssid\":\"" + cfgSsid + "\",\"tz\":\"" + cfgTz + "\"}";
  web.send(200, "application/json", o);
}

static void setupWeb() {
  if (webUp) return;
  webUp = true;
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
  // Said on the panel, not just in the log. One arriving is the only
  // sign you get that a link you pasted did anything, so it is worth
  // a second of screen even if the robot was asleep.
  //
  // (Declared here because the handlers below are lambdas and cannot
  // see anything declared after them.)

  // ---- one reminder, from anywhere ----
  //
  //   POST or GET  /api/remind
  //     text  what to be reminded of          (required)
  //     at    "HH:MM", 24 hour                (optional)
  //     in    minutes from now                (optional)
  //     d     "YYYY-MM-DD"                    (optional, today)
  //     t     the pairing token               (when the lock is on)
  //
  //  GET as well as POST, and the token as a parameter, so the whole
  //  thing fits in an address bar. Somebody wanting to be reminded in
  //  forty minutes should be able to type it, not compose a request.
  //
  //  Deliberately not "t" for the text: authed() already reads "t" as
  //  the pairing token, so a reminder sent that way would be checked
  //  as a password, fail, and come back 401 with nothing to say why.
  //
  //  No date means today. No time at all means three times across the
  //  day, at nine, noon and six, and only on that day: a thing with no
  //  hour attached is a thing for today rather than a thing for a
  //  moment. Slots already past are skipped, and if all of them are, it
  //  is due now, because something you added at seven in the evening
  //  with no time on it still wants saying this evening.
  web.on("/api/remind", HTTP_ANY, []() {
    if (!guard()) return;
    String txt = web.hasArg("text") ? web.arg("text") : web.arg("m");
    txt.trim();
    if (!txt.length()) {
      web.send(400, "application/json", "{\"ok\":false,\"err\":\"text is required\"}");
      return;
    }
    struct tm nowT;
    if (!timeOk || !getLocalTime(&nowT, 0)) {
      web.send(409, "application/json", "{\"ok\":false,\"err\":\"the clock is not set yet\"}");
      return;
    }
    String ds = web.hasArg("d") ? web.arg("d") : web.arg("date");
    String ts = web.hasArg("at") ? web.arg("at") : web.arg("time");

    int Y = nowT.tm_year + 1900, M = nowT.tm_mon + 1, D = nowT.tm_mday;
    if (ds.length() >= 10 && ds.indexOf('-') == 4) {
      Y = ds.substring(0, 4).toInt();
      M = ds.substring(5, 7).toInt();
      D = ds.substring(8, 10).toInt();
      if (Y < 2024 || M < 1 || M > 12 || D < 1 || D > 31) {
        web.send(400, "application/json", "{\"ok\":false,\"err\":\"d must be YYYY-MM-DD\"}");
        return;
      }
    }
    auto stamp = [&](int hh, int mm) -> uint32_t {
      struct tm w = {};
      w.tm_year = Y - 1900; w.tm_mon = M - 1; w.tm_mday = D;
      w.tm_hour = hh; w.tm_min = mm; w.tm_isdst = -1;
      return (uint32_t)mktime(&w);
    };

    int added = 0;
    uint32_t firstAt = 0;

    // "in" wins where both are given, because it is the more specific
    // thing to have asked for.
    String rel = web.hasArg("in") ? web.arg("in") : "";
    if (rel.length()) {
      long mins = rel.toInt();
      if (mins < 0 || mins > 60 * 24 * 30) {
        web.send(400, "application/json",
                 "{\"ok\":false,\"err\":\"in must be minutes, up to a month\"}");
        return;
      }
      firstAt = (uint32_t)time(nullptr) + (uint32_t)mins * 60UL;
      if (addRem(txt.c_str(), firstAt)) added = 1;
      if (!added) {
        web.send(507, "application/json", "{\"ok\":false,\"err\":\"no room, clear some first\"}");
        return;
      }
      sortRems(); saveRems();
      char o[120];
      snprintf(o, sizeof(o), "{\"ok\":true,\"added\":1,\"at\":%lu,\"waiting\":%d}",
               (unsigned long)firstAt, remPending());
      web.send(200, "application/json", o);
      Serial.printf("reminder in %ld min: %s\n", mins, txt.c_str());
      remAddedCard(1, firstAt);
      return;
    }

    int colon = ts.indexOf(':');
    if (colon > 0) {
      int hh = ts.substring(0, colon).toInt();
      int mm = ts.substring(colon + 1).toInt();
      if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
        web.send(400, "application/json", "{\"ok\":false,\"err\":\"at must be HH:MM\"}");
        return;
      }
      firstAt = stamp(hh, mm);
      if (addRem(txt.c_str(), firstAt)) added = 1;
    } else {
      const int H[3] = { 9, 12, 18 };
      uint32_t tnow = (uint32_t)time(nullptr);
      for (int i = 0; i < 3; i++) {
        uint32_t w = stamp(H[i], 0);
        if (w + 60 < tnow) continue;             // that hour has gone
        if (!firstAt) firstAt = w;
        if (addRem(txt.c_str(), w)) added++;
      }
      if (!added) {                              // the whole day has gone
        firstAt = tnow;
        if (addRem(txt.c_str(), firstAt)) added = 1;
      }
    }
    if (!added) {
      web.send(507, "application/json", "{\"ok\":false,\"err\":\"no room, clear some first\"}");
      return;
    }
    sortRems();
    saveRems();
    char o[120];
    snprintf(o, sizeof(o),
             "{\"ok\":true,\"added\":%d,\"at\":%lu,\"waiting\":%d}",
             added, (unsigned long)firstAt, remPending());
    web.send(200, "application/json", o);
    Serial.printf("reminder added: %s (%d)\n", txt.c_str(), added);
    remAddedCard(added, firstAt);
  });

  // Reminders arriving in bulk, from Rafiq or from anything else.
  //
  //  POST /api/rems   n=3  t0=..&a0=..  t1=..&a1=..  t2=..&a2=..
  //    tN  the words        aN  unix seconds        dN  1 if done
  //
  //  This used to empty the list and refill it from whatever arrived,
  //  on the assumption that Rafiq owned the reminders and the robot
  //  merely displayed them. That was wrong and it lost things: anything
  //  added through /api/remind vanished the next time the Mac saved,
  //  which it does whenever it marks one done. You would get ok:true
  //  and then find nothing on the robot.
  //
  //  The robot owns the list now. This adds what it does not already
  //  have, matched on the words and the minute, and never removes
  //  anything. Clearing is a thing you do deliberately, on the robot or
  //  with clear=1 below.
  // What is on the vehicle screen. Strings rather than numbers, so it
  // needs its own way in rather than riding on cfgv.
  web.on("/api/bike", HTTP_ANY, []() {
    if (!guard()) return;
    bool any = false;
    struct { const char* k; char* dst; size_t n; const char* pref; } F[] = {
      { "plate", bikePlate, sizeof(bikePlate), "bplate" },
      { "make",  bikeMake,  sizeof(bikeMake),  "bmake"  },
      { "model", bikeModel, sizeof(bikeModel), "bmodel" },
      { "owner", bikeOwner, sizeof(bikeOwner), "bowner" },
      { "name",  cfgName,   sizeof(cfgName),   "name"   },
    };
    for (auto& f : F) {
      if (!web.hasArg(f.k)) continue;
      String s = web.arg(f.k); s.trim();
      if (!s.length()) continue;
      snprintf(f.dst, f.n, "%s", s.c_str());
      prefs.putString(f.pref, f.dst);
      any = true;
    }
    // The template is a number rather than a string, so it is read
    // here instead of in the table above.
    if (web.hasArg("tpl")) {
      cfgBikeTpl = constrain((int)web.arg("tpl").toInt(), 0, BIKE_TPL_N - 1);
      prefs.putInt("btpl", cfgBikeTpl);
      bikeEdit = false;
      any = true;
    }
    if (web.hasArg("on")) {
      cfgBike = web.arg("on") == "1" || web.arg("on") == "true";
      prefs.putBool("bike", cfgBike);
      if (!cfgBike && screen == S_BIKE) { screen = S_HOME; bikeEdit = false; }
      any = true;
    }
    char o[240];
    snprintf(o, sizeof(o),
             "{\"ok\":true,\"changed\":%s,\"on\":%s,\"tpl\":%d,\"tpls\":%d,"
             "\"plate\":\"%s\",\"make\":\"%s\",\"model\":\"%s\",\"owner\":\"%s\",\"name\":\"%s\"}",
             any ? "true" : "false", cfgBike ? "true" : "false", cfgBikeTpl, BIKE_TPL_N,
             bikePlate, bikeMake, bikeModel, bikeOwner, cfgName);
    web.send(200, "application/json", o);
  });

  // What the robot is actually holding, ids and all, so the app can
  // show the real list rather than only the part of it the app itself
  // sent. Reminders added by a plain URL show up here too.
  web.on("/api/rem", HTTP_ANY, []() {
    if (!guard()) return;

    if (web.arg("list") == "1" || !web.args() ||
        (!web.hasArg("id") && !web.hasArg("drop"))) {
      String o = "{\"ok\":true,\"waiting\":" + String(remPending()) +
                 ",\"clock\":" + String(timeOk ? "true" : "false") +
                 ",\"rems\":[";
      for (int i = 0; i < remCount; i++) {
        if (i) o += ',';
        o += "{\"id\":" + String(rems[i].id) +
             ",\"at\":" + String(rems[i].at) +
             ",\"first\":" + String(rems[i].first) +
             ",\"tries\":" + String(rems[i].tries) +
             ",\"done\":" + String(rems[i].done ? "true" : "false") +
             ",\"text\":" + jstr(rems[i].text) + "}";
      }
      o += "]}";
      web.send(200, "application/json", o);
      return;
    }

    uint32_t id = (uint32_t)strtoul(web.arg("id").c_str(), nullptr, 10);
    int i = remById(id);
    if (i < 0) { web.send(404, "application/json", "{\"ok\":false,\"why\":\"no such id\"}"); return; }

    if (web.arg("drop") == "1") {
      for (int k = i; k < remCount - 1; k++) rems[k] = rems[k + 1];
      remCount--;
      if (remIdx > remCount) remIdx = remCount;
      if (remShowing == i) { remShowing = -1; toastUntil = 0; }
      else if (remShowing > i) remShowing--;
      saveRems();
      web.send(200, "application/json",
               "{\"ok\":true,\"dropped\":" + String(id) +
               ",\"waiting\":" + String(remPending()) + "}");
      return;
    }

    if (web.hasArg("text")) {
      String t = web.arg("text"); t.trim();
      if (t.length()) {
        snprintf(rems[i].text, REM_TEXT, "%s", t.c_str());
        for (char* c = rems[i].text; *c; c++)
          if (*c == '\x1e' || *c == '\x1f' || *c == '\n' || *c == '\r') *c = ' ';
      }
    }
    if (web.hasArg("at")) {
      uint32_t at = (uint32_t)strtoul(web.arg("at").c_str(), nullptr, 10);
      if (at) {
        // A new time is a fresh start: the ladder it had climbed was
        // for the old one, and keeping the tries would have it give up
        // after one nudge at a time you have only just set.
        rems[i].at = at;
        rems[i].first = at;
        rems[i].tries = 0;
        rems[i].done = false;
      }
    }
    if (web.hasArg("done")) rems[i].done = web.arg("done") == "1";
    sortRems();
    saveRems();
    web.send(200, "application/json",
             "{\"ok\":true,\"id\":" + String(id) +
             ",\"waiting\":" + String(remPending()) + "}");
  });

  web.on("/api/rems", HTTP_ANY, []() {
    if (!guard()) return;
    if (web.arg("clear") == "1") {
      remCount = 0; remIdx = 0; saveRems();
      web.send(200, "application/json", "{\"ok\":true,\"waiting\":0}");
      return;
    }
    int n = web.arg("n").toInt();
    if (n < 0) n = 0;
    if (n > 64) n = 64;                      // more than the list can hold, on purpose
    int added = 0, already = 0, full = 0;
    for (int i = 0; i < n; i++) {
      String kt = "t" + String(i), ka = "a" + String(i), kd = "d" + String(i);
      if (!web.hasArg(ka)) continue;
      uint32_t at = (uint32_t)strtoul(web.arg(ka).c_str(), nullptr, 10);
      if (!at) continue;
      String txt = web.arg(kt); txt.trim();
      if (!txt.length()) continue;
      // Same words, same minute: the same reminder arriving twice.
      bool dup = false;
      for (int k = 0; k < remCount; k++)
        if (rems[k].at / 60 == at / 60 && txt == rems[k].text) { dup = true; break; }
      if (dup) { already++; continue; }
      if (!addRem(txt.c_str(), at)) { full++; continue; }
      if (web.arg(kd) == "1") rems[remCount - 1].done = true;
      added++;
    }
    if (added) { sortRems(); saveRems(); }
    if (remIdx > remCount) remIdx = remCount;
    char o[120];
    snprintf(o, sizeof(o),
             "{\"ok\":true,\"added\":%d,\"already\":%d,\"full\":%d,\"waiting\":%d}",
             added, already, full, remPending());
    web.send(200, "application/json", o);
    Serial.printf("%d reminders in, %d new, %d already here, %d no room\n",
                  n, added, already, full);
    if (added) {
      uint32_t soonest = 0;
      for (int k = 0; k < remCount; k++)
        if (!rems[k].done && (!soonest || rems[k].at < soonest)) soonest = rems[k].at;
      remAddedCard(added, soonest);
    }
  });

  web.on("/api/toast", HTTP_POST, []() {
    if (!guard()) return;
    String m = web.arg("m"); m.trim();
    toastKind = web.arg("k");
    toastText = m.substring(0, 84);
    int secs = web.arg("s").toInt(); if (secs <= 0) secs = 4;
    toastUntil = millis() + (unsigned long)constrain(secs, 1, 300) * 1000UL;
    toastFlash = millis();
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
  // On a break. The Mac locks itself; this holds the sign.
  web.on("/api/dnd", HTTP_POST, []() {
    if (!guard()) return;
    int m = constrain((int)web.arg("m").toInt(), 0, 480);
    dndUntil = m ? millis() + (unsigned long)m * 60000UL : 0;
    dndLine = (int)random(DND_N);
    if (m) wake("break");
    okJson();
  });
  // One flag per device, read from the system by the Mac. Nothing is
  // heard or seen here, and nothing is recorded anywhere.
  web.on("/api/busy", HTTP_POST, []() {
    if (!guard()) return;
    bool c = web.arg("cam").toInt() != 0, m = web.arg("mic").toInt() != 0;
    if ((c || m) && !(busyCam || busyMic)) { busyAt = millis(); wake("live"); }
    busyCam = c; busyMic = m;
    okJson();
  });
  // How hard a knock has to be. Set from the app or the page, so a
  // strength too heavy to knock through is never a device you have lost.
  web.on("/api/tap", HTTP_POST, []() {
    if (!guard()) return;
    cfgTap = constrain((int)web.arg("n").toInt(), 0, TAP_N - 1);
    prefs.putInt("tap", cfgTap);
    applyTap();
    okJson();
  });
  // Switching off altogether, and the way to stop it doing so.
  // Adding, removing and reordering the networks it knows.
  //
  // Passwords only ever travel in this direction. Nothing here or in the
  // state report ever sends one back out, so a paired app can set one
  // and still cannot read the others.
  web.on("/api/net", HTTP_POST, []() {
    if (!guard()) return;
    if (web.hasArg("del")) {
      int d = web.arg("del").toInt();
      if (d >= 0 && d < netCount) {
        for (int i = d; i < netCount - 1; i++) {
          strncpy(netSsid[i], netSsid[i + 1], 33);
          strncpy(netPass[i], netPass[i + 1], 65);
        }
        netCount--;
        netSsid[netCount][0] = netPass[netCount][0] = 0;
        saveNets();
        netReload = true;
      }
    } else if (web.hasArg("up")) {           // move one nearer the front
      int u = web.arg("up").toInt();
      if (u > 0 && u < netCount) {
        char ts[33], tp[65];
        strncpy(ts, netSsid[u], 33); strncpy(tp, netPass[u], 65);
        strncpy(netSsid[u], netSsid[u - 1], 33); strncpy(netPass[u], netPass[u - 1], 65);
        strncpy(netSsid[u - 1], ts, 33); strncpy(netPass[u - 1], tp, 65);
        saveNets();
        netReload = true;
      }
    } else {
      String ss = web.arg("ssid"); ss.trim();
      if (!ss.length()) { web.send(400, "application/json", "{\"ok\":false}"); return; }
      // a name already on the list is an edit, not another copy of it
      int at = -1;
      for (int i = 0; i < netCount; i++) if (ss == netSsid[i]) { at = i; break; }
      if (at < 0) {
        if (netCount >= NET_MAX) {
          web.send(409, "application/json", "{\"ok\":false,\"err\":\"full\"}");
          return;
        }
        at = netCount++;
      }
      strncpy(netSsid[at], ss.c_str(), 32); netSsid[at][32] = 0;
      if (web.hasArg("pass")) {
        strncpy(netPass[at], web.arg("pass").c_str(), 64); netPass[at][64] = 0;
      }
      saveNets();
      netReload = true;
    }
    okJson();
  });
  // One way in for the plain numbered settings, rather than an endpoint
  // each. Every one is clamped to its own range here, so a value typed
  // wrong somewhere else cannot put the device into a state it has no
  // screen for.
  web.on("/api/cfgv", HTTP_POST, []() {
    if (!guard()) return;
    String k = web.arg("k");
    int v = web.arg("v").toInt();
    if      (k == "bri")  { cfgBright   = constrain(v, 0, 255);           prefs.putInt("bri", cfgBright);   applyBright(); }
    else if (k == "face") { cfgFace     = constrain(v, 0, FACE_N - 1);    prefs.putInt("face", cfgFace); }
    else if (k == "slpi") { cfgSleepIdx = constrain(v, 0, SLEEP_N - 1);   prefs.putInt("slpi", cfgSleepIdx); }
    else if (k == "popi") { cfgPopupIdx = constrain(v, 0, POPUP_N - 1);   prefs.putInt("popi", cfgPopupIdx); }
    else if (k == "eye")  { cfgEyes     = constrain(v, 0, STYLE_N - 1);   prefs.putInt("eye", cfgEyes); applyEyes(cfgEyes); }
    // Reachable from the app on purpose. The pad is the only thing
    // driving this now, so if the pad ever stops there has to be a way
    // back in that does not involve the pad, and the network is it.
    else if (k == "knock"){ cfgKnock    = (v != 0);                      prefs.putBool("knock", cfgKnock); }
    else if (k == "bike") { cfgBike     = (v != 0);                      prefs.putBool("bike", cfgBike); }
    else if (k == "btpl") { cfgBikeTpl  = constrain(v, 0, BIKE_TPL_N - 1); prefs.putInt("btpl", cfgBikeTpl); }
    else if (k == "offl") { cfgOffline  = (v != 0);                      prefs.putBool("offl", cfgOffline);
                            if (cfgOffline) { WiFi.disconnect(true, false); WiFi.mode(WIFI_OFF); }
                            else { netDown = false; netMisses = 0; netNextTry = 0;
                                   WiFi.mode(WIFI_STA); WiFi.setSleep(false); setupWeb(); } }
    else if (k == "hadj") { cfgHijriAdj = constrain(v, -2, 2);           prefs.putInt("hadj", cfgHijriAdj); }
    else if (k == "shake"){ cfgBack = v ? BACK_BOTH : BACK_TOUCH;        prefs.putInt("back", cfgBack); }
    else if (k == "back") { cfgBack = constrain(v, 0, BACK_N - 1);       prefs.putInt("back", cfgBack); }
    else if (k == "deepi"){ cfgDeepIdx  = constrain(v, 0, DEEP_N - 1);   prefs.putInt("deepi", cfgDeepIdx); }
    // Sent in hundredths, because the form only carries whole numbers.
    else if (k == "bfull"){ battFull    = constrain(v / 100.0f, 3.90f, 4.30f); prefs.putFloat("bfull", battFull); }
    else { web.send(400, "application/json", "{\"ok\":false,\"err\":\"no such setting\"}"); return; }
    okJson();
  });
  web.on("/api/autoup", HTTP_POST, []() {
    if (!guard()) return;
    cfgAutoUp = web.arg("a").toInt() != 0;
    prefs.putBool("autoup", cfgAutoUp);
    nextAutoUp = millis() + 30000;
    okJson();
  });
  web.on("/api/reset", HTTP_POST, []() {
    if (!guard()) return;
    resetSettings();
    okJson();
  });
  web.on("/api/deep", HTTP_POST, []() {
    if (!guard()) return;
    deepOff = web.arg("off").toInt() != 0;
    prefs.putBool("nodeep", deepOff);
    okJson();
  });
  // Rafiq saying goodbye. Without this the robot waits out the full
  // link timeout before it believes you have gone, which is most of a
  // minute of standing there wondering.
  web.on("/api/bye", HTTP_POST, []() {
    if (!guard()) return;
    macLinked = false;
    macSeen = 0;
    linkCardJoin = false;
    linkCardUntil = millis() + 1400;
    // The countdown to switching off starts here. With no Mac there is
    // nothing to stay reachable for, so when it runs out it powers
    // down properly rather than just darkening the screen.
    sleptAt = millis();
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
    } else if (web.hasArg("done")) {
      int d = web.arg("done").toInt();
      if (d >= 0 && d < taskCount) {
        tasks[d].done = web.arg("v").toInt() != 0;
        saveTasks();
      }
    } else {
      String n = web.arg("name"); n.trim();
      int m = constrain((int)web.arg("mins").toInt(), 1, 240);
      if (n.length() && taskCount < TASK_MAX) {
        tasks[taskCount].name = n.substring(0, 40);
        tasks[taskCount].mins = m;
        tasks[taskCount].done = false;
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
    // Wake the panel first. Asked from the Mac while the screen was
    // off, this used to fetch and flash a whole firmware in the dark
    // and come back on afterwards as though nothing had happened,
    // which is an alarming way to find out your robot has restarted.
    wake("update");
    delay(200); runUpdate();
  });
  web.on("/api/reboot", HTTP_POST, []() {
    if (!guard()) return;
    web.send(200, "application/json", "{\"ok\":true}");
    delay(300); ESP.restart();
  });
  {
    const char* keep[] = { "X-Rafiq-Token", "X-Rafiq-App" };
    web.collectHeaders(keep, 2);
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
  // Smaller than it was, with the name of whoever built it underneath.
  // Two sizes down leaves room to say something that is not just the
  // product shouting at you.
  oled.setTextSize(2);
  oled.setCursor((SCRW - 5 * 12) / 2, 16);
  oled.print("RAFIQ");
  oled.drawFastHLine(30, 36, SCRW - 60, SSD1306_WHITE);
  ctr("your companion", 41, 1);
  ctr("developed by Ahmed", 54, 1);
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
  ctr("and hold on", 32, 1);
  ctr("Hotspot", 44, 1);
  oled.drawFastHLine(30, 56, SCRW - 60, SSD1306_WHITE);
  oled.display();
  holdCard(2600);
}

// ================================================================
//  SETUP
// ================================================================
// ================================================================
//  THE NETWORK TASK
// ================================================================
//  Fetching blocks for seconds at a time, and it used to do that on the
//  very loop that reads your knocks and draws the screen. Worse, it was
//  held back until you had been still for a couple of seconds, so the
//  usual way to meet it was to come back to the clock, stop touching the
//  thing, and find it dead to the touch. The timeouts are set twice over,
//  once to connect and once to read, so a single weather fetch could hold
//  the whole device for the better part of half a minute.
//
//  This chip has one core, but FreeRTOS still preempts, so the fetching
//  happens here instead, underneath the loop. The loop is raised above it
//  and yields on every pass, which is what keeps knocks and redraws
//  answering while something is downloading.
//
//  Nothing is shared but a few numbers and three flags. The loop only
//  ever sets a flag and the task only ever clears it, so there is nothing
//  for the two of them to disagree about.
static void netLoop(void*) {
  for (;;) {
    if (netReload) {                    // the app changed the list
      netReload = false;
      loadNets();
      netNextTry = 0;
    }
    if (cfgOffline) {
      // Asked to stay off. Nothing here runs at all.
      vTaskDelay(pdMS_TO_TICKS(400));
      continue;
    }
    if (!online()) {
      wantTime = wantWx = wantPrayerNow = false;
      netUsing = -1;
      // Try each saved network once. A chip scanning for something
      // that is not coming back is a way of spending the battery, so
      // after one pass round the list it stops and says so, and the
      // robot carries on offline. The next wake tries again.
      if (netCount && !rescueAP && !netDown && (long)(millis() - netNextTry) >= 0) {
        netTrying = (netTrying + 1) % netCount;
        if (!joinOne(netTrying, 7000)) {
          netNextTry = millis() + 4000;
          // Only on the way up. Once it has been online this session
          // a dropout is a dropout, not a change of mode: the Mac may
          // be back in a second and tearing the radio down would make
          // that worse. It decides again when it next wakes.
          if (++netMisses >= netCount * 2 && !hadNet) {
            netDown = true;
            WiFi.disconnect(true, false);
            WiFi.mode(WIFI_OFF);
            Serial.println("nothing to join; going offline and switching the radio off");
          }
        }
      }
    } else {
      netMisses = 0;
      hadNet = true;
      if (wantTime)      { wantTime = false;      trySyncTime(1500); }
      if (wantWx)        { wantWx = false;        fetchWeather(); }
      if (wantPrayerNow) { wantPrayerNow = false; fetchPrayer(); }

      // Looking for an update is the slowest thing it does, and used to
      // freeze the panel for up to half a minute while it ran. The
      // strings are filled first and upState is set last, so the panel
      // never reads a tag that is only half written.
      if (wantOtaLatest) {
        wantOtaLatest = false;
        uint8_t seq = upSeq;                   // who asked, and are they still there
        bool ok = otaFetchLatest();
        if (seq == upSeq) {
          if (!ok) upState = U_FAIL;
          else if (upTag == String("v" FW_VERSION) || upTag == String(FW_VERSION))
            upState = U_NONE;
          else { upYes = true; upState = U_ASK; }
        }
      }
      if (wantOtaList) {
        wantOtaList = false;
        uint8_t seq = upSeq;
        bool ok = otaFetchList();
        if (seq == upSeq) upState = ok ? U_LIST : U_FAIL;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

void setup() {
  // Waking from being switched off is not a boot, whatever the processor
  // thinks. Skipping the animations is the difference between picking it
  // up and having it there, and picking it up and watching it introduce
  // itself again.
  esp_sleep_wakeup_cause_t woke_ = esp_sleep_get_wakeup_cause();
  bool fromDeep = (woke_ == ESP_SLEEP_WAKEUP_GPIO || woke_ == ESP_SLEEP_WAKEUP_TIMER);
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
  // Leaning it about is gone as a way to drive this. Clear the old flag
  // so a device that had it switched on does not carry the setting
  // around in flash forever.
  if (prefs.isKey("ctrl")) prefs.remove("ctrl");
  // A paired Mac survives a reflash, because the token lives in NVS and
  // OTA never touches that. Losing it would mean walking over to the
  // device after every update, which nobody would put up with.
  cfgTok      = prefs.getString("tok", "");
  cfgLock     = prefs.getBool("lock", false) && cfgTok.length();
  cfgFollow   = prefs.getBool("follow", false);
  cfgTap      = constrain(prefs.getInt("tap", TAP_MED), 0, TAP_N - 1);
  cfgAutoUp   = prefs.getBool("autoup", false);
  cfgKnock    = prefs.getBool("knock", false);   // the pad drives this now
  cfgHijriAdj = constrain(prefs.getInt("hadj", 0), -2, 2);
  cfgOffline  = prefs.getBool("offl", false);
  cfgBike     = prefs.getBool("bike", false);
  cfgBikeTpl  = constrain(prefs.getInt("btpl", 0), 0, BIKE_TPL_N - 1);
  { String s;
    s = prefs.getString("bplate", "KA 50 HJ 5683"); snprintf(bikePlate, sizeof(bikePlate), "%s", s.c_str());
    s = prefs.getString("bmake", "Royal Enfield"); snprintf(bikeMake, sizeof(bikeMake), "%s", s.c_str());
    s = prefs.getString("bmodel", "Meteor 350");    snprintf(bikeModel, sizeof(bikeModel), "%s", s.c_str());
    s = prefs.getString("bowner", "Ahmed");         snprintf(bikeOwner, sizeof(bikeOwner), "%s", s.c_str());
    s = prefs.getString("name",   "Ahmed");         snprintf(cfgName,   sizeof(cfgName),   "%s", s.c_str()); }
  // Carried over from the old on/off. Shake on meant a shake and a
  // double both went back, which is "both"; shake off meant the double
  // only. Written back once so the next boot reads the new key.
  if (prefs.isKey("back")) cfgBack = constrain(prefs.getInt("back", BACK_BOTH), 0, BACK_N - 1);
  else {
    cfgBack = prefs.getBool("shake", true) ? BACK_BOTH : BACK_TOUCH;
    prefs.putInt("back", cfgBack);
  }
  cfgDeepIdx  = constrain(prefs.getInt("deepi", 1), 0, DEEP_N - 1);
  battFull    = constrain(prefs.getFloat("bfull", 4.10f), 3.90f, 4.30f);
  nextAutoUp  = millis() + 120000;          // not in the first two minutes
  cfgTz       = prefs.getString("tz", DEF_TZ);
  cfgSsid     = prefs.getString("ssid", "");
  cfgPass     = prefs.getString("pass", "");
  cfgKey      = prefs.getString("key", "");
  message     = prefs.getString("msg", "");
  locLat      = prefs.getFloat("lat", NAN);
  locLon      = prefs.getFloat("lon", NAN);
  {
    String c = prefs.getString("city", "");
    strncpy(wCity, c.c_str(), sizeof(wCity) - 1);
    wCity[sizeof(wCity) - 1] = 0;
  }
  loadTasks();
  loadRems();
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

  // A first guess at resting, so it is right from the first second
  // rather than after a minute. Not load bearing: the poll keeps
  // watching and will correct this on its own if it is wrong.
  // Pulled down, so an unconnected pin rests low and nothing inverts.
  // The widest range the ADC has. Half of a full cell is about 2.1V,
  // which is over the default and would simply read as "as high as it
  // goes" for the whole top half of the pack.
  analogSetPinAttenuation(BATT_PIN, ADC_11db);
  readBattery();

  pinMode(TOUCH_PIN, INPUT_PULLDOWN);
  // Which level means nobody is touching it.
  //
  // This is sampled at boot, and waking from deep sleep IS a boot: the
  // finger that woke it is still on the pad thirty milliseconds later
  // when this runs. So it learned the touched level as the resting
  // one, and from then on everything was inverted. The pad read as
  // pressed whenever nobody was near it, and as released while you
  // held it, and the next deep sleep armed its wake on the resting
  // level, so touching the thing did nothing at all.
  //
  // It only showed up now because deep sleep itself only started
  // happening often. Offline, the robot goes off the moment the screen
  // darkens, so this ran on nearly every wake, and each wake made it
  // worse.
  //
  // Remembered instead, and only measured when there is certainly no
  // finger on it: a cold start, or a wake by the timer.
  {
    bool byTouch = (woke_ == ESP_SLEEP_WAKEUP_GPIO);
    bool known   = prefs.isKey("trest");
    if (byTouch && known) {
      touchRest = prefs.getBool("trest", false);
      Serial.println("woken by the pad, so the resting level is the remembered one");
    } else {
      int hi = 0;
      for (int i = 0; i < 12; i++) { if (digitalRead(TOUCH_PIN)) hi++; delay(3); }
      touchRest = (hi >= 7);
      prefs.putBool("trest", touchRest);
    }
    touchLvl = touchRest; touchLvlAt = millis();
    Serial.printf("touch pad on GPIO%d rests %s\n", TOUCH_PIN, touchRest ? "high" : "low");
  }

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) { Serial.println("no OLED"); return; }
  oled.setTextWrap(false);
  oled.setTextColor(SSD1306_WHITE);
  applyBright();

  eyes.begin(SCRW, SCRH, 50);
  applyEyes(cfgEyes);

  // Waking is waking, however the processor got there. What is
  // skipped on the way back from deep sleep is the introduction: the
  // sensor sweep, the name card and the how-to card. The eyes opening
  // is not an introduction, it is the robot waking up, and leaving it
  // out made coming back from a touch look like a fault.
  animWake();
  startSensors();
  applyFallInt();
  applyTap();
  // Ask the hardware whether the wire is there rather than assuming it.
  intWired = probeIntPin();
  applyFallInt();                     // the probe borrowed the interrupt setup
  applyTap();
  deepOff = prefs.getBool("nodeep", false);
  Serial.printf("INT1 %s\n", intWired ? "wired, it can switch off" : "not wired, screen off only");
  if (!fromDeep) animSenses(1500);

  // Off means off, from here and not from a few lines later. Setting
  // the mode at all brings the radio up, so when you have said stay
  // off, none of this runs: no mode, no scan, no join, no web server
  // and no Mac. Previously the setting was read before this point and
  // then ignored by it, so a board told to stay offline still woke up,
  // powered the radio, worked down the whole list of networks and only
  // then went quiet. That is not off, that is off afterwards.
  // The clock runs through deep sleep and the system time comes back
  // with it, so ask the board what time it thinks it is before
  // deciding it does not know. Without this, waking offline meant a
  // board with a perfectly good clock calling itself clockless, and
  // everything downstream of timeOk went quiet with it: no reminders
  // fired, no prayer alerts fired, and the home screen refused to say
  // the time it was holding. A cold start reads 1970 and fails this
  // on its own, which is the case where saying nothing is right.
  {
    struct tm t0;
    if (getLocalTime(&t0, 0) && t0.tm_year > 123) {   // past 2023, so it is real
      timeOk = true;
      clockSrc = "kept through sleep";
      Serial.println("clock survived: reminders and prayer times still stand");
    }
  }

  loadNets();
  if (cfgOffline) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_OFF);
    btStop();                        // nothing uses it; make sure nothing can
    netUsing = -1; netTrying = 0;
    Serial.println("offline by choice: radio stays down");
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    // Work down the list. Whichever answers first is the one it stays on,
    // so put the one you are usually near at the top.
    for (int i = 0; i < netCount && !online(); i++) {
      WiFi.begin(netSsid[i], netPass[i]);
      unsigned long t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 7000) {
        animWifiFrame();
        delay(40);
      }
      if (online()) { netUsing = i; cfgSsid = String(netSsid[i]); }
    }
    netTrying = netUsing < 0 ? 0 : netUsing;
    setupWeb();
  }

  if (online()) {
    restingFace("Connected", 700);
    // A few seconds here, then the loop keeps trying. Boot should not
    // hang on a clock that may take a minute to arrive.
    for (int f = 0; f < 6 && !timeOk; f++) {
      animClockFrame();
      if (trySyncTime(700)) break;
    }
    nextTimeTry = millis() + 15000;
    if (!fromDeep) {
      restingFace(timeOk ? "Clock set" : "Clock still coming", 700);
      nameCard();
    }
  } else if (!fromDeep && !cfgOffline) {
    offlineWelcome();
  }

  eyes.setAutoblinker(ON, 7, 5);      // a blink now and then, not a flutter
  eyes.setIdleMode(ON, 5, 4);
  eyes.setMood(STYLES[cfgEyes].mood);

  // Two columns, numbers on a common left edge so the words line up.
  // Not shown when it was only switched off: you know how to knock by
  // the second time you pick it up.
  oled.clearDisplay();
  if (!fromDeep) {
    // What the pad does, because the pad is how it is driven. It used
    // to list what one to four knocks meant, which is a feature that
    // is off unless you ask for it.
    titleBarC("HOW TO USE ME");
    at(8,  16, "1  next");
    at(8,  28, "2  back");
    at(66, 16, "hold  open");
    at(66, 28, "5s  home");
    oled.drawFastHLine(8, 40, 112, SSD1306_WHITE);
    ctr("Touch the pad", 48, 1);
  }
  if (!fromDeep) {
    oled.display();
    holdCard(2000);                    // a knock ends it, and it never dawdles
  }

  screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0;
  lastActive = millis();
  if (fromDeep) {
    wokeForAlarm = (woke_ == ESP_SLEEP_WAKEUP_TIMER);
    wokeBy = wokeForAlarm ? "prayer" : "picked up";
    Serial.printf("back from being off (%s)\n", wokeBy.c_str());
  }
  drawHome();                          // on screen before anything can block

  // These all started at zero, so the first pass through loop() fired
  // every one of them back to back: weather, then prayer, then a story.
  // Between them that is the better part of half a minute of blocking
  // network calls, during which nothing redraws and no knock is acted
  // on. The card stayed up and the device looked wedged. Stagger them.
  if (cfgFollow && online()) cursorUdp.begin(CURSOR_PORT);

  // The screen has to outrank the network or a download would still take
  // it. Raising this task rather than lowering the other one leaves the
  // network comfortably above idle, so it still gets on with things.
  vTaskPrioritySet(NULL, 2);
  if (xTaskCreate(netLoop, "rafiq-net", 12288, nullptr, 1, &netTask) != pdPASS) {
    Serial.println("no network task; fetching will block the screen");
    netTask = nullptr;
  }

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
  readBattery();                       // every twenty seconds, it decides
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

  // These used to wait for a lull, because each one froze the screen
  // while it ran. They are handed to the network task now, so they can
  // simply go when they are due and the screen carries on regardless.
  if (online()) {
    // keep trying for a clock until one lands, then leave it alone
    if (!timeOk && (long)(now - nextTimeTry) >= 0) {
      nextTimeTry = now + 20000;
      wantTime = true;
    }
    if ((long)(now - nextWx) >= 0) { nextWx = now + 900000UL; wantWx = true; }

    struct tm t;
    bool haveDay = timeOk && getLocalTime(&t, 0);
    // They barely move week to week, and refetching daily meant losing
    // them whenever the network was down. Keep what is in flash; refresh
    // when asked, or once every fiftieth boot.
    bool dueByBoot = (cBoot >= prayerBoot + PRAYER_REFRESH_BOOTS);
    if ((long)(now - nextPrayerTry) >= 0 && haveDay &&
        (!prayerOk || prayerWanted || dueByBoot)) {
      nextPrayerTry = now + 300000UL;
      wantPrayerNow = true;
    }
    // a fresh read every six hours, and the queue left over from a reload
    // The longest fetch of the lot, and the only one that writes into
    // the shelf the reader draws from. It stays on this loop, where
    // nothing can be halfway through reading that, and only runs while
    // asleep so it cannot freeze a screen you are looking at.
    if (asleep && (long)(now - nextStory) >= 0 && cfgKey.length() &&
        !storyBusy && readCount < READS_MAX) {
      nextStory = now + 21600000UL;
      fetchStory(false);
    }
    if (refillWant > 0 && !storyBusy && cfgKey.length() &&
        (long)(now - nextRefill) >= 0 && asleep) {
      refillWant--;
      nextRefill = now + 5000;
      fetchStory(false);                       // quietly, while you are elsewhere
    }
  }

  // Looking for itself once a day, and putting on whatever it finds.
  // Only while nothing is going on: never mid game, mid read, mid focus
  // or with a Mac driving it.
  if (cfgAutoUp && online() && upState == U_OFF && (long)(now - nextAutoUp) >= 0 &&
      asleep && !sessionRunning() && !storyBusy) {
    nextAutoUp = now + AUTOUP_EVERY_MS;
    wantOtaLatest = true;
    autoUpArmed = true;
  }
  // It found one, and nobody needs asking.
  if (autoUpArmed && upState == U_ASK) {
    autoUpArmed = false;
    upState = U_OFF;
    wake("update");
    otaInstall();
    upState = U_FAIL;
  }
  if (autoUpArmed && (upState == U_NONE || upState == U_FAIL)) {
    autoUpArmed = false;
    upState = U_OFF;                 // nothing found; say nothing
  }

  // A reminder coming due. Checked here rather than left to the Mac,
  // which is the whole point of keeping them: the Mac may be shut.
  if (timeOk && remCount && now - remCheck > 2000) {
    remCheck = now;
    time_t tnow = time(nullptr);
    for (int i = 0; i < remCount; i++) {
      if (rems[i].done || (long)rems[i].at > (long)tnow) continue;
      // Only on the day it was for. Something from yesterday has had
      // its chances and should not follow you into the week.
      {
        time_t ft = (time_t)rems[i].first, nt = (time_t)tnow;
        struct tm fd, nd; localtime_r(&ft, &fd); localtime_r(&nt, &nd);
        if (fd.tm_yday != nd.tm_yday || fd.tm_year != nd.tm_year) {
          rems[i].done = true; saveRems(); continue;
        }
      }
      // Being switched off and brought back by the alarm counts as
      // having been asleep: the boot is why `asleep` is already false.
      remWokeIt = asleep || wokeForAlarm;
      wake("reminder");
      toastKind = "remind";
      toastText = rems[i].text;
      toastUntil = millis() + 60000UL;           // a minute, it is why it woke up
      toastFlash = millis();
      remShowing = i;                            // so a press knows which one
      break;
    }
  }

  if (popupUntil && now > popupUntil) { popupUntil = 0; screen = S_HOME; depth = 0; }

  // Trying a strength is ten seconds with an end on it. No knock in
  // there is a command, which is the only way a screen that measures
  // knocks can be trusted not to walk out from under you. When the
  // window closes it puts back whatever was set, drops you on the list
  // you came from and says what it saw. Nothing is committed by trying;
  // only Set ever commits.
  if (tapTesting) {
    lastActive = now;
    if ((int32_t)(now - tapTestEnds) >= 0) {
      tapTesting = false;
      tapChosen  = false;
      applyTap();
      depth = 3;
      char m[16];
      if (tapSeen) snprintf(m, sizeof(m), "SAW %lu", (unsigned long)tapSeen);
      else         snprintf(m, sizeof(m), "SAW NONE");
      flash(m, 1400);
    }
  }

  // Focus used to hold the panel lit for the whole run, which is the
  // one reliable way to burn a countdown into an OLED. It now shows
  // for twelve seconds, goes dark for ten, and every third time comes
  // back with a word instead of the clock. The device is never asleep
  // through any of it: the timer keeps running and one knock brings it
  // straight back.
  if (sessionRunning()) {
    lastActive = now;
    screen = S_FOCUS;
    // Browsing the list while something is running is allowed; only the
    // countdown itself owns depth zero.
    if (depth > 1) depth = 0;
    if (depth == 1 || millis() < flashUntil) {
      fzPhase = FZ_SHOW; screenPower(true);      // you are reading it
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

  // A stopwatch that is counting holds the screen; one sitting at a
  // number does not. It used to hold it either way, so leaving a
  // stopped stopwatch on screen kept the robot awake all night.
  if (swOn) { screen = S_FOCUS; if (swRun) lastActive = now; }
  // A depth on a screen that has no inside is a depth left behind by
  // the screen you came from.
  if (depth && !screenHasDepth(screen)) { depth = 0; itemIdx = 0; subIdx = 0; }

  // Seven minutes of a dark screen with nobody listening and it can
  // switch off altogether. A Mac on the other end counts as somebody
  // listening: disconnect in Rafiq and the clock starts, reconnect and
  // it never goes past a dark screen.
  if (wantDeep) {
    // Asked for, by holding the pad or by the Mac letting go. No
    // conditions: if you held it for ten seconds you meant it.
    wantDeep = false;
    goDeep();
  }
  // Otherwise it switches off on its own, but only when the Mac is not
  // there. With the Mac connected it only darkens the screen, because
  // powering down drops the network and the Mac would lose it mid
  // sentence. Holding the pad still forces it either way.
  // Off the network there is nothing to stay reachable for, so the
  // screen going dark and the robot switching off are the same moment.
  // On the network it keeps the two apart, because dropping the Mac
  // mid sentence to save a little current is a poor trade.
  uint32_t wait = offlineNow() ? 0 : deepAfterMs();
  if (!deepOff && (offlineNow() || deepAfterMs()) && asleep && !sessionRunning() &&
      !macLinked && (now - sleptAt) > wait && upState == U_OFF && !storyBusy) {
    goDeep();
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

  // A word about what just happened, over whatever is on screen. It
  // does not move you anywhere, which is the whole point of it.
  if (millis() < flashUntil) {
    lastActive = now;
    if (now - lastDraw >= 60) { lastDraw = now; drawFlash(); }
    delay(2); return;
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
  if (dndUntil) {
    if (now >= dndUntil) { dndUntil = 0; }
    else {
      lastActive = now;
      if (now - lastDraw >= 200) { lastDraw = now; drawDnd(); }
      delay(2); return;
    }
  }
  if (busyCam || busyMic) {
    lastActive = now;
    if (now - lastDraw >= 200) { lastDraw = now; drawBusy(); }
    delay(2); return;
  }
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
  if (toastUntil && now >= toastUntil) {
    toastUntil = 0; toastText = ""; toastKind = "";
    // Nobody said anything, so nobody was there. Ask again, a little
    // further off each time, and give up after the last step rather
    // than asking all evening.
    if (remShowing >= 0 && remShowing < remCount && !rems[remShowing].done) {
      Rem& r = rems[remShowing];
      if (r.tries >= REM_STEPS) {
        r.done = true;
        Serial.println("reminder asked all it is going to, letting it go");
      } else {
        uint16_t mins = REM_LADDER[r.tries++];
        r.at = (uint32_t)time(nullptr) + (uint32_t)mins * 60UL;
        Serial.printf("reminder ignored, back in %u min (%u of %u)\n",
                      mins, r.tries, (unsigned)REM_STEPS);
      }
      saveRems();
    }
    remShowing = -1;
    remWokeIt = false;
  }
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
      case S_BIKE:     drawBike();      break;
      case S_REMIND:   drawReminders(); break;
      case S_SYSTEM:   drawSystem();   break;
      default:         drawHome();     break;
    }
    // Only ever past five seconds, so it costs ordinary use nothing and
    // the one time it appears is the one time you want telling.
    if (touchHold) { drawHoldTier(now - touchPressAt); oled.display(); }
  }
  delay(2);
}

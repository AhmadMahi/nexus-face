/*
  ================================================================
   RAFIQ  -  ESP32-C3 desk companion
  ================================================================
   6.0  Bluetooth first. The phone is home: its clock, its
        notifications, and RAFIQ commands sent from a Shortcut. WiFi
        is something you ask for (Settings, the page, or RAFIQ sync /
        wifi / update / config) and it goes away again by itself.
        Light sleep keeps the phone linked with the screen dark, on a
        core built with power management. See HANDOFF.md.
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
// Up here with the rest, not beside the code that uses them. Put them
// halfway down and the prototypes Arduino generates land above them,
// naming types nothing has heard of yet.
// NimBLE, not the core's bundled BLE wrapper. See the note on the
// Bluetooth module: Apple's notification service needs a GATT client
// over an INBOUND connection, and only this library will give you one.
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

//  Arduino hoists its generated prototypes to just below the
//  includes, above everything the sketch declares. Anything that
//  appears in a function signature therefore has to be a type by
//  this point or the prototype will not parse, which is why a
//  notification is defined here rather than beside the rest of the
//  Bluetooth code where it belongs. Apple hands over an id, a
//  category, and then, only if you ask, the app, the title and the
//  body. The id is what you quote back to dismiss it or take a call.
struct Note {
  uint32_t uid;
  uint8_t  cat;
  bool     unread;
  uint32_t at;                         // millis when it landed
  // Long enough for a real bundle identifier. Eighteen was not:
  // com.apple.MobileSMS is nineteen characters, so it arrived as
  // com.apple.MobileS, and appShort took the tail of that and showed
  // you "MobileS". Every friendly name silently missed.
  char     app[40];
  char     title[34];
  char     msg[100];
};
#include <WiFiUdp.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include "esp_pm.h"
#include "driver/usb_serial_jtag.h"
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
// first four seconds, because a bar on screen during ordinary use was
// worse than the thing it explained; nobody holds for four seconds by
// accident, so after that it is safe to say what is about to happen.
// Four, then three to change your mind: seven in all.
#define TOUCH_HOME_MS  4000UL

#define TOUCH_COUNT_MS  3000UL   // and then it counts three and goes
#define TOUCH_GAP_MS  300UL      // quiet for this long and the count is final
#define TOUCH_DEBOUNCE 40UL
enum { TG_ONE = 1, TG_TWO, TG_THREE, TG_LONG };
uint32_t touchPressAt = 0;       // when the finger went down
uint32_t touchLiftAt  = 0;       // when it last came up
uint8_t  touchTaps    = 0;       // lifts so far in this run
bool     touchLongDone = false;  // the long press already fired this press
// Holding, in one move.
//
//  Nothing for five seconds, then it goes home and begins counting
//  down to switching off. Let go during the count and it stays, at
//  home, which is where you wanted to be. Hold through it and it
//  switches off.
//
//  There were two tiers before this, five seconds and then eight,
//  with a screen in between explaining the difference. One thing that
//  does one thing, with three seconds to change your mind, is better
//  than two things that need telling apart.
//
//  When the count began, or 0 if it is not running.
uint32_t sleepArmed   = 0;
// 6.1: the hold bar. Times follow C3 Buddy: Open from the hold time
// until 1.5 s after it (at least 2 s), Cancel for a second, then Back,
// then from Back + 2 s a switch-off bar that ends in deep sleep 3 s on.
const uint16_t HOLD_OPTS[] = { 500, 700, 1000, 1500, 2000, 2500 };
#define HOLD_N 6
int  cfgHoldIdx = 1;
bool holdShown  = false;
bool cfg12h     = false;               // 12 hour clock
// What wakes it from sleep: the pad and movement (knock, shake, lift),
// only the pad, or only movement.
int  cfgWakeBy  = 0;
const char* WAKEBY_NAME[3] = { "touch+move", "touch", "move" };
static bool touchWakes()  { return cfgWakeBy != 2; }
static bool pocketLocked();
static bool motionWakes() { return cfgWakeBy != 1 && !pocketLocked(); }   // 7.9: not while locked
static uint32_t holdMs()          { return HOLD_OPTS[cfgHoldIdx]; }
static uint32_t holdMaxMs()       { uint32_t a = holdMs() + 1500; return a > 2000 ? a : 2000; }
static uint32_t holdBackMs()      { return holdMs() + (holdMaxMs() - holdMs()) / 2; }   // Open ends, Back begins

// The longest unbroken touch the pad has ever reported, for finding
// out what this board's own ceiling actually is rather than arguing
// about datasheets. Shown on SYSTEM.
uint32_t touchLongest = 0;
bool     wantDeep     = false;   // asked for, by holding or by the Mac letting go
// It came back from being switched off because a reminder or a prayer
// was due, and that is the only reason it is on. Waking that way is a
// boot, so `asleep` is false by the time the reminder fires and
// nothing downstream could tell this from someone picking it up.
bool     wokeForAlarm = false;
// When the next prayer or reminder is due, as a moment on the clock,
// or 0 for none. In RTC memory, so it survives deep sleep: a wake that
// comes to nothing can put itself back down without having to work
// the answer out again from scratch, and without the chance of
// working out a different one and losing the alarm.
RTC_DATA_ATTR uint32_t rtcAlarmAt = 0;

// How long the pad has to be held before a touch counts as waking it.
//
// A sleeve, a sleeve's cuff, a hand put down on the desk next to it:
// all of them are shorter than a second, and every one of them used to
// cost a full wake with the screen and the radio up. A second is
// longer than any of them and shorter than anyone waits for an answer.
// Three seconds is long enough to want telling about, so that one
// lights the screen half a second in and fills a ring.
const uint16_t WAKE_OPTS[] = { 0, 1000, 3000 };
const char*    WAKE_NAME[] = { "off", "1s", "3s" };
#define WAKE_N 3
#define WAKE_RING_AFTER 500UL        // and only when there is a wait worth showing
#define WAKE_RING_MIN  1200UL
int cfgWakeIdx = 2;


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
// Which gesture on the body of the robot goes back a level.
//
// Two presses on the pad always go back. That is how the thing is
// driven and it is not a setting. What this picks is what the
// accelerometer does: a knock on the shell, a shake of it, or either.
// It was a plain on/off for the shake before, which left no way to
// ask for the knock on its own, and for one release it offered
// "touch" as one of the three, which was me misreading what was being
// asked for: the pad is not the question here, the body is.
//
// Walked by holding on the row, the way every other setting is
// walked, and the value is shown so you can see where you are without
// opening anything.
//
// Knocking has to be switched on for the knock to mean anything, and
// the row says so rather than offering a setting that does nothing.
enum { BACK_KNOCK = 0, BACK_SHAKE, BACK_BOTH, BACK_N };
int cfgBack = BACK_BOTH;
const char* BACK_NAME[BACK_N] = { "knock", "shake", "both" };
static bool backByShake() { return cfgBack != BACK_KNOCK; }
static bool backByKnock() { return cfgBack != BACK_SHAKE; }
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

#define FW_VERSION "7.12.0"
#define OTA_REPO   "AhmadMahi/nexus-face"
#define OTA_ASSET  "nexus_face.bin"

#define DEF_WIFI_SSID "__WIFI_SSID__"
#define DEF_WIFI_PASS "__WIFI_PASS__"
#define DEF_TZ        "IST-5:30"
const char* RESCUE_SSID = "RAFIQ-SETUP";
const char* RESCUE_PASS = "password";

//  The panel, with one addition: while a finger is held on the pad
//  past the hold time, every frame gets the hold strip drawn along its
//  bottom just before it goes out. The screens never know.
static void drawHoldStrip();
static bool gamePlaying();
static bool holdStripWanted();
struct OledX : public Adafruit_SSD1306 {
  using Adafruit_SSD1306::Adafruit_SSD1306;
  void display() {
    if (holdStripWanted()) drawHoldStrip();
    Adafruit_SSD1306::display();
  }
};
OledX oled(SCRW, SCRH, &Wire, -1);
RoboEyes<OledX> eyes(oled);
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
       S_FAITH, S_READS, S_GAMES, S_SETTINGS, S_SYSTEM,
       S_TODAY, S_FHUB, S_CALM, S_COUNT };      // 7.6: the hubs
const char* S_NAME[S_COUNT] =
  { "HOME", "VEHICLE", "REMINDERS", "MAC", "WEATHER", "NOTIFICATIONS", "PRAYER",
    "FAITH", "SHORT READS", "GAMES", "SETTINGS", "SYSTEM",
    "TODAY", "FAITH", "CALM" };

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
//  Three ways to be, not two. WiFi is what it has always done.
//  Bluetooth means the WiFi radio never starts and the phone is what
//  it talks to. Off means neither.
//
//  cfgOffline stays as the word the rest of the file uses for "there
//  is no WiFi to be had", because that is as true in Bluetooth mode
//  as in Off and every piece of logic asking the question wants the
//  same answer in both. One line here instead of thirty edits.
enum { NET_WIFI = 0, NET_BT, NET_OFF, NET_N };
int cfgNet = NET_WIFI;
const char* NET_NAME[NET_N] = { "wifi", "bluetooth", "off" };
#define cfgOffline (cfgNet != NET_WIFI)
static void bleOn();
static void bleOff();
static const char* btShort();
bool netDown = false;
int  netMisses = 0;                // failed joins since the last success
bool hadNet = false;               // it has been online at least once this time up
static bool offlineNow() { return cfgOffline || netDown; }

// ---------------- 6.0: Bluetooth first ----------------
//  Bluetooth is home. WiFi is something you ask for, for a while, and
//  then it goes away again by itself.
//
//  cfgNet is what the radios are doing right now, and the rest of the
//  file goes on asking it exactly as before. cfgNetHome is where it
//  comes back to, and is the only one kept in flash: Bluetooth, or
//  Off. WiFi is never kept, so a real restart always lands on
//  Bluetooth.
//
//  A WiFi session is one of four kinds:
//    MANUAL   you asked for WiFi. Ends on a real restart, or thirty
//             minutes after the page or the Mac last used it. Deep
//             sleep and waking again does not end it.
//    SYNC     RAFIQ sync. Join, fetch everything, check for an update,
//             write a read, radio off.
//    UPDATE   Check update, asked for while on Bluetooth. Ends when
//             you leave the update screen.
//    HOTSPOT  The setup hotspot, for an update from a file. Ends ten
//             minutes after the last phone leaves it.
extern bool wxOk;                      // declared with the weather, further down
int cfgNetHome = NET_BT;
const char* lastReset = "";
const char* bootNote = nullptr;
volatile uint8_t btNews = 0;
// ---- 7.9: pocket lock ----
int  cfgPLock = 2;                     // off, 1, 3, 10 minutes
const uint8_t PLOCK_MIN[4] = { 0, 1, 3, 10 };
const char* PLOCK_NAME[4] = { "off", "1 min", "3 min", "10 min" };
uint32_t lastUserAt = 0;               // the last real touch, not a popup or a glance
bool     unlocking = false;
uint32_t unlockAt = 0;
bool     unlockShown = false;
// ---- 7.8: the battery log, one charge cycle ----
//  Kept in RTC memory through deep sleep and written to flash every half
//  hour and before deep sleep: about fifty small writes a day, and a few
//  additions per loop. It costs nothing you could measure.
#define BLOG_MAGIC 0xB10C0001UL
struct BLog {
  uint32_t magic, start;               // start: when the cycle began (epoch, 0 if unknown)
  float    v0; uint8_t pct0;           // the battery then
  uint32_t sOn, sDark, sLight, sDeep, sWifi;   // seconds in each state
  uint32_t wakes, restarts;
};
RTC_DATA_ATTR BLog blog;
RTC_DATA_ATTR uint32_t rtcDeepAt = 0;
RTC_DATA_ATTR bool blogArmed = true;   // a reset waits for the voltage to fall back first
bool cfgBlog = true;
int  cfgBlogV = 1;                     // 4.10, 4.20, 4.25, 4.30
int  cfgCap = 350;                     // mAh
const float BLOG_V[4] = { 4.10f, 4.20f, 4.25f, 4.30f };
// Typical draw in each state, for the shares (estimates; "Used" is measured).
const float BLOG_MA[5] = { 38.0f, 19.0f, 3.0f, 0.3f, 80.0f };
// ---- 7.7: night sleep ----
bool cfgNight = false;
int  cfgBed = 23 * 60;                 // bedtime, minutes after midnight
RTC_DATA_ATTR int nightPush = 0;       // "an hour later", for tonight only
uint32_t nightCardUntil = 0;
bool nightDeep = false;
// ================================================================
//  DESIGN RULES (7.6)
// ================================================================
//  One set of measures, so every screen reads as one product.
//    panel     128 x 64
//    padding   3 px each side, everywhere
//    bar       11 px: name left, time or count right
//    rows      12 px pitch, 4 visible, first text at y 14
//    icons     8 x 8 at x 3; text after them at x 15 (icon + 4 px)
//    hub icons the same icons, twice the size: one icon language
//    selected  a rounded bar 1 px in from each edge
//    values    right-aligned to the padding; 5 px further in with a scrollbar
//    scrollbar 2 px at the right edge, only past four rows
//    hint      centred on the bottom line, y 54
#define UI_PAD     3
#define UI_BAR_H   11
#define UI_ROW_Y   14
#define UI_ROW_H   12
#define UI_ROWS    4
#define UI_ICON    8
#define UI_TEXT_X  (UI_PAD + UI_ICON + 4)
#define UI_HINT_Y  54

// Eight by eight, one byte a row, leftmost pixel the top bit.
static const uint8_t IC_BELL[8]  = { 0x18, 0x3C, 0x7E, 0x7E, 0x7E, 0xFF, 0x00, 0x18 };
static const uint8_t IC_CHECK[8] = { 0xFF, 0x81, 0x83, 0x85, 0xA9, 0x91, 0x81, 0xFF };
static const uint8_t IC_MAC[8]   = { 0x00, 0x7E, 0x42, 0x42, 0x42, 0x7E, 0xFF, 0x00 };
static const uint8_t IC_SUN[8]   = { 0x10, 0x54, 0x38, 0xFE, 0x38, 0x54, 0x10, 0x00 };
static const uint8_t IC_CAR[8]   = { 0x00, 0x3C, 0x66, 0xFF, 0xFF, 0x66, 0x00, 0x00 };
static const uint8_t IC_MOON[8]  = { 0x3C, 0x70, 0xE0, 0xE0, 0xE0, 0x70, 0x3C, 0x00 };
static const uint8_t IC_BEADS[8] = { 0x3C, 0x42, 0x81, 0x81, 0x81, 0x42, 0x3C, 0x18 };
static const uint8_t IC_DAWN[8]  = { 0x00, 0x54, 0x38, 0x7C, 0x7C, 0xFF, 0x00, 0x00 };
static const uint8_t IC_BOOK[8]  = { 0x00, 0x66, 0x99, 0x99, 0x99, 0x99, 0xE7, 0x18 };
static const uint8_t IC_STAR[8]  = { 0x10, 0x10, 0x38, 0xFE, 0x38, 0x6C, 0x44, 0x00 };
static const uint8_t IC_SLIDE[8] = { 0x70, 0xFF, 0x70, 0x00, 0x0E, 0xFF, 0x0E, 0x00 };
static const uint8_t IC_LEAF[8]  = { 0x0E, 0x3F, 0x7E, 0x7E, 0xFC, 0xF8, 0x80, 0x00 };
static const uint8_t IC_PAGE[8]  = { 0xFE, 0x82, 0xBA, 0x82, 0xBA, 0x82, 0xFE, 0x00 };
static const uint8_t IC_PAD[8]   = { 0x00, 0x7E, 0xDD, 0x8F, 0xDB, 0x7E, 0x66, 0x00 };
static const uint8_t IC_HAND[8]  = { 0x20, 0x20, 0x2C, 0x3E, 0x7E, 0x7E, 0x3C, 0x00 };
static const uint8_t IC_BT[8]    = { 0x10, 0x18, 0x54, 0x38, 0x38, 0x54, 0x18, 0x10 };
static const uint8_t IC_SHIELD[8]= { 0x7E, 0x81, 0x81, 0x81, 0x42, 0x42, 0x24, 0x18 };
static const uint8_t IC_BATT[8]  = { 0x00, 0xFC, 0x84, 0xB6, 0xB6, 0x84, 0xFC, 0x00 };
static const uint8_t IC_CHIP[8]  = { 0x54, 0x7C, 0xC6, 0x44, 0xC6, 0x7C, 0x54, 0x00 };

// ---- the hubs ----
enum { HI_SCREEN = 0, HI_FAITH, HI_ADHKAR, HI_RELAX, HI_PSET };
struct HubIt { uint8_t kind; uint8_t a; const uint8_t* icon; const char* name; };
int inHub = -1, hubSel = 0, hubEntry = 0;

extern "C" void ble_svc_gatt_changed(uint16_t start_handle, uint16_t end_handle);
// getLocalTime(&t, 0) can give up without looking: it notes millis(),
// then only looks while no millisecond has passed since. If the counter
// ticks between the two, it reports "no time" with the clock fine. That
// was the robot's "waiting for the clock" face appearing at random, and
// the same answer reached prayer alerts and reminders. This just looks.
static bool nowLocal(struct tm* t) {
  time_t n = time(nullptr);
  localtime_r(&n, t);
  return t->tm_year > (2016 - 1900);
}
// ---- 7.3: devices and links ----
//  Every device that has paired is remembered with a name. Two can be
//  preferred: Primary (the notification phone: clock, notifications,
//  guard, auto Away) and Second (a companion, say the Mac). With
//  Multi-link on, two stay connected; a preferred device arriving takes
//  the place of one that is not. btConn stays "the phone" for all the
//  older code; btConn2 is the companion.
struct DevRec { uint8_t a[6]; char label[18]; };
#define DEV_N 6
DevRec   devs[DEV_N];
int      devN = 0;
uint8_t  prefA[2][6];                  // all zero: no preference
bool     cfgMulti = false, cfgAutoAway = true;
volatile uint16_t btConn2 = 0xFFFF;
struct LinkRec { uint16_t h; uint8_t a[6]; bool authed; };
LinkRec  links[3];
int      linkN = 0;                    // set on the NimBLE task, read by loop
volatile uint16_t authQ[4];            // links that just finished pairing, for loop
volatile uint8_t  authHead = 0, authTail = 0;
bool     btEverLinked = false;
uint32_t doorPauseUntil = 0;
volatile uint32_t advFastUntil = 30000;     // fast for the first half minute after start
volatile bool     advSlow = false;
static void advFast() {                     // a link just went: listen quickly for a while
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setMinInterval(244); adv->setMaxInterval(244);    // 152.5 ms
  advSlow = false;
  advFastUntil = millis() + 30000;
}           // after turning a device away, a minute closed
// ---- 7.3: automatic Away ----
bool     awayAuto = false;             // this Away began because the phone left
RTC_DATA_ATTR uint8_t rtcAwayCheck = 0;
bool     awayCheckBoot = false, awayQuietDeep = false;
#define  AUTO_AWAY_MS 120000UL
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
//  7.4:
//    EVT   notify  gestures, as they happen ("1", "2", "hold" ...)
//    PTR   write without response  "x y", -1000..1000, the pointer
//    CFG   read    the settings, as JSON with /api/state's own names
#define RQ_EVT  "52a1f005-7a3e-4b5c-9d6f-0a1b2c3d4e5f"
#define RQ_PTR  "52a1f006-7a3e-4b5c-9d6f-0a1b2c3d4e5f"
#define RQ_CFG  "52a1f007-7a3e-4b5c-9d6f-0a1b2c3d4e5f"
NimBLECharacteristic* rqEvt = nullptr;
//  7.5:
//    LST  read   {"apps":[seen],"muted":[...],"vip":[...]}
//    OTA  write without response: firmware bytes, after "!ota begin"
#define RQ_LST  "52a1f008-7a3e-4b5c-9d6f-0a1b2c3d4e5f"
#define RQ_OTA  "52a1f009-7a3e-4b5c-9d6f-0a1b2c3d4e5f"
// ---- 7.5: the Mac's cards: 0 health, 1 top three, 2 pinned task ----
struct Card { bool on; char title[22]; char line[3][24]; int8_t bar; };
Card cards[3];
int  macSel = 0;
bool knobOn = false, knobUsed = false, walkOn = false, macBye = false, macDim = false;
float knobRef = 0; int knobLast = 0; uint32_t knobAt = 0;
int8_t leanState = 0;                  // -1 left, 0 middle, 1 right (gesture mode)
uint32_t walkUntil = 0;
// ---- app filter and VIPs, kept on the robot so every phone obeys it ----
#define APPS_N 12
#define MUTE_N 12
#define VIP_N  8
char seenApps[APPS_N][20]; int seenN = 0;
char mutedApps[MUTE_N][20]; int mutedN = 0;
char vipWords[VIP_N][20];  int vipN = 0;
// ---- updates over Bluetooth ----
volatile bool otaOn = false, otaErr = false;
volatile uint32_t otaSize = 0, otaGot = 0, otaLastAt = 0;
String   rdRx;                         // a story arriving from the Mac
bool     rdRxOn = false;
uint16_t otaConn = 0xFFFF;
int otaPctSent = -1;
extern bool cfgGesture, cfgFollow;     // both declared with gestures, further down
struct AppMsg { uint8_t kind; uint16_t len; uint16_t conn; char data[420]; };
#define APPQ_N 4
AppMsg appQ[APPQ_N];
volatile uint8_t appHead = 0, appTail = 0;   // written by NimBLE, read by loop
volatile bool btApp = false;           // the phone on the line runs the Rafiq app
extern bool timeOk;                    // both declared with the clock, further down
extern const char* clockSrc;
uint32_t appNoteSeq = 0;
uint16_t appRxCmd = 0, appRxNote = 0;   // what has arrived from the app
char     appLast[16] = "";
// ---- 6.4 Away ----
//  "Away: back at 3, call Ahmed". The screen shows that and the time,
//  nothing else, through restarts, until a RAFIQ home. Everything done
//  to it meanwhile is counted into the tamper log. Bluetooth stays on,
//  or home could never arrive.
bool     awayOn = false;
String   awayText = "";
#define  AWAY_SHOW_MS 3000UL        // a touch or a move while still listening
#define  AWAY_SENT_SHOW_MS 5000UL   // when it has just been sent
// 6.5: Away listens on Bluetooth for a while, then sleeps for real.
//   sent from the phone: 5 minutes listening for home
//   switched off and on: the message for 5 s, then 2 minutes listening
//   after that: deep sleep. A touch or a move shows the message for
//   3 s and it is straight back, Bluetooth never coming on, so nobody
//   but a restart (you) can reach it.
#define  AWAY_LISTEN_SENT_MS    300000UL
#define  AWAY_LISTEN_RESTART_MS 120000UL
#define  AWAY_RESTART_SHOW_MS     3000UL
#define  AWAY_DEEP_SHOW_MS        3000UL
uint32_t awayListenUntil = 0;
uint32_t awayShowMs = AWAY_SHOW_MS;
RTC_DATA_ATTR uint8_t rtcAwayDeep = 0;   // 1 asleep in Away, 2 pausing after a move
enum { AE_TOUCH = 0, AE_SHAKE, AE_MOVE, AE_KNOCK, AE_N };
const char* AE_NAME[AE_N] = { "Touched", "Shaken", "Moved", "Knocked" };
uint16_t aeCount[AE_N] = { 0, 0, 0, 0 };
uint32_t aeLast[AE_N]  = { 0, 0, 0, 0 };
static void awayEv(int k) { if (!awayOn) return; aeCount[k]++; aeLast[k] = millis(); }
uint32_t popGlanceMs = 1000;
// ---- 7.0 ----
// Who to get in touch with, shown in Away between showings of the
// message. Kept in the firmware at the owner's request: leave these
// out of anything pushed to a public repository.
#define OWNER_NAME  "Ahmed"
#define OWNER_PHONE "+918660027729"
#define OWNER_PHONE_SHOW "+91 86600 27729"
#define OWNER_MAIL  "mahiahmad53@gmail.com"
// Relax from a Shortcut lasts this long, then the robot sleeps.
#define RELAX_RQ_MS 180000UL
uint32_t relaxUntil = 0;
// The timer. While it runs, nothing else works: a touch only keeps the
// screen on (or lets it go back to its rhythm), RAFIQ timer changes it
// and RAFIQ home stops it. Done, it flashes until touched.
bool     tmrOn = false, tmrDone = false, tmrPinned = false;
uint32_t tmrStart = 0, tmrEnd = 0, tmrDoneAt = 0;           // how long news shows when it woke the robot
// ---- 6.4 phone-away sleep ----
//  Ten minutes dark and linked-or-listening after the phone goes, then
//  off, waking every 3 minutes for the first hour and every 5 after to
//  see whether it is back. A check wake is silent: no screen.
RTC_DATA_ATTR uint32_t rtcAwaySince = 0;   // when checking began, system clock
RTC_DATA_ATTR uint8_t  rtcWakeCheck = 0;   // this timer wake is a check, not a prayer
bool     deepAuto = false;             // this deep sleep is the phone-away kind
bool     checkWake = false;            // woke only to look for the phone
uint32_t checkUntil = 0;
#define  CHECK_WINDOW_MS 20000UL
uint32_t    glanceUntil = 0;           // a lift's look at the time ends here
#define GLANCE_MS 1500UL
const char* banText = "";              // a quiet line on a black screen
uint32_t    banUntil = 0;
// 6.1 popups: drawn over wherever you are. A tap closes it and you are
// exactly where you were; a hold opens it in Notifications. If it woke
// the robot, closing it puts the robot back to sleep.
bool     popOn = false, popWoke = false;
uint32_t popUntil = 0;
bool     notesDirty = false;
uint32_t notesSavedAt = 0;
int      noteSel = 0;
static void popupShow();
static void popupClose(bool open);
static void goSleepQuick();
static void popupOnWake();
static void noteHeader(const Note& n, const char* right);
static void twoButtons(const char* a, const char* b);
static void saveNotes();
NimBLEClient* btCl = nullptr;          // the phone, as a client: fetched once per link
volatile bool btClStale = true;          // 1 linked, 2 unlinked, for loop to say
enum { WS_NONE = 0, WS_MANUAL, WS_SYNC, WS_UPDATE, WS_HOTSPOT };
int      wsKind    = WS_NONE;
uint32_t wsStartMs = 0;
uint32_t wsLastUse = 0;
volatile bool wsEndWant = false;       // asked for from a web handler, done from loop
bool     wsSawUp   = false;            // the update screen has been up this session
#define WS_IDLE_S           600UL      // ten minutes unused, then off (7.4)
#define WS_SYNC_MS       150000UL      // the whole of a sync, joining included
#define WS_ASK_MS        180000UL      // an update question nobody answers
#define WS_HOTSPOT_IDLE_MS 600000UL
// Kept through deep sleep, lost on any real restart. That is the whole
// rule for when manual WiFi ends, carried by where the number lives.
RTC_DATA_ATTR uint32_t rtcWsUntil = 0;   // seconds on the system clock
RTC_DATA_ATTR uint8_t  rtcWsKind  = 0;

bool     syncRun = false, syncStarted = false, syncStoryDone = false, syncUpArmed = false;
uint32_t syncAt = 0;
bool     upQuick = false;              // a No ends the whole question
bool     upAfterJoin = false;          // Check update, waiting for a network
bool     upDirect = false;             // and go straight to the newest one
uint32_t lastSyncAt = 0;               // system clock seconds, 0 never
uint32_t askSince = 0;

// Commands that change the radios wait a moment, so the answer to the
// phone (clear that notification) goes out before the radio does.
enum { RQ_NONE = 0, RQ_SYNC, RQ_UPDATE, RQ_WIFI, RQ_HOTSPOT, RQ_TAMPER, RQ_DEEP, RQ_REBOOT };
int      rqPend = RQ_NONE;
uint32_t rqPendAt = 0;

// Weather from the phone or the last sync, kept in flash, because on
// Bluetooth deep sleep is every time the phone is not around.
uint32_t wxAt = 0;                     // system clock seconds it arrived
volatile bool wxDirty = false;
volatile bool netBusy = false;        // the network task is mid-fetch

// Notifications still land in the list, but wake nothing.
bool cfgQuiet = false;

// The phone guard. Going away from the phone and the phone going away
// from you are the same event, seen from here.
bool     cfgPGuard = false;
bool     pgEver = false;               // linked at least once since Bluetooth started
uint32_t pgLostAt = 0, pgWeakSince = 0, pgRssiAt = 0, pgUntil = 0;
float    pgRssi = 0;                   // smoothed; 0 means not measured yet
bool     pgFired = false;
const char* pgWhy = "";
#define PG_LOST_MS   4000UL
#define PG_WEAK_DBM  (-88)
#define PG_OK_DBM    (-80)
#define PG_WEAK_MS   6000UL
#define PG_SHOW_MS 120000UL

// Find me: the screen calls out for twenty seconds.
uint32_t findUntil = 0;

// Tamper. Armed, it goes dark and quiet and writes down what happens
// to it. Only a real restart disarms it.
uint32_t tamperCountAt = 0;            // the countdown, 0 when not counting
#define TAMPER_COUNT_MS 10000UL
RTC_DATA_ATTR uint8_t rtcTamper = 0;   // 1 armed, 2 pausing after a move
RTC_DATA_ATTR char    rtcTz[32] = "";
#define TLOG_PATH "/tamper.txt"
#define TLOG_MAX  4096
#define TL_N 24
char tlLines[TL_N][24];
int  tlN = 0, tlSel = 0;

// Light sleep. Only on a core built with power management; a stock
// core says no to esp_pm_configure and the robot behaves exactly as
// 5.20 did, off the moment the screen goes dark on Bluetooth.
bool pmAvail = false;
int  pmMode  = -1;
volatile uint32_t btDropAt = 0;        // when the phone last went
#define BT_DEEP_GRACE_MS 600000UL      // no phone for ten minutes, then off (6.4)

static bool wsStart(int kind);
static void wsEnd(const char* why);
static void wsTouch();
static void serviceWs();
static void syncBegin();
static void pocketTick();
static void blogTick();
static void blogSave();
static void blogReset();
static void blogBoot(bool fromDeep);
static void drawBattUse();
static void nightTick();
static void idleRhythm();
static void nightGo();
static void drawNightCard();
static long secsToNightEnd();
static void drawHub();
static bool isHub(int s);
static int hubCount(int s);
static void hubEnter(int hubScr, int sel);
static void uiHubCard(const char* name, const uint8_t* ic, const char* l1, const char* l2);
static void uiRow(int r, const uint8_t* ic, const char* label, const char* value, bool on, bool scrolled);
static void uiScroll(int first, int total);
static int uiFirst(int sel, int total);
static void gestTilt();
static void evtSend(const char* what);
static bool noteAllowed(const Note& n);
static bool noteVip(const Note& n);
static String lstJson();
static void filtersLoad();
static bool popRinging();
static void drawMac();
static int macItems(int* idx);
static bool cardsAny();
static void drawWalk();
static void drawBleOta();
static void otaStop(const char* why);
static void listSave(const char* key, char (*src)[20], int n);
static void appBang(char* c, uint16_t conn);
static void linkTick();
static void devLoad();
static bool anyLinked();
static void phoneFix();
static void devLabel(const uint8_t* a, const char* label, bool onlyIfDefault);
static const char* prefName(int k);
static void ringInit();
static void ringPx(int cx, int cy, int r, int k);
static void fmtDur(long s, char* b, size_t n);
static void fmtClock(time_t t, char* b, size_t n);
static void appTick();
static void cursorFeed(const char* b);
static String cfgJson();
static bool cfgApply(const String& k, int v);
static void rafiqPayload(const char* p, bool fresh);
static void tmrTick();
static void tmrTouch();
static void drawTimer();
static bool tmrCommand(const char* c);
static void tmrStop(const char* why);
static void awayDeepGo();
static void awayEyes(bool open);
static void awayText1(const char* t);
static void drawContact();
static void awayDeepWake(bool timer);
static void drawAway();
static void awayFlush(bool all);
static void awaySet(bool on, const char* text);
static void wsFailCard(const char* a, const char* b);
static bool rafiqTagged(const Note& n);
static void pmSet(bool idle);
static bool phoneHeld();
static void pgTick();
static void tlogAdd(const char* what);
static void tamperArm();
static void serviceTamper();
static void rafiqNote(const Note& n);
static bool rafiqIs(const Note& n);
static void saveWx();
static void tlogLoad();
static void drawTlog();
static void tlogDelete(int k);

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
         s == S_FOCUS || s == S_SETTINGS || s == S_REMIND ||
         s == S_MSG ||
         s == S_TODAY || s == S_FHUB || s == S_CALM;     // 7.6.1: the hubs have menus
}

// Which screens are worth offering at all. Weather needs the network
// and the vehicle screen is something you asked for; neither should sit
// in the carousel as a dead end.
static bool screenOn_(int s) {
  if (s == S_FOCUS)   return cardsAny();              // 7.5: the Mac screen, when it has something
  if (s == S_BIKE)    return cfgBike;
  if (s == S_WEATHER) return !offlineNow() || wxOk;   // the last report is still worth showing
  return true;
}
// The round, in order. Notifications come straight after home: they
// are the thing most often looked for. The enum keeps its numbers for
// the page and the Mac; only the walk changes.
// 7.6: five stops. Everything else lives inside a hub, which is what
// makes it quick to find: four choices to look at, not twelve.
const int S_ORDER[] = { S_HOME, S_TODAY, S_FHUB, S_CALM, S_SETTINGS };
#define S_ORDER_N (int)(sizeof(S_ORDER) / sizeof(S_ORDER[0]))
static bool cardsAny();
static int macItems(int* idx);
static bool prayerSoon(int mins);
static int nextScreen(int from) {
  // Home looks ahead: with a prayer close, Faith comes first.
  if (from == S_HOME && prayerSoon(15)) return S_FHUB;
  int at = 0;
  for (int i = 0; i < S_ORDER_N; i++) if (S_ORDER[i] == from) { at = i; break; }
  for (int i = 1; i <= S_ORDER_N; i++) {
    int s = S_ORDER[(at + i) % S_ORDER_N];
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

// Ignoring it gets you asked again, every fifteen minutes, and after
// seven goes it stops rather than following you round the house all
// evening.
//
// The gaps used to grow: three at five minutes, two at thirty, two at
// an hour. That was two different behaviours for one situation, since
// waving it away by hand already meant fifteen minutes, and an hour is
// long enough that the thing you were being reminded of has usually
// gone past. One interval, one meaning.
#define REM_STEPS 7
// A press is different. It means "I have seen it, not now", so it comes
// back sooner and does not count against the ladder: you can keep
// saying not now for as long as you like.
#define REM_WAVED_MIN 15
// How long a reminder holds the screen before it puts itself off.
#define REM_SHOW_MS 10000UL
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
       C_SHAKE, C_DEEP, C_WAKEH, C_BATT,
       C_PAIR, C_UPDATE,
       C_AUTOUP, C_RESET, C_REBOOT, C_ABOUT,
       C_GUARD, C_TAMPER, C_TLOG, C_HOLD, C_CLOCK, C_WAKEBY,
       C_MULTI, C_DEV1, C_DEV2, C_AUTOAWAY, C_NIGHT, C_BED,
       C_BLOG, C_BVIEW, C_BRESET, C_PLOCK, C_COUNT };
const char* C_NAME[C_COUNT] =
  { "Brightness", "Watch face", "Sleep after", "Page turn", "Popup time",
    "Eye style", "Prayer times", "Hijri shift", "Network", "Vehicle",
    "Hotspot", "Accelerometer", "Knocks", "Tap strength",
    "Go back by", "Power down", "Wake on hold", "Battery full",
    "Pair a Mac", "Update", "Auto update",
    "Reset settings", "Reboot", "About",
    "Phone guard", "Tamper alarm", "Tamper log", "Hold time", "Clock", "Wake by",
    "Multi-link", "Primary", "Second", "Auto away", "Night sleep", "Bedtime",
    "Battery log", "Battery use", "New log at", "Pocket lock" };

// ---------------- settings, in groups ----------------
//
//  Twenty four rows in one list is a list you scroll through hunting
//  for the thing you came for, and the thing you came for is usually
//  the network or the brightness, which were eight apart. Four groups
//  of five to eight is four words to read and then a short list.
//
//  itemIdx stays what it always was, a C_ index, so every case below
//  is untouched. What is new is which of them this group contains and
//  where you are in the group.
// SG_, not G_: the games already own G_.
//  Display first, and in the order the flat list had, because that is
//  what makes this strictly better rather than a trade. Grouping adds
//  one press to reach a group, so anything that was near the top of
//  the old list would have moved further away. Five of them did, until
//  the check said so. Like this, every single setting is the same
//  number of presses away or fewer, and the worst case goes from
//  twenty three to seven.
// 7.6: grouped by what you are trying to do. Prayer settings is not in
// the list: it is opened from Faith, next to the prayer times it changes.
enum { SG_DISPLAY = 0, SG_TOUCH, SG_CONN, SG_SAFE, SG_BATT, SG_SYSTEM, SG_COUNT,
       SG_FAITHSET = SG_COUNT, SG_ALL,
       SG_WIRELESS = SG_CONN, SG_DEVICES = SG_CONN, SG_CONTROLS = SG_TOUCH };
const char* SG_NAME[SG_ALL] = { "Display", "Touch and motion", "Connections",
                                "Away and safety", "Battery", "System", "Prayer settings" };

#define SG_MAX 8
#define SG_END 0xFF
const uint8_t SG_ROWS[SG_ALL][SG_MAX] = {
  { C_BRIGHT,   C_FACE,  C_CLOCK, C_SLEEP,  C_POPUP,  C_TURN,   C_EYES,  C_BIKE },
  { C_WAKEBY,   C_PLOCK, C_WAKEH, C_HOLD,   C_SHAKE,  C_KNOCK,  C_TAP,   C_ACCEL },
  { C_MODE,     C_MULTI, C_DEV1,  C_DEV2,   C_PAIR,   C_HOTSPOT, SG_END, SG_END },
  { C_AUTOAWAY, C_GUARD, C_TAMPER, C_TLOG,  SG_END,   SG_END,   SG_END,  SG_END },
  { C_BVIEW,    C_BLOG,  C_BRESET, C_BATT,  C_DEEP,   C_NIGHT,  C_BED,   SG_END },
  { C_UPDATE,   C_RESET, C_REBOOT, C_ABOUT, SG_END,   SG_END,   SG_END,  SG_END },
  { C_PRAYER,   C_HIJRI, SG_END,  SG_END,   SG_END,   SG_END,   SG_END,  SG_END },
};
static int sgLen(int g) {
  int n = 0; while (n < SG_MAX && SG_ROWS[g][n] != SG_END) n++; return n;
}
static int sgPos(int g, int item) {
  for (int i = 0; i < sgLen(g); i++) if (SG_ROWS[g][i] == item) return i;
  return 0;
}
static int sgNext(int g, int item) {
  return SG_ROWS[g][(sgPos(g, item) + 1) % sgLen(g)];
}
// Which list is on screen: -1 is the four groups, otherwise the rows
// of that one. Depth is left alone; it has been got wrong here before
// and the settings are the one screen that already uses three of it.
int setGrp = -1, grpSel = 0;

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

// The last time the clock was known to be right.
//
// A robot that has just come up has nothing to show and used to show a
// cartoon face, which tells you nothing and looks like a fault. This
// is something true instead.
//
// It is never fed back into the system clock and never sets timeOk. A
// stale time that reminders and prayer alerts believed would be worse
// than no time at all, so it is for the screen and nothing else.
RTC_DATA_ATTR time_t lastGoodEpoch = 0;
static bool lastGoodHM(char* o, size_t n) {
  if (lastGoodEpoch <= 0) return false;
  time_t e = lastGoodEpoch;
  struct tm t;
  if (!localtime_r(&e, &t)) return false;
  snprintf(o, n, "%02d:%02d", t.tm_hour, t.tm_min);
  return true;
}

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

// ================================================================
//  BLUETOOTH
// ================================================================
//  The phone instead of the network.
//
//  The robot advertises, you pair it once from Settings on the
//  iPhone, and from then on it borrows the phone's clock and the
//  phone's notifications. WiFi stays off the whole time.
//
//  This is NimBLE-Arduino, not the BLE library bundled with the
//  core, and the swap was forced. Apple's notification service
//  requires running a GATT client over the connection the PHONE
//  made to us, and the bundled wrapper cannot: its client only
//  dials out, and its server drops BLE_GAP_EVENT_NOTIFY_RX on the
//  floor because only its client class has a case for it. Every
//  notification would have arrived and been thrown away. NimBLE
//  hands out a client for an inbound connection, which is the
//  whole ballgame.
//
//  btStage is the reason the screen says what it says. None of this
//  can be tried from a desk, so instead of a light that is on or
//  off, it reports which rung it reached, and a failure says where.
enum { BT_OFF = 0, BT_ADVERTISING, BT_CONNECTED, BT_BONDED, BT_FAIL };
int      btStage = BT_OFF;
bool     btUp    = false;             // the stack is running
uint32_t btSince = 0;                 // when the rung last changed
uint16_t btConn  = 0xFFFF;            // the live connection, or none

static NimBLEUUID ANCS_SVC("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
static NimBLEUUID ANCS_NS ("9FBF120D-6301-42D9-8C58-25E699A21DBD");
static NimBLEUUID ANCS_CP ("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
static NimBLEUUID ANCS_DS ("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");
static NimBLEUUID CTS_SVC((uint16_t)0x1805);
static NimBLEUUID CTS_CHR((uint16_t)0x2A2B);

//  A mouse, so iOS Settings lists it at all. That page only shows
//  classic radios and the few standard profiles the system consumes
//  itself; a plain peripheral is invisible there however long you
//  look, which is what made 5.15.0 unfindable. A keyboard would be
//  listed too and is the wrong pick: iOS hides the on screen
//  keyboard while a hardware one is attached. No report is ever
//  sent, and iOS only draws a pointer when AssistiveTouch is on,
//  so it sits there bonded and silent.
static const uint8_t HID_MAP[] = {
  0x05, 0x01,        // usage page: generic desktop
  0x09, 0x02,        // usage: mouse
  0xA1, 0x01,        // collection: application
  0x85, 0x01,        //   report id 1
  0x09, 0x01,        //   usage: pointer
  0xA1, 0x00,        //   collection: physical
  0x05, 0x09,        //     usage page: buttons
  0x19, 0x01, 0x29, 0x03,
  0x15, 0x00, 0x25, 0x01,
  0x95, 0x03, 0x75, 0x01,
  0x81, 0x02,        //     three buttons
  0x95, 0x01, 0x75, 0x05,
  0x81, 0x01,        //     padding to a byte
  0x05, 0x01,        //     usage page: generic desktop
  0x09, 0x30, 0x09, 0x31,
  0x15, 0x81, 0x25, 0x7F,
  0x75, 0x08, 0x95, 0x02,
  0x81, 0x06,        //     x and y, relative
  0xC0,              //   end collection
  0xC0               // end collection
};

//  Soliciting Apple's notification service, as raw advertising data
//  because there is no setter for it. 0x15 is the solicitation list
//  for 128 bit UUIDs, and the sixteen bytes are the ANCS UUID
//  backwards, which is how they go on the air. This is the byte that
//  makes the iPhone offer to share its notifications; without it
//  there is no prompt and no access, whatever else you advertise.
static const uint8_t ANCS_SOLICIT[18] = {
  0x11, 0x15,
  0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,
  0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79
};

NimBLEHIDDevice*   btHid  = nullptr;
NimBLECharacteristic* btKeys = nullptr;    // never written, and that is fine
NimBLERemoteCharacteristic* ancsCP = nullptr;

static void btSet(int stage) {
  if (btStage == stage) return;
  // Linked and unlinked are news worth a word when someone is looking.
  // Set here, said from loop: this runs on the NimBLE task.
  if (stage == BT_BONDED) btNews = 1;
  else if (btStage == BT_BONDED && stage == BT_ADVERTISING) btNews = 2;
  btStage = stage;
  btSince = millis();
  Serial.printf("bluetooth: %s\n",
                stage == BT_OFF ? "off" : stage == BT_ADVERTISING ? "advertising"
              : stage == BT_CONNECTED ? "connected"
              : stage == BT_BONDED ? "bonded" : "would not start");
}

static const char* btShort() {
  switch (btStage) {
    case BT_ADVERTISING: return "pair me";
    case BT_CONNECTED:   return "linked";
    case BT_BONDED:      return "paired";
    case BT_FAIL:        return "no radio";
    default:             return "starting";
  }
}

// A count rather than a flag, and a longer window, because the first
// version only guarded the twelve seconds around startup. The GATT
// work below only begins when a phone connects, which on a bonded
// pair is seconds after boot but can be minutes: a crash there would
// loop just as tightly and the note would already have been torn up.
//
// Counting also means an ordinary power cut in the first minute is
// not mistaken for a crash. Three unproven starts in a row is a
// pattern; one is a Tuesday.
#define BT_PROVEN_MS 60000UL
#define BT_GIVE_UP        3

// And the general case, because the robot is a sealed box now and
// the cable is not an option any more.
//
// The Bluetooth count above only knows about Bluetooth. Anything else
// that panics on its way up would loop the same way and leave nothing
// to update over. A panic reboots the chip and sets the reset reason,
// so consecutive panics can be counted without writing to NVS on
// every ordinary boot: a clean start clears the count, and three
// panics in a row bring it up somewhere known to be reachable.
#define SAFE_AFTER 3
bool safeMode = false;
bool     btNoteOut = false;
uint32_t btNoteAt  = 0;
int      btTries   = 0;
bool     btFellBack = false;      // say so once, on the first screen

// Pairing takes as long as it takes you to find the Settings page,
// and the radio only runs while the robot is awake: nodding off part
// way through takes the thing you are hunting for off the air. So the
// panel stays up while it is advertising or mid handshake, and lets
// go once the bond is made or the window has gone by.
#define BT_PAIR_HOLD_MS 90000UL
static bool btPairing() {
  if (cfgNet != NET_BT || !btUp) return false;
  if (btStage == BT_CONNECTED) return true;      // a handshake in progress
  return btStage == BT_ADVERTISING && millis() - btSince < BT_PAIR_HOLD_MS;
}

// ---- the clock from the phone, and the notifications: state ----
//
//  Up here only because the callbacks below have to be able to throw
//  it all away. The working parts are further down, past the point
//  where timeOk and clockSrc exist.
enum { CTS_IDLE = 0, CTS_DONE, CTS_NONE };
int      ctsState = CTS_IDLE;
uint32_t ctsAskedAt = 0, ctsSyncedAt = 0;
int      ctsFails = 0;

enum { ANCS_NONE = 0, ANCS_WAIT, ANCS_READY, ANCS_FAIL };
int      ancsState = ANCS_NONE;
int      ancsTries = 0;
uint32_t ancsLastTry = 0, ancsAskedAt = 0, btSecAskedAt = 0;
bool     ancsBusy = false;             // one attribute request at a time

//  What a notification is, once the phone has told us.
//
//  Twenty of them, newest first. Apple hands over an id, a category
//  and then, only if you ask, the app, the title and the body. The
//  id is what you quote back to dismiss it or answer a call.
#define NOTE_MAX 20
// How many attributes ancsAsk requests and onDataSource expects back.
#define ANCS_ATTRS 5
Note notes[NOTE_MAX];
int  noteN = 0;                        // in use, notes[0] is the newest
int  noteIdx = 0;                      // which one is being read
bool noteConfirm = false, noteYes = false;
uint32_t noteTotal = 0;

//  Apple's categories. Only the ones worth saying out loud are
//  named; the rest show the app, which is more use than "other".
#define CAT_CALL    1
#define CAT_MISSED  2
#define CAT_VOICE   3

//  The host task produces, loop() consumes. One writer and one
//  reader each side on a single core, so byte indices need no lock.
#define UIDQ_N 12
volatile uint32_t uidQ[UIDQ_N];
volatile uint8_t  uidQCat[UIDQ_N];
volatile uint8_t  uidHead = 0, uidTail = 0;

//  The data source answer is assembled on the host task and handed
//  over whole, so the screen never sees half a notification.
Note     noteStage;
volatile bool noteReady = false;
volatile bool btWokeReq = false;       // a connection wants the screen
uint8_t  dsBuf[768];
//  The longer parts of whatever is being parsed. The list keeps a
//  hundred characters of a message; a RAFIQ command or a weather
//  report from a Shortcut needs the whole of it, and when it was sent.
char     stMsg[404];
char     stSub[34];
char     stDate[20];
size_t   dsLen = 0;
uint8_t  dsCat = 0;

static int linkFind(uint16_t h) { for (int i = 0; i < linkN; i++) if (links[i].h == h) return i; return -1; }
static void linkDrop(uint16_t h) {
  int i = linkFind(h);
  if (i < 0) return;
  for (int k = i; k < linkN - 1; k++) links[k] = links[k + 1];
  linkN--;
}
// Everything that was set up on the phone's link belongs to that link.
static void phoneReset(uint16_t h) {
  btConn = h;
  btClStale = true; btApp = false;
  ctsState = CTS_IDLE; ctsSyncedAt = 0; ctsFails = 0;
  ancsState = (h == 0xFFFF) ? ANCS_NONE : ANCS_WAIT; ancsTries = 0;
  ancsCP = nullptr; ancsBusy = false;
  uidHead = uidTail = 0; dsLen = 0;
}

class BtServerCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* sv, NimBLEConnInfo& ci) override {
    uint16_t h = ci.getConnHandle();
    if (linkN < 3) { links[linkN].h = h; memset(links[linkN].a, 0, 6); links[linkN].authed = false; linkN++; }
    if (btConn == 0xFFFF) {                       // the first link is the phone, until told otherwise
      phoneReset(h);
      btSet(ci.isEncrypted() ? BT_BONDED : BT_CONNECTED);
      btWokeReq = true;
    } else {
      btConn2 = h;                                // a companion
    }
    // Ask for the encryption ourselves rather than waiting to be
    // asked; with nothing demanding it the bond never completed.
    btSecAskedAt = millis();
    NimBLEDevice::startSecurity(h);
    // With Multi-link on, keep a door open: one more can come in, and
    // loop decides who stays (two at most, preferred devices first).
    if (cfgMulti && linkN < 2) NimBLEDevice::startAdvertising();   // a second is welcome; more is loop's call
  }
  void onDisconnect(NimBLEServer* sv, NimBLEConnInfo& ci, int reason) override {
    uint16_t h = ci.getConnHandle();
    bool wasComp = (h == btConn2);       // 7.4: the companion, not the phone
    linkDrop(h);
    if (h == btConn2) btConn2 = 0xFFFF;
    if (h == btConn) {
      btDropAt = millis();
      // The phone went. If a companion is still here, it becomes the
      // phone, so the clock and the rest carry on from it.
      uint16_t other = btConn2;
      btConn2 = 0xFFFF;
      phoneReset(other);
      if (other != 0xFFFF) btSet(BT_BONDED);
      else                 btSet(BT_ADVERTISING);
    }
    // Straight back to advertising, or the phone has nothing to come
    // back to and you would be pairing it by hand every time.
    if (linkN < (cfgMulti ? 2 : 1)) { advFast(); NimBLEDevice::startAdvertising(); }
    // A companion gone takes gesture mode with it.
    if (wasComp) {
      cfgGesture = false; knobOn = false; macDim = false;
      // 7.5: gone while awake, without saying goodnight: left behind.
      if (walkOn && !macBye) { walkUntil = millis() + 60000UL; btWokeReq = true; }
      macBye = false;
    }
    if (otaOn && h == otaConn) { otaErr = true; }
  }
  void onAuthenticationComplete(NimBLEConnInfo& ci) override {
    uint16_t h = ci.getConnHandle();
    if (!ci.isEncrypted()) {
      Serial.println("bluetooth: pairing did not take");
      if (h == btConn) btSet(BT_CONNECTED);
      return;
    }
    int i = linkFind(h);
    if (i >= 0) { memcpy(links[i].a, ci.getIdAddress().getVal(), 6); links[i].authed = true; }
    uint8_t nx = (authHead + 1) % 4;
    if (nx != authTail) { authQ[authHead] = h; authHead = nx; }
    // 90 to 120 ms, may skip 4, 6 s to give up: Apple's own guidance,
    // and most of the radio's time asleep. Following the pointer asks
    // for a quicker one while it runs.
    NimBLEDevice::getServer()->updateConnParams(h, 72, 96, 4, 600);
    // "Our services may have changed: read them again." Without this a
    // bonded Mac keeps the list it saw before an update, and new
    // channels (7.4's events, pointer, settings) never appear to it.
    ble_svc_gatt_changed(0x0001, 0xFFFF);
    if (h == btConn) btSet(BT_BONDED);
  }
};

// ---- the device list, kept in flash ----
static void devSave() {
  prefs.putBytes("devs", devs, sizeof(DevRec) * devN);
  prefs.putInt("devn", devN);
  prefs.putBytes("prefa", prefA, sizeof(prefA));
}
static void devLoad() {
  devN = constrain(prefs.getInt("devn", 0), 0, DEV_N);
  if (devN) prefs.getBytes("devs", devs, sizeof(DevRec) * devN);
  memset(prefA, 0, sizeof(prefA));
  if (prefs.isKey("prefa")) prefs.getBytes("prefa", prefA, sizeof(prefA));
}
static int devFind(const uint8_t* a) { for (int i = 0; i < devN; i++) if (!memcmp(devs[i].a, a, 6)) return i; return -1; }
static int devSeen(const uint8_t* a) {
  int i = devFind(a);
  if (i >= 0) return i;
  if (devN == DEV_N) { for (int k = 0; k < DEV_N - 1; k++) devs[k] = devs[k + 1]; devN--; }   // forget the oldest
  i = devN++;
  memcpy(devs[i].a, a, 6);
  snprintf(devs[i].label, sizeof(devs[i].label), "Device %02X%02X", a[1], a[0]);
  devSave();
  return i;
}
static void devLabel(const uint8_t* a, const char* label, bool onlyIfDefault) {
  int i = devSeen(a);
  if (onlyIfDefault && strncmp(devs[i].label, "Device ", 7)) return;
  snprintf(devs[i].label, sizeof(devs[i].label), "%.17s", label);
  devSave();
}
static int prefRank(const uint8_t* a) {        // 1 primary, 2 second, 0 neither
  static const uint8_t zero[6] = { 0 };
  for (int k = 0; k < 2; k++) if (memcmp(prefA[k], zero, 6) && !memcmp(prefA[k], a, 6)) return k + 1;
  return 0;
}
static const char* prefName(int k) {
  static const uint8_t zero[6] = { 0 };
  // "any" and "nothing has ever linked" looked identical, so pressing
  // the row and watching it stay on "any" told you nothing about why.
  if (!devN) return "none seen";
  if (!memcmp(prefA[k], zero, 6)) return "any";
  int i = devFind(prefA[k]);
  return i < 0 ? "?" : devs[i].label;
}
static bool linkIsMac(int i) {
  if (i < 0 || !links[i].authed) return false;
  int d = devFind(links[i].a);
  return d >= 0 && !strncmp(devs[d].label, "Mac", 3);
}
// The phone is whoever can be one: not a Mac. If a Mac holds the
// phone's place and anything else is linked, they swap.
static void phoneFix() {
  int pi = linkFind(btConn);
  if (pi < 0 || !linkIsMac(pi)) return;
  for (int k = 0; k < linkN; k++) {
    if (links[k].h == btConn || !links[k].authed || linkIsMac(k)) continue;
    uint16_t mac = btConn;
    phoneReset(links[k].h);
    btConn2 = mac;
    btSince = millis();
    btSet(BT_BONDED);
    Serial.println("links: the Mac is a companion; the other device is the phone");
    return;
  }
}
// Is one of the links a Mac? macLinked is the old WiFi answer and says
// nothing about Bluetooth, which is where a Mac actually lives now.
// A story is being sent on this link, so it was put on the quick
// rhythm. Put the ordinary one back when it is over, however it ended.
uint16_t rdConn = 0xFFFF;
static void rdRhythmBack() {
  NimBLEServer* sv = NimBLEDevice::getServer();
  if (sv && rdConn != 0xFFFF) sv->updateConnParams(rdConn, 72, 96, 4, 600);
  rdConn = 0xFFFF;
}

static bool macOnBle() {
  for (int i = 0; i < linkN; i++) if (links[i].authed && linkIsMac(i)) return true;
  return false;
}

static bool anyLinked() {
  for (int i = 0; i < linkN; i++) if (links[i].authed) return true;
  return false;
}

// Loop's half: who stays, and who is the phone.
static void linkTick() {
  while (authTail != authHead) {
    uint16_t h = authQ[authTail]; authTail = (authTail + 1) % 4;
    int i = linkFind(h);
    if (i < 0) continue;
    btEverLinked = true;
    devSeen(links[i].a);
    int rank = prefRank(links[i].a);
    NimBLEServer* sv = NimBLEDevice::getServer();
    // More than two: the one to let go is a device nobody preferred,
    // the newcomer itself if it is that, or else the other one.
    if (linkN > 2 && sv) {
      int victim = -1;
      if (rank == 0) victim = i;
      else for (int k = 0; k < linkN; k++) if (k != i && links[k].authed && prefRank(links[k].a) == 0) { victim = k; break; }
      if (victim < 0) victim = i;
      Serial.printf("links: three, letting %04X go\n", links[victim].h);
      sv->disconnect(links[victim].h);
      if (victim == i) {
        // Turned away: keep the door shut a minute, or a phone that
        // reconnects by itself would be in and out all day.
        doorPauseUntil = millis() + 60000;
        NimBLEDevice::stopAdvertising();
        continue;
      }
    }
    // The preferred phone always becomes the phone (but never a Mac).
    if (rank == 1 && h != btConn && !linkIsMac(i)) {
      uint16_t old = btConn;
      phoneReset(h);
      btConn2 = old;
      btSince = millis();
      btSet(BT_BONDED);
      Serial.println("links: the primary device is the phone now");
    }
    if (!cfgMulti && linkN > 1 && sv && h != btConn) sv->disconnect(h);   // one at a time when Multi-link is off
  }
  phoneFix();
  // Keep the door open. Asking once, from inside the connect callback,
  // was not enough: when that one request did not take, nothing asked
  // again, and the second device could never get in. Once a second is
  // plenty; nothing else here is that cheap to check.
  static uint32_t advCheckAt = 0;
  if (btUp && cfgNet == NET_BT && millis() - advCheckAt > 1000) {
    advCheckAt = millis();
    bool want = (linkN == 0) || (cfgMulti && linkN < 2 && (int32_t)(millis() - doorPauseUntil) > 0);
    NimBLEAdvertising* adv0 = NimBLEDevice::getAdvertising();
    if (want && !adv0->isAdvertising()) {
      NimBLEDevice::startAdvertising();
      Serial.printf("links: %d linked, advertising again\n", linkN);
    }
  }
  // 7.4: two in, nothing goes out. Not even listening for a third.
  if (btUp && linkN >= 2) {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (adv->isAdvertising()) NimBLEDevice::stopAdvertising();
  }
  // Fast for half a minute after a link goes (so it comes straight
  // back), then slow: a phone that is away does not need hearing ten
  // times a second, and the battery does.
  if (btUp && !advSlow && (int32_t)(millis() - advFastUntil) > 0) {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    bool was = adv->isAdvertising();
    if (was) NimBLEDevice::stopAdvertising();
    adv->setMinInterval(874); adv->setMaxInterval(874);   // 546.25 ms
    advSlow = true;
    if (was) NimBLEDevice::startAdvertising();
  }
}


//  Both of these run on the NimBLE host task, so they do nothing but
//  write down what arrived. Drawing or sleeping from here is how you
//  get a crash that only happens when a phone is nearby.
static void onNotifSource(NimBLERemoteCharacteristic* c, uint8_t* d,
                          size_t len, bool isNotify) {
  if (len < 8) return;
  uint8_t evt = d[0], flags = d[1], cat = d[2];
  uint32_t uid = d[4] | (d[5] << 8) | (d[6] << 16) | ((uint32_t)d[7] << 24);
  if (evt == 0 && !(flags & 0x04)) {           // added, and not pre existing
    uint8_t nx = (uidHead + 1) % UIDQ_N;
    if (nx == uidTail) return;                 // full; drop the oldest ask
    uidQ[uidHead] = uid; uidQCat[uidHead] = cat;
    uidHead = nx;
  }
}

static void onDataSource(NimBLERemoteCharacteristic* c, uint8_t* d,
                         size_t len, bool isNotify) {
  if (dsLen + len > sizeof(dsBuf)) dsLen = 0;
  memcpy(dsBuf + dsLen, d, len); dsLen += len;
  if (dsLen < 5 || dsBuf[0] != 0) return;
  Note n = {};
  n.uid = dsBuf[1] | (dsBuf[2] << 8) | (dsBuf[3] << 16) | ((uint32_t)dsBuf[4] << 24);
  n.cat = dsCat;
  stMsg[0] = stSub[0] = stDate[0] = 0;
  size_t p = 5;
  // Exactly as many as ancsAsk asked for. The number has to match or
  // nothing ever arrives: loop one too many and the last turn runs
  // off the end of a complete answer and returns as though more were
  // coming, so the notification is parsed and then thrown away.
  for (int a = 0; a < ANCS_ATTRS; a++) {
    if (p + 3 > dsLen) return;                 // more is still coming
    uint8_t id = dsBuf[p];
    uint16_t L = dsBuf[p + 1] | (dsBuf[p + 2] << 8);
    if (p + 3 + L > dsLen) return;
    char*  dst = nullptr; size_t cap = 0;
    if      (id == 0) { dst = n.app;   cap = sizeof(n.app); }
    else if (id == 1) { dst = n.title; cap = sizeof(n.title); }
    else if (id == 2) { dst = stSub;   cap = sizeof(stSub); }
    else if (id == 3) { dst = stMsg;   cap = sizeof(stMsg); }
    else if (id == 5) { dst = stDate;  cap = sizeof(stDate); }
    if (dst) { size_t k = L < cap - 1 ? L : cap - 1; memcpy(dst, dsBuf + p + 3, k); dst[k] = 0; }
    if (id == 3) { strncpy(n.msg, stMsg, sizeof(n.msg) - 1); n.msg[sizeof(n.msg) - 1] = 0; }
    p += 3 + L;
  }
  n.unread = true;
  noteStage = n;
  noteReady = true;
  dsLen = 0;
  ancsBusy = false;
}

//  Once per connection, never more. NimBLEServer::getClient() deletes
//  everything it has discovered every time it is called, so calling it
//  for the clock after the notifications were set up threw the
//  notification subscriptions away (nothing ever arrived) and left
//  ancsCP pointing at freed memory (the occasional reboot). C3 Buddy
//  learned this first; this is its rule. (btCl and btClStale are
//  declared with the 6.0 globals, because the disconnect callback,
//  further up, has to mark it stale.)
static NimBLEClient* btPeer() {
  NimBLEServer* sv = NimBLEDevice::getServer();
  if (!sv || btConn == 0xFFFF) return nullptr;
  if (btClStale || !btCl) { btCl = sv->getClient(btConn); btClStale = false; }
  return btCl;
}

// NimBLE's task only queues; loop does the work, as for ANCS.
class RqChrCb : public NimBLECharacteristicCallbacks {
 public:
  explicit RqChrCb(uint8_t k) : kind(k) {}
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
    if (kind == 9) {                     // 7.5: firmware bytes, straight to flash
      if (!otaOn || otaErr) return;
      NimBLEAttValue v = c->getValue();
      size_t n = v.length();
      if (otaGot + n > otaSize || Update.write((uint8_t*)v.data(), n) != n) { otaErr = true; return; }
      otaGot += n; otaLastAt = millis();
      return;
    }
    if (kind == 5) {                     // the pointer: no queue, ten a second
      NimBLEAttValue v = c->getValue();
      char b[24]; size_t n = v.length() < sizeof(b) - 1 ? v.length() : sizeof(b) - 1;
      memcpy(b, v.data(), n); b[n] = 0;
      cursorFeed(b);
      return;
    }
    if (ci.getConnHandle() == btConn) btApp = true;   // the phone runs the app (a companion's writes do not count)
    uint8_t next = (appHead + 1) % APPQ_N;
    if (next == appTail) return;       // full: drop, the app will resend
    AppMsg& m = appQ[appHead];
    NimBLEAttValue v = c->getValue();
    size_t n = v.length() < sizeof(m.data) - 1 ? v.length() : sizeof(m.data) - 1;
    memcpy(m.data, v.data(), n); m.data[n] = 0;
    m.len = (uint16_t)n; m.kind = kind; m.conn = ci.getConnHandle();
    appHead = next;
  }
  void onRead(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
    if (kind == 6) { String j = cfgJson(); c->setValue((const uint8_t*)j.c_str(), j.length()); return; }
    if (kind == 8) { String j = lstJson(); c->setValue((const uint8_t*)j.c_str(), j.length()); return; }
    char b[420];
    long tl = (tmrOn && !tmrDone) ? (long)(tmrEnd - millis()) / 1000 : 0;
    if (tl < 0) tl = 0;
    snprintf(b, sizeof(b),
             "fw=%s;bat=%d;away=%d;timer=%ld;unread=%d;quiet=%d;guard=%d;wake=%d;h12=%d;clock=%d;rxc=%u;rxn=%u;relax=%d;follow=%d;gest=%d;knob=%d;walk=%d;v=%.2f;ls=%d;lon=%lu;ldk=%lu;lls=%lu;ldp=%lu;lwf=%lu;lwk=%lu;lrs=%lu;lst=%lu;lp0=%d;rds=%d;last=%s",
             FW_VERSION, isnan(battV) ? -1 : battPct(battV), awayOn ? 1 : 0, tl,
             noteUnread(), cfgQuiet ? 1 : 0, cfgPGuard ? 1 : 0, cfgWakeBy, cfg12h ? 1 : 0,
             timeOk ? 1 : 0, (unsigned)appRxCmd, (unsigned)appRxNote,
             relaxOn ? 1 : 0, cfgFollow ? 1 : 0, cfgGesture ? 1 : 0, knobOn ? 1 : 0, walkOn ? 1 : 0,
             isnan(battV) ? 0.0f : battV, pmMode == 1 ? 1 : 0,
             (unsigned long)blog.sOn, (unsigned long)blog.sDark, (unsigned long)blog.sLight,
             (unsigned long)blog.sDeep, (unsigned long)blog.sWifi, (unsigned long)blog.wakes,
             (unsigned long)blog.restarts, (unsigned long)blog.start, (int)blog.pct0,
             readCount, appLast);
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
  rqEvt = svc->createCharacteristic(RQ_EVT, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY);
  svc->createCharacteristic(RQ_PTR, NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC)
     ->setCallbacks(new RqChrCb(5));
  svc->createCharacteristic(RQ_CFG, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC)
     ->setCallbacks(new RqChrCb(6));
  svc->createCharacteristic(RQ_LST, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC)
     ->setCallbacks(new RqChrCb(8));
  svc->createCharacteristic(RQ_OTA, NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC)
     ->setCallbacks(new RqChrCb(9));
}

// What the app sent, handled on loop.
static void appTick() {
  while (appTail != appHead) {
    AppMsg& m = appQ[appTail];
    if (m.kind == 1) {                             // a command
      Serial.printf("app: %s\n", m.data);
      appRxCmd++;
      snprintf(appLast, sizeof(appLast), "%.15s", m.data);
      for (char* q = appLast; *q; q++) if (*q == ';' || *q == '=') *q = ' ';
      if (!strcmp(m.data, "ping")) { }             // the app saying hello
      else if (m.data[0] == '!') appBang(m.data, m.conn);   // 7.4: what the apps' HTTP calls did
      else if (!strncmp(m.data, "iam ", 4)) {      // the app saying who it is
        int li = linkFind(m.conn);
        // Every characteristic here needs an encrypted link, so a write
        // arriving at all is the proof that the link is encrypted.
        // Waiting for our own authed flag to agree threw the name away
        // whenever iam arrived first, which on a reconnect to a Mac
        // that is already bonded is exactly what happens. The Mac was
        // then never remembered, never appeared in the device list, and
        // Second had nothing to offer but "any".
        if (li >= 0 && !links[li].authed) {
          NimBLEServer* sv = NimBLEDevice::getServer();
          if (sv) {
            NimBLEConnInfo ci = sv->getPeerInfoByHandle(m.conn);
            if (ci.isEncrypted()) {
              memcpy(links[li].a, ci.getIdAddress().getVal(), 6);
              links[li].authed = true;
              devSeen(links[li].a);
            }
          }
        }
        if (li >= 0 && links[li].authed) devLabel(links[li].a, m.data + 4, false);
        phoneFix();
        appRxCmd--;                                 // not a command anyone sent
      }
      else rafiqPayload(m.data, true);             // fresh, it was sent just now
    } else if (m.kind == 2) {                      // a notification
      appRxNote++;
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
        if (noteAllowed(n)) {
          addNote(n);
          if (!cfgQuiet || noteVip(n)) popupShow();
        }
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

static void bleOn() {
  if (btUp) return;
  Serial.println("bluetooth: starting");
  btTries = prefs.getInt("btry2", 0) + 1;
  prefs.putInt("btry2", btTries);      // zeroed once it has plainly held
  btNoteOut = true; btNoteAt = millis();

  char nm[24];
  snprintf(nm, sizeof(nm), "Rafiq %s", cfgName);
  NimBLEDevice::init(nm);
  NimBLEDevice::setSecurityAuth(true, false, true);   // bond, no MITM, secure
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  NimBLEServer* sv = NimBLEDevice::createServer();
  sv->setCallbacks(new BtServerCb());

  btHid = new NimBLEHIDDevice(sv);
  btHid->setManufacturer("Rafiq");
  btHid->setPnp(0x02, 0xE502, 0xA111, 0x0210);
  btHid->setHidInfo(0x00, 0x02);       // not localised, remote wakeable
  btHid->setReportMap((uint8_t*)HID_MAP, sizeof(HID_MAP));
  btKeys = btHid->getInputReport(1);
  if (!isnan(battV)) btHid->setBatteryLevel(battPct(battV));
  rqServiceAdd(sv);                    // 7.2: for the Android app, and the Mac app later
  sv->start();                         // starts the services too

  // 3 flags + 18 solicitation + 4 appearance + 4 HID = 29 of 31. The
  // name will not fit beside them and goes in the scan response,
  // which iOS asks for anyway because it scans actively.
  NimBLEAdvertisementData ad;
  ad.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  ad.addData(ANCS_SOLICIT, sizeof(ANCS_SOLICIT));
  ad.setAppearance(0x03C2);            // a mouse, so Settings lists it
  ad.addServiceUUID(NimBLEUUID((uint16_t)0x1812));
  NimBLEAdvertisementData sr;
  sr.setName(nm);

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setAdvertisementData(ad);
  adv->setScanResponseData(sr);
  advFast();

  btUp = true;
  // start() says whether the controller took it. An earlier build
  // called a function that returns nothing and then put "pair me" on
  // the screen whatever had happened. A screen that cannot be wrong
  // about this is the whole point of the rungs.
  if (adv->start()) btSet(BT_ADVERTISING);
  else              btSet(BT_FAIL);
}

// bleOn and bleOff, not btStart and btStop: the core already owns
// those two, for a classic controller the C3 does not have.
static void bleOff() {
  btCl = nullptr; btClStale = true;   // deinit deletes it
  if (!btUp) return;
  Serial.println("bluetooth: stopping");
  NimBLEDevice::deinit(true);
  btUp = false;
  btConn = 0xFFFF;
  btHid = nullptr; btKeys = nullptr; ancsCP = nullptr;
  ancsState = ANCS_NONE; ctsState = CTS_IDLE;
  uidHead = uidTail = 0; dsLen = 0; noteReady = false;
  prefs.putInt("btry2", 0);            // stopped on purpose, not a crash
  btNoteOut = false;
  btSet(BT_OFF);
}

// ---------------- gesture mode ----------------
//
//  Switched on from the Mac. While it is on the pad stops driving the
//  robot and starts driving the Mac: one press and two presses are
//  sent straight across and Rafiq decides what they mean, which may
//  be different in every application you have in front of you.
//
//  It is not remembered anywhere. There is nothing it can usefully do
//  without a Mac on the other end, so it comes up off, the app turns
//  it on when it finds the robot, and it turns itself off the moment
//  the Mac goes quiet. Quitting Rafiq is therefore enough to get your
//  robot back, and so is holding the pad.
bool      cfgGesture = false;
// What drives it: a knock on the desk, the pad, or either.
//
// Either is the awkward one and the reason this guard exists at all.
// Pressing a pad glued to a small light robot knocks the small light
// robot, and the knock lands first, on the press, while the press
// itself is not resolved until you lift. So with both switched on a
// single press would reach the Mac twice. A knock that arrives with a
// finger on the pad, or just after one left it, is that finger.
enum { GSRC_KNOCK = 0, GSRC_TOUCH, GSRC_BOTH, GSRC_N };
int       cfgGestSrc = GSRC_KNOCK;
static bool gestByKnock() { return cfgGestSrc != GSRC_TOUCH; }
static bool gestByTouch() { return cfgGestSrc != GSRC_KNOCK; }
bool      gestMuted  = false;        // what Rafiq says the mic is doing
IPAddress macAddr;                   // learned from the app's own calls
WiFiUDP   tapUdp;
#define TAP_PORT 4211
// How long the panel stays lit in gesture mode before it rests. The
// sleep setting is about a robot you are looking at; this is not one.
#define GESTURE_DARK_MS 10000UL
// Presses are sent, not polled for. The Mac asks the robot how it is
// every ten seconds, which is fine for "are you there" and useless
// for a press: you would tap and wait. This goes the other way and
// arrives in the time it takes a packet to cross the room.
// 7.5: anything the apps should hear: gestures, a task done, a prayer,
// update progress. Bonded and encrypted, so no token travels with it.
static void evtSend(const char* what) {
  if (rqEvt && linkN) { rqEvt->setValue((const uint8_t*)what, strlen(what)); rqEvt->notify(); }
}
static void sendTap(const char* what) {
  if (!cfgGesture) return;
  evtSend(what);
  if (!macLinked || !online() || macAddr == IPAddress()) return;
  // The token goes with it. A packet that moves your pointer and
  // presses your keys is a packet anyone on the network could send,
  // so Rafiq checks both who it came from and that it knew the word.
  tapUdp.beginPacket(macAddr, TAP_PORT);
  tapUdp.print(cfgTok);
  tapUdp.print(' ');
  tapUdp.print(what);
  tapUdp.endPacket();
  Serial.printf("gesture %s -> %s\n", what, macAddr.toString().c_str());
}
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
// Zero, so the first pass through the loop that has a network asks.
unsigned long nextResync = 0;
#define TIME_RESYNC_MS (6UL * 3600000UL)
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

// ================================================================
//  THE CLOCK AND THE NOTIFICATIONS, FROM THE PHONE
//
//  iOS serves the Current Time Service, and Apple's Notification
//  Centre Service, to anything it has bonded with. Both run as GATT
//  client work over the connection the PHONE made to us, which is
//  the reason this file uses NimBLE: its server will hand out a
//  client for an inbound connection, and nothing else here would.
//
//  Everything in here runs on the main task. The two callbacks that
//  do not are above, and they only ever write something down.
// ================================================================
#define CTS_SETTLE_MS   2000UL         // let the bond finish first
#define CTS_RETRY_MS   15000UL
#define CTS_REFRESH_MS 21600000UL      // six hours
#define CTS_GIVE_UP         5          // tries before it stops pestering
#define ANCS_SETTLE_MS  1500UL
#define ANCS_GIVE_UP        8
#define ANCS_ANSWER_MS  3000UL
#define BT_SEC_NUDGE_MS 6000UL

// newlib here has no timegm, and mktime would read the timezone,
// which is the one thing this must not do. Days from the civil
// epoch, which is what every timegm is underneath.
static time_t utcFromTm(const struct tm* t) {
  int y = t->tm_year + 1900;
  unsigned m = (unsigned)t->tm_mon + 1, d = (unsigned)t->tm_mday;
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = (long)era * 146097 + (long)doe - 719468;
  return (time_t)days * 86400L + t->tm_hour * 3600L + t->tm_min * 60L + t->tm_sec;
}

static bool applyCts(const uint8_t* b, size_t n) {
  if (!b || n < 7) return false;
  struct tm t = {};
  t.tm_year = (b[0] | (b[1] << 8)) - 1900;
  t.tm_mon  = b[2] - 1;
  t.tm_mday = b[3];
  t.tm_hour = b[4];
  t.tm_min  = b[5];
  t.tm_sec  = b[6];
  // A phone that answers with nonsense is worse than one that does
  // not answer at all, because the reminders would believe it.
  if (t.tm_year < 120 || t.tm_mon < 0 || t.tm_mon > 11 ||
      t.tm_mday < 1 || t.tm_mday > 31 || t.tm_hour > 23 ||
      t.tm_min > 59 || t.tm_sec > 60) return false;
  struct timeval tv = { .tv_sec = utcFromTm(&t), .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  // What CTS hands over is already the phone's own wall clock, so the
  // zone stays at UTC and what comes back out of localtime is what
  // the phone is showing. The http date path does the same.
  setenv("TZ", "UTC0", 1); tzset();
  timeOk   = true;
  clockSrc = "your phone";
  Serial.println("clock: set from the phone");
  return true;
}

static bool readCts() {
  NimBLEClient* cl = btPeer();
  if (!cl) return false;
  NimBLERemoteService* svc = cl->getService(CTS_SVC);
  if (!svc) return false;
  NimBLERemoteCharacteristic* ch = svc->getCharacteristic(CTS_CHR);
  if (!ch) return false;
  NimBLEAttValue v = ch->readValue();
  return applyCts(v.data(), v.size());
}

static bool setupAncs() {
  NimBLEClient* cl = btPeer();
  if (!cl) return false;
  NimBLERemoteService* svc = cl->getService(ANCS_SVC);
  if (!svc) return false;               // the prompt has not been answered yet
  NimBLERemoteCharacteristic* ns = svc->getCharacteristic(ANCS_NS);
  NimBLERemoteCharacteristic* ds = svc->getCharacteristic(ANCS_DS);
  ancsCP = svc->getCharacteristic(ANCS_CP);
  if (!ns || !ds || !ancsCP) return false;
  if (!ds->subscribe(true, onDataSource))   return false;
  if (!ns->subscribe(true, onNotifSource))  return false;
  return true;
}

//  Ask for the parts worth showing. The caps are the buffers these
//  land in, so the phone truncates rather than us.
static void ancsAsk(uint32_t uid) {
  if (!ancsCP) return;
  uint8_t cmd[] = { 0x00,
    (uint8_t)uid, (uint8_t)(uid >> 8), (uint8_t)(uid >> 16), (uint8_t)(uid >> 24),
    0x00,                               // app identifier   )
    0x01, (uint8_t)(sizeof(((Note*)0)->title) - 2), 0,   //  )
    0x02, (uint8_t)(sizeof(stSub) - 2), 0,                //  ) ANCS_ATTRS
    0x03, (uint8_t)((sizeof(stMsg) - 4) & 0xFF),          //  ) of them
          (uint8_t)((sizeof(stMsg) - 4) >> 8),
    0x05 };                             // the date, for RAFIQ
  dsLen = 0; ancsBusy = true; ancsAskedAt = millis();
  if (!ancsCP->writeValue(cmd, sizeof(cmd), true)) ancsBusy = false;
}

//  0 is the positive action, 1 the negative one. On a call that is
//  answer and decline; on anything else it is usually open and clear.
static void ancsAction(uint32_t uid, uint8_t action) {
  if (ancsState != ANCS_READY || !ancsCP) return;
  uint8_t cmd[] = { 0x02,
    (uint8_t)uid, (uint8_t)(uid >> 8), (uint8_t)(uid >> 16), (uint8_t)(uid >> 24),
    action };
  ancsCP->writeValue(cmd, sizeof(cmd), true);
}

//  Newest first, and never the same one twice: iOS re-announces a
//  notification when its badge changes, and a list that grew every
//  time would be nothing but duplicates.
// The newest one that landed while the robot was asleep, so waking it
// yourself can show you what you missed rather than Home.
uint32_t noteSleptOn = 0;

static void addNote(const Note& n) {
  notesDirty = true;
  if (asleep) noteSleptOn = n.uid;
  for (int i = 0; i < noteN; i++) {
    if (notes[i].uid == n.uid) { notes[i] = n; notes[i].at = millis(); return; }
  }
  if (noteN < NOTE_MAX) noteN++;
  for (int i = noteN - 1; i > 0; i--) notes[i] = notes[i - 1];
  notes[0] = n;
  notes[0].at = millis();
  noteTotal++;
}

static int noteUnread() {
  int k = 0;
  for (int i = 0; i < noteN; i++) if (notes[i].unread) k++;
  return k;
}

static void btTick() {
  if (cfgNet != NET_BT) return;
  uint32_t now = millis();

  // A phone coming back to a robot it already knows is not news, and
  // lighting the screen for it every time it walks back into range is
  // a battery spent on nothing. Pairing for the first time still shows.
  if (btWokeReq) {
    btWokeReq = false;
    if (!asleep || NimBLEDevice::getNumBonds() == 0) wake("phone");
  }

  // Handed over whole, so the screen never sees half a notification.
  if (noteReady) {
    noteReady = false;
    if (rafiqIs(noteStage)) rafiqNote(noteStage);
    else {
      // Says RAFIQ but did not come from Shortcuts. Kept as a
      // notification, with the app it came from, so a Shortcut that is
      // being refused shows you why instead of doing nothing quietly.
      if (rafiqTagged(noteStage))
        snprintf(noteStage.msg, sizeof(noteStage.msg), "Not run, from %s", noteStage.app);
      // 7.5: switched-off apps never arrive; a VIP shows even when quiet.
      if (noteAllowed(noteStage)) {
        addNote(noteStage);
        if (!cfgQuiet || noteVip(noteStage)) popupShow();
      }
    }
  }

  if (!btUp || btConn == 0xFFFF) return;

  // Nudge the pairing along if iOS has not got round to it. Without
  // this the rung sits on "linked" for ever and nothing below runs.
  if (btStage == BT_CONNECTED && now - btSecAskedAt > BT_SEC_NUDGE_MS) {
    btSecAskedAt = now;
    NimBLEDevice::startSecurity(btConn);
  }
  if (btStage != BT_BONDED) return;
  if (now - btSince < CTS_SETTLE_MS) return;

  // The notification service only appears once you have said yes to
  // the prompt on the phone, so not finding it is not a failure yet.
  if (ancsState == ANCS_WAIT && !btApp && now - ancsLastTry > ANCS_SETTLE_MS) {
    ancsLastTry = now;
    if (setupAncs()) {
      ancsState = ANCS_READY;
      Serial.println("ancs: ready");
      { int li = linkFind(btConn); if (li >= 0 && links[li].authed) devLabel(links[li].a, "iPhone", true); }
    } else if (++ancsTries >= ANCS_GIVE_UP) {
      ancsState = ANCS_FAIL;
      Serial.println("ancs: no access; say yes to the prompt on the phone");
    }
  }

  if (ctsState != CTS_DONE && ctsFails < CTS_GIVE_UP && !btApp &&
      now - ctsAskedAt > CTS_RETRY_MS) {
    ctsAskedAt = now; ctsFails++;
    if (readCts()) { ctsState = CTS_DONE; ctsSyncedAt = now; ctsFails = 0; }
  }
  if (ctsState == CTS_DONE && now - ctsSyncedAt > CTS_REFRESH_MS) {
    ctsState = CTS_IDLE; ctsFails = 0; ctsAskedAt = 0;
  }

  // One attribute request at a time: the data source answers in
  // pieces and two overlapping replies cannot be told apart.
  if (ancsState == ANCS_READY) {
    if (ancsBusy && now - ancsAskedAt > ANCS_ANSWER_MS) ancsBusy = false;
    if (!ancsBusy && uidTail != uidHead) {
      uint32_t uid = uidQ[uidTail];
      dsCat = uidQCat[uidTail];
      uidTail = (uidTail + 1) % UIDQ_N;
      ancsAsk(uid);
    }
  }
}

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
    wReg(adxl, 0x2C, 0x1A);            // 7.7: BW_RATE, low power at 100 Hz
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
  if (!timeOk || !nowLocal(&t)) { snprintf(o, n, sec ? "--:--:--" : "--:--"); return; }
  int h = t.tm_hour;
  if (cfg12h) { h %= 12; if (!h) h = 12; }
  if (sec) snprintf(o, n, cfg12h ? "%d:%02d:%02d" : "%02d:%02d:%02d", h, t.tm_min, t.tm_sec);
  else     snprintf(o, n, cfg12h ? "%d:%02d" : "%02d:%02d", h, t.tm_min);
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
  // 7.6: the title and the clock with a gap of one letter, or the title alone
  if (!timeOk || UI_PAD + (int)strlen(title) * 6 + 6 + 5 * 6 > SCRW - UI_PAD) { titleBar(title, ""); return; }
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
  fb.ok = timeOk && nowLocal(&t);
  if (!fb.ok) {
    strcpy(fb.hm, "--:--"); strcpy(fb.hh, "--"); strcpy(fb.mm, "--"); strcpy(fb.ss, "--");
    strcpy(fb.day, "waiting"); strcpy(fb.dlong, "for the clock"); strcpy(fb.dshort, "--");
    fb.H = fb.M = fb.sec = 0; fb.mday = 1; fb.yday = 0;
    return;
  }
  if (cfg12h) {
    int h12 = t.tm_hour % 12; if (!h12) h12 = 12;
    snprintf(fb.hm, sizeof(fb.hm), "%d:%02d", h12, t.tm_min);
    snprintf(fb.hh, sizeof(fb.hh), "%d", h12);
  } else {
    snprintf(fb.hm, sizeof(fb.hm), "%02d:%02d", t.tm_hour, t.tm_min);
    snprintf(fb.hh, sizeof(fb.hh), "%02d", t.tm_hour);
  }
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
// WiFi's signal on WiFi; on Bluetooth, the phone link's.
static bool linkRssi(int& r) {
  if (online()) { r = (int)WiFi.RSSI(); return true; }
  if (cfgNet == NET_BT && btUp && btConn != 0xFFFF) {
    int8_t v = 0;
    if (ble_gap_conn_rssi(btConn, &v) == 0 && v < 0) { r = v; return true; }
  }
  return false;
}
static int sigBars() {
  int r;
  if (!linkRssi(r)) return 0;
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
  if (!timeOk || !nowLocal(&t)) return false;
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
  { int q;
    if (linkRssi(q)) snprintf(r, sizeof(r), "%ddBm", q);
    else             snprintf(r, sizeof(r), cfgNet == NET_BT ? "no phone" : "no wifi"); }
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
  { int q;
    if (linkRssi(q)) snprintf(l[5], 14, "%s %d", online() ? "rssi" : "bt", q);
    else             snprintf(l[5], 14, "offline"); }
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
    int q;
    snprintf(l[2], 24, "net   %s", cfgNet == NET_BT ? "bluetooth" : "none");
    if (linkRssi(q)) snprintf(l[3], 24, "rssi  %d dBm", q);
    else             snprintf(l[3], 24, "rssi  --");
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
  // 6.0.1: on Bluetooth the phone gives it the time, and with the
  // time, home is the watch face, exactly as it was on WiFi. The hello
  // and pairing screens are only for when there is no clock yet.
  if (offlineNow() && !(cfgNet == NET_BT && (fb.ok || NimBLEDevice::getNumBonds() > 0))) {
    char hi[34];
    snprintf(hi, sizeof(hi), "%s, %s",
             GREET[(millis() / 11000UL) % GREET_N], cfgName);
    // Bluetooth mode with nothing paired is the one state where the
    // robot has something to ask of you, so it asks instead of
    // saying hello to nobody.
    if (cfgNet == NET_BT && btStage != BT_BONDED) {
      btIcon(SCRW / 2, 20, 7);
      if (btStage == BT_FAIL) {
        ctr("Radio did not start", 36, 1);
        ctr("Turn it off and on", 48, 1);
      } else {
        ctr(btStage == BT_CONNECTED ? "Allow the pairing" : "Pair me in Settings", 36, 1);
        char nm[26];
        snprintf(nm, sizeof(nm), "Rafiq %s", cfgName);
        ctr(nm, 48, 1);
      }
      ctr(btShort(), 57, 1);
      oled.display();
      return;
    }
    // On Bluetooth it is not offline, it is somewhere else. One mark
    // or the other, never both.
    if (cfgNet == NET_BT) btIcon(6, 5, 3);
    else                  offlineIcon(4, 2);
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
    ctr("CONNECTING", 4, 1);
    char lk[8];
    if (lastGoodHM(lk, sizeof(lk))) {
      // The time we last knew, said plainly and labelled as past, so it
      // is never mistaken for the time now.
      ctr(lk, 22, 2);
      ctr("last known", 42, 1);
    } else {
      // Nothing to remember yet, so just plainly alive and waiting.
      char d[5];
      int k = (int)((millis() / 400UL) % 4);
      for (int i = 0; i < k; i++) d[i] = '.';
      d[k] = 0;
      ctr(d, 26, 2);
    }
    // a different line every eight seconds, so it is never a dead panel
    ctr(IDLE_LINES[(millis() / 8000UL) % IDLE_N], 55, 1);
    oled.display();
    return;
  }

  drawFaceOnly();
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
  if (!isnan(wHum)) {
    snprintf(l, sizeof(l), "%d%%", (int)roundf(wHum));
    at(30 + tw + 10, 30, l);
  }

  ctr(wxWord(wCode), 44, 1);
  // Off the network it is a report, not a reading, so it says how old.
  uint32_t tnow = (uint32_t)time(nullptr);
  char ln[48];
  if (!online() && wxAt && tnow >= wxAt) {
    uint32_t age = (tnow - wxAt) / 60;
    if (age < 60)        snprintf(ln, sizeof(ln), "%.11s  %lum ago", wCity, (unsigned long)age);
    else if (age < 2880) snprintf(ln, sizeof(ln), "%.11s  %luh ago", wCity, (unsigned long)(age / 60));
    else                 snprintf(ln, sizeof(ln), "%.11s  %lud ago", wCity,
                                  (unsigned long)(age / 1440 > 99 ? 99 : age / 1440));
  } else if (isnan(wWind)) snprintf(ln, sizeof(ln), "%s", wCity);
  else snprintf(ln, sizeof(ln), "%s  %.0f km/h", wCity, wWind);
  ctr(ln, 54, 1);
  oled.display();
}

static void fmt12(char* o, size_t n, int mins) {
  int h = mins / 60, m = mins % 60;
  int d = h % 12; if (!d) d = 12;
  snprintf(o, n, "%2d:%02d%s", d, m, h >= 12 ? "pm" : "am");
}
static bool isFriday() {
  struct tm t;
  return timeOk && nowLocal(&t) && t.tm_wday == 5;
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
  int nowMin = (timeOk && nowLocal(&t)) ? t.tm_hour * 60 + t.tm_min : -1;
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

// The bell is the reminders' bell, further down. One bell.
//  What Apple sends as the app is a bundle identifier, which is not
//  a thing to put on a screen. The tail of it usually is, and the
//  handful that are not are worth spelling out.
static const char* appShort(const Note& n) {
  if (!n.app[0]) return "Phone";
  const char* p = strrchr(n.app, '.');
  const char* t = (p && p[1]) ? p + 1 : n.app;
  if (!strcasecmp(t, "MobileSMS"))   return "Messages";
  if (!strcasecmp(t, "mobilemail"))  return "Mail";
  if (!strcasecmp(t, "MobilePhone")) return "Phone";
  if (!strcasecmp(t, "facetime"))    return "FaceTime";
  if (!strcasecmp(t, "mobilecal"))   return "Calendar";
  if (!strcasecmp(t, "whatsapp"))    return "WhatsApp";
  return t;
}

static bool noteIsCall(const Note& n) {
  return n.cat == CAT_CALL || n.cat == CAT_MISSED || n.cat == CAT_VOICE;
}

static const char* noteRow(int i, char* b, size_t n) {
  if (i >= noteN) { snprintf(b, n, "Clear all"); return b; }
  const Note& x = notes[i];
  snprintf(b, n, "%c%s: %s", x.unread ? '*' : ' ', appShort(x),
           x.title[0] ? x.title : x.msg);
  return b;
}
static void drawMessage() {
  oled.clearDisplay();

  if (depth == 0) {
    bar("NOTIFICATIONS");
    bellIcon(SCRW / 2, 31, 11);
    char l[26];
    int un = noteUnread();
    if (!noteN)  snprintf(l, sizeof(l), "Nothing yet");
    else if (un) snprintf(l, sizeof(l), "%d new of %d", un, noteN);
    else         snprintf(l, sizeof(l), "%d kept", noteN);
    ctr(l, 45, 1);
    ctr(noteN ? "Hold to open"
              : cfgNet != NET_BT        ? "needs bluetooth"
              : (ancsState == ANCS_READY || btApp) ? "from your phone"
              : ancsState == ANCS_FAIL  ? "allow it on the phone"
                                        : "linking up", 56, 1);
    oled.display();
    return;
  }

  if (depth == 1) {
    if (noteSel > noteN) noteSel = noteN;
    char r[12]; snprintf(r, sizeof(r), "%d", noteN);
    drawList("NOTIFICATIONS", r, noteN + 1, noteSel, noteRow);
    return;
  }

  // depth 2: one of them, read in full
  if (!noteN) { depth = 0; return; }
  if (noteIdx >= noteN) noteIdx = noteN - 1;
  Note& n = notes[noteIdx];
  char when[8];
  if (timeOk) {
    uint32_t ago = (millis() - n.at) / 1000UL;
    time_t   at  = time(nullptr) - (time_t)ago;
    struct tm* lt = localtime(&at);
    if (lt) {
      int h = lt->tm_hour;
      if (cfg12h) { h %= 12; if (!h) h = 12; }
      snprintf(when, sizeof(when), cfg12h ? "%d:%02d" : "%02d:%02d", h, lt->tm_min);
    } else snprintf(when, sizeof(when), "--:--");
  } else {
    snprintf(when, sizeof(when), "--:--");
  }
  // 7.4: the text first, then everything that frames it drawn over a
  // cleared strip. fitText scrolls long text by moving it up past its
  // top line, and drawn last it ran over the header and the buttons.
  if (noteIsCall(n)) marquee(n.title[0] ? n.title : "unknown", 30, 1);
  else               fitText(n.msg[0] ? n.msg : "(no text)", 24, 50, n.at);
  oled.fillRect(0, 0, SCRW, 23, SSD1306_BLACK);
  oled.fillRect(0, 51, SCRW, 13, SSD1306_BLACK);
  noteHeader(n, when);
  {
    char app[22]; snprintf(app, sizeof(app), "%s", appShort(n));
    ctr(app, 14, 1);
  }
  char pos[32];
  snprintf(pos, sizeof(pos), "tap:%d/%d", noteIdx + 1, noteN);
  twoButtons(pos, "hold:clear");
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
    ctr(readCount ? "Hold to open" : macLinked ? "Hold to ask the Mac"
                                                : "Needs the Mac", 55, 1);
    oled.display();
    return;
  }

  if (depth == 1) {
    if (!readCount) {
      oled.clearDisplay();
      titleBar("SHORT READS", "");
      ctr(storyState.c_str(), 24, 1);
      ctr(macLinked ? "Hold to ask the Mac" : "Needs the Mac", 40, 1);
      ctr(macLinked ? "for a new one" : "to write them", 50, 1);
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

// What holding on looks like once it means something.
//
//  Nothing at all for the first four seconds, because a bar on screen
//  during ordinary use was worse than the thing it explained. At four
//  it has already gone home, and this is the three seconds you have
//  to say you did not mean the rest of it.
//
//  A dial rather than a notice. The track is dotted all the way round
//  so you can see how far there is to go even at a glance, the lit
//  arc is what is left of it, and the number sits in the middle. Two
//  solid white bands top and bottom is what this was before, and on a
//  panel this small two bands is most of the panel.
//
//  It takes the whole screen. A ring punched into the middle of a
//  settings list left the list showing round the edges and the title
//  band sliced in half, which looked like a glitch rather than a
//  thing the robot meant to do.
static bool holdStripWanted() {
  return holdShown && touchOn && !sleepArmed && !cfgGesture && !gamePlaying() && !awayOn && !tmrOn;
}
// Along the bottom, like C3 Buddy: a bar filling left to right with a
// mark where Open turns into Back, and on the right what letting go
// will do, in a solid label.
static void drawHoldStrip() {
  uint32_t held = millis() - touchPressAt;
  uint32_t a = holdMs(), b = holdMaxMs(), m = holdBackMs();
  float f = (float)(held - a) / (float)(b - a);
  if (f < 0) f = 0;
  if (f > 1) f = 1;
  bool back = held >= m;
  oled.fillRect(0, 51, SCRW, 13, SSD1306_BLACK);
  oled.drawFastHLine(0, 51, SCRW, SSD1306_WHITE);
  oled.drawRoundRect(2, 54, 80, 8, 3, SSD1306_WHITE);
  int w = (int)(76 * f);
  if (w > 1) oled.fillRoundRect(4, 56, w, 4, 1, SSD1306_WHITE);
  int tick = 4 + (int)(76.0f * (float)(m - a) / (float)(b - a));
  oled.drawFastVLine(tick, 52, 2, SSD1306_WHITE);
  oled.fillRoundRect(86, 53, 40, 10, 3, SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_BLACK);
  const char* lab = back ? "BACK" : "OPEN";
  oled.setCursor(86 + (40 - (int)strlen(lab) * 6) / 2, 54);
  oled.print(lab);
  oled.setTextColor(SSD1306_WHITE);
}
static void drawHoldTier(uint32_t now) {
  uint32_t gone = now - sleepArmed;
  if (gone > TOUCH_COUNT_MS) gone = TOUCH_COUNT_MS;
  const float left = 1.0f - (float)gone / (float)TOUCH_COUNT_MS;
  int secs = (int)((TOUCH_COUNT_MS - gone + 999) / 1000);
  if (secs < 1) secs = 1;
  char n[2] = { (char)('0' + secs), 0 };

  oled.clearDisplay();
  ctr("GOING TO SLEEP", 0, 1);

  const int CX = SCRW / 2, CY = 33, R = 19;
  // The track: the whole way round, dotted, so the lit part has
  // something to be a fraction of.
  for (int d = 0; d < 360; d += 9) {
    float a = d * 0.01745f;
    oled.drawPixel(CX + (int)(R * cosf(a)), CY + (int)(R * sinf(a)), SSD1306_WHITE);
  }
  // Three notches, one per second, so it reads as a count and not
  // just as something draining.
  for (int i = 0; i < 3; i++) {
    float a = -1.5708f + i * 2.0944f;
    for (int rr = R + 1; rr <= R + 3; rr++)
      oled.drawPixel(CX + (int)(rr * cosf(a)), CY + (int)(rr * sinf(a)), SSD1306_WHITE);
  }
  ringArc(CX, CY, R, left);
  ctr(n, 21, 3);

  // Not "let go to stay": letting go does nothing now, and the pad
  // may let go on its own anyway. What is worth saying is how to get
  // it back, which depends on what the wake setting is.
  char back[24];
  if (WAKE_OPTS[cfgWakeIdx]) snprintf(back, sizeof(back), "hold %s to wake me",
                                      WAKE_NAME[cfgWakeIdx]);
  else                       snprintf(back, sizeof(back), "touch to wake me");
  ctr(back, 55, 1);
}

// Gesture mode, on screen.
//
//  Almost nothing, on purpose. The robot is not the thing you are
//  looking at while this is on, and a lit panel on a desk you are
//  working at is a distraction and a drain. A line to say what it is
//  and a line to say how to stop.
//
//  Except when you are on a call. Then the mic is the only thing
//  worth saying and it is said as large as it will go, because the
//  question "am I muted" is one you want answered from across the
//  room without touching anything.
static void drawGesture() {
  oled.clearDisplay();

  if (busyMic || busyCam) {
    // A capsule on a stand, down the left, with the word beside it
    // rather than under it. Centred, the word at double height and
    // the line telling you what to press ran into each other.
    const int cx = 30;
    oled.fillRoundRect(cx - 7, 6, 15, 21, 7, SSD1306_WHITE);
    // The cradle goes under the capsule, which means angles measured
    // downwards: the first version swept the top and showed as two
    // stubs poking out of its sides.
    for (int a = 20; a <= 160; a += 3)
      oled.drawPixel(cx + (int)(13 * cosf(a * 0.01745f)),
                     24 + (int)(13 * sinf(a * 0.01745f)), SSD1306_WHITE);
    oled.drawFastVLine(cx, 37, 6, SSD1306_WHITE);
    oled.drawFastHLine(cx - 7, 43, 15, SSD1306_WHITE);
    if (gestMuted)
      for (int i = -1; i <= 1; i++)
        oled.drawLine(cx - 16 + i, 2, cx + 16 + i, 46, SSD1306_WHITE);
    at(54, 12, gestMuted ? "MUTED" : "LIVE", 2);
    at(54, 34, gestMuted ? "2 unmutes" : "1 mutes", 1);
    oled.display();
    return;
  }

  ctr("gesture mode", 22, 1);
  ctr(cfgGestSrc == GSRC_KNOCK ? "knock the desk"
    : cfgGestSrc == GSRC_TOUCH ? "touch the pad" : "knock or touch", 34, 1);
  ctr("hold 4s to stop", 52, 1);
  oled.display();
}

// Bluetooth's rune. h is half its height, so it can be a mark in the
// corner of a line of text or the whole of a screen.
static void btIcon(int cx, int cy, int h) {
  const int w = (h + 1) / 2, m = h / 2;
  oled.drawFastVLine(cx, cy - h, h * 2, SSD1306_WHITE);
  oled.drawLine(cx, cy - h, cx + w, cy - m, SSD1306_WHITE);
  oled.drawLine(cx + w, cy - m, cx - w, cy + m, SSD1306_WHITE);
  oled.drawLine(cx, cy + h - 1, cx + w, cy + m, SSD1306_WHITE);
  oled.drawLine(cx + w, cy + m, cx - w, cy - m, SSD1306_WHITE);
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

// Fill a window with words.
//
// Two big lines if they will go, small ones if not, and a slow crawl
// when even small will not fit. `since` is when this text went up, so
// a crawl starts from the top rather than continuing mid sentence from
// whatever was there before.
//
// The window is cut hard at both ends afterwards. Adafruit's text has
// no clip, so a line halfway out of it would paint straight over
// whatever band sits above or below; this cuts it off instead, and
// anything that belongs in those bands is drawn after this returns.
static void fitText(const char* text, int top, int bottom, uint32_t since) {
  const int h = bottom - top + 1;
  int size = 2, n = wrapInto(text, 10, REM_LN);
  if (n * 18 > h) { size = 1; n = wrapInto(text, 21, REM_LN); }
  const int lh = size == 2 ? 18 : 10;
  const int blockH = n * lh;
  int off;
  if (blockH > h) {
    uint32_t travel = (uint32_t)(blockH - h);
    uint32_t climb  = travel * 1000UL / 9;       // nine pixels a second
    uint32_t cycle  = 1800 + climb + 1800;       // read, climb, read, again
    uint32_t t      = (millis() - since) % cycle;
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
  oled.fillRect(0, 0, SCRW, top, SSD1306_BLACK);
  oled.fillRect(0, bottom + 1, SCRW, SCRH - bottom - 1, SSD1306_BLACK);
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
  char day[16], hm[8]; day[0] = hm[0] = 0;
  bool hasWhen = r.at && timeOk;
  if (hasWhen) {
    time_t tt = (time_t)r.at;
    struct tm lt; localtime_r(&tt, &lt);
    strftime(day, sizeof(day), "%a %d %b", &lt);
    strftime(hm,  sizeof(hm),  "%H:%M", &lt);
  }
  // 14 to 53 is forty pixels, which is exactly four small lines. The
  // ribbon took one off the top and for a moment the window was 39,
  // so a four line reminder that used to sit still started crawling
  // for the sake of one pixel. The count sits at 56, so 53 is free.
  const int top = hasWhen ? 14 : 3, bottom = 53;
  const int h = bottom - top + 1;

  fitText(r.text, top, bottom, remShownAt);

  // A ribbon across the top: the day on one edge, the time on the
  // other. The two used to run together on one unmarked line, which
  // read as a sentence rather than a header.
  if (hasWhen) {
    oled.fillRect(0, 0, SCRW, 12, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    at(2, 2, day);
    const char* right = r.done ? "done" : hm;
    at(SCRW - 2 - (int)strlen(right) * 6, 2, right);
    oled.setTextColor(SSD1306_WHITE);
  } else if (r.done) {
    at(SCRW - 2 - 4 * 6, 2, "done");
  }
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
  else if (cfgNet == NET_BT) {
    int q;
    if (linkRssi(q)) snprintf(v, sizeof(v), "bt %d dBm", q);
    else             snprintf(v, sizeof(v), "bt %s", btShort());
  }
  else               snprintf(v, sizeof(v), "offline");
  vals[1] = v;
  snprintf(v, sizeof(v), "%u kB", (unsigned)(ESP.getFreeHeap() / 1024));
  vals[2] = v;
  // Live, so you can watch it change with a finger on the pad, and a
  // count so a touch that happened while the screen was elsewhere still
  // shows. "rest hi" or "rest lo" says which way round it decided the
  // board drives the pin.
  { char tc[20]; numStr(tc, sizeof(tc), touchCount);
    // And the longest unbroken touch it has ever seen. The TTP223
    // lets go on its own if a pad stays covered, and how long it
    // waits depends on the module. This is that number, measured on
    // this board rather than read off somebody's datasheet.
    if (touchLongest >= 100) {
      char lg[10];
      snprintf(lg, sizeof(lg), " %lu.%lus", (unsigned long)(touchLongest / 1000),
               (unsigned long)((touchLongest % 1000) / 100));
      strncat(tc, lg, sizeof(tc) - strlen(tc) - 1);
    }
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
  // 6.4: white words on black. Full white screens are kept for the
  // things that must be noticed: the call to prayer, the guard, find.
  oled.clearDisplay();
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
  if (toastKind == "rafiq")  head = "RAFIQ";
  if (toastKind == "syncfail") head = "SYNC FAILED";
  if (toastKind == "hotspot") head = "HOTSPOT";
  bar(head);

  bool loud = (toastKind == "remind" || toastKind == "break");
  if (loud) {
    // A ribbon that says what a press does, then the whole of the
    // message filling everything under it. There was a bar along the
    // bottom counting down the card's own life, which is not
    // something anyone needs to watch, and the message was squeezed
    // into one centred line above it whatever its length.
    // The words first: fitText cuts the window at both ends, so a
    // ribbon drawn before it is a ribbon painted over.
    fitText(toastText.length() ? toastText.c_str() : "Stand up, look away",
            14, 62, toastFlash);
    oled.fillRect(0, 0, SCRW, 12, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
    ctr("2 to snooze 15 min", 2, 1);
    oled.setTextColor(SSD1306_WHITE);
    // Caught from across a desk: the whole card turns over for a
    // sixth of a second every three seconds. It used to flash white
    // for half a second once, at the start, which you had to be
    // looking at it to see. Inverting the buffer keeps the words
    // readable through the flash instead of blanking them.
    if ((millis() - toastFlash) % 3000UL < 170UL)
      oled.fillRect(0, 0, SCRW, SCRH, SSD1306_INVERSE);
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

// Whichever face is chosen, drawn and nothing else. Shared so the
// picker shows the real thing rather than a second copy of this list
// that could drift out of step with it.
static void drawFaceOnly() {
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
}

// Choosing a watch face by looking at it.
//
// There are twenty three, and the setting used to cycle them one name
// at a time from the settings list, so finding the one you wanted
// meant pressing a row twenty times and reading words. Hold the row
// and the face itself is on the screen: tap for the next, hold to keep
// it, anything else puts back the one you came in with.
int facePickFrom = -1;
uint32_t facePickAt = 0;
static void drawFacePick() {
  drawFaceOnly();
  // A strip along the bottom, cleared first, because a face may well
  // have drawn into it.
  oled.fillRect(0, 52, SCRW, SCRH - 52, SSD1306_BLACK);
  oled.drawFastHLine(0, 51, SCRW, SSD1306_WHITE);
  // The strip is 21 characters wide and the two halves have to share
  // it. The name alone can be twelve, and "hold:keep" is nine, so they
  // take turns on the left: the hint while you have just arrived, the
  // name once you are looking. The count keeps the right, always.
  char l[26];
  if (millis() - facePickAt < 3000UL) snprintf(l, sizeof(l), "hold:keep");
  else                                snprintf(l, sizeof(l), "%.12s", FACE_NAME[cfgFace]);
  at(UI_PAD, 54, l);
  char r[8];
  snprintf(r, sizeof(r), "%d/%d", cfgFace + 1, FACE_N);
  at(SCRW - UI_PAD - (int)strlen(r) * 6, 54, r);
  oled.display();
}

static void drawSettings() {
  oled.clearDisplay();
  if (depth == 0) {                      // 7.6: the same card as every hub
    char l1[20], l2[20];
    if (!isnan(battV)) snprintf(l1, sizeof(l1), "Battery %d%%", battPct(battV));
    else               snprintf(l1, sizeof(l1), "No battery");
    snprintf(l2, sizeof(l2), "%s", linkN ? (linkN > 1 ? "2 linked" : "Linked") : "Not linked");
    uiHubCard("SETTINGS", IC_SLIDE, l1, l2);
    return;
  }
  if (depth == 2 && itemIdx == C_FACE)  { drawFacePick(); return; }
  if (depth == 2 && itemIdx == C_ABOUT) { drawAbout(); return; }
  if (depth == 2 && itemIdx == C_ACCEL) { drawAccel(); return; }
  if (depth == 2 && itemIdx == C_PAIR)  { drawPair();  return; }
  if (depth == 2 && itemIdx == C_TLOG)  { drawTlog();  return; }
  if (depth == 2 && itemIdx == C_BVIEW) { drawBattUse(); return; }   // 7.8
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
  // The groups. Four fit on the screen; the list moves to keep the
  // chosen one in view.
  if (depth == 1 && setGrp < 0) {
    static const uint8_t* const ic[SG_COUNT] = { IC_SUN, IC_HAND, IC_BT, IC_SHIELD, IC_BATT, IC_CHIP };
    bar("SETTINGS");
    int first = uiFirst(grpSel, SG_COUNT);
    for (int r = 0; r < UI_ROWS && first + r < SG_COUNT; r++) {
      int g = first + r;
      uiRow(r, ic[g], SG_NAME[g], "", g == grpSel, SG_COUNT > UI_ROWS);   // names only: they need the room
    }
    uiScroll(first, SG_COUNT);
    oled.display();
    return;
  }

  const int grp = setGrp < 0 ? SG_DISPLAY : setGrp;
  const int rows = sgLen(grp);
  {
    char up[22]; snprintf(up, sizeof(up), "%s", depth == 2 ? "CHANGE" : SG_NAME[grp]);
    for (char* q = up; *q; q++) *q = toupper((unsigned char)*q);
    bar(up);
  }

  char v[18];
  int here = sgPos(grp, itemIdx);
  int first = here > 3 ? here - 3 : 0;
  if (first > rows - 4) first = rows - 4;
  if (first < 0) first = 0;
  for (int r = 0; r < 4 && first + r < rows; r++) {
    int i = SG_ROWS[grp][first + r], y = 14 + r * 12;
    bool on = (i == itemIdx);
    const int sc = rows > UI_ROWS ? 5 : 0;          // 7.6: room for the scrollbar
    if (on) { oled.fillRoundRect(1, y - 2, SCRW - 2 - (sc ? 3 : 0), UI_ROW_H, 2, SSD1306_WHITE); oled.setTextColor(SSD1306_BLACK); }
    else      oled.setTextColor(SSD1306_WHITE);
    at(UI_PAD + 1, y, C_NAME[i]);
    switch (i) {
      case C_BRIGHT: { int k = 0;
                       for (int j = 0; j < BRIGHT_N; j++) if (BRIGHT_OPTS[j] == cfgBright) k = j;
                       snprintf(v, sizeof(v), "%s", BRIGHT_NAME[k]); break; }
      case C_FACE:   snprintf(v, sizeof(v), "%s", FACE_NAME[cfgFace]); break;
      case C_PRAYER: snprintf(v, sizeof(v), "%s", prayerOk ? "saved" : "none"); break;
      case C_ACCEL:  snprintf(v, sizeof(v), "hold"); break;
      case C_KNOCK:  snprintf(v, sizeof(v), "%s", cfgKnock ? "on" : "off"); break;
      case C_HIJRI:  snprintf(v, sizeof(v), "%+d d", cfgHijriAdj); break;
      case C_MODE:   snprintf(v, sizeof(v), "%s",
                              cfgNet == NET_OFF ? "off" :
                              cfgNet == NET_BT  ? btShort() :
                              rescueAP ? "hotspot" :
                              wsKind == WS_SYNC ? "syncing" :
                              (netDown ? "no signal" : "wifi now")); break;
      case C_GUARD:  snprintf(v, sizeof(v), "%s", cfgPGuard ? "on" : "off"); break;
      case C_TAMPER: snprintf(v, sizeof(v), "hold"); break;
      case C_TLOG:   snprintf(v, sizeof(v), "hold"); break;
      case C_HOLD:   snprintf(v, sizeof(v), "%u.%u s", HOLD_OPTS[cfgHoldIdx] / 1000, (HOLD_OPTS[cfgHoldIdx] % 1000) / 100); break;
      case C_CLOCK:  snprintf(v, sizeof(v), "%s", cfg12h ? "12 hour" : "24 hour"); break;
      case C_WAKEBY: snprintf(v, sizeof(v), "%s", WAKEBY_NAME[cfgWakeBy]); break;
      case C_MULTI:  if (cfgMulti) snprintf(v, sizeof(v), "on, %d in", linkN);
                     else          snprintf(v, sizeof(v), "off"); break;
      case C_DEV1:   snprintf(v, sizeof(v), "%.12s", prefName(0)); break;
      case C_DEV2:   snprintf(v, sizeof(v), "%.12s", prefName(1)); break;
      case C_AUTOAWAY: snprintf(v, sizeof(v), "%s", cfgAutoAway ? "on" : "off"); break;
      case C_NIGHT:  snprintf(v, sizeof(v), "%s", cfgNight ? "on" : "off"); break;
      case C_BLOG:   snprintf(v, sizeof(v), "%s", cfgBlog ? "on" : "off"); break;
      case C_PLOCK:  snprintf(v, sizeof(v), "%s", PLOCK_NAME[constrain(cfgPLock, 0, 3)]); break;
      case C_BVIEW:  snprintf(v, sizeof(v), "%s", cfgBlog ? "hold" : "off"); break;
      case C_BRESET: snprintf(v, sizeof(v), "%.2f V", BLOG_V[constrain(cfgBlogV, 0, 3)]); break;
      case C_BED:    snprintf(v, sizeof(v), "%02d:%02d", cfgBed / 60, cfgBed % 60); break;
      case C_BIKE:   snprintf(v, sizeof(v), "%s", cfgBike ? "on" : "off"); break;
      case C_SHAKE:  snprintf(v, sizeof(v), "%s",
                              (cfgBack == BACK_KNOCK && !cfgKnock)
                                ? "knock off" : BACK_NAME[cfgBack]); break;
      case C_DEEP:   snprintf(v, sizeof(v), "%s", DEEP_NAME[cfgDeepIdx]); break;
      case C_WAKEH:  snprintf(v, sizeof(v), "%s", WAKE_NAME[cfgWakeIdx]); break;
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
      case C_UPDATE: snprintf(v, sizeof(v), "%s", rescueAP ? "hotspot on" : "from a file"); break;
      // Everything left is something you open rather than something
      // with a value. "x2" meant knock twice, from when knocking was
      // the only way in; holding is how you open anything now.
      default:       snprintf(v, sizeof(v), "hold"); break;
    }
    // 7.6: never into the label, never under the scrollbar
    {
      const int sc = rows > UI_ROWS ? 5 : 0;
      int room = (SCRW - UI_PAD - sc - (UI_PAD + 1) - ((int)strlen(C_NAME[i]) + 1) * 6) / 6;
      if (room < 0) room = 0;
      if ((int)strlen(v) > room) v[room] = 0;
      oled.setCursor(SCRW - UI_PAD - sc - (int)strlen(v) * 6, y);
      oled.print(v);
    }
  }
  oled.setTextColor(SSD1306_WHITE);
  uiScroll(first, rows);
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
  if (wxOk) { wxAt = (uint32_t)time(nullptr); wxDirty = true; }   // kept by loop()
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
  if (!timeOk || !nowLocal(&t)) return;
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
//  WHERE A READ COMES FROM
// ================================================================
//  The Mac. It holds the key, it has the network, and it can be told
//  what to write without a firmware update. The robot used to call
//  OpenAI itself over WiFi, which meant the radio coming up for a
//  story and the key living on the device; both are gone.
//
// Asking the Mac for one. The robot no longer writes its own: the key
// and the fetching live on the Mac, which has the network anyway, and
// this way the radio never has to come up for a story.
static void refillShelf() {
  if (!macLinked) { storyState = "Needs the Mac"; return; }
  evtSend("want read");
  storyState = "Asked the Mac";
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
extern bool macDim;
static void applyBright() {
  oled.ssd1306_command(SSD1306_SETCONTRAST);
  oled.ssd1306_command(macDim ? min(cfgBright, 8) : cfgBright);   // 7.5: low while you type
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
  if (awayOn) { awayEyes(false); goSleepQuick(); return; }  // Away: eyes close quickly, then dark
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
  // A linked phone is somebody listening too, as the Mac is.
  if ((wokeForAlarm || !macLinked) && !phoneHeld()) { wokeForAlarm = false; wantDeep = true; }
  else                                              { wokeForAlarm = false; goSleep(); }
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
  if (!prayerOk || !timeOk || !nowLocal(&t)) return -1;
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
  cfgNetHome = NET_BT; prefs.putInt("net", cfgNetHome);
  cfgPGuard = false; prefs.putBool("guard", cfgPGuard);
  cfgQuiet = false;  prefs.putBool("quiet", cfgQuiet);
  cfgWakeIdx = 2;    prefs.putInt("wakeh", cfgWakeIdx);
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

// Lie down.
//
//  secs is how long until the next alarm, or -1 for none.
//
//  Two things have to happen in the right order. The pad wake is by
//  LEVEL, not by edge, so entering deep sleep while a finger is still
//  on the pad wakes the chip again immediately, and again, and again:
//  a loop that empties the battery faster than anything it was meant
//  to save. So it waits for the finger first.
//
//  And if the finger never comes, something is resting on the pad.
//  Waiting for ever is not an option and neither is arming a wake that
//  fires at once, so it goes down on the timer alone and sets a short
//  one, far enough out to cost nothing and near enough that it can look
//  again and arm the pad properly the moment the pad is free.
#define SLEEP_RELEASE_MS 20000UL     // long enough for a hand, not for a bag
#define SLEEP_RETRY_S    60          // and then look again this often
static void sleepNow(long secs) {
  rtcDeepAt = (uint32_t)time(nullptr);   // 7.8: the deep sleep is counted on waking
  blogSave();
  // Lying down deliberately is not a crash, and a deep wake is a
  // boot, so without this a robot that sleeps often in Bluetooth mode
  // would count its way to the fallback having never once failed.
  if (btNoteOut) { prefs.putInt("btry2", 0); btNoteOut = false; }
  uint32_t t0 = millis();
  bool held = (digitalRead(TOUCH_PIN) != touchRest);
  while (held && millis() - t0 < SLEEP_RELEASE_MS) {
    delay(20);
    held = (digitalRead(TOUCH_PIN) != touchRest);
  }
  if (held) {
    Serial.println("something is on the pad; sleeping on the clock alone");
    if (secs < 0 || secs > SLEEP_RETRY_S) secs = SLEEP_RETRY_S;
  } else {
    esp_deep_sleep_enable_gpio_wakeup(BIT(TOUCH_PIN),
                                      touchRest ? ESP_GPIO_WAKEUP_GPIO_LOW
                                                : ESP_GPIO_WAKEUP_GPIO_HIGH);
  }
  if (secs > 0) esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
  esp_deep_sleep_start();
}

// Did you mean it?
//
//  Runs before everything. Before the serial port has settled, before
//  the settings are read, before the sensors, the display or the
//  radio. Nothing here costs anything except a few hundred
//  milliseconds of a processor that was going to have to start up
//  anyway, which is the entire point: a sleeve brushing the pad used
//  to cost a full wake, the screen on and the radio up for the whole
//  of a screen timeout. Now it costs almost nothing.
//
//  The wake itself cannot be made to wait. The chip is off, the wake
//  is a level on a pin, and there is no duration anywhere in it. So it
//  wakes, looks, and lies back down.
//
//  Three seconds is long enough that saying nothing would read as a
//  flat battery, so past half a second the screen comes up on its own
//  and fills a ring. Only the screen. The radio stays down either way,
//  and if you let go the screen goes out again having cost a fraction
//  of what the radio would have.
//
//  Never returns if the touch was nothing.
static void wakeGate() {
  prefs.begin("nexus", false);
  int idx = constrain(prefs.getInt("wakeh", 2), 0, WAKE_N - 1);
  uint32_t need = WAKE_OPTS[idx];
  // Read, never measured. The finger that woke it is on the pad.
  touchRest = prefs.getBool("trest", false);
  int bright = constrain(prefs.getInt("bri", 160), 0, 255);
  // Told never to switch off. It should not be here at all in that
  // case, but if it is, refusing a touch would leave it asleep with
  // no way back and the setting saying that cannot happen.
  bool noDeep = prefs.getBool("nodeep", false);
  prefs.end();
  if (!need || noDeep) return;                 // asked for any touch at all

  pinMode(TOUCH_PIN, INPUT_PULLDOWN);
  const bool ring = need >= WAKE_RING_MIN;
  uint32_t t0 = millis();
  bool lit = false;

  while (true) {
    uint32_t held = millis() - t0;
    if (digitalRead(TOUCH_PIN) == touchRest) break;        // let go: it was nothing
    if (held >= need) {                                    // meant it
      if (lit) { oled.clearDisplay(); oled.display(); }
      Serial.printf("held %lums, waking up\n", (unsigned long)held);
      return;
    }
    if (ring && !lit && held >= WAKE_RING_AFTER) {
      Wire.begin(SDA_PIN, SCL_PIN);
      Wire.setClock(400000);
      if (oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) {
        oled.setTextWrap(false);
        oled.setTextColor(SSD1306_WHITE);
        oled.ssd1306_command(SSD1306_SETCONTRAST);
        oled.ssd1306_command(bright);
        lit = true;
      } else {
        // No panel to draw on is not a reason to refuse to wake up.
        break;
      }
    }
    if (lit) {
      oled.clearDisplay();
      ringArc(SCRW / 2, SCRH / 2, 16,
              (float)(held - WAKE_RING_AFTER) / (float)(need - WAKE_RING_AFTER));
      oled.drawCircle(SCRW / 2, SCRH / 2, 16, SSD1306_WHITE);
      oled.display();
    }
    delay(8);
  }

  // Nothing after all. Straight back down, without having loaded the
  // reminders or the prayer times or counted this as a boot.
  if (lit) { oled.clearDisplay(); oled.display(); oled.ssd1306_command(SSD1306_DISPLAYOFF); }
  long secs = -1;
  if (rtcAlarmAt) {
    secs = (long)rtcAlarmAt - (long)time(nullptr);
    // Due already, or near enough that going back down would miss it.
    if (secs <= 2) { Serial.println("brushed, but something is due; staying up"); return; }
  }
  Serial.printf("brushed, not held; back to sleep%s\n",
                secs > 0 ? " with the alarm still set" : "");
  sleepNow(secs);
}

static void goDeep() {
  if (notesDirty) saveNotes();         // the list survives the sleep
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
  if (deepOff) return;
  awayFlush(true);
  bool checking = deepAuto && cfgNet == NET_BT && NimBLEDevice::getNumBonds() > 0;
  Serial.println(checking ? "switching off, checking for the phone" : "switching off until touched");
  if (!checking) { sleepCard(); rtcAwaySince = 0; }

  screenPower(false);

  // Whichever is sooner, a prayer or a reminder. Worked out here, from
  // the clock, so it holds with no network: the chip keeps counting
  // through deep sleep and comes back on its own at the right minute.
  //
  // The answer is kept as a moment rather than a duration, in RTC
  // memory, which survives deep sleep. A wake that turns out to be
  // nothing can then put itself straight back down without loading
  // the reminders and the prayer table again to work out the same
  // number, and without the risk of working out a different one.
  long pray = secsToNextAlert();
  long rem  = secsToNextRem();
  long secs = -1;
  if (pray > 0 && rem > 0) secs = pray < rem ? pray : rem;
  else if (pray > 0)       secs = pray;
  else if (rem > 0)        secs = rem;
  rtcWakeCheck = 0;
  if (checking) {
    uint32_t tnow = (uint32_t)time(nullptr);
    if (!rtcAwaySince) rtcAwaySince = tnow;
    long chk = (tnow - rtcAwaySince < 3600) ? 180 : 300;   // 3 min for an hour, then 5
    if (secs <= 0 || chk < secs) { secs = chk; rtcWakeCheck = 1; }
  }
  if (nightDeep) {                        // 7.7: night sleep ends just before Fajr
    long ne = secsToNightEnd();
    if (ne > 0 && (secs <= 0 || ne < secs)) secs = ne;
    nightDeep = false;
  }
  rtcAlarmAt = (secs > 0 && timeOk) ? (uint32_t)time(nullptr) + (uint32_t)secs : 0;
  if (secs > 0)
    Serial.printf("next wake in %ld s (%s)\n", secs,
                  (rem > 0 && (pray <= 0 || rem <= pray)) ? "a reminder" : "a prayer");

  prefs.end();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  sleepNow(secs);
}

static void wake(const char* why) {
  if (asleep && blog.magic == BLOG_MAGIC) blog.wakes++;      // 7.8
  lastActive = millis();
  wokeBy = why;
  if (!asleep) return;
  asleep = false;
  if (pmAvail) pmSet(false); else setCpuFrequencyMhz(160);
  screenPower(true);
  eyes.setAutoblinker(ON, 7, 5); eyes.setIdleMode(ON, 5, 4);
  applyEyes(cfgEyes); eyes.open();
  // In gesture mode the eyes do not open for you. It is a knock
  // sensor on a desk, and three hundred milliseconds of animation
  // between your knock and your Mac is three hundred milliseconds
  // of nothing useful.
  // 6.1: a short opening for a touch, none for a notification or a
  // card, so news is on the screen the moment it arrives.
  // Lifted or shaken, it is a look at the time and no more: the face
  // at once, and dark again shortly after unless the pad is touched.
  bool glance = !strcmp(why, "shake") || !strcmp(why, "picked up") || !strcmp(why, "moved");
  bool news = glance || !strcmp(why, "notification") || !strcmp(why, "shortcut") || !strcmp(why, "guard") ||
              !strcmp(why, "timer") ||
              !strcmp(why, "update") || !strcmp(why, "sync") || !strcmp(why, "message");
  glanceUntil = (glance && !awayOn) ? millis() + GLANCE_MS : 0;
  if (!cfgGesture && !news && !awayOn)
    for (int i = 0; i < 8; i++) { eyesFrame(); delay(16); }
  if (awayOn) { awayEyes(true); lastActive = millis(); lastDraw = 0; }   // eyes open, then the message
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
  if (awayOn) return;                  // away: nothing comes up
  struct tm t;
  if (!prayerOk || !timeOk || !nowLocal(&t)) return;

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
    if (d == 0) { char b[24]; snprintf(b, sizeof(b), "pray %s", PRAYERS[i]); evtSend(b); }   // 7.5: the Mac pauses
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
  macAddr = web.client().remoteIP();         // where to send presses back to
  if (!macLinked) {
    macLinked = true;
    linkCardJoin = true;
    linkCardUntil = millis() + 1600;
    wake("mac");
  }
}
static bool guard() {
  if (!authed()) { web.send(401, "application/json", "{\"ok\":false,\"err\":\"pair first\"}"); return false; }
  wsTouch();                           // somebody is using the WiFi
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
// "x y", each -1000..1000. From UDP on WiFi, or Bluetooth (7.4).
static void cursorFeed(const char* b) {
  if (!cfgFollow) return;
  int x = 0, y = 0;
  if (sscanf(b, "%d %d", &x, &y) == 2) {
    curX = constrain(x / 1000.0f, -1.0f, 1.0f);
    curY = constrain(y / 1000.0f, -1.0f, 1.0f);
    curUntil = millis() + CURSOR_HOLD_MS;
  }
}
static void serviceCursor() {
  if (!cfgFollow || !online()) return;
  int n = cursorUdp.parsePacket();
  while (n > 0) {
    char b[32];
    int got = cursorUdp.read(b, sizeof(b) - 1);
    if (got > 0) { b[got] = 0; cursorFeed(b); }
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
  // On Bluetooth the hotspot is a session of its own: the radio comes
  // up for it, and goes back to Bluetooth ten minutes after the last
  // phone leaves. Nothing is written to flash, so a restart is home.
  if (cfgNet != NET_WIFI && !wsStart(WS_HOTSPOT)) return;
  WiFi.mode(online() ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(RESCUE_SSID, RESCUE_PASS);
  rescueAP = true;
  wsLastUse = millis();
  Serial.printf("hotspot up: %s at %s\n", RESCUE_SSID, WiFi.softAPIP().toString().c_str());
  // Say how to use it, on the screen, because there is nowhere else
  // to read it from.
  toastKind = "hotspot";
  // Two lines of 21 is all this card shows: the network and its
  // password, then where to go.
  toastText = String(RESCUE_SSID) + " " + RESCUE_PASS + "\n192.168.4.1 > Update";
  toastUntil = millis() + 120000; toastFlash = millis(); remShowing = -1;
  wake("hotspot");
}

static void knockOne() {
  cTap++;
  if (awayOn) { lastActive = millis(); return; }   // away: nothing but the message
  if (tmrOn && !awayOn) { lastActive = millis(); return; }   // timer: nothing else
  // The new cards from 6.0 take a press before anything else: it
  // cancels a countdown, quiets the guard, stops the finder.
  if (tamperCountAt) { tamperCountAt = 0; flash("NOT ARMED", 1200); return; }
  if (pgUntil)       { pgUntil = 0; return; }
  if (walkUntil)     { walkUntil = 0; return; }     // 7.5: Mac left behind, seen
  if (findUntil)     { findUntil = 0; return; }
  if (popOn)         { popupClose(false); return; }
  // Anything the Mac put on the screen goes away on one knock. It is
  // the Mac's idea of what you want to see, and this is the desk.
  if (dndUntil)    { dndUntil = 0;      return; }
  if (relaxOn)     { relaxOn = false; relaxUntil = 0; return; }
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
  // 7.6: in a hub's menu a tap moves the choice; inside an item, it
  // goes on to the next item of the same hub.
  if (isHub(screen)) {
    if (depth == 1) { int n = hubCount(screen); if (n) hubSel = (hubSel + 1) % n; return; }
    screen = nextScreen(screen); return;
  }
  if (inHub >= 0 && depth == 0 && screen != S_SETTINGS) {
    int n = hubCount(inHub);
    if (n) hubEnter(inHub, (hubSel + 1) % n);
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
    if (depth == 1) {
      if (setGrp < 0) grpSel = (grpSel + 1) % SG_COUNT;   // the four
      else            itemIdx = sgNext(setGrp, itemIdx);  // within one
      return;
    }
    switch (itemIdx) {
      case C_BRIGHT: { int i = 0;
                       for (int k = 0; k < BRIGHT_N; k++) if (BRIGHT_OPTS[k] == cfgBright) i = k;
                       cfgBright = BRIGHT_OPTS[(i + 1) % BRIGHT_N];
                       applyBright(); prefs.putInt("bri", cfgBright); break; }
      case C_FACE:   // Into the picker, where you can see them.
                     facePickFrom = cfgFace; facePickAt = millis(); depth = 2; break;
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

static void knockTwoInner();
static void knockTwo() {
  // 7.6: a hub opens to its menu, and the menu opens the chosen item.
  if (!awayOn && !tmrOn && isHub(screen)) {
    if (depth == 0) { depth = 1; hubSel = 0; return; }
    hubEnter(screen, hubSel);
    return;
  }
  knockTwoInner();
}
static void knockTwoInner() {
  cDouble++;
  if (awayOn) { lastActive = millis(); return; }   // away: nothing but the message
  if (tmrOn && !awayOn) { lastActive = millis(); return; }   // timer: nothing else
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
      case S_SETTINGS: depth = 1; setGrp = -1; grpSel = 0; itemIdx = 0; break;
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
    if (setGrp < 0) {                     // open the group you are on
      setGrp = grpSel;
      itemIdx = SG_ROWS[setGrp][0];
      return;
    }
    switch (itemIdx) {
      case C_REBOOT:  delay(150); ESP.restart(); break;
      case C_UPDATE:
        // 7.4.1: one way to update, a file through the hotspot. Rafiq
        // no longer fetches releases from GitHub by itself.
        startHotspot();
        break;
      case C_GUARD:
        cfgPGuard = !cfgPGuard;
        prefs.putBool("guard", cfgPGuard);
        pgFired = false; pgUntil = 0;
        flash(cfgPGuard ? "GUARD ON" : "GUARD OFF", 1000);
        break;
      case C_TAMPER: tamperArm(); break;
      case C_TLOG:   tlogLoad(); depth = 2; break;
      case C_HOLD:
        cfgHoldIdx = (cfgHoldIdx + 1) % HOLD_N; prefs.putInt("holdi", cfgHoldIdx);
        break;
      case C_CLOCK:
        cfg12h = !cfg12h; prefs.putBool("h12", cfg12h);
        break;
      case C_WAKEBY:
        cfgWakeBy = (cfgWakeBy + 1) % 3; prefs.putInt("wakeby", cfgWakeBy);
        break;
      case C_MULTI:
        cfgMulti = !cfgMulti; prefs.putBool("multi", cfgMulti);
        if (cfgMulti && btUp) NimBLEDevice::startAdvertising();
        else if (!cfgMulti && btConn2 != 0xFFFF && NimBLEDevice::getServer())
          NimBLEDevice::getServer()->disconnect(btConn2);
        flash(cfgMulti ? "TWO AT ONCE" : "ONE AT A TIME", 1100);
        break;
      case C_DEV1: case C_DEV2: {
        // Walk the remembered devices, then "any". The other slot's
        // device is skipped, so one device cannot be both.
        int k = (itemIdx == C_DEV1) ? 0 : 1;
        static const uint8_t zero[6] = { 0 };
        int cur = memcmp(prefA[k], zero, 6) ? devFind(prefA[k]) : -1;
        for (int step = 0; step <= devN; step++) {
          cur++;
          if (cur >= devN) { memset(prefA[k], 0, 6); break; }
          if (memcmp(devs[cur].a, prefA[1 - k], 6)) { memcpy(prefA[k], devs[cur].a, 6); break; }
        }
        devSave();
        break;
      }
      case C_AUTOAWAY:
        cfgAutoAway = !cfgAutoAway; prefs.putBool("autoaway", cfgAutoAway);
        break;
      case C_PLOCK:
        cfgPLock = (cfgPLock + 1) % 4; prefs.putInt("plock", cfgPLock);
        flash(cfgPLock ? "POCKET LOCK ON" : "POCKET LOCK OFF", 1100);
        break;
      case C_BLOG:
        cfgBlog = !cfgBlog; prefs.putBool("blog", cfgBlog);
        if (cfgBlog && blog.magic != BLOG_MAGIC) blogReset();
        break;
      case C_BRESET:
        cfgBlogV = (cfgBlogV + 1) % 4; prefs.putInt("blogv", cfgBlogV); blogArmed = true;
        break;
      case C_BVIEW:
        if (cfgBlog) { subIdx = 0; depth = 2; }
        break;
      case C_NIGHT:
        cfgNight = !cfgNight; prefs.putBool("night", cfgNight);
        flash(cfgNight ? "NIGHT SLEEP ON" : "NIGHT SLEEP OFF", 1100);
        break;
      case C_BED: {
        // 21:00 to 01:30, half an hour a press, then round again
        int b = cfgBed < 720 ? cfgBed + 1440 : cfgBed;
        b += 30; if (b > 1440 + 90) b = 21 * 60;
        cfgBed = b % 1440; prefs.putInt("bed", cfgBed);
        break;
      }
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
        // Bluetooth, then WiFi for now, then off, then round again.
        // Only Bluetooth and Off are kept. WiFi lasts until a real
        // restart, or half an hour of nobody using it.
        if (cfgNet == NET_WIFI) {
          cfgNetHome = NET_OFF; prefs.putInt("net", cfgNetHome);
          wsEnd("EVERYTHING OFF");
        } else if (cfgNet == NET_BT) {
          if (wsStart(WS_MANUAL)) flash("WIFI UNTIL RESTART", 1500);
        } else {
          cfgNetHome = NET_BT; prefs.putInt("net", cfgNetHome);
          cfgNet = NET_BT; bleOn();
          flash("BLUETOOTH ON", 1300);
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
        flash(cfgBack == BACK_KNOCK ? (cfgKnock ? "BACK BY KNOCK" : "KNOCKS ARE OFF")
            : cfgBack == BACK_SHAKE ? "BACK BY SHAKE" : "BACK BY EITHER", 1200);
        break;
      case C_DEEP:
        cfgDeepIdx = (cfgDeepIdx + 1) % DEEP_N;
        prefs.putInt("deepi", cfgDeepIdx);
        flash(DEEP_NAME[cfgDeepIdx], 900);
        break;
      case C_WAKEH:
        cfgWakeIdx = (cfgWakeIdx + 1) % WAKE_N;
        prefs.putInt("wakeh", cfgWakeIdx);
        flash(cfgWakeIdx ? "HOLD TO WAKE ME" : "ANY TOUCH WAKES ME", 1100);
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
  if (awayOn) { lastActive = millis(); return; }   // away: nothing but the message
  if (tmrOn && !awayOn) { lastActive = millis(); return; }   // timer: nothing else
  // 7.6: back from a hub's menu to its front, from its front to Home, and
  // from where an item began straight back to its hub's menu.
  if (isHub(screen)) { if (depth == 1) depth = 0; else { screen = S_HOME; depth = 0; } return; }
  if (inHub >= 0 && depth <= hubEntry &&
      !(screen == S_SETTINGS && depth == 1 && setGrp != SG_FAITHSET)) {
    int h = inHub; inHub = -1;
    screen = h; depth = 1;
    return;
  }
  if (screen == S_FOCUS && (swOn || depth == 1)) {
    if (swOn) { swOn = false; swRun = false; depth = 0; return; }
    depth = 0; itemIdx = 0;
    return;
  }
  // Back out of a group to the four, rather than out of the settings
  // altogether. Leaving takes one more press, which is what you want
  // when the thing you were looking for is in the group next door.
  if (screen == S_SETTINGS && depth == 1 && setGrp >= 0) { setGrp = -1; return; }
  if (screen == S_SETTINGS && itemIdx == C_FACE && depth == 2) {
    if (facePickFrom >= 0) cfgFace = facePickFrom;
    facePickFrom = -1; depth = 1;
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
  if (awayOn) { lastActive = millis(); return; }   // away: nothing but the message
  if (tmrOn && !awayOn) { lastActive = millis(); return; }   // timer: nothing else
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
        else upState = upQuick ? U_OFF : U_MENU;
        return;
      }
      upState = upQuick ? U_OFF : U_MENU;
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
      upState = upQuick ? U_OFF : U_MENU;
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
  if (awayOn) { lastActive = millis(); return; }   // away: nothing but the message
  if (tmrOn && !awayOn) { lastActive = millis(); return; }   // timer: nothing else
  // The new cards from 6.0 take a press before anything else: it
  // cancels a countdown, quiets the guard, stops the finder.
  if (tamperCountAt) { tamperCountAt = 0; flash("NOT ARMED", 1200); return; }
  if (nightCardUntil) {                               // 7.7: tap, an hour later; hold, now
    if (g == TG_ONE) {
      nightPush = constrain(nightPush + 60, 0, 360); nightCardUntil = 0;
      char b[24]; int at = (cfgBed + nightPush) % 1440;
      snprintf(b, sizeof(b), "SLEEP AT %02d:%02d", at / 60, at % 60); flash(b, 1300);
    } else { nightCardUntil = 0; nightGo(); }
    return;
  }
  if (pgUntil)       { pgUntil = 0; return; }
  if (walkUntil)     { walkUntil = 0; return; }     // 7.5: Mac left behind, seen
  if (findUntil)     { findUntil = 0; return; }
  if (popOn && popRinging()) {                           // 7.5: a call, ringing
    ancsAction(notes[0].uid, g == TG_LONG ? 1 : 0);      // hold declines, tap answers
    flash(g == TG_LONG ? "DECLINED" : "ANSWERED", 900);
    popupClose(false);
    return;
  }
  if (popOn) {
    if (g == TG_LONG) popupClose(true);                  // open it
    else if (popWoke) {                                   // a touch on a glance: stay up
      popWoke = false;
      popUntil = millis() + popupSecs() * 1000UL;
    } else popupClose(false);
    return;
  }
  // The face picker: a tap is the next one, a hold keeps what is on
  // the screen, and anything else puts back the one you arrived with.
  if (screen == S_SETTINGS && depth == 2 && itemIdx == C_FACE) {
    if (g == TG_ONE) { cfgFace = (cfgFace + 1) % FACE_N; facePickAt = 0; return; }
    if (g == TG_LONG) {
      prefs.putInt("face", cfgFace);
      facePickFrom = -1; depth = 1;
      flash("KEPT", 800);
      return;
    }
    if (facePickFrom >= 0) cfgFace = facePickFrom;
    facePickFrom = -1; depth = 1;
    return;
  }
  if (screen == S_SETTINGS && depth == 2 && itemIdx == C_TLOG && g == TG_ONE) {
    tlSel = (tlSel + 1) % (tlN + 1);
    return;
  }
  if (screen == S_SETTINGS && depth == 2 && itemIdx == C_BVIEW) {    // 7.8: the battery log
    if (g == TG_ONE)  { subIdx = (subIdx + 1) % 9; return; }
    if (g == TG_LONG) { blogReset(); blogSave(); subIdx = 0; flash("NEW LOG", 900); return; }
  }
  if (screen == S_SETTINGS && depth == 2 && itemIdx == C_TLOG && g == TG_LONG) {
    if (tlSel >= tlN) { if (fsOk) LittleFS.remove(TLOG_PATH); flash("LOG CLEARED", 1100); }
    else              { tlogDelete(tlSel); flash("DELETED", 800); }
    int keep = tlSel;
    tlogLoad();
    tlSel = keep < tlN ? keep : tlN;
    return;
  }

  // Gesture mode is driven by knocking the desk, not by the pad, so
  // the pad does nothing here at all beyond the four second hold
  // that gets you out, which is handled with the other long holds.
  //
  // It used to send from here as well as from the knock, which is a
  // double send waiting to happen: pressing a pad glued to a small
  // light robot knocks the robot. The same jolt that the shake
  // handler had to be taught to ignore would have arrived at your
  // Mac as a second tap.
  if (cfgGesture) {
    // 7.5: the pad, named as the pad, so the Mac can tell it from a knock
    // (a mute must never come from a bumped desk). A hold that turned the
    // knob was the knob, not a hold.
    if (g == TG_ONE) sendTap("t1");
    else if (g == TG_LONG) { if (!knobUsed) sendTap("th"); }
    else if (g == TG_TWO) { if (!knobUsed) sendTap("t2"); }
    knobUsed = false;
    return;
  }

  // The update screen, first, because while it is up it owns the
  // panel and returns from loop() before anything else draws.
  //
  // It was only ever driven by knocks, and knocks are off unless you
  // ask for them. So opening Check update put a screen in front of
  // you that the pad could not touch: presses fell through to the
  // carousel underneath, invisibly, and nothing would come back. Not
  // a freeze in the sense of a crash, but there was no way out of it
  // short of the battery.
  if (upState != U_OFF) {
    switch (g) {
      case TG_ONE:  updateKnock(1); return;     // the next one
      case TG_LONG: updateKnock(2); return;     // this one
      case TG_TWO:  updateKnock(3); return;     // back
      default:      upState = U_OFF; return;    // three presses leave it
    }
  }

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
  // Notifications, C3 Buddy's way. The list: a tap moves, a hold opens
  // the one picked, and holding on the last row clears them all. A
  // message: a tap goes to the next, a hold clears this one, back
  // returns to the list.
  if (screen == S_FOCUS) {                    // 7.5: the Mac screen
    int idx[4]; int k = macItems(idx);
    if (depth == 0) {
      if (g == TG_LONG && k) { depth = 1; macSel = 0; clickShrink(); return; }
      if (g == TG_LONG) return;
    } else {
      switch (g) {
        case TG_ONE:  if (k) macSel = (macSel + 1) % k; return;
        case TG_LONG:
          if (k) {
            int w = idx[macSel < k ? macSel : 0];
            char b[12]; snprintf(b, sizeof(b), "done %d", w); evtSend(b);
            if (w == 0) cards[2].on = false;
            else { cards[1].line[w - 1][0] = 0; }
            flash("DONE", 700);
            if (macItems(idx) == 0) depth = 0;
          }
          return;
        case TG_TWO: depth = 0; return;
        default: return;
      }
    }
  }
  if (screen == S_MSG && depth == 1) {
    switch (g) {
      case TG_ONE:  noteSel = (noteSel + 1) % (noteN + 1); return;
      case TG_LONG:
        if (noteSel >= noteN) {
          if (noteN) { noteN = 0; noteIdx = 0; noteSel = 0; notesDirty = true; flash("CLEARED", 1100); }
          depth = 0;
        } else {
          noteIdx = noteSel; notes[noteIdx].unread = false; notesDirty = true; depth = 2;
        }
        return;
      case TG_TWO:  depth = 0; return;
      default: return;
    }
  }
  if (screen == S_MSG && depth == 2) {
    switch (g) {
      case TG_ONE:
        if (noteN) { noteIdx = (noteIdx + 1) % noteN; notes[noteIdx].unread = false; notesDirty = true; }
        return;
      case TG_LONG:
        if (noteIdx < noteN) {
          Note& n = notes[noteIdx];
          if (!noteIsCall(n)) ancsAction(n.uid, 1);   // clear it on the phone too
          for (int i = noteIdx; i < noteN - 1; i++) notes[i] = notes[i + 1];
          noteN--; notesDirty = true;
          noteSel = noteIdx < noteN ? noteIdx : noteN;
          flash("CLEARED", 700);
        }
        depth = noteN ? 1 : 0;
        return;
      case TG_TWO:  depth = 1; noteSel = noteIdx; return;
      default: return;
    }
  }
  if (g == TG_LONG && screen == S_MSG && depth == 0 && noteN) {
    depth = 1; noteSel = 0;
    clickShrink();
    return;
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
  // 7.6: a hold on Home goes straight to Today's menu: what is new is
  // always one hold away. (Faces are chosen in Settings, Display.)
  if (g == TG_LONG && screen == S_HOME && depth == 0) {
    screen = S_TODAY; depth = 1; hubSel = 0; inHub = -1;
    clickShrink();
    return;
  }

  switch (g) {
    case TG_ONE:   knockOne();   break;
    case TG_TWO:   knockThree(); break;       // back, out one level
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
  // Knocking is off unless you switch it on, except in gesture mode,
  // where knocking IS the mode: tapping the desk next to the robot is
  // how you drive it.
  if (!cfgKnock && !tapTesting && !cfgGesture) { burst = 0; return; }
  if (burst < 4 && millis() - burstStart < TAP_WINDOW_MS) return;   // four is all there is
  uint8_t n = burst;
  burst = 0;
  // Gesture mode takes them before anything else and sends them on.
  // One knock and two, which is all the Mac has anything to do with.
  if (cfgGesture) {
    uint32_t now = millis();
    bool byPad = touchOn || (now - touchLiftAt) < SHAKE_AFTER_MS
                         || (now - touchPressAt) < SHAKE_AFTER_MS;
    if (!byPad) {                        // 7.5: named, and three counts
      if (n == 1) sendTap("k1");
      else if (n == 2) sendTap("k2");
      else if (n >= 3) sendTap("k3");
    }
    return;
  }
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
  // Three knocks go back, unless you have said the shake is the only
  // thing on the body that does. Knocking is still counted and still
  // drives everything else; it just stops being a way out.
  else if (n == 3) { if (backByKnock()) knockThree(); }
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
          holdShown = false;
          glanceUntil = 0;                       // a touch makes a glance a real wake
          awayEv(AE_TOUCH);
          if (tmrOn && !awayOn) { tmrTouch(); touchLongDone = true; touchTaps = 0; holdShown = false; }
          if (asleep && pocketLocked()) {            // 7.9: locked: only a hold wakes it
            unlocking = true; unlockAt = now; unlockShown = false;
            touchTaps = 0; touchLongDone = true;
          }
          else if (asleep) { if (touchWakes()) { wake("touch"); popupOnWake(); }
                             lastUserAt = now; touchTaps = 0; touchLongDone = true; }
          else lastUserAt = now;                     // awake: a real touch keeps it unlocked
        } else {
          touchLiftAt = now;
          // What the bar said is what happens. Nothing is decided while
          // the finger is still down, so nothing has to be undone.
          uint32_t held = now - touchPressAt;
          if (!touchLongDone) {
            if      (held < holdMs())     touchGesture(TG_ONE);    // a tap, at once
            else if (held < holdBackMs()) touchGesture(TG_LONG);   // Open
            else                          touchGesture(TG_TWO);    // Back
          }
          touchLongDone = false; holdShown = false; touchTaps = 0;
        }
      }
    } else touchEdge = 0;

    if (touchOn && !touchLongDone) {
      uint32_t held = now - touchPressAt;
      if (held > touchLongest) touchLongest = held;
      if (held >= holdMs() && !cfgGesture) holdShown = true;
      if (cfgGesture && held >= TOUCH_HOME_MS) {
        cfgGesture = false;
        touchLongDone = true; holdShown = false;
        flash("GESTURE OFF", 1100);
        Serial.println("held to four in gesture mode: back to being a robot");
      }
      // Held to four: home, and the three second count to switching
      // off, as before 6.1. Letting go now does nothing; the count runs.
      if (!cfgGesture && !sleepArmed && !awayOn && !tmrOn && held >= TOUCH_HOME_MS) {
        sleepArmed = now;
        touchLongDone = true; holdShown = false;
        screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0; faceMode = false;
        upState = U_OFF; swOn = false; swRun = false; popOn = false;
        Serial.println("held to four: home, and switching off");
      }
    }
    if (sleepArmed) {
      lastActive = now;
      if (now - sleepArmed >= TOUCH_COUNT_MS) { sleepArmed = 0; wantDeep = true; }
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
      // A drop in a bag or a pocket should not leave the robot awake
      // for the next several minutes over something nobody watched.
      // The animation is for when you are looking; if it was asleep
      // when it fell, it goes straight back to being asleep.
      bool fellAsleep = asleep;
      wake("fall"); onFall();
      if (fellAsleep) goSleep();
      return;
    }
    if (s & INT_TAP1) awayEv(AE_KNOCK);
    if ((s & INT_TAP1) && (awayOn || (asleep && !motionWakes()))) {
      if (awayOn && asleep && motionWakes()) wake("knock");   // show the message, act on nothing
    }
    else if (s & INT_TAP1) {
      if (tapTesting) { tapSeen++; tapLastSeen = now; }
      // In gesture mode a knock is the whole point and waking is not.
      // The robot sits dark on the desk and the knock goes straight
      // to the Mac; opening its eyes first would cost the battery and
      // put a lit panel next to you for no reason.
      if (!cfgGesture) { wake("knock"); lastActive = now; }
      if (!burst) burstStart = now;
      if (burst < 4) burst++;
    }
  }
  settleBurst();

  if (fabsf(amag - 1.0f) > SHAKE_G && now - lastShake > 600) {
    lastShake = now; cShake++;
    awayEv(AE_SHAKE);
    if (asleep) { if (motionWakes()) wake("shake"); return; }
    // 7.1: a shake still steps back (below), but is not "activity": a
    // bag being carried shakes, and that must not keep the screen lit.
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
      // The update screen owns the panel, so stepping back has to put
      // it down before anything else can be seen to happen.
      if (upState != U_OFF) { updateKnock(3); }
      else if (bikeEdit) bikeEdit = false;      // out of the chooser, nothing kept
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
    if (asleep) { awayEv(AE_MOVE); if (!motionWakes()) return; wake("picked up"); }
    // 7.1: moving it wakes it (above, by the Wake by setting) but does
    // not keep it awake. Only touching, knocking and using a screen do.
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
    if (asleep) { awayEv(AE_MOVE); if (!motionWakes()) return; wake("moved"); }
    // 7.1: turning it over is not an interaction either.
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
  // Gesture mode rests dark and quickly. There is nothing on that
  // screen to look at, the knock does not need it, and a lit panel on
  // a desk you are working at is a drain and a distraction. Ten
  // seconds, whatever the sleep setting says, because the setting is
  // about a robot you are using and this is not one.
  if (btPairing()) return;                       // not while it is being paired
  if (cfgGesture) {
    if (!asleep && now - lastActive > GESTURE_DARK_MS) goSleep();
  }
  else if (fuse && now - lastActive > fuse * 1000UL) goSleep();   // 0 means never
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
    <div id="upg" style="display:none">
      <button onclick="if(confirm('Install the newest release?'))act('/api/update')">Install the newest release</button>
      <button class="g" onclick="listRel()">List earlier releases</button>
      <div id="rel"></div>
    </div>
    <div id="upf">
      <p style="margin:10px 0 6px">Pick the <b>APP</b> bin (Rafiq_vX_APP_wireless_update.bin), not the FULL one. Keep this page open until it says it is restarting.</p>
      <input type="file" id="upfile" accept=".bin" style="width:100%">
      <button onclick="upFile()">Install this file</button>
      <div id="ups" style="margin-top:8px"></div>
    </div>
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
window.updTab=function(f){document.getElementById('upg').style.display=f?'none':'';document.getElementById('upf').style.display=f?'':'none';document.getElementById('ub1').className=f?'g':'';document.getElementById('ub2').className=f?'':'g'};
window.upFile=function(){const x=document.getElementById('upfile').files[0],st=document.getElementById('ups');if(!x){st.textContent='Pick a file first';return}const d=new FormData();d.append('f',x,x.name);const r=new XMLHttpRequest();r.open('POST','/ota');const t=tok();if(t)r.setRequestHeader('X-Rafiq-Token',t);r.upload.onprogress=function(e){if(e.lengthComputable)st.textContent='Sending '+Math.round(e.loaded*100/e.total)+'%'};r.onload=function(){st.textContent=r.responseText};r.onerror=function(){st.textContent='The connection dropped. If Rafiq restarted, it worked.'};st.textContent='Sending';r.send(d)};
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
  o += "\"wakeh\":" + String(cfgWakeIdx) + ",";
  o += "\"gesture\":" + String(cfgGesture ? "true" : "false") + ",";
  o += "\"gsrc\":" + String(cfgGestSrc) + ",";
  o += "\"wakehName\":\"" + String(WAKE_NAME[cfgWakeIdx]) + "\",";
  o += "\"backName\":\"" + String(BACK_NAME[cfgBack]) + "\",";
  o += "\"hadj\":" + String(cfgHijriAdj) + ",";
  o += "\"offline\":" + String(cfgOffline ? "true" : "false") + ",";
  o += "\"net\":" + String(cfgNet) + ",";
  o += "\"home\":" + String(cfgNetHome) + ",\"ws\":" + String(wsKind) + ",";
  o += "\"guard\":" + String(cfgPGuard ? "true" : "false") + ",";
  o += "\"netName\":\"" + String(NET_NAME[cfgNet]) + "\",";
  o += "\"bt\":\"" + String(btShort()) + "\",";
  o += "\"safe\":" + String(safeMode ? "true" : "false") + ",";
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

// ================================================================
//  INSTALLING FROM A FILE  (6.0)
// ================================================================
//  GitHub needs the internet. This needs only the hotspot: join it,
//  open 192.168.4.1/ota, pick the APP file. A FULL image is refused
//  by its size, because it would not fit the slot.

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
    wsTouch();
    web.send_P(200, "text/html; charset=utf-8", PAGE);
  });
  web.on("/api/state", HTTP_GET, []() { if (!authed()) { web.send(401, "application/json", "{\"ok\":false}"); return; } wsTouch(); sawMac(); apiState(); });
  // Installing from a file, for when there is no internet to reach
  // GitHub with: join the hotspot and open this. Only while the
  // hotspot is up, or for a paired Mac, so a page on the home network
  // cannot be used to put something else on the robot.
  web.on("/ota", HTTP_POST, []() {
    bool ok = otaUpOk && !Update.hasError();
    web.send(200, "text/plain", ok ? "Installed. Rafiq is restarting." : ("Not installed: " + otaUpErr));
    if (ok) { delay(800); ESP.restart(); }
  }, []() { otaUpload(); });

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
    cfgGesture = false;
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
    if (!timeOk || !nowLocal(&nowT)) {
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
    // Whether Rafiq has it muted, so the robot can say so across the
    // desk. Knowing at a glance is worth more than the press.
    if (web.hasArg("muted")) gestMuted = web.arg("muted").toInt() != 0;
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
    if (!cfgApply(web.arg("k"), web.arg("v").toInt())) {
      web.send(400, "application/json", "{\"ok\":false,\"err\":\"no such setting\"}");
      return;
    }
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
    cfgGesture = false;                        // nothing to send presses to
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
      // One list, whatever it came from. The page has always been
      // able to put a line on the robot; now it lands beside the
      // phone's notifications instead of in a screen of its own.
      {
        Note pn = {};
        pn.uid = (uint32_t)millis() | 0x80000000UL;   // never an ANCS id
        pn.cat = 0; pn.unread = true;
        strncpy(pn.app,   "Rafiq.page", sizeof(pn.app) - 1);
        strncpy(pn.title, "From the page", sizeof(pn.title) - 1);
        strncpy(pn.msg,   message.c_str(), sizeof(pn.msg) - 1);
        addNote(pn);
      }
      popupShow();
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
static void animWake(unsigned long TOTAL) {
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
      // Each flag is cleared before its fetch starts, so the flags alone
      // cannot say a fetch is still running. This can: a session must
      // not take the radio away in the middle of one.
      netBusy = true;
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
      netBusy = false;
    }
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

// ================================================================
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
  if (kind != WS_HOTSPOT && !netCount) {
    wsFailCard("No WiFi saved yet", "RAFIQ config to add");
    return false;
  }
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
  // Up to fifteen seconds for a fetch that is already under way. The
  // network task runs while this waits, because this yields.
  for (int i = 0; i < 150 && netBusy; i++) vTaskDelay(pdMS_TO_TICKS(100));
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

// A card, not a flash: it stays long enough to read, and a press
// dismisses it. Two lines of 21.
static void wsFailCard(const char* a, const char* b) {
  wake("sync");
  toastKind = "syncfail";
  toastText = String(a) + "\n" + b;
  toastUntil = millis() + 8000; toastFlash = millis(); remShowing = -1;
}

static bool gamePlaying() { return screen == S_GAMES && gState == GS_PLAY; }
// Woken by you, with something waiting that arrived while it was
// asleep. Finding Home and no sign of it is the robot keeping news to
// itself. A tap sends it away and goes Home, a hold opens it, which is
// what the popup already means, so this is the popup rather than a
// screen of its own.
static void popupOnWake() {
  if (popOn || awayOn || tmrOn) return;
  if (!popupSecs()) return;                  // popups are switched off
  if (!noteN || !notes[0].unread) return;
  if (notes[0].uid != noteSleptOn) return;   // not one you missed
  if ((screen == S_GAMES && gState == GS_PLAY) || upState != U_OFF || alertPhase != AL_NONE) return;
  noteSleptOn = 0;
  popOn = true;
  popWoke = false;                           // you are here, so it waits for you
  popUntil = millis() + popupSecs() * 1000UL;
}

static void popupShow() {
  if (awayOn || tmrOn) return;         // still kept in the list
  if (!noteN || !popupSecs()) return;
  // Never over a game being played, an update being asked, or the call
  // to prayer. It still lands in the list.
  if ((screen == S_GAMES && gState == GS_PLAY) || upState != U_OFF || alertPhase != AL_NONE) return;
  popWoke = asleep || popWoke;
  wake("notification");
  popOn = true;
  // Asleep, it is a glance: a second, then dark again, unless you
  // touch it. Awake, it stays for the popup time.
  popUntil = millis() + (popWoke ? popGlanceMs : popupSecs() * 1000UL);
}
static void popupClose(bool open) {
  popOn = false;
  if (open) {
    notes[0].unread = false; notesDirty = true;
    screen = S_MSG; depth = 2; noteIdx = 0; noteSel = 0;
    popWoke = false;
    return;
  }
  if (popWoke) { popWoke = false; goSleepQuick(); }
}
// Straight to dark, no closing eyes: after a glance nobody is watching.
static void goSleepQuick() {
  if (asleep) return;
  asleep = true;
  eyes.setIdleMode(OFF); eyes.setAutoblinker(OFF);
  screenPower(false);
  sleptAt = millis();
  nSlept++;
}
// The bar on top says who, and from which app; the body is the
// message. Two solid buttons at the bottom, apart, so what a tap and a
// hold do can be read at a glance.
static void noteHeader(const Note& n, const char* right) {
  const char* who = noteIsCall(n) ? (n.cat == CAT_MISSED ? "Missed call" : n.cat == CAT_VOICE ? "Voicemail" : "Calling")
                                  : (n.title[0] ? n.title : appShort(n));
  int room = (SCRW - 6 - (int)strlen(right) * 6 - 6) / 6;
  char l[24]; snprintf(l, sizeof(l), "%.*s", room > 0 ? (room < 23 ? room : 23) : 0, who);
  titleBar(l, right);
}
static void twoButtons(const char* a, const char* b) {
  oled.fillRoundRect(0, 53, 62, 11, 3, SSD1306_WHITE);
  oled.fillRoundRect(66, 53, 62, 11, 3, SSD1306_WHITE);
  oled.setTextSize(1); oled.setTextColor(SSD1306_BLACK);
  oled.setCursor((62 - (int)strlen(a) * 6) / 2, 55);      oled.print(a);
  oled.setCursor(66 + (62 - (int)strlen(b) * 6) / 2, 55); oled.print(b);
  oled.setTextColor(SSD1306_WHITE);
}
static void drawPopup() {
  oled.clearDisplay();
  if (!noteN) { popOn = false; return; }
  const Note& n = notes[0];
  char app[12]; snprintf(app, sizeof(app), "%.8s", appShort(n));
  if (noteIsCall(n)) marquee(n.title[0] ? n.title : "unknown", 26, 1);
  else               fitText(n.msg[0] ? n.msg : "(no text)", 14, 50, n.at);
  oled.fillRect(0, 0, SCRW, 13, SSD1306_BLACK);      // the frame over the text
  oled.fillRect(0, 51, SCRW, 13, SSD1306_BLACK);
  noteHeader(n, app);
  if (popRinging()) twoButtons("tap:answer", "hold:deny");     // 7.8.1: fits its button
  else              twoButtons("tap:close", "hold:open");
  oled.display();
}

// The list is kept in flash: on Bluetooth deep sleep is every time the
// phone is away, and a list that emptied every time would be no list.
// Times are kept as an age, so they survive the clock starting again.
#define NOTES_PATH "/notes.bin"
static void saveNotes() {
  notesDirty = false; notesSavedAt = millis();
  if (!fsOk) return;
  File f = LittleFS.open(NOTES_PATH, "w");
  if (!f) return;
  uint32_t hdr[3] = { 0x52464E31u, (uint32_t)noteN, (uint32_t)time(nullptr) };
  f.write((const uint8_t*)hdr, sizeof(hdr));
  uint32_t now = millis();
  for (int i = 0; i < noteN; i++) {
    Note c = notes[i];
    c.at = now - notes[i].at;                    // age in ms
    f.write((const uint8_t*)&c, sizeof(c));
  }
  f.close();
}
static void loadNotes() {
  if (!fsOk) return;
  File f = LittleFS.open(NOTES_PATH, "r");
  if (!f) return;
  uint32_t hdr[3];
  if (f.read((uint8_t*)hdr, sizeof(hdr)) == sizeof(hdr) && hdr[0] == 0x52464E31u) {
    uint32_t tnow = (uint32_t)time(nullptr);
    uint32_t gone = tnow >= hdr[2] ? (tnow - hdr[2]) : 0;
    if (gone > 30UL * 86400UL) gone = 30UL * 86400UL;
    int n = (int)hdr[1];
    noteN = 0;
    for (int i = 0; i < n && i < NOTE_MAX; i++) {
      Note c;
      if (f.read((uint8_t*)&c, sizeof(c)) != sizeof(c)) break;
      c.at = millis() - c.at - gone * 1000UL;    // back to this boot's millis
      notes[noteN++] = c;
    }
  }
  f.close();
}

// The message in the ordinary font, wrapped at 21 and centred. Only
// if it is too long for the screen does it fall back to scrolling.
static void awayText1(const char* t) {
  int n = wrapInto(t, 21, REM_LN);
  if (n * 10 > 50) { fitText(t, 2, 50, 0); return; }
  int y = 2 + (50 - n * 10) / 2;
  for (int i = 0; i < n; i++) ctr(remLines[i], y + i * 10, 1);
}
// Who to get in touch with: the second half of every showing.
// Small pictures, seven by seven: a person, a handset, an envelope.
static const uint8_t ICO_PERSON[7] = { 0x1C, 0x3E, 0x3E, 0x1C, 0x00, 0x7F, 0x7F };
static const uint8_t ICO_PHONE[7]  = { 0x60, 0x70, 0x30, 0x18, 0x0D, 0x07, 0x03 };
static const uint8_t ICO_MAIL[7]   = { 0x7F, 0x63, 0x55, 0x49, 0x41, 0x7F, 0x00 };
static void icon7(int x, int y, const uint8_t* rows) {
  for (int j = 0; j < 7; j++)
    for (int i = 0; i < 7; i++)
      if (rows[j] & (0x40 >> i)) oled.drawPixel(x + i, y + j, SSD1306_WHITE);
}
// Who to get in touch with: the second half of every showing. A small
// heading and a line, then one thing to a row with its picture.
static void drawContact() {
  oled.clearDisplay();
  ctr("GET IN TOUCH", 1, 1);
  oled.drawFastHLine(0, 11, SCRW, SSD1306_WHITE);
  icon7(4, 17, ICO_PERSON); at(16, 17, OWNER_NAME);
  icon7(4, 29, ICO_PHONE);  at(16, 29, OWNER_PHONE_SHOW);
  icon7(4, 41, ICO_MAIL);
  const char* m = OWNER_MAIL;
  const char* atp = strchr(m, '@');
  if (atp) {
    char user[22]; snprintf(user, sizeof(user), "%.*s", (int)(atp - m), m);
    at(16, 41, user);
    at(16, 51, atp);
  } else at(16, 41, m);
  oled.display();
}
static void drawAway() {
  // The first half of the time it is lit, the message; the second
  // half, who to get in touch with.
  if ((int32_t)(millis() - lastActive) >= (int32_t)(awayShowMs / 2)) { drawContact(); return; }
  oled.clearDisplay();
  awayText1(awayText.length() ? awayText.c_str() : "Away");
  if (fb.ok || timeOk) { loadBits(); ctr(fb.hm, 56, 1); }
  oled.display();
}
static void awayFlush(bool all) {
  uint32_t now = millis();
  for (int k = 0; k < AE_N; k++) {
    if (!aeCount[k] || (!all && now - aeLast[k] < 20000)) continue;
    char l[24];
    if (aeCount[k] == 1) snprintf(l, sizeof(l), "%s", AE_NAME[k]);
    else                 snprintf(l, sizeof(l), "%s x%u", AE_NAME[k], (unsigned)aeCount[k]);
    tlogAdd(l);
    aeCount[k] = 0;
  }
}
static void awaySet(bool on, const char* text) {
  if (text && *text) { awayText = String(text).substring(0, 160); prefs.putString("awayt", awayText); }
  if (on == awayOn) {
    if (on) { awayListenUntil = millis() + AWAY_LISTEN_SENT_MS; awayShowMs = AWAY_SENT_SHOW_MS; wake("away"); lastActive = millis(); }
    return;
  }
  awayOn = on;
  prefs.putBool("away", awayOn);
  if (on) {
    tlogAdd("Away on");
    popOn = false; pgUntil = 0; findUntil = 0; upState = U_OFF;
    awayListenUntil = millis() + AWAY_LISTEN_SENT_MS;
    awayShowMs = AWAY_SENT_SHOW_MS;
    wake("away");
  } else {
    awayFlush(true);
    tlogAdd("Away off");
    if (awayAuto) { awayAuto = false; prefs.putBool("awaya", false); }
    awayListenUntil = 0; rtcAwayDeep = 0;
    screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0;
    wake("shortcut");
    flash("WELCOME BACK", 1200);
  }
}

// Off, with only the pad and the accelerometer able to wake it, as the
// Wake by setting allows. Never returns.
static void awayDeepArm(bool pause) {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (pause) {                                   // a move just woke it: let it settle
    rtcAwayDeep = 2;
    esp_sleep_enable_timer_wakeup(20ULL * 1000000ULL);
  } else {
    rtcAwayDeep = 1;
    rtcAwayCheck = 0;
    if (awayAuto) {
      // Every 3 minutes for the first hour, every 5 after: a short look
      // for the phone, the screen dark, then off again if it is not there.
      uint32_t tnow = (uint32_t)time(nullptr);
      if (!rtcAwaySince) rtcAwaySince = tnow;
      uint32_t chk = (tnow - rtcAwaySince < 3600) ? 180 : 300;
      esp_sleep_enable_timer_wakeup((uint64_t)chk * 1000000ULL);
      rtcAwayCheck = 1;
    }
    if (touchWakes())
      esp_deep_sleep_enable_gpio_wakeup(BIT(TOUCH_PIN), touchRest ? ESP_GPIO_WAKEUP_GPIO_LOW
                                                                  : ESP_GPIO_WAKEUP_GPIO_HIGH);
    if (motionWakes() && intWired)
      esp_deep_sleep_enable_gpio_wakeup(BIT(TAP_INT_PIN), ESP_GPIO_WAKEUP_GPIO_HIGH);
  }
  esp_deep_sleep_start();
}
// Away's eyes: open or close in six frames, about a fifth of a second.
static void awayEyes(bool open) {
  for (int i = 0; i <= 5; i++) {
    int pct = open ? i * 20 : 100 - i * 20;
    oled.clearDisplay();
    calmEyes(pct, 0, 30);
    oled.display();
    delay(30);
  }
  if (open) delay(120);                  // a beat with them open, then the message
}
static void awayDeepGo() {
  awayFlush(true);
  if (!awayQuietDeep) tlogAdd("Away asleep");
  awayQuietDeep = false;
  prefs.putBool("trest", touchRest);
  const char* tz = getenv("TZ");
  snprintf(rtcTz, sizeof(rtcTz), "%s", tz ? tz : "");
  oled.clearDisplay(); oled.display();
  screenPower(false);
  bleOff();
  if (cfgNet == NET_WIFI) { WiFi.disconnect(true, false); WiFi.mode(WIFI_OFF); }
  if (intWired && adxl) {                        // the same activity watch as tamper
    wReg(adxl, 0x2E, 0x00);
    wReg(adxl, 0x24, 6);                         // 375 mg
    wReg(adxl, 0x27, 0xF0);                      // activity, AC coupled, x y z
    wReg(adxl, 0x2F, 0x00);                      // to INT1
    wReg(adxl, 0x2E, 0x10);
    rReg(adxl, 0x30);
    delay(20);
  }
  Serial.println("away: listening is over, sleeping until touched or moved");
  awayDeepArm(false);
}
// First thing in setup on a wake from Away's sleep: the message for
// three seconds, written down, and off again. No radio, nothing else.
static void awayDeepWake(bool timer) {
  if (rtcTz[0]) { setenv("TZ", rtcTz, 1); tzset(); }
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  prefs.begin("nexus", false);
  touchRest  = prefs.getBool("trest", false);
  cfgWakeBy  = constrain(prefs.getInt("wakeby", 0), 0, 2);
  cfg12h     = prefs.getBool("h12", false);
  awayText   = prefs.getString("awayt", "Away");
  cfgBright  = prefs.getInt("bri", cfgBright);
  awayAuto   = prefs.getBool("awaya", false);
  intWired   = true;                              // it was, or this wake could not have happened
  pinMode(TOUCH_PIN, INPUT);
  pinMode(TAP_INT_PIN, INPUT_PULLDOWN);
  if (timer) {                                   // the pause after a move is over
    rReg(0x53, 0x30);
    delay(5);
    if (digitalRead(TAP_INT_PIN)) awayDeepArm(true);
    awayDeepArm(false);
  }
  uint64_t st = esp_sleep_get_gpio_wakeup_status();
  bool moved = (st & BIT(TAP_INT_PIN)) && !(st & BIT(TOUCH_PIN));
  fsOk = LittleFS.begin(false);
  tlogAdd(moved ? "Moved (away)" : "Touched (away)");
  // Any touch shows it, no hold: whoever finds it needs to read it,
  // and would not know to hold.
  if (oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) {
    oled.setTextWrap(false); oled.setTextColor(SSD1306_WHITE);
    applyBright();
    awayEyes(true);
    oled.clearDisplay();
    awayText1(awayText.c_str());
    time_t t = time(nullptr);
    if (t > 1700000000) {
      struct tm lt; localtime_r(&t, &lt);
      int h = lt.tm_hour;
      if (cfg12h) { h %= 12; if (!h) h = 12; }
      char c[8]; snprintf(c, sizeof(c), cfg12h ? "%d:%02d" : "%02d:%02d", h, lt.tm_min);
      ctr(c, 56, 1);
    }
    oled.display();
    delay(AWAY_DEEP_SHOW_MS / 2);
    drawContact();
    delay(AWAY_DEEP_SHOW_MS / 2);
    awayEyes(false);
    oled.clearDisplay(); oled.display();
    oled.ssd1306_command(SSD1306_DISPLAYOFF);
  }
  LittleFS.end();
  // Do not go back to sleep with the pad still pressed, or it wakes at
  // once: wait for it to be let go, a few seconds at most.
  for (int i = 0; i < 50 && digitalRead(TOUCH_PIN) != touchRest; i++) delay(100);
  rReg(0x53, 0x30);                              // let INT1 go
  delay(5);
  if (digitalRead(TAP_INT_PIN) || digitalRead(TOUCH_PIN) != touchRest) awayDeepArm(true);
  awayDeepArm(false);
}

static void tmrSet(long secs) {
  uint32_t now = millis();
  if (secs <= 0) {                               // taken to nothing: it is done
    tmrOn = true; tmrDone = true; tmrDoneAt = now; tmrEnd = now;
    wake("timer");
    return;
  }
  tmrOn = true; tmrDone = false; tmrPinned = false;
  tmrStart = now; tmrEnd = now + (uint32_t)secs * 1000UL;
  popOn = false;
  wake("timer");
  Serial.printf("timer: %ld s\n", secs);
}
static long tmrLeft() {                          // seconds, never below zero
  long ms = (long)(tmrEnd - millis());
  return ms > 0 ? (ms + 999) / 1000 : 0;
}
static void tmrStop(const char* why) {
  tmrOn = false; tmrDone = false; tmrPinned = false;
  screen = S_HOME; depth = 0;
  wake("timer");
  if (why) flash(why, 1100);
}
// "timer 15", "timer +5", "timer -10". Minutes.
static bool tmrCommand(const char* c) {
  if (strncmp(c, "timer", 5) || (c[5] && c[5] != ' ')) return false;
  const char* q = c + 5;
  while (*q == ' ') q++;
  if (!*q) return false;
  char sign = (*q == '+' || *q == '-') ? *q : 0;
  if (sign) q++;
  while (*q == ' ') q++;
  if (!isdigit((unsigned char)*q)) return false;
  long m = atol(q);
  if (m > 600) m = 600;                          // ten hours is plenty
  if (!sign) { tmrSet(m * 60); return true; }
  long left = (tmrOn && !tmrDone) ? tmrLeft() : 0;
  tmrSet(sign == '+' ? left + m * 60 : left - m * 60);
  return true;
}
// The rhythm: over five minutes left, five seconds lit each minute;
// one to five minutes, five on and fifteen off; the last minute, always.
// A touch pins it on; another lets it go back to the rhythm.
static bool tmrWantsScreen() {
  if (tmrDone || tmrPinned) return true;
  long left = tmrLeft();
  if (left <= 60) return true;
  uint32_t t = (millis() - tmrStart) / 1000;
  if (left <= 300) return (t % 20) < 5;
  return (t % 60) < 5;
}
static void tmrTouch() {
  if (tmrDone) { tmrStop(nullptr); return; }     // the flash is seen: done
  tmrPinned = !tmrPinned;
}
static void tmrTick() {
  if (!tmrOn) return;
  uint32_t now = millis();
  if (!tmrDone && (long)(now - tmrEnd) >= 0) {
    tmrDone = true; tmrDoneAt = now;
    Serial.println("timer: done");
  }
  bool want = tmrWantsScreen();
  if (want && asleep)   wake("timer");
  if (!want && !asleep) goSleepQuick();
}
static void drawTimer() {
  oled.clearDisplay();
  if (tmrDone) {
    bool b = (millis() / 350) % 2;
    if (b) oled.fillRect(0, 0, SCRW, SCRH, SSD1306_WHITE);
    oled.setTextColor(b ? SSD1306_BLACK : SSD1306_WHITE);
    ctr("TIME'S UP", 18, 2);
    ctr("touch to stop", 46, 1);
    oled.setTextColor(SSD1306_WHITE);
    oled.display();
    return;
  }
  // 7.3: a ring that fills as the time goes, the time inside it, and
  // what is left and when it ends beside it. Small type, laid out.
  ringInit();
  long left = tmrLeft();
  long total = (long)((tmrEnd - tmrStart) / 1000);
  char b[16];
  at(0, 1, "TIMER");
  if (tmrPinned) oled.fillCircle(36, 4, 2, SSD1306_WHITE);   // held on by a touch
  if (total >= 3600) snprintf(b, sizeof(b), "%ldh %02ldm", total / 3600, (total / 60) % 60);
  else               snprintf(b, sizeof(b), "%ld min", (total + 59) / 60);
  at(SCRW - (int)strlen(b) * 6, 1, b);
  oled.drawFastHLine(0, 11, SCRW, SSD1306_WHITE);
  const int cx = 32, cy = 38, r = 23;
  for (int k = 0; k < 180; k += 4) ringPx(cx, cy, r, k);          // the track, dotted
  int upto = total > 0 ? (int)(180L * (total - left) / total) : 180;
  for (int k = 0; k < upto; k++) { ringPx(cx, cy, r, k); ringPx(cx, cy, r - 1, k); ringPx(cx, cy, r - 2, k); }
  if (left >= 3600) snprintf(b, sizeof(b), "%ldh%02ld", left / 3600, (left / 60) % 60);
  else              snprintf(b, sizeof(b), "%02ld:%02ld", left / 60, left % 60);
  at(cx - (int)strlen(b) * 3, cy - 3, b);
  at(66, 22, "left");
  fmtDur(left, b, sizeof(b));
  at(66, 32, b);
  if (timeOk) {
    at(66, 46, "ends");
    fmtClock(time(nullptr) + left, b, sizeof(b));
    at(66, 56, b);
  }
  oled.display();
}

// A ring, worked out once: 180 points, 2 degrees apart, cos and sin
// times 1024. The C3 has no floating point unit, so nothing per frame.
static int16_t ringC[180], ringS[180];
static bool ringReady = false;
static void ringInit() {
  if (ringReady) return;
  for (int k = 0; k < 180; k++) {
    float t = (k * 2 - 90) * 3.14159265f / 180.0f;
    ringC[k] = (int16_t)lroundf(cosf(t) * 1024.0f);
    ringS[k] = (int16_t)lroundf(sinf(t) * 1024.0f);
  }
  ringReady = true;
}
static void ringPx(int cx, int cy, int r, int k) {
  oled.drawPixel(cx + (r * ringC[k] + (ringC[k] >= 0 ? 512 : -512)) / 1024,
                 cy + (r * ringS[k] + (ringS[k] >= 0 ? 512 : -512)) / 1024, SSD1306_WHITE);
}
static void fmtDur(long s, char* b, size_t n) {
  if (s >= 3600)     snprintf(b, n, "%ldh %02ldm", s / 3600, (s / 60) % 60);
  else if (s >= 60)  snprintf(b, n, "%ldm %02lds", s / 60, s % 60);
  else               snprintf(b, n, "%lds", s);
}
static void fmtClock(time_t t, char* b, size_t n) {
  struct tm lt; localtime_r(&t, &lt);
  if (cfg12h) snprintf(b, n, "%d:%02d %s", lt.tm_hour % 12 ? lt.tm_hour % 12 : 12, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM");
  else        snprintf(b, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
}

// One setting, by the name the page and the apps use. WiFi and
// Bluetooth both come here (7.4), so they can never disagree.
static bool cfgApply(const String& k, int v) {
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
    // WiFi is a session, never a home. Asking for it here keeps this
    // session going until a restart; asking for Bluetooth or Off makes
    // that home and ends the session once this answer has gone out.
    else if (k == "net" || k == "offl") {
                            int want = (k == "offl") ? (v ? NET_OFF : NET_BT) : constrain(v, 0, NET_N - 1);
                            if (want == NET_WIFI) wsStart(WS_MANUAL);
                            else {
                              cfgNetHome = want; prefs.putInt("net", cfgNetHome);
                              if (cfgNet == NET_WIFI) wsEndWant = true;
                              else { cfgNet = want; if (want == NET_BT) bleOn(); else bleOff(); }
                            } }
    else if (k == "hadj") { cfgHijriAdj = constrain(v, -2, 2);           prefs.putInt("hadj", cfgHijriAdj); }
    else if (k == "shake"){ cfgBack = v ? BACK_BOTH : BACK_KNOCK;        prefs.putInt("back", cfgBack); }
    else if (k == "back") { cfgBack = constrain(v, 0, BACK_N - 1);       prefs.putInt("back", cfgBack); }
    else if (k == "wakeh"){ cfgWakeIdx = constrain(v, 0, WAKE_N - 1);    prefs.putInt("wakeh", cfgWakeIdx); }
    // Not written to flash. See cfgGesture: it belongs to the Mac.
    else if (k == "gest") { cfgGesture = (v != 0); if (cfgGesture) wake("gesture"); }
    else if (k == "gsrc") { cfgGestSrc = constrain(v, 0, GSRC_N - 1); }
    else if (k == "deepi"){ cfgDeepIdx  = constrain(v, 0, DEEP_N - 1);   prefs.putInt("deepi", cfgDeepIdx); }
    // Sent in hundredths, because the form only carries whole numbers.
    else if (k == "bfull"){ battFull    = constrain(v / 100.0f, 3.90f, 4.30f); prefs.putFloat("bfull", battFull); }
    else if (k == "night") { cfgNight = v != 0; prefs.putBool("night", cfgNight); }
    else if (k == "plock") { cfgPLock = constrain(v, 0, 3); prefs.putInt("plock", cfgPLock); }
    else if (k == "blog")  { cfgBlog = v != 0; prefs.putBool("blog", cfgBlog); if (cfgBlog && blog.magic != BLOG_MAGIC) blogReset(); }
    else if (k == "blogv") { cfgBlogV = constrain(v, 0, 3); prefs.putInt("blogv", cfgBlogV); blogArmed = true; }
    else if (k == "cap")   { cfgCap = constrain(v, 50, 5000); prefs.putInt("cap", cfgCap); }
    else if (k == "blogreset") { blogReset(); blogSave(); }
    else if (k == "bed")   { cfgBed = constrain(v, 0, 1439); prefs.putInt("bed", cfgBed); }
    else return false;
  return true;
}

// The settings the apps show, with /api/state's own names, so the apps
// read this exactly as they read that. Kept under 512 bytes (the most a
// Bluetooth value can be): networks go last and stop when it is full.
static String cfgJson() {
  String o;
  o.reserve(512);
  long dl = dndUntil && (long)(dndUntil - millis()) > 0 ? (long)(dndUntil - millis()) / 1000 : 0;
  o += "{\"fw\":\"" FW_VERSION "\",\"bri\":" + String(cfgBright) + ",\"face\":" + String(cfgFace) +
       ",\"slpi\":" + String(cfgSleepIdx) + ",\"popi\":" + String(cfgPopupIdx) +
       ",\"eye\":" + String(cfgEyes) + ",\"tap\":" + String(cfgTap) +
       ",\"deepi\":" + String(cfgDeepIdx) + ",\"btpl\":" + String(cfgBikeTpl) +
       ",\"knock\":" + String(cfgKnock ? "true" : "false") +
       ",\"shake\":" + String(cfgBack == BACK_BOTH ? "true" : "false") +
       ",\"bike\":" + String(cfgBike ? "true" : "false") +
       ",\"follow\":" + String(cfgFollow ? "true" : "false") +
       ",\"relax\":" + String(relaxOn ? "true" : "false") +
       ",\"gesture\":" + String(cfgGesture ? "true" : "false") +
       ",\"deepOff\":" + String(deepOff ? "true" : "false") +
       ",\"autoUp\":" + String(cfgAutoUp ? "true" : "false") +
       ",\"turn\":" + String(cfgAutoTurn ? "true" : "false") +
       ",\"offline\":" + String(cfgNetHome == NET_OFF ? "true" : "false") +
       ",\"intWired\":" + String(intWired ? "true" : "false") +
       ",\"night\":" + String(cfgNight ? "true" : "false") +
       ",\"bed\":" + String(cfgBed) + ",\"npush\":" + String(nightPush) +
       ",\"dndLeft\":" + String(dl) +
       ",\"battPct\":" + String(isnan(battV) ? -1 : battPct(battV)) +
       ",\"battFull\":" + String(battFull, 2) +
       ",\"netMax\":" + String(NET_MAX) + ",\"nets\":[";
  for (int i = 0; i < netCount; i++) {
    String nm = String(netSsid[i]); nm.replace("\\", " "); nm.replace("\"", "'");
    String one = String(i ? "," : "") + "{\"ssid\":\"" + nm + "\",\"on\":false}";
    if (o.length() + one.length() + 3 > 510) break;
    o += one;
  }
  o += "]}";
  return o;
}

// "!" commands: only from the apps, never a Shortcut, because they reach
// settings, networks and the pointer. They do what the HTTP calls of the
// same names do.
static int remBatch = 0;
static uint32_t remBatchAt = 0, remBatchSoonest = 0;
static void appBang(char* c, uint16_t conn) {
  char* a = c + 1;                                 // after the "!"
  char* sp = strchr(a, ' ');
  String verb = sp ? String(a).substring(0, sp - a) : String(a);
  const char* rest = sp ? sp + 1 : "";
  if (verb == "cfg") {
    char k[12] = ""; int v = 0;
    if (sscanf(rest, "%11s %d", k, &v) == 2) cfgApply(String(k), v);
  } else if (verb == "relax") {
    relaxOn = atoi(rest) != 0; relaxUntil = 0;
    if (relaxOn) { relaxKind = 0; relaxNext = millis() + 30000UL; wake("relax"); }
  } else if (verb == "follow") {
    cfgFollow = atoi(rest) != 0;
    prefs.putBool("follow", cfgFollow);
    if (cfgFollow) wake("follow"); else curUntil = 0;
    // a quicker link while the eyes follow, back to the light one after
    NimBLEServer* sv = NimBLEDevice::getServer();
    // The timeout stays at 6 s in both. Four seconds was outside the
    // window Apple allows, so the whole request was refused and Follow
    // ran at the ordinary rhythm, which is the thing it most needs not
    // to do.
    if (sv) sv->updateConnParams(conn, cfgFollow ? 24 : 72, cfgFollow ? 40 : 96, cfgFollow ? 0 : 4, 600);
  } else if (verb == "dnd") {
    int mm = constrain(atoi(rest), 0, 480);
    dndUntil = mm ? millis() + (unsigned long)mm * 60000UL : 0;
    dndLine = (int)random(DND_N);
    if (mm) wake("break");
  } else if (verb == "busy") {
    int cm = 0, mi = 0, mu = -1;
    sscanf(rest, "%d %d %d", &cm, &mi, &mu);
    if ((cm || mi) && !(busyCam || busyMic)) { busyAt = millis(); wake("live"); }
    busyCam = cm; busyMic = mi;
    if (mu >= 0) gestMuted = mu != 0;
  } else if (verb == "tap") {
    cfgTap = constrain(atoi(rest), 0, TAP_N - 1); prefs.putInt("tap", cfgTap); applyTap();
  } else if (verb == "deep") {
    deepOff = atoi(rest) != 0; prefs.putBool("nodeep", deepOff);
  } else if (verb == "autoup") {
    cfgAutoUp = atoi(rest) != 0; prefs.putBool("autoup", cfgAutoUp);
  } else if (verb == "turn") {
    cfgAutoTurn = atoi(rest) != 0; prefs.putBool("turn", cfgAutoTurn);
  } else if (verb == "bike") {                     // plate \x1F make \x1F model \x1F owner
    char* f[4] = { (char*)rest, nullptr, nullptr, nullptr };
    int k = 1;
    for (char* p = (char*)rest; *p && k < 4; p++) if (*p == 0x1F) { *p = 0; f[k++] = p + 1; }
    struct { char* dst; size_t n; const char* pref; } D[] = {
      { bikePlate, sizeof(bikePlate), "bplate" }, { bikeMake, sizeof(bikeMake), "bmake" },
      { bikeModel, sizeof(bikeModel), "bmodel" }, { bikeOwner, sizeof(bikeOwner), "bowner" } };
    for (int i = 0; i < k && i < 4; i++) {
      if (!f[i] || !*f[i]) continue;
      snprintf(D[i].dst, D[i].n, "%s", f[i]);
      prefs.putString(D[i].pref, D[i].dst);
    }
  } else if (verb == "net") {                      // add ssid\x1Fpass | del i | up i
    if (!strncmp(rest, "add ", 4)) {
      String ss = String(rest + 4), pw = "";
      int sep = ss.indexOf((char)0x1F);
      if (sep >= 0) { pw = ss.substring(sep + 1); ss = ss.substring(0, sep); }
      ss.trim();
      if (ss.length()) {
        int at = -1;
        for (int i = 0; i < netCount; i++) if (ss == netSsid[i]) { at = i; break; }
        if (at < 0 && netCount < NET_MAX) at = netCount++;
        if (at >= 0) {
          strncpy(netSsid[at], ss.c_str(), 32); netSsid[at][32] = 0;
          strncpy(netPass[at], pw.c_str(), 64); netPass[at][64] = 0;
          saveNets(); netReload = true;
          flash("NETWORK SAVED", 1000);
        } else flash("NETWORKS FULL", 1200);
      }
    } else if (!strncmp(rest, "del ", 4)) {
      int d = atoi(rest + 4);
      if (d >= 0 && d < netCount) {
        for (int i = d; i < netCount - 1; i++) { strncpy(netSsid[i], netSsid[i + 1], 33); strncpy(netPass[i], netPass[i + 1], 65); }
        netCount--; netSsid[netCount][0] = netPass[netCount][0] = 0;
        saveNets(); netReload = true;
      }
    } else if (!strncmp(rest, "up ", 3)) {
      int u = atoi(rest + 3);
      if (u > 0 && u < netCount) {
        char ts[33], tp[65];
        strncpy(ts, netSsid[u], 33); strncpy(tp, netPass[u], 65);
        strncpy(netSsid[u], netSsid[u - 1], 33); strncpy(netPass[u], netPass[u - 1], 65);
        strncpy(netSsid[u - 1], ts, 33); strncpy(netPass[u - 1], tp, 65);
        saveNets(); netReload = true;
      }
    }
  } else if (verb == "card") {          // slot title \x1F line \x1F line \x1F line \x1F bar
    int slot = atoi(rest);
    if (slot < 0 || slot > 2) return;
    const char* p = strchr(rest, ' ');
    Card& cd = cards[slot];
    memset(&cd, 0, sizeof(cd)); cd.bar = -1;
    if (!p || !p[1]) return;                       // no text: the card is gone
    char* f[5] = { (char*)p + 1, nullptr, nullptr, nullptr, nullptr };
    int k = 1;
    for (char* q = (char*)p + 1; *q && k < 5; q++) if (*q == 0x1F) { *q = 0; f[k++] = q + 1; }
    snprintf(cd.title, sizeof(cd.title), "%.21s", f[0]);
    for (int i = 0; i < 3; i++) if (k > i + 1 && f[i + 1]) snprintf(cd.line[i], sizeof(cd.line[i]), "%.23s", f[i + 1]);
    if (k > 4 && f[4]) cd.bar = (int8_t)constrain(atoi(f[4]), -1, 100);
    cd.on = true;
  } else if (verb == "knob") {
    knobOn = atoi(rest) != 0;
  } else if (verb == "walk") {
    walkOn = atoi(rest) != 0;
  } else if (verb == "bye") {                      // the Mac is going to sleep: not left behind
    macBye = true;
  } else if (verb == "dim") {
    macDim = atoi(rest) != 0; applyBright();
  } else if (verb == "mute" || verb == "vip") {    // the whole list, \x1F between
    bool vip = verb == "vip";
    char tmp[MUTE_N][20]; int n = 0;
    const char* a = rest;
    while (*a && n < (vip ? VIP_N : MUTE_N)) {
      const char* b = strchr(a, 0x1F); size_t L = b ? (size_t)(b - a) : strlen(a);
      if (L) { snprintf(tmp[n], 20, "%.*s", (int)(L < 19 ? L : 19), a); n++; }
      if (!b) break; a = b + 1;
    }
    if (vip) { vipN = n; for (int i = 0; i < n; i++) memcpy(vipWords[i], tmp[i], 20); listSave("vipw", tmp, n); }
    else     { mutedN = n; for (int i = 0; i < n; i++) memcpy(mutedApps[i], tmp[i], 20); listSave("mutea", mutedApps, mutedN); }
  } else if (verb == "pt") {                       // five prayer times, minutes after midnight
    int v[5];
    if (sscanf(rest, "%d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5) {
      for (int i = 0; i < 5; i++) prayerMin[i] = constrain(v[i], 0, 1439);
      struct tm t; if (nowLocal(&t)) prayerDay = t.tm_yday;
      savePrayer();
    }
  } else if (verb == "read") {
    // A story from the Mac, in pieces.
    //
    // Up to 6000 characters will not fit in one write, so it arrives as
    // begin, a run of chunks, then end with the length. The length is
    // the point: the queue here is four deep and the chunks could in
    // principle outrun it, and a story that lost a piece in the middle
    // would be saved looking perfectly fine. If the count does not
    // match, nothing is stored and the Mac is told what arrived.
    //
    // 0x1E stands in for a newline. The Mac turns real control
    // characters into spaces before sending, which would flatten the
    // blank line that separates the title from the story.
    if (!strcmp(rest, "begin")) {
      rdRx = ""; rdRxOn = true;
      storyState = "Receiving";
      // A story is twenty or so pieces and every one of them is a round
      // trip, so at the resting rhythm of 90 to 120 ms it takes most of
      // a minute. The same quick rhythm the update asks for, and within
      // Apple's rules so it is actually granted.
      {
        NimBLEServer* sv = NimBLEDevice::getServer();
        if (sv && conn != 0xFFFF) sv->updateConnParams(conn, 12, 24, 0, 600);
      }
      rdConn = conn;
      evtSend("read ok");
    } else if (!strncmp(rest, "+ ", 2)) {
      if (!rdRxOn) return;
      // Fenced with bars by the Mac, because the thing that cleans a
      // command there also trims its ends, and a piece that happened to
      // finish on a space would arrive a character short.
      const char* q = rest + 2;
      size_t n = strlen(q);
      if (n < 2 || q[0] != '|' || q[n - 1] != '|') { evtSend("read ok"); return; }
      for (size_t i = 1; i + 1 < n; i++) {
        if (rdRx.length() >= STORY_MAX_CHARS) break;
        rdRx += (q[i] == 0x1E) ? '\n' : q[i];
      }
      evtSend("read ok");
    } else if (!strncmp(rest, "end ", 4)) {
      if (!rdRxOn) return;
      rdRxOn = false;
      uint32_t want = strtoul(rest + 4, nullptr, 10);
      if (want != rdRx.length()) {
        char b[32]; snprintf(b, sizeof(b), "read err %u", (unsigned)rdRx.length());
        evtSend(b);
        storyState = "Came through short";
        rdRx = "";
        rdRhythmBack();
        return;
      }
      addRead(rdRx);
      rdRx = "";
      loadShelf();
      rdRhythmBack();
      // Say so. A story arriving with nothing on the screen is a story
      // you do not know you have.
      storyState = "Ready";
      flash("NEW STORY", 1200);
      evtSend("read done");
    } else if (!strcmp(rest, "abort")) {
      rdRxOn = false; rdRx = "";
      rdRhythmBack();
    }
  } else if (verb == "ota") {
    if (!strncmp(rest, "begin ", 6)) {
      uint32_t sz = strtoul(rest + 6, nullptr, 10);
      if (otaOn) Update.abort();
      if (sz < 100000 || !Update.begin(sz)) { evtSend("ota err begin"); flash("NO ROOM", 1200); return; }
      otaOn = true; otaErr = false; otaSize = sz; otaGot = 0; otaLastAt = millis(); otaPctSent = -1;
      otaConn = conn;
      NimBLEServer* sv = NimBLEDevice::getServer();
      // 15 to 30 ms, never skipping. Not 7.5 to 15: Apple requires the
      // minimum to be at least 15 ms AND a multiple of 15 ms, with the
      // maximum at least 15 ms above it, and a request that breaks any
      // of that is simply refused. The refusal is silent, so the link
      // stayed at the ordinary 90 to 120 ms with a latency of 4, which
      // is why sending 1.5 MB took as long as it did while the hotspot,
      // on WiFi, finished in moments.
      if (sv) sv->updateConnParams(conn, 12, 24, 0, 600);
      pmSet(false);
      wake("update");
      evtSend("ota ready");
    } else if (!strcmp(rest, "end")) {
      if (!otaOn) return;
      if (otaErr || otaGot != otaSize) { otaStop(otaErr ? "write" : "short"); return; }
      if (!Update.end(true)) { otaOn = false; evtSend("ota err verify"); flash("UPDATE BAD", 1400); return; }
      otaOn = false;
      evtSend("ota ok");
      flash("UPDATED", 900);
      delay(600);
      ESP.restart();
    } else if (!strcmp(rest, "abort")) {
      otaStop("abort");
    }
  } else if (verb == "night") {                    // "!night 60": tonight, an hour later
    nightPush = constrain(nightPush + constrain(atoi(rest), 0, 240), 0, 360);
    nightCardUntil = 0;
    char b[24]; snprintf(b, sizeof(b), "SLEEP AT %02d:%02d", ((cfgBed + nightPush) % 1440) / 60, ((cfgBed + nightPush) % 1440) % 60);
    flash(b, 1300);
  } else if (verb == "remclear") {
    remCount = 0; remIdx = 0; saveRems();
  } else if (verb == "rem") {                      // wall-seconds done text
    // The app sends the time as a wall clock (local seconds). The robot's
    // own clock is either the phone's wall clock (Bluetooth) or real time
    // with a zone (WiFi); the difference between the two is the offset.
    unsigned long wall = 0; int done = 0, used = 0;
    if (sscanf(rest, "%lu %d %n", &wall, &done, &used) >= 2 && used > 0 && wall > 1700000000UL) {
      time_t now = time(nullptr); struct tm lt; localtime_r(&now, &lt);
      long off = (long)(utcFromTm(&lt) - now);
      uint32_t at = (uint32_t)((long)wall - off);
      String txt = String(rest + used); txt.trim();
      bool dup = false;
      for (int k = 0; k < remCount; k++)
        if (rems[k].at / 60 == at / 60 && txt == rems[k].text) { dup = true; break; }
      if (!dup && txt.length() && addRem(txt.c_str(), at)) {
        if (done) rems[remCount - 1].done = true;
        remBatch++; remBatchAt = millis();
        if (!done && (!remBatchSoonest || at < remBatchSoonest)) remBatchSoonest = at;
      }
    }
  }
}

// Lean left or right sends once, then waits to come back to the middle.
// Holding the pad with the knob on turns tilt into a dial instead: the
// angle from where the hold began, sent as it changes.
static void gestTilt() {
  if (!cfgGesture || !linkN) { leanState = 0; return; }
  uint32_t now = millis();
  if (now - knobAt < 80) return;
  knobAt = now;
  float tx, ty; tiltRead(tx, ty);
  if (touchOn && knobOn) {
    float deg = asinf(constrain(tx, -0.99f, 0.99f)) * 57.3f;
    if (!knobUsed && fabsf(deg - knobRef) < 6) { if (knobLast == 0) knobRef = deg; }
    int rel = (int)lroundf(deg - knobRef);
    if (abs(rel) >= 6) knobUsed = true;
    if (knobUsed && abs(rel - knobLast) >= 3) {
      knobLast = rel;
      char b[16]; snprintf(b, sizeof(b), "kv %d", rel);
      evtSend(b);
    }
    return;
  }
  knobLast = 0;
  if (leanState == 0 && tx > 0.35f)       { leanState = 1;  sendTap("lr"); }
  else if (leanState == 0 && tx < -0.35f) { leanState = -1; sendTap("ll"); }
  else if (leanState != 0 && fabsf(tx) < 0.15f) leanState = 0;
}

// ---- app filter and VIPs ----
static void listLoad(const char* key, char (*dst)[20], int max, int& n) {
  n = 0;
  String v = prefs.getString(key, "");
  int a = 0;
  while (a < (int)v.length() && n < max) {
    int b = v.indexOf((char)0x1F, a); if (b < 0) b = v.length();
    String one = v.substring(a, b); one.trim();
    if (one.length()) { snprintf(dst[n], 20, "%s", one.c_str()); n++; }
    a = b + 1;
  }
}
static void filtersLoad() {
  listLoad("seena", seenApps, APPS_N, seenN);
  listLoad("mutea", mutedApps, MUTE_N, mutedN);
  listLoad("vipw", vipWords, VIP_N, vipN);
}
static void listSave(const char* key, char (*src)[20], int n) {
  String v;
  for (int i = 0; i < n; i++) { if (i) v += (char)0x1F; v += src[i]; }
  prefs.putString(key, v);
}
static bool hasWord(const char* hay, const char* w) {
  size_t L = strlen(w); if (!L) return false;
  for (const char* p = hay; *p; p++) {
    size_t i = 0;
    while (i < L && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)w[i])) i++;
    if (i == L) return true;
  }
  return false;
}
static bool noteVip(const Note& n) {
  for (int i = 0; i < vipN; i++)
    if (hasWord(n.title, vipWords[i]) || hasWord(n.msg, vipWords[i]) || hasWord(n.app, vipWords[i])) return true;
  return false;
}
// Seen once, remembered, so the apps can offer it to switch off.
static void seenApp(const char* a) {
  for (int i = 0; i < seenN; i++) if (!strcasecmp(seenApps[i], a)) return;
  if (seenN == APPS_N) { for (int i = 0; i < APPS_N - 1; i++) memcpy(seenApps[i], seenApps[i + 1], 20); seenN--; }
  snprintf(seenApps[seenN++], 20, "%s", a);
  listSave("seena", seenApps, seenN);
}
// False: dropped, never stored or shown. A VIP always comes through.
static bool noteAllowed(const Note& n) {
  const char* a = appShort(n);
  seenApp(a);
  if (noteVip(n)) return true;
  for (int i = 0; i < mutedN; i++) if (!strcasecmp(mutedApps[i], a)) return false;
  return true;
}
static String lstJson() {
  String o = "{\"apps\":[";
  for (int i = 0; i < seenN; i++) { if (i) o += ","; o += "\""; o += seenApps[i]; o += "\""; }
  o += "],\"muted\":[";
  for (int i = 0; i < mutedN; i++) { if (i) o += ","; o += "\""; o += mutedApps[i]; o += "\""; }
  o += "],\"vip\":[";
  for (int i = 0; i < vipN; i++) { if (i) o += ","; o += "\""; o += vipWords[i]; o += "\""; }
  o += "]}";
  if (o.length() > 510) o = o.substring(0, 500) + "]}";
  return o;
}

// ---- a call ringing on the popup: tap answers, hold declines ----
static bool popRinging() {
  if (!noteN) return false;
  const Note& n = notes[0];
  return n.cat == CAT_CALL && n.uid < 0x40000000UL && ancsState == ANCS_READY && millis() - n.at < 60000UL;
}

// ---- the Mac screen ----
static int macItems(int* idx) {        // what a hold can tick off: pinned, then the three
  int k = 0;
  if (cards[2].on && cards[2].line[0][0]) idx[k++] = 0;
  if (cards[1].on) for (int i = 0; i < 3; i++) if (cards[1].line[i][0]) idx[k++] = i + 1;
  return k;
}
static void drawMac() {
  oled.clearDisplay();
  char tb[8]; clockStr(tb, sizeof(tb), false);
  if (depth == 0) {
    titleBar("MAC", tb);
    int y = 14;
    if (cards[2].on && cards[2].line[0][0]) { char b[24]; snprintf(b, sizeof(b), "> %.19s", cards[2].line[0]); at(0, y, b); y += 12; }
    if (cards[1].on) for (int i = 0; i < 3 && y <= 38; i++) if (cards[1].line[i][0]) {
      char b[24]; snprintf(b, sizeof(b), "o %.19s", cards[1].line[i]); at(0, y, b); y += 12; }
    if (y == 14 && !cards[0].on) ctr("nothing from the Mac", 30, 1);
    if (cards[0].on && cards[0].line[0][0]) {
      oled.drawFastHLine(0, 51, SCRW, SSD1306_WHITE);
      ctr(cards[0].line[0], 54, 1);
    }
  } else {
    int idx[4]; int k = macItems(idx);
    titleBar("DONE?", tb);
    if (!k) { ctr("nothing to tick off", 30, 1); oled.display(); return; }
    if (macSel >= k) macSel = 0;
    for (int r = 0; r < k; r++) {
      int y = 14 + r * 12;
      const char* t = idx[r] == 0 ? cards[2].line[0] : cards[1].line[idx[r] - 1];
      if (r == macSel) { oled.fillRect(0, y - 2, SCRW, 12, SSD1306_WHITE); oled.setTextColor(SSD1306_BLACK); }
      char b[24]; snprintf(b, sizeof(b), "%s%.19s", idx[r] == 0 ? "> " : "o ", t);
      at(0, y, b); oled.setTextColor(SSD1306_WHITE);
    }
  }
  oled.display();
}
static bool cardsAny() { return cards[0].on || cards[1].on || cards[2].on; }

// ---- Mac left behind ----
static void drawWalk() {
  oled.clearDisplay();
  bool on = (millis() / 500) % 2;
  if (on) oled.drawRoundRect(0, 0, SCRW, SCRH, 6, SSD1306_WHITE);
  ctr("YOUR MAC", 12, 2);
  ctr("is left behind", 36, 1);
  ctr("touch to dismiss", 52, 1);
  oled.display();
}

// ---- update over Bluetooth ----
static void drawBleOta() {
  oled.clearDisplay();
  titleBarC("UPDATE");
  ctr("from your Mac", 16, 1);
  int pct = otaSize ? (int)((uint64_t)otaGot * 100 / otaSize) : 0;
  oled.drawRoundRect(8, 30, 112, 10, 3, SSD1306_WHITE);
  oled.fillRoundRect(10, 32, (108 * pct) / 100, 6, 2, SSD1306_WHITE);
  char b[8]; snprintf(b, sizeof(b), "%d%%", pct);
  ctr(b, 46, 1);
  oled.display();
}
static void otaStop(const char* why) {
  if (otaOn) Update.abort();
  otaOn = false;
  // Put the ordinary rhythm back; the fast one is for sending bytes.
  {
    NimBLEServer* sv = NimBLEDevice::getServer();
    if (sv && otaConn != 0xFFFF) sv->updateConnParams(otaConn, 72, 96, 4, 600);
  }
  char b[32]; snprintf(b, sizeof(b), "ota err %s", why); evtSend(b);
  Serial.printf("ota over bluetooth stopped: %s\n", why);
  flash("UPDATE STOPPED", 1400);
}

// An icon in the current colour, at 1x or 2x.
static void uiIcon(int x, int y, const uint8_t* ic, int scale, uint16_t col) {
  for (int j = 0; j < 8; j++)
    for (int i = 0; i < 8; i++)
      if (ic[j] & (0x80 >> i)) oled.fillRect(x + i * scale, y + j * scale, scale, scale, col);
}
// The scrollbar: a 2 px track, the thumb as long as the share in view.
static void uiScroll(int first, int total) {
  if (total <= UI_ROWS) return;
  const int top = UI_BAR_H + 2, h = SCRH - top - 1;
  int th = max(6, h * UI_ROWS / total);
  int ty = top + (h - th) * first / (total - UI_ROWS);
  oled.drawFastVLine(SCRW - 1, top, h, SSD1306_WHITE);
  oled.fillRect(SCRW - 2, ty, 2, th, SSD1306_WHITE);
}
// The first row in view, keeping the chosen one inside the window.
static int uiFirst(int sel, int total) {
  int first = sel > UI_ROWS - 1 ? sel - (UI_ROWS - 1) : 0;
  if (first > total - UI_ROWS) first = total - UI_ROWS;
  return first < 0 ? 0 : first;
}
// One row: icon (or none), label, value right-aligned, chosen or not.
static void uiRow(int r, const uint8_t* ic, const char* label, const char* value, bool on, bool scrolled) {
  int y = UI_ROW_Y + r * UI_ROW_H;
  uint16_t fg = on ? SSD1306_BLACK : SSD1306_WHITE;
  if (on) oled.fillRoundRect(1, y - 2, SCRW - 2 - (scrolled ? 3 : 0), UI_ROW_H, 2, SSD1306_WHITE);
  int tx = UI_PAD + 1;
  if (ic) { uiIcon(UI_PAD + 1, y, ic, 1, fg); tx = UI_TEXT_X + 1; }
  oled.setTextColor(fg);
  int room = (SCRW - tx - UI_PAD - (scrolled ? 5 : 0)) / 6;
  int vlen = value && *value ? (int)strlen(value) : 0;
  char b[22]; snprintf(b, sizeof(b), "%.*s", max(0, room - (vlen ? vlen + 1 : 0)), label);
  at(tx, y, b);
  if (vlen) at(SCRW - UI_PAD - (scrolled ? 5 : 0) - vlen * 6, y, value);
  oled.setTextColor(SSD1306_WHITE);
}

// ---- the hubs (state and kinds are with the design rules, at the top) ----
static bool isHub(int s) { return s == S_TODAY || s == S_FHUB || s == S_CALM; }
static int hubItems(int s, HubIt* o) {
  int n = 0;
  if (s == S_TODAY) {
    o[n++] = { HI_SCREEN, (uint8_t)S_MSG, IC_BELL, "Notifications" };
    o[n++] = { HI_SCREEN, (uint8_t)S_REMIND, IC_CHECK, "Reminders" };
    if (cardsAny()) o[n++] = { HI_SCREEN, (uint8_t)S_FOCUS, IC_MAC, "Mac" };
    if (screenOn_(S_WEATHER)) o[n++] = { HI_SCREEN, (uint8_t)S_WEATHER, IC_SUN, "Weather" };
    if (cfgBike) o[n++] = { HI_SCREEN, (uint8_t)S_BIKE, IC_CAR, "Vehicle" };
  } else if (s == S_FHUB) {
    o[n++] = { HI_SCREEN, (uint8_t)S_PRAYER, IC_MOON, "Prayer times" };
    o[n++] = { HI_FAITH, F_ZIKR, IC_BEADS, "Zikr" };
    o[n++] = { HI_ADHKAR, 0, IC_DAWN, "Adhkar" };
    o[n++] = { HI_FAITH, F_QURAN, IC_BOOK, "Quran" };
    o[n++] = { HI_FAITH, F_NAMES, IC_STAR, "99 Names" };
    o[n++] = { HI_PSET, 0, IC_SLIDE, "Prayer settings" };
  } else if (s == S_CALM) {
    o[n++] = { HI_RELAX, 0, IC_LEAF, "Relax" };
    o[n++] = { HI_SCREEN, (uint8_t)S_READS, IC_PAGE, "Short reads" };
    o[n++] = { HI_SCREEN, (uint8_t)S_GAMES, IC_PAD, "Games" };
  }
  return n;
}
static int hubCount(int s) { HubIt t[8]; return hubItems(s, t); }
static const uint8_t* hubIcon(int s) {
  return s == S_TODAY ? IC_BELL : s == S_FHUB ? IC_MOON : s == S_CALM ? IC_LEAF : IC_SLIDE;
}
static bool prayerSoon(int mins) {
  if (!prayerOk) return false;
  struct tm t; if (!nowLocal(&t)) return false;
  int now = t.tm_hour * 60 + t.tm_min;
  for (int i = 0; i < 5; i++) {
    int p = prayerAt(i); if (p < 0) continue;
    int d = p - now; if (d < 0) d += 1440;
    if (d <= mins) return true;
  }
  return false;
}
// A value for a menu row: the count or the reading that matters.
static void hubValue(const HubIt& it, char* v, size_t n) {
  v[0] = 0;
  if (it.kind != HI_SCREEN) return;
  if (it.a == S_MSG && noteUnread()) snprintf(v, n, "%d", noteUnread());
  else if (it.a == S_REMIND) { int k = 0; for (int i = 0; i < remCount; i++) if (!rems[i].done) k++; if (k) snprintf(v, n, "%d", k); }
  else if (it.a == S_WEATHER && wxOk) snprintf(v, n, "%dC", (int)lroundf(wTemp));
}

// The front of a hub (and of Settings): the icon twice the size, two
// lines of what is inside, and the hint. Same places on every hub.
static void uiHubCard(const char* name, const uint8_t* ic, const char* l1, const char* l2) {
  oled.clearDisplay();
  bar(name);
  const int iy = UI_BAR_H + (UI_HINT_Y - UI_BAR_H - 16) / 2;        // centred in the space above the hint
  uiIcon(UI_PAD + 5, iy, ic, 2, SSD1306_WHITE);
  const int tx = UI_PAD + 5 + 16 + 8;
  char b[18];
  bool two = l2 && *l2;
  // the text block (8 px a line, 2 px between) centred on the 16 px icon
  int ty = two ? iy - 1 : iy + 4;
  snprintf(b, sizeof(b), "%.15s", l1); at(tx, ty, b);
  if (two) { snprintf(b, sizeof(b), "%.15s", l2); at(tx, ty + 10, b); }
  ctr("hold to open", UI_HINT_Y, 1);
  oled.display();
}
static void drawHub() {
  char l1[24] = "", l2[24] = "";
  const char* nm = S_NAME[screen];
  if (depth == 0) {
    if (screen == S_TODAY) {
      int u = noteUnread();
      if (u) snprintf(l1, sizeof(l1), "%d new", u); else snprintf(l1, sizeof(l1), "All read");
      int k = 0; for (int i = 0; i < remCount; i++) if (!rems[i].done) k++;
      if (k) snprintf(l2, sizeof(l2), "%d reminder%s", k, k == 1 ? "" : "s");
      else if (wxOk) snprintf(l2, sizeof(l2), "%dC outside", (int)lroundf(wTemp));
    } else if (screen == S_FHUB) {
      struct tm t;
      if (prayerOk && nowLocal(&t)) {
        int now = t.tm_hour * 60 + t.tm_min, i = nextPrayer(now), p = prayerAt(i);
        int d = p - now; if (d < 0) d += 1440;
        snprintf(l1, sizeof(l1), "%s %02d:%02d", PRAYERS[i], p / 60, p % 60);
        if (d >= 60) snprintf(l2, sizeof(l2), "in %dh %02dm", d / 60, d % 60);
        else         snprintf(l2, sizeof(l2), "in %d min", d);
      } else { snprintf(l1, sizeof(l1), "Zikr, Quran"); snprintf(l2, sizeof(l2), "and adhkar"); }
    } else {
      snprintf(l1, sizeof(l1), "Relax, reads"); snprintf(l2, sizeof(l2), "and %d games", G_COUNT);
    }
    uiHubCard(nm, hubIcon(screen), l1, l2);
    return;
  }
  // the menu
  HubIt it[8]; int n = hubItems(screen, it);
  if (hubSel >= n) hubSel = 0;
  oled.clearDisplay();
  char cnt[24]; snprintf(cnt, sizeof(cnt), "%d/%d", hubSel + 1, n);
  titleBar(nm, cnt);
  int first = uiFirst(hubSel, n);
  bool sc = n > UI_ROWS;
  for (int r = 0; r < UI_ROWS && first + r < n; r++) {
    char v[16]; hubValue(it[first + r], v, sizeof(v));
    uiRow(r, it[first + r].icon, it[first + r].name, v, first + r == hubSel, sc);
  }
  uiScroll(first, n);
  oled.display();
}

// The four hub items that are a list, and nothing else, open the list.
//
// Choosing Notifications and then having to open Notifications is a
// step that buys nothing, because there is nothing else on that
// screen. Weather, Vehicle, Prayer times and Mac are a screen rather
// than a list, so they stay where they were. An empty list is still
// its summary: there is nothing to open, and "nothing yet" is better
// read on the screen that says it.
static bool hubOpensList(int s) {
  if (s == S_MSG)    return noteN > 0;
  if (s == S_REMIND) return remCount > 0;
  return s == S_READS || s == S_GAMES;
}

// Into a hub's item. hubEntry is the depth it begins at, so "back" from
// there returns to the hub's menu rather than wandering further out.
static void hubEnter(int hubScr, int sel) {
  HubIt it[8]; int n = hubItems(hubScr, it);
  if (!n) return;
  if (sel < 0 || sel >= n) sel = 0;
  hubSel = sel; inHub = hubScr;
  const HubIt& x = it[sel];
  itemIdx = 0; subIdx = 0;
  switch (x.kind) {
    case HI_SCREEN:
      screen = x.a;
      if (hubOpensList(x.a)) { depth = 1; hubEntry = 1; }
      else                   { depth = 0; hubEntry = 0; }
      break;
    case HI_FAITH:
      screen = S_FAITH; depth = 2; itemIdx = x.a; hubEntry = 2;
      if (x.a == F_ZIKR) zikrReset();
      break;
    case HI_ADHKAR: {
      struct tm t; int h = nowLocal(&t) ? t.tm_hour : 8;
      screen = S_FAITH; depth = 2; itemIdx = (h >= 3 && h < 15) ? F_MORNING : F_EVENING; hubEntry = 2;
      break;
    }
    case HI_RELAX:
      relaxOn = true; relaxKind = 0; relaxNext = millis() + 30000UL;
      relaxUntil = millis() + RELAX_RQ_MS;
      wake("relax");
      break;
    case HI_PSET:
      screen = S_SETTINGS; depth = 1; setGrp = SG_FAITHSET; itemIdx = SG_ROWS[SG_FAITHSET][0]; hubEntry = 1;
      break;
  }
}

// The night ends a quarter of an hour before Fajr (or at 05:00 with no
// prayer times), so the Fajr alert is never slept through.
// Awake: 90 to 120 ms, may skip 4 (about half a second). Dark: 120 to
// 150 ms, may skip 12 (under two seconds, Apple's limit). The radio then
// wakes roughly once every two seconds instead of twice a second.
static void idleRhythm() {
  static bool was = false;
  // Never during an update. The update asks for the quickest rhythm
  // the link will take, and this would put it straight back to the
  // ordinary one: the update wakes the robot, waking flips idle, and
  // flipping idle is exactly what this watches for. The fast request
  // was being undone a moment after it was made.
  if (otaOn) { was = false; return; }
  bool idle = asleep && !cfgFollow;
  if (idle == was) return;
  was = idle;
  NimBLEServer* sv = NimBLEDevice::getServer();
  if (!sv || !btUp) return;
  for (int i = 0; i < linkN; i++) {
    if (!links[i].authed) continue;
    if (idle) sv->updateConnParams(links[i].h, 96, 120, 12, 600);
    else      sv->updateConnParams(links[i].h, 72, 96, 4, 600);
  }
}
static int nightEnd() {
  if (prayerOk) { int f = prayerAt(0) - 15; return f < 0 ? f + 1440 : f; }
  return 5 * 60;
}
static bool nightNow(int nowMin) {
  int bed = (cfgBed + nightPush) % 1440, end = nightEnd();
  if (bed == end) return false;
  return bed < end ? (nowMin >= bed && nowMin < end) : (nowMin >= bed || nowMin < end);
}
static long secsToNightEnd() {
  struct tm t; if (!nowLocal(&t)) return -1;
  int now = t.tm_hour * 60 + t.tm_min;
  int d = nightEnd() - now; if (d <= 0) d += 1440;
  return (long)d * 60 - t.tm_sec;
}
static void drawNightCard() {
  oled.clearDisplay();
  titleBarC("NIGHT SLEEP");
  long left = ((long)nightCardUntil - (long)millis()) / 1000 + 1;
  char b[22]; snprintf(b, sizeof(b), "in %ld s", left < 0 ? 0 : left);
  ctr(b, 18, 1);
  ctr("tap: an hour later", 34, 1);
  ctr("hold: sleep now", 46, 1);
  oled.display();
}
// Asleep with the screen dark and nothing going on, at night: a card for
// ten seconds, then deep sleep until just before Fajr.
static void nightTick() {
  static uint32_t lastCheck = 0;
  if (!cfgNight || !timeOk) return;
  uint32_t now = millis();
  struct tm t;
  if (nightCardUntil) {
    if ((int32_t)(now - nightCardUntil) >= 0) { nightCardUntil = 0; nightGo(); }
    return;
  }
  if (now - lastCheck < 15000) return;
  lastCheck = now;
  if (!nowLocal(&t)) return;
  int m = t.tm_hour * 60 + t.tm_min;
  // The push is for tonight: it is forgotten in the hour after the night ends.
  if (nightPush && !nightNow(m) && (m - nightEnd() + 1440) % 1440 < 60) nightPush = 0;
  if (!nightNow(m) || !asleep) return;
  if (tmrOn || awayOn || relaxOn || otaOn || rescueAP || cfgNet != NET_BT || upState != U_OFF) return;
  nightCardUntil = now + 10000;
  wake("night");
}
static void nightGo() {
  nightDeep = true;
  int e = nightEnd();
  oled.clearDisplay();
  ctr("Good night", 20, 1);
  char b[22]; snprintf(b, sizeof(b), "awake at %02d:%02d", e / 60, e % 60);
  ctr(b, 34, 1);
  oled.display();
  delay(1500);
  deepAuto = false;
  goDeep();
}

static void blogReset() {
  memset(&blog, 0, sizeof(blog));
  blog.magic = BLOG_MAGIC;
  blog.start = timeOk ? (uint32_t)time(nullptr) : 0;
  blog.v0 = isnan(battV) ? 0 : battV;
  blog.pct0 = isnan(battV) ? 0 : battPct(battV);
}
static void blogSave() {
  if (cfgBlog && blog.magic == BLOG_MAGIC) prefs.putBytes("blog", &blog, sizeof(blog));
}
// At start: after a deep sleep the RTC copy is current and the sleep is
// added; after anything else the flash copy is, and it was a restart.
static void blogBoot(bool fromDeep) {
  if (fromDeep && blog.magic == BLOG_MAGIC) {
    uint32_t now = (uint32_t)time(nullptr);
    if (rtcDeepAt && now > rtcDeepAt && now - rtcDeepAt < 7UL * 86400UL) blog.sDeep += now - rtcDeepAt;
  } else {
    BLog b;
    if (prefs.getBytes("blog", &b, sizeof(b)) == sizeof(b) && b.magic == BLOG_MAGIC) blog = b;
    else blogReset();
    if (!fromDeep) blog.restarts++;
  }
  rtcDeepAt = 0;
}
// Every pass of the loop: the time since the last one goes to the state
// it was spent in. Light sleep keeps the clock running, so the sleeping
// itself lands here on the next pass.
static void blogTick() {
  static uint32_t last = 0, ms[5] = { 0 }, saveAt = 0, vAt = 0;
  uint32_t now = millis();
  if (!last) { last = now; saveAt = now; return; }
  uint32_t dt = now - last; last = now;
  if (!cfgBlog || blog.magic != BLOG_MAGIC) return;
  int k = (cfgNet == NET_WIFI || rescueAP) ? 4 : !asleep ? 0 : (pmMode == 1 ? 2 : 1);
  ms[k] += dt;
  uint32_t* sec[5] = { &blog.sOn, &blog.sDark, &blog.sLight, &blog.sDeep, &blog.sWifi };
  if (ms[k] >= 1000) { *sec[k] += ms[k] / 1000; ms[k] %= 1000; }
  // Full: the voltage you chose. Not again until it has fallen 0.10 V,
  // so a robot left on the charger does not wipe its log over and over.
  if (now - vAt > 5000 && !isnan(battV)) {
    vAt = now;
    float thr = BLOG_V[constrain(cfgBlogV, 0, 3)];
    if (blogArmed && battV >= thr - 0.005f) { blogReset(); blogArmed = false; blogSave(); Serial.println("battery log: full, a new cycle"); }
    else if (!blogArmed && battV < thr - 0.10f) blogArmed = true;
  }
  if (now - saveAt > 30UL * 60000UL) { saveAt = now; blogSave(); }
}
static float blogMah(int k) {
  uint32_t secs[5] = { blog.sOn, blog.sDark, blog.sLight, blog.sDeep, blog.sWifi };
  return secs[k] / 3600.0f * BLOG_MA[k];
}
static void fmtHM(uint32_t s, char* b, size_t n) { snprintf(b, n, "%lu:%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60)); }

// The view: tap moves down the list, hold starts a new cycle, back leaves.
static void drawBattUse() {
  oled.clearDisplay();
  titleBar("BATTERY USE", "mAh");
  const char* lab[9] = { "Screen", "Dark", "Light", "Deep", "WiFi", "Wakes", "Restarts", "Used", "Since" };
  const int NROWS = 9;                   // not N: RoboEyes owns N
  int sel = constrain(subIdx, 0, NROWS - 1);
  int first = uiFirst(sel, NROWS);
  for (int r = 0; r < UI_ROWS && first + r < NROWS; r++) {
    int i = first + r, y = UI_ROW_Y + r * UI_ROW_H;
    char t[16] = "", m[16] = "";
    if (i < 5) {
      uint32_t secs[5] = { blog.sOn, blog.sDark, blog.sLight, blog.sDeep, blog.sWifi };
      fmtHM(secs[i], t, sizeof(t));
      snprintf(m, sizeof(m), "%d", (int)lroundf(blogMah(i)));
    } else if (i == 5) snprintf(t, sizeof(t), "%lu", (unsigned long)blog.wakes);
    else if (i == 6) snprintf(t, sizeof(t), "%lu", (unsigned long)blog.restarts);
    else if (i == 7) {
      int now = isnan(battV) ? blog.pct0 : battPct(battV);
      int d = constrain((int)blog.pct0 - now, 0, 100);   // a percentage, so at most three digits
      snprintf(t, sizeof(t), "-%d%%", d);
      snprintf(m, sizeof(m), "%d", d * cfgCap / 100);
    } else {
      if (blog.start) { time_t st = blog.start; struct tm lt; localtime_r(&st, &lt); snprintf(t, sizeof(t), "%02d:%02d", lt.tm_hour, lt.tm_min); }
      else snprintf(t, sizeof(t), "--:--");
      if (blog.v0 > 0) snprintf(m, sizeof(m), "%.2f", blog.v0);
    }
    bool on = (i == sel);
    if (on) oled.fillRoundRect(1, y - 2, SCRW - 2 - 3, UI_ROW_H, 2, SSD1306_WHITE);
    oled.setTextColor(on ? SSD1306_BLACK : SSD1306_WHITE);
    at(UI_PAD + 1, y, lab[i]);
    at(84 - (int)strlen(t) * 6, y, t);                                   // the time column ends at 84
    if (m[0]) at(SCRW - UI_PAD - 5 - (int)strlen(m) * 6, y, m);           // mAh, clear of the scrollbar
    oled.setTextColor(SSD1306_WHITE);
  }
  uiScroll(first, NROWS);
  oled.display();
}

// Locked: dark, quiet for the set minutes since a real touch, and
// nothing running that wants the screen.
static bool pocketLocked() {
  if (!cfgPLock || !asleep) return false;
  if (tmrOn || relaxOn || cfgFollow || awayOn || otaOn) return false;
  return millis() - lastUserAt > (uint32_t)PLOCK_MIN[constrain(cfgPLock, 0, 3)] * 60000UL;
}
static uint32_t unlockMs() { uint32_t w = WAKE_OPTS[cfgWakeIdx]; return w ? w : 1000; }
// While a finger is down on a locked robot: dark for the first second,
// then a thin bar that fills; full, and it wakes. Let go early and it is
// dark again at once.
static void pocketTick() {
  if (!unlocking) return;
  uint32_t now = millis(), held = now - unlockAt;
  if (!touchOn) {                                   // let go before the end
    unlocking = false;
    if (unlockShown) { unlockShown = false; oled.clearDisplay(); oled.display(); screenPower(false); }
    return;
  }
  if (held >= unlockMs()) {
    unlocking = false; unlockShown = false;
    lastUserAt = now;
    wake("unlock");
    return;
  }
  if (held >= 1000) {
    if (!unlockShown) { unlockShown = true; screenPower(true); }
    uint32_t span = unlockMs() > 1000 ? unlockMs() - 1000 : 1;
    int w = (int)((uint64_t)(held - 1000) * 60 / span);
    oled.clearDisplay();
    oled.drawRoundRect(SCRW / 2 - 32, SCRH - 6, 64, 4, 2, SSD1306_WHITE);
    if (w > 0) oled.fillRoundRect(SCRW / 2 - 31, SCRH - 5, min(w, 62), 2, 1, SSD1306_WHITE);
    oled.display();
  }
}

static void syncBegin() {
  if (!wsStart(WS_SYNC)) return;
  syncRun = true; syncStarted = false; syncStoryDone = false; syncUpArmed = false;
  syncAt = millis();
  flash("SYNCING", 1500);
}


// Everything a session needs, once a pass.
static void serviceWs() {
  uint32_t now = millis();

  // Commands from the phone that move the radios, a moment after they
  // arrived so the reply to the phone has gone out first.
  if (rqPend && now - rqPendAt > 700) {
    int c = rqPend; rqPend = RQ_NONE;
    switch (c) {
      case RQ_SYNC:
        // A Mac on Bluetooth can fetch the weather, the prayer times and
        // a story and send them across, so asking it costs nothing and
        // the WiFi radio stays off. That was the whole point of moving
        // this work to the Mac; bringing WiFi up to do it anyway undid
        // it. WiFi is still the fallback when there is no Mac.
        if (macOnBle()) {
          evtSend("want sky");
          evtSend("want read");
          flash("ASKING THE MAC", 1400);
        } else syncBegin();
        break;
      case RQ_UPDATE:  startHotspot(); break;   // 7.4.1: "update" opens the hotspot
      case RQ_WIFI:    if (wsStart(WS_MANUAL)) flash("WIFI UNTIL RESTART", 1500); break;
      case RQ_HOTSPOT: startHotspot(); break;
      case RQ_TAMPER:  tamperArm(); break;
      case RQ_DEEP:    wantDeep = true; break;
      case RQ_REBOOT:  ESP.restart(); break;
    }
    // 6.6: the command above may have just started a session and
    // stamped it with a time later than the one read at the top. Read
    // it again, or "now - started" goes below zero, wraps round to
    // forty-nine days, and the session is ended as idle in the same
    // pass that began it. That is why config from a Shortcut showed
    // the hotspot card with no hotspot behind it, and why a sync or
    // an update sent from a Shortcut stopped at once.
    now = millis();
  }

  if (wsEndWant) { wsEndWant = false; wsEnd("BLUETOOTH"); return; }
  if (cfgNet != NET_WIFI || wsKind == WS_NONE) return;

  // Nothing to join. Say so and go home.
  if (netDown && !rescueAP) {
    bool wasUpd = upAfterJoin;
    bool wasSync = syncRun;
    wsEnd("");
    if (wasSync || wsKind == WS_NONE) wsFailCard("No saved WiFi nearby", "Back on Bluetooth");
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
      // 7.4.1: a sync no longer looks for releases; updates are from a file
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
      !wantOtaLatest && !netBusy && !syncUpArmed && upState == U_OFF && !storyBusy) {
    if (!syncStoryDone) {
      syncStoryDone = true;
      // A new short read, if there is a key to write one with. This
      // holds the loop for the length of the call, which is why it is
      // the last thing and says so on the screen.
      if (macLinked && readCount < READS_MAX) {
        oled.clearDisplay(); bar("SYNC"); ctr("Asking for a read", 30, 1); oled.display();
        evtSend("want read");
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
    if (wsKind == WS_SYNC) {
      bool on = online();
      wsEnd(on ? "SYNC PARTLY DONE" : "");
      if (!on) wsFailCard("No saved WiFi nearby", "Back on Bluetooth");
      return;
    }
  }

  if (wsKind == WS_UPDATE) {
    if (upState != U_OFF) wsSawUp = true;
    else if (wsSawUp) { wsEnd(""); return; }
    else if (now - wsStartMs > 20000) { wsEnd(""); return; }
  }
  if (wsKind == WS_MANUAL && rtcWsUntil && !syncRun && !netBusy && upState == U_OFF &&
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
// Shortcuts posts from more than one process: the app itself
// (com.apple.shortcuts), the old Workflow id, and the background runner
// that automations and widgets use (com.apple.WorkflowKit...). 6.0
// only knew the first, so commands from an automation arrived as an
// ordinary notification titled RAFIQ and did nothing.
static bool rqFromShortcuts(const char* app) {
  char l[40]; int n = 0;
  for (const char* p = app; *p && n < 39; p++) l[n++] = tolower((unsigned char)*p);
  l[n] = 0;
  return strstr(l, "shortcut") || strstr(l, "workflow");
}
static bool rafiqTagged(const Note& n) {
  return rqTagged(n.title) || rqTagged(stSub) || rqTagged(stMsg);
}
static bool rafiqIs(const Note& n) {
  return rqFromShortcuts(n.app) && rafiqTagged(n);
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
  snprintf(b, sizeof(b), "BT %.8s %d%% %s\nsync %s %.10s",
           btShort(), isnan(battV) ? 0 : battPct(battV), pmAvail ? "LS" : "",
           sy, lastReset);
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

  if (tmrCommand(c)) { }
  else if (is("sync") || is("sync now")) rqPend = RQ_SYNC;
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
  else if (is("night later") || is("night +1")) {                   // 7.7
    nightPush = constrain(nightPush + 60, 0, 360); nightCardUntil = 0;
    flash("AN HOUR LATER", 1200);
  }
  else if (is("relax") || is("relax on")) {
    relaxOn = true; relaxKind = 0; relaxNext = now + 30000UL;
    relaxUntil = now + RELAX_RQ_MS;    // three minutes, then sleep
  }
  else if (is("zikr") || is("dhikr") || is("tasbih")) {
    screen = S_FAITH; depth = 2; itemIdx = F_ZIKR; subIdx = 0;
    zikrReset();
  }
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
  else if (is("wake touch") || is("wake by touch")) { cfgWakeBy = 1; prefs.putInt("wakeby", 1); flash("WAKE BY TOUCH", 1200); }
  else if (is("wake move") || is("wake shake") || is("wake by move") || is("wake by shake") || is("wake motion")) {
    cfgWakeBy = 2; prefs.putInt("wakeby", 2); flash("WAKE BY MOVE", 1200);
  }
  else if (is("wake both") || is("wake by both") || is("wake all")) { cfgWakeBy = 0; prefs.putInt("wakeby", 0); flash("WAKE BY BOTH", 1200); }
  else if (is("wake by")) {
    cfgWakeBy = (cfgWakeBy + 1) % 3; prefs.putInt("wakeby", cfgWakeBy);
    flash(cfgWakeBy == 1 ? "WAKE BY TOUCH" : cfgWakeBy == 2 ? "WAKE BY MOVE" : "WAKE BY BOTH", 1200);
  }
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

  // Away: the rest of the text is the message. Away on its own turns it
  // on with the last message. While away, only home and away are heard.
  if (!strcmp(first, "away")) {
    if (!fresh) return;
    awayAuto = false; prefs.putBool("awaya", false);   // sent by hand: only home ends it
    const char* t = p + n;
    bool colon = false;
    while (*t == ':' || *t == ' ' || *t == '-') { if (*t == ':') colon = true; t++; }
    // "Away message: ..." and "Away msg ..." mean the same: message and
    // msg are a label, not what you want shown. Only as a whole word,
    // and only when it reads as a label: followed by a colon or a dash,
    // or straight after "away" with no colon. "Away: Messages go to
    // Sara" keeps every word.
    int L = !strncasecmp(t, "message", 7) ? 7 : !strncasecmp(t, "msg", 3) ? 3 : 0;
    if (L && (t[L] == ':' || t[L] == '-' || t[L] == ' ' || !t[L])) {
      const char* q = t + L;
      while (*q == ' ') q++;
      if (*q == ':' || *q == '-' || !colon) t = q;
    }
    while (*t == ':' || *t == ' ' || *t == '-') t++;
    String m = String(t); m.replace("\r", ""); m.trim();
    awaySet(true, m.c_str());
    return;
  }
  if (tmrOn && !awayOn) {
    if (!fresh) return;
    char ln[48]; int k = 0;
    for (const char* q = p; *q && *q != '\n' && k < 47; q++) {
      char ch = tolower((unsigned char)*q);
      if (ch == '.' || ch == '!' || ch == '\r') continue;
      ln[k++] = ch;
    }
    ln[k] = 0;
    while (k && ln[k - 1] == ' ') ln[--k] = 0;
    if (!strcmp(ln, "home")) tmrStop("TIMER STOPPED");
    else tmrCommand(ln);
    return;
  }
  if (awayOn) {
    char fc[16]; strcpy(fc, first);                // "Home." and "home!" count too
    size_t L = strlen(fc);
    while (L && (fc[L - 1] == '.' || fc[L - 1] == '!')) fc[--L] = 0;
    if (fresh && !strcmp(fc, "home")) awaySet(false, nullptr);
    return;
  }

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
    popGlanceMs = 3000;                  // something you sent: three seconds, not one
    if (popupSecs()) popupShow();
    else { wake("message"); screen = S_MSG; depth = 2; noteIdx = 0; }
    popGlanceMs = 1000;
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
  if (awayOn) return;
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
static const char* tlLabel(int i, char* b, size_t n) {
  if (i >= tlN) snprintf(b, n, "Clear all");
  else          snprintf(b, n, "%s", tlLines[i]);
  return b;
}
static void drawTlog() {
  char r[8]; snprintf(r, sizeof(r), "%d", tlN);
  drawList("TAMPER LOG", r, tlN + 1, tlSel, tlLabel);
}
// Remove the k-th newest line, as the list shows them.
static void tlogDelete(int k) {
  if (!fsOk) return;
  File f = LittleFS.open(TLOG_PATH, "r");
  if (!f) return;
  String all = f.readString(); f.close();
  int total = 0;
  for (unsigned i = 0; i < all.length(); i++) if (all[i] == '\n') total++;
  int target = total - 1 - k, line = 0;
  String out;
  int start = 0;
  for (unsigned i = 0; i < all.length(); i++) {
    if (all[i] != '\n') continue;
    if (line != target) out += all.substring(start, i + 1);
    line++; start = i + 1;
  }
  File w = LittleFS.open(TLOG_PATH, "w");
  if (w) { w.print(out); w.close(); }
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

void setup() {
  // Waking from being switched off is not a boot, whatever the processor
  // thinks. Skipping the animations is the difference between picking it
  // up and having it there, and picking it up and watching it introduce
  // itself again.
  esp_sleep_wakeup_cause_t woke_ = esp_sleep_get_wakeup_cause();
  bool fromDeep = (woke_ == ESP_SLEEP_WAKEUP_GPIO || woke_ == ESP_SLEEP_WAKEUP_TIMER);
  Serial.begin(115200);
  // Armed for tamper: a wake is either something moving it or the end
  // of the pause after one. Either way it is written down and it goes
  // straight back. Nothing else runs, and this never returns.
  if (rtcTamper && (woke_ == ESP_SLEEP_WAKEUP_GPIO || woke_ == ESP_SLEEP_WAKEUP_TIMER))
    tamperWake(woke_ == ESP_SLEEP_WAKEUP_GPIO);
  // Asleep in Away: show the message, write it down, back off. Never
  // returns, and never reaches the radio.
  if (rtcAwayDeep && woke_ == ESP_SLEEP_WAKEUP_TIMER && rtcAwayCheck) awayCheckBoot = true;
  else if (rtcAwayDeep && (woke_ == ESP_SLEEP_WAKEUP_GPIO || woke_ == ESP_SLEEP_WAKEUP_TIMER))
    awayDeepWake(woke_ == ESP_SLEEP_WAKEUP_TIMER);
  rtcAwayCheck = 0;
  rtcAwayDeep = 0;
  // Before the serial port settles, before anything is read and long
  // before anything is powered. A touch that was not meant costs this
  // much and nothing else.
  if (woke_ == ESP_SLEEP_WAKEUP_GPIO) wakeGate();
  delay(300);

  prefs.begin("nexus", false);
  pmInit();                            // says whether light sleep is on offer
  bool awayBoot = prefs.getBool("away", false);   // Away: no start-up screens at all
  cBoot = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", cBoot);

  // Three panics in a row and the robot stops trying to be clever.
  // A deep sleep wake reports ESP_RST_DEEPSLEEP and a power up
  // reports ESP_RST_POWERON, so neither of those is counted, and
  // neither writes to NVS unless there was something to clear.
  {
    esp_reset_reason_t rr = esp_reset_reason();
    bool crashed = (rr == ESP_RST_PANIC   || rr == ESP_RST_INT_WDT ||
                    rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT);
    // 6.0.1: a restart nobody asked for is said on the screen at boot,
    // and kept for RAFIQ status, so "it rebooted" can be told apart
    // from "it went to sleep and woke up".
    lastReset = rr == ESP_RST_POWERON ? "power on" : rr == ESP_RST_DEEPSLEEP ? "sleep wake"
              : rr == ESP_RST_SW ? "restart" : rr == ESP_RST_PANIC ? "crash"
              : (rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT) ? "watchdog"
              : rr == ESP_RST_BROWNOUT ? "low power" : rr == ESP_RST_EXT ? "reset pin" : "other";
    if (crashed)                  bootNote = rr == ESP_RST_PANIC ? "RESTARTED: CRASH" : "RESTARTED: WATCHDOG";
    else if (rr == ESP_RST_BROWNOUT) bootNote = "RESTARTED: LOW POWER";
    int crashes = prefs.getInt("panics", 0);
    if (crashed) { crashes++; prefs.putInt("panics", crashes); }
    else if (crashes)         prefs.putInt("panics", 0);
    if (crashed && crashes >= SAFE_AFTER) {
      safeMode = true;
      prefs.putInt("panics", 0);
      Serial.printf("safe mode: %d panics running\n", crashes);
    }
  }
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
  cfgAutoUp   = false;                 // 7.4.1: never fetches releases by itself
  cfgKnock    = prefs.getBool("knock", false);   // the pad drives this now
  cfgHijriAdj = constrain(prefs.getInt("hadj", 0), -2, 2);
  // Carried over from the old on/off. Off meant off; anything else
  // was WiFi, because Bluetooth did not exist yet.
  if (prefs.isKey("net")) cfgNet = constrain(prefs.getInt("net", NET_WIFI), 0, NET_N - 1);
  else {
    cfgNet = prefs.getBool("offl", false) ? NET_OFF : NET_WIFI;
    prefs.putInt("net", cfgNet);
  }
  // 6.0: Bluetooth is home and WiFi is never kept. A robot coming up
  // from 5.x on WiFi moves to Bluetooth once, here, and anything that
  // ever writes WiFi as home again is corrected the same way.
  if (!prefs.getBool("netv6", false)) prefs.putBool("netv6", true);
  if (cfgNet == NET_WIFI) { cfgNet = NET_BT; prefs.putInt("net", cfgNet); }
  cfgNetHome = cfgNet;
  cfgPGuard  = prefs.getBool("guard", false);
  cfgHoldIdx = constrain(prefs.getInt("holdi", 1), 0, HOLD_N - 1);
  cfg12h     = prefs.getBool("h12", false);
  cfgWakeBy  = constrain(prefs.getInt("wakeby", 0), 0, 2);
  cfgMulti    = prefs.getBool("multi", false);
  cfgAutoAway = prefs.getBool("autoaway", true);
  cfgNight    = prefs.getBool("night", false);
  cfgBed      = constrain(prefs.getInt("bed", 23 * 60), 0, 1439);
  awayAuto    = prefs.getBool("awaya", false);
  devLoad();
  filtersLoad();                       // 7.5
  cfgBlog  = prefs.getBool("blog", true);
  cfgPLock = constrain(prefs.getInt("plock", 2), 0, 3);
  lastUserAt = millis();
  cfgBlogV = constrain(prefs.getInt("blogv", 1), 0, 3);
  cfgCap   = constrain(prefs.getInt("cap", 350), 50, 5000);
  blogBoot(fromDeep);                  // 7.8
  cfgQuiet   = prefs.getBool("quiet", false);
  lastSyncAt = prefs.getUInt("lsync", 0);
  cfgWakeIdx  = constrain(prefs.getInt("wakeh", 2), 0, WAKE_N - 1);
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
    cfgBack = prefs.getBool("shake", true) ? BACK_BOTH : BACK_KNOCK;
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
  loadWx();
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
  // Any real start ends a tamper watch. Deep sleep wakes never get
  // here while it is armed, so arriving here at all is the disarm.
  bool tamperWas = prefs.getBool("tamper", false);
  rtcTamper = 0;
  if (tamperWas) { prefs.putBool("tamper", false); tlogAdd("Disarmed"); }
  loadNotes();
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
  if (!fromDeep && !awayBoot) animWake(1200);   // from sleep or in Away: no ceremony
  startSensors();
  applyFallInt();
  applyTap();
  // Ask the hardware whether the wire is there rather than assuming it.
  intWired = probeIntPin();
  applyFallInt();                     // the probe borrowed the interrupt setup
  applyTap();
  deepOff = prefs.getBool("nodeep", false);
  Serial.printf("INT1 %s\n", intWired ? "wired, it can switch off" : "not wired, screen off only");
  if (!fromDeep && !awayBoot) animSenses(700);

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
    if (nowLocal(&t0) && t0.tm_year > 123) {   // past 2023, so it is real
      timeOk = true;
      clockSrc = "kept through sleep";
      Serial.println("clock survived: reminders and prayer times still stand");
    }
  }

  loadNets();
  // Manual WiFi carries on through deep sleep and ends on anything
  // else. The deadline lives in RTC memory, which a real restart
  // clears, so this is the whole of that rule.
  {
    bool keep = (esp_reset_reason() == ESP_RST_DEEPSLEEP) && rtcWsKind == WS_MANUAL &&
                rtcWsUntil && (uint32_t)time(nullptr) < rtcWsUntil;
    if (keep) { cfgNet = NET_WIFI; wsKind = WS_MANUAL; wsStartMs = wsLastUse = millis();
                Serial.println("manual wifi carries on through the sleep"); }
    else      { rtcWsUntil = 0; rtcWsKind = 0; }
  }
  bool needHotspot = false;
  // Safe mode is WiFi and nothing else, for this start only. With no
  // network saved it is the hotspot, so there is always a way in.
  if (safeMode) {
    prefs.putInt("btry2", 0);
    if (netCount) { cfgNet = NET_WIFI; wsKind = WS_MANUAL; wsStartMs = wsLastUse = millis(); }
    else          { cfgNet = NET_OFF; needHotspot = true; }
    flash("SAFE MODE", 1500);
    flash("UPDATE ME", 1800);
  }
  // Did the last attempt at Bluetooth come back? If the note is still
  // there, it did not, and the robot is not going to try it again on
  // its own. WiFi, and a word on the screen about why.
  // 7.4: only a crash is a failed start. Power off and on, the reset
  // button, a restart or a deep-sleep wake are not, and used to add up
  // until WiFi came on by itself. And a real failure no longer turns
  // WiFi on: WiFi is only ever on because it was asked for.
  {
    esp_reset_reason_t r = esp_reset_reason();
    bool crash = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT);
    if (!crash) prefs.putInt("btry2", 0);
  }
  if (cfgNet == NET_BT && prefs.getInt("btry2", 0) >= BT_GIVE_UP) {
    prefs.putInt("btry2", 0);
    btFellBack = true;
    Serial.printf("bluetooth crashed %d starts running: trying again, no WiFi\n", BT_GIVE_UP);
    flash("BLUETOOTH RESTARTED", 1400);
  }
  if (cfgOffline) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_OFF);
    netUsing = -1; netTrying = 0;
    if (cfgNet == NET_BT) {
      bleOn();
      Serial.println("bluetooth mode: the aerial belongs to the phone");
    } else {
      btStop();                      // nothing uses it; make sure nothing can
      Serial.println("offline by choice: both radios stay down");
    }
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
  } else if (!fromDeep && !cfgOffline && !awayBoot) {
    offlineWelcome();
  }
  if (needHotspot) startHotspot();
  if (tamperWas) flash("TAMPER OFF, SEE LOG", 1800);
  if (bootNote) flash(bootNote, 2500);
  Serial.printf("last reset: %s, light sleep: %s\n", lastReset, pmAvail ? "yes" : "no");

  eyes.setAutoblinker(ON, 7, 5);      // a blink now and then, not a flutter
  eyes.setIdleMode(ON, 5, 4);
  eyes.setMood(STYLES[cfgEyes].mood);

  // Two columns, numbers on a common left edge so the words line up.
  // Not shown when it was only switched off: you know how to knock by
  // the second time you pick it up.
  oled.clearDisplay();
  if (!fromDeep && !awayBoot) {
    // What the pad does, because the pad is how it is driven. It used
    // to list what one to four knocks meant, which is a feature that
    // is off unless you ask for it.
    // The controls as they are since 6.1: a tap moves on, a hold opens,
    // a longer hold goes back, four seconds switches off.
    titleBarC("HOW TO USE ME");
    ctr("tap: next  hold: open", 16, 1);
    ctr("long: back  4s: off", 28, 1);
    oled.drawFastHLine(8, 40, 112, SSD1306_WHITE);
    ctr("Touch the pad", 48, 1);
  }
  if (!fromDeep && !awayBoot) {
    oled.display();
    holdCard(2000);                    // a knock ends it, and it never dawdles
  }

  screen = S_HOME; depth = 0; itemIdx = 0; subIdx = 0;
  lastActive = millis();
  // The finger that woke it is very likely still on the pad, because
  // holding it there is what woke it. Start with the press already
  // down and already spent, or the three seconds you held to wake it
  // would be read as a long press as well and open whatever a long
  // press opens.
  if (digitalRead(TOUCH_PIN) != touchRest) {
    touchOn = true; touchLvl = digitalRead(TOUCH_PIN);
    touchEdge = 0; touchPressAt = millis();
    touchLongDone = true; touchTaps = 0;
  }
  if (fromDeep) {
    wokeForAlarm = (woke_ == ESP_SLEEP_WAKEUP_TIMER);
    wokeBy = wokeForAlarm ? "prayer" : "picked up";
    if ((wokeForAlarm && rtcWakeCheck) || awayCheckBoot) {   // only looking for the phone
      wokeForAlarm = false; wokeBy = "check";
      checkWake = true;
    }
    Serial.printf("back from being off (%s)\n", wokeBy.c_str());
  }
  if (!fromDeep) rtcAwaySince = 0;
  rtcWakeCheck = 0;
  awayOn   = prefs.getBool("away", false);
  awayText = prefs.getString("awayt", "");
  if (awayOn && !checkWake) {
    // Switched off and on: eyes, the message for three seconds, then
    // two minutes of Bluetooth in the background for a RAFIQ home.
    awayListenUntil = millis() + AWAY_LISTEN_RESTART_MS;
    awayShowMs = AWAY_RESTART_SHOW_MS;
    awayEyes(true);                    // the eyes open, then the message
    lastActive = millis(); lastDraw = 0;
    tlogAdd("Away: restarted");
    Serial.println("away: restarted, listening for two minutes");
  }
  if (checkWake) {
    // Dark, listening, for twenty seconds. The loop decides: the phone
    // came back and it stays linked, or it is off again.
    asleep = true; sleptAt = millis();
    screenPower(false);
    checkUntil = millis() + CHECK_WINDOW_MS;
  } else {
    drawHome();                        // on screen before anything can block
  }

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

  // Keep a note of the time while it is trustworthy, for the screen
  // that comes up before the clock has landed.
  {
    static uint32_t lgAt = 0;
    if (timeOk && (!lgAt || now - lgAt > 60000UL)) {
      lgAt = now;
      time_t e = time(nullptr);
      if (e > 1735689600L) lastGoodEpoch = e;      // after 2025, so it is real
    }
  }

  // Up this long with the radio running is survival. Tear the note up
  // so a later unplug is not read as a crash.
  if (btNoteOut && now - btNoteAt > BT_PROVEN_MS) {
    prefs.putInt("btry2", 0); btNoteOut = false;
    Serial.println("bluetooth: held, the count is clear");
  }
  btTick();                            // the phone's clock and notifications
  appTick();                           // 7.2: what the Rafiq app sent
  if (screen == S_HOME) inHub = -1;    // 7.6: Home forgets which hub you were in
  linkTick();                          // 7.3: who stays connected, and who is the phone
  nightTick();                         // 7.7: bedtime
  blogTick();                          // 7.8: where the battery goes
  pocketTick();                        // 7.9: holding to unlock
  idleRhythm();                        // 7.7: a slower link while the screen is dark
  gestTilt();                          // 7.5: lean and the tilt knob, for the Mac
  if (remBatch && millis() - remBatchAt > 800) {
    sortRems(); saveRems();
    if (remIdx > remCount) remIdx = remCount;
    remAddedCard(remBatch, remBatchSoonest);
    remBatch = 0; remBatchSoonest = 0;
  }
  // 7.3: the phone gone two minutes, nothing else connected: Away, by
  // itself. It ends by itself too, when anything Rafiq knows comes back.
  if (cfgAutoAway && !awayOn && cfgNet == NET_BT && btUp && btEverLinked && !tmrOn &&
      linkN == 0 && btDropAt && (int32_t)(millis() - btDropAt) > (int32_t)AUTO_AWAY_MS) {
    tlogAdd("Auto away: phone gone");
    awaySet(true, nullptr);
    awayAuto = true; prefs.putBool("awaya", true);
    awayListenUntil = millis() + 4000;   // the message once, then off
    awayShowMs = 3000;
  }
  if (awayOn && awayAuto && anyLinked()) {
    tlogAdd("Auto away: phone back");
    awaySet(false, nullptr);
  }
  serviceWs();                         // WiFi sessions, syncs, commands waiting
  if (btNews) {
    uint8_t k = btNews; btNews = 0;
    if (!asleep && cfgNet == NET_BT) { banText = k == 1 ? "iPhone connected" : "iPhone disconnected"; banUntil = millis() + 1100; }
  }
  pgTick();                            // the phone guard
  serviceTamper();
  // 6.4: these run asleep as well as awake.
  tmrTick();
  awayFlush(false);
  if (awayOn && awayListenUntil && (int32_t)(now - awayListenUntil) > 0) awayDeepGo();
  if (phoneHeld()) rtcAwaySince = 0;   // the phone is here: the checking schedule starts over
  if (checkWake) {
    if (phoneHeld() || anyLinked()) { checkWake = false; Serial.println("check: phone is back, staying linked"); }
    else if (now > checkUntil) {
      checkWake = false;
      if (awayOn) { awayQuietDeep = true; awayDeepGo(); }   // still away: back to Away's sleep
      else        { deepAuto = true; goDeep(); }
    }
  }
  if (wxDirty) { wxDirty = false; saveWx(); }

  // Light sleep, between passes of this loop, whenever the screen is
  // dark on Bluetooth. The phone stays linked through it. Not with a
  // cable in, because the USB port does not survive it.
  bool lsNow = asleep && pmAvail && cfgNet == NET_BT && !usb_serial_jtag_is_connected();
  pmSet(lsNow);
  if (now - lastPoll >= (lsNow ? 100UL : 45UL)) { lastPoll = now; input(); }
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
    cfgGesture = false;                        // nothing to send presses to
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
    // The clock comes back from deep sleep still running but no
    // longer right. The RTC drifts, and a board that wakes and sleeps
    // a dozen times a day accumulates that drift until a reminder set
    // for nine goes off at some other time. Nothing used to ask
    // again: timeOk was already true by then, and only a missing
    // clock counted as a reason to look.
    //
    // So it asks on every wake that finds a network, and again every
    // few hours if it stays up. One short request, and what it buys
    // is that the number every alarm is worked out from is the right
    // one when the robot next lies down.
    if ((long)(now - nextResync) >= 0 ||
        (!timeOk && (long)(now - nextTimeTry) >= 0)) {
      nextTimeTry = now + 20000;
      nextResync  = now + TIME_RESYNC_MS;
      wantTime = true;
    }
    if ((long)(now - nextWx) >= 0) { nextWx = now + 900000UL; wantWx = true; }

    struct tm t;
    bool haveDay = timeOk && nowLocal(&t);
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
    // Nothing here any more. How often a read arrives is the Mac's
    // business now, and it knows when it last sent one; the robot
    // asking on a timer of its own would only ask twice.
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
      // Ten seconds of asking, then it takes itself away for fifteen
      // minutes. A minute of a flashing card nobody is there for is a
      // minute of the screen on and the battery going.
      toastUntil = millis() + REM_SHOW_MS;
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
  // Switching off in gesture mode would take the mode with it and
  // leave the Mac tapping at nothing, so it does not happen. Dark,
  // yes. Off, no.
  if (wantDeep && cfgGesture) wantDeep = false;
  if (wantDeep) {
    // Asked for, by holding the pad or by the Mac letting go. No
    // conditions: if you held it for four seconds you meant it.
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
  //
  // 6.0: on Bluetooth with a core that can light sleep, a linked phone
  // is somebody listening as well, so it stays dark and linked rather
  // than switching off. A minute after the phone goes, it switches off
  // as before. A sync, the hotspot or an update in progress hold it up.
  bool btWait = pmAvail && cfgNet == NET_BT && btUp;
  uint32_t wait = btWait ? BT_DEEP_GRACE_MS : (offlineNow() ? 0 : deepAfterMs());
  uint32_t since = sleptAt;
  if (btWait && btDropAt && (int32_t)(btDropAt - sleptAt) > 0) since = btDropAt;
  if (!deepOff && !cfgGesture && (offlineNow() || deepAfterMs()) && asleep && !sessionRunning() &&
      !macLinked && !phoneHeld() && !pgUntil && !syncRun && !rqPend && !tamperCountAt &&
      (wsKind == WS_NONE || wsKind == WS_MANUAL) &&
      (now - since) > wait && upState == U_OFF && !storyBusy && !awayOn && !checkWake && !tmrOn) {
    deepAuto = true;
    goDeep();
  }

  if (asleep) { delay(lsNow ? 50 : 6); return; }

  // 7.1: the sensor and knock tests are about moving it, so while one
  // is open the screen holds itself, as a game does.
  if (screen == S_SETTINGS && depth == 2 && (itemIdx == C_ACCEL || tapTesting)) lastActive = now;

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

  if (tmrOn && !awayOn && !asleep) {
    lastActive = now;
    if (now - lastDraw >= (tmrDone ? 120UL : 250UL)) { lastDraw = now; drawTimer(); }
    delay(5); return;
  }
  if (awayOn && !asleep) {
    if ((int32_t)(millis() - lastActive) > (int32_t)awayShowMs) {
      awayShowMs = AWAY_SHOW_MS;
      awayEyes(false);                   // eyes close, then dark
      goSleepQuick();
      return;
    }
    if (now - lastDraw >= 100) { lastDraw = now; drawAway(); }
    delay(5); return;
  }
  if (glanceUntil && now > glanceUntil) {
    glanceUntil = 0;
    if (!popOn && !pgUntil && !findUntil && upState == U_OFF && alertPhase == AL_NONE) goSleepQuick();
  }
  if (banUntil && now > banUntil) banUntil = 0;
  if (banUntil) {
    if (now - lastDraw >= 100) { lastDraw = now; oled.clearDisplay(); ctr(banText, 28, 1); oled.display(); }
    delay(2); return;
  }
  if (popOn && now > popUntil) popupClose(false);
  if (popOn) {
    lastActive = now;
    if (now - lastDraw >= 100) { lastDraw = now; drawPopup(); }
    delay(2); return;
  }
  if (notesDirty && now - notesSavedAt > 3000) saveNotes();

  // 6.0's cards, below the call to prayer and above everything else.
  if (tamperCountAt) {
    lastActive = now;
    if (now - lastDraw >= 100) { lastDraw = now; drawTamperCount(); }
    delay(2); return;
  }
  // 7.5: an update arriving over Bluetooth owns the screen
  if (otaOn) {
    lastActive = now;
    if (otaErr) otaStop("link");
    else if ((int32_t)(now - otaLastAt) > 20000) otaStop("timeout");
    else {
      int pct = otaSize ? (int)((uint64_t)otaGot * 100 / otaSize) : 0;
      if (pct / 5 != otaPctSent / 5) { otaPctSent = pct; char b[12]; snprintf(b, sizeof(b), "ota %d", pct); evtSend(b); }
      if (now - lastDraw >= 250) { lastDraw = now; drawBleOta(); }
      delay(2); return;
    }
  }
  if (nightCardUntil) {
    lastActive = now;
    if (now - lastDraw >= 200) { lastDraw = now; drawNightCard(); }
    delay(2); return;                                  // the card owns the screen
  }
  if (walkUntil && now > walkUntil) walkUntil = 0;
  if (walkUntil) {
    lastActive = now;
    if (now - lastDraw >= 120) { lastDraw = now; drawWalk(); }
    delay(2); return;
  }
  if (pgUntil && now > pgUntil) pgUntil = 0;
  if (pgUntil) {
    lastActive = now;
    if (now - lastDraw >= 120) { lastDraw = now; drawGuard(); }
    delay(2); return;
  }
  if (findUntil && now > findUntil) findUntil = 0;
  if (findUntil) {
    lastActive = now;
    if (now - lastDraw >= 120) { lastDraw = now; drawFind(); }
    delay(2); return;
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
        // The same fifteen minutes as snoozing it by hand. Nobody
        // answering and you waving it away mean the same thing, and
        // a ladder that asked again in five and then not for an hour
        // was two different behaviours for one situation. The count
        // still runs, so it still gives up rather than asking all
        // evening.
        r.tries++;
        r.at = (uint32_t)time(nullptr) + (uint32_t)REM_WAVED_MIN * 60UL;
        Serial.printf("reminder not answered, back in %d min (%u of %u)\n",
                      REM_WAVED_MIN, r.tries, (unsigned)REM_STEPS);
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
  if (relaxOn && relaxUntil && (long)(now - relaxUntil) >= 0) {
    relaxOn = false; relaxUntil = 0;
    goSleep();
    return;
  }
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

  // Gesture mode takes the panel the way the update screen does.
  // Nothing underneath it is being driven by the pad any more, so
  // drawing it would only be showing you a menu you cannot use.
  if (cfgGesture) {
    if (now - lastDraw >= 150) { lastDraw = now; drawGesture(); }
    delay(2);
    return;
  }

  if (now - lastDraw >= 110) {
    lastDraw = now;
    // Past five seconds of holding, the overlay is the screen.
    //
    // It used to be drawn over the top of whatever menu was there,
    // but the menu pushes itself to the panel before the overlay is
    // even drawn, so every frame sent two pictures: the menu, then
    // the ring. A hundred and ten milliseconds apart, which is what
    // the blinking was. Drawing one or the other fixes it and saves
    // a frame.
    if (sleepArmed) { drawHoldTier(now); oled.display(); delay(2); return; }
    switch (screen) {
      case S_FOCUS:    drawMac();      break;   // 7.5
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
      case S_TODAY: case S_FHUB: case S_CALM: drawHub(); break;   // 7.6
      default:         drawHome();     break;
    }
  }
  delay(2);
}

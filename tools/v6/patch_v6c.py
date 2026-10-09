#!/usr/bin/env python3
import re, sys
SRC = sys.argv[1]
s = open(SRC).read()

def rep(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f"anchor matched {n} times, wanted {count}:\n{old[:200]}")
    s = s.replace(old, new)

rep("static void saveWx();\n", "static void saveWx();\nstatic void tlogLoad();\nstatic void drawTlog();\n")

# ---------------------------------------------------------------- waking and sleeping
rep("""  asleep = false;
  setCpuFrequencyMhz(160);""", """  asleep = false;
  if (pmAvail) pmSet(false); else setCpuFrequencyMhz(160);""")

rep("""  if (wokeForAlarm || !macLinked) { wokeForAlarm = false; wantDeep = true; }
  else                            goSleep();""",
"""  // A linked phone is somebody listening too, as the Mac is.
  if ((wokeForAlarm || !macLinked) && !phoneHeld()) { wokeForAlarm = false; wantDeep = true; }
  else                                              { wokeForAlarm = false; goSleep(); }""")

# ---------------------------------------------------------------- presses on the new cards
OVER = """  // The new cards from 6.0 take a press before anything else: it
  // cancels a countdown, quiets the guard, stops the finder.
  if (tamperCountAt) { tamperCountAt = 0; flash("NOT ARMED", 1200); return; }
  if (pgUntil)       { pgUntil = 0; return; }
  if (findUntil)     { findUntil = 0; return; }
"""
rep("""static void touchGesture(uint8_t g) {
  lastActive = millis();
""", """static void touchGesture(uint8_t g) {
  lastActive = millis();
""" + OVER + """  if (screen == S_SETTINGS && depth == 2 && itemIdx == C_TLOG && g == TG_ONE) {
    if (tlN) tlSel = (tlSel + 1) % tlN;
    return;
  }
""")
rep("""static void knockOne() {
  cTap++;
""", """static void knockOne() {
  cTap++;
""" + OVER)

# ---------------------------------------------------------------- loop
rep("""  btTick();                            // the phone's clock and notifications
  if (now - lastPoll >= 45) { lastPoll = now; input(); }""",
"""  btTick();                            // the phone's clock and notifications
  serviceWs();                         // WiFi sessions, syncs, commands waiting
  pgTick();                            // the phone guard
  serviceTamper();
  if (wxDirty) { wxDirty = false; saveWx(); }

  // Light sleep, between passes of this loop, whenever the screen is
  // dark on Bluetooth. The phone stays linked through it. Not with a
  // cable in, because the USB port does not survive it.
  bool lsNow = asleep && pmAvail && cfgNet == NET_BT && !usb_serial_jtag_is_connected();
  pmSet(lsNow);
  if (now - lastPoll >= (lsNow ? 100UL : 45UL)) { lastPoll = now; input(); }""")

rep("""  uint32_t wait = offlineNow() ? 0 : deepAfterMs();
  if (!deepOff && !cfgGesture && (offlineNow() || deepAfterMs()) && asleep && !sessionRunning() &&
      !macLinked && (now - sleptAt) > wait && upState == U_OFF && !storyBusy) {
    goDeep();
  }""",
"""  //
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
      (now - since) > wait && upState == U_OFF && !storyBusy) {
    goDeep();
  }""")

rep("""  if (asleep) { delay(6); return; }""",
    """  if (asleep) { delay(lsNow ? 50 : 6); return; }""")

rep("""  if (alertPhase != AL_NONE) {                 // the call takes the screen
    lastActive = now;
    if (now - lastDraw >= 60) { lastDraw = now; drawPrayerAlert(); }
    delay(2);
    return;
  }""", """  if (alertPhase != AL_NONE) {                 // the call takes the screen
    lastActive = now;
    if (now - lastDraw >= 60) { lastDraw = now; drawPrayerAlert(); }
    delay(2);
    return;
  }

  // 6.0's cards, below the call to prayer and above everything else.
  if (tamperCountAt) {
    lastActive = now;
    if (now - lastDraw >= 100) { lastDraw = now; drawTamperCount(); }
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
  }""")

# ---------------------------------------------------------------- setup
rep("""  Serial.begin(115200);
  // Before the serial port settles, before anything is read and long
  // before anything is powered. A touch that was not meant costs this
  // much and nothing else.
  if (woke_ == ESP_SLEEP_WAKEUP_GPIO) wakeGate();""",
"""  Serial.begin(115200);
  // Armed for tamper: a wake is either something moving it or the end
  // of the pause after one. Either way it is written down and it goes
  // straight back. Nothing else runs, and this never returns.
  if (rtcTamper && (woke_ == ESP_SLEEP_WAKEUP_GPIO || woke_ == ESP_SLEEP_WAKEUP_TIMER))
    tamperWake(woke_ == ESP_SLEEP_WAKEUP_GPIO);
  // Before the serial port settles, before anything is read and long
  // before anything is powered. A touch that was not meant costs this
  // much and nothing else.
  if (woke_ == ESP_SLEEP_WAKEUP_GPIO) wakeGate();""")

rep("""  prefs.begin("nexus", false);
  cBoot = prefs.getUInt("boots", 0) + 1;""", """  prefs.begin("nexus", false);
  pmInit();                            // says whether light sleep is on offer
  cBoot = prefs.getUInt("boots", 0) + 1;""")

rep("""  if (prefs.isKey("net")) cfgNet = constrain(prefs.getInt("net", NET_WIFI), 0, NET_N - 1);
  else {
    cfgNet = prefs.getBool("offl", false) ? NET_OFF : NET_WIFI;
    prefs.putInt("net", cfgNet);
  }""", """  if (prefs.isKey("net")) cfgNet = constrain(prefs.getInt("net", NET_WIFI), 0, NET_N - 1);
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
  cfgQuiet   = prefs.getBool("quiet", false);
  lastSyncAt = prefs.getUInt("lsync", 0);""")

rep("""  loadTasks();
  loadRems();""", """  loadTasks();
  loadWx();
  loadRems();""")

rep("""  fsOk = LittleFS.begin(true);                 // format it once if it is blank
  if (!fsOk) Serial.println("no filesystem");""",
"""  fsOk = LittleFS.begin(true);                 // format it once if it is blank
  if (!fsOk) Serial.println("no filesystem");
  // Any real start ends a tamper watch. Deep sleep wakes never get
  // here while it is armed, so arriving here at all is the disarm.
  bool tamperWas = prefs.getBool("tamper", false);
  rtcTamper = 0;
  if (tamperWas) { prefs.putBool("tamper", false); tlogAdd("Disarmed"); }""")

rep("""  loadNets();
  // Safe mode is WiFi and nothing else. Gesture mode is already off
  // at boot and only the Mac turns it on, so the radio is the whole
  // of it: whatever was wrong can be fixed over the air from here.
  if (safeMode) {
    cfgNet = NET_WIFI;
    prefs.putInt("net", cfgNet);
    prefs.putInt("btry2", 0);
    flash("SAFE MODE", 1500);
    flash("ON WIFI, UPDATE ME", 1800);
  }""", """  loadNets();
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
  }""")

rep("""  if (cfgNet == NET_BT && prefs.getInt("btry2", 0) >= BT_GIVE_UP) {
    prefs.putInt("btry2", 0);
    cfgNet = NET_WIFI;
    prefs.putInt("net", cfgNet);
    btFellBack = true;
    Serial.printf("bluetooth failed %d starts running: back on WiFi\\n", BT_GIVE_UP);
    flash("BLUETOOTH FAILED", 1400);
    flash("BACK ON WIFI", 1200);
  }""", """  if (cfgNet == NET_BT && prefs.getInt("btry2", 0) >= BT_GIVE_UP) {
    prefs.putInt("btry2", 0);
    btFellBack = true;
    // For this start only. Home stays Bluetooth, and the next restart
    // tries it again, by which time an update may have fixed it.
    if (netCount) { cfgNet = NET_WIFI; wsKind = WS_MANUAL; wsStartMs = wsLastUse = millis(); }
    else          { cfgNet = NET_OFF; needHotspot = true; }
    Serial.printf("bluetooth failed %d starts running: %s for now\\n", BT_GIVE_UP,
                  netCount ? "wifi" : "the hotspot");
    flash("BLUETOOTH FAILED", 1400);
    flash(netCount ? "ON WIFI FOR NOW" : "HOTSPOT FOR NOW", 1200);
  }""")

rep("""  } else if (!fromDeep && !cfgOffline) {
    offlineWelcome();
  }""", """  } else if (!fromDeep && !cfgOffline) {
    offlineWelcome();
  }
  if (needHotspot) startHotspot();
  if (tamperWas) flash("TAMPER OFF, SEE LOG", 1800);""")

open(SRC, 'w').write(s)
print("part 3 ok")

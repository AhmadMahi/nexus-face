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

def rep_re(pat, new):
    global s
    m = list(re.finditer(pat, s, re.S))
    if len(m) != 1:
        sys.exit(f"regex matched {len(m)} times:\n{pat[:200]}")
    s = s[:m[0].start()] + new + s[m[0].end():]

# ---------------------------------------------------------------- settings rows
rep("""       C_AUTOUP, C_RESET, C_REBOOT, C_ABOUT, C_COUNT };""",
    """       C_AUTOUP, C_RESET, C_REBOOT, C_ABOUT,
       C_GUARD, C_TAMPER, C_TLOG, C_COUNT };""")
rep("""    "Reset settings", "Reboot", "About" };""",
    """    "Reset settings", "Reboot", "About",
    "Phone guard", "Tamper alarm", "Tamper log" };""")
rep("""  { C_MODE,   C_HOTSPOT, C_PAIR,  C_PRAYER, C_UPDATE, C_AUTOUP, SG_END,  SG_END },""",
    """  { C_MODE,   C_HOTSPOT, C_PAIR,  C_PRAYER, C_UPDATE, C_AUTOUP, C_GUARD, SG_END },""")
rep("""  { C_DEEP,   C_BATT,    C_RESET, C_REBOOT, C_ABOUT,  SG_END,   SG_END,  SG_END },""",
    """  { C_DEEP,   C_BATT,    C_TAMPER, C_TLOG, C_RESET,  C_REBOOT, C_ABOUT, SG_END },""")

# row values
rep("""      case C_MODE:   snprintf(v, sizeof(v), "%s",
                              cfgNet == NET_OFF ? "off" :
                              cfgNet == NET_BT  ? btShort() :
                              (netDown ? "no signal" : "wifi")); break;""",
"""      case C_MODE:   snprintf(v, sizeof(v), "%s",
                              cfgNet == NET_OFF ? "off" :
                              cfgNet == NET_BT  ? btShort() :
                              rescueAP ? "hotspot" :
                              wsKind == WS_SYNC ? "syncing" :
                              (netDown ? "no signal" : "wifi now")); break;
      case C_GUARD:  snprintf(v, sizeof(v), "%s", cfgPGuard ? "on" : "off"); break;
      case C_TAMPER: snprintf(v, sizeof(v), "hold"); break;
      case C_TLOG:   snprintf(v, sizeof(v), "hold"); break;""")

rep("""  if (depth == 2 && itemIdx == C_PAIR)  { drawPair();  return; }""",
    """  if (depth == 2 && itemIdx == C_PAIR)  { drawPair();  return; }
  if (depth == 2 && itemIdx == C_TLOG)  { drawTlog();  return; }""")

# row actions
rep("""      case C_UPDATE:
        if (online()) { upState = U_MENU; upPick = 0; upMsg = ""; }
        else { upState = U_FAIL; upMsg = "No network"; }
        break;""",
"""      case C_UPDATE:
        // On Bluetooth this brings WiFi up for the length of the
        // conversation and takes it down again when you leave.
        if (online()) { upState = U_MENU; upPick = 0; upMsg = ""; upQuick = false; }
        else if (wsStart(WS_UPDATE)) { upAfterJoin = true; upDirect = false; upState = U_LOOK; upMsg = ""; }
        else { upState = U_FAIL; upMsg = "No saved WiFi"; upQuick = true; }
        break;
      case C_GUARD:
        cfgPGuard = !cfgPGuard;
        prefs.putBool("guard", cfgPGuard);
        pgFired = false; pgUntil = 0;
        flash(cfgPGuard ? "GUARD ON" : "GUARD OFF", 1000);
        break;
      case C_TAMPER: tamperArm(); break;
      case C_TLOG:   tlogLoad(); depth = 2; break;""")

rep_re(r"      case C_MODE:\n        // Walked, the way every other list is walked.*?        break;\n      case C_BIKE:\n",
"""      case C_MODE:
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
""")

# the update conversation: a question asked from a sync or a Shortcut
# ends on No, rather than dropping you into the menu
rep("""        if (upYes) { upState = U_OFF; otaInstall(); upState = U_FAIL; }  // returns only if it failed
        else upState = U_MENU;
        return;
      }
      upState = U_MENU;
      return;""",
"""        if (upYes) { upState = U_OFF; otaInstall(); upState = U_FAIL; }  // returns only if it failed
        else upState = upQuick ? U_OFF : U_MENU;
        return;
      }
      upState = upQuick ? U_OFF : U_MENU;
      return;""")
rep("""    default:                                   // it said its piece
      upState = U_MENU;
      return;""",
"""    default:                                   // it said its piece
      upState = upQuick ? U_OFF : U_MENU;
      return;""")

# ---------------------------------------------------------------- hotspot
rep_re(r"static void startHotspot\(\) \{\n  if \(rescueAP\) return;.*?\n\}\n",
r"""static void startHotspot() {
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
  toastText = String(RESCUE_SSID) + " " + RESCUE_PASS + "\n192.168.4.1/ota";
  toastUntil = millis() + 20000; toastFlash = millis(); remShowing = -1;
  wake("hotspot");
}
""")

rep("""  if (toastKind == "note")   head = "REMINDERS";""",
    """  if (toastKind == "note")   head = "REMINDERS";
  if (toastKind == "rafiq")  head = "RAFIQ";
  if (toastKind == "hotspot") head = "HOTSPOT";""")

# ---------------------------------------------------------------- reset
rep("""  cfgNet = NET_WIFI; prefs.putInt("net", cfgNet);""",
    """  cfgNetHome = NET_BT; prefs.putInt("net", cfgNetHome);
  cfgPGuard = false; prefs.putBool("guard", cfgPGuard);
  cfgQuiet = false;  prefs.putBool("quiet", cfgQuiet);""")

# ---------------------------------------------------------------- web
rep("""static bool guard() {
  if (!authed()) { web.send(401, "application/json", "{\\"ok\\":false,\\"err\\":\\"pair first\\"}"); return false; }
  sawMac();""",
"""static bool guard() {
  if (!authed()) { web.send(401, "application/json", "{\\"ok\\":false,\\"err\\":\\"pair first\\"}"); return false; }
  wsTouch();                           // somebody is using the WiFi
  sawMac();""")
rep("""  web.on("/api/state", HTTP_GET, []() { if (!authed()) { web.send(401, "application/json", "{\\"ok\\":false}"); return; } sawMac(); apiState(); });""",
    """  web.on("/api/state", HTTP_GET, []() { if (!authed()) { web.send(401, "application/json", "{\\"ok\\":false}"); return; } wsTouch(); sawMac(); apiState(); });
  // Installing from a file, for when there is no internet to reach
  // GitHub with: join the hotspot and open this. Only while the
  // hotspot is up, or for a paired Mac, so a page on the home network
  // cannot be used to put something else on the robot.
  web.on("/ota", HTTP_GET, []() { wsTouch(); web.send_P(200, "text/html; charset=utf-8", OTA_PAGE); });
  web.on("/ota", HTTP_POST, []() {
    bool ok = otaUpOk && !Update.hasError();
    web.send(200, "text/plain", ok ? "Installed. Rafiq is restarting." : ("Not installed: " + otaUpErr));
    if (ok) { delay(800); ESP.restart(); }
  }, []() { otaUpload(); });""")
rep("""    web.send_P(200, "text/html; charset=utf-8", PAGE);
  });""", """    wsTouch();
    web.send_P(200, "text/html; charset=utf-8", PAGE);
  });""")

rep("""    else if (k == "net")  { cfgNet = constrain(v, 0, NET_N - 1);         prefs.putInt("net", cfgNet);
                            if (cfgNet == NET_BT) bleOn(); else bleOff();
                            if (cfgOffline) { WiFi.disconnect(true, false); WiFi.mode(WIFI_OFF); }
                            else { netDown = false; netMisses = 0; netNextTry = 0;
                                   WiFi.mode(WIFI_STA); WiFi.setSleep(false); setupWeb(); } }
    else if (k == "offl") { cfgNet = v ? NET_OFF : NET_WIFI;             prefs.putInt("net", cfgNet);
                            if (cfgOffline) { WiFi.disconnect(true, false); WiFi.mode(WIFI_OFF); }
                            else { netDown = false; netMisses = 0; netNextTry = 0;
                                   WiFi.mode(WIFI_STA); WiFi.setSleep(false); setupWeb(); } }""",
"""    // WiFi is a session, never a home. Asking for it here keeps this
    // session going until a restart; asking for Bluetooth or Off makes
    // that home and ends the session once this answer has gone out.
    else if (k == "net" || k == "offl") {
                            int want = (k == "offl") ? (v ? NET_OFF : NET_BT) : constrain(v, 0, NET_N - 1);
                            if (want == NET_WIFI) wsStart(WS_MANUAL);
                            else {
                              cfgNetHome = want; prefs.putInt("net", cfgNetHome);
                              if (cfgNet == NET_WIFI) wsEndWant = true;
                              else { cfgNet = want; if (want == NET_BT) bleOn(); else bleOff(); }
                            } }""")

rep("""  o += "\\"net\\":" + String(cfgNet) + ",";""",
    """  o += "\\"net\\":" + String(cfgNet) + ",";
  o += "\\"home\\":" + String(cfgNetHome) + ",\\"ws\\":" + String(wsKind) + ",";
  o += "\\"guard\\":" + String(cfgPGuard ? "true" : "false") + ",";""")

# ---------------------------------------------------------------- weather
rep("""  wxOk  = !isnan(wTemp);
}""", """  wxOk  = !isnan(wTemp);
  if (wxOk) { wxAt = (uint32_t)time(nullptr); wxDirty = true; }   // kept by loop()
}""")
rep("""  if (s == S_WEATHER) return !offlineNow();""",
    """  if (s == S_WEATHER) return !offlineNow() || wxOk;   // the last report is still worth showing""")
rep("""  snprintf(l, sizeof(l), "%d%%", (int)roundf(wHum));
  at(30 + tw + 10, 30, l);""", """  if (!isnan(wHum)) {
    snprintf(l, sizeof(l), "%d%%", (int)roundf(wHum));
    at(30 + tw + 10, 30, l);
  }""")
rep("""  snprintf(l, sizeof(l), "%s  %.0f km/h", wCity, wWind);
  ctr(l, 54, 1);""", """  // Off the network it is a report, not a reading, so it says how old.
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
  ctr(ln, 54, 1);""")

# system screen network line
rep("""  else if (rescueAP) snprintf(v, sizeof(v), "hotspot");
  else               snprintf(v, sizeof(v), "offline");""",
    """  else if (rescueAP) snprintf(v, sizeof(v), "hotspot");
  else if (cfgNet == NET_BT) snprintf(v, sizeof(v), "bt %s", btShort());
  else               snprintf(v, sizeof(v), "offline");""")

open(SRC, 'w').write(s)
print("part 2 ok")

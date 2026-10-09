# Rafiq 7.3.1: two devices really connect, the Mac is never "the phone",
# and the clock never randomly reads as missing.
import re, sys
SRC=sys.argv[1]; s=open(SRC).read()
def rep(a,b,c=1):
    global s
    n=s.count(a)
    if n!=c: sys.exit(f"anchor {n}x:\n{a[:200]}")
    s=s.replace(a,b)
rep('#define FW_VERSION "7.3.0"', '#define FW_VERSION "7.3.1"')

# ---- 1. the clock, read straight ----
n_before = len(re.findall(r'getLocalTime\(&([A-Za-z0-9_]+), 0\)', s))
s = re.sub(r'getLocalTime\(&([A-Za-z0-9_]+), 0\)', r'nowLocal(&\1)', s)
rep("volatile uint8_t btNews = 0;", """volatile uint8_t btNews = 0;
// getLocalTime(&t, 0) can give up without looking: it notes millis(),
// then only looks while no millisecond has passed since. If the counter
// ticks between the two, it reports "no time" with the clock fine. That
// was the robot's "waiting for the clock" face appearing at random, and
// the same answer reached prayer alerts and reminders. This just looks.
static bool nowLocal(struct tm* t) {
  time_t n = time(nullptr);
  localtime_r(&n, t);
  return t->tm_year > (2016 - 1900);
}""")
print("clock reads replaced:", n_before)

# ---- 2. the Mac is a companion, never the phone ----
rep("static bool anyLinked() {", """static bool linkIsMac(int i) {
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
static bool anyLinked() {""")
rep("""    // The preferred phone always becomes the phone.
    if (rank == 1 && h != btConn) {""", """    // The preferred phone always becomes the phone (but never a Mac).
    if (rank == 1 && h != btConn && !linkIsMac(i)) {""")
rep("""    if (!cfgMulti && linkN > 1 && sv && h != btConn) sv->disconnect(h);   // one at a time when Multi-link is off
  }""", """    if (!cfgMulti && linkN > 1 && sv && h != btConn) sv->disconnect(h);   // one at a time when Multi-link is off
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
      Serial.printf("links: %d linked, advertising again\\n", linkN);
    }
  }""")
rep("static bool anyLinked();\n", "static bool anyLinked();\nstatic void phoneFix();\n")
# the Mac saying who it is settles who is the phone
rep("""        if (li >= 0 && links[li].authed) devLabel(links[li].a, m.data + 4, false);""",
    """        if (li >= 0 && links[li].authed) devLabel(links[li].a, m.data + 4, false);
        phoneFix();""")

# ---- 3. say how many are linked ----
rep("""      case C_MULTI:  snprintf(v, sizeof(v), "%s", cfgMulti ? "on" : "off"); break;""",
    """      case C_MULTI:  if (cfgMulti) snprintf(v, sizeof(v), "on, %d in", linkN);
                     else          snprintf(v, sizeof(v), "off"); break;""")
open(SRC,'w').write(s); print("7.3.1 ok")

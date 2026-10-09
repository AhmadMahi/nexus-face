# Rafiq 7.1.0: movement no longer keeps the screen awake. Only real
# interaction (touch, knock, a lean or shake that acts, any screen in
# use) restarts the Sleep after countdown.
import sys
SRC=sys.argv[1]; s=open(SRC).read()
def rep(old,new,count=1):
    global s
    n=s.count(old)
    if n!=count: sys.exit(f"anchor {n}x:\n{old[:200]}")
    s=s.replace(old,new)
rep('#define FW_VERSION "7.0.0"', '#define FW_VERSION "7.1.0"')
# a shake still goes back, but jiggling in a bag must not hold the screen
rep("""    if (asleep) { if (motionWakes()) wake("shake"); return; }
    lastActive = now;""", """    if (asleep) { if (motionWakes()) wake("shake"); return; }
    // 7.1: a shake still steps back (below), but is not "activity": a
    // bag being carried shakes, and that must not keep the screen lit.""")
rep("""    if (asleep) { awayEv(AE_MOVE); if (!motionWakes()) return; wake("picked up"); }
    lastActive = now;""", """    if (asleep) { awayEv(AE_MOVE); if (!motionWakes()) return; wake("picked up"); }
    // 7.1: moving it wakes it (above, by the Wake by setting) but does
    // not keep it awake. Only touching, knocking and using a screen do.""")
rep("""    if (asleep) { awayEv(AE_MOVE); if (!motionWakes()) return; wake("moved"); }
    lastActive = now;""", """    if (asleep) { awayEv(AE_MOVE); if (!motionWakes()) return; wake("moved"); }
    // 7.1: turning it over is not an interaction either.""")
open(SRC,'w').write(s); print("7.1 ok")
s=open(SRC).read()
old="""  // the counter paces itself, and holds the screen while it runs
  if (screen == S_FAITH && depth == 2 && itemIdx == F_ZIKR) {"""
new="""  // 7.1: the sensor and knock tests are about moving it, so while one
  // is open the screen holds itself, as a game does.
  if (screen == S_SETTINGS && depth == 2 && (itemIdx == C_ACCEL || tapTesting)) lastActive = now;

  // the counter paces itself, and holds the screen while it runs
  if (screen == S_FAITH && depth == 2 && itemIdx == F_ZIKR) {"""
assert s.count(old)==1; s=s.replace(old,new); open(SRC,'w').write(s); print("7.1 tests hold ok")

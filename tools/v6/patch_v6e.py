import sys
SRC=sys.argv[1]; s=open(SRC).read()
def rep(old,new,count=1):
    global s
    n=s.count(old)
    if n!=count: sys.exit(f"anchor {n}x:\n{old[:160]}")
    s=s.replace(old,new)
# the network task says when it is in the middle of something
rep("""    } else {
      netMisses = 0;
      hadNet = true;
      if (wantTime)""", """    } else {
      netMisses = 0;
      hadNet = true;
      // Each flag is cleared before its fetch starts, so the flags alone
      // cannot say a fetch is still running. This can: a session must
      // not take the radio away in the middle of one.
      netBusy = true;
      if (wantTime)""")
rep("""        if (seq == upSeq) upState = ok ? U_LIST : U_FAIL;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(40));""", """        if (seq == upSeq) upState = ok ? U_LIST : U_FAIL;
      }
      netBusy = false;
    }
    vTaskDelay(pdMS_TO_TICKS(40));""")
rep("volatile bool wxDirty = false;\n", "volatile bool wxDirty = false;\nvolatile bool netBusy = false;        // the network task is mid-fetch\n")
# wsEnd waits a little for a fetch to finish rather than cutting it off
rep("""static void wsEnd(const char* why) {
  if (cfgNet != NET_WIFI) return;
  wantTime = false;""", """static void wsEnd(const char* why) {
  if (cfgNet != NET_WIFI) return;
  // Up to fifteen seconds for a fetch that is already under way. The
  // network task runs while this waits, because this yields.
  for (int i = 0; i < 150 && netBusy; i++) vTaskDelay(pdMS_TO_TICKS(100));
  wantTime = false;""")
# sync is done only when the task is idle too
rep("""  if (syncRun && syncStarted && !wantTime && !wantWx && !wantPrayerNow &&
      !wantOtaLatest && !syncUpArmed && upState == U_OFF && !storyBusy) {""",
    """  if (syncRun && syncStarted && !wantTime && !wantWx && !wantPrayerNow &&
      !wantOtaLatest && !netBusy && !syncUpArmed && upState == U_OFF && !storyBusy) {""")
rep("""  if (wsKind == WS_MANUAL && rtcWsUntil && !syncRun && upState == U_OFF &&""",
    """  if (wsKind == WS_MANUAL && rtcWsUntil && !syncRun && !netBusy && upState == U_OFF &&""")
open(SRC,'w').write(s); print("part 5 ok")

# Rafiq 7.2.1: the status the app reads says how many commands and
# notifications arrived from it, and the last one, so a link that reads
# but does not write can be told apart from one where nothing happens.
import sys
SRC=sys.argv[1]; s=open(SRC).read()
def rep(old,new,count=1):
    global s
    n=s.count(old)
    if n!=count: sys.exit(f"anchor {n}x:\n{old[:200]}")
    s=s.replace(old,new)
rep('#define FW_VERSION "7.2.0"', '#define FW_VERSION "7.2.1"')
rep("uint32_t appNoteSeq = 0;", "uint32_t appNoteSeq = 0;\nuint16_t appRxCmd = 0, appRxNote = 0;   // what has arrived from the app\nchar     appLast[16] = \"\";")
rep("""    snprintf(b, sizeof(b),
             "fw=%s;bat=%d;away=%d;timer=%ld;unread=%d;quiet=%d;guard=%d;wake=%d;h12=%d;clock=%d",
             FW_VERSION, isnan(battV) ? -1 : battPct(battV), awayOn ? 1 : 0, tl,
             noteUnread(), cfgQuiet ? 1 : 0, cfgPGuard ? 1 : 0, cfgWakeBy, cfg12h ? 1 : 0,
             timeOk ? 1 : 0);""", """    snprintf(b, sizeof(b),
             "fw=%s;bat=%d;away=%d;timer=%ld;unread=%d;quiet=%d;guard=%d;wake=%d;h12=%d;clock=%d;rxc=%u;rxn=%u;last=%s",
             FW_VERSION, isnan(battV) ? -1 : battPct(battV), awayOn ? 1 : 0, tl,
             noteUnread(), cfgQuiet ? 1 : 0, cfgPGuard ? 1 : 0, cfgWakeBy, cfg12h ? 1 : 0,
             timeOk ? 1 : 0, (unsigned)appRxCmd, (unsigned)appRxNote, appLast);""")
rep("""    char b[180];
    long tl = (tmrOn && !tmrDone)""", """    char b[220];
    long tl = (tmrOn && !tmrDone)""")
rep("""    if (m.kind == 1) {                             // a command
      Serial.printf("app: %s\\n", m.data);""", """    if (m.kind == 1) {                             // a command
      Serial.printf("app: %s\\n", m.data);
      appRxCmd++;
      snprintf(appLast, sizeof(appLast), "%.15s", m.data);
      for (char* q = appLast; *q; q++) if (*q == ';' || *q == '=') *q = ' ';""")
rep("""    } else if (m.kind == 2) {                      // a notification""", """    } else if (m.kind == 2) {                      // a notification
      appRxNote++;""")
open(SRC,'w').write(s); print("7.2.1 ok")

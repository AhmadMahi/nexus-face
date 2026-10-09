# Rafiq 6.6.0: commands from a Shortcut that start a WiFi session
# (config, sync, update) were ended in the same pass that started them.
import sys
SRC=sys.argv[1]; s=open(SRC).read()
def rep(old,new,count=1):
    global s
    n=s.count(old)
    if n!=count: sys.exit(f"anchor {n}x:\n{old[:200]}")
    s=s.replace(old,new)
rep('#define FW_VERSION "6.5.0"', '#define FW_VERSION "6.6.0"')
rep("""      case RQ_REBOOT:  ESP.restart(); break;
    }
  }""", """      case RQ_REBOOT:  ESP.restart(); break;
    }
    // 6.6: the command above may have just started a session and
    // stamped it with a time later than the one read at the top. Read
    // it again, or "now - started" goes below zero, wraps round to
    // forty-nine days, and the session is ended as idle in the same
    // pass that began it. That is why config from a Shortcut showed
    // the hotspot card with no hotspot behind it, and why a sync or
    // an update sent from a Shortcut stopped at once.
    now = millis();
  }""")
# the hotspot card stays up while you join, until a tap or two minutes
rep("""  toastUntil = millis() + 20000; toastFlash = millis(); remShowing = -1;
  wake("hotspot");""", """  toastUntil = millis() + 120000; toastFlash = millis(); remShowing = -1;
  wake("hotspot");""")
open(SRC,'w').write(s); print("6.6 ok")

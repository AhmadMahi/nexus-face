# Rafiq 7.6.1: the hubs open. The loop closes any screen it believes has
# nothing inside (screenHasDepth), and the hubs were not on its list, so
# a hold opened Today's menu and the very next pass of the loop shut it.
import sys
SRC=sys.argv[1]; s=open(SRC).read()
def rep(a,b,c=1):
    global s
    n=s.count(a)
    if n!=c: sys.exit(f"anchor {n}x:\n{a[:200]}")
    s=s.replace(a,b)
rep('#define FW_VERSION "7.6.0"', '#define FW_VERSION "7.6.1"')
rep("""static bool screenHasDepth(int s) {
  return s == S_FAITH || s == S_READS || s == S_GAMES ||
         s == S_FOCUS || s == S_SETTINGS || s == S_REMIND ||
         s == S_MSG;
}""", """static bool screenHasDepth(int s) {
  return s == S_FAITH || s == S_READS || s == S_GAMES ||
         s == S_FOCUS || s == S_SETTINGS || s == S_REMIND ||
         s == S_MSG ||
         s == S_TODAY || s == S_FHUB || s == S_CALM;     // 7.6.1: the hubs have menus
}""")
open(SRC,'w').write(s); print("7.6.1 ok")

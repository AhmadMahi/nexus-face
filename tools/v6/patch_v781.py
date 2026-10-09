# Rafiq 7.8.1: the call popup's "hold:decline" was 72 px in a 62 px
# button and ran past the edge of the screen. "hold:deny" fits.
import sys
SRC=sys.argv[1]; s=open(SRC).read()
def rep(a,b,c=1):
    global s
    n=s.count(a)
    if n!=c: sys.exit(f"anchor {n}x:\n{a[:200]}")
    s=s.replace(a,b)
rep('#define FW_VERSION "7.8.0"', '#define FW_VERSION "7.8.1"')
rep('  if (popRinging()) twoButtons("tap:answer", "hold:decline");', '  if (popRinging()) twoButtons("tap:answer", "hold:deny");     // 7.8.1: fits its button')
open(SRC,'w').write(s); print("7.8.1 ok")

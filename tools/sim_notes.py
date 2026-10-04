"""The notifications: Apple's wire format, and the list they land in.

onNotifSource, onDataSource, addNote and the helpers are lifted out of
the sketch, compiled, and fed real ANCS traffic, including answers
split across packets the way they actually arrive. This is the check
that found the attribute count: ancsAsk requests three attributes and
the parser looped four times, so the last turn ran off the end of a
complete answer and returned as though more were coming. Every
notification was parsed and then dropped on the floor, and nothing
that only reads the source would ever have said so.
"""
import re, sys, os, subprocess, tempfile

src = open("nexus-repo/nexus_face/nexus_face.ino").read()
fails = []
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)
print()

def grab(sig):
    i = src.index(sig); depth = 0; out = []
    for ch in src[i:]:
        out.append(ch)
        if ch == "{": depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0: break
    return "".join(out)

def const(n):
    return int(re.search(r"#define %s\s+(\d+)" % n, src).group(1))

NOTE_MAX   = const("NOTE_MAX")
UIDQ_N     = const("UIDQ_N")
ANCS_ATTRS = const("ANCS_ATTRS")

STRUCT = src[src.index("struct Note {"):]
STRUCT = STRUCT[:STRUCT.index("};") + 2]

ns  = grab("static void onNotifSource(NimBLERemoteCharacteristic* c, uint8_t* d,")
ds  = grab("static void onDataSource(NimBLERemoteCharacteristic* c, uint8_t* d,")
add = grab("static void addNote(const Note& n) {")
unr = grab("static int noteUnread() {")
app = grab("static const char* appShort(const Note& n) {")
isc = grab("static bool noteIsCall(const Note& n) {")
for name, blob in (("onNotifSource", ns), ("onDataSource", ds)):
    globals()[name] = blob.replace("NimBLERemoteCharacteristic* c", "void* c")

HARNESS = r"""
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
static unsigned long _t = 1000;
#define millis() (_t)
@@STRUCT@@
#define NOTE_MAX @@NMAX@@
#define ANCS_ATTRS @@ATTRS@@
#define UIDQ_N @@UIDQ@@
#define CAT_CALL 1
#define CAT_MISSED 2
#define CAT_VOICE 3
Note notes[NOTE_MAX];
int  noteN = 0;
unsigned long noteTotal = 0;
volatile uint32_t uidQ[UIDQ_N];
volatile uint8_t  uidQCat[UIDQ_N];
volatile uint8_t  uidHead = 0, uidTail = 0;
Note noteStage;
volatile bool noteReady = false;
uint8_t dsBuf[512];
size_t  dsLen = 0;
uint8_t dsCat = 0;
bool    ancsBusy = false;
@@NS@@
@@DS@@
@@ADD@@
@@UNR@@
@@APP@@
@@ISC@@
static int hexbuf(const char* h, uint8_t* out) {
  int n = 0;
  for (const char* p = h; p[0] && p[1]; p += 2) {
    char b[3] = { p[0], p[1], 0 };
    out[n++] = (uint8_t)strtol(b, NULL, 16);
  }
  return n;
}
int main(void) {
  char line[4096];
  while (fgets(line, sizeof line, stdin)) {
    char* nl = strchr(line, '\n'); if (nl) *nl = 0;
    char* sp = strchr(line, ' ');
    char* arg = sp ? sp + 1 : (char*)"";
    if (sp) *sp = 0;
    if (!strcmp(line, "NS")) {
      uint8_t b[64]; int n = hexbuf(arg, b);
      onNotifSource(NULL, b, n, true);
      printf("Q %d\n", (uidHead - uidTail + UIDQ_N) % UIDQ_N);
    } else if (!strcmp(line, "CAT")) {
      dsCat = (uint8_t)atoi(arg);
      printf("OK\n");
    } else if (!strcmp(line, "DS")) {
      uint8_t b[512]; int n = hexbuf(arg, b);
      onDataSource(NULL, b, n, true);
      if (noteReady) {
        printf("NOTE %u|%u|%s|%s|%s\n", noteStage.uid, noteStage.cat,
               noteStage.app, noteStage.title, noteStage.msg);
      } else printf("WAIT\n");
    } else if (!strcmp(line, "COMMIT")) {
      noteReady = false; addNote(noteStage);
      printf("N %d unread %d total %lu\n", noteN, noteUnread(), noteTotal);
    } else if (!strcmp(line, "RESET")) {
      noteN = 0; noteTotal = 0; dsLen = 0; noteReady = false;
      uidHead = uidTail = 0; memset(notes, 0, sizeof notes);
      printf("OK\n");
    } else if (!strcmp(line, "TICK")) {
      _t += (unsigned long)atoi(arg); printf("OK\n");
    } else if (!strcmp(line, "LIST")) {
      printf("L");
      for (int i = 0; i < noteN; i++) printf(" %u", notes[i].uid);
      printf("\n");
    } else if (!strcmp(line, "APP")) {
      Note n; memset(&n, 0, sizeof n);
      strncpy(n.app, arg, sizeof(n.app) - 1);
      printf("A %s\n", appShort(n));
    }
    fflush(stdout);
  }
  return 0;
}
"""

d = tempfile.mkdtemp()
c = os.path.join(d, "n.cpp")
_h = HARNESS
for _k, _v in [("@@STRUCT@@", STRUCT), ("@@NMAX@@", str(NOTE_MAX)),
               ("@@ATTRS@@", str(ANCS_ATTRS)), ("@@UIDQ@@", str(UIDQ_N)),
               ("@@NS@@", onNotifSource), ("@@DS@@", onDataSource),
               ("@@ADD@@", add), ("@@UNR@@", unr), ("@@APP@@", app), ("@@ISC@@", isc)]:
    _h = _h.replace(_k, _v)
open(c, "w").write(_h)
EXE = os.path.join(d, "n")
r = subprocess.run(["c++", "-std=c++17", "-O1", "-w", "-o", EXE, c], capture_output=True, text=True)
assert r.returncode == 0, "extracted code will not compile:\n" + r.stderr[:800]

class Box:
    def __init__(self):
        self.p = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  text=True, bufsize=1)
    def cmd(self, c):
        self.p.stdin.write(c + "\n"); self.p.stdin.flush()
        return self.p.stdout.readline().strip()

def attr(i, val):
    b = val.encode()
    return bytes([i, len(b) & 0xFF, len(b) >> 8]) + b

def ds_payload(uid, app, title, msg):
    return (bytes([0x00]) + uid.to_bytes(4, "little")
            + attr(0, app) + attr(1, title) + attr(3, msg)).hex()

def ns_payload(evt, flags, cat, uid):
    return bytes([evt, flags, cat, 1]).hex() + uid.to_bytes(4, "little").hex()

# ---------------------------------------------------------------
def t_one_notification():
    b = Box(); b.cmd("RESET"); b.cmd("CAT 4")
    out = b.cmd("DS " + ds_payload(4242, "com.apple.MobileSMS", "Ammi", "Are you coming?"))
    assert out.startswith("NOTE"), f"a complete answer was not staged: {out}"
    uid, cat, ap, ti, ms = out[5:].split("|")
    assert (int(uid), int(cat), ap, ti, ms) == (4242, 4, "com.apple.MobileSMS", "Ammi", "Are you coming?"), out
    print("        uid, category, app, title and body all come back intact")
run("A complete answer becomes a notification", t_one_notification)

def t_the_count_must_match():
    """The bug this file was written for. With the loop running one
    more time than there are attributes, a COMPLETE answer takes the
    'more is still coming' branch and the notification is silently
    lost. Asserted on the real constant, both ways."""
    body = grab("static void onDataSource(NimBLERemoteCharacteristic* c, uint8_t* d,")
    assert "for (int a = 0; a < ANCS_ATTRS; a++)" in body, \
        "the loop count is a literal again; it must be the same symbol ancsAsk uses"
    ask = grab("static void ancsAsk(uint32_t uid) {")
    requested = len(re.findall(r"\n\s+0x0[013],", ask))
    assert requested == ANCS_ATTRS, \
        f"ancsAsk requests {requested} attributes but the parser expects {ANCS_ATTRS}"
    print(f"        ancsAsk requests {requested}, onDataSource expects {ANCS_ATTRS}")
run("The parser expects exactly what was asked for", t_the_count_must_match)

def t_split_across_packets():
    """ANCS answers arrive in MTU sized pieces. Nothing may be staged
    until the last one lands."""
    full = ds_payload(77, "com.apple.mobilemail", "Invoice",
                      "The quarterly statement you asked for is attached.")
    for cut in (6, 14, 30, len(full) // 2, len(full) - 4):
        b = Box(); b.cmd("RESET"); b.cmd("CAT 6")
        a, z = full[:cut * 2 // 2], full[cut * 2 // 2:]
        if len(a) % 2: a, z = full[:cut - 1], full[cut - 1:]
        first = b.cmd("DS " + a)
        assert first == "WAIT", f"staged a half notification after {cut} chars: {first}"
        out = b.cmd("DS " + z)
        assert out.startswith("NOTE"), f"never completed after the rest arrived: {out}"
        assert "Invoice" in out and "quarterly" in out
    print("        five different split points, none staged early, all completed")
run("An answer split across packets is reassembled", t_split_across_packets)

def t_long_fields_are_cut_not_overrun():
    b = Box(); b.cmd("RESET"); b.cmd("CAT 4")
    out = b.cmd("DS " + ds_payload(9, "a" * 60, "T" * 200, "M" * 400))
    assert out.startswith("NOTE"), out
    _, _, ap, ti, ms = out[5:].split("|")
    sizes = dict(re.findall(r"char\s+(\w+)\[(\d+)\]", STRUCT))
    assert len(ap) <= int(sizes["app"]) - 1, f"app field overran: {len(ap)}"
    assert len(ti) <= int(sizes["title"]) - 1, f"title overran: {len(ti)}"
    assert len(ms) <= int(sizes["msg"]) - 1, f"message overran: {len(ms)}"
    print(f"        app {len(ap)}, title {len(ti)}, body {len(ms)}, all inside their buffers")
run("Oversized fields are truncated, not written past", t_long_fields_are_cut_not_overrun)

def t_notification_source_filters():
    b = Box(); b.cmd("RESET")
    assert b.cmd("NS " + ns_payload(0, 0x00, 4, 11)) == "Q 1", "a new one was not queued"
    assert b.cmd("NS " + ns_payload(0, 0x04, 4, 12)) == "Q 1", \
        "a pre existing notification was queued; the backlog would flood in on every connect"
    assert b.cmd("NS " + ns_payload(1, 0x00, 4, 13)) == "Q 1", "a modify was treated as new"
    assert b.cmd("NS " + ns_payload(2, 0x00, 4, 14)) == "Q 1", "a removal was treated as new"
    assert b.cmd("NS 0001") == "Q 1", "a runt packet was not ignored"
    print("        added only, and never the backlog iOS replays on connect")
run("Only genuinely new notifications are queued", t_notification_source_filters)

def t_queue_does_not_corrupt_when_full():
    b = Box(); b.cmd("RESET")
    last = ""
    for i in range(UIDQ_N + 8):
        last = b.cmd("NS " + ns_payload(0, 0, 4, 100 + i))
    depth = int(last.split()[1])
    assert depth == UIDQ_N - 1, f"ring holds {depth}, expected {UIDQ_N - 1} before wrapping"
    print(f"        {UIDQ_N + 8} arrivals into a {UIDQ_N} slot ring: holds {depth}, drops the rest")
run("A burst of notifications does not corrupt the queue", t_queue_does_not_corrupt_when_full)

def t_list_is_newest_first_and_deduped():
    b = Box(); b.cmd("RESET"); b.cmd("CAT 4")
    for uid in (1, 2, 3):
        b.cmd("DS " + ds_payload(uid, "x.y", f"t{uid}", "m")); b.cmd("COMMIT")
    assert b.cmd("LIST") == "L 3 2 1", b.cmd("LIST")
    # iOS re-announces a notification when its badge changes
    b.cmd("DS " + ds_payload(2, "x.y", "t2 again", "m")); out = b.cmd("COMMIT")
    assert b.cmd("LIST") == "L 3 2 1", "a repeat created a duplicate entry"
    assert out.startswith("N 3 "), out
    print("        newest first, and a re-announced notification updates rather than duplicates")
run("The list stays newest first with no duplicates", t_list_is_newest_first_and_deduped)

def t_caps_at_twenty():
    b = Box(); b.cmd("RESET"); b.cmd("CAT 4")
    for uid in range(1, NOTE_MAX + 11):
        b.cmd("DS " + ds_payload(uid, "x.y", "t", "m")); out = b.cmd("COMMIT")
    n = int(out.split()[1])
    assert n == NOTE_MAX, f"kept {n}, should cap at {NOTE_MAX}"
    ids = [int(x) for x in b.cmd("LIST").split()[1:]]
    assert ids[0] == NOTE_MAX + 10, "the newest is not at the front"
    assert len(ids) == NOTE_MAX and ids == sorted(ids, reverse=True)
    print(f"        {NOTE_MAX + 10} arrived, {n} kept, oldest dropped off the end")
run("Twenty is the ceiling and the oldest falls off", t_caps_at_twenty)

def t_app_names_are_readable():
    b = Box(); b.cmd("RESET")
    for bundle, want in [("com.apple.MobileSMS", "Messages"),
                         ("com.apple.mobilemail", "Mail"),
                         ("com.apple.MobilePhone", "Phone"),
                         ("net.whatsapp.WhatsApp", "WhatsApp"),
                         ("com.burbn.instagram", "instagram"),
                         ("", "Phone")]:
        got = b.cmd("APP " + bundle)[2:]
        assert got == want, f"{bundle!r} shown as {got!r}, wanted {want!r}"
    print("        bundle identifiers become something worth putting on a screen")
run("The app is shown as a name, not a bundle id", t_app_names_are_readable)

def t_calls_are_never_declined_by_accident():
    nav = src[src.index("if (screen == S_MSG && depth > 0) {"):]
    nav = nav[:nav.index("\n  if (g == TG_LONG && screen == S_MSG")]
    assert "if (!noteIsCall(n)) ancsAction(n.uid, 1);" in nav, \
        "a hold on a call would send the negative action, which declines it"
    print("        a hold clears a call from the robot only, never from the phone")
run("A knock cannot decline a call", t_calls_are_never_declined_by_accident)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

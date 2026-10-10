"""A short read, from the Mac to the robot and back out again.

The story is written on the Mac and arrives over Bluetooth in pieces,
with its length at the end. If the two ends disagree about that length
by even one character the robot refuses the whole thing, so this runs
both halves against each other over text chosen to break them.

The traps, all of them real:

  - every command is cleaned by ascii() on the way out, and that cleaning
    TRIMS the ends, so a piece that happened to finish on a space used to
    arrive a character short. The pieces are fenced with bars now.
  - ascii() can make a string LONGER: an ellipsis becomes three dots. Cap
    before that and the robot silently drops the tail.
  - a real newline is flattened to a space, which would lose the blank
    line that is the only thing making the first line a title. 0x1E
    stands in for it.
"""
import re, sys, os, unicodedata

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
ino = open(os.path.join(ROOT, "nexus_face", "nexus_face.ino")).read()

# ---- the Mac's ascii(), mirrored ----
def ascii_(s):
    out = []
    for ch in s:
        u = ord(ch)
        if ch in "‘’": out.append("'")
        elif ch in "“”": out.append('"')
        elif ch in "–—": out.append("-")
        elif ch == "…": out.append("...")
        elif ch in "\n\r\t": out.append(" ")
        elif u == 0x1F or u == 0x1E: out.append(ch)
        elif 0x20 <= u < 0x7F: out.append(ch)
        else:
            d = unicodedata.normalize("NFD", ch)
            if d and 0x20 <= ord(d[0]) < 0x7F: out.append(d[0])
    return "".join(out).strip(" \t\n\r")

CHUNK = 360
CAP = 6000

def mac_send(text):
    """Returns the commands the Mac puts on the wire, and the length it
    promises at the end."""
    safe = ascii_(text.replace("\n", "\x1e"))[:CAP]
    chunks = [safe[i:i + CHUNK] for i in range(0, len(safe), CHUNK)]
    cmds = ["!read begin"]
    for c in chunks:
        cmds.append("!read+ |" + c + "|")
    cmds.append("!read end %d" % len(safe))
    # everything goes through ascii() once more as it is sent
    return [ascii_(c) for c in cmds], len(safe)

def robot(cmds):
    """The firmware's side: accumulate, then check the length."""
    buf = ""
    on = False
    for c in cmds:
        rest = c[len("!read "):] if c.startswith("!read ") else c[len("!read"):]
        if c == "!read begin":
            buf = ""; on = True
        elif c.startswith("!read+ "):
            if not on: continue
            q = c[len("!read+ "):]
            if len(q) < 2 or q[0] != "|" or q[-1] != "|": continue
            for ch in q[1:-1]:
                if len(buf) >= CAP: break
                buf += "\n" if ch == "\x1e" else ch
        elif c.startswith("!read end "):
            if not on: continue
            on = False
            want = int(c[len("!read end "):])
            return (len(buf) == want), buf, want, len(buf)
    return False, buf, -1, len(buf)

fails = []
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)
print()

STORY = ("A Window in Ramadan\n\nAyaan had never noticed the window before. "
         + "It was small, and the light came through it in the afternoon. " * 30
         + "\n\nHe closed the book and smiled.")

def t_round_trip():
    cmds, n = mac_send(STORY)
    ok, got, want, had = robot(cmds)
    assert ok, f"refused: promised {want}, received {had}"
    assert got.split("\n")[0] == "A Window in Ramadan", repr(got[:40])
    assert got.split("\n")[1] == "", "the blank line after the title is gone"
    print(f"        {len(cmds)-2} pieces, {n} characters, title and blank line intact")
run("A story arrives whole, with its title", t_round_trip)

def t_space_on_a_boundary():
    """The one that would have refused every story. Built so a piece ends
    exactly on a space."""
    bad = 0
    for pad in range(0, 40):
        text = "T" * pad + " " + ("word " * 2000)
        cmds, n = mac_send(text)
        ok, got, want, had = robot(cmds)
        if not ok: bad += 1
    assert bad == 0, f"{bad} of 40 offsets were refused"
    print("        40 different offsets, every one landing differently, none refused")
run("A piece that ends on a space still counts", t_space_on_a_boundary)

def t_unfenced_would_fail():
    """Prove the fence is what is doing the work."""
    text = "x" * 359 + " " + "y" * 400
    safe = ascii_(text.replace("\n", "\x1e"))[:CAP]
    chunks = [safe[i:i + CHUNK] for i in range(0, len(safe), CHUNK)]
    naive = [ascii_("!read+ " + c) for c in chunks]      # no bars
    got = ""
    for c in naive: got += c[len("!read+ "):]
    assert len(got) < len(safe), "the model no longer reproduces the fault"
    print(f"        without the bars: {len(safe)} sent, {len(got)} arrived, "
          f"{len(safe)-len(got)} lost to trimming")
run("Without the fence it would lose characters", t_unfenced_would_fail)

def t_ellipsis_grows():
    text = "Title here\n\n" + ("she waited… " * 900)
    cmds, n = mac_send(text)
    ok, got, want, had = robot(cmds)
    assert ok, f"refused: promised {want}, received {had}"
    assert n <= CAP, f"{n} characters promised, over the {CAP} the robot keeps"
    assert "..." in got and "…" not in got
    print(f"        ellipses expanded to three dots, capped at {n}, still accepted")
run("Text that grows when cleaned is still capped correctly", t_ellipsis_grows)

def t_accents_and_emoji():
    text = "Café at Dawn\n\nShe smiled \U0001F642 and said “later” — then left."
    cmds, n = mac_send(text)
    ok, got, want, had = robot(cmds)
    assert ok
    assert got.startswith("Cafe at Dawn"), repr(got[:20])
    assert '"later"' in got and "-" in got
    assert "\U0001F642" not in got
    print("        accents keep their letter, curly quotes straighten, emoji dropped")
run("Anything the panel cannot draw is converted or dropped", t_accents_and_emoji)

def t_firmware_agrees():
    """The constants on both sides have to be the same ones."""
    cap = int(re.search(r"#define STORY_MAX_CHARS (\d+)", ino).group(1))
    assert cap == CAP, f"firmware keeps {cap}, the Mac sends up to {CAP}"
    assert "q[0] != '|' || q[n - 1] != '|'" in ino, "the firmware does not expect the fence"
    assert "(q[i] == 0x1E) ? '\\n' : q[i]" in ino, "the firmware does not decode the newline"
    mac = open(os.path.join(ROOT, "..", "..", "mac", "rafiq-app", "mac", "Sources", "Reads.swift")).read() \
          if os.path.exists(os.path.join(ROOT, "..", "..", "mac", "rafiq-app", "mac", "Sources", "Reads.swift")) else ""
    if mac:
        assert 'by: 360' in mac and 'prefix(6000)' in mac
    print(f"        both ends cap at {cap}, fence the pieces and decode 0x1E")
run("The two ends agree about the rules", t_firmware_agrees)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

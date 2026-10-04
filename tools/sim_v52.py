"""v5.2.0: stable reminder ids, a save format that can say which one it
is, the single-reminder endpoint the app edits through, and the two
long-hold overlays. Everything here is asserted against the real source
so it cannot quietly go stale."""
import re, sys
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
fails = []
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)

VER = re.search(r'#define FW_VERSION "([^"]+)"', src).group(1)
print(f"\nfirmware {VER}\n")

# ---------------------------------------------------------------- ids
def t_fmt_marker():
    assert '#define REM_FMT "2\\x1e"' in src, "the save format does not say which it is"
    save = re.search(r"static void saveRems\(\) \{.*?\n\}", src, re.S).group(0)
    assert "String s = REM_FMT;" in save, "the marker is not written"
    assert "s += String(rems[i].id);" in save, "the id is not saved"
    assert 'prefs.putUInt("remid", remNextId);' in save, "the counter is not saved"
    print("        marker written, id first, counter kept")
run("Saved rows say which format they are", t_fmt_marker)

SEP, ROW = "\x1f", "\x1e"
def load(blob, next_id=1):
    """The C loader, in Python. Same rules, same order, so a change to
    one that is not made to the other shows up as a disagreement."""
    out, i = [], 0
    v2 = blob.startswith("2" + ROW)
    if v2: i = 2
    while i < len(blob) and len(out) < 12:
        e = blob.find(ROW, i)
        if e < 0: e = len(blob)
        row = blob[i:e]
        want, at, f, ok = (5 if v2 else 4), -1, [], True
        for _ in range(want):
            at = row.find(SEP, at + 1)
            if at < 0: ok = False; break
            f.append(at)
        if ok and f[0] > 0:
            k = 0
            if v2:
                rid = int(row[:f[0]]); k = 1
                p0 = f[0] + 1
            else:
                rid = next_id; next_id += 1; p0 = 0
            r = dict(id=rid, at=int(row[p0:f[k]]),
                     first=int(row[f[k]+1:f[k+1]]), tries=int(row[f[k+1]+1:f[k+2]]),
                     done=row[f[k+2]+1:f[k+3]] == "1", text=row[f[k+3]+1:])
            if r["id"] >= next_id: next_id = r["id"] + 1
            out.append(r)
        i = e + 1
    return out, next_id

def save(rems):
    return "2" + ROW + ROW.join(
        SEP.join([str(r["id"]), str(r["at"]), str(r["first"]), str(r["tries"]),
                  "1" if r["done"] else "0", r["text"]]) for r in rems)

def t_v1_loads():
    old = ROW.join(SEP.join([str(1700000000 + i*600), str(1700000000 + i*600),
                             "0", "0", t])
                   for i, t in enumerate(["Call Amma", "Service the bike", "Pay the bill"]))
    got, nxt = load(old, next_id=1)
    assert len(got) == 3, f"{len(got)} of 3 old rows survived the upgrade"
    assert [r["text"] for r in got] == ["Call Amma", "Service the bike", "Pay the bill"]
    ids = [r["id"] for r in got]
    assert ids == [1, 2, 3], f"old rows got ids {ids}"
    assert nxt == 4, f"next id is {nxt}"
    print(f"        3 version 1 rows kept their words and were given ids {ids}")
run("Reminders saved by the last firmware still load", t_v1_loads)

def t_v2_roundtrip():
    rems = [dict(id=7, at=1700000000, first=1700000000, tries=2, done=False,
                 text="Pick up the parcel, it closes at six"),
            dict(id=9, at=1700003600, first=1700003600, tries=0, done=True,
                 text="Call Amma")]
    got, nxt = load(save(rems))
    assert got == rems, f"what came back is not what went in:\n{got}\n{rems}"
    assert nxt == 10, f"next id is {nxt}, should clear the highest"
    print("        two rows out and back unchanged, counter past the highest")
run("Saving and loading a reminder returns the same reminder", t_v2_roundtrip)

def t_ids_unique():
    """Never reused, so an app holding an id can never be pointed at
    something that was created after the one it meant."""
    assert "rems[remCount].id = remNextId++;" in src, "ids are not handed out on add"
    assert "if (r.id >= remNextId) remNextId = r.id + 1;" in src, \
        "loading does not push the counter past what it read"
    add = re.search(r"static bool addRem\(.*?\n\}", src, re.S).group(0)
    assert "remNextId++" in add and add.count("remNextId++") == 1, "the counter moves oddly"
    print("        handed out once, counter never walks backwards")
run("An id is never reused", t_ids_unique)

def t_text_safe():
    """The separators are the file format. A reminder containing one
    would split into two rows that no longer parse."""
    add = re.search(r"static bool addRem\(.*?\n\}", src, re.S).group(0)
    assert "'\\x1e'" in add and "'\\x1f'" in add, "nothing strips the separators on add"
    ep = src[src.index('web.on("/api/rem", HTTP_ANY'):]
    ep = ep[:ep.index('web.on("/api/rems"')]
    assert "'\\x1e'" in ep and "'\\x1f'" in ep, "nothing strips the separators on edit"
    # and what goes out in JSON is escaped
    assert "static String jstr(const char* t)" in src, "no JSON escaper"
    assert "jstr(rems[i].text)" in ep, "the text is sent unescaped"
    print("        separators stripped on the way in, quotes escaped on the way out")
run("A reminder cannot break the file it is saved in", t_text_safe)

# ----------------------------------------------------------- endpoint
def t_endpoint():
    ep = src[src.index('web.on("/api/rem", HTTP_ANY'):]
    ep = ep[:ep.index('web.on("/api/rems"')]
    assert "if (!guard()) return;" in ep, "it is not behind the pairing"
    assert '"\\"rems\\":["' in ep.replace('\\\\', '\\') or '\\"rems\\":[' in ep, \
        "it does not return the list"
    for field in ("id", "at", "first", "tries", "done", "text"):
        assert f'\\"{field}\\":' in ep, f"the list leaves out {field}"
    assert '\\"clock\\":" + String(timeOk' in ep, \
        "the app is not told whether the times mean anything"
    assert 'if (web.arg("drop") == "1")' in ep, "nothing can be deleted"
    assert "for (int k = i; k < remCount - 1; k++) rems[k] = rems[k + 1];" in ep, \
        "deleting leaves a hole in the list"
    assert 'if (web.hasArg("at")) {' in ep and "rems[i].tries = 0;" in ep, \
        "changing the time does not reset the nagging"
    assert "404" in ep, "an unknown id is answered as if it worked"
    print("        list, drop, retime, retext, and a 404 when there is no such one")
run("One endpoint the app can read, edit and delete through", t_endpoint)

def t_showing_survives():
    """remShowing is an index. Sorting moves rows under it, so without
    this, answering a reminder card marked a different one done."""
    srt = re.search(r"static void sortRems\(\) \{.*?\n\}", src, re.S).group(0)
    assert "uint32_t showId" in srt and "remShowing = remById(showId)" in srt, \
        "a sort can leave the card pointing at the wrong reminder"
    ep = src[src.index('web.on("/api/rem", HTTP_ANY'):]
    ep = ep[:ep.index('web.on("/api/rems"')]
    assert "else if (remShowing > i) remShowing--;" in ep, \
        "deleting above the showing one shifts it by one"
    assert "if (remShowing == i) { remShowing = -1; toastUntil = 0; }" in ep, \
        "deleting the one on screen leaves the card pointing at nothing"
    print("        survives a sort and a delete from either side")
run("The reminder on screen stays the reminder on screen", t_showing_survives)

# ------------------------------------------------------------ overlay
def t_hold():
    """One overlay, and a dial rather than a notice. Two solid white
    bands on a panel this small is most of the panel."""
    f = re.search(r"static void drawHoldTier\(uint32_t now\) \{.*?\n\}\n", src, re.S).group(0)
    assert "KEEP HOLDING" not in src, "the old flashing word is still there"
    assert "houseGlyph" not in src, "the home overlay is still in there, unused"
    assert "LET GO FOR HOME" not in src, "there are still two overlays"
    assert "oled.clearDisplay();" in f, \
        "the overlay leaves the screen underneath showing round the edges"
    assert "oled.fillRect(0, 0, SCRW, 11, SSD1306_WHITE)" not in f, "the heavy bands are back"
    assert '"GOING TO SLEEP"' in f, "it does not say what is happening"
    code = re.sub(r"//[^\n]*", "", f)       # the comment says why it went
    assert '"let go to stay"' not in code, \
        "it still offers something letting go cannot do any more"
    assert '"hold %s to wake me"' in f and "WAKE_NAME[cfgWakeIdx]" in f, \
        "it does not say how to get it back, or does not match the wake setting"
    assert "ringArc(CX, CY, R, left);" in f, "the ring does not show what is left"
    assert "for (int d = 0; d < 360; d += 9)" in f, "there is no track behind the ring"
    assert re.search(r"ctr\(n, \d+, 3\);", f), "the number is not drawn large, in the middle"
    assert "uint32_t gone = now - sleepArmed;" in f, "it is not counting from when it armed"

    # Geometry, measured from the source rather than restated, and the
    # notches worked out where they actually land instead of where the
    # extremes of the ring would be. Only one of the three is at the
    # top of the circle; the other two are at thirty degrees either
    # side of the bottom and come nowhere near the edge.
    import math as _m
    CY = int(re.search(r"const int CX = SCRW / 2, CY = (\d+), R = \d+;", f).group(1))
    R  = int(re.search(r"const int CX = SCRW / 2, CY = \d+, R = (\d+);", f).group(1))
    ny = int(re.search(r"ctr\(n, (\d+), 3\);", f).group(1))
    ty = int(re.search(r'ctr\("GOING TO SLEEP", (\d+), 1\);', f).group(1))
    by = int(re.search(r'ctr\(back, (\d+), 1\);', f).group(1))
    out = int(re.search(r"for \(int rr = R \+ 1; rr <= R \+ (\d+); rr\+\+\)", f).group(1))
    marks = [CY + (R + out) * _m.sin(-1.5708 + i * 2.0944) for i in range(3)]
    dial_top, dial_bot = min(min(marks), CY - R), max(max(marks), CY + R)
    assert dial_top > ty + 8, f"the dial reaches the top line: {dial_top:.0f} against {ty+8}"
    assert dial_bot < by, f"the dial reaches the bottom line: {dial_bot:.0f} against {by}"
    assert by + 8 <= 64, "the bottom line runs off the screen"
    # the digit has to sit inside the ring, corners and all
    half = ((18 / 2) ** 2 + (24 / 2) ** 2) ** 0.5
    assert half < R - 3, f"the digit's corners ({half:.1f}) poke through the ring ({R-3})"
    assert abs((ny + 12) - CY) <= 1, "the number is not centred in the ring"
    print(f"        text {ty}..{ty+8} and {by}..{by+8}, "
          f"dial {dial_top:.0f}..{dial_bot:.0f}, digit centred")
run("One overlay, a dial, and everything clear of everything else", t_hold)

def t_count_three():
    H = int(re.search(r"#define TOUCH_HOME_MS\s+(\d+)", src).group(1))
    C = int(re.search(r"#define TOUCH_COUNT_MS\s+(\d+)", src).group(1))
    L = int(re.search(r"#define TOUCH_LONG_MS (\d+)", src).group(1))
    assert (H, C) == (4000, 3000), f"four then three, not {H} then {C}"
    assert H - L >= 3000, "too little between opening something and going home"
    assert "TOUCH_SLEEP_MS" not in src, "the second tier is still defined"

    def walk(hold_ms):
        """Returns (went home, switched off)."""
        armed, home, slept = None, False, False
        for ms in range(0, 20000, 50):
            on = ms < hold_ms
            if on and armed is None and ms >= H:
                armed = ms; home = True
            if armed is not None and ms - armed >= C:
                slept = True; break                    # the pad is out of it
        return home, slept

    assert walk(2000)  == (False, False), "two seconds did something"
    assert walk(4200)  == (True, True),   "four seconds did not go home and switch off"
    # the one the pad used to beat: it lets go a second into the count
    assert walk(5000)  == (True, True),   "the pad letting go mid count stopped it"
    assert walk(7100)  == (True, True),   "holding right through did not switch it off"
    assert H + C == 7000
    print(f"        {H//1000}s decides it, {C//1000}s of saying so, off at {(H+C)//1000}s, "
          f"whatever the pad does")
run("Four seconds decides it and nothing after can take it back", t_count_three)

def t_measure():
    """So the ceiling can be found rather than argued about."""
    assert "uint32_t touchLongest = 0;" in src, "nothing records the longest touch"
    assert "if (held > touchLongest) touchLongest = held;" in src, \
        "it is only updated on release, so a pad that lets go is never seen"
    sysblk = src[src.index('const char* LB[5] = { "Uptime"'):]
    sysblk = sysblk[:sysblk.index("vals[4]")]
    assert "touchLongest" in sysblk, "it is measured but never shown"
    print("        longest unbroken touch measured live and shown on SYSTEM")
run("The board can say what its own pad ceiling is", t_measure)

# ------------------------------------------------------------ the app
print("\nthe panel\n")
def t_panel_fixed():
    """The white bands the panel used to show were the window growing to
    fit a page and not handing the height back. The runtime check cannot
    measure the settings page, so the invariant is asserted here: the
    size is declared once, in one place, and nothing can negotiate it."""
    app = open("rafiq-app/mac/Sources/App.swift").read()
    assert "static let width: CGFloat  = 330" in app and \
           "static let height: CGFloat = 440" in app, "the panel size moved"
    assert ".frame(width: Panel.width, height: Panel.height, alignment: .topLeading)" in app, \
        "the panel no longer declares a hard frame, so the window can grow again"
    print("        330 x 440, declared once, not negotiated")
run("The panel cannot change size, whatever is on the page", t_panel_fixed)

def t_no_self_layout():
    """A view laid out from a measurement of itself never settles. The
    gutter was ten points when the content overflowed, which changed
    the width the text wrapped in, which changed the height, which
    changed whether it overflowed."""
    app = open("rafiq-app/mac/Sources/App.swift").read()
    body = app[app.index("private struct Scrolled<V: View>: View {"):]
    body = body[:body.index("\n    }\n")]
    assert ".padding(.trailing, 10)" in body, "the gutter is gone"
    assert "overflows ? 10" not in body, "the gutter is measured from the content again"
    # whatever is still allowed to read the measurement must not move anything
    for ok in (".scrollIndicators(", ".scrollDisabled("):
        assert ok in body, f"{ok} was dropped along with the circularity"
    print("        constant gutter; the measurement only decides scrolling")
run("Nothing on the page is laid out from a measurement of the page", t_no_self_layout)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

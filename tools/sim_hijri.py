"""The other calendar, and the glyphs that draw it."""
import re, sys, datetime as dt
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
gl  = open("nexus-repo/nexus_face/arabic_glyphs.h").read()
fails=[]
def must(c,w):
    if not c: fails.append(w)
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)

# the firmware's own arithmetic, lifted
def g2jdn(y,m,d):
    a=(14-m)//12; yy=y+4800-a; mm=m+12*a-3
    return d+(153*mm+2)//5+365*yy+yy//4-yy//100+yy//400-32045
def jdn2h(jd):
    l=jd-1948440+10632; n=(l-1)//10631; l=l-10631*n+354
    j=((10985-l)//5316)*((50*l)//17719)+(l//5670)*((43*l)//15238)
    l=l-((30-j)//15)*((17719*j)//50)-(j//16)*((15238*j)//43)+29
    m=(24*l)//709; d=l-(709*m)//24; return 30*n+j-30, m, d
def H(y,m,d,adj=0): return jdn2h(g2jdn(y,m,d)+adj)

print("\nthe calendar\n")

def t_anchors():
    for g, want in [((2021,8,10),(1443,1,1)), ((2024,3,11),(1445,9,1)),
                    ((2025,3,1),(1446,9,1)), ((2026,2,18),(1447,9,1))]:
        got = H(*g)
        assert got == want, f"{g} gave {got}, expected {want}"
    print("        four known first-of-months, all exact")
run("It lands on the first of Ramadan and Muharram where it should", t_anchors)

def t_continuous():
    prev=None; bad=0
    d=dt.date(2026,1,1)
    while d < dt.date(2031,1,1):
        cur=H(d.year,d.month,d.day)
        if prev and not (cur[2]==prev[2]+1 or (cur[2]==1 and prev[2] in (29,30))): bad+=1
        prev=cur; d+=dt.timedelta(days=1)
    assert bad==0, f"{bad} days where the date jumped"
    print("        five years, not one jump")
run("Every day follows the one before it", t_continuous)

def t_bounds():
    for y in range(2024, 2041):
        for m in range(1,13):
            hy,hm,hd = H(y,m,15)
            assert 1<=hm<=12, f"{y}-{m} gave month {hm}"
            assert 1<=hd<=30, f"{y}-{m} gave day {hd}"
            assert 1440<hy<1480, f"{y}-{m} gave year {hy}"
    print("        seventeen years, every month in range")
run("The month never falls outside one to twelve", t_bounds)

def t_adj():
    base = H(2026,10,2)
    assert H(2026,10,2,1)[2] == base[2]+1, "plus one did not move it a day"
    assert H(2026,10,2,-1)[2] == base[2]-1, "minus one did not move it back"
    assert "cfgHijriAdj = constrain(prefs.getInt(\"hadj\", 0), -2, 2);" in src, "not remembered"
    assert "C_HIJRI" in src, "no way to set it on the robot"
    assert 'k == "hadj"' in src, "no way to set it from the app"
run("The shift moves it a day either way, and is remembered", t_adj)

print("\nthe glyphs\n")

def t_glyphs():
    for name, n in (("AR_BIG",10), ("AR_SMALL",10), ("AR_MONTH",12)):
        w = int(re.search(rf"#define {name}_W (\d+)", gl).group(1))
        h = int(re.search(rf"#define {name}_H (\d+)", gl).group(1))
        body = re.search(rf"const uint8_t {name}\[(\d+)\]\[(\d+)\] PROGMEM", gl)
        cnt, sz = int(body.group(1)), int(body.group(2))
        assert cnt == n, f"{name} has {cnt} glyphs, not {n}"
        assert sz == ((w+7)//8)*h, f"{name} is {sz} bytes, not {((w+7)//8)*h} for {w}x{h}"
        assert w <= 128 and h <= 64, f"{name} is {w}x{h}, bigger than the screen"
        # every glyph must have ink, or a month renders as nothing
        rows = re.findall(r"\{([^}]*)\}", re.search(rf"{name}\[\d+\]\[\d+\] PROGMEM = \{{(.*?)\n\}};", gl, re.S).group(1))
        for i, r in enumerate(rows):
            data = [int(x,16) for x in re.findall(r"0x([0-9A-F]{2})", r)]
            assert any(data), f"{name}[{i}] is blank"
        print(f"        {name}: {cnt} glyphs, {w}x{h}, {sz}B each, all have ink")
run("Every glyph is the size it says and none is blank", t_glyphs)

def t_faces():
    names = re.findall(r'"([^"]*)"', re.search(r'FACE_NAME\[FACE_N\]\s*=\s*\{(.*?)\};', src, re.S).group(1))
    for n in ("arabic","hijri","crescent"): assert n in names, f"{n} is not listed"
    assert len(names) == len(set(names)) == 23, f"{len(names)} faces"
    for f in ("F_ARABIC","F_HIJRI","F_CRESCENT"):
        assert f"case {f}:" in src, f"{f} is declared but never drawn"
    # and the app agrees, or it sets the wrong one
    app = open("rafiq-app/mac/Sources/Device.swift").read()
    a = re.findall(r'"([^"]*)"', re.search(r'faceNames\s+= \[(.*?)\]', app, re.S).group(1))
    assert a == names, f"the app lists {len(a)} faces, the robot {len(names)}"
    print(f"        {len(names)} faces, and the app's list matches exactly")
run("The three are listed, drawn, and known to the app", t_faces)

def t_slide():
    s = re.search(r"static void slideBand\(.*?\n\}", src, re.S).group(0)
    assert "PERIOD = 2000" in s, "not two seconds each"
    assert "fillRect(0, y - H, SCRW, H, SSD1306_BLACK)" in s, "what slides off the top is left there"
    assert "fillRect(0, y + 8, SCRW, H, SSD1306_BLACK)" in s, "what slides off the bottom is left there"
    print("        two seconds each, and both overflows wiped")
run("The date band swaps every two seconds and cleans up after itself", t_slide)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -",f) for f in fails]; sys.exit(1)
print("PASS")

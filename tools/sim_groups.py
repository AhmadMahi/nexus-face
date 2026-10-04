"""The settings, in groups, walked rather than read.

Twenty four rows in one list meant hunting. Four groups means two
steps to anything. The risk in a change like this is not the drawing,
it is a row that ends up in no group, or in two, and so becomes
unreachable or appears twice. Both are checked by walking every path
a finger can take.
"""
import re, sys
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
fails = []
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)

NAMES = re.findall(r'"([^"]*)"',
         re.search(r"const char\* C_NAME\[C_COUNT\] =\s*\{(.*?)\};", src, re.S).group(1))
ORDER = [x.strip().split(" ")[0] for x in
         re.split(r",", re.search(r"enum \{ (C_BRIGHT = 0,.*?)C_COUNT \};", src, re.S).group(1))
         if x.strip().startswith("C_")]
GNAMES = re.findall(r'"([^"]*)"',
          re.search(r"SG_NAME\[SG_COUNT\] = \{(.*?)\};", src).group(1))
BODY = re.search(r"SG_ROWS\[SG_COUNT\]\[SG_MAX\] = \{(.*?)\n\};", src, re.S).group(1)
GROUPS = [[t for t in re.findall(r"C_\w+|SG_END", row) if t != "SG_END"]
          for row in BODY.strip().split("\n")]
print(f"\n{len(ORDER)} settings in {len(GROUPS)} groups\n")

def t_every_row_in_exactly_one():
    """A row in no group cannot be reached at all. A row in two shows
    up twice and the second one is a lie about where it lives."""
    seen = {}
    for gi, g in enumerate(GROUPS):
        for r in g:
            seen.setdefault(r, []).append(GNAMES[gi])
    missing = [NAMES[ORDER.index(r)] for r in ORDER if r not in seen]
    assert not missing, f"unreachable: {', '.join(missing)}"
    twice = {r: v for r, v in seen.items() if len(v) > 1}
    assert not twice, f"in two groups: {twice}"
    unknown = [r for r in seen if r not in ORDER]
    assert not unknown, f"grouped but not a setting: {unknown}"
    assert len(GNAMES) == len(GROUPS), f"{len(GNAMES)} names, {len(GROUPS)} groups"
    print("        " + ", ".join(f"{n} {len(g)}" for n, g in zip(GNAMES, GROUPS)))
run("Every setting is in exactly one group", t_every_row_in_exactly_one)

def t_groups_fit_one_screen():
    assert len(GROUPS) <= 4, f"{len(GROUPS)} groups will not fit without scrolling"
    for n, g in zip(GNAMES, GROUPS):
        w = 3 + len(n) * 6 + 4 + 6 + 3          # name, gap, one digit, margin
        assert w <= 128, f"{n!r} row needs {w}px"
        assert len(g) <= 8, f"{n} holds {len(g)}, the table only has room for 8"
    print(f"        {len(GROUPS)} groups, widest row {max(3+len(n)*6+13 for n in GNAMES)}px of 128")
run("The groups fit on one screen and the table holds every row", t_groups_fit_one_screen)

# ------------------------------------------------------- walk it
class Bot:
    """depth, which list is on screen, and where you are in it.
    Mirrors knockOne, knockTwo and knockThree for S_SETTINGS."""
    def __init__(self):
        self.depth = 0; self.grp = -1; self.sel = 0; self.item = ORDER[0]
    def open(self):                               # hold on the gear
        self.depth = 1; self.grp = -1; self.sel = 0; self.item = ORDER[0]
    def one(self):
        if self.depth != 1: return
        if self.grp < 0: self.sel = (self.sel + 1) % len(GROUPS)
        else:
            g = GROUPS[self.grp]
            self.item = g[(g.index(self.item) + 1) % len(g)]
    def hold(self):
        if self.depth != 1: return
        if self.grp < 0:
            self.grp = self.sel; self.item = GROUPS[self.grp][0]
        else:
            self.depth = 2                        # whatever the row does
    def back(self):
        if self.depth == 1 and self.grp >= 0: self.grp = -1; return
        if self.depth > 0: self.depth -= 1
    def showing(self):
        if self.depth == 0: return "the gear"
        if self.depth == 1 and self.grp < 0: return f"groups: {GNAMES[self.sel]}"
        if self.depth == 1: return f"{GNAMES[self.grp]}: {NAMES[ORDER.index(self.item)]}"
        return f"inside {NAMES[ORDER.index(self.item)]}"

def t_everything_reachable():
    """Every one of the twenty four, from a cold open, by pressing."""
    found = {}
    for gi in range(len(GROUPS)):
        b = Bot(); b.open()
        for _ in range(gi): b.one()
        assert b.showing() == f"groups: {GNAMES[gi]}", b.showing()
        b.hold()
        for _ in range(len(GROUPS[gi])):
            found[b.item] = b.showing()
            b.one()
        # and walking round comes back to where it started
        assert b.item == GROUPS[gi][0], f"{GNAMES[gi]} does not wrap"
    assert len(found) == len(ORDER), \
        f"reached {len(found)} of {len(ORDER)}: missing " \
        f"{[NAMES[ORDER.index(r)] for r in ORDER if r not in found]}"
    worst = max(GROUPS, key=len)
    print(f"        all {len(ORDER)} reached; the longest walk is "
          f"{len(GROUPS) - 1} + {len(worst) - 1} presses")
run("Every setting can be reached by pressing, from a cold open", t_everything_reachable)

def t_back_goes_to_the_groups():
    # from every group, not just whichever happens to be second
    for gi, name in enumerate(GNAMES):
        b = Bot(); b.open()
        for _ in range(gi): b.one()
        b.hold()
        assert b.showing().startswith(name + ":"), b.showing()
        b.one()                                   # and from part way down it
        b.back()
        assert b.showing() == f"groups: {name}", \
            f"back out of {name} left the settings instead: {b.showing()}"
        b.back()
        assert b.showing() == "the gear", b.showing()
    print(f"        out of all {len(GNAMES)} groups, then out of the settings")
run("Going back leaves the group before it leaves the settings", t_back_goes_to_the_groups)

def t_shorter_than_before():
    """The point of the change: how far you press to reach a thing."""
    before = {r: ORDER.index(r) for r in ORDER}
    after = {}
    for gi, g in enumerate(GROUPS):
        for ri, r in enumerate(g): after[r] = gi + ri
    worse = {NAMES[ORDER.index(r)]: (before[r], after[r])
             for r in ORDER if after[r] > before[r]}
    assert not worse, f"further away than before: {worse}"
    print(f"        worst case {max(before.values())} presses before, "
          f"{max(after.values())} now; average "
          f"{sum(before.values())/len(before):.1f} to {sum(after.values())/len(after):.1f}")
run("Nothing is further away than it was", t_shorter_than_before)

def t_source_wiring():
    assert "if (setGrp < 0) grpSel = (grpSel + 1) % SG_COUNT;" in src, "a press does not walk the groups"
    assert "else            itemIdx = sgNext(setGrp, itemIdx);" in src, "a press does not walk the rows"
    assert "setGrp = grpSel;\n      itemIdx = SG_ROWS[setGrp][0];" in src, "holding does not open a group"
    assert "if (screen == S_SETTINGS && depth == 1 && setGrp >= 0) { setGrp = -1; return; }" in src, \
        "back does not return to the groups"
    assert "case S_SETTINGS: depth = 1; setGrp = -1; grpSel = 0; itemIdx = 0; break;" in src, \
        "opening the settings does not start at the groups"
    # itemIdx is still a C_ index everywhere, so no case had to move
    assert "switch (itemIdx) {" in src and "case C_BRIGHT:" in src, \
        "the settings switches were rewritten, which they did not need to be"
    print("        itemIdx is still a C_ index, so none of the cases moved")
run("The wiring is there and nothing underneath it changed", t_source_wiring)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

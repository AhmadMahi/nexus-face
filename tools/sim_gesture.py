"""Gesture mode: the pad pointed at the Mac.

The parts worth checking are not the mapping table, they are the ways
it can leave you stranded: a robot stuck in a mode with nothing on the
other end, and a UDP port that presses keys on your machine.
"""
import re, sys
src = open("nexus-repo/nexus_face/nexus_face.ino").read()
ges = open("rafiq-app/mac/Sources/Gestures.swift").read()
dev = open("rafiq-app/mac/Sources/Device.swift").read()
svc = open("rafiq-app/mac/Sources/Services.swift").read()
fails = []
def run(title, fn):
    try: fn(); print(f"  ok   {title}")
    except AssertionError as e:
        print(f"  FAIL {title}: {e}"); fails.append(title)
print()

# -------------------------------------------------- it cannot strand you
def t_never_stuck():
    """A robot in gesture mode with no Mac is a brick: the pad drives
    something that is not listening. Every way the Mac can go away has
    to take the mode with it."""
    assert "bool      cfgGesture = false;" in src, "there is no mode"
    assert "prefs.putBool(\"gest\"" not in src and "prefs.getBool(\"gest\"" not in src, \
        "it is remembered across a reboot, so a robot can come up stranded"
    # every place the robot decides the Mac has gone
    gone = [i for i, l in enumerate(src.split("\n")) if "macLinked = false" in l
            and not l.strip().startswith("bool")]
    assert len(gone) >= 3, f"only {len(gone)} places notice the Mac leaving"
    lines = src.split("\n")
    for i in gone:
        near = "\n".join(lines[i:i + 3])
        assert "cfgGesture = false" in near, \
            f"line {i+1} drops the Mac but leaves the robot in gesture mode"
    assert 'flash("GESTURE OFF", 1100);' in src, "holding the pad cannot leave it"
    print(f"        {len(gone)} ways the Mac can go, all of them end the mode")
run("A robot can never be left in gesture mode with nothing listening", t_never_stuck)

def t_hold_leaves_rather_than_sleeps():
    # touchSeenFree: nothing that can switch the robot off fires
    # until the pad has been seen settled at rest. See sim_pad.
    assert "if (cfgGesture && touchSeenFree && held >= TOUCH_HOME_MS) {" in src, \
        "a long hold in gesture mode does not leave it"
    assert "if (!cfgGesture && touchSeenFree && !sleepArmed && held >= TOUCH_HOME_MS) {" in src, \
        "a long hold in gesture mode would also switch the robot off"
    print("        four seconds leaves the mode instead of switching the robot off")
run("The long hold is the way back, not the way off", t_hold_leaves_rather_than_sleeps)

def t_source():
    """Knock the desk, touch the pad, or either. Either is the one
    that needs care: pressing a pad glued to a small light robot
    knocks the robot, and the knock lands first, on the press, while
    the press is not resolved until you lift. Unguarded, one press
    reaches the Mac twice."""
    names = re.findall(r"GSRC_(\w+)", src)
    assert "KNOCK" in names and "TOUCH" in names and "BOTH" in names, names
    assert "static bool gestByKnock() { return cfgGestSrc != GSRC_TOUCH; }" in src
    assert "static bool gestByTouch() { return cfgGestSrc != GSRC_KNOCK; }" in src
    # the knock path throws away a knock your own finger made
    sb = src[src.index("if (cfgGesture) {\n    uint32_t now = millis();"):]
    sb = sb[:sb.index("\n  }")]
    assert "bool byPad = touchOn || (now - touchLiftAt) < SHAKE_AFTER_MS" in sb, \
        "with both on, pressing the pad would send twice"
    assert "if (gestByKnock() && !byPad) {" in sb, "the knock path ignores the setting"
    # the touch path only sends when it is allowed to
    t = src[src.index("  if (cfgGesture) {\n    if (gestByTouch()) {"):]
    t = t[:t.index("\n  }")]
    assert 'sendTap("1")' in t and 'sendTap("2")' in t, "the pad never sends"
    assert "if (gestByTouch())" in t, "the pad sends whatever the setting says"
    # and the robot says which it is listening for
    assert 'cfgGestSrc == GSRC_KNOCK ? "knock the desk"' in src, \
        "the screen does not say what it is listening for"
    print("        knock, touch or either, and either cannot double send")
run("Knock, touch or either, with the double send guarded", t_source)

def t_source_both_ends():
    ges = open("rafiq-app/mac/Sources/Gestures.swift").read()
    d = open("rafiq-app/mac/Sources/Device.swift").read()
    names = re.findall(r'"([^"]*)"',
             re.search(r"sourceNames = \[(.*?)\]", ges).group(1))
    n = len(re.findall(r"GSRC_\w+ = 0|GSRC_TOUCH|GSRC_BOTH", src))
    assert len(names) == 3, f"the app offers {len(names)} choices, the robot has 3"
    assert '"k": "gsrc"' in d, "the app cannot tell the robot which"
    assert "if on { await Device.shared.setGestureSource(source) }" in ges, \
        "switching gesture mode on does not carry the setting with it"
    print(f"        {names}, and the robot is told")
run("Both ends agree on what it listens for", t_source_both_ends)

# ------------------------------------------------------- the packet
def t_packet_is_guarded():
    """It presses keys on a Mac. Anyone on the network can send one."""
    f = src[src.index("static void sendTap(const char* what)"):]
    f = f[:f.index("\n}")]
    assert "tapUdp.print(cfgTok);" in f, "the packet carries no token"
    assert "macAddr == IPAddress()" in f, "it would send into the void"
    assert "macAddr = web.client().remoteIP();" in src, "it never learns where the Mac is"
    # and the Mac checks both halves
    h = ges[ges.index("private func heard("):]
    h = h[:h.index("\n    }")]
    assert "parts[0] == token" in h, "the app does not check the token"
    assert "from == want" in h, "the app does not check who sent it"
    assert "guard !token.isEmpty" in h, "an unpaired robot would accept anything"
    print("        token and sender both checked, at both ends")
run("A packet that presses your keys is not taken on trust", t_packet_is_guarded)

def t_push_not_poll():
    assert "#define TAP_PORT 4211" in src and "NWEndpoint.Port(rawValue: 4211)" in ges, \
        "the two ends do not agree on a port"
    assert "Gestures.shared.start()" in svc, "the app never listens"
    assert "if g.on && self?.dev.gesture == false { g.push() }" in svc, \
        "a robot that restarted is never told again"
    print("        port 4211, pushed on press, and the mode put back after a restart")
run("Presses are pushed, not waited for", t_push_not_poll)

# ------------------------------------------------------------ actions
def t_actions_and_permission():
    """Three of the four need nothing. Only keys needs Accessibility,
    and the app has to know the difference rather than failing quietly."""
    for a in ("case nothing", "case openApp(String)", "case shortcut(String)", "case keys(String)"):
        assert a in ges, f"{a} is missing"
    assert "var needsTrust: Bool { if case .keys = self { return true }; return false }" in ges, \
        "nothing knows which actions need the permission"
    assert "guard Keys.trusted else { say(\"needs Accessibility\"); return }" in ges, \
        "pressing keys fails silently when it has not been allowed to"
    assert "AXIsProcessTrusted()" in ges, "it does not ask whether it has been allowed"
    assert "/usr/bin/shortcuts" in ges, "Shortcuts are not run"
    assert "NSWorkspace.shared.openApplication" in ges, "apps are not opened"
    print("        nothing, open, Shortcut and keys; only the last one asks for anything")
run("The actions that need no permission work without one", t_actions_and_permission)

def t_focus():
    assert "NSWorkspace.shared.frontmostApplication?.bundleIdentifier" in ges, \
        "it cannot tell which app is in front"
    assert "didActivateApplicationNotification" in ges, "it polls for the front app"
    assert "maps.first { $0.bundleId == bundleId }" in ges, "per app mappings do nothing"
    assert "?? maps.first { $0.bundleId == nil }" in ges, "there is no fallback"
    print("        the front app chooses the mapping, with a fallback behind it")
run("What a press does depends on what is in front", t_focus)

def t_mute():
    """The mic can be stopped at the device with no permission. The
    camera cannot be stopped at all, only told."""
    assert "kAudioDevicePropertyMute" in ges, "it does not mute the device"
    assert "kAudioDevicePropertyVolumeScalar" in ges, \
        "no fallback for a device with no mute switch"
    for app in ("us.zoom.xos", "com.microsoft.teams", "com.google.Chrome"):
        assert app in ges, f"{app} is not in the table"
    m = ges[ges.index("func muteAll()"):]
    m = m[:m.index("\n    }")]
    assert "Audio.setInputMuted(true)" in m, "it does not stop the microphone"
    assert "camera needs Accessibility" in m, \
        "it does not say when it could not switch the camera off"
    u = ges[ges.index("func unmute()"):]
    u = u[:u.index("\n    }")]
    assert "app.cam" not in u, "two presses would switch a camera back on for you"
    assert "if micTakesOver && micLive {" in ges, "the mic does not take over the mappings"
    print("        mic stopped at the device, camera only told, and never turned back on")
run("One press kills the mic for certain and the camera where it can", t_mute)

def t_robot_is_told_the_mute():
    assert 'if (web.hasArg("muted")) gestMuted' in src, "the robot is never told"
    assert "muted: mu" in ges or "muted: mu)" in ges, "the app never says"
    d = src[src.index("static void drawGesture()"):]
    d = d[:d.index("\n}")]
    assert "gestMuted ?" in d, "the robot does not show it"
    assert 'ctr("gesture mode", 22, 1);' in d, "the plain screen is gone"
    # the mic screen has to fit
    assert 'at(54, 12, gestMuted ? "MUTED" : "LIVE", 2);' in d, "the word is not beside the icon"
    assert 54 + 5 * 12 <= 128, "MUTED runs off the edge"
    assert 12 + 16 < 34, "the word and the line under it overlap"
    print("        shown on the robot, large, and it fits")
run("The robot says whether you are muted, from across the desk", t_robot_is_told_the_mute)

# ------------------------------------------------ shipping it at all
def t_release_tag():
    """Two releases went out tagged with a bare version number. The
    updater only looks at mac-v, so it could not see either of them
    and reported "up to date" while two versions behind, which is the
    worst way for this to fail because nothing looks wrong."""
    up = open("rafiq-app/mac/Sources/Updater.swift").read()
    rel = open("rafiq-app/mac/release.sh").read()
    build = open("rafiq-app/mac/build.sh").read()
    prefix = re.search(r'tagPrefix = "([^"]*)"', up).group(1)
    assert prefix == "mac-v", f"the app looks for {prefix!r}"
    # the script must read both out of the source, not restate them
    assert 'tagPrefix = \\"\\(.*\\)\\"' in rel or "tagPrefix" in rel, \
        "the release script does not read the prefix the app uses"
    assert 'VER="\\(.*\\)"' in rel or 'VER=' in rel, \
        "the release script does not read the version from build.sh"
    assert 'TAG="$PREFIX$VER"' in rel, "the tag is still assembled by hand"
    assert "gh release create \"$TAG\"" in rel, "it does not use the tag it worked out"
    assert 'FAIL: it should have seen' in rel, \
        "it does not check that the app can see what it just published"
    ver = re.search(r'^VER="([^"]*)"', build, re.M).group(1)
    print(f"        app wants {prefix!r}, script builds {prefix}{ver} from the source")
run("The release tag is derived, not typed", t_release_tag)

def t_signed_with_a_certificate():
    """Ad hoc signing hashes the code, so the signature changes on
    every build and macOS throws the Accessibility grant away with
    it. Gesture mode's keyboard actions depend on that grant, so the
    build has to sign with something that stays the same."""
    build = open("rafiq-app/mac/build.sh").read()
    assert 'IDENT="Rafiq Signing"' in build, "the build does not use a certificate"
    assert 'codesign --force --sign "$IDENT"' in build, "it does not sign with it"
    assert "WARNING: no" in build, \
        "a machine without the certificate would quietly go back to ad hoc"
    assert 'codesign --verify --deep --strict "$APP"' in build, "nothing checks the signature"
    print("        signed with a certificate, and it says so when it cannot be")
run("The app is signed with something that does not change", t_signed_with_a_certificate)

# ------------------------------------------------- knock, not touch
def t_knock_drives_it():
    """The robot sits on a desk. You knock the desk. The pad is only
    the way out."""
    sb = src[src.index("static void settleBurst() {"):]
    sb = sb[:sb.index("if (upState != U_OFF)")]
    assert "!cfgKnock && !tapTesting && !cfgGesture" in sb, \
        "knocks are still ignored in gesture mode when knocking is switched off"
    assert 'if (n == 1) sendTap("1");' in sb and 'else if (n == 2) sendTap("2");' in sb, \
        "knocks do not send"
    # whether the pad also sends is a setting now, and t_source owns
    # the rule that stops both of them sending for one press
    print("        knocking the desk sends, whatever the knock setting says")
run("Knocking the desk is a gesture even with knocks switched off", t_knock_drives_it)

def t_rests_dark_never_off():
    assert "if (!cfgGesture) { wake(\"knock\"); lastActive = now; }" in src, \
        "a knock still lights the screen"
    assert "if (!cfgGesture)\n    for (int i = 0; i < 18; i++) { eyesFrame(); delay(16); }" in src, \
        "waking still plays the eyes, which is time between your knock and your Mac"
    assert "if (cfgGesture) {\n    if (!asleep && now - lastActive > GESTURE_DARK_MS) goSleep();" in src, \
        "it does not rest dark on its own"
    assert "if (wantDeep && cfgGesture) wantDeep = false;" in src, \
        "holding could still switch it off and take the mode with it"
    assert "if (!deepOff && !cfgGesture && (offlineNow() || deepAfterMs())" in src, \
        "it could still switch itself off on the idle timer"
    dark = int(re.search(r"#define GESTURE_DARK_MS (\d+)UL", src).group(1))
    assert 5000 <= dark <= 30000, f"{dark}ms is not a sensible time to stay lit"
    print(f"        dark after {dark // 1000}s, no animation, and it cannot switch off")
run("It rests dark, wakes without ceremony, and never switches off", t_rests_dark_never_off)

# ----------------------------------------------- the panel, and the race
def t_tile_does_not_flap():
    """The panel asks the robot how it is every ten seconds, and that
    answer describes the robot as it was when the request left. Tap a
    tile while one is in flight and the reply undoes it, which read as
    the first tap never registering."""
    d = open("rafiq-app/mac/Sources/Device.swift").read()
    assert "private var trustLocalUntil = Date.distantPast" in d, "nothing guards a local change"
    assert "private var pollMayWrite: Bool { Date() >= trustLocalUntil }" in d, "no window"
    body = d[d.index("func refresh() async {"):]
    body = body[:body.index("\n    }")]
    for f in ("following", "relaxing", "gesture", "knock", "shake", "offline", "bike"):
        line = [l for l in body.split("\n") if re.match(rf"\s+{f}\s*=\s*Self\.json", l)]
        assert line, f"{f} is no longer read from the robot at all"
        assert all(l.startswith("                ") for l in line), \
            f"{f} is written by the poll without the guard"
    # and the setters say so at once
    for setter in ("following = on; justChanged()", "relaxing = on; justChanged()",
                   "gesture = on; justChanged()", "bike = on; justChanged()"):
        assert setter in d, f"missing: {setter}"
    # the readings the robot owns are never held back
    assert "focusLeft = Self.jsonInt(s, \"focusLeft\")" in body, "the countdown stopped updating"
    assert "netDown   = Self.jsonBool(s, \"netDown\")" in body, "signal state stopped updating"
    print("        local changes believed for a moment, robot readings never held back")
run("A tile lights on the first tap and stays lit", t_tile_does_not_flap)

def t_keys_are_pressed_not_spelled():
    """Clicking the field and pressing the keys, rather than spelling
    them out. A local monitor only sees events meant for this app, so
    recording needs no permission; sending them later is the part
    that does."""
    rob = open("rafiq-app/mac/Sources/RobotSettings.swift").read()
    ges = open("rafiq-app/mac/Sources/Gestures.swift").read()
    assert "struct KeyRecorder: View" in rob, "there is no recorder"
    assert "NSEvent.addLocalMonitorForEvents(matching: .keyDown)" in rob, \
        "it does not listen for keys"
    assert "return nil                                        // never typed anywhere" in rob, \
        "the recorded key would also be typed into whatever is focused"
    assert "if e.keyCode == 53 { stop(); return nil }" in rob, "escape cannot get you out"
    assert "NSEvent.removeMonitor(m)" in rob and ".onDisappear(perform: stop)" in rob, \
        "the monitor is left running after the page closes"
    assert 'TextField("cmd+shift+a"' not in rob, "it still wants them spelled out"
    # the two directions must use one table, or a recorded key does nothing
    assert "static func name(for code: CGKeyCode)" in ges, "no way back from a key code"
    assert "codes.first { $0.value == code }?.key" in ges, \
        "the reverse lookup is a second table and can drift from the first"
    blk = re.search(r"private static let codes:.*?\]\n", ges, re.S).group(0)
    pairs = re.findall(r'"([a-z0-9]+)":\s*(\d+)', blk)
    assert len(set(c for _, c in pairs)) == len(pairs), \
        "two names share a key code, so a recording is ambiguous"
    writes = re.findall(r'parts\.append\("(\w+)"\)', ges)
    reads = set()
    for m in re.finditer(r'case ((?:"\w+"(?:, )?)+):\s*flags\.insert', ges):
        reads.update(re.findall(r'"(\w+)"', m.group(1)))
    missing = [w for w in writes if w not in reads]
    assert not missing, f"it can record {missing} and cannot send it"
    assert "complaint = \"cannot send that key\"" in rob, \
        "a key it cannot send would be recorded and quietly do nothing"
    print(f"        {len(pairs)} keys, one table both ways, every modifier round trips")
run("Keys are pressed into the field, not spelled out", t_keys_are_pressed_not_spelled)

def t_shortcuts_listed():
    """Nobody remembers the exact name of a shortcut, and a name one
    character out fails silently."""
    ges = open("rafiq-app/mac/Sources/Gestures.swift").read()
    rob = open("rafiq-app/mac/Sources/RobotSettings.swift").read()
    assert 'run(["list", "--folders"])' in ges, "it does not ask what folders you have"
    assert 'run(["list", "--folder-name", f])' in ges, "it does not look inside them"
    assert 'run(["list", "--folder-name", "none"])' in ges, \
        "shortcuts outside every folder would be lost"
    assert 'if out.isEmpty {' in ges and 'run(["list"])' in ges, \
        "no fallback if the folder calls come back empty"
    assert "struct Folder: Identifiable" in ges, "nothing holds the grouping"
    assert "Task.detached" in ges, "reading them would block the panel"
    assert "ForEach(g.folders) { f in" in rob and "Menu(f.name)" in rob, \
        "the folders are read but the menu is still flat"
    assert 'Button("Open Shortcuts")' in rob, "no way to go and look"
    assert 'Button("Reload the list")' in rob, "no way to pick up a new one"
    assert "g.readShortcuts()" in rob, "the list is never asked for"
    print("        grouped by folder, with the loose ones and a way to go and look")
run("Shortcuts are grouped by folder, so the Mac ones can be found", t_shortcuts_listed)

def t_page_on_the_grid():
    app = open("rafiq-app/mac/Sources/App.swift").read()
    rob = open("rafiq-app/mac/Sources/RobotSettings.swift").read()
    assert "case .gestures: showGestures = true" in app, "the page cannot be reached"
    assert "GesturePane(showing: $showGestures)" in app, "the page is not shown from the panel"
    assert "if ges.on { showGestures = true; closeOthers(except: .gestures) }" in app, \
        "switching it on does not open the page"
    assert "case gesture," not in rob and "GesturePane()" not in rob, \
        "it is still in the robot's settings as well"
    assert "if keep != .gestures { showGestures = false }" in app, \
        "it does not close when another page opens"
    print("        opens from the grid when switched on, gone from robot settings")
run("The page opens from the main grid, and only from there", t_page_on_the_grid)

def t_grid_swap():
    app = open("rafiq-app/mac/Sources/App.swift").read()
    rob = open("rafiq-app/mac/Sources/RobotSettings.swift").read()
    assert 'Tile(icon: "hand.tap", name: "Gestures"' in app, "gestures is not on the main page"
    assert 'name: "Update"' not in app, "update is still on the main page"
    assert 'Tile(icon: "arrow.down.circle", name: "Update"' in rob, \
        "update did not land in the robot's settings"
    print("        gestures on the grid, update in the robot's settings")
run("Update and gestures changed places", t_grid_swap)

def t_apps_to_add():
    ges = open("rafiq-app/mac/Sources/Gestures.swift").read()
    assert "net.whatsapp.WhatsApp" in ges, "WhatsApp is not there"
    assert "com.tinyspeck.slackmacgap" in ges, "Slack is not there"
    assert "static let order" in ges, "there is no list to offer"
    assert "if !app.cam.isEmpty, Keys.press(app.cam)" in ges, \
        "an app with no camera shortcut would have an empty one pressed at it"
    assert "static var names: String" in ges and "silenced at the device" in ges, \
        "it does not say which apps it can tell, or that the rest still work"
    rob = open("rafiq-app/mac/Sources/RobotSettings.swift").read()
    assert 'Button("Choose an app…") { pickApp() }' in rob, "no way to add anything else"
    print("        Zoom, Teams, WhatsApp, Slack, browsers, and anything you pick")
run("The apps worth adding are offered, and the rest still get silenced", t_apps_to_add)

print()
if fails:
    print(f"{len(fails)} FAILED:"); [print("  -", f) for f in fails]; sys.exit(1)
print("PASS")

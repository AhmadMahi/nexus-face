# Rafiq: decisions, architecture and handoff

Read this first. It is the single source of truth for the Rafiq robot firmware,
the Rafiq Mac app and the Rafiq Android app, as of **firmware 7.9.0, Mac app
4.5.0, Android app 7.4.0**. Older detail (6.x history, the full sandbox
toolchain recipe) is in `firmware/nexus-face/HANDOFF.md`.

---

## 0. How to work on this project (for Claude Code)

- **The owner is Ahmed.** He dictates, often from a phone, so expect
  speech-to-text slips: "Anthropic" or "Refig" usually means **Rafiq**,
  "traffic app" means the Rafiq app. Read charitably; ask when truly unsure.
- **Explain, then confirm, then build** for anything larger than a bug fix.
  Ahmed likes to see the plan and approve it.
- **Every firmware change bumps `FW_VERSION`.** Every Mac change bumps `VER` in
  `mac/build.sh`. Android bumps `versionCode` and `versionName`.
- **Verify before delivering:**
  - firmware builds with **no new warnings** (the original code carries some
    of its own; compare against them, never add to them)
  - `test_rafiq.cpp` passes (125 tests at 7.9.0)
  - any screen change goes through the layout checker (`tools/v6/ui_check.py`,
    see section 11) with **0 problems**
- **Writing style in documents and UI text:** no em dashes or en dashes,
  plain sentences, no invented numbers, sentence case.
- **Code style:** long plain-English comments that say *why*. The firmware is
  one large `.ino` on purpose: the tools parse it.
- **Be honest** about what was and was not tested on hardware.

---

## 1. The product

Rafiq is a 3D-printed pocket robot (keyring size) with a 128 by 64 OLED face.
It shows phone notifications, keeps prayer times, runs timers and reminders,
works with a Mac, and looks after a bag (Away, tamper, left behind). It lives
on **Bluetooth**; WiFi only comes on when asked.

| Repo / folder | What |
|---|---|
| `firmware/nexus-face` | ESP32-C3 firmware (`nexus_face/nexus_face.ino` + `faith_data.h` + `arabic_glyphs.h`) |
| `mac/rafiq-app` | Mac menu bar app (Swift, built by `mac/build.sh`, CI on GitHub Actions); also the Windows twin (`windows/`, not updated for Bluetooth yet) |
| `android/RafiqAndroid` | Android app (plain Java, no Gradle) |

GitHub: `AhmadMahi/nexus-face` (firmware) and `AhmadMahi/rafiq-app` (apps).

---

## 2. Hardware

| Part | Where | Notes |
|---|---|---|
| ESP32-C3 Super Mini | | 4 MB flash, BLE only, no 32 kHz crystal |
| SSD1306 128x64 | I2C 0x3C, SDA 8, SCL 9 | |
| ADXL345 | I2C 0x53 | Knocks, tilt, free fall, motion. **Low-power mode at 100 Hz** since 7.7 (BW_RATE 0x2C = 0x1A) |
| MPU6050 | I2C | Optional |
| TTP223 touch pad | GPIO5 | The main input |
| ADXL INT1 | GPIO4 | Often not wired; probed at boot (`intWired`) |
| Battery divider | GPIO1 | 2:1. Battery is **350 mAh** |
| Power switch, USB-C | | Charging over USB-C |

Measured by the owner: about **15 hours** per charge with about 2 to 3 hours of
screen use. That implies about 19 mA when idle, far above what light sleep
should draw (2 to 5 mA). **The battery log (7.8) exists to find out why.**
Suspects: light sleep not engaging, the board's always-on power LED, wakes.

---

## 3. Build and CI

### 3.1 Firmware
- **Arduino-ESP32 core 3.3.10** (ESP-IDF 5.5.4). FQBN:
  `esp32:esp32:esp32c3:PartitionScheme=min_spiffs,CDCOnBoot=cdc`
- Libraries: **NimBLE-Arduino 2.5.1** (not 3.x), Adafruit GFX 1.12.6, Adafruit
  SSD1306 2.5.17, ArduinoJson 7.4.2, FluxGarage RoboEyes 1.1.2.
- Partitions: app0 and app1 are 1.9 MB each. **Flash use is 94%** at 7.9.0.
  Watch it; every feature costs a few KB.
- **The light-sleep core patch is required for real light sleep**
  (`Rafiq_LightSleep_core_patch_3.3.10.zip`, in `firmware/`). It enables
  `CONFIG_PM_ENABLE`, tickless idle and BT modem sleep. Without it the firmware
  still builds and runs (`pmAvail` is false) but **never light-sleeps**, which
  costs a lot of battery.
- **CI warning:** the GitHub workflow builds on a stock core. For release
  builds, install the patch in the workflow (unzip over the core's
  `tools/esp32c3-libs/3.3.10`) or build locally with it. Verify with
  `riscv32-esp-elf-nm *.elf | grep esp_pm_configure`.
- Outputs: `*.ino.bin` is the **APP** image (wireless update);
  `*.ino.merged.bin` is the **FULL** image (USB only, at 0x0).

### 3.2 How the firmware source was evolved
The `.ino` was changed through a chain of Python patch scripts
(`tools/v6/patch_v6.py` ... `patch_v79.py`), each applied to the previous
result, starting from the 5.x base. **The `.ino` in this bundle is the result
after all patches.** Going forward, edit the `.ino` directly; the patch
scripts are history (they show what each version changed and why).

### 3.3 Mac app
- `cd mac && ./build.sh` on a Mac with the Command Line Tools produces the DMG.
- GitHub Actions: workflow "Build and release Rafiq for macOS" (macOS runner).
  Tag `mac-vX.Y.Z` (must match `VER` in `build.sh`) or run it manually; the DMG
  is attached as artifact `rafiq-macos`.
- **The runner's Swift compiler is strict.** Rules that already broke builds:
  - a captured `weak self` must not be used inside a `Task` in an escaping
    `@Sendable` closure; capture a constant (`let me = self`) instead
  - helpers called from stored property initialisers of a `@MainActor` class
    must be `nonisolated`
  - CoreBluetooth delegate methods are `nonisolated` and step onto the main
    actor with `MainActor.assumeIsolated` (the manager uses `queue: .main`)
  - `if let (a, b) = ...` tuple destructuring is not valid
- The app is not notarised: first open needs System Settings, Privacy and
  Security, **Open Anyway**.
- Info.plist keys added: Bluetooth, Reminders (full access), Location.

### 3.4 Android app
Plain Java against `android.jar` API 33 (minSdk 26, targetSdk 33). Build with
`aapt`, `javac -source 8`, `dalvik-exchange` (dx), `zipalign`, `apksigner`;
see `android/RafiqAndroid/BUILD.md`. **Keep `rafiq.keystore`** (password
`rafiq123`): every update must be signed with it or Android refuses to install
over the old app. Keep the keystore private.

---

## 4. The Bluetooth protocol (firmware 7.2 and later)

One custom service; every characteristic needs a **bonded, encrypted** link.
UUID pattern `52a1f0XX-7a3e-4b5c-9d6f-0a1b2c3d4e5f`:

| XX | Name | Kind | Format |
|---|---|---|---|
| 000 | service | | |
| 001 | CMD | write | A command as text: a RAFIQ command (section 6) or an app `!` command |
| 002 | NOTE | write | A notification: `cat \x1F app \x1F title \x1F text` (cat 1 call, 2 missed, 4 social) |
| 003 | TIME | write | 8 bytes little-endian: the phone's **wall clock** in seconds (UTC + offset) |
| 004 | STAT | read | `key=value;` pairs (below) |
| 005 | EVT | notify | Events out (below) |
| 006 | PTR | write without response | `"x y"`, each -1000..1000: the Mac pointer for Follow |
| 007 | CFG | read | Settings as JSON with `/api/state`'s own names, under 512 bytes |
| 008 | LST | read | `{"apps":[seen],"muted":[...],"vip":[...]}` |
| 009 | OTA | write without response | Firmware bytes after `!ota begin <size>` |

**STAT keys:** `fw bat away timer unread quiet guard wake h12 clock rxc rxn
relax follow gest knob walk v ls lon ldk lls ldp lwf lwk lrs lst lp0 last`.
(`v` volts, `ls` light sleep now, `lon/ldk/lls/ldp/lwf` seconds screen on, dark,
light sleep, deep sleep, WiFi this cycle, `lwk` wakes, `lrs` restarts, `lst`
cycle start epoch, `lp0` percent at cycle start.)

**EVT events:** pad `t1` (tap), `th` (hold), `t2` (long hold); knocks `k1 k2
k3`; lean `ll lr`; knob `kv <degrees>`; tasks `done <k>` (0 pinned, 1 to 3 top
three); prayer `pray <name>`; update `ota ready`, `ota <percent>`, `ota ok`,
`ota err <why>`. WiFi/UDP gestures used `1` and `2`; still understood.

**After a firmware change** the robot sends the GATT **Service Changed**
indication on every trusted link (7.4.1), because macOS caches services of
bonded devices and otherwise never sees new characteristics. The Mac app
re-discovers on `didModifyServices`.

**Text over the link is ASCII.** The apps convert curly quotes, dashes and
accents and drop emoji, because the OLED font is ASCII.

---

## 5. Firmware architecture (7.9.0)

### 5.1 Network model
- `cfgNetHome` is Bluetooth or Off; WiFi is never "home". WiFi sessions:
  manual (**10 minutes** unused, then off), sync (about 150 s), update,
  hotspot.
- **WiFi never comes on by itself** (7.4). Quick power cycles used to count as
  "Bluetooth failed" and fall back to WiFi; now only real crashes count, and
  even then it stays on Bluetooth.
- **Updates only from a file:** Settings, Update opens the hotspot
  (`RAFIQ-SETUP`, 192.168.4.1, Update, From a file), or the Mac sends it over
  Bluetooth (7.5). GitHub self-update was removed (7.4.1).

### 5.2 Links and devices (multi-link, 7.3)
- Every bonded device is remembered with a label (`iPhone` from Apple's
  notification service, `Android ...` or `Mac ...` from the app's `iam`).
- **Multi-link** on: up to two devices at once. **Primary** and **Second**
  preferences are chosen on the robot and saved.
- `btConn` is "the phone" (clock, notifications, guard). **A Mac is never the
  phone** (7.3.1): if it holds the place and anything else links, they swap.
- **Radio:** fast advertising for 30 s after a drop, then slow (546 ms).
  **Silent when two are linked.** A watchdog re-opens advertising every second
  when there is room (7.3.1; one request at connect was not enough).
- Link rhythm: awake 90 to 120 ms with latency 4; **dark 120 to 150 ms with
  latency 12** (under Apple's 2 s limit, 7.7). Follow and OTA ask for faster.

### 5.3 Power
- **Light sleep** when the screen is dark and a phone is held (needs the core
  patch). **Deep sleep** when alone; it wakes for a touch, a prayer alert or a
  reminder.
- **Automatic Away** two minutes after nothing is linked; checks for the phone
  every 3 minutes for the first hour, then 5. Ends when a known device
  returns. Manual `away:` needs `home`.
- **Night sleep** (7.7): from bedtime to 15 minutes before Fajr (or 05:00). At
  bedtime, if idle and dark, a 10 s card: **tap = an hour later**, hold = now.
  Also `RAFIQ night later` and the Mac button. The push is for tonight only;
  it resets in the hour after the night ends.
- **Pocket lock** (7.9): after N minutes (off, 1, **3**, 10) since the last real
  touch, a short touch or a movement does nothing; a hold of **Wake on hold**
  (1 s or 3 s, default 3 s) wakes it, with a thin bar after the first second.
  Never locks during a timer, Relax, Follow, Away or an update.
- **Battery log** (7.8): seconds screen on, dark but awake, light sleep, deep
  sleep, WiFi; wakes; restarts; estimated mAh per state; measured percent used.
  One cycle; resets when the voltage reaches **New log at** (4.10, 4.20, 4.25,
  4.30 V) and not again until it falls 0.10 V below. Kept in RTC memory and in
  NVS (`blog`) every 30 minutes and before deep sleep.

### 5.4 Navigation (7.6)
Five stops: **Home, Today, Faith, Calm, Settings**. Tap = next, hold = open,
longer hold = back, 4 s = home then off.

| Hub | Inside |
|---|---|
| Today | Notifications, Reminders, Mac (when the Mac sends cards), Weather, Vehicle (when on) |
| Faith | Prayer times, Zikr, Adhkar (morning or evening by the clock), Quran, 99 Names, Prayer settings |
| Calm | Relax, Short reads, Games |
| Settings | Display, Touch and motion, Connections, Away and safety, Battery, System |

- Inside a hub, a tap moves to the next item; back returns to the hub's menu.
- **Home looks ahead:** hold on Home opens Today's menu; a tap goes to Faith
  when a prayer is within 15 minutes.
- **Watch faces** are chosen in Settings, Display, Face (hold on Home no
  longer opens faces).
- `screenHasDepth()` must list every screen that has an inside, **including
  the hubs**, or the loop closes them at once (the 7.6.1 bug).

### 5.5 Settings groups

| Group | Rows |
|---|---|
| Display | Brightness, Face, Clock, Sleep after, Popup time, Page turn, Eye style, Vehicle |
| Touch and motion | Wake by, Pocket lock, Wake on hold, Hold time, Go back by, Knocks, Tap strength, Accelerometer |
| Connections | Network, Multi-link, Primary, Second, Pair a Mac, Hotspot |
| Away and safety | Auto away, Phone guard, Tamper alarm, Tamper log |
| Battery | Battery use, Battery log, New log at, Battery full, Power down, Night sleep, Bedtime |
| System | Update, Reset settings, Reboot, About |
| Prayer settings (opened from Faith) | Prayer times, Hijri shift |

### 5.6 Design rules (one block in the firmware, 7.6)
Panel 128 x 64. Padding 3 px. Title bar 11 px (name left, time or count right;
**a long title drops the clock**). Rows 12 px pitch, 4 visible, first text at
y 14. Icons 8 x 8 at x 3, text at x 15. Hub icons are the same icons at 2x.
Selected row: rounded bar 1 px inset. Values right-aligned to the padding,
5 px further in with a scrollbar. Scrollbar 2 px at the right edge, only past
four rows. Hint line centred at y 54. **Titles in capitals.** Labels short
enough for their longest value; nothing fixed is ever truncated. Helpers:
`uiRow`, `uiScroll`, `uiFirst`, `uiIcon`, `uiHubCard`.

### 5.7 Notifications
- From an iPhone via ANCS (5 attributes), from Android via NOTE.
- **App filter and VIPs live on the robot** (7.5), so every phone follows the
  same rules: switched-off apps are dropped; VIP names or words always pop up,
  even when quiet. Seen apps are remembered (`seena`), muted (`mutea`), VIPs
  (`vipw`).
- **Calls:** a ringing iPhone call popup says `tap:answer` / `hold:deny`; it
  uses ANCS positive and negative actions.
- Inbox kept in `/notes.bin` through sleep.

### 5.8 The Mac screen and cards (7.5)
The old Focus screen slot (`S_FOCUS`) is now **Mac**: card 0 health line,
card 1 top three tasks, card 2 pinned task. Sent with `!card <slot> title \x1F
line \x1F line \x1F line \x1F bar`. Hold opens a tick list; hold ticks one off
and sends `done <k>`.

### 5.9 Other features
Timer ring (7.3), Away with contact card (`OWNER_*` defines), tamper alarm and
log, phone guard, find, relax (3 min), zikr, reminders (Shortcut, Mac and Apple
Reminders, kept on the robot), prayer times (Aladhan method 1, school 0) with a
three-step alert, Hijri, Quran, 99 Names, adhkar, short reads, weather, 10
games, 23 watch faces.

---

## 6. Commands

### 6.1 RAFIQ commands (iPhone Shortcut, Android, Mac)
A notification from the Shortcuts app titled or tagged RAFIQ, fresh (under
120 s), run once. Timer running: only `timer` and `home`. Away: only `home`
and `away:`.

`sync`, `status`, `find`, `home`, `sleep`, `off`, `reboot`, `relax`, `zikr`,
`notifications on|off`, `guard on|off`, `tamper on`, `wake touch|shake|both|by`,
`update` (opens the hotspot), `wifi`, `config`/`hotspot`, `prayer refresh`,
`away`, `away: <text>`, `msg: <text>`, `remind HH:MM <text>`,
`prayer <name> +N`, `bright N`, `face N`, `timer N | +N | -N`,
`night later` / `night +1`, weather lines `temp=..;cond=..;hum=..;wind=..;city=..`.

### 6.2 App `!` commands (apps only, never a Shortcut)
`!cfg <key> <value>`, `!relax 0|1`, `!follow 0|1`, `!dnd <min>`,
`!busy <cam> <mic> [muted]`, `!tap <n>`, `!deep 0|1`, `!autoup 0|1`,
`!turn 0|1`, `!bike plate\x1Fmake\x1Fmodel\x1Fowner`,
`!net add ssid\x1Fpass | del i | up i`, `!remclear`,
`!rem <wall-seconds> <done> <text>`, `!card ...`, `!knob 0|1`, `!walk 0|1`,
`!bye` (Mac going to sleep), `!dim 0|1`, `!mute a\x1Fb`, `!vip a\x1Fb`,
`!pt f d a m i` (prayer minutes), `!ota begin <size> | end | abort`,
`!night <minutes>`. Also `iam <name>` and `ping`.

`!cfg` keys include `bri face slpi popi eye hadj back wakeh gest gsrc knock bike
btpl deepi bfull offl net night bed blog blogv cap blogreset plock` (all
handled by one function, `cfgApply`, shared with HTTP `/api/cfgv`).

---

## 7. Mac app (4.5.0)

| File | Role |
|---|---|
| `RobotLink.swift` | CoreBluetooth link: finds the robot, pairs, characteristics, STAT and CFG polling (10 s), events, pointer, notes, list, OTA with flow control, RSSI, **offline queue** |
| `MacFeatures.swift` | The Mac batch: gesture priority, meeting mute, clicker, knob, screenshot, Mac health card, dim while typing, prayer pause, Apple Reminders, pinned task, walk-away, last seen, low battery, weather and prayer times |
| `BatteryDiary.swift` | Battery diary: a reading every 10 minutes, real drain from the percentage, CSV export |
| `Device.swift` | HTTP API (WiFi), the Bluetooth mapping (`bleCommands`), settings state, `applyBleState` |
| `Gestures.swift` | Per-app gesture maps (tap or one knock, two knocks, hold, lean left, lean right) and actions (app, Shortcut, keys) |
| `App.swift` | The panel: 16 tiles, header pills (battery, timer, away, offline, waiting) |
| `SettingsPane.swift` | Connection, Rafiq and this Mac, Battery, WiFi |
| `RobotSettings.swift` | The robot's settings, Notifications filter page, Gestures, Update page |
| `UpdatePane.swift` | Update over Bluetooth (7.5+) or through the hotspot |

**Gesture priority:** a ringing call (handled on the robot) > a meeting (pad tap
mutes the Mac microphone in hardware; knocks never touch the mic) > presenting
(Keynote, PowerPoint, a browser: knock next, lean back) > the knob (hold the
pad and tilt) > three knocks = screenshot to clipboard > your own gestures.

**Offline queue (4.4):** when the robot is not linked, messages queue in order;
states keep only the latest (`away`/`home` share one key; each `!cfg` key is
one key); moments (timer, find, relax, zikr, sync, sleep, restart, gestures)
are greyed out. Queue survives a restart; items older than 3 hours are
dropped.

**Two lock settings, different things:** "Lock when I walk away" (old) locks
after N idle minutes, no robot involved; "Walk-away lock" (4.3) locks when
Rafiq leaves (weak signal 20 s, or 10 s after the link drops; never in a call
or presentation). macOS must require the password immediately after sleep.

---

## 8. Android app (7.4.0)
Forwards notifications (NotificationListenerService, re-binds itself after
updates), sets the clock, sends `iam`, one-tap commands in three tabs
(Home, Controls, More) with a dark theme, Setup behind a gear, a test
notification, a left-behind alarm on the phone (link lost 20 s or weak signal
10 s; quiet after sending off, reboot, update, wifi and similar).

---

## 9. Decisions log (most important first)

1. **Bluetooth first; WiFi only on request, off after 10 minutes.** Battery.
2. **Updates from a file or over Bluetooth; no GitHub self-update.** Control.
3. **One protocol for every app** (section 4), with `!` commands only from apps
   so a stray Shortcut can never change settings, networks or the pointer.
4. **Filter and VIPs on the robot**, so every phone obeys the same list.
5. **A Mac is never the phone**; Primary and Second only decide who stays.
6. **Silent radio with two linked;** slow advertising after 30 s.
7. **Five-stop navigation with hubs** (psychology: fewer choices, recognition
   over recall, settings next to their feature, Home anticipates).
8. **One design rulebook** and an automatic layout checker for every screen.
9. **Meeting mute in hardware**, pad only, never knocks.
10. **Night sleep with "an hour later"**, ending before Fajr.
11. **Pocket lock with a hold to unlock**, bar after 1 s.
12. **A 30-second deep-sleep polling cycle was rejected:** reconnecting costs
    far more than staying linked in light sleep (estimated 2 to 5 times worse).
13. **Battery log resets on a voltage**, not on 100%, with a 0.10 V re-arm.
14. **Rename to "Kairo" is parked.** Do not rename yet. A rename touches the
    BLE name prefix (apps scan for `Rafiq`), the Shortcut keyword `RAFIQ`, the
    hotspot `RAFIQ-SETUP`, screens, apps and documents; keep accepting
    `RAFIQ` for a while when it happens.
15. **Wake on hold keeps off, 1 s, 3 s** (no 2 s): inserting an option would
    shift saved indices used by the Mac app and deep-sleep wake.

---

## 10. Gotchas we hit (do not repeat)

- **RoboEyes defines single-letter macros** (`N`, `E`, `S`, `W`, `NE`, ...). Never
  name a variable or constant with one of them.
- **`getLocalTime(&t, 0)` can give up without reading the clock.** Use
  `nowLocal(&t)` (7.3.1). This caused the random robot face at boot and could
  skip prayer alerts.
- **`screenHasDepth()`** must include any new screen with an inside.
- **NimBLE `getClient()`** must be called once per connection (`btPeer()`
  caches it); calling it again wiped subscriptions and caused reboots (6.1).
- **`millis()` arithmetic:** compare as signed differences
  (`(int32_t)(now - t) > 0`) and re-read `millis()` after slow work.
- **CoreBluetooth caches services of bonded devices** (section 4). Fix: Service
  Changed from the robot, `didModifyServices` in the app; last resort, forget
  the device in macOS Bluetooth settings.
- **Two-button labels must fit 62 px** (10 characters). `hold:decline` did not
  (fixed to `hold:deny` in 7.8.1).
- **The panel's title bar** fits about 20 characters; `bar()` drops the clock
  for long titles.
- **Mac app ASCII bug (open, see section 12):** `RobotLink.ascii()` drops the
  0x1F separator, so `!mute`, `!vip`, `!bike` and `!net add` arrive glued
  together and do nothing.

---

## 11. Tools

| Tool | What |
|---|---|
| `tools/v6/test_rafiq.cpp` + `extract_rafiq.py` | Host unit tests for the command parsers (extracts functions from the `.ino`) |
| `tools/v6/ui_check.py` + `gfx.py` | Mirrors the drawing code, renders screens, and checks every element for overlaps, padding and edges |
| `tools/v6/patch_*.py` | The version history as patches (section 3.2) |

Typical test run: `python3 extract_rafiq.py <functions> > fw_funcs.inc && g++ -std=gnu++17 -o t test_rafiq.cpp && ./t`.

---

## 12. Open work (agreed with the owner, not built yet)

### Firmware (next: 7.10)
1. **After a drop**, show the animation, then go straight back to sleep
   instead of staying awake.
2. **Hub items open directly:** Notifications opens the list (depth 1),
   likewise Reminders, Short reads and Games. Weather, Vehicle, Prayer times
   and Mac stay at their own screen. Back still returns to the hub's menu.
3. **On waking, show an unseen notification first** (one that popped up while
   asleep): tap dismisses it and goes Home; hold opens it.
4. **"Second" always shows "any":** trace why the Mac is not in the device list
   (`devSeen` runs only for links marked `authed`; check the Mac link reaches
   `onAuthenticationComplete`) and fix it.
5. **No robot face at startup:** show the last known time if there is one,
   otherwise a gentle animation with "Connecting..." until the clock syncs.

### Mac app (next: 4.6)
6. **Fix the 0x1F separator bug** in `RobotLink.ascii()`: keep `\u{1F}`.
   This is why the notification filter, VIPs, vehicle details and adding
   WiFi networks do not work.
7. **Notifications page:** an All on / All off switch at the top, switches
   aligned on the right, and the real state shown when you come back (re-read
   LST after writing).
8. **Main grid:** put Notifications where Sync is; move Sync into the robot
   settings.
9. **Robot settings, Battery:** Full charge as a dropdown, with the battery
   log (diary) directly below.
10. **Phrases:** a small "Add phrase" button at the bottom of the list.
11. **Remind me:** every minute from 1 to 60, and confirm it syncs to the robot.
12. **Reset:** require typing RESET before anything is reset.
13. **Rename** the old idle lock to "Lock when idle"; one line under each lock
    setting explaining the difference.

### Questions waiting for the owner
- "Better mark": the owner mentioned a setting by this name. Best guess:
  Battery, **Full charge** (the voltage counted as 100%). Confirm before
  changing anything.

### Later
- Measure real current (USB meter or the battery diary) and act on it:
  light sleep share, the power LED, wakes.
- Android: low battery alert and a filter screen.
- Windows app: Bluetooth.
- The rename (parked, see decision 14).

---

## 13. Privacy
- `OWNER_NAME`, `OWNER_PHONE`, `OWNER_MAIL` are compiled into the firmware for
  the Away contact card. **Do not push real values to a public repo**; use
  placeholders there.
- `rafiq.keystore` and its password must stay private.

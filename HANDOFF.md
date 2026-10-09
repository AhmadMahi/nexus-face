# Rafiq (Nexus Robo): Complete Project Handoff

> **For Claude (future conversation):** this file is the single source of truth for Rafiq as of **firmware 6.3.0, 7 October 2026**. Read it fully before answering. The user (Ahmed) will usually attach the source zip. Build outputs and the patched core are not kept between conversations: ask for the zips when code changes are needed, and rebuild the environment as in section 4. Sister project: **C3 Buddy** (wearable, has its own handoff). Rafiq 6.0 borrows C3 Buddy's Shortcut bridge idea, but **not its code, core version or input grammar**.

---

## 0c. 6.3.0 changes

- **Glance on lift or shake.** A wake by `shake`, `picked up` or `moved` shows the watch face at once, with no eye animation, for **1.5 s** (`GLANCE_MS`), then `goSleepQuick()`. Touching the pad during the glance makes it a normal wake with the normal sleep timer. Knock wakes stay normal wakes, because a knock is also a command. Hold-to-wake from deep sleep (Settings > Wake hold, off/1 s/3 s) is unchanged.
- **Notifications show the sender first.** The popup's top bar is sender on the left and app on the right; calls show "Calling" / "Missed call" / "Voicemail" with the name below. The reader shows the sender and time on top, with the app under it.
- **Two solid buttons** at the bottom replace the thin hint line: popup "tap:close" and "hold:open"; reader "tap:n/N" and "hold:clear".

## 0b. 6.2.0 changes

- **Hold strip at the bottom**, drawn over any screen. The display goes through `OledX::display()`, which adds the strip while holding, so screens need no changes.
  - Two zones only, **OPEN then BACK**. The bar fills from the hold time to max(hold+1.5 s, 2 s); the mark sits at the midpoint.
  - Held to **4 s**: home and the original **3 s switch-off countdown**, which letting go does not stop. This is kept from 5.x at the user's request.
- **Popup while asleep is a 1 s glance**, then straight back to sleep (`goSleepQuick`, no eye animation). A tap during the glance keeps it up for the popup time; a hold opens it.
- **iPhone connected / disconnected**: small white text on a black screen for 1.1 s, only when awake.
- **Home on Bluetooth** is always the watch face once the robot has ever been paired. The hello and pairing screens are only for a never-paired robot.
- **Settings > Controls > Wake by**: touch+move (default), touch, move. Gates the touch wake and the knock, shake, pick-up and moved wakes.
- **Faster starts:** from sleep, no eye animation at all (was 2.6 s on every wake). From power on, eyes 1.2 s and the senses check 0.7 s (were 2.6 s and 1.5 s).

## 0a. 6.1.0 changes

**Root cause of "no notifications" and the random reboots.**
- `NimBLEServer::getClient()` deletes every discovered service on each call (NimBLE-Arduino 2.5.1, NimBLEServer.cpp line 1113).
- `setupAncs()` and `readCts()` each called it, in the same pass, so the clock read destroyed the notification subscriptions and left `ancsCP` dangling. Nothing arrived, and the next write could crash.
- This was present since 5.x.
- **Fix:** `btPeer()` caches the client once per connection (`btCl`, `btClStale` set on disconnect and in `bleOff`). It is the only call site; verified by grep. **Rule: never call getClient anywhere else.**

**Other changes:**
- **Touch:** C3 Buddy grammar.
  - Tap = next, on release, with no double-tap wait.
  - Hold shows a bar: OPEN (hold time to max(hold+1.5 s, 2 s)), CANCEL (1 s), BACK, then SWITCH OFF from Back+2 s to deep sleep 3 s later.
  - Settings > Controls > Hold time (0.5 to 2.5 s, default 0.7).
  - Knocks and leaning unchanged as extras. Triple tap removed.
  - The bar is not shown during a game in play; games otherwise untouched.
- **Notifications** (was Notices, `S_MSG`), C3 Buddy style:
  - The popup draws over any screen. A tap closes it and returns you exactly where you were (back to sleep if it woke the robot). A hold opens it.
  - Popup time comes from Settings > Popup time.
  - Inbox: summary, then a list (unread marked *, last row "Clear all"), then a reader (tap next, hold clears this one, back to the list).
  - Saved to `/notes.bin` (survives deep sleep).
- **Screen order:** Home, Notifications, Reminders, Vehicle, Weather, Prayer, Faith, Reads, Games, Settings, System. Focus is removed from the round and the RAFIQ `focus` commands are gone; the Mac `/api/focus` API is kept.
- **Clock:** Settings > System > Clock, 12 hour / 24 hour.
- **Signal:** on Bluetooth, the System network row and the signal displays show the iPhone link RSSI.
- **Update page** (192.168.4.1): the Update card has two tabs, "From GitHub" (unchanged) and "From a file" (posts to `/ota`). The separate /ota page is gone.
- **Speed:** a touch wake opens the eyes in 8 frames (was 18); news wakes show at once.

## 0. 6.0.1 changes (after first hardware test)

| Report from the user | Cause | Fix |
|---|---|---|
| Home showed the robot "Salam" screen, not the watch face | `drawHome()` treated Bluetooth as offline | On Bluetooth with a clock, home is the watch face. The pairing and hello screens remain only when there is no clock |
| RAFIQ Shortcut shows as a notification named Rafiq and does nothing | Most likely the app id: 6.0 only accepted ids starting `com.apple.shortcuts`, while automations post from other Shortcuts processes | Any app id containing "shortcut" or "workflow" counts. A RAFIQ-tagged notification that is still refused lands in the list as "Not run, from <app id>" |
| Sync with no WiFi | Was a brief flash | "SYNC FAILED" card: "No saved WiFi nearby / Back on Bluetooth" (or "No WiFi saved yet / RAFIQ config to add") |
| Wants iPhone linked and unlinked notices | | "IPHONE CONNECTED" / "IPHONE DISCONNECTED" flash, only when awake on Bluetooth; never wakes it |
| Sometimes goes back to sleep and reboots | **Unknown.** Could be a crash, a brown-out, or the 60 s no-phone deep sleep followed by a normal wake | The reset reason is now kept (`lastReset`). A crash, watchdog or brown-out is said at boot ("RESTARTED: CRASH" and so on). `RAFIQ status` shows the reset reason and "LS" if light sleep is available |

Pending for **6.1**: C3 Buddy touch grammar (hold bar Open / Cancel / Back, sleep bar), C3 Buddy-style popups and notification inbox. Questions asked; see the conversation.

## 1. Snapshot

| Item | Value |
|---|---|
| Product | **Rafiq** (also "Nexus Robo"; repo `AhmadMahi/nexus-face`): ESP32-C3 desk and bag companion. Face, clock, prayer times and adhan alerts, Quran, 99 Names, adhkar, short reads, 12 tilt games, reminders, focus timer, Mac and Windows companion apps |
| Owner | Ahmed |
| Firmware | **6.3.0** (6.2.0 bottom hold strip, glance popups; 6.0.0 first Bluetooth-first build; 6.0.1 home and diagnostics; 6.1.0 notification fix and C3 Buddy touch) |
| Status | Built, unit tested and previewed. **Not yet tested on hardware** (see section 13) |
| Deliverables | `Rafiq_v6.0.0_APP_wireless_update.bin`, `Rafiq_v6.0.0_FULL_usb_flash_at_0x0.bin`, `Rafiq_v6.0.0_source.zip` (whole repo), `Rafiq_LightSleep_core_patch_3.3.10.zip`, this file |
| Size | 1,809,605 bytes, **92%** of the 1,966,080 byte app slot (5.20 was 90%). About 156 KB left |

---

## 2. Working agreements

- The user **dictates**. Expect speech-to-text slips ("Wi-Fi BLE", "traffic app" meaning the Rafiq app). Read charitably. If a message is cut off, build what is clear and ask about the rest.
- Every firmware change ships an **APP bin**, a **FULL bin**, the **source zip** and an updated **HANDOFF.md**. Bump `FW_VERSION` every time. The GitHub workflow also checks that the tag matches `FW_VERSION`.
- **Verify before delivering:**
  - a warning-free build for new code (the original carries 36 warnings of its own; no new ones allowed)
  - a stock-core build as well as a patched one
  - host unit tests for parsers
  - pixel-accurate previews for any UI change
- **Own mistakes plainly.** Be honest about what was and was not tested on hardware.
- Organisation writing rules apply to documents: **no em dashes or en dashes**, no invented numbers.
- The user is on the Claude mobile app. Lead with the answer and keep replies scannable.
- **Keep the house style of the code:** long plain-English comments that say *why*, in the first person plural or about the robot. The file is one big `.ino` on purpose: the `tools/` sims parse it.

---

## 3. Hardware (from the code; confirm with the user)

| Part | Pin / address | Notes |
|---|---|---|
| ESP32-C3 Super Mini | | 4 MB flash, BLE only (no Classic BT), **no 32 kHz crystal** |
| SSD1306 128x64 | I2C 0x3C, SDA 8 SCL 9 | Full 64 rows visible (unlike C3 Buddy's enclosure) |
| ADXL345 | I2C 0x53 | Knocks (tap), free fall, motion |
| MPU6050 | I2C | Optional, gyro and temperature; ADXL wins for acceleration |
| TTP223 touch pad | GPIO5 | Main input. Resting level learned and kept in NVS `trest` |
| ADXL INT1 | GPIO4 | **Usually not wired.** Probed at boot (`intWired`). Needed only for tamper in deep sleep |
| Battery divider | GPIO1 | 2:1 |
| LED / buzzer | none | Planned: buzzer or vibration motor later. **Reserve GPIO10** (GPIO3 as backup). A motor needs a transistor and a flyback diode |

---

## 4. Build environment

### 4.1 Toolchain
- **Arduino-ESP32 core 3.3.10** (ESP-IDF **5.5.4**, commit `735507283d5b2f9fb363a1901172dbd9e847945d`, lib-builder 43a8f6d). **Not 3.3.0** like C3 Buddy.
- Toolchain: esp-rv32 2601 (riscv32-esp-elf 14.2.0_20260121).
- Libraries:
  - Adafruit GFX 1.12.6
  - Adafruit SSD1306 2.5.17
  - Adafruit BusIO 1.17.4
  - ArduinoJson 7.4.2
  - FluxGarage RoboEyes 1.1.2 (reports 1.1.1)
  - **NimBLE-Arduino 2.5.1** (master is 3.0.0-dev; do not use it)
- FQBN: `esp32:esp32:esp32c3:PartitionScheme=min_spiffs,CDCOnBoot=cdc`
- Partitions: nvs 0x9000 (20 KB), otadata 0xE000, app0 0x10000 (1.9 MB), app1 0x1F0000 (1.9 MB), spiffs/LittleFS 0x3D0000 (128 KB), coredump 0x3F0000.
- Outputs: `*.ino.bin` = **APP** (wireless). `*.ino.merged.bin` = **FULL** (USB at 0x0 only).

### 4.2 Rebuilding the environment in a sandbox (what worked)
`downloads.arduino.cc` is blocked; GitHub is not.
1. Get arduino-cli 1.3.1 from GitHub releases. Add the index URL `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json` and run `core update-index`.
2. **Install the core by hand:**
   - Unzip `esp32-core-3.3.10.zip` to `~/.arduino15/packages/esp32/hardware/esp32/3.3.10`.
   - Extract into `packages/esp32/tools/<name>/<version>`:
     - esp-rv32 2601 (strip one directory level)
     - esptool_py 5.3.0 (strip one directory level)
     - esp32c3-libs 3.3.10 (move the inner folder's contents up)
3. **Fake what arduino-cli insists on:**
   - Write `~/.arduino15/package_index.json` with an `arduino` package listing `dfu-util 0.11.0-arduino5`, and create the empty folder `packages/arduino/tools/dfu-util/0.11.0-arduino5`.
   - Build ctags from `github.com/arduino/ctags`: `sed -i 's/__unused__/ctags_unused_/g' *.c *.h`, then `./configure && make`. Copy the binary to `packages/builtin/tools/ctags/5.8-arduino11/`.
4. Clone the libraries from GitHub at the tags in 4.1 into `~/Arduino/libraries`.
5. The sketch folder must be named `nexus_face`.
6. Baseline check: v5.20.0 builds to 1,776,013 bytes on stock 3.3.10.

### 4.3 Light-sleep core patch (rebuilt for 3.3.10)
Same idea as C3 Buddy's 3.3.0 patch, redone for this core. Install steps are in the patch zip's README.

1. **Get the exact IDF source:**
   - `git init`, then `git fetch --shallow-since=2026-02-01 origin release/v5.5`, then check out `735507283d`.
   - Init these submodules shallow: micro-ecc, spiffs, cJSON, mbedtls, lwip, esp-mqtt, protobuf-c, unity, nimble, esp_wifi/lib, bt lib_esp32c3_family, esp_phy/lib, heap/tlsf, esp_coex/lib, CMock, and **esp_ble_mesh/lib/lib** (the Arduino config has BLE mesh on). Skip openthread and the other chips' BT libs.
2. **Python environment:**
   - venv with `cmake<3.31`, ninja, `esp-idf-kconfig>=2.5,<3`, `idf-component-manager~=2.2`, plus the rest of `tools/requirements/requirements.core.txt`.
   - Run **cmake directly** (`cmake -G Ninja -DIDF_TARGET=esp32c3 -DPYTHON=... -B build .`), because idf.py demands a constraints file from a blocked host.
   - Set `IDF_COMPONENT_MANAGER=0`, `ESP_ROM_ELF_DIR=/tmp`, and put the core's riscv toolchain on PATH.
3. **Project and config:**
   - Set `COMPONENTS` in the project to main plus every IDF component that has a matching `lib*.a` in the Arduino libs folder, except openthread.
   - Start `sdkconfig` from the Arduino one and append the 8 PM keys; remove the other `LPCLK_SEL` choices.
   - Confirm that `CONFIG_BT_CTRL_SLEEP_MODE_EFF=1` after configure.
4. **Build** with ninja (1,257 objects, about 15 to 25 minutes on one CPU).
5. **Copy back:**
   - Every rebuilt `lib*.a` that exists in Arduino's `lib/`, **except** the precompiled blobs (pp, net80211, core, mesh, smartconfig, espnow, wapi, coexist, phy, btbb, btdm_app), `libspi_flash`, `libmain` and **`libesp_app_format`**. That last one would stamp the image "pmproj"; keep it stock.
   - Copy `sections.ld`.
   - **Merge** `sdkconfig.h`: take the rebuild's values for keys it owns, never take flash-mode keys, keep Arduino-only keys. The result was 12 keys, all PM related.
6. **Verify** the link contains `esp_pm_configure`, `vApplicationSleep`, `esp_pm_impl_waiti` and `btdm_sleep_enter_phase1_wrapper`.

---

## 5. Firmware architecture (what 6.0 touched)

One file, `nexus_face/nexus_face.ino` (about 11,750 lines), plus `faith_data.h` and `arabic_glyphs.h`. **Tasks:**
- **loop** runs at priority 2.
- **netLoop** runs at priority 1: it does all fetching and is gated by `cfgOffline`.
- **NimBLE host task**: callbacks only record; the main task does the GATT work in `btTick()`.

### 5.1 Network model (6.0)

| Variable | Meaning |
|---|---|
| `cfgNet` | What the radios are doing **now**: `NET_WIFI`, `NET_BT` or `NET_OFF`. All older code reads this unchanged |
| `cfgNetHome` | Where it returns to. **Only BT or OFF.** The only one kept in NVS (`net`). Any stored WiFi is corrected to BT at boot |
| `wsKind` | The current WiFi session: `WS_MANUAL`, `WS_SYNC`, `WS_UPDATE`, `WS_HOTSPOT` |

| Session | Started by | Ends |
|---|---|---|
| MANUAL | Settings > Network (BT then WiFi), the page or Mac (`net=0`), `RAFIQ wifi`, safe mode, BT failure fallback | Real restart (deadline lives in RTC memory: `rtcWsUntil`, `rtcWsKind`); **survives deep sleep wakes**; or 30 min after the last page or app request (`wsTouch()` in `guard()`, `/api/state` and `/`) |
| SYNC | `RAFIQ sync`, `RAFIQ prayer refresh` | All fetches done (time, weather, prayer, update check, then a new short read if an OpenAI key is set), or 150 s. Update found = U_ASK screen, wait up to 3 min |
| UPDATE | Settings > Check update while on BT, `RAFIQ update` (goes straight to the newest release) | Leaving the update screen |
| HOTSPOT | Settings > Hotspot, `RAFIQ config` | 10 min with no station connected |

- **Bluetooth is off during any WiFi session.** WiFi with TLS plus NimBLE together is a RAM risk on the C3. A sync therefore drops the phone for about a minute, and it reconnects after.
- `wsEnd()` waits up to 15 s for `netBusy`, a flag the network task holds for the length of each fetch, so the radio is never pulled mid-download. Flags alone could not say this, because netLoop clears each flag before its fetch starts.
- Web handlers never end a session directly. They set `wsEndWant`, which `serviceWs()` acts on after the reply has gone out.

### 5.2 RAFIQ Shortcut bridge (ANCS)
- **ANCS now requests 5 attributes:** app id, title (32), subtitle (32), message (**400**, two-byte length), date. `dsBuf` grew from 512 to 768 bytes. The long parts land in `stMsg` / `stSub` / `stDate`; `Note.msg` still keeps 100 characters for the list.
- **Recognised only if the app id is `com.apple.shortcuts` (or `is.workflow...`)** and RAFIQ starts the title, subtitle or message. *Rafiq is also a person's name*: a WhatsApp from someone called Rafiq must never be a command. A test covers this.
- **Freshness:** commands run only if the notification date is within 120 s old (up to 300 s ahead allowed), compared wall clock to wall clock, and if its FNV-1a hash of `date|payload` is not among the last 8 (NVS `rqseen`). With no clock or no date, run-once alone decides.
- **Weather lines are always applied**, even when stale. They are data, not commands.
- Radio-changing commands are queued (`rqPend`) and run 700 ms later, so `ancsAction(uid, 1)` (clear) goes out first. Shortcuts notifications usually cannot be cleared anyway.
- A line with no `:` or `=` is a command. A payload of commands wakes the screen; a weather report alone does not.

### 5.3 Power
| State | When | Current (estimates, not measured) |
|---|---|---|
| Awake | Screen on, 160 MHz | 20 to 35 mA |
| Light sleep | Screen dark, `cfgNet == NET_BT`, **patched core**, no USB host. Phone stays linked. Loop polls input every 100 ms, `delay(50)` | 1 to 3 mA |
| Deep sleep | Not linked for 60 s (`BT_DEEP_GRACE_MS`, counted from screen-off or disconnect, whichever is later), or held 4 s, or WiFi manual idle as before | 5.20 figures |
| Stock core | `pmAvail == false`: behaves exactly like 5.20 on BT (deep sleep as soon as the screen darkens) | |

- `pmInit()` runs at boot and tries `esp_pm_configure(160,160,false)`. `pmSet(idle)` switches between 160/160 (awake) and 160/40 with light sleep (idle). `wake()` uses `pmSet(false)` instead of `setCpuFrequencyMhz` when PM is available.
- A reconnecting, already-bonded phone no longer lights the screen while asleep. First pairing still does.
- Deep sleep is blocked during a sync, the hotspot, an update, a pending command, the guard alert or the tamper countdown.

### 5.4 Phone guard
- **When:** `cfgPGuard` (NVS `guard`), Bluetooth only.
- **Measurement:** RSSI via `ble_gap_conn_rssi` every 2 s, smoothed (0.7 / 0.3).
- **Alert:** weaker than **-88 dBm for 6 s**, or **disconnected for 4 s** after having been linked. Shows "WAIT FOR ME" with worried eyes, blinking, for 2 min. A press dismisses it.
- **Re-arm:** when the phone is back at better than -80 dBm (or simply reconnected). "THERE YOU ARE" flashes.
- **Limitation:** visual only until a buzzer or motor is added.

### 5.5 Tamper
- **Arming:** Settings > System > Tamper alarm (hold), or `RAFIQ tamper on`. 10 s countdown; a press cancels.
- **Once armed:** logs "Armed", sets NVS `tamper`, then screen off, Bluetooth off, WiFi off.
- **With INT1 wired:**
  - The ADXL activity interrupt goes to INT1: THRESH_ACT 6 (375 mg), AC coupled, xyz.
  - The chip deep sleeps waking on GPIO4 high.
  - A wake logs "Moved", then pauses 30 s on the timer before re-arming (continuous movement does not flood the log).
  - `tamperWake()` runs first in `setup()`, before `wakeGate()`.
- **Without INT1** (the usual case): light-sleep polling at 5 Hz. Logs "Moved" (more than 0.25 g change from baseline) and "Touched", each at most once per 30 s. Roughly 1 to 2 mA, estimated.
- **Disarm:** any real restart (power or reset button) logs "Disarmed" and shows "TAMPER OFF, SEE LOG". There is no Shortcut disarm, because Bluetooth is off.
- **Log:** `/tamper.txt` on LittleFS, at most 4 KB (keeps the newer half). Viewable in Settings > System > Tamper log, 24 newest first.
- **Clock:** time comes from the RTC; the time zone is saved to `rtcTz` at arming. Prayer and reminder alarms do not fire while armed.

### 5.6 Install from a file
- **Route:** `GET /ota` serves a small upload page; `POST /ota` streams to `Update`.
- **Allowed** only while the hotspot is up, or for a paired Mac token.
- **Checks:** rejects anything not starting with 0xE9. A FULL image fails on size.
- **Feedback:** progress on the panel every 64 kB; restarts after success.

### 5.7 Storage added in 6.0
| Where | Key | Contents |
|---|---|---|
| NVS `nexus` | `netv6` | Migration done marker |
| | `guard`, `quiet` | Phone guard on; notifications quiet |
| | `lsync` | Last sync, system clock seconds |
| | `wx` | Weather: `temp|hum|wind|code|at|city` |
| | `rqseen` | 8 x uint32 run-once hashes |
| | `tamper` | Armed flag (for the Disarmed entry) |
| LittleFS | `/tamper.txt` | Tamper log |
| RTC | `rtcWsUntil`, `rtcWsKind`, `rtcTamper`, `rtcTz` | Survive deep sleep only |

### 5.8 Settings rows added
- Wireless: **Phone guard** (on/off).
- System: **Tamper alarm** (hold to arm), **Tamper log** (hold to view; press next, double back).
- **Network** now cycles: Bluetooth, then WiFi now, then Off, then Bluetooth. It shows `wifi now`, `syncing` or `hotspot` while in a session.

---

## 6. RAFIQ commands

**Setting up on the iPhone (once):**
1. Settings > Bluetooth > Rafiq > **Share System Notifications** on.
2. Settings > Notifications > Shortcuts: allowed.

**Making a Shortcut:**
- Name the Shortcut **RAFIQ** (or set the notification title to RAFIQ).
- Add the action **Show Notification** with the command as the body. Several commands go on separate lines or are separated by `;`.

| Command | Does |
|---|---|
| `sync` | Sync burst (WiFi about a minute, then Bluetooth) |
| `update` | WiFi, check the newest release, ask Yes/No |
| `wifi` | Manual WiFi until restart or 30 min unused |
| `config` / `config-robo` / `hotspot` | Hotspot for a file install |
| `bluetooth` | Says it is on Bluetooth (it already is, or the message could not have arrived) |
| `guard on` / `guard off` | Phone guard |
| `tamper on` | Arm tamper (10 s countdown) |
| `find` | Screen calls out for 20 s |
| `msg: text` | Onto the Messages screen and into the list |
| `remind 18:30 Call mom` | Reminder today, or tomorrow if passed |
| `prayer fajr +2` | Prayer correction in minutes (also dhuhr/zuhr, asr, maghrib, isha; capped at 60) |
| `prayer refresh` | Fresh prayer times via a sync |
| `focus` / `focus 40` / `focus stop` | Focus timer (default 25, capped at 240) |
| `relax` / `relax off` | Relax screen |
| `bright 50` | Brightness in percent (floor 10) |
| `face 3` | Watch face (1-based) |
| `notifications on` / `off` (also `quiet`, `silent`) | Off = they still land in the list but wake nothing |
| `home`, `sleep`, `off` / `deep sleep`, `reboot` | Navigation and power |
| `status` | Card: link, battery, last sync, guard, quiet |
| Weather lines | `temp=24;cond=Sunny;hum=60;wind=12;city=X`, or C3 Buddy's long form (`Temp: 21°C (H: ..)`, `Condition:`, `Humidity:`, `Wind:`). °F and mph are converted. Conditions map to icons |

Unknown text shows "RAFIQ?".

---

## 7. Decisions log (6.0)

1. **Bluetooth first, WiFi on demand** (user, after weighing an auto-scan design). Rejected: background scans and boot scans with cooldowns. Asking is cheaper and simpler.
2. **WiFi is never stored as home.** A manual session survives deep sleep but not a restart (user).
3. **The sync drops Bluetooth for its duration** instead of running both radios. RAM safety with TLS.
4. **The sync writes a short read and asks before installing an update** (user).
5. **Light-sleep patch rebuilt for 3.3.10**, not reused from C3 Buddy's 3.3.0. The firmware still builds and runs on stock.
6. **RAFIQ commands only from the Shortcuts app**, because Rafiq is a common name.
7. **Weather is persisted** because Bluetooth mode deep sleeps often.
8. **The hotspot became a session** that closes itself, and gained a file-install page. Previously it forced WiFi as home and never closed.
9. **Safe mode and the Bluetooth-failure fallback are WiFi for this boot only**, or the hotspot when no network is saved.
10. **The tamper fallback without INT1** watches from light sleep. Rafiq boards usually lack the wire.
11. **CI workflow now installs NimBLE-Arduino 2.5.1.** It was missing, so CI builds since Bluetooth arrived probably failed.

---

## 8. Known limits and conflicts

- **GitHub releases are stock-core builds.** A device that updates itself from GitHub loses light sleep, and then behaves like 5.20 on Bluetooth. Ship releases built locally with the patch, or accept it.
- **Mac and Windows apps still use WiFi only.** They work during a MANUAL session. Planned: a custom GATT service carrying the same API, with the apps falling back to BLE (three connections are allowed). Until then, gesture mode, the pointer, the camera light and the like are dormant on Bluetooth.
- **BRIDGE is one way.** Shortcuts cannot read anything back.
- **Shortcuts notifications pile up.** They cannot be cleared by the accessory.
- **No Bluetooth trigger for automations.** iOS automations cannot fire on Bluetooth connect for a BLE-only device. Use Time of Day, NFC, Back Tap or a widget.
- **iPhone Focus modes** may delay or suppress ANCS. Untested.
- **Guard and tamper alerts are visual only** until a buzzer or motor is added.
- **Frozen prayer times drift** about up to a minute a day near the solstices. `RAFIQ sync` refreshes them.

---

## 9. Verification done for 6.0

- **Builds:**
  - patched core: 1,809,605 bytes (92%)
  - stock core: 1,799,193 bytes (91%)
  - 36 warnings, all pre-existing; zero from new code
- **Light-sleep symbols linked:** `esp_pm_configure`, `vApplicationSleep`, `esp_pm_impl_waiti`, `btdm_sleep_enter_phase1_wrapper`, `ble_gap_conn_rssi`, `usb_serial_jtag_is_connected`.
- **Images:** APP valid (checksum and hash), header "arduino-lib-builder". FULL has the bootloader at 0x0, partitions at 0x8000 and the app at 0x10000, byte-identical to the parts.
- **92 host tests** (`tools/v6`) over the real parser functions:
  - who counts as RAFIQ, including WhatsApp from "Rafiq", "Rafiqa", SMS and other shortcuts
  - the freshness window, the clock-skew allowance, no clock, malformed dates
  - run-once, including after a reboot
  - every command and their caps
  - multi-line and `;` payloads
  - both weather formats, °F, mph, and the icon mapping
- **13 pixel-accurate previews** with zero overflows: guard (both blink phases), find, tamper countdown, hotspot card, status card (normal and longest), tamper log, Wireless and System rows, and the worst-case weather age lines.

---

## 10. Release checklist (6.x)

1. Patch-core build: no new warnings, under 95%. Stock-core build also succeeds.
2. `tools/v6` tests pass. Previews for any changed screen, with no overflow.
3. On hardware:
   - first boot after 5.20 lands on Bluetooth and keeps all data
   - pair, then confirm clock and notifications
   - screen dark with the phone linked: it stays linked, and a notification wakes it
   - walk away: guard alert, then deep sleep 60 s later
   - `RAFIQ sync` refreshes and returns to Bluetooth
   - `RAFIQ update` offers the newest release and asks
   - manual WiFi survives a deep-sleep wake and ends on reset
   - hotspot install from a file
   - tamper both ways (with and without INT1), then disarm on reset
4. **Measure current** in light sleep with the phone linked (battery lead, USB unplugged).
5. Bump `FW_VERSION`; update the README and this file.

---

## 11. Open questions for the user

1. **Hardware test results** for everything in the release checklist, especially the light-sleep current and whether the guard thresholds (-88 / -80 dBm) suit a bag.
2. **Is INT1 (GPIO4) wired** on this unit? It decides which tamper mode runs.
3. **Buzzer or motor:** which part, and does GPIO10 suit?
4. **GitHub releases:** build them locally with the patch from now on?
5. **Mac app BLE migration:** next step when ready.
6. **NimBLE version** on the user's own Arduino IDE. 6.0 was built with 2.5.1.

---

## 12. Ideas not built

- A GATT control service (state notify, command write) for the Mac and Windows apps, mirroring `/api/*`.
- AMS music control, iPhone battery (as C3 Buddy has).
- On-device prayer calculation from saved latitude and longitude, removing the need to refresh at all.
- Buzzer patterns for the guard, tamper and find features.
- BLE OTA through the Mac app.

---

## 13. Honest status

Nothing in 6.0 has run on a real Rafiq yet. Everything above "verification done" is build-, host- and render-level. The riskiest paths on hardware are:
- repeated `bleOff()` / `bleOn()` cycles around syncs (NimBLE deinit and reinit)
- light sleep with the touch pad and I2C polling
- the deep-sleep tamper path
- the ANCS request with a 400-byte message and a date attribute (iOS should honour it; C3 Buddy does the same with 480)

If the device misbehaves, safe mode (3 crashes in a row) still brings up WiFi or the hotspot for an update.

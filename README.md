# NEXUS Face

An ESP32-C3 desk companion. A 0.96" OLED, two accelerometers, animated
robot eyes, a real clock, live weather, and its own WiFi hotspot with a
control panel. It updates itself from the releases here.

## Driving it

Everything is done by knocking on the case.

| | |
| --- | --- |
| 1 knock | next screen, or next item once you are inside |
| 2 knocks | go in |
| 3 knocks | come back out |

Screens: **HOME, FOCUS, WEATHER, PRAYER, MESSAGES, STORY, SETTINGS, SYSTEM.**

In **SETTINGS**, two knocks opens the list, one knock walks it, two knocks
opens the item, and then one knock changes the value. Three knocks steps
back out. Brightness, sleep timeout, popup time, eye style, a fresh story,
**Check update** and reboot are all reachable without touching a phone.

Knock it while it is asleep and it wakes. Drop it and it falls flat.

## Hardware

| | |
| --- | --- |
| Board | ESP32-C3 |
| Display | SSD1306 0.96", I2C `0x3C` |
| Motion | ADXL345 `0x53` and/or MPU6050 `0x68` |
| SDA | GPIO 8 |
| SCL | GPIO 9 |

Both sensors are optional and it uses whichever answers. The ADXL345 is
worth having: its tap and free-fall detection are done in hardware, so
knocks and drops are caught reliably.

## Screens

**HOME** the time, with the day and date. Nothing else.
**FOCUS** a countdown for whatever you are working on, with the task
scrolling along the bottom.
**WEATHER** an icon, the temperature, humidity and wind.
**PRAYER** all five times, with the next one picked out.
**MESSAGES** whatever you sent from the page.
**STORY** a short story written by gpt-4o-mini, turning its own pages.
**SYSTEM** knock counts, falls, boots, memory, signal, address.
**SETTINGS** everything you can change from the device itself.
**SYSTEM** memory, uptime, knocks, falls, signal and address.

## Working together

The page has two halves. **Let's work together** is the one you use day to
day: build a plan out of stretches, each with a name and a length, and the
clock runs them in order.

```
Write the spec    10 min
Review PRs        10 min
Break              5 min
Refactor parser   15 min
```

Press Start and the screen fills with a countdown, the task name scrolling
underneath. Each stretch flashes **COMPLETED** as it ends and hands over to
the next. Name one "break" and it flashes **BREAK OVER** instead, and after
thirty minutes of unbroken work it will tell you to **TAKE A BREAK** and go
walk for a minute whether you planned one or not. The plan is kept in flash,
so it survives a reboot.

**Configuration** is everything else: screens, weather, prayer times, system,
the OpenAI key and the network.

## Network

It joins your WiFi and serves one page at its address on your network.

If it ever cannot get on, it raises a rescue hotspot so the page is still
reachable and you can fix the credentials without a cable:

```
NEXUS-RESCUE / password  ->  http://192.168.4.1
```

## Stories

Put an OpenAI key in the Configuration tab and the STORY screen fills
itself from **gpt-4o-mini**: long, warm romances of around eight hundred
words, with Muslim names. It refreshes every six hours or on demand.

The story screen shows a couple of lines as a cover. **Two knocks** opens
it, **one knock** turns each page, **three knocks** closes it. While you
are reading, the idle timeout stretches to two minutes so it will not doze
off mid sentence.

You can also paste your own text on the page and it is wrapped and stored
exactly the same way, up to about 3400 characters, or fifty pages.

Everything is wrapped into lines the moment it arrives, so turning a page
costs nothing. The story survives a reboot. The key lives in flash and
never appears in the published binary.

The clock syncs over NTP. If it has no route to a time server, the panel
quietly hands over your phone's own clock and timezone as soon as you open
it, so the time is right regardless.

## Flashing

Download `nexus_face.bin` from [Releases](../../releases) and flash at
`0x10000`, or build from source:

1. Install **Adafruit GFX**, **Adafruit SSD1306** and **FluxGarage RoboEyes**.
2. Put your WiFi details in `DEF_WIFI_SSID` / `DEF_WIFI_PASS` at the top.
3. Set **Tools > Partition Scheme > Minimal SPIFFS (1.9MB APP with OTA)**.
   Updates need a spare app slot; schemes without one cannot self-update.
4. Upload.

Credentials are copied into flash on first boot and read from there
afterwards, so an update never wipes them. The published binary carries
placeholders, never anyone's password.

## Updates

**SETTINGS > Check update**, three knocks. It asks GitHub for the latest
release, compares it with the version it is running, and installs it with a
progress bar if it is newer. There is a button on the panel too.

To publish one: bump `FW_VERSION`, then

```bash
git tag v1.0.1 && git push origin v1.0.1
```

Actions builds it, checks the tag matches `FW_VERSION`, and attaches
`nexus_face.bin` to the release.

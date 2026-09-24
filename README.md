# NEXUS Face

An ESP32-C3 desk companion. A 0.96" OLED, two accelerometers, animated
robot eyes, a real clock, live weather, and its own WiFi hotspot with a
control panel. It updates itself from the releases here.

## Driving it

Everything is done by knocking on the case.

| | |
| --- | --- |
| 1 knock | next thing on this screen |
| 2 knocks | next screen |
| 3 knocks | do the selected thing |
| 4 knocks | straight back to HOME |

Screens: HOME, CLOCK, WEATHER, MESSAGES, FACE, SENSORS, SETTINGS, SYSTEM.

In **SETTINGS** one knock moves down the list and three knocks change the
value, so brightness, sleep timeout, eye style, **Check update** and reboot
are all reachable without a phone.

Tilt the board and the eyes follow. Shake it and it gets cross. Knock it
while it is asleep and it wakes. Drop it and it falls over.

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

## Network

It joins your WiFi **and** runs its own hotspot at the same time, so the
panel is reachable either way.

```
hotspot   NEXUS-ROBOT / password
panel     http://192.168.4.1   (or its address on your network)
```

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

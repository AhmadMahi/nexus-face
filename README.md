# RAFIQ

*rafiq* is Arabic for companion. A desk companion built on an ESP32-C3. It has a face, it knows the time,
the weather and the prayer times, it keeps a shelf of short reads, and it
carries the Quran, the 99 Names and the morning and evening adhkar in its
own firmware so those work with no network at all.

You drive it by knocking on the desk beside it.

## Knocking

One rule, everywhere:

| Knocks | On a card | In a list | In a reader |
|--------|-----------|-----------|-------------|
| 1 | next screen | next item | next page |
| 2 | go in | open it | back to the list |
| 3 | home | back out | back to the carousel |
| 4 | home | reload | reload |

Reload only means something on the shelf of short reads, where it writes a
new one, and on the zikr counter, where it starts the hundred again.

## Screens

```
HOME -> FOCUS -> WEATHER -> MESSAGES -> PRAYER -> FAITH -> SHORT READS -> GAMES -> SETTINGS -> SYSTEM
```

**HOME** the time, the day and the date. With no network there is no clock
to show, so it runs a stopwatch instead. Two knocks restart it.

**FOCUS** a plan of timed stretches, set on the page. Each one counts down,
flashes when it is done and hands over to the next. Half an hour of work
without a pause and it asks you to walk for a minute.

**WEATHER** from Open-Meteo, located by IP.

**MESSAGES** whatever was last sent from the page.

**PRAYER** five times from AlAdhan, with the next one picked out. They are
kept in flash, so they are still there with no network.

Calculated times and the local masjid rarely agree, so each prayer carries
its own correction in minutes, set on the page and kept in flash.

The call comes in three steps. Ten minutes out it says a word. Five minutes
out it says it again and begins to flash. On the minute itself it flashes
for a minute and then leaves you alone. Each step fires once a day.

**FAITH** two knocks opens five things:

- **Zikr** thirty three SubhanAllah, thirty three Alhamdulillah, thirty
  three Allahu Akbar and one to round it to a hundred. It paces itself.
- **99 Names** one large name per knock, with its meaning.
- **Quran** all 114 chapters. Each one gives its name and meaning, whether
  it was revealed in Makkah or Madinah, its verse count and a short note on
  what it covers. Descriptive notes, not tafsir.
- **Morning adhkar** and **Evening adhkar**.

All of it is compiled into the firmware, so none of it needs a network.

**SHORT READS** a shelf of stories, newest first. Two knocks opens the
shelf, two more opens one, one knock turns each page. Four knocks writes a
new one and fills the rest of the shelf quietly afterwards. You can also
paste your own on the page. They live in the filesystem and survive a
reboot and an update.

**GAMES** six, all played by tilting, two to a page. One knock steps
through them and the page turns itself; the dots along the bottom say
where you are.

- **Snake** tilt to steer. Works up from 220ms a step to 120ms.
- **Brick** tilt to slide the paddle, whose edges send the ball away at an
  angle so you steer with it as well as block. Five wall patterns that
  cycle as the levels climb.
- **Car** three lanes. Obstacles are spaced so only one lane is ever
  deadly at a time, and never spawn into a lane that would close the last
  gap: hard, never unavoidable.
- **Catch** tilt the basket. Keep the squares, let the crosses through.
- **Pong** tilt to return it, first to seven. The ball quickens on every
  return so a rally always ends.
- **Roll** the one that leans hardest on the sensor: the ball carries
  momentum, so you lead it and catch it again. Every level is flood filled
  before it is used, so the goal is always reachable.

While playing: two knocks pause, three leave, four start again. Best scores
are kept in flash.

Tilt only means anything relative to how the device is sitting, and this one
could be upright or flat. So the first time you play it asks once: hold
still, tilt right, tilt away. That mapping is kept in flash. Four knocks on
the game list forgets it and asks again. The resting position is measured
afresh every time a game starts, since that is what changes when you move
it.

**SETTINGS** brightness, sleep timeout, page turn (by knock or automatic),
popup time, eye style, hotspot, check for update, reboot.

**SYSTEM** uptime, network, memory and address. The full detail lives on
the page.

## The page

It runs whenever the device is on its network, at the address shown on the
SYSTEM screen. Two tabs: the work session, messages and the shelf on one,
and everything configurable on the other.

## Credentials

Nothing secret is ever committed or published. The binary attached to a
release carries placeholders only. Real credentials live in a gitignored
`secrets.h` when you build locally, and on the device itself in NVS, where
an update cannot reach them. Set the network from the page, or from the
hotspot in SETTINGS if the device cannot get online.

The OpenAI key, used only for writing short reads, is entered on the page
and stored the same way.

## Building

Board ESP32-C3, partition scheme **Minimal SPIFFS (1.9MB APP with OTA)**.
The OTA slot and the 128 kB filesystem both depend on it.

Libraries: Adafruit GFX, Adafruit SSD1306, FluxGarage RoboEyes, ArduinoJson.

Wiring, all on one I2C bus: SDA GPIO8, SCL GPIO9. OLED 0x3C, ADXL345 0x53,
MPU6050 0x68.

## Updating

SETTINGS, then Check update, then two knocks. It reads the latest release
here, shows the download moving, and restarts into it.

If it says **will not fit**, the second line gives the numbers, for example
`1352k into 1280k slot`. That means the board was flashed with a partition
table whose OTA slot is smaller than the build. Only USB can rewrite a
partition table, so connect it and upload once with **Minimal SPIFFS
(1.9MB APP with OTA)** selected. Settings and credentials live in NVS at a
fixed offset that both schemes share, so they survive.

The page shows the ceiling under System as **ota room**, so you can see how
much headroom is left before it becomes a problem.

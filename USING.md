# Using Rafiq

Everything you can do with it, and how. Start at [The pad](#the-pad); the
rest can wait until you want it.

- [The pad](#the-pad)
- [Knocking](#knocking)
- [The screens](#the-screens)
- [Watch faces](#watch-faces)
- [Settings](#settings)
- [Notifications and the radio](#notifications-and-the-radio)
- [Keyring mode](#keyring-mode)
- [Shortcuts from your phone](#shortcuts-from-your-phone)
- [The config page](#the-config-page)
- [Updating](#updating)
- [When something is wrong](#when-something-is-wrong)

---

## The pad

One pad on the front. How long you hold it is the whole language.

| Hold | Does |
|---|---|
| a tap | next screen, next item, next page |
| a hold | open what is in front of you |
| a longer hold | back out |
| **four seconds** | switch off |

Out of the box a hold is **0.7 seconds** and a longer hold is about
**1.5 seconds**. **Settings, Touch and motion, Hold time** moves the first
one anywhere from 0.5s to 2.5s, and the second follows it.

Asleep, it needs a deliberate hold to wake, not a brush. **Settings, Touch
and motion, Wake on hold** sets how long: `off` for any touch, `1s`, or
`3s`.

Taps also count. Two taps inside three seconds means one thing, three means
another, and what they mean depends on the screen, listed below.

## Knocking

Knock on the desk beside it. **Settings, Touch and motion, Knocks** turns
this on; it is off until you ask for it, because a desk gets knocked.

One rule everywhere:

| Knocks | On a card | In a list | In a reader |
|---|---|---|---|
| 1 | next screen | next item | next page |
| 2 | go in | open it | back to the list |
| 3 | home | back out | back to the carousel |
| 4 | home | reload | reload |

Reload only means something in two places: the shelf of short reads, where
it writes a new one, and the zikr counter, where it starts the hundred
again.

## The screens

```
HOME  VEHICLE  REMINDERS  MAC  WEATHER  NOTIFICATIONS  PRAYER  FAITH
SHORT READS  GAMES  SETTINGS  SYSTEM
```

| Screen | What it is |
|---|---|
| **HOME** | the time, the day, the date, and your watch face. No clock yet means a stopwatch instead |
| **VEHICLE** | service and document dates for a bike or car, set on the config page |
| **REMINDERS** | up to twenty, each with its own repeat. It nags every minute until you answer |
| **MAC** | what your Mac is doing: battery, the top three tasks, what is playing |
| **WEATHER** | temperature, conditions and wind. Synced from the phone or the Mac |
| **NOTIFICATIONS** | what arrived from the phone, newest first |
| **PRAYER** | five times with the next one picked out, each with its own correction in minutes. Friday midday is named Jumuah |
| **FAITH** | the tasbih counter, the 99 Names, and the morning and evening adhkar. All carried in the firmware, so no network is needed |
| **SHORT READS** | a shelf of stories. Four knocks writes a new one |
| **GAMES** | six, played by tilting: Snake, Brick, Car, Catch, Pong and Roll |
| **SETTINGS** | six groups, below |
| **SYSTEM** | uptime, network, memory, address, firmware version |

Three hubs sit alongside these and are opened from a Shortcut rather than
by walking the list: **TODAY**, **FAITH** and **CALM**.

### Prayer alerts

The call comes in three steps, each firing once a day. Ten minutes out it
says a word. Five minutes out it says it again and begins to flash. On the
minute itself it flashes for a minute and then leaves you alone.

### Games and tilt

Tilt only means anything relative to how the thing is sitting, and this one
could be upright or flat. The first time you play it asks once: hold still,
tilt right, tilt away. That mapping is kept. Four knocks on the game list
forgets it and asks again.

While playing: two knocks pause, three leave, four start again.

## Watch faces

Twenty one, for the clock screen only.

| | |
|---|---|
| laid out by hand | classic, stacked, date up, minimal, side, banner |
| they lean as you lean | drift, parallax |
| something pours | water, sand |
| hands | dial, bauhaus, regulator |
| what it knows about itself | rings, infograph, status, vitals, bars |
| showing off | terminal, binary |
| the other calendar | hijri |

**Two knocks on the clock** moves to the next one and keeps it. Or
**Settings, Display, Watch face**.

**terminal** is the one to pick when you want to see what is actually going
on. It prints the time, the date, the network, the signal, the battery and
the uptime as six lines. When the radio is off it says so, in those words:
`net   bluetooth off`.

## Settings

Six groups. Tap to move, hold to open.

**1. Display**

| Row | Choices |
|---|---|
| Brightness | 10%, then quarters |
| Watch face | the twenty one above |
| Clock | 24 hour or 12 hour |
| Sleep after | 5s to 10 min, or never |
| Popup time | off, 5s to 60s |
| Page turn | by knock, or automatic |
| Eye style | round, square, wide, sleepy, joy, cyclops |
| Vehicle | the VEHICLE screen on or off |

**2. Phone and keyring**

| Row | Choices |
|---|---|
| **Notifications** | **on, linked** or **off, daily**. This decides the radio. See below |
| **Keyring mode** | off, or on. See below |
| **Card** | which line it says when somebody picks it up |
| Network | Bluetooth, WiFi, or off. WiFi here is temporary: it ends at the next real restart, or after half an hour unused |
| Multi-link | whether a phone and a Mac can both be connected |
| Primary, Second | which devices those are |
| Pair a Mac | hold to pair |
| Hotspot | hold to raise the config page |

**3. Touch and motion**

| Row | Choices |
|---|---|
| Wake by | touch, movement, or both |
| Pocket lock | ignores the pad while it is in a pocket |
| Wake on hold | instant, 1s, 3s |
| Hold time | how long a hold has to be |
| Go back by | knock, shake, or both |
| Knocks | on or off |
| Tap strength | ultra, light, medium, hard |
| Accelerometer | hold for a live view |
| Motion sensor | 25 Hz or 100 Hz. 25 is kinder to the battery |

**4. Away and safety**

| Row | Choices |
|---|---|
| Auto away | goes quiet by itself when you leave |
| Phone guard | calls out if your phone goes out of range |
| Tamper alarm | arms it. Any movement is logged |
| Tamper log | hold to read it |

**5. Battery**

| Row | Choices |
|---|---|
| Battery use | hold for the history |
| Battery log | on or off |
| New log at | the voltage that starts a fresh log |
| **Battery full** | 3.95V to 4.25V in steps of 0.05. Set what yours reads when full and the whole percentage curve scales to it |
| Power down | 1m, 2m, 5m, 10m, 30m, never |
| **Processor** | auto, 80, 160 or 240 MHz. 80 is the quiet one |
| Night sleep | off at night |
| Bedtime | when that is |

**6. System**

Update, Reset settings, Reboot, About.

Reset asks before it does anything, and **your networks are kept**. It is
the settings that go back, not the credentials.

**Prayer settings** is a seventh group, opened from the FAITH screen rather
than from this list, because that is where you are when you want it.

## Notifications and the radio

**One setting decides both.** Settings, Phone and keyring, Notifications.

| Notifications | What the radio does |
|---|---|
| **on, linked** | Bluetooth comes up at boot and stays connected to your phone. Notifications arrive, Shortcuts reach it, the clock stays right |
| **off, daily** | Bluetooth comes up at boot, takes the time, and goes. It comes back once a day for a few seconds to take the time again, then goes. The rest of the day the radio is not running |

The second one is the long battery. The C3 has no crystal, so the clock
drifts, which is the only reason it comes back at all.

`notifications on` and `notifications off` from a Shortcut do the same
thing as the row.

**WiFi is never on.** Not at boot, not on a schedule, not as a fallback. It
comes up only when you ask for it by name: Settings, Hotspot, or
`RAFIQ config`, `RAFIQ wifi`, `RAFIQ sync`, `RAFIQ update`. It goes away
again by itself.

## Keyring mode

**Settings, Phone and keyring, Keyring mode.** It asks before it starts,
because getting out of it is deliberately hard.

What it becomes:

1. **Ten seconds of notice** on the screen, counting down. Both ways out
   are open here: hold the pad for four seconds, or send `home`.
2. **The screen goes dark** but the radio stays up for **two minutes**,
   listening for one word. Send `home` from a Shortcut in that time and it
   comes back to normal. Picking it up in this window shows the card.
3. **After two minutes it is a keyring.** Both radios off, 80 MHz, deep
   sleep. Pick it up or shake it and it opens its eyes, says one line, and
   goes straight back to sleep. No menus, no clock, no animation, nothing.

**Switching it off and on again starts at step 1.** That is the way back
in: power cycle it, then send `home` within two minutes. Nothing else gets
you out, and nothing you do to it in step 3 gets you out either.

The cards, chosen in **Card**:

| Card | Says |
|---|---|
| contact | your name, your number, "if found, call me" |
| don't touch | "Do not touch me / I am very busy" |
| handsome | "Oh, hello / and who are you?" |
| lost | "Lost? / your number / that is my human" |
| keyring | "Just a keyring / nothing to see" |
| sleepy | "Shhh / I am asleep" |

There is a seventh you cannot choose. Poke it four times inside eight
seconds and it says **"Stop it / I am getting dizzy"** instead, with angry
eyes. That counter survives sleep, so four pokes means four pokes however
long it was switched off in between.

## Shortcuts from your phone

Make a Shortcut named **RAFIQ** and send it a line. Anything below works.

| | |
|---|---|
| **getting around** | `home`, `status`, `find me`, `where are you` |
| **notifications** | `notifications on`, `notifications off`, `quiet`, `silent` |
| **the clock and data** | `sync`, `prayer refresh`, `check update` |
| **the radio** | `bluetooth`, `wifi`, `hotspot`, `config` |
| **power** | `sleep`, `deep sleep`, `power off`, `reboot` |
| **faith** | `zikr`, `tasbih`, `dhikr` |
| **calm** | `relax` |
| **night** | `night later`, `night +1` |
| **waking** | `wake touch`, `wake move`, `wake both` |
| **safety** | `guard on`, `guard off`, `tamper on` |
| **timers** | `timer 15`, `timer +5`, `timer -10` |
| **the watch face** | `face 7` |
| **away** | `away`, `away: back at four` |

`home` is the one worth putting on your lock screen. It takes you back to
the home screen, ends Away, and is the only way out of keyring mode.

## The config page

The only thing that raises WiFi.

**Settings, Phone and keyring, Hotspot**, or `RAFIQ config`. It puts up a
network called `RAFIQ-SETUP` and serves a page at `192.168.4.1`. The
hotspot stays up until ten minutes after the last phone leaves.

On the page: your WiFi networks, reminders, the vehicle dates, the prayer
corrections, notification filters and VIPs, the short reads shelf, your
name and greeting, and firmware upload.

## Updating

Three ways, all of them ending in the same place.

| How | What to do |
|---|---|
| **from the Rafiq app** | Check updates, then Update. Over Bluetooth or WiFi |
| **from the robot** | Settings, System, Update, then hold. It offers the newest release, or lists the last eight so you can put an older one back |
| **from a file** | the config page, Upload firmware, pick the APP `.bin` |

If it says **will not fit**, the second line gives the numbers, for example
`1352k into 1280k slot`. The board was flashed with a partition table whose
OTA slot is smaller than the build. Only USB can rewrite a partition table:
connect it and upload once with **Minimal SPIFFS (1.9MB APP with OTA)**
selected. Settings and credentials live in NVS and survive that.

## When something is wrong

| It does this | Try this |
|---|---|
| the pad does nothing | it relearns its resting level a minute after power on. Give it a minute without touching it |
| it says RESTARTED: CRASH | three crashes in a row and it comes up in safe mode on WiFi, so it can be updated |
| the clock is wrong | `RAFIQ sync`, or switch Notifications on so it holds a link |
| the battery reads low | Settings, Battery, Battery full. If it still reads low, add 100nF from GPIO1 to ground, see [WIRING.md](WIRING.md) |
| nothing on the screen | four seconds on the pad switches it off; hold again to bring it back |
| it will not come out of keyring mode | switch it off, switch it on, and either hold the pad for four seconds in the first ten, or send `home` within two minutes |

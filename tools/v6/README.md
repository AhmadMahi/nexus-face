# 6.0 tooling

| File | What |
|---|---|
| `patch_v6*.py` | The 6.0 change as five scripted edits over 5.20.0. Each anchor must match exactly once or it stops. Kept so the change can be reviewed, or replayed onto a different 5.x. |
| `extract_rafiq.py` | Copies the real RAFIQ parser functions out of `nexus_face.ino`. |
| `test_rafiq.cpp` | 88 host tests over those functions. |
| `gfx.py`, `screens.py` | Pixel-accurate previews of the new screens using Adafruit GFX's own font bitmaps. Fails loudly on any text past the edge. Copy `glcdfont.c` from Adafruit GFX next to it. |

```
python3 extract_rafiq.py utcFromTm rqSkip rqTagged rqAfterTag rafiqIs rqFresh rqFnv \
  rqRanBefore rqRemember wxCodeFromWords rqNum rqWeather rqCommand rafiqPayload rafiqNote > fw_funcs.inc
g++ -std=gnu++17 -w -o t test_rafiq.cpp && ./t
```

`conn_check.py` reads every `updateConnParams` call out of the `.ino` and
measures it against Apple's rules. A central refuses a request that breaks
them and says nothing, so the link simply stays slow; that is how sending
firmware over Bluetooth ran at the ordinary rhythm for so long.

`pm_check.py` asks whether the core this will build against is the patched one.
Without the patch `esp_pm_configure` fails, `pmAvail` is false, `phoneHeld()`
returns false whatever is connected, and the robot deep-sleeps the moment the
screen darkens and never light-sleeps. It builds and runs either way, which is
how three releases went out on a stock core. Run it before any release build.

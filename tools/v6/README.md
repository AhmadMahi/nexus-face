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

## Running the checks (7.10)

```
zsh run_tests.sh                 # 125 host tests; the function list lives here,
                                 # not in the block above, which is the 6.0 one
python3 geniconsjson.py          # icons.json, straight from the .ino's IC_* table
python3 ui_check.py              # every screen measured; 0 problems expected
```

`icons.json`, `fw_funcs.inc`, `t` and `screens.png` are all generated and are
not kept in the repository. The icon table used to be a hand-kept copy, which
is a layout checker drawing yesterday's icons.

"""icons.json, generated from the firmware's own icon table.

The checker used to need a hand-kept copy. A hand-kept copy drifts,
and a layout checker drawing yesterday's icons is worse than none.
This reads the IC_* arrays straight out of the .ino every run.
"""
import re, json, sys, os
ino = os.path.join(os.path.dirname(__file__), "..", "..", "nexus_face", "nexus_face.ino")
src = open(ino).read()
out = {}
for m in re.finditer(r"static const uint8_t IC_(\w+)\[8\]\s*=\s*\{([^}]*)\};", src):
    name = "IC_" + m.group(1)
    vals = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2))]
    if len(vals) != 8:
        print(f"IC_{m.group(1)} has {len(vals)} bytes, not 8", file=sys.stderr); sys.exit(1)
    out[name] = vals
json.dump(out, open(os.path.join(os.path.dirname(__file__), "icons.json"), "w"), indent=1)
print(f"{len(out)} icons")

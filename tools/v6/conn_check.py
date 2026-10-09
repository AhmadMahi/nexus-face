"""Every connection parameter request, against Apple's rules.

A central may refuse a request that breaks them, and the refusal is
silent: the link simply stays as it was. That is not a crash, not a log
line and not anything you can see, so the only sign is that something
feels slow. Sending firmware over Bluetooth asked for 7.5 to 15 ms,
which breaks all three of the interval rules, and so ran at the
ordinary rhythm the whole way.

Rules, from Apple's Accessory Design Guidelines. They have moved over
the years, so these are the ones every release agrees on:

  Interval Min >= 15 ms
  Interval Min is a multiple of 15 ms
  Interval Max >= Interval Min + 15 ms   (or both are exactly 15 ms)
  Peripheral Latency <= 30
  Interval Max * (Latency + 1) <= 6 s
  Supervision Timeout > Interval Max * (Latency + 1) * 3
  Supervision Timeout in 6 s to 18 s
"""
import re, sys, os

ino = os.path.join(os.path.dirname(__file__), "..", "..", "nexus_face", "nexus_face.ino")
src = open(ino).read()

UNITS_MS = 1.25          # connection interval units
TIMEOUT_MS = 10.0        # supervision timeout units

calls = []
for m in re.finditer(r"updateConnParams\(\s*([^;]+?)\s*\)\s*;", src):
    # The first argument is the handle, whatever it is called. The four
    # that matter are the rest, and any of them may be a ternary, in
    # which case both branches are a real request and both are checked.
    parts = [p.strip() for p in m.group(1).split(",")]
    if len(parts) < 5: continue
    line = src[:m.start()].count("\n") + 1
    cols = []
    for p in parts[1:5]:
        nums = [int(x) for x in re.findall(r"\b(\d+)\b", p)]
        if not nums: break
        cols.append(nums)
    if len(cols) != 4: continue
    for v in range(max(len(c) for c in cols)):
        calls.append((line, [c[v] if v < len(c) else c[0] for c in cols]))

assert calls, "no updateConnParams calls found; the parser has gone stale"

bad = []
print()
for line, (mn, mx, lat, to) in calls:
    mnms, mxms, toms = mn * UNITS_MS, mx * UNITS_MS, to * TIMEOUT_MS
    why = []
    if mnms < 15:                       why.append(f"min {mnms:.1f}ms is under 15ms")
    if abs(mnms % 15) > 0.001:          why.append(f"min {mnms:.1f}ms is not a multiple of 15ms")
    if not (mxms >= mnms + 15 or (mnms == 15 and mxms == 15)):
        why.append(f"max {mxms:.1f}ms is not at least min+15ms")
    if lat > 30:                        why.append(f"latency {lat} is over 30")
    if mxms * (lat + 1) > 6000:         why.append("max * (latency+1) is over 6s")
    if toms <= mxms * (lat + 1) * 3 / 1000 * 1000:
        why.append(f"timeout {toms:.0f}ms is not more than 3x max*(latency+1)")
    if not (6000 <= toms <= 18000):     why.append(f"timeout {toms:.0f}ms is outside 6s to 18s")
    tag = "ok  " if not why else "FAIL"
    print(f"  {tag} line {line}: {mnms:6.1f} to {mxms:6.1f} ms, latency {lat}, timeout {toms/1000:.1f}s")
    for w in why:
        print(f"         {w}")
        bad.append((line, w))

print()
if bad:
    print(f"{len(bad)} problem(s): a central may refuse these, silently")
    sys.exit(1)
print(f"PASS: {len(calls)} requests, all within Apple's rules")

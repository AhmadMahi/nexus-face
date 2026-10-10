"""Is the core this will build against the patched one?

Without the light-sleep patch, esp_pm_configure fails, pmAvail is false,
and phoneHeld() returns false whatever is actually connected. The robot
then treats a phone in your pocket and a Mac on your desk as being
alone, and switches off the moment the screen darkens. It also never
light-sleeps, which is most of the battery.

None of that is an error. It builds, it runs, the serial log says so in
one line nobody reads, and the only sign is a robot that keeps going to
sleep. Three releases went out like that.

Run this before any release build.
"""
import os, sys, glob

# A path may be given, so the check can be pointed at a stock core to
# show it still fails on one.
CORE = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
    "~/Library/Arduino15/packages/esp32/tools/esp32c3-libs/3.3.10")
WANT = {
    "CONFIG_PM_ENABLE": "1",
    "CONFIG_FREERTOS_USE_TICKLESS_IDLE": "1",
    "CONFIG_BT_CTRL_MODEM_SLEEP": "1",
    "CONFIG_ESP_PHY_MAC_BB_PD": "1",
}

print()
if not os.path.isdir(CORE):
    print(f"  no core at {CORE}")
    sys.exit(1)

bad = []
for variant in ("dio_qspi", "qio_qspi"):
    h = os.path.join(CORE, variant, "include", "sdkconfig.h")
    if not os.path.exists(h):
        bad.append(f"{variant}: no sdkconfig.h"); continue
    text = open(h).read()
    for key, val in WANT.items():
        if f"#define {key} {val}" not in text:
            bad.append(f"{variant}: {key} is not {val}")
    print(f"  {variant}: " + ", ".join(
        f"{k}={'1' if f'#define {k} 1' in text else '0'}" for k in WANT))

print()
if bad:
    for b in bad: print("  FAIL", b)
    print()
    print("  This is the stock core. A build from it cannot light-sleep and will")
    print("  deep-sleep with the phone connected. Apply")
    print("  Rafiq_LightSleep_core_patch_3.3.10.zip over")
    print(f"  {CORE}")
    sys.exit(1)
print("PASS: the core is patched; light sleep is available")

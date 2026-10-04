#!/bin/zsh
# Compile the sketch exactly the way the IDE does: same core, same
# partition scheme, so the binary that goes out is the binary that was
# tested.
set -e
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
HERE="${0:A:h}"
"$CLI" compile \
  --fqbn esp32:esp32:esp32c3:PartitionScheme=min_spiffs,CDCOnBoot=cdc \
  --build-path "$HERE/build/nexus_face" \
  --output-dir "$HERE/out" \
  "$HERE/nexus-repo/nexus_face" "$@"

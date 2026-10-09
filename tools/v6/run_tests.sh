#!/bin/zsh
# The host tests, with the function list that actually works at 7.9.0.
#
# The README's list is the 6.0 one and no longer extracts: rafiqIs now
# calls rafiqTagged and rqFromShortcuts, and rqCommand calls
# tmrCommand, and the extractor emits in the order given, so a callee
# has to be named before its caller.
set -e
cd "${0:A:h}"
python3 extract_rafiq.py \
  utcFromTm rqSkip rqTagged rqAfterTag rafiqTagged rqFromShortcuts rafiqIs \
  rqFresh rqFnv rqRanBefore rqRemember wxCodeFromWords rqNum rqWeather \
  tmrCommand rqCommand rafiqPayload rafiqNote > fw_funcs.inc
g++ -std=gnu++17 -w -o t test_rafiq.cpp
./t

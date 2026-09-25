#!/bin/bash
# Builds Rafiq.app and a DMG, with nothing but the Command Line Tools.
set -euo pipefail
NAME="Rafiq"
APP="build/$NAME.app"
VER="1.2.1"

rm -rf build && mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

xcrun --sdk macosx swiftc -O -whole-module-optimization \
  -target arm64-apple-macos14.0 \
  -o "$APP/Contents/MacOS/$NAME" Sources/*.swift

# The command line helper rides along inside the bundle, but NOT in MacOS/:
# the disk is case insensitive, so "rafiq" there is the same file as the app
# binary "Rafiq" and silently replaces it.
xcrun --sdk macosx swiftc -O -target arm64-apple-macos14.0 \
  -o "$APP/Contents/Resources/rafiq" cli/rafiq.swift

cp Rafiq.icns "$APP/Contents/Resources/$NAME.icns"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>$NAME</string>
  <key>CFBundleDisplayName</key><string>$NAME</string>
  <key>CFBundleExecutable</key><string>$NAME</string>
  <key>CFBundleIdentifier</key><string>in.iotcart.rafiq</string>
  <key>CFBundleIconFile</key><string>$NAME</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VER</string>
  <key>CFBundleVersion</key><string>$VER</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <!-- lives in the menu bar, so no Dock icon and no window on launch -->
  <key>LSUIElement</key><true/>
  <key>NSHumanReadableCopyright</key><string>Ahmed  ·  iotcart.in</string>
  <!-- the clock speaks plain HTTP on the LAN. Allow local addresses only;
       arbitrary loads stay off, so this cannot reach the wider internet. -->
  <key>NSAppTransportSecurity</key>
  <dict><key>NSAllowsLocalNetworking</key><true/></dict>
  <key>NSLocalNetworkUsageDescription</key>
  <string>Rafiq sends your messages to the clock on your own network.</string>
</dict></plist>
PLIST

# arm64 binaries must carry at least an ad hoc signature to run at all
codesign --force --sign - --timestamp=none "$APP" 2>/dev/null
codesign --verify --deep --strict "$APP" && echo "signed (ad hoc)"

# --- the disk image ---
STAGE="build/stage"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
rm -f "build/$NAME-$VER.dmg"
hdiutil create -quiet -volname "$NAME" -srcfolder "$STAGE" \
  -ov -format ULFO "build/$NAME-$VER.dmg"
rm -rf "$STAGE"

echo
echo "  app  $(du -sh "$APP" | cut -f1)   $APP"
echo "  dmg  $(du -sh "build/$NAME-$VER.dmg" | cut -f1)   build/$NAME-$VER.dmg"

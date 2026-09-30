#!/bin/bash
# Build "TWIN Panel.app" from TwinApp.swift and put it in ~/Applications (or the directory given).
#   tools/twin/app/build.sh [DEST]
# swiftc only, no Xcode project. The app starts tools/twin/twin.py of THIS checkout (its path goes
# into Info.plist as TwinScript), so build again if the checkout moves.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SCRIPT="$(cd "$HERE/.." && pwd)/twin.py"
DEST="${1:-$HOME/Applications}"
APP="$DEST/TWIN Panel.app"
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

swiftc -O -o "$BUILD/TwinPanel" "$HERE/TwinApp.swift" -framework AppKit -framework WebKit

mkdir -p "$BUILD/TWIN Panel.app/Contents/MacOS"
cp "$BUILD/TwinPanel" "$BUILD/TWIN Panel.app/Contents/MacOS/TwinPanel"
cat > "$BUILD/TWIN Panel.app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key><string>com.nickoscope.twinpanel</string>
  <key>CFBundleName</key><string>TWIN — панель</string>
  <key>CFBundleExecutable</key><string>TwinPanel</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSAppTransportSecurity</key>
  <dict>
    <key>NSAllowsLocalNetworking</key><true/>
    <key>NSAllowsArbitraryLoadsInWebContent</key><true/>
  </dict>
  <key>TwinScript</key><string>$SCRIPT</string>
</dict>
</plist>
EOF
codesign --force --sign - "$BUILD/TWIN Panel.app" >/dev/null   # ad hoc: a local build, not distributed
mkdir -p "$DEST"
rm -rf "$APP"
cp -R "$BUILD/TWIN Panel.app" "$APP"
echo "$APP  (starts $SCRIPT)"

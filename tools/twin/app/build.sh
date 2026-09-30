#!/bin/bash
# Build "TWIN-NickoScopeMatrix-64x128.app": the virtual twin as a self-contained Mac app, and a .dmg of it.
#   tools/twin/app/build.sh [--no-install]
#
# The app is every .swift file here but icon.swift (a script of its own): main.swift (the window, the
# twin, top-level code - Swift allows it in main.swift only) and SyncEngine.swift (Sync with panel).
#
# Everything the twin needs goes inside the bundle (Contents/Resources): the esp32sim engine, the
# ESP32-S3 mask ROM, the eFuse word, the firmware release of docs/firmware/latest and the web pages
# (the engine's panel page, the project's web flasher with the twin's shim, as twin.py build_web makes
# them). Build-time inputs, as tools/twin/README.md sets them up: the engine checkout (TWIN_ENGINE,
# default ~/twin/esp32sim, built with cargo build --release), ~/twin/rom/esp32s3_rev0_rom.elf and
# ~/twin/efuse-opi.txt. Python and swiftc are needed only here, not by the app.
#
# Signing: SIGN_ID if set; else the first "Developer ID Application" identity (then the hardened
# runtime, a timestamp, and notarization if the keychain profile NOTARY_PROFILE, default
# "twin-notary", exists - `xcrun notarytool store-credentials twin-notary ...`, once, by the owner);
# else the first "Apple Development" identity (this Mac only); else ad hoc. The engine's JIT maps its
# code with MAP_JIT, which the hardened runtime allows only with com.apple.security.cs.allow-jit.
#
# The result: /Applications/TWIN-NickoScopeMatrix-64x128.app (unless --no-install) and
# ~/twin/dist/TWIN-NickoScopeMatrix-64x128-<app>-firmware-<ver>.dmg.
#
# For a test build beside the installed app: APP_OUT=<dir> also leaves the .app there, DIST=<dir> puts
# the .dmg there, and TWIN_BUNDLE_ID=<id> gives it another bundle identifier - so another settings
# domain (defaults), another data directory by default (~/Library/Application Support/<id>: its own
# flash, MAC and sync state, never the installed app's, and nothing copied in from ~/twin/state), its
# own key for the sync state in the Keychain, "TEST <id>" in its window's title, no Dock icon (a
# test build is quit by its PID when its test ends, never left beside the installed app), and a window that
# opens behind the others without taking the focus (main.swift buildWindow). A test that syncs
# with a twin playing the panel also passes -dataDir <dir> and -panelAddress 127.0.0.1:<port>: the
# test switches hold for nothing else (SyncEngine.swift).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
TWIN_DIR="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$TWIN_DIR/../.." && pwd)"
TWIN_HOME="${TWIN_HOME:-$HOME/twin}"
ENGINE="${TWIN_ENGINE:-$TWIN_HOME/esp32sim}"
ROM="$TWIN_HOME/rom/esp32s3_rev0_rom.elf"
EFUSE="$TWIN_HOME/efuse-opi.txt"
LATEST="$REPO/docs/firmware/latest"
DIST="${DIST:-$TWIN_HOME/dist}"
NOTARY_PROFILE="${NOTARY_PROFILE:-twin-notary}"
INSTALL=1; [ "${1:-}" = "--no-install" ] && INSTALL=0

VERSION="$(tr -d '[:space:]' < "$LATEST/VERSION")"                     # v2.7.7
IMAGE="$LATEST/AnimatedPixelClock-waveshare-$VERSION-Full.bin"
APP_VERSION="1.3"
NAME="TWIN-NickoScopeMatrix-64x128"
BUNDLE_ID="${TWIN_BUNDLE_ID:-com.nickoscope.TWIN-NickoScopeMatrix-64x128}"
APP_OUT="${APP_OUT:-}"
# A test build stays out of the Dock and the app switcher (LSUIElement), so it is never mistaken for
# the installed app and quit instead of it.
TEST_KEYS=""
[ "$BUNDLE_ID" != "com.nickoscope.TWIN-NickoScopeMatrix-64x128" ] && TEST_KEYS="<key>LSUIElement</key><true/>"
for f in "$ENGINE/target/release/esp32sim" "$ROM" "$EFUSE" "$IMAGE"; do
    [ -f "$f" ] || { echo "missing: $f (see tools/twin/README.md, Setup)" >&2; exit 1; }
done
(cd "$LATEST" && shasum -a 256 -c SHA256SUMS.txt >/dev/null) || { echo "$IMAGE: not the checksum in SHA256SUMS.txt" >&2; exit 1; }

BUILD="$(mktemp -d)"; trap 'rm -rf "$BUILD"' EXIT
APP="$BUILD/$NAME.app"; RES="$APP/Contents/Resources"
mkdir -p "$APP/Contents/MacOS" "$RES/engine" "$RES/rom" "$RES/firmware"

echo "== the app (swiftc)"
SOURCES=()
for f in "$HERE"/*.swift; do [ "$(basename "$f")" = icon.swift ] || SOURCES+=("$f"); done
swiftc -O -o "$APP/Contents/MacOS/TwinPanel" "${SOURCES[@]}" -framework AppKit -framework WebKit

echo "== the twin inside it: engine $(git -C "$ENGINE" rev-parse --short HEAD 2>/dev/null || echo '?'), firmware $VERSION"
cp "$ENGINE/target/release/esp32sim" "$RES/engine/esp32sim"
cp "$ROM" "$RES/rom/"
cp "$EFUSE" "$RES/efuse-opi.txt"
cp "$IMAGE" "$RES/firmware/merged.bin"
echo "$VERSION" > "$RES/firmware/VERSION"
cp "$HERE/NOTICE.md" "$RES/NOTICE.md"                             # the bundled works' licenses
# The local-network question macOS asks for Sync with panel, in both languages of the app.
mkdir -p "$RES/en.lproj" "$RES/ru.lproj"
printf '"NSLocalNetworkUsageDescription" = "Sync with panel finds the LED panel on your network (Bonjour) and talks to it and to the twin.";\n' > "$RES/en.lproj/InfoPlist.strings"
printf '"NSLocalNetworkUsageDescription" = "Синхронизация с панелью находит LED-панель в вашей сети (Bonjour) и обменивается данными с ней и с двойником.";\n' > "$RES/ru.lproj/InfoPlist.strings"
# The web pages, made by twin.py's own build_web, so the page and the flasher are the ones twin.py serves.
TWIN_HOME="$TWIN_HOME" TWIN_ENGINE="$ENGINE" python3 -c "import sys; sys.path.insert(0, '$TWIN_DIR'); import twin; twin.build_web('$BUILD/web')" >/dev/null
cp -R "$BUILD/web" "$RES/web"

echo "== the icon"
swift "$HERE/icon.swift" "$BUILD/icon.png"
ICONSET="$BUILD/AppIcon.iconset"; mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
    sips -z $s $s "$BUILD/icon.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    sips -z $((s * 2)) $((s * 2)) "$BUILD/icon.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$RES/AppIcon.icns"

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
  <key>CFBundleName</key><string>$NAME</string>
  <key>CFBundleDisplayName</key><string>$NAME</string>
  <key>CFBundleExecutable</key><string>TwinPanel</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$APP_VERSION</string>
  <key>CFBundleVersion</key><string>$APP_VERSION.$(date +%Y%m%d%H%M)</string>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleLocalizations</key><array><string>en</string><string>ru</string></array>
  <key>LSApplicationCategoryType</key><string>public.app-category.developer-tools</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSHumanReadableCopyright</key><string>Firmware: NickoScope/AnimatedPixelClock. Engine: esp32sim (MIT), fork NickoScope/TWIN-NickoScopeMatrix-64x128.</string>
  <key>NSLocalNetworkUsageDescription</key><string>Sync with panel finds the LED panel on your network (Bonjour) and talks to it and to the twin.</string>
  <key>NSBonjourServices</key><array><string>_http._tcp</string></array>
  <key>NSAppTransportSecurity</key>
  <dict>
    <key>NSAllowsLocalNetworking</key><true/>
    <key>NSAllowsArbitraryLoadsInWebContent</key><true/>
  </dict>
  <key>TwinFirmware</key><string>$VERSION</string>
  $TEST_KEYS
</dict>
</plist>
EOF

cat > "$BUILD/engine.entitlements" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.cs.allow-jit</key><true/></dict></plist>
EOF

echo "== signing"
ids="$(security find-identity -v -p codesigning 2>/dev/null)"
SIGN="${SIGN_ID:-}"
if [ -z "$SIGN" ]; then
    SIGN="$(echo "$ids" | grep -o '"Developer ID Application: [^"]*"' | head -1 | tr -d '"')" || true
    [ -z "$SIGN" ] && SIGN="$(echo "$ids" | grep -o '"Apple Development: [^"]*"' | head -1 | tr -d '"')" || true
    [ -z "$SIGN" ] && SIGN="-"
fi
case "$SIGN" in "Developer ID"*) DIST_SIGNED=1; TS="--timestamp" ;; *) DIST_SIGNED=0; TS="" ;; esac
RT="--options runtime"; [ "$SIGN" = "-" ] && RT=""
codesign --force --sign "$SIGN" $RT $TS --entitlements "$BUILD/engine.entitlements" "$RES/engine/esp32sim"
codesign --force --sign "$SIGN" $RT $TS "$APP"
codesign --verify --deep --strict "$APP"
echo "signed with: ${SIGN/(*)/(…)}"
if [ -n "$APP_OUT" ]; then
    mkdir -p "$APP_OUT"; rm -rf "$APP_OUT/$NAME.app"; cp -R "$APP" "$APP_OUT/$NAME.app"
    echo "$APP_OUT/$NAME.app"
fi

if [ "$INSTALL" = 1 ]; then
    echo "== installing"
    rm -rf "$HOME/Applications/TWIN Panel.app" "/Applications/TWIN Panel.app"   # the earlier builds' name
    rm -rf "/Applications/$NAME.app"
    cp -R "$APP" "/Applications/$NAME.app"
    echo "/Applications/$NAME.app"
fi

echo "== the disk image"
mkdir -p "$DIST"
DMG="$DIST/$NAME-$APP_VERSION-firmware-$VERSION.dmg"
STAGE="$BUILD/dmg"; mkdir -p "$STAGE"; cp -R "$APP" "$STAGE/"; ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"
hdiutil create -volname "$NAME" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
[ "$SIGN" != "-" ] && codesign --force --sign "$SIGN" $TS "$DMG"
if [ "$DIST_SIGNED" = 1 ] && xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null 2>&1; then
    echo "== notarizing (Apple)"
    xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$DMG"
    [ "$INSTALL" = 1 ] && xcrun stapler staple "/Applications/$NAME.app" || true
else
    echo "(not notarized: needs a Developer ID Application certificate and the keychain profile $NOTARY_PROFILE)"
fi
echo "$DMG"

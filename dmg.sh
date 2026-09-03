#!/bin/bash
# Build a Finder-launchable DMG: drag CoPrintSlicer.app to Applications and open it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

ARCH="$(uname -m)"
PACKAGED="$ROOT/build/${ARCH}/CoPrintSlicer/CoPrintSlicer.app"
DEV_APP="$ROOT/build/${ARCH}/src/Release/CoPrintSlicer.app"

if [ -d "$PACKAGED" ] && [ ! -L "$PACKAGED/Contents/Resources" ]; then
    APP_SRC="$PACKAGED"
else
    APP_SRC="$DEV_APP"
fi

if [ ! -d "$APP_SRC" ]; then
    echo "error: no app bundle found. Build first with ./build_release_macos.sh -s" >&2
    echo "  looked for:" >&2
    echo "    $PACKAGED" >&2
    echo "    $DEV_APP" >&2
    exit 1
fi

DMG_TEMP="$ROOT/dmg_temp"
APP_DST="${DMG_TEMP}/CoPrintSlicer.app"
rm -rf "$DMG_TEMP"
mkdir -p "$DMG_TEMP"

"$ROOT/scripts/macos_package_app.sh" "$APP_SRC" "$APP_DST"

ln -s /Applications "$DMG_TEMP/Applications"

DMG_OUT="$ROOT/CoPrintSlicer.dmg"
rm -f "$DMG_OUT"
hdiutil create -volname "CoPrintSlicer" -srcfolder "$DMG_TEMP" -ov -format UDZO "$DMG_OUT"

rm -rf "$DMG_TEMP"
echo "DMG ready: $DMG_OUT"
echo "Test like a user: open the DMG, drag CoPrintSlicer to Applications, then open it from Applications."

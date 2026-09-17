#!/usr/bin/env bash
# Turn a development CoPrintSlicer.app (Resources often a symlink into the
# source tree) into a Finder-launchable bundle: real Resources, Info.plist
# keys macOS 26 expects, then code-sign it.
#
# Signing identity is controlled by the CODESIGN_IDENTITY env var:
#   - unset / "-"  -> ad-hoc signature (default). Free, instant, only good for
#                     local/dev use; Gatekeeper will warn on other Macs unless
#                     the quarantine xattr is stripped (see xattr -cr below).
#   - "Developer ID Application: <Name> (<TEAMID>)" -> real signing with a
#                     Developer ID cert (requires Apple Developer Program).
#                     Automatically enables hardened runtime + timestamp,
#                     which is required for notarization (see notarize_dmg.sh).
#
# Usage: macos_package_app.sh <src.app> <dst.app>
set -euo pipefail

CODESIGN_IDENTITY="${CODESIGN_IDENTITY:--}"
ENTITLEMENTS="$(cd "$(dirname "$0")" && pwd)/disable_validation.entitlements"

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <src.app> <dst.app>" >&2
    exit 1
fi

if [ ! -d "$1" ]; then
    echo "error: source app not found: $1" >&2
    exit 1
fi
SRC="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
DST="$2"
DST_PARENT="$(dirname "$DST")"
mkdir -p "$DST_PARENT"
DST="$(cd "$DST_PARENT" && pwd)/$(basename "$DST")"

if [ ! -d "$SRC" ]; then
    echo "error: source app not found: $SRC" >&2
    exit 1
fi

if [ "$SRC" != "$DST" ]; then
    rm -rf "$DST"
    ditto "$SRC" "$DST"
fi

RESOURCES="$DST/Contents/Resources"
if [ -L "$RESOURCES" ]; then
    TARGET="$(readlink "$RESOURCES")"
    case "$TARGET" in
        /*) ;;
        *) TARGET="$(cd "$(dirname "$RESOURCES")" && pwd)/$TARGET" ;;
    esac
    if [ ! -d "$TARGET" ]; then
        echo "error: Resources symlink target missing: $TARGET" >&2
        exit 1
    fi
    rm "$RESOURCES"
    echo "Copying Resources from $TARGET"
    ditto "$TARGET" "$RESOURCES"
fi

if [ -L "$RESOURCES" ] || [ ! -d "$RESOURCES" ]; then
    echo "error: $RESOURCES must be a real directory (not a symlink)" >&2
    exit 1
fi

PLIST="$DST/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :NSPrincipalClass NSApplication" "$PLIST" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c "Add :NSPrincipalClass string NSApplication" "$PLIST"
/usr/libexec/PlistBuddy -c "Set :NSHighResolutionCapable true" "$PLIST" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c "Add :NSHighResolutionCapable bool true" "$PLIST"
/usr/libexec/PlistBuddy -c "Set :CSResourcesFileMapped false" "$PLIST" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c "Add :CSResourcesFileMapped bool false" "$PLIST"

find "$DST" -name '.DS_Store' -delete
xattr -cr "$DST"

if [ "$CODESIGN_IDENTITY" = "-" ]; then
    echo "Signing $DST (ad-hoc)"
    codesign --force --deep --sign - --timestamp=none "$DST"
else
    echo "Signing $DST with identity: $CODESIGN_IDENTITY"
    codesign --force --deep --options runtime --timestamp \
        --entitlements "$ENTITLEMENTS" \
        --sign "$CODESIGN_IDENTITY" "$DST"
fi
codesign --verify --verbose --deep --strict "$DST"
echo "Packaged Finder-launchable app: $DST"

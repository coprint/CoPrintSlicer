#!/usr/bin/env bash
# Notarize + staple a signed DMG. No-op (with a warning) if credentials are
# not configured, so it's safe to call this unconditionally from dmg.sh.
#
# Requires the DMG to already be signed with a real "Developer ID Application"
# identity (see CODESIGN_IDENTITY in macos_package_app.sh) - notarization
# rejects ad-hoc / unsigned binaries.
#
# Credentials, one of:
#   App Store Connect API key (recommended, doesn't need 2FA / doesn't expire):
#     APPLE_API_KEY_ID, APPLE_API_ISSUER_ID, APPLE_API_KEY_PATH (path to .p8)
#   Apple ID + app-specific password:
#     APPLE_ID, APPLE_TEAM_ID, APPLE_APP_PASSWORD
#
# Usage: notarize_dmg.sh <path.dmg>
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <path.dmg>" >&2
    exit 1
fi
DMG="$1"

if [ -n "${APPLE_API_KEY_ID:-}" ] && [ -n "${APPLE_API_ISSUER_ID:-}" ] && [ -n "${APPLE_API_KEY_PATH:-}" ]; then
    AUTH_ARGS=(--key "$APPLE_API_KEY_PATH" --key-id "$APPLE_API_KEY_ID" --issuer "$APPLE_API_ISSUER_ID")
elif [ -n "${APPLE_ID:-}" ] && [ -n "${APPLE_TEAM_ID:-}" ] && [ -n "${APPLE_APP_PASSWORD:-}" ]; then
    AUTH_ARGS=(--apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_PASSWORD")
else
    echo "notarize_dmg.sh: no Apple credentials configured, skipping notarization." >&2
    echo "  (set APPLE_API_KEY_ID/APPLE_API_ISSUER_ID/APPLE_API_KEY_PATH, or APPLE_ID/APPLE_TEAM_ID/APPLE_APP_PASSWORD)" >&2
    exit 0
fi

echo "Submitting $DMG for notarization..."
xcrun notarytool submit "$DMG" "${AUTH_ARGS[@]}" --wait

echo "Stapling notarization ticket to $DMG..."
xcrun stapler staple "$DMG"

echo "Notarization complete: $DMG"

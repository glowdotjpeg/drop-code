#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VERSION=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$ROOT/App/Info.plist")
ARCH=$(uname -m)
APP="$ROOT/.build/DropCode.app"
DIST="$ROOT/dist"
ARCHIVE="DropCode-v$VERSION-macos-$ARCH.zip"

"$ROOT/scripts/build-app.sh"
codesign --verify --deep --strict --verbose=2 "$APP"

rm -rf "$DIST"
mkdir -p "$DIST"
ditto -c -k --sequesterRsrc --keepParent "$APP" "$DIST/$ARCHIVE"

cd "$DIST"
shasum -a 256 "$ARCHIVE" > "$ARCHIVE.sha256"
printf '%s\n' "$DIST/$ARCHIVE"

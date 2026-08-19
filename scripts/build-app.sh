#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
APP="$ROOT/.build/DropCode.app"
if [ -z "${SIGNING_IDENTITY:-}" ] && [ -f "$ROOT/.signing.local" ]; then
    . "$ROOT/.signing.local"
fi
SIGNING_IDENTITY=${SIGNING_IDENTITY:--}

cd "$ROOT"
swift build -c release
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
mkdir -p "$APP/Contents/Resources"
cp "$ROOT/.build/release/DropCode" "$APP/Contents/MacOS/DropCode"
cp "$ROOT/App/Info.plist" "$APP/Contents/Info.plist"
for PRODUCTS in "$ROOT/.build/release" "$ROOT/.build/out/Products/Release"; do
    [ -d "$PRODUCTS" ] || continue
    find "$PRODUCTS" -maxdepth 1 -type d -name '*.bundle' \
        -exec cp -R {} "$APP/Contents/Resources/" \;
done
codesign \
    --force \
    --options runtime \
    --timestamp=none \
    --sign "$SIGNING_IDENTITY" \
    "$APP"

printf '%s\n' "$APP"

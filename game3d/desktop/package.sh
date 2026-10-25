#!/usr/bin/env bash
# Packages a built CarPolo 3D executable for players.
#   desktop/package.sh windows <path/to/carpolo3d.exe> <outdir>   -> CarPolo3D-windows-x64.zip
#   desktop/package.sh linux   <path/to/carpolo3d>     <outdir>   -> CarPolo3D-linux-x64.tar.gz
#   desktop/package.sh macos   <path/to/carpolo3d>     <outdir>   -> CarPolo3D-macos.zip ("CarPolo 3D.app", needs macOS tools)
set -euo pipefail
PLATFORM="$1"; EXE="$2"; OUT="$3"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME="$(cd "$HERE/.." && pwd)"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
BUILD_NUMBER="${BUILD_NUMBER:-1}"

copy_assets() {   # $1 = destination directory
    mkdir -p "$1"
    cp "$GAME/assets/world.bin" "$GAME/assets/font.ttf" "$GAME/assets/font-OFL.txt" "$GAME/assets/icon.png" "$1/"
}

case "$PLATFORM" in
windows)
    D="$STAGE/CarPolo3D"
    mkdir -p "$D"
    cp "$EXE" "$D/CarPolo3D.exe"
    copy_assets "$D/assets"
    sed 's/$/\r/' "$HERE/LISEZMOI.txt" > "$D/LISEZMOI.txt"
    (cd "$STAGE" && rm -f "$OUT/CarPolo3D-windows-x64.zip" && zip -qr9 "$OUT/CarPolo3D-windows-x64.zip" CarPolo3D)
    echo "$OUT/CarPolo3D-windows-x64.zip"
    ;;
linux)
    D="$STAGE/CarPolo3D"
    mkdir -p "$D"
    cp "$EXE" "$D/carpolo3d"
    chmod 755 "$D/carpolo3d"
    copy_assets "$D/assets"
    cp "$HERE/LISEZMOI.txt" "$HERE/carpolo3d.desktop" "$D/"
    cp "$HERE/installer-raccourci.sh" "$D/" && chmod 755 "$D/installer-raccourci.sh"
    tar -C "$STAGE" -czf "$OUT/CarPolo3D-linux-x64.tar.gz" CarPolo3D
    echo "$OUT/CarPolo3D-linux-x64.tar.gz"
    ;;
macos)
    APP="$STAGE/CarPolo 3D.app"
    mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
    cp "$EXE" "$APP/Contents/MacOS/carpolo3d"
    chmod 755 "$APP/Contents/MacOS/carpolo3d"
    copy_assets "$APP/Contents/Resources/assets"
    sed "s/@BUILD_NUMBER@/$BUILD_NUMBER/" "$HERE/Info.plist" > "$APP/Contents/Info.plist"
    # .icns from the 1024 px PNG
    ICONSET="$STAGE/AppIcon.iconset"
    mkdir -p "$ICONSET"
    for s in 16 32 128 256 512; do
        sips -z $s $s "$HERE/icon.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
        sips -z $((s * 2)) $((s * 2)) "$HERE/icon.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
    done
    iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"
    # ad-hoc signature (required on Apple Silicon; the app is not notarised)
    codesign --force --deep --sign - "$APP"
    mkdir -p "$STAGE/dmg"
    mv "$APP" "$STAGE/dmg/"
    cp "$HERE/LISEZMOI.txt" "$STAGE/dmg/"
    (cd "$STAGE/dmg" && rm -f "$OUT/CarPolo3D-macos.zip" && ditto -c -k --sequesterRsrc --keepParent "CarPolo 3D.app" "$OUT/CarPolo3D-macos.zip" && zip -q "$OUT/CarPolo3D-macos.zip" LISEZMOI.txt)
    echo "$OUT/CarPolo3D-macos.zip"
    ;;
*)
    echo "usage: $0 windows|linux|macos <exe> <outdir>" >&2
    exit 1
    ;;
esac

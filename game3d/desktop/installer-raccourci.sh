#!/bin/sh
# Adds CarPolo 3D to the application menu (Linux, current user only)
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$HOME/.local/share/applications" "$HOME/.local/share/icons/hicolor/256x256/apps"
cp "$DIR/assets/icon.png" "$HOME/.local/share/icons/hicolor/256x256/apps/carpolo3d.png"
sed "s|^Exec=.*|Exec=\"$DIR/carpolo3d\"|; s|^Icon=.*|Icon=carpolo3d|" "$DIR/carpolo3d.desktop" \
    > "$HOME/.local/share/applications/carpolo3d.desktop"
echo "CarPolo 3D ajouté au menu des applications."

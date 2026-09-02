#!/bin/sh
# Copy the built dylib and the Steam launch wrapper to the install location:
#   ~/Library/Application Support/Sid Meier's Civilization VI/CAI/
# The files must live outside Documents, Desktop and Downloads: Steam runs
# %command% wrappers itself and has no macOS privacy grant for those folders,
# so a wrapper there never starts (the launch hangs at "CreatingProcess").
set -e
cd "$(dirname "$0")/.."
DEST="$HOME/Library/Application Support/Sid Meier's Civilization VI/CAI"
mkdir -p "$DEST"
cp native/build/libcai.dylib "$DEST/libcai.dylib"
cp steam_launch.sh "$DEST/steam_launch.sh"
chmod +x "$DEST/steam_launch.sh"
echo "deployed to $DEST"
echo "Steam launch options: \"$DEST/steam_launch.sh\" %command%"

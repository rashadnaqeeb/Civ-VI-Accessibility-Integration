#!/bin/sh
# Install the Civilization VI Accessibility Integration (CAI) on macOS.
#
# Run this from the unzipped release folder, which holds:
#   CivVi-Accessibility-Integration/   the mod
#   libcai.dylib                       the native layer (speech, sounds, config)
#   steam_launch.sh                    the Steam launch wrapper that loads it
#
# What it does:
#   1. Copies the mod into the game's Mods folder.
#   2. Copies the dylib and the wrapper into
#      ~/Library/Application Support/Sid Meier's Civilization VI/CAI/
#      (they must live outside Documents, Desktop and Downloads: Steam has no
#      privacy grant for those folders and a wrapper there never starts).
#   3. Puts the Steam launch option on the clipboard and prints it. Setting the
#      launch option in Steam is the one manual step.
#
# Requirements: Apple Silicon, macOS 13 or later, Civilization VI 1.4.6 from
# Steam. Options: --no-clipboard (do not touch the clipboard), --help.
set -e
# --help prints the comment block above, up to this line.

MOD_NAME="CivVi-Accessibility-Integration"
SUPPORT="$HOME/Library/Application Support/Sid Meier's Civilization VI"
MODS_DIR="$SUPPORT/Sid Meier's Civilization VI/Mods"
CAI_DIR="$SUPPORT/CAI"
GAME_APP="$HOME/Library/Application Support/Steam/steamapps/common/Sid Meier's Civilization VI/Civ6.app"
# The build the native layer was verified against; the same number lives in
# mac/native/entry.cpp, and package.sh checks that the two agree.
SUPPORTED_BUILD="376653.178"

USE_CLIPBOARD=1
for arg in "$@"; do
  case "$arg" in
    --no-clipboard) USE_CLIPBOARD=0 ;;
    -h|--help) sed -n '2,/^set -e$/{/^set -e$/!p;}' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
  esac
done

HERE="$(cd "$(dirname "$0")" && pwd)"
fail() { echo "error: $*" >&2; exit 1; }

# --- Checks -------------------------------------------------------------------
[ "$(uname -m)" = "arm64" ] || fail "this Mac is not Apple Silicon; the accessibility mod's native layer is arm64 only"
MAJOR="$(sw_vers -productVersion | cut -d. -f1)"
[ "$MAJOR" -ge 13 ] 2>/dev/null || fail "macOS 13 or later is required (this is $(sw_vers -productVersion))"
for f in "$MOD_NAME/CivViAccess.modinfo" libcai.dylib steam_launch.sh; do
  [ -e "$HERE/$f" ] || fail "missing $f next to this script; run install.sh from the unzipped release folder"
done
if [ -d "$GAME_APP" ]; then
  BUILD="$(defaults read "$GAME_APP/Contents/Info.plist" CFBundleVersion 2>/dev/null || true)"
  if [ "$BUILD" != "$SUPPORTED_BUILD" ]; then
    echo "warning: the installed game is build ${BUILD:-unknown}; the native layer supports build $SUPPORTED_BUILD and stays off on other builds" >&2
  fi
else
  echo "warning: Civilization VI was not found in the default Steam library. That is fine if it is installed in another Steam library; installing anyway" >&2
fi

# --- Mod folder ---------------------------------------------------------------
mkdir -p "$MODS_DIR"
TARGET="$MODS_DIR/$MOD_NAME"
if [ -L "$TARGET" ]; then
  echo "$TARGET is a symbolic link (a development setup); leaving it in place"
else
  rm -rf "$TARGET"
  cp -R "$HERE/$MOD_NAME" "$TARGET"
  echo "mod installed to $TARGET"
fi

# --- Native layer -------------------------------------------------------------
mkdir -p "$CAI_DIR"
cp "$HERE/libcai.dylib" "$CAI_DIR/libcai.dylib"
cp "$HERE/steam_launch.sh" "$CAI_DIR/steam_launch.sh"
chmod +x "$CAI_DIR/steam_launch.sh"
# Downloaded files carry the quarantine flag; clear it so the wrapper and the
# dylib are not blocked.
xattr -d com.apple.quarantine "$CAI_DIR/libcai.dylib" "$CAI_DIR/steam_launch.sh" 2>/dev/null || true
echo "native layer installed to $CAI_DIR"

# --- Steam launch option ------------------------------------------------------
LAUNCH_OPTION="\"$CAI_DIR/steam_launch.sh\" %command%"
if [ "$USE_CLIPBOARD" = 1 ] && command -v pbcopy >/dev/null 2>&1; then
  printf '%s' "$LAUNCH_OPTION" | pbcopy && CLIP=1 || CLIP=0
else
  CLIP=0
fi
echo
echo "One step remains: set the game's launch options in Steam."
echo "In Steam, open Library, select Sid Meier's Civilization VI, choose Properties,"
echo "then General, and paste this into Launch Options:"
echo
echo "$LAUNCH_OPTION"
echo
[ "$CLIP" = 1 ] && echo "It is on the clipboard now."
echo "Then start the game from Steam as usual. The mod is enabled automatically."
echo "Tip: in System Settings, Keyboard, turn on \"Use F1, F2, etc. keys as standard function keys\","
echo "since the mod uses the function keys."

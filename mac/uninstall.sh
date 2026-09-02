#!/bin/sh
# Remove the Civilization VI Accessibility Integration (CAI) from this Mac:
# the mod folder, the native layer, and the log folder the game wrote while
# the mod ran. The settings file civ6-accessibility-integration.ini is kept
# unless --purge is given. The Steam launch option must be cleared by hand
# (the script says how).
# Options: --purge (also remove the settings file), --yes (no confirmation).
set -e
# --help prints the comment block above, up to this line.

MOD_NAME="CivVi-Accessibility-Integration"
SUPPORT="$HOME/Library/Application Support/Sid Meier's Civilization VI"
MODS_DIR="$SUPPORT/Sid Meier's Civilization VI/Mods"
CAI_DIR="$SUPPORT/CAI"
INI="$SUPPORT/civ6-accessibility-integration.ini"
LOGS="$HOME/Library/Logs/CAI"

PURGE=0; YES=0
for arg in "$@"; do
  case "$arg" in
    --purge) PURGE=1 ;;
    --yes) YES=1 ;;
    -h|--help) sed -n '2,/^set -e$/{/^set -e$/!p;}' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
  esac
done

if [ "$YES" != 1 ]; then
  printf 'Remove the accessibility mod and its native layer? [y/N] '
  read -r answer
  case "$answer" in y|Y|yes|YES) ;; *) echo "nothing removed"; exit 0 ;; esac
fi

TARGET="$MODS_DIR/$MOD_NAME"
if [ -L "$TARGET" ]; then
  echo "$TARGET is a symbolic link (a development setup); leaving it in place"
elif [ -d "$TARGET" ]; then
  rm -rf "$TARGET"; echo "removed $TARGET"
fi
[ -d "$CAI_DIR" ] && { rm -rf "$CAI_DIR"; echo "removed $CAI_DIR"; }
[ -d "$LOGS" ] && { rm -rf "$LOGS"; echo "removed $LOGS"; }
if [ "$PURGE" = 1 ]; then
  [ -f "$INI" ] && { rm -f "$INI"; echo "removed $INI"; }
else
  [ -f "$INI" ] && echo "kept the settings file $INI (use --purge to remove it)"
fi
echo
echo "Finally, clear the game's launch options in Steam: Library, Sid Meier's Civilization VI,"
echo "Properties, General, Launch Options. Otherwise Steam keeps starting the game through"
echo "the removed wrapper and the launch fails."

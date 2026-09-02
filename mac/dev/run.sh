#!/bin/sh
# Launch the Mac Civ VI child process with the CAI dylib injected, inside the
# user's GUI launchd session (gui/<uid>). Launching from an SSH shell instead
# puts the process in the Background session, where the speech synthesis
# service ignores voice selection and always renders with Samantha.
# Steam must be running. Output goes to run.log next to this script; the
# generated launchd plist lands here too (both are gitignored).
# Development only: assumes the game in the default Steam library.
cd "$(dirname "$0")" || exit 1
HERE="$(pwd)"
DYLIB="$(cd ../native && pwd)/build/libcai.dylib"
[ -f "$DYLIB" ] || { echo "no dylib at $DYLIB; run build.sh first"; exit 1; }
GAME="$HOME/Library/Application Support/Steam/steamapps/common/Sid Meier's Civilization VI/Civ6.app/Contents/MacOS"
LABEL=cai.civ6.dev
PLIST="$HERE/$LABEL.plist"
cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>Label</key><string>$LABEL</string>
<key>ProgramArguments</key><array><string>$GAME/Civ6_Exe_Child</string></array>
<key>WorkingDirectory</key><string>$GAME</string>
<key>EnvironmentVariables</key><dict>
  <key>SteamAppId</key><string>289070</string>
  <key>SteamGameId</key><string>289070</string>
  <key>DYLD_INSERT_LIBRARIES</key><string>$DYLIB</string>
</dict>
<key>StandardOutPath</key><string>$HERE/run.log</string>
<key>StandardErrorPath</key><string>$HERE/run.log</string>
<key>RunAtLoad</key><true/>
</dict></plist>
PL
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null
# launchd needs a moment to tear the previous job down; retry the bootstrap.
n=0
until launchctl bootstrap "gui/$(id -u)" "$PLIST" 2>/dev/null; do
  n=$((n+1)); [ $n -ge 10 ] && { echo "bootstrap failed"; exit 1; }; sleep 1
done
sleep 1
echo "launched pid $(pgrep -x Civ6_Exe_Child) in gui/$(id -u) as $LABEL"

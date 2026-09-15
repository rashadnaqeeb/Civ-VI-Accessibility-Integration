#!/bin/sh
# Steam launch wrapper for CAI on macOS. The game's Steam launch option is
#   "/full/path/to/steam_launch" %command%
# where steam_launch is the native launcher built from native/steam_launch.c
# and installed next to this script; all it does is run this script through
# /bin/sh. Steam runs the launcher with the game's own command line as
# arguments and the Steam environment (SteamAppId and friends) already set.
#
# - Why the launcher and not this script as the launch option: Steam spawns
#   the launch command preferring x86_64 over arm64, so a "#!/bin/sh" script
#   is run by the x86_64 slice of /bin/sh and needs Rosetta, which macOS 27
#   drops on upgrade and a new Mac never has. The arm64-only launcher takes
#   Steam's arm64 fallback and runs natively. With Rosetta installed this
#   script also works as the launch option on its own.
# - On macOS %command% is the app bundle (Civ6.app). The bundle's own
#   executable is Aspyr's launcher (Civ6_Exe), which spawns the real game
#   (Civ6_Exe_Child). Started from a wrapper the launcher idles in its event
#   loop and never spawns the game, so the script starts Civ6_Exe_Child
#   directly, as the development launch does.
# - The game is universal. `arch -arm64` starts its native slice whether this
#   shell is native (started by the launcher) or translated (script used
#   directly under Rosetta); the CAI dylib is arm64 only and Apple Silicon is
#   the only supported hardware.
# - Steam on macOS does not accept a plain VAR=value prefix before %command%,
#   hence the script. It loads the CAI dylib next to this script into the game.
#
# Whatever goes wrong, the game is started: without the mod when the dylib is
# missing or the Mac is not Apple Silicon. The log keeps this launch and the
# previous one (steam_launch.log and steam_launch.log.1).
DIR="$(cd "$(dirname "$0")" && pwd)"
DYLIB="$DIR/libcai.dylib"
LOG="$HOME/Library/Logs/CAI/steam_launch.log"
mkdir -p "$(dirname "$LOG")"
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.1"
[ -n "$1" ] || { echo "CAI: no game command received from Steam" >> "$LOG"; exit 1; }
APP="$1"; shift
case "$APP" in
  *.app|*.app/) EXE="$APP/Contents/MacOS/Civ6_Exe_Child" ;;
  *) EXE="$(dirname "$APP")/Civ6_Exe_Child" ;;
esac
echo "$(date) pid $$ app=$APP exe=$EXE args=$*" >> "$LOG"
export SteamAppId="${SteamAppId:-289070}" SteamGameId="${SteamGameId:-289070}"
cd "$(dirname "$EXE")" || { echo "CAI: cannot enter $(dirname "$EXE")" >> "$LOG"; exit 1; }
if [ ! -f "$DYLIB" ]; then
  echo "CAI: $DYLIB not found; starting the game without the accessibility mod" >> "$LOG"
  exec "$EXE" "$@" 2>>"$LOG"
elif [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" = "1" ]; then
  # arch and sh are protected system binaries: macOS strips DYLD_* from their
  # environment when they start. So the arm64 shell sets the variable itself,
  # right before it starts the game, which is allowed to receive it. The
  # single quotes are deliberate: $0 and $@ are the inner shell's.
  # shellcheck disable=SC2016
  exec /usr/bin/arch -arm64 /bin/sh -c 'export DYLD_INSERT_LIBRARIES="$0"; exec "$@"' "$DYLIB" "$EXE" "$@" 2>>"$LOG"
else
  echo "CAI: this Mac is not Apple Silicon; starting the game without the accessibility mod" >> "$LOG"
  exec "$EXE" "$@" 2>>"$LOG"
fi

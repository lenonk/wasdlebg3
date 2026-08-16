#!/usr/bin/env bash
# Launch BG3 directly with bg3le, bypassing Steam's launch options and its
# container runtime entirely. Use this when the Steam route produces no log:
# here we control the environment completely, so if the library still does not
# load we know the problem is the library and not the plumbing.
#
#   ./run-bg3le.sh "/path/to/Baldurs Gate 3"
#
# Steam should be running in the background so the game can reach the Steam API.
set -uo pipefail

SO="${BG3LE_SO:-$HOME/bg3le/bg3le.so}"
LOG="${BG3LE_LOG:-$HOME/bg3le.log}"
APPID=1086940

GAME="${1:-}"
if [ -z "$GAME" ]; then
    for d in "$HOME"/.steam/steam/steamapps/common/"Baldurs Gate 3" \
             "$HOME"/.local/share/Steam/steamapps/common/"Baldurs Gate 3" \
             /run/media/*/*/SteamLibrary/steamapps/common/"Baldurs Gate 3" \
             /media/*/*/SteamLibrary/steamapps/common/"Baldurs Gate 3"; do
        [ -d "$d" ] && { GAME="$d"; break; }
    done
fi
[ -n "$GAME" ] || { echo "usage: $0 \"/path/to/Baldurs Gate 3\"" >&2; exit 2; }
[ -f "$SO" ] || { echo "library not found: $SO" >&2; exit 2; }

BIN="$GAME/bin"
[ -x "$BIN/bg3" ] || { echo "no executable at $BIN/bg3" >&2; ls "$BIN" >&2; exit 2; }

cd "$BIN" || exit 2

# Launched outside Steam, the game cannot infer its own app id and SteamAPI_Init
# fails. This file is the standard way to tell it.
if [ ! -f steam_appid.txt ]; then
    if echo "$APPID" > steam_appid.txt 2>/dev/null; then
        echo "wrote steam_appid.txt (needed when not launched by Steam)"
    else
        echo "WARNING: could not write steam_appid.txt here; the game may refuse to start"
    fi
fi

rm -f "$LOG"
echo "game    : $BIN/bg3"
echo "library : $SO"
echo "log     : $LOG"
echo "launching — the game's own output follows"
echo "------------------------------------------------------------"

BG3LE_LOG="$LOG" \
BG3LE_VERBOSE="${BG3LE_VERBOSE:-1}" \
LD_PRELOAD="$SO" \
    ./bg3 "$@"
rc=$?

echo "------------------------------------------------------------"
echo "game exited with $rc"
if [ -s "$LOG" ]; then
    echo "log at $LOG:"
    cat "$LOG"
else
    echo "NO LOG WRITTEN. If the game itself ran, LD_PRELOAD was dropped;"
    echo "if the game did not run, look at its output above."
fi

#!/usr/bin/env bash
# Installs bg3le.so somewhere Steam can reach it and prints the launch options.
# Deliberately does not touch the game directory or write anything to it.
set -uo pipefail

DEST="${BG3LE_DEST:-$HOME/.local/share/bg3le}"
SO="build/bg3le.so"

if [ ! -f "$SO" ]; then
    echo "$SO not found — run 'make' first." >&2
    exit 1
fi

case "$DEST" in
    *" "*) echo "Install path contains a space: $DEST" >&2
           echo "Steam launch options cannot be quoted; pick a path without spaces." >&2
           exit 1 ;;
esac

mkdir -p "$DEST" || exit 1
cp "$SO" "$DEST/" || exit 1
[ -f build/bg3le-check ] && cp build/bg3le-check "$DEST/"
echo "installed $DEST/bg3le.so"
echo

# Locate the game so we can tell the user whether their build is supported.
GAME=""
for d in "$HOME"/.steam/steam/steamapps/common/"Baldurs Gate 3" \
         "$HOME"/.local/share/Steam/steamapps/common/"Baldurs Gate 3" \
         /run/media/*/*/SteamLibrary/steamapps/common/"Baldurs Gate 3" \
         /media/*/*/SteamLibrary/steamapps/common/"Baldurs Gate 3"; do
    [ -d "$d" ] && { GAME="$d"; break; }
done

if [ -n "$GAME" ] && [ -x build/bg3le-check ]; then
    echo "found game at: $GAME"
    ./build/bg3le-check "$GAME" || {
        echo
        echo "That build is not recognised. Please open an issue with the output above:"
        echo "  https://github.com/Nerocon/wasdlebg3/issues"
        exit 1
    }
elif [ -z "$GAME" ]; then
    echo "Could not find a Baldur's Gate 3 install automatically."
    echo "Check yours with:  ./build/bg3le-check \"/path/to/Baldurs Gate 3\""
    echo
fi

cat <<EOF
------------------------------------------------------------
Set this as the Steam launch options for Baldur's Gate 3
(right-click the game -> Properties -> General):

    LD_PRELOAD=$DEST/bg3le.so %command%

Then launch the game and hold W. Left Shift walks.
------------------------------------------------------------
EOF

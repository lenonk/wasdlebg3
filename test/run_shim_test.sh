#!/usr/bin/env bash
# Verifies the LD_PRELOAD shim against a stand-in game, with no display and no
# copy of BG3 required. Asserts on the harness's RESULT line and on the log.
set -uo pipefail
B="${1:-build}"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
fails=0
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

check() { # name expected actual
  if [ "$2" = "$3" ]; then
    echo "PASS  $1"
  else
    echo "FAIL  $1 — expected '$2', got '$3'"
    fails=$((fails + 1))
  fi
}

# The harness pushes 6 key events, 4 of them WASD.
base=$(./"$B"/sdl_harness | grep '^RESULT')
check "baseline passes all 6 keys through" "RESULT keys=6 axes=0 other=0" "$base"

sup=$(BG3LE_LOG=/dev/null LD_PRELOAD=./"$B"/bg3le.so ./"$B"/sdl_harness | grep '^RESULT')
check "shim hides all 4 WASD keys" "RESULT keys=2 axes=0 other=0" "$sup"

off=$(BG3LE_LOG=/dev/null BG3LE_SUPPRESS=0 LD_PRELOAD=./"$B"/bg3le.so \
      ./"$B"/sdl_harness | grep '^RESULT')
check "BG3LE_SUPPRESS=0 restores all keys" "RESULT keys=6 axes=0 other=0" "$off"

# Non-movement keys must survive untouched and in order.
order=$(BG3LE_LOG=/dev/null LD_PRELOAD=./"$B"/bg3le.so ./"$B"/sdl_harness \
        | awk '/GAME SAW key/{printf "%s ", $4}')
check "unrelated keys pass through in order" "X Space " "$order"

# Steam re-execs through many helper processes and each inherits LD_PRELOAD.
# Loading into something that is not the game must produce no log noise at all.
: > "$TMP/quiet.log"
BG3LE_LOG="$TMP/quiet.log" LD_PRELOAD=./"$B"/bg3le.so ./"$B"/sdl_harness >/dev/null
check "silent in a non-game process" "0" "$(wc -l < "$TMP/quiet.log")"

# ...but must still be diagnosable on demand.
: > "$TMP/loud.log"
BG3LE_LOG="$TMP/loud.log" BG3LE_VERBOSE=1 LD_PRELOAD=./"$B"/bg3le.so \
  ./"$B"/sdl_harness >/dev/null
check "BG3LE_VERBOSE=1 explains why it went idle" "1" \
      "$(grep -c 'not the game' "$TMP/loud.log")"

# A non-game host must never be mistaken for BG3, so nothing may be written.
check "never claims to have identified BG3" "0" \
      "$(grep -c 'BG3 identified' "$TMP/loud.log")"

echo
if [ "$fails" -eq 0 ]; then echo "ALL PASSED"; else echo "FAILED ($fails)"; fi
exit $((fails != 0))

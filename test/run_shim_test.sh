#!/usr/bin/env bash
# Verifies the LD_PRELOAD shim against a stand-in game. Needs no display and no
# copy of BG3 — it asserts on what the harness reports SDL_PollEvent handed back.
set -uo pipefail
B="${1:-build}"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
fails=0
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

SHIM=(env BG3LE_LOG=/dev/null LD_PRELOAD=./"$B"/bg3le.so)

check() { # name expected actual
  if [ "$2" = "$3" ]; then
    echo "PASS  $1"
  else
    echo "FAIL  $1 — expected '$2', got '$3'"
    fails=$((fails + 1))
  fi
}

base=$(./"$B"/sdl_harness | grep '^RESULT')
check "baseline passes all 6 keys through" "RESULT keys=6 other=0" "$base"

sup=$("${SHIM[@]}" ./"$B"/sdl_harness | grep '^RESULT')
check "shim hides all 4 WASD keys" "RESULT keys=2 other=0" "$sup"

off=$("${SHIM[@]}" BG3LE_SUPPRESS=0 ./"$B"/sdl_harness | grep '^RESULT')
check "BG3LE_SUPPRESS=0 restores all keys" "RESULT keys=6 other=0" "$off"

order=$("${SHIM[@]}" ./"$B"/sdl_harness | awk '/GAME SAW key/{printf "%s ", $4}')
check "unrelated keys pass through in order" "X Space " "$order"

# The walk modifier is observed, never stolen: the game still needs its binding.
mod=$("${SHIM[@]}" ./"$B"/sdl_harness modifier)
check "walk modifier reaches the game" "2" "$(grep -c 'GAME SAW key Left Shift' <<<"$mod")"
check "...but W does not" "0" "$(grep -c 'GAME SAW key W ' <<<"$mod")"

modoff=$("${SHIM[@]}" BG3LE_WALK_KEY=none ./"$B"/sdl_harness modifier)
check "BG3LE_WALK_KEY=none still passes the key" "2" \
      "$(grep -c 'GAME SAW key Left Shift' <<<"$modoff")"

# Typing a save name must not be eaten, and must not walk the character.
txt=$("${SHIM[@]}" ./"$B"/sdl_harness textinput | grep '^RESULT')
check "WASD reaches the game while typing" "RESULT keys=4 other=0" "$txt"

# Losing focus with a key held must not leave the character walking.
foc=$("${SHIM[@]}" BG3LE_TRACE=1 BG3LE_VERBOSE=1 BG3LE_LOG="$TMP/f.log" ./"$B"/sdl_harness focus >/dev/null; \
      grep -c 'focus lost' "$TMP/f.log")
check "focus loss releases movement" "1" "$foc"

# Steam re-execs through many helpers; loading into one must be silent.
: > "$TMP/quiet.log"
BG3LE_LOG="$TMP/quiet.log" LD_PRELOAD=./"$B"/bg3le.so ./"$B"/sdl_harness >/dev/null
check "silent in a non-game process" "0" "$(wc -l < "$TMP/quiet.log")"

: > "$TMP/loud.log"
BG3LE_LOG="$TMP/loud.log" BG3LE_VERBOSE=1 LD_PRELOAD=./"$B"/bg3le.so \
  ./"$B"/sdl_harness >/dev/null 2>/dev/null
check "BG3LE_VERBOSE=1 explains why it went idle" "1" \
      "$(grep -c 'not the game' "$TMP/loud.log")"
check "never claims to have identified BG3" "0" \
      "$(grep -c 'BG3 identified' "$TMP/loud.log")"

echo
if [ "$fails" -eq 0 ]; then echo "ALL PASSED"; else echo "FAILED ($fails)"; fi
exit $((fails != 0))

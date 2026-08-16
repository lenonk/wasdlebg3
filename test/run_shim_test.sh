#!/usr/bin/env bash
# Verifies the LD_PRELOAD shim against a stand-in game, with no display and no
# copy of BG3 required. Asserts on the harness's RESULT line.
set -uo pipefail
B="${1:-build}"
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
fails=0

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

sup=$(BG3LE_LOG=/dev/null BG3LE_SUPPRESS=1 LD_PRELOAD=./"$B"/bg3le.so \
      ./"$B"/sdl_harness | grep '^RESULT')
check "shim hides all 4 WASD keys" "RESULT keys=2 axes=0 other=0" "$sup"

inj=$(BG3LE_LOG=/dev/null BG3LE_SUPPRESS=1 BG3LE_INJECT=1 LD_PRELOAD=./"$B"/bg3le.so \
      ./"$B"/sdl_harness | grep '^RESULT')
check "shim injects stick motion for each vector change" "RESULT keys=2 axes=8 other=0" "$inj"

# Non-movement keys must survive untouched and in order.
order=$(BG3LE_LOG=/dev/null LD_PRELOAD=./"$B"/bg3le.so ./"$B"/sdl_harness \
        | awk '/GAME SAW key/{printf "%s ", $4}')
check "unrelated keys pass through in order" "X Space " "$order"

# A diagonal must not outrun a cardinal.
diag=$(BG3LE_LOG=/dev/stdout BG3LE_INJECT=1 LD_PRELOAD=./"$B"/bg3le.so \
       ./"$B"/sdl_harness 2>/dev/null | grep -c 'injecting left-stick (23169,-23169)')
check "diagonal is normalised" "1" "$diag"

echo
if [ "$fails" -eq 0 ]; then echo "ALL PASSED"; else echo "FAILED ($fails)"; fi
exit $((fails != 0))

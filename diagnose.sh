#!/usr/bin/env bash
# Collects everything needed to work out why bg3le did or did not load.
# Read-only: launches nothing, changes nothing. Paste the whole output back.
SO="${1:-$HOME/bg3le/bg3le.so}"
APPID=1086940

echo "=============== bg3le diagnose ==============="
date
echo

echo "--- 1. the library ---"
if [ -f "$SO" ]; then
    ls -la "$SO"
    md5sum "$SO"
else
    echo "MISSING: $SO"
    echo "candidates found elsewhere:"
    find "$HOME" -name 'bg3le.so' 2>/dev/null | head -5
fi
echo

echo "--- 2. does it load? (this is the decisive test) ---"
if [ -f "$SO" ]; then
    out=$(LD_PRELOAD="$SO" BG3LE_VERBOSE=1 BG3LE_LOG=/dev/stdout /bin/true 2>&1)
    rc=$?
    echo "exit=$rc"
    echo "$out"
    case "$out" in
      *"cannot be preloaded"*|*"not found"*|*"undefined symbol"*)
        echo ">>> The loader REFUSED the library. That alone explains a missing log." ;;
      *"not the game"*)
        echo ">>> Library loads correctly. The problem is upstream of the library." ;;
    esac
fi
echo

echo "--- 3. glibc (need 2.34+) ---"
ldd --version 2>/dev/null | head -1
echo

echo "--- 4. any logs anywhere ---"
find "$HOME" /tmp -maxdepth 3 -name 'bg3le*.log' -mmin -600 2>/dev/null | while read -r f; do
    echo "$f  ($(wc -l < "$f") lines, $(stat -c%s "$f") bytes)"
done
echo "(nothing listed above = no process ever loaded the library)"
echo

echo "--- 5. Steam launch options as Steam has them stored ---"
found=0
for f in "$HOME"/.steam/steam/userdata/*/config/localconfig.vdf \
         "$HOME"/.local/share/Steam/userdata/*/config/localconfig.vdf; do
    [ -f "$f" ] || continue
    found=1
    echo "from $f:"
    # Print the LaunchOptions line inside the block for this appid.
    awk -v id="\"$APPID\"" '
      $1==id {depth=1; next}
      depth && /LaunchOptions/ {print "   " $0; depth=0}
      depth && /^\s*}/ {depth=0}
    ' "$f" | head -3
done
[ "$found" = 1 ] || echo "no localconfig.vdf found"
echo "(blank = no launch options saved for BG3 -- that would be the whole problem)"
echo

echo "--- 6. is BG3 running, and as what? ---"
ps -eo pid,comm,args 2>/dev/null \
  | grep -iE '[b]g3|[w]ine|[p]roton|[p]ressure-vessel' \
  | grep -vE 'diagnose|bg3le-check|grep' \
  | cut -c1-160 | head -12
echo "(run this WHILE the game is up; 'bg3' = native, 'wine'/'proton' = Windows build)"
echo

echo "--- 7. the game install ---"
for d in "$HOME"/.steam/steam/steamapps/common/"Baldurs Gate 3" \
         "$HOME"/.local/share/Steam/steamapps/common/"Baldurs Gate 3" \
         /run/media/*/*/SteamLibrary/steamapps/common/"Baldurs Gate 3" \
         /media/*/*/SteamLibrary/steamapps/common/"Baldurs Gate 3"; do
    [ -d "$d" ] || continue
    echo "found: $d"
    ls -la "$d/bin/" 2>/dev/null | grep -iE 'bg3|\.so' | head -8
done
echo
echo "=============== end ==============="

#!/usr/bin/env bash
# Builds dist/LinuxNativeWASD-<version>.zip: the bg3le plugin, built against the Steam Runtime sniper sysroot so it
# loads on any distribution, with its installer, README and license. Install only with the game closed.
#   BG3LE_SNIPER_SYSROOT  sniper sysroot, as made by bg3le's tools/build-sniper.sh (default: ~/bg3mods/bg3le's)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SYSROOT="${BG3LE_SNIPER_SYSROOT:-$HOME/bg3mods/bg3le/build-sniper/sysroot}"
[ -f "$SYSROOT/usr/include/SDL2/SDL.h" ] || { echo "no sniper sysroot at $SYSROOT" >&2; exit 1; }
BUILD="build-sniper"
SO="$ROOT/$BUILD/bg3le/linux_native_wasd.so"

[ -f "$ROOT/vendor/bg3lese/Makefile" ] || git -C "$ROOT" submodule update --init --recursive
make -C "$ROOT" -B BUILD="$BUILD" CC=clang \
    CFLAGS="--target=x86_64-linux-gnu --sysroot=$SYSROOT -O2 -g -Wall -Wextra" \
    SDL_CFLAGS="-I$SYSROOT/usr/include/SDL2 -D_REENTRANT" "$BUILD/bg3le/linux_native_wasd.so"
newest="$(objdump -T "$SO" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
echo "plugin needs $newest"

VERSION="$(grep -o '#define WASD_VERSION "[^"]*"' "$ROOT/src/bg3le_plugin.c" | cut -d'"' -f2)"
mkdir -p "$ROOT/dist"
python3 - "$ROOT" "$SO" "$ROOT/dist/LinuxNativeWASD-$VERSION.zip" <<'PY'
import os, sys, zipfile
root, so, archive = sys.argv[1:]
files = [(so, "linux_native_wasd.so"), (os.path.join(root, "bg3le/install.py"), "install.py"),
         (os.path.join(root, "bg3le/README.md"), "README.md"), (os.path.join(root, "LICENSE"), "LICENSE")]
with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for path, name in files:
        # from_file keeps the Unix mode, so unzip leaves install.py executable.
        info = zipfile.ZipInfo.from_file(path, name)
        info.compress_type = zipfile.ZIP_DEFLATED
        with open(path, "rb") as f:
            z.writestr(info, f.read())
print("  " + archive)
PY

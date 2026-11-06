#!/bin/bash
# Bygger Zatacka för BananaOS (aarch64) och/eller för värden (x86_64, testrigg).
#
#   ./scripts/build.sh            allt: SDL2 (länkning) + spelet för aarch64, paketerar port/zatacka/
#   ./scripts/build.sh aarch64    bara spelet för aarch64 (SDL2 måste redan finnas i work/)
#   ./scripts/build.sh host       SDL2 + spelet nativt för x86_64 (hosttest)
#
# Kräver Ubuntu 24.04 x86_64 (eller motsvarande) med:
#   apt install gcc-aarch64-linux-gnu libc6-dev-arm64-cross cmake git curl   (+ qemu-user för röktest)
#
# SDL2 2.32.10 byggs bara för att LÄNKA mot (ingen grafikbackend behövs). På enheten används BananaOS:s egen
# Banana-patchade libSDL2-2.0.so.0. SDL2 har inga symbolversioner, så det räcker att API:t finns i 2.32.
# Källan till spelet (zatacka-decomp) klonas till work/ för TSCR.CHR och som referens; src/zatacka.c är
# porten av dess zatacka.cpp.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
WORK=$ROOT/work
SDL_VER=2.32.10
DECOMP_URL=https://github.com/GASQBDUV/zatacka-decomp
DECOMP_REV=$(cat reference/decomp-commit.txt)
WHAT=${1:-all}
JOBS=$(nproc)

mkdir -p "$WORK"

fetch() {
    if [ ! -d "$WORK/SDL2-$SDL_VER" ]; then
        curl -fsSL "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz" |
            tar -xz -C "$WORK"
    fi
    if [ ! -d "$WORK/zatacka-decomp" ]; then
        git clone -q "$DECOMP_URL" "$WORK/zatacka-decomp"
    fi
    git -C "$WORK/zatacka-decomp" -c advice.detachedHead=false checkout -q "$DECOMP_REV"
}

# $1 = target (aarch64|host)
build_sdl() {
    local t=$1 prefix=$WORK/sdl-$1 extra=()
    [ -f "$prefix/lib/libSDL2-2.0.so.0" ] && return 0
    if [ "$t" = aarch64 ]; then
        extra=(-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64
               -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc -DCMAKE_FIND_ROOT_PATH=/usr/aarch64-linux-gnu
               -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
               -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER)
    fi
    # Bara det som behövs för att länka: inga X11/Wayland/KMS/GL-backends, inget ljud.
    # Värdbygget används i testriggen med SDL_VIDEODRIVER=dummy (mjukvaruyta, samma väg som Banana-SDL:s
    # dummy/evdev-drivrutin på enheten).
    cmake -S "$WORK/SDL2-$SDL_VER" -B "$WORK/sdl-build-$t" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" "${extra[@]}" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF \
        -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF \
        -DSDL_AUDIO=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_LIBUDEV=OFF -DSDL_PIPEWIRE=OFF -DSDL_PULSEAUDIO=OFF \
        -DSDL_ALSA=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_RPI=OFF -DSDL_OFFSCREEN=ON -DSDL_DUMMYVIDEO=ON \
        >"$WORK/sdl-$t-configure.log"
    cmake --build "$WORK/sdl-build-$t" -j"$JOBS" >"$WORK/sdl-$t-build.log"
    cmake --install "$WORK/sdl-build-$t" >/dev/null
}

SRCS=(src/zatacka.c src/bgi.c src/platform.c src/font8x8.c)
CFLAGS=(-std=gnu99 -O2 -ffp-contract=off -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -fno-strict-aliasing)

build_game() {
    local t=$1 cc out prefix=$WORK/sdl-$1 rpath=()
    if [ "$t" = aarch64 ]; then
        cc=aarch64-linux-gnu-gcc; out=port/zatacka/zatacka
        # Cortex-A55 (RK3568); -mcpu ger bara schemaläggning + v8.2-instruktioner som A55 har.
        CFLAGS+=(-mcpu=cortex-a55)
    else
        cc=gcc; out=$WORK/host/zatacka; mkdir -p "$WORK/host"
        rpath=("-Wl,-rpath,$prefix/lib")   # värdens egen SDL2 2.32.10, inte systemets
    fi
    $cc "${CFLAGS[@]}" -I"$prefix/include/SDL2" -D_REENTRANT -DZATACKA_GIT_REV="\"$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo dev)\"" \
        "${SRCS[@]}" -o "$out" -L"$prefix/lib" -lSDL2 -lm -Wl,--as-needed "${rpath[@]}"
    if [ "$t" = aarch64 ]; then
        aarch64-linux-gnu-strip "$out"
        cp "$WORK/zatacka-decomp/original-game/TSCR.CHR" port/zatacka/
        echo "== $out"; file "$out" | sed 's/, BuildID[^,]*//'
        echo "   behöver: $(aarch64-linux-gnu-objdump -p "$out" | awk '/NEEDED/{printf "%s ", $2}')"
        echo "   GLIBC max: $(aarch64-linux-gnu-objdump -T "$out" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
    else
        cp "$WORK/zatacka-decomp/original-game/TSCR.CHR" "$WORK/host/"
        echo "== $out"
    fi
}

fetch
case $WHAT in
    all)     build_sdl aarch64; build_game aarch64 ;;
    aarch64) build_sdl aarch64; build_game aarch64 ;;
    host)    build_sdl host; build_game host ;;
    *) echo "okänt mål: $WHAT (all|aarch64|host)" >&2; exit 2 ;;
esac

#!/bin/bash
# Kör Zatacka utan enheten, i exakt RG DS Plus-geometri (ett 1024x1536-fönster, två paneler över varandra),
# med SDL:s dummy-drivrutin (mjukvaruyta, samma ritväg som Banana-SDL:s dummy/evdev) och samma inputskript som
# test-on-device.sh. Dumparna blir PNG i <utmapp>.
#
#   ./hosttest/run-host-test.sh <utmapp> [host|qemu] [VAR=värde ...]
#     host  x86_64-bygget (./scripts/build.sh host)
#     qemu  aarch64-binären i port/zatacka under qemu-aarch64 (apt install qemu-user)
#   t.ex. ./hosttest/run-host-test.sh /tmp/zt host ZATACKA_SCREENS=swap ZATACKA_FILTER=smooth
#
# Det enheten har som detta inte täcker: Banana-SDL:s KMS-del (BP_Init/BP_Present), evdev-input, pekskärmen,
# bakgrundsbelysningen och prestanda.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mkdir -p "${1:?utmapp}" && cd "$1" && pwd)
HOW=${2:-host}
shift $(( $# >= 2 ? 2 : 1 ))
SCRIPT=$(grep -v '^#' "$ROOT/scripts/autoplay.script" | tr -s ' \n' ',' | sed 's/^,//; s/,$//')
rm -f "$OUT"/zatacka-panels-*.ppm "$OUT"/zatacka-panels-*.png
case $HOW in
    host) RUN=("$ROOT/work/host/zatacka") ;;
    qemu) RUN=(qemu-aarch64 -L /usr/aarch64-linux-gnu -E "LD_LIBRARY_PATH=$ROOT/work/sdl-aarch64/lib"
               "$ROOT/port/zatacka/zatacka") ;;
    *) echo "host|qemu" >&2; exit 2 ;;
esac
env SDL_VIDEODRIVER=dummy ZATACKA_WINDOW=1024x1536 ZATACKA_SHOT_DIR="$OUT" ZATACKA_SHOT_TRIGGER= \
    ZATACKA_SEED=original ZATACKA_LABELS=pad ZATACKA_SCRIPT="$SCRIPT" "$@" timeout 90 "${RUN[@]}" 2>&1 | tee "$OUT/zatacka.log"
for f in "$OUT"/zatacka-panels-*.ppm; do
    python3 -I - "$f" <<'EOF'
import sys, zlib, struct
src = sys.argv[1]
d = open(src, 'rb').read()
hdr = d.split(b'\n', 3)
w, h = map(int, hdr[1].split()); px = hdr[3][:w*h*3]
raw = b''.join(b'\0' + px[y*w*3:(y+1)*w*3] for y in range(h))
c = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(src[:-4] + '.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + c(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                                    + c(b'IDAT', zlib.compress(raw, 6)) + c(b'IEND', b''))
EOF
    rm -f "$f"
done
ls "$OUT"

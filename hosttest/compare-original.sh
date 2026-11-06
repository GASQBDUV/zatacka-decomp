#!/bin/bash
# Pixeljämförelse mot originalet: en tvåspelarrunda (röd + gul, ingen styrning) med originalets frö slutar i
# samma bild som ZATACKA.EXE i DOSBox (reference/dosbox-original/round.png). Kör efter ändringar i
# src/zatacka.c eller src/bgi.c. Kräver ImageMagick (convert) och ./scripts/build.sh host.
#
#   ./hosttest/compare-original.sh        förväntat: ~39 avvikande pixlar, alla i resultatsiffrorna
#                                         (x 579-595), inga i spelplanen (x < 559)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
env SDL_VIDEODRIVER=dummy ZATACKA_WINDOW=640x480 ZATACKA_SCREENS=single ZATACKA_SEED=original ZATACKA_STATS=0 \
    ZATACKA_SHOT_DIR="$T" ZATACKA_SHOT_TRIGGER= \
    ZATACKA_SCRIPT="1100:+space,1200:-space,1400:+p0l,1500:-p0l,1600:+p1l,1700:-p1l,1900:+space,2000:-space,9000:shot,9100:quit" \
    "$ROOT/work/host/zatacka" >/dev/null 2>&1
convert "$T/zatacka-panels-000.ppm" "$T/port.png"
convert "$T/port.png" "$ROOT/reference/dosbox-original/round.png" -compose difference -composite -threshold 1% "$T/d.png"
ALL=$(convert "$T/d.png" -format '%[fx:round(mean*w*h)]' info:)
FIELD=$(convert "$T/d.png" -crop 559x480+0+0 +repage -format '%[fx:round(mean*w*h)]' info:)
echo "avvikande pixlar: $ALL totalt, $FIELD i spelplanen"
[ "$FIELD" -eq 0 ] || { cp "$T/port.png" "$T/d.png" /tmp/ 2>/dev/null; echo "SPELPLANEN SKILJER (bilder: /tmp/port.png, /tmp/d.png)"; exit 1; }

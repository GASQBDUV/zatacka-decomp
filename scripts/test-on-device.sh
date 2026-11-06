#!/bin/bash
# Startar Zatacka på enheten via menyn, tar skärmdumpar av båda panelerna och hämtar loggarna.
#
#   DEVICE=root@<ip> ./scripts/test-on-device.sh            automatiskt: inputskript spelar själv
#   DEVICE=root@<ip> ./scripts/test-on-device.sh --manual 60   du spelar; dump var 15:e sekund i 60 s
#
# Resultat i test-out/<tid>/: shot-NN.png (hela fönstret: övre halvan = övre panelen), zatacka.log, ui.log,
# bananad.log, stats.tsv (RSS, CPU-tid, belysning per 3 s). Titta på bilderna med Read-verktyget.
#
# Det automatiska skriptet (ZATACKA_SCRIPT, se platform.c) trycker START, låter röd, gul och blå (pek) gå med,
# startar, styr röd (vänster) och blå (pek höger), dumpar i varje fas och avslutar sig självt.
# ZATACKA_SEED=original gör partiet deterministiskt, så bilderna ska likna värdtestets (hosttest/run-host-test.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
: "${DEVICE:?sätt DEVICE=root@<ip>}"
MANUAL=0
SECS=0
ENTRY=/data/roms/ports/Zatacka.sh
while [ $# -gt 0 ]; do
    case $1 in
        --manual) MANUAL=1 ;;
        --entry) ENTRY=$2; shift ;;
        [0-9]*) SECS=$1 ;;
        *) echo "okänt argument: $1" >&2; exit 2 ;;
    esac
    shift
done
SSH=(ssh -o ConnectTimeout=8 -o BatchMode=yes "$DEVICE")
OUT=test-out/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"

AUTO_SCRIPT=$(grep -v '^#' scripts/autoplay.script | tr -s ' \n' ',' | sed 's/^,//; s/,$//')

"${SSH[@]}" "test -x /data/zatacka/zatacka && test -f '$ENTRY'" ||
    { echo "Zatacka finns inte på enheten ($ENTRY). Kör deploy.sh först." >&2; exit 1; }
if "${SSH[@]}" 'test -s /run/banana/game.pid'; then
    echo "Ett spel körs redan (/run/banana/game.pid). Avsluta det, eller: ssh $DEVICE 'kill -TERM -\$(cut -d\" \" -f1 /run/banana/game.pid)'" >&2
    exit 1
fi

"${SSH[@]}" 'mkdir -p /data/screenshots && rm -f /data/screenshots/zatacka-panels-*.ppm /run/banana/zatacka-shot'
if [ "$MANUAL" = 0 ]; then
    [ "$SECS" -gt 0 ] || SECS=25
    printf 'ZATACKA_SEED=original\nZATACKA_SCRIPT="%s"\n' "$AUTO_SCRIPT" | "${SSH[@]}" 'cat > /data/zatacka/test.env'
else
    [ "$SECS" -gt 0 ] || SECS=60
    "${SSH[@]}" 'rm -f /data/zatacka/test.env'
fi

echo "== startar $ENTRY via menyn (/run/banana/launch)"
"${SSH[@]}" "echo '$ENTRY' > /run/banana/launch"

# vänta på att spelet startat
for _ in $(seq 1 20); do
    "${SSH[@]}" 'test -s /run/banana/game.pid' && break
    sleep 0.5
done
"${SSH[@]}" 'test -s /run/banana/game.pid' || { echo "spelet startade inte; se ui.log" >&2; }

printf 't\trss_kb\tcpu_ticks\tbl_power(alla)\tloadavg\n' > "$OUT/stats.tsv"
T0=$(date +%s)
NEXT_SHOT=15
while :; do
    T=$(( $(date +%s) - T0 ))
    [ "$T" -ge "$SECS" ] && break
    LINE=$("${SSH[@]}" 'p=$(pidof zatacka 2>/dev/null | cut -d" " -f1); [ -n "$p" ] || { echo -; exit; }
        rss=$(awk "/VmRSS/{print \$2}" /proc/$p/status); cpu=$(awk "{print \$14+\$15}" /proc/$p/stat)
        bl=$(cat /sys/class/backlight/*/bl_power 2>/dev/null | tr "\n" " ")
        echo "$rss	$cpu	$bl	$(cut -d" " -f1-3 /proc/loadavg)"' || echo -)
    [ "$LINE" = - ] && { echo "   ($T s) zatacka körs inte längre"; break; }
    printf '%s\t%s\n' "$T" "$LINE" >> "$OUT/stats.tsv"
    if [ "$MANUAL" = 1 ] && [ "$T" -ge "$NEXT_SHOT" ]; then
        "${SSH[@]}" 'touch /run/banana/zatacka-shot'
        NEXT_SHOT=$((NEXT_SHOT + 15))
    fi
    sleep 3
done

if "${SSH[@]}" 'test -s /run/banana/game.pid'; then
    if [ "$MANUAL" = 1 ]; then "${SSH[@]}" 'touch /run/banana/zatacka-shot'; sleep 1.5; fi
    echo "== avslutar (SIGTERM till processgruppen)"
    "${SSH[@]}" 'kill -TERM -$(cut -d" " -f1 /run/banana/game.pid) 2>/dev/null; sleep 2; test -s /run/banana/game.pid && echo "game.pid finns kvar!" || true'
fi

echo "== hämtar dumpar och loggar till $OUT"
for f in $("${SSH[@]}" 'ls /data/screenshots/zatacka-panels-*.ppm 2>/dev/null' || true); do
    n=$(basename "$f" .ppm | sed 's/zatacka-panels-//')
    "${SSH[@]}" "cat '$f'" > "$OUT/shot-$n.ppm"
done
"${SSH[@]}" 'cat /data/banana/logs/zatacka/zatacka.log' > "$OUT/zatacka.log" 2>/dev/null || true
"${SSH[@]}" 'tail -n 80 /data/banana/logs/banana-ui.log' > "$OUT/ui.log" 2>/dev/null || true
"${SSH[@]}" 'tail -n 40 /data/banana/logs/bananad.log' > "$OUT/bananad.log" 2>/dev/null || true
"${SSH[@]}" 'rm -f /data/zatacka/test.env'

# PPM -> PNG utan beroenden
python3 - "$OUT" <<'EOF'
import sys, os, zlib, struct, glob
def ppm2png(src, dst):
    d = open(src, 'rb').read()
    parts, pos = [], 0
    while len(parts) < 4:
        while d[pos:pos+1].isspace(): pos += 1
        if d[pos:pos+1] == b'#':
            pos = d.index(b'\n', pos); continue
        start = pos
        while not d[pos:pos+1].isspace(): pos += 1
        parts.append(d[start:pos])
    pos += 1
    w, h = int(parts[1]), int(parts[2])
    px = d[pos:pos + w*h*3]
    raw = b''.join(b'\0' + px[y*w*3:(y+1)*w*3] for y in range(h))
    chunk = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(dst, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                          + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
for f in sorted(glob.glob(os.path.join(sys.argv[1], 'shot-*.ppm'))):
    ppm2png(f, f[:-4] + '.png'); os.remove(f); print('   ' + os.path.basename(f[:-4]) + '.png')
EOF
echo "== slutet av zatacka.log:"
tail -n 15 "$OUT/zatacka.log" | sed 's/^/   /'
echo "== $OUT"

#!/bin/bash
# Lägger Zatacka på enheten över SSH: /data/zatacka (program) och /data/roms/ports/Zatacka.sh (Ports-post).
#
#   DEVICE=root@<ip> ./scripts/deploy.sh [--mode dual|swap|single] [--shelf] [--no-ports]
#
#   --mode     skriver /data/zatacka/zatacka.conf med ZATACKA_SCREENS (annars rörs en befintlig conf inte)
#   --shelf    egen hylla "Zatacka" i menyn (lägger [zatacka] i /data/banana/systems.conf; syns efter omstart
#              av menyn). Ändrar bara den filen, och bara om sektionen saknas.
#   --no-ports ingen Ports-post (t.ex. med --shelf)
#
# Ingen scp/rsync behövs: tar | ssh. Kortet är exFAT: inga rättigheter eller symlänkar, allt är körbart.
set -euo pipefail
cd "$(dirname "$0")/.."
: "${DEVICE:?sätt DEVICE=root@<ip>}"
MODE='' SHELF=0 PORTS=1
while [ $# -gt 0 ]; do
    case $1 in
        --mode) MODE=$2; shift ;;
        --shelf) SHELF=1 ;;
        --no-ports) PORTS=0 ;;
        *) echo "okänt argument: $1" >&2; exit 2 ;;
    esac
    shift
done
case $MODE in ''|dual|swap|single) ;; *) echo "--mode: dual|swap|single" >&2; exit 2 ;; esac

[ -x port/zatacka/zatacka ] && [ -f port/zatacka/TSCR.CHR ] || ./scripts/build.sh aarch64
SSH=(ssh -o ConnectTimeout=8 -o BatchMode=yes "$DEVICE")

if "${SSH[@]}" 'test -s /run/banana/game.pid' 2>/dev/null; then
    echo "Ett spel körs på enheten (/run/banana/game.pid). Avsluta det först." >&2
    exit 1
fi

echo "== /data/zatacka"
tar -C port/zatacka -cf - zatacka TSCR.CHR run-zatacka.sh zatacka.conf.example |
    "${SSH[@]}" 'mkdir -p /data/zatacka && tar -xf - -C /data/zatacka && ls -l /data/zatacka'

if [ -n "$MODE" ]; then
    echo "== zatacka.conf: ZATACKA_SCREENS=$MODE"
    printf '# deploy.sh --mode %s\nZATACKA_SCREENS=%s\n' "$MODE" "$MODE" | "${SSH[@]}" 'cat > /data/zatacka/zatacka.conf'
fi

if [ "$PORTS" = 1 ]; then
    echo "== /data/roms/ports/Zatacka.sh"
    "${SSH[@]}" 'mkdir -p /data/roms/ports && cat > /data/roms/ports/Zatacka.sh' < port/Zatacka.sh
fi

if [ "$SHELF" = 1 ]; then
    echo "== egen hylla [zatacka]"
    "${SSH[@]}" 'mkdir -p /data/zatacka/shelf && : > "/data/zatacka/shelf/Achtung die Kurve.zatacka"'
    if "${SSH[@]}" 'grep -q "^\[zatacka\]" /data/banana/systems.conf 2>/dev/null'; then
        echo "   finns redan i /data/banana/systems.conf"
    else
        "${SSH[@]}" 'cat >> /data/banana/systems.conf' < port/systems.conf.snippet
        echo "   tillagd i /data/banana/systems.conf (syns när menyn startats om, t.ex. efter omstart)"
    fi
fi

echo "== klart. Starta från Ports (eller: DEVICE=$DEVICE ./scripts/test-on-device.sh)"

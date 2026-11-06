#!/bin/sh
# Startar Zatacka (Achtung, die Kurve!) på BananaOS över båda panelerna.
# Körs från Ports-posten (/data/roms/ports/Zatacka.sh, via run-port) eller från en egen hylla (systems.conf).
#
# Inställningar: /data/zatacka/zatacka.conf (skal-syntax, se zatacka.conf.example), t.ex. ZATACKA_SCREENS=swap.
# Testläge: /data/zatacka/test.env läses EN gång och tas sedan bort (test-on-device.sh lägger ett inputskript där).
D=$(cd "$(dirname "$0")" && pwd)
LOGDIR=/data/banana/logs/zatacka
mkdir -p "$LOGDIR"
LOG=$LOGDIR/zatacka.log
# behåll förra körningens logg
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.1"
exec >"$LOG" 2>&1
echo "run-zatacka: start $(date '+%F %T'), $D, argument: $*"

[ -f "$D/zatacka.conf" ] && . "$D/zatacka.conf"
if [ -f "$D/test.env" ]; then
    . "$D/test.env"
    rm -f "$D/test.env"
    echo "run-zatacka: testläge (test.env)"
fi
: "${ZATACKA_SCREENS:=dual}"
export ZATACKA_SCREENS ZATACKA_FILTER ZATACKA_SEED ZATACKA_LABELS ZATACKA_STATS ZATACKA_KEYBOARD ZATACKA_SCRIPT ZATACKA_SHOT_DIR

# run-port släcker den nedre panelens bakgrundsbelysning (one_panel_dark top) och skriver dess sökväg i
# /run/banana/panel-off. bananad släcker den igen efter varje uppvakning så länge filen finns. Vi använder båda
# panelerna: tänd den och ta bort filen. Med ZATACKA_SCREENS=single får den vara släckt.
if [ "$ZATACKA_SCREENS" != single ] && [ -f /run/banana/panel-off ]; then
    BL=$(cat /run/banana/panel-off)
    rm -f /run/banana/panel-off
    [ -w "$BL/bl_power" ] && echo 0 > "$BL/bl_power" && echo "run-zatacka: tände $BL"
fi

# Banana-SDL: dummy-drivrutinen med evdev-input och båda panelerna som ett fönster (övre panelen överst).
# Mjukvaruyta, ingen GL: varje bildruta går direkt till panelerna (DRM dumb buffers, page flip), touch på den nedre
# panelen hamnar i fönstrets nedre halva. Samma väg som Poketool (run-bank) använder.
export SDL_VIDEODRIVER=evdev SDL_BANANA_PANELS=stack SDL_BANANA_TOP="${BANANA_TOP:-DSI-2}"
unset SDL_KMSDRM_FIRST
cd "$D" || exit 1
"$D/zatacka"
S=$?
echo "run-zatacka: exit $S"
exit $S

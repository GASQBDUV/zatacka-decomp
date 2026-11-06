# Zatacka (Achtung, die Kurve!) med två skärmar på BananaOS (Anbernic RG DS Plus)

Projektet är en **native port** av `GASQBDUV/zatacka-decomp`, den byte-identiska C++-rekonstruktionen av
DOS-spelet `ZATACKA.EXE` ("Achtung, die Kurve!", ca 1995), till **BananaOS** på **Anbernic RG DS Plus**
(fungerar i princip även på RG DS). Spelet visas på den övre panelen. Den nedre panelen visar en stor
resultattavla, lobbyhjälp med knapparna och pekzoner för blå spelare, som styrde med musen i originalet.

Läs `uzdoom-bananaos/CLAUDE.md` för det allmänt BananaOS-specifika (SSH, `/run/banana/launch`, `game.pid`,
run-port, panel-off, exFAT). Här upprepas bara det som skiljer.

---

## STATUS (uppdatera när du jobbar)

<!-- STATUS-START -->
**Läge 2026-11-06 (gjort i molnet, ingen tillgång till hårdvaran):**

- ✅ Spelkoden är portad med minimala ändringar (`src/zatacka.c`, varje ändring märkt `BANANA:`). Originalet
  ligger kvar i `reference/zatacka.cpp` för diff. Spellogiken är oförändrad, buggarna inräknade.
- ✅ **Pixelidentisk med originalet i DOSBox:** en hel tvåspelarrunda med `ZATACKA_SEED=original` slutar i
  samma bild som originalet (`reference/dosbox-original/round.png` mot `reference/host-test/round-640x480-som-originalet.png`):
  samma startpositioner (Borlands `rand()`), svansar, hål och död. Bara 39 pixlar skiljer, alla inom resultatsiffrorna
  (strokefontens linjerastrering). Splashtexten: 161 av ~3000 pixlar, ±1 px.
- ✅ **aarch64-bygget lyckas** (`port/zatacka/zatacka`, 72 KB). Det behöver bara `libSDL2-2.0.so.0`, libm och
  libc, GLIBC ≤ 2.34. Under qemu-aarch64 ger det **bit för bit samma** paneldumpar som x86_64-bygget med
  samma inputskript.
- ✅ Startskriptet är provkört lokalt med låtsas-`/data`: det tänder den släckta nedre panelen, tar bort
  `/run/banana/panel-off`, läser `test.env` en gång och kör med `SDL_VIDEODRIVER=evdev`.
- ⬜ **Inte testat på riktig hårdvara.** Det som inte kunde testas i molnet: Banana-SDL:s `BP_Init`/`BP_Present`
  (DRM), evdev-input (joypad, pekskärm), bakgrundsbelysningen, sömn/lock och prestanda.
  **Nästa steg:** `build.sh` → `deploy.sh` → `test-on-device.sh` → gå igenom *Testchecklista*.
<!-- STATUS-END -->

---

## Snabbstart för Claude på hemdatorn

1. **Bygg:** `./scripts/build.sh` (aarch64 + paketering i `port/zatacka/`) och `./scripts/build.sh host`
   (x86_64 för testriggen). Kräver Ubuntu 24.04 x86_64 med `gcc-aarch64-linux-gnu libc6-dev-arm64-cross cmake`
   (och `qemu-user` för röktest). SDL2 2.32.10 byggs bara för att länka. Spelkällan (för `TSCR.CHR`) klonas
   till `work/zatacka-decomp`, låst på commiten i `reference/decomp-commit.txt`.
2. **Värdtest först** (sekunder, utan enheten): `./hosttest/run-host-test.sh /tmp/zt host` och
   `./hosttest/run-host-test.sh /tmp/zq qemu`. Titta på `/tmp/zt/zatacka-panels-*.png` med Read. Jämför med
   `reference/host-test/`.
3. **Enheten:** be användaren om IP-adressen. SSH är root med nyckel, se uzdoom-projektet.
   `export DEVICE=root@<ip>`, testa med `ssh $DEVICE uname -a`.
4. **Deploy:** `./scripts/deploy.sh` (`--mode swap|single`, `--shelf` för en egen hylla).
   Inga spelfiler behövs: spelet är fritt, och `TSCR.CHR` följer med från decomp-repot.
5. **Testa:** `./scripts/test-on-device.sh` spelar automatiskt (samma inputskript som värdtestet,
   `scripts/autoplay.script`), dumpar båda panelerna i varje fas till `test-out/<tid>/shot-NN.png` och hämtar
   loggar och `stats.tsv`. **Titta på bilderna med Read.** Övre halvan är den övre panelen.
   `--manual 60` låter användaren spela och dumpar var 15:e sekund.

---

## Mappstruktur

```
CLAUDE.md                       den här filen
src/zatacka.c                   spelet: decompens zatacka.cpp + "BANANA:"-ändringar
src/bgi.{h,c}                   BGI-delmängden på en indexerad 640x480-yta (två sidor) + TSCR.CHR-strokefonten
src/font8x8.c                   BGI:s DEFAULT_FONT ersatt med dhepper/font8x8 (Public Domain, IBM VGA)
src/platform.{h,c}              SDL2: fönster över båda panelerna, skalning, timing, joypad/pek/tangentbord,
                                andra panelens innehåll, skärmdumpar, testskript
scripts/build.sh                SDL2 (länkning) + spelet för aarch64 eller värden
scripts/deploy.sh               tar | ssh -> /data/zatacka + /data/roms/ports/Zatacka.sh (+ --mode, --shelf)
scripts/test-on-device.sh       start via menyn, skriptad input, paneldumpar, loggar, stats
scripts/autoplay.script         inputskriptet för automatiska tester (värd och enhet)
hosttest/run-host-test.sh       samma test utan enhet: x86_64 eller aarch64 under qemu, 1024x1536
hosttest/compare-original.sh    pixeljämförelse mot ZATACKA.EXE i DOSBox (spelplanen ska vara identisk)
port/Zatacka.sh                 Ports-posten (run-port kör den med bash)
port/systems.conf.snippet       [zatacka]-hylla för /data/banana/systems.conf (deploy.sh --shelf)
port/zatacka/run-zatacka.sh     startskriptet (panelbelysning, Banana-SDL-miljö, conf, test.env, logg)
port/zatacka/zatacka.conf.example  inställningar
port/zatacka/{zatacka,TSCR.CHR} byggda filer
reference/zatacka.cpp           originalkällan (decomp) för diff mot src/zatacka.c
reference/dosbox-original/      originalet i DOSBox (splash, slutet av en tvåspelarrunda)
reference/host-test/            förväntade paneldumpar från autoplay-skriptet och jämförelsebilden
reference/*-commit.txt          låsta versioner: zatacka-decomp, BananaLinux (källan som lästes)
```

---

## Arkitektur – det viktiga

### Originalet

`ZATACKA.EXE` är Borland C++ 3.1, small model, BGI-grafik i 640x480x16 (VGAHI). Spelkoden är ~500 rader.
Den tar över tangentbordsinterruptet (INT 9, scancodes från port 60h) för fem tangentbordsspelare, läser
musen via INT 33h för den sjätte (blå), och väntar med `delay()` i busy-loopar. **Kollisionen läser skärmen:**
`getpixel(x, y) > 7`, dvs. allt i spelarfärg (8..13), resultatgrått (14) eller text (15) är hinder. Därför
måste grafiken vara en riktig indexerad yta med BGI:s exakta pixelsemantik. Ett GL-lager räcker inte.

### Porten: BGI i minnet + Banana-SDL:s mjukvarufönster

```
banana-ui släpper DRM ─▶ run-port ─▶ Zatacka.sh ─▶ run-zatacka.sh:
    tänder nedre panelen, SDL_VIDEODRIVER=evdev SDL_BANANA_PANELS=stack
  ─▶ zatacka: ETT fönster 1024x1536 (mjukvaruyta, ingen GL)
       sida 0 (BGI, 640x480 index) = spelet          ─┐  skalas 1,6x (Plus: exakt 1024x768) till sin panel,
       sida 1 (BGI, 640x480 index) = andra panelen   ─┘  bara smutsrektangeln, högst ~60 ggr/s
     SDL_UpdateWindowSurface ─▶ Banana-SDL BP_Present: båda halvorna till var sin panel (dumb buffers,
     page flip i takt med 60 Hz). Pek på nedre panelen ─▶ fönstrets nedre halva (BP_MapTouch).
```

- **Varför dummy/evdev och inte offscreen/GLES som UZDoom:** Banana-SDL:s dummy-drivrutin
  (`buildroot/patches/sdl2/0001-banana-panels.patch` i BananaLinux) lägger mjukvarufönstret direkt på båda
  panelerna. Drivrutinsnamnet `evdev` är dummy plus evdev-input (namnet `dummy` läser **ingen** input).
  Poketool (`run-bank`) använder samma väg. Ett 2D-spel med pixelkollision behöver ingen GPU.
- **Skalning:** närmaste granne via förberäknade tabeller (`make_maps`), bara den ändrade rektangeln. 1,6x ger
  ojämna 4/5 px breda linjer. `ZATACKA_FILTER=smooth` ger ytviktad skalning med jämnare linjer. På RG DS
  (640x480-paneler) blir det 1:1.
- **Timing:** Borlands `delay(ms)` blir en deadline-kedja (`delay()` i platform.c). Ticket blir exakt
  `15 - levande/2` ms, som på en snabb DOS-maskin. Bildrutor visas högst var 16:e ms och bara när något
  ändrats. Ligger kedjan mer än 100 ms efter (sömn, väntan på START) börjar den om i stället för att köra ikapp.
- **Slump:** Borlands LCG (`0x015A4E35 * seed + 1`) och C++-`random(n)`. Originalet anropar aldrig `srand`,
  så varje parti har samma startpositioner. `ZATACKA_SEED=original` ger det. Standard är tidsseedat.
- **Bevarade buggar:** bara röd får hålntimer vid start, och de andras första tick lägger en nollad
  `img_bg` som en svart 1x1-pixel. Därför `calloc` för `img_bg`, och BGI-bildbuffertar har BGI:s huvud
  (bredd-1, höjd-1).
- **Strokefonten:** `TSCR.CHR` läses från fil, som originalet. Två detaljer är uppmätta mot DOSBox: koordinater
  skalas med **heltalstrunkering** (`sx * mx / dx`), och `TOP_TEXT` lägger baslinjen `(cap - dec) * skala`
  under y. Med versalhöjd i stället hamnade texten 14 px för högt.

### Input

Positionsbaserat via SDL GameController. Mappningen för `retrogame_joypad` (GUID `190000004b4800000111000000010000`)
läggs in i koden (`RGDS_MAPPING`), med knappnumren från BananaOS:s `retrogame_joypad.cfg`: b0 B, b1 A, b2 X, b3 Y,
b4 L1, b5 R1, b6 L2, b7 R2, b8 select, b9 start, b10 F, b13–16 styrkorset. En joystick som SDL inte känner igen
läses med samma rånummer.

| Spelare | Original | RG DS | vänster / höger |
|---|---|---|---|
| Röd | 1 / Q | styrkorset (och spaken) | ◀ / ▶ |
| Gul | L.Ctrl / L.Alt | Y / A | |
| Orange | M / , | L2 / L1 | |
| Grön | ←  / ↓ | R1 / R2 | |
| Rosa | numpad / * | X / B | |
| Blå | vänster/höger musknapp | **pek på nedre panelen**, vänster/höger halva (två fingrar = mittenknappen) | |
| SPACE | | START | |
| ESC | | SELECT | |
| (nytt) | | SELECT+START i 0,7 s: avsluta var som helst. F: skärmdump | |

Vänsterknappen går med i lobbyn och högerknappen lämnar. START startar med minst två spelare. Originalets
tangenter fungerar också (USB-tangentbord, värden). Korta tryck mellan två pumpar "spärras" (`latch`), så att
inget tryck missas.

### Andra panelen (sida 1)

Den ritas av `draw_companion()` med samma BGI-funktioner, bara när något den visar ändrats (`companion_signature`).
Spelet anger fasen med `plat_phase()` vid sina etiketter:
splash (knapphjälp), lobby (spelare, knappar, READY, pekzoner "join/leave"), runda (stor resultattavla, utslagna
dimmade, pekzoner för blå), mellan rundor ("START: next round" / "X WINS!"), game over. Swap-läget byter bara
vilken sida som hamnar på vilken panel.

---

## Testchecklista på enheten

Kör `test-on-device.sh` och titta på `shot-*.png`, `zatacka.log` och `stats.tsv`:

- [ ] Loggen: `videodrivrutin evdev, SDL_BANANA_PANELS=stack` och `fönster 1024x1536: två paneler över varandra`.
- [ ] Loggen: `handkontroll 0: "…" GUID 190000004b48…, GameController`.
- [ ] Shot 0 (splash): titeln uppe, knapphjälpen nere. Shot 1 (lobby): RED/YELLOW/BLUE READY på båda.
      Shot 2–4: banan uppe, resultattavlan nere. Jämför med `reference/host-test/dual-*.png` (spelet är
      deterministiskt, men timingen på enheten kan ge små skillnader).
- [ ] Det som syns **på panelerna** stämmer med dumparna (dumpen är fönstrets yta, inte vad DRM visar):
      rätt panel, inte upp och ner, ingen förskjutning.
- [ ] Den nedre panelen är tänd under spelet och efter lock/sömn (`bl_power` 0 för båda i `stats.tsv`).
- [ ] Handkontroll i spel (`--manual`): röd = styrkorset, gul = Y/A, orange = L2/L1, grön = R1/R2, rosa = X/B.
      START/SELECT som SPACE/ESC. SELECT+START (håll) avslutar. **Om vänster på korset även styr grön** syns
      joypaden som tangentbord: loggen säger `första tangenthändelsen: Left`. Sätt `ZATACKA_KEYBOARD=0` i
      `/data/zatacka/zatacka.conf`.
- [ ] Pek: blå går med (vänster halva) och lämnar (höger) i lobbyn och styr i spelet. Zonen lyser när den trycks.
- [ ] Prestanda: `stats:`-raden var 30:e sekund. Den ska visa ~70 tick/s med 2 levande (14 ms tick), ~60
      bildrutor/s under en runda och låg ms per bildruta. CPU-tiden i `stats.tsv` ska växa långsamt.
- [ ] Avslut: game over → SELECT, eller SELECT+START. Menyn kommer tillbaka och loggen slutar med
      `run-zatacka: exit 0`.
- [ ] RG DS (inte Plus), om det finns en: fönstret blir 640x960 och spelet 1:1.

### Felsökning

| Symptom | Sannolik orsak / åtgärd |
|---|---|
| svart på båda, loggen säger `SDL_BANANA_PANELS: two panels not found` / `no DRM master` | menyn höll fortfarande DRM: starta **via menyn** (`/run/banana/launch`), inte direkt från SSH |
| `fönster 1024x768: en panel` | `SDL_BANANA_PANELS` nådde inte programmet, eller SDL är inte Banana-SDL (`ldd`-ordningen, `LD_LIBRARY_PATH`) |
| ingen input alls | drivrutinen blev `dummy` i stället för `evdev` (dummy läser ingen input) |
| knappar på fel spelare | `RGDS_MAPPING`/`raw_index` i platform.c. Loggen visar namn, GUID och antal knappar |
| nedre panelen släckt | `/run/banana/panel-off` kom tillbaka (run-port efter oss?) eller `ZATACKA_SCREENS=single` |
| pek gör ingenting | pekhändelser kommer som `SDL_FINGER*` (Banana mappar till fönstrets nedre halva). Logga i `finger()` |
| hackigt | `stats:`-raden. Prova `ZATACKA_FILTER` av (standard), kontrollera att perf-profilen är `performance` (Ports) |

---

## Kända begränsningar och idéer

- **Originalets avslut:** ESC fungerar bara på splash, mellan rundor och vid game over. När någon vunnit väntar
  originalet bara på SPACE. SELECT+START är vårt tillägg för att alltid kunna avsluta.
- **Mittenklick:** två fingrar samtidigt på var sin halva motsvarar mittenknappen (båda hållen på en gång), som
  i originalet.
- **Flyttal:** originalet räknade med 8087-emulatorn (80-bitars mellanresultat), porten med double. En hel runda
  var pixelidentisk ändå, men små avvikelser efter långa rundor kan inte uteslutas.
- **Idéer:** fler handkontroller (en spelare per extra handkontroll via Bluetooth/USB), stora pekzoner för fler
  spelare på nedre panelen, ljud saknas helt (originalet hade inget).

---

## Arbetsregler för Claude i det här projektet

Samma som i `uzdoom-bananaos/CLAUDE.md`:

- Ändra aldrig BananaOS-systemet. Allt ligger under `/data`. `/data/banana/systems.conf` ändras bara med
  `deploy.sh --shelf`, när användaren vill det.
- Starta via menyn (`/run/banana/launch`). Avsluta ett hängt test med `kill -TERM -$(cut -d" " -f1 /run/banana/game.pid)`.
- Uppdatera STATUS-blocket när något verifierats på hårdvaran.

Tillägg för det här projektet:

- Ändringar i spellogiken (`src/zatacka.c`) märks `BANANA:` och ska vara så små som möjligt. Kör
  `./hosttest/compare-original.sh` efter ändringar i `zatacka.c` eller `bgi.c`: spelplanen ska ha 0 avvikande
  pixlar mot originalet i DOSBox.
- Licenser: zatacka-decomp är MIT (Simon Alling, Simon Lydell). `TSCR.CHR` är Borlands fontfil från decomp-repot
  och följer bara med till användarens egen enhet. font8x8 är Public Domain. Porten följer MIT.

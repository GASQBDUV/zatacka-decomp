/*
 * Plattformslagret: SDL2-fönster (på BananaOS över båda panelerna), input, timing och den andra panelens innehåll.
 * Ersätter DOS-delarna i ZATACKA.EXE (tangentbords-ISR, INT 33h, delay()).
 */
#ifndef ZATACKA_PLATFORM_H
#define ZATACKA_PLATFORM_H

enum {
    PHASE_SPLASH,
    PHASE_LOBBY,
    PHASE_ROUND,
    PHASE_ROUND_OVER,
    PHASE_GAME_OVER,
};

void plat_init(int argc, char **argv);
const char *plat_data_dir(void);   /* katalogen med TSCR.CHR (binärens katalog) */

void plat_pump(void);              /* läser input, visar en bildruta när det är dags */
void plat_idle(void);              /* plat_pump + en kort paus (spelets väntslingor) */
void delay(unsigned ms);           /* som Borlands delay(), men med pump och presentation */

int plat_mouse_buttons(void);      /* som BX från INT 33h fn 3: bit 0 vänster, bit 1 höger, bit 2 mitten */
void plat_phase(int phase);
const char *plat_controls_text(int id, const char *keyboard_text);

void bc_srand(unsigned seed);      /* zatacka.c: Borlands rand() */

#endif

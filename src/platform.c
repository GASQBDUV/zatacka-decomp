/*
 * Plattformslagret för Zatacka på BananaOS (Anbernic RG DS / RG DS Plus) och andra SDL2-system.
 *
 * Bild: BGI-lagret ritar i två indexerade 640x480-sidor: sida 0 är spelets skärm, precis som i DOS, och sida 1
 * är andra panelens innehåll (lobbyhjälp, stor resultattavla, pekzoner). Varje sida skalas till sin panel i
 * fönstrets mjukvaruyta. Bara det som ändrats skalas om, och SDL_UpdateWindowSurface visar ytan.
 *
 * På BananaOS ger Banana-SDL:s dummy/evdev-drivrutin (SDL_VIDEODRIVER=evdev, SDL_BANANA_PANELS=stack) ett fönster
 * över båda panelerna (RG DS Plus: 1024x1536, den övre panelen är den övre halvan). Varje bildruta hamnar direkt på
 * panelerna (DRM dumb buffers, page flip i takt med 60 Hz), och pekskärmen mappas till fönstrets nedre halva.
 * 640x480 x 1,6 = 1024x768 fyller en Plus-panel exakt; på RG DS (640x480) blir det 1:1.
 *
 * Input: originalets tangenter (SDL-scancodes), handkontrollen (SDL GameController, positionsbaserat:
 * syd/öst/väst/nord) och pekskärmen eller musen för blå spelare (som styrde med musen i originalet).
 *
 * Timing: Borlands delay(ms) blir en deadline-schemaläggare. Spelets tick blir exakt 15 - levande/2 ms, som på
 * en snabb DOS-maskin, oberoende av när bildrutor visas (högst ~60/s).
 */
#include <SDL.h>

#include "bgi.h"
#include "platform.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef ZATACKA_GIT_REV
#define ZATACKA_GIT_REV "dev"
#endif

/* ---- spelets tillstånd (zatacka.c) ---- */
#define NPLAYERS 6
#define NKEYPLAYERS 5
extern volatile int key_esc, key_space;
extern volatile int keyL[NKEYPLAYERS], keyR[NKEYPLAYERS];
extern int score[NPLAYERS], joined[NPLAYERS], dead[NPLAYERS];
extern int participating_count, alive_count;

static const char *const player_name[NPLAYERS] = {"RED", "YELLOW", "ORANGE", "GREEN", "PINK", "BLUE"};

/* ---- inställningar (miljövariabler) ---- */
enum { SCREENS_DUAL, SCREENS_SWAP, SCREENS_SINGLE };
static int cfg_screens = SCREENS_DUAL;
static int cfg_smooth;
static int cfg_pad_labels;
static int cfg_stats;
static int cfg_keyboard = 1; /* ZATACKA_KEYBOARD=0: bortse från tangentbordet (om joypaden också syns som ett) */
static const char *cfg_shot_trigger = "/run/banana/zatacka-shot";
static const char *cfg_shot_dir = "/data/screenshots";
static char data_dir[PATH_MAX] = ".";

/* ---- fönster och vyer ---- */
static SDL_Window *win;
static int win_w, win_h;
typedef struct {
    SDL_Rect panel; /* panelens del av fönstret */
    SDL_Rect dst;   /* sidans 640x480 skalad, centrerad i panelen */
    int page;       /* -1 = svart */
    int *xmap, *ymap;          /* närmaste granne: målpixel -> källpixel */
    int *xinv, *yinv;          /* källpixel -> första målpixel (641/481 poster) */
    int *xw, *yw;              /* mjuk skalning: vikt (0..256) för källpixel map, resten till map+1 */
} View;
static View views[2];
static int nviews;
static int phase = PHASE_SPLASH;
static Uint32 lut[256];
static int lut_serial = -1;
static int need_full = 1;
static SDL_Surface *shadow; /* om fönstrets yta inte är 32 bitar */

/* ---- tid ---- */
static Uint64 perf_freq;
static Uint64 t_start_us, last_present_us, tick_deadline_us, last_trigger_check_us, last_stats_us;
static unsigned stat_ticks, stat_presents;
static Uint64 stat_present_us;

static Uint64 now_us(void)
{
    return (Uint64)((double)SDL_GetPerformanceCounter() * 1e6 / (double)perf_freq);
}

static void sleep_us(Uint64 us)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(us / 1000000);
    ts.tv_nsec = (long)(us % 1000000) * 1000;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) ;
}

/* ---- input ---- */
enum {
    IN_L0, IN_L1, IN_L2, IN_L3, IN_L4,
    IN_R0, IN_R1, IN_R2, IN_R3, IN_R4,
    IN_ESC, IN_SPACE,
    IN_MOUSE_L, IN_MOUSE_R, IN_MOUSE_M,
    IN_COUNT
};
static unsigned char latch[IN_COUNT];  /* tryckt sedan förra pumpen (korta tryck missas inte) */
static unsigned char script_held[IN_COUNT];
static int mouse_bits;                 /* riktig mus (inte pek) */
static int touch_bits;                 /* pekzoner: bit 0 vänster, bit 1 höger */
static Uint64 combo_since_us;

/* handkontroll, positionsbaserat (SDL:s a = syd, b = öst, x = väst, y = nord) */
enum { PB_LEFT, PB_RIGHT, PB_UP, PB_DOWN, PB_SOUTH, PB_EAST, PB_WEST, PB_NORTH, PB_L1, PB_R1, PB_L2, PB_R2,
       PB_SELECT, PB_START, PB_HOTKEY, PB_COUNT };
static const struct { int l, r; } pad_bind[NKEYPLAYERS] = {
    {PB_LEFT, PB_RIGHT},  /* röd:    styrkorset <  > (och spaken) */
    {PB_WEST, PB_EAST},   /* gul:    Y  A */
    {PB_L2, PB_L1},       /* orange: L2 L1 */
    {PB_R1, PB_R2},       /* grön:   R1 R2 */
    {PB_NORTH, PB_SOUTH}, /* rosa:   X  B */
};
static const char *const pad_labels[NPLAYERS] = {
    "(D-pad < >)", "(Y A)", "(L2 L1)", "(R1 R2)", "(X B)", "(Touch L R)"};
/* RG DS / RG DS Plus: retrogame_joypad (BananaOS:s retrogame_joypad.cfg: b0 B, b1 A, b2 X, b3 Y, b4 L1, b5 R1,
   b6 L2, b7 R2, b8 select, b9 start, b10 F, b11 L3, b12 R3, b13-16 upp/ned/vänster/höger, a0/a1 spaken) */
#define RGDS_MAPPING "190000004b4800000111000000010000,RetroGame Joypad,platform:Linux," \
    "a:b0,b:b1,x:b3,y:b2,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7," \
    "back:b8,start:b9,guide:b10,leftstick:b11,rightstick:b12,dpup:b13,dpdown:b14,dpleft:b15,dpright:b16," \
    "leftx:a0,lefty:a1,"
/* samma knappnummer för en joystick som SDL inte känner igen som handkontroll */
static const int raw_index[PB_COUNT] = {15, 16, 13, 14, 0, 1, 3, 2, 4, 5, 6, 7, 8, 9, 10};

#define MAXPADS 4
typedef struct { SDL_GameController *gc; SDL_Joystick *js; SDL_JoystickID id; unsigned char prev[PB_COUNT]; } Pad;
static Pad pads[MAXPADS];

#define MAXFINGERS 10
static struct { SDL_FingerID id; float x, y; int on; } fingers[MAXFINGERS];

/* testskript (ZATACKA_SCRIPT="ms:handling,..."), för automatiska tester utan händer */
typedef struct { Uint32 ms; int what; int on; } ScriptEv;
enum { SA_INPUT, SA_SHOT, SA_QUIT };
static ScriptEv *script;
static int script_n, script_pos;

static void shot(void);
static void present(void);

/* ===================================================================================================== */

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("zatacka: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int pad_button(Pad *p, int b)
{
    static const SDL_GameControllerButton gcb[PB_COUNT] = {
        SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_UP,
        SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X,
        SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_INVALID, SDL_CONTROLLER_BUTTON_INVALID, SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_GUIDE};
    if (p->gc) {
        if (b == PB_L2) return SDL_GameControllerGetAxis(p->gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
        if (b == PB_R2) return SDL_GameControllerGetAxis(p->gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
        if (b == PB_LEFT && SDL_GameControllerGetAxis(p->gc, SDL_CONTROLLER_AXIS_LEFTX) < -16000) return 1;
        if (b == PB_RIGHT && SDL_GameControllerGetAxis(p->gc, SDL_CONTROLLER_AXIS_LEFTX) > 16000) return 1;
        return SDL_GameControllerGetButton(p->gc, gcb[b]);
    }
    if (p->js) {
        if (b == PB_LEFT && SDL_JoystickNumAxes(p->js) > 0 && SDL_JoystickGetAxis(p->js, 0) < -16000) return 1;
        if (b == PB_RIGHT && SDL_JoystickNumAxes(p->js) > 0 && SDL_JoystickGetAxis(p->js, 0) > 16000) return 1;
        return raw_index[b] < SDL_JoystickNumButtons(p->js) && SDL_JoystickGetButton(p->js, raw_index[b]);
    }
    return 0;
}

static void pad_open(int device_index)
{
    int i;
    for (i = 0; i < MAXPADS && (pads[i].gc || pads[i].js); i++) ;
    if (i == MAXPADS) return;
    if (SDL_IsGameController(device_index)) {
        pads[i].gc = SDL_GameControllerOpen(device_index);
        if (!pads[i].gc) return;
        pads[i].js = SDL_GameControllerGetJoystick(pads[i].gc);
    } else {
        pads[i].js = SDL_JoystickOpen(device_index);
        if (!pads[i].js) return;
    }
    pads[i].id = SDL_JoystickInstanceID(pads[i].js);
    memset(pads[i].prev, 0, sizeof pads[i].prev);
    {
        char guid[33];
        SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(pads[i].js), guid, sizeof guid);
        logf_("handkontroll %d: \"%s\" GUID %s, %s, %d knappar, %d axlar", i, SDL_JoystickName(pads[i].js), guid,
              pads[i].gc ? "GameController" : "rå joystick (RG DS-numrering)", SDL_JoystickNumButtons(pads[i].js),
              SDL_JoystickNumAxes(pads[i].js));
    }
}

static void pad_close(SDL_JoystickID id)
{
    int i;
    for (i = 0; i < MAXPADS; i++)
        if ((pads[i].gc || pads[i].js) && pads[i].id == id) {
            if (pads[i].gc) SDL_GameControllerClose(pads[i].gc);
            else SDL_JoystickClose(pads[i].js);
            memset(&pads[i], 0, sizeof pads[i]);
            logf_("handkontroll %d borttagen", i);
        }
}

static int pad_already_open(int device_index)
{
    SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device_index);
    int i;
    for (i = 0; i < MAXPADS; i++)
        if ((pads[i].gc || pads[i].js) && pads[i].id == id) return 1;
    return 0;
}

/* vilken pekzon ett finger ligger i: fönstrets nedre panel (eller hela fönstret med en panel), vänster/höger halva */
static int touch_zone(float x, float y)
{
    if (nviews == 2 && y < 0.5f) return 0; /* den övre panelen har ingen pekskärm; ska inte hända */
    return x < 0.5f ? 1 : 2;
}

static void recompute_touch(void)
{
    int i;
    touch_bits = 0;
    for (i = 0; i < MAXFINGERS; i++)
        if (fingers[i].on) touch_bits |= touch_zone(fingers[i].x, fingers[i].y);
}

static void finger(SDL_TouchFingerEvent *e, int on)
{
    int i, freei = -1;
    for (i = 0; i < MAXFINGERS; i++) {
        if (fingers[i].on && fingers[i].id == e->fingerId) break;
        if (!fingers[i].on && freei < 0) freei = i;
    }
    if (i == MAXFINGERS) {
        if (!on || freei < 0) return;
        i = freei;
    }
    fingers[i].id = e->fingerId;
    fingers[i].x = e->x;
    fingers[i].y = e->y;
    fingers[i].on = on;
    recompute_touch();
    if (on && (touch_bits & 1)) latch[IN_MOUSE_L] = 1;
    if (on && (touch_bits & 2)) latch[IN_MOUSE_R] = 1;
}

static int key_input(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: return IN_ESC;
    case SDL_SCANCODE_SPACE: case SDL_SCANCODE_RETURN: return IN_SPACE;
    case SDL_SCANCODE_1: return IN_L0;
    case SDL_SCANCODE_Q: return IN_R0;
    case SDL_SCANCODE_LCTRL: return IN_L1;
    case SDL_SCANCODE_LALT: return IN_R1;
    case SDL_SCANCODE_M: return IN_L2;
    case SDL_SCANCODE_COMMA: return IN_R2;
    case SDL_SCANCODE_LEFT: return IN_L3;
    case SDL_SCANCODE_DOWN: return IN_R3;
    case SDL_SCANCODE_KP_DIVIDE: return IN_L4;
    case SDL_SCANCODE_KP_MULTIPLY: return IN_R4;
    default: return -1;
    }
}

static int input_by_name(const char *s)
{
    static const char *const names[IN_COUNT] = {
        "p0l", "p1l", "p2l", "p3l", "p4l", "p0r", "p1r", "p2r", "p3r", "p4r",
        "esc", "space", "bl", "br", "bm"};
    int i;
    for (i = 0; i < IN_COUNT; i++)
        if (!strcmp(s, names[i])) return i;
    return -1;
}

static void parse_script(const char *s)
{
    /* "1200:+space,1300:-space,5000:shot,9000:quit" (ms sedan start) */
    char *dup = strdup(s), *tok, *save = NULL;
    for (tok = strtok_r(dup, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
        char *colon = strchr(tok, ':');
        ScriptEv ev;
        if (!colon) continue;
        *colon = 0;
        ev.ms = (Uint32)strtoul(tok, NULL, 10);
        ev.on = 1;
        if (!strcmp(colon + 1, "shot")) ev.what = -SA_SHOT - 1;
        else if (!strcmp(colon + 1, "quit")) ev.what = -SA_QUIT - 1;
        else {
            const char *n = colon + 1;
            if (*n == '+' || *n == '-') { ev.on = *n == '+'; n++; }
            ev.what = input_by_name(n);
            if (ev.what < 0) { logf_("testskript: okänd input \"%s\"", n); continue; }
        }
        script = realloc(script, sizeof *script * (script_n + 1));
        script[script_n++] = ev;
    }
    free(dup);
    logf_("testskript: %d händelser", script_n);
}

static void run_script(void)
{
    Uint32 t = (Uint32)((now_us() - t_start_us) / 1000);
    while (script_pos < script_n && script[script_pos].ms <= t) {
        ScriptEv *e = &script[script_pos++];
        if (e->what == -SA_SHOT - 1) shot();
        else if (e->what == -SA_QUIT - 1) { logf_("testskript: avslut"); exit(0); }
        else {
            script_held[e->what] = (unsigned char)e->on;
            if (e->on) latch[e->what] = 1;
        }
    }
}

static void update_inputs(void)
{
    unsigned char held[IN_COUNT];
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    int i, k, sel = 0, start = 0;

    memcpy(held, script_held, sizeof held);
    for (k = 0; cfg_keyboard && k < SDL_NUM_SCANCODES; k++)
        if (ks[k]) { int in = key_input((SDL_Scancode)k); if (in >= 0) held[in] = 1; }
    for (i = 0; i < MAXPADS; i++) {
        Pad *p = &pads[i];
        unsigned char now[PB_COUNT];
        int b;
        if (!p->gc && !p->js) continue;
        for (b = 0; b < PB_COUNT; b++) now[b] = (unsigned char)pad_button(p, b);
        for (k = 0; k < NKEYPLAYERS; k++) {
            if (now[pad_bind[k].l]) held[IN_L0 + k] = 1;
            if (now[pad_bind[k].r]) held[IN_R0 + k] = 1;
        }
        if (now[PB_SELECT]) { held[IN_ESC] = 1; sel = 1; }
        if (now[PB_START]) { held[IN_SPACE] = 1; start = 1; }
        if (now[PB_HOTKEY] && !p->prev[PB_HOTKEY]) shot(); /* F: skärmdump av båda panelerna */
        memcpy(p->prev, now, sizeof now);
    }
    if (mouse_bits & 1) held[IN_MOUSE_L] = 1;
    if (mouse_bits & 2) held[IN_MOUSE_R] = 1;
    if (mouse_bits & 4) held[IN_MOUSE_M] = 1;
    if (touch_bits & 1) held[IN_MOUSE_L] = 1;
    if (touch_bits & 2) held[IN_MOUSE_R] = 1;

    for (i = 0; i < IN_COUNT; i++) {
        held[i] |= latch[i];
        latch[i] = 0;
    }
    for (k = 0; k < NKEYPLAYERS; k++) {
        keyL[k] = held[IN_L0 + k];
        keyR[k] = held[IN_R0 + k];
    }
    key_esc = held[IN_ESC];
    key_space = held[IN_SPACE];
    mouse_bits = (mouse_bits & ~0x38) | (held[IN_MOUSE_L] << 3) | (held[IN_MOUSE_R] << 4) | (held[IN_MOUSE_M] << 5);

    /* SELECT+START hållna i 0,7 s: avsluta direkt, var man än är (originalet kan bara avslutas från vissa skärmar) */
    if (sel && start) {
        if (!combo_since_us) combo_since_us = now_us();
        else if (now_us() - combo_since_us > 700000) { logf_("SELECT+START: avslut"); exit(0); }
    } else combo_since_us = 0;
}

int plat_mouse_buttons(void)
{
    return mouse_bits >> 3 & 7;
}

static void handle_event(SDL_Event *e)
{
    static int logged_key;
    int in;
    switch (e->type) {
    case SDL_QUIT:
        logf_("SDL_QUIT (SIGTERM/SIGINT): avslut");
        exit(0);
    case SDL_KEYDOWN:
        if (!e->key.repeat && cfg_keyboard && (in = key_input(e->key.keysym.scancode)) >= 0) latch[in] = 1;
        if (!e->key.repeat && !logged_key++)
            logf_("första tangenthändelsen: %s (scancode %d)%s", SDL_GetScancodeName(e->key.keysym.scancode),
                  e->key.keysym.scancode, cfg_keyboard ? "" : ", tangentbordet avstängt");
        if (e->key.keysym.scancode == SDL_SCANCODE_F12) shot();
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (e->button.which != SDL_TOUCH_MOUSEID) {
            int bit = e->button.button == SDL_BUTTON_LEFT ? 1 : e->button.button == SDL_BUTTON_RIGHT ? 2
                    : e->button.button == SDL_BUTTON_MIDDLE ? 4 : 0;
            if (e->type == SDL_MOUSEBUTTONDOWN) {
                mouse_bits |= bit;
                if (bit == 1) latch[IN_MOUSE_L] = 1;
                if (bit == 2) latch[IN_MOUSE_R] = 1;
                if (bit == 4) latch[IN_MOUSE_M] = 1;
            } else mouse_bits &= ~bit;
        }
        break;
    case SDL_FINGERDOWN: finger(&e->tfinger, 1); break;
    case SDL_FINGERMOTION: finger(&e->tfinger, 1); break;
    case SDL_FINGERUP: finger(&e->tfinger, 0); break;
    case SDL_CONTROLLERDEVICEADDED:
    case SDL_JOYDEVICEADDED:
        if (!pad_already_open(e->cdevice.which)) pad_open(e->cdevice.which);
        break;
    case SDL_JOYDEVICEREMOVED:
        pad_close(e->jdevice.which);
        break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_EXPOSED || e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
            need_full = 1;
        break;
    }
}

/* ===================================================================================================== */
/* den andra panelens innehåll (sida 1) */

enum { C_BG = 16, C_CARD, C_DIM, C_LINE, C_DIMPLAYER0 /* ..+5 */, C_ZONE = C_DIMPLAYER0 + NPLAYERS, C_ZONE_ON };

static void set_rgb_if(int c, int r, int g, int b)
{
    if (bgi_rgb[c][0] != r || bgi_rgb[c][1] != g || bgi_rgb[c][2] != b) bgi_set_rgb8(c, r, g, b);
}

static void companion_colors(void)
{
    int i;
    set_rgb_if(C_BG, 8, 8, 12);
    set_rgb_if(C_CARD, 24, 24, 30);
    set_rgb_if(C_DIM, 120, 120, 130);
    set_rgb_if(C_LINE, 55, 55, 65);
    for (i = 0; i < NPLAYERS; i++)
        set_rgb_if(C_DIMPLAYER0 + i, bgi_rgb[8 + i][0] * 2 / 5, bgi_rgb[8 + i][1] * 2 / 5, bgi_rgb[8 + i][2] * 2 / 5);
    set_rgb_if(C_ZONE, bgi_rgb[13][0] / 5, bgi_rgb[13][1] / 5, bgi_rgb[13][2] / 5);
    set_rgb_if(C_ZONE_ON, bgi_rgb[13][0] * 3 / 5, bgi_rgb[13][1] * 3 / 5, bgi_rgb[13][2] * 3 / 5);
}

static void text8(int x, int y, int size, int color, int just, const char *s)
{
    settextstyle(DEFAULT_FONT, HORIZ_DIR, size);
    settextjustify(just, TOP_TEXT);
    setcolor(color);
    outtextxy(x, y, s);
}

static void text_stroke(int x, int y, int mx, int dx, int my, int dy, int color, int just, const char *s)
{
    settextstyle(TRIPLEX_SCR_FONT, HORIZ_DIR, USER_CHAR_SIZE);
    setusercharsize(mx, dx, my, dy);
    settextjustify(just, TOP_TEXT);
    setcolor(color);
    outtextxy(x, y, s);
}

static void box(int x0, int y0, int x1, int y1, int fill, int edge)
{
    setfillstyle(SOLID_FILL, fill);
    bar(x0, y0, x1, y1);
    if (edge >= 0) {
        setcolor(edge);
        line(x0, y0, x1, y0); line(x1, y0, x1, y1); line(x1, y1, x0, y1); line(x0, y1, x0, y0);
    }
}

static int goal_score(void) { return (participating_count - 1) * 10; } /* score_to_win() i zatacka.c */

static int winner(void)
{
    int i, g = goal_score();
    if (participating_count < 2) return -1;
    for (i = 0; i < NPLAYERS; i++)
        if (joined[i] && score[i] > g - 1) return i;
    return -1;
}

/* lobbytexten utan parenteser: "(Y A)" -> "Y A" */
static const char *controls_plain(int id, char *buf, size_t n)
{
    static const char *const kb[NPLAYERS] = {"(1 Q)", "(L.Ctrl L.Alt)", "(M ,)", "(L.Arrow D.Arrow)", "(/ *)",
                                             "(L.Mouse R.Mouse)"};
    const char *s = plat_controls_text(id, kb[id]);
    size_t len = strlen(s);
    if (len >= 2 && s[0] == '(' && s[len - 1] == ')') snprintf(buf, n, "%.*s", (int)(len - 2), s + 1);
    else snprintf(buf, n, "%s", s);
    return buf;
}

static void touch_zones(int y0, int y1, const char *left, const char *right)
{
    int on_l = (touch_bits | mouse_bits) & 1, on_r = (touch_bits | mouse_bits) & 2;
    box(8, y0, 315, y1, on_l ? C_ZONE_ON : C_ZONE, 13);
    box(324, y0, 631, y1, on_r ? C_ZONE_ON : C_ZONE, 13);
    text8(161, (y0 + y1) / 2 - 8, 2, 15, CENTER_TEXT, left);
    text8(477, (y0 + y1) / 2 - 8, 2, 15, CENTER_TEXT, right);
}

static void draw_companion(void)
{
    int i, n = 0, y;
    char buf[64];

    bgi_push();
    bgi_setpage(1);
    setfillstyle(SOLID_FILL, C_BG);
    bar(0, 0, BGI_W - 1, BGI_H - 1);

    switch (phase) {
    case PHASE_SPLASH:
        text_stroke(320, 40, 1, 1, 3, 2, 15, CENTER_TEXT, "Achtung, die Kurve !");
        text8(140, 150, 2, 15, LEFT_TEXT, "START");
        text8(300, 150, 2, C_DIM, LEFT_TEXT, "play");
        text8(140, 185, 2, 15, LEFT_TEXT, "SELECT");
        text8(300, 185, 2, C_DIM, LEFT_TEXT, "quit");
        text8(140, 220, 2, 15, LEFT_TEXT, "SEL+START");
        text8(300, 220, 2, C_DIM, LEFT_TEXT, "exit (hold)");
        for (i = 0; i < NPLAYERS; i++) box(140 + i * 62, 290, 186 + i * 62, 300, 8 + i, -1);
        text8(320, 340, 1, C_DIM, CENTER_TEXT, "2-6 players on one handheld. Steer, leave holes, survive.");
        text8(320, 360, 1, C_DIM, CENTER_TEXT, "Every death scores a point for everyone still alive.");
        text8(320, 440, 1, C_LINE, CENTER_TEXT, "ZATACKA.EXE (c. 1995) via zatacka-decomp - " ZATACKA_GIT_REV);
        break;

    case PHASE_LOBBY:
        text8(320, 14, 2, 15, CENTER_TEXT, "LEFT button joins");
        text8(320, 38, 2, C_DIM, CENTER_TEXT, "right button leaves");
        for (i = 0; i < NPLAYERS; i++) {
            y = 74 + i * 46;
            box(16, y, 623, y + 38, joined[i] ? C_CARD : C_BG, joined[i] ? 8 + i : C_LINE);
            box(24, y + 8, 34, y + 30, joined[i] ? 8 + i : C_DIMPLAYER0 + i, -1);
            text8(50, y + 11, 2, joined[i] ? 8 + i : C_DIMPLAYER0 + i, LEFT_TEXT, player_name[i]);
            text8(200, y + 11, 2, joined[i] ? 15 : C_DIM, LEFT_TEXT, controls_plain(i, buf, sizeof buf));
            if (joined[i]) text8(610, y + 11, 2, 8 + i, RIGHT_TEXT, "READY");
        }
        y = 74 + NPLAYERS * 46 + 8;
        if (participating_count >= 2)
            text8(320, y, 2, 15, CENTER_TEXT, "START: go!");
        else
            text8(320, y, 2, C_DIM, CENTER_TEXT, "2+ players needed");
        touch_zones(y + 28, 470, "BLUE: join", "BLUE: leave");
        break;

    default: { /* PHASE_ROUND, PHASE_ROUND_OVER, PHASE_GAME_OVER: resultattavlan */
        int rowh, top = 48, bottom = 400, w = winner(), sz;
        snprintf(buf, sizeof buf, "FIRST TO %d", goal_score());
        text8(320, 14, 2, C_DIM, CENTER_TEXT, buf);
        for (i = 0; i < NPLAYERS; i++) n += joined[i] != 0;
        if (n == 0) n = 1;
        rowh = (bottom - top) / n;
        if (rowh > 110) rowh = 110;
        top += (bottom - 48 - rowh * n) / 2;
        /* siffrornas höjd: TSCR:s versalhöjd är 24 enheter */
        sz = rowh * 6 / 10;
        for (i = 0, y = top; i < NPLAYERS; i++) {
            int alive, col, dim;
            if (!joined[i]) continue;
            alive = phase == PHASE_ROUND ? !dead[i] : 1;
            col = alive ? 8 + i : C_DIMPLAYER0 + i;
            dim = alive ? 15 : C_DIM;
            box(16, y + 4, 623, y + rowh - 4, w == i ? C_CARD : C_BG, w == i ? 15 : C_LINE);
            box(24, y + 10, 36, y + rowh - 10, col, -1);
            text8(52, y + rowh / 2 - 8, 2, col, LEFT_TEXT, player_name[i]);
            if (phase == PHASE_ROUND && !alive) text8(220, y + rowh / 2 - 8, 2, C_DIM, LEFT_TEXT, "out");
            snprintf(buf, sizeof buf, "%d", score[i]);
            /* med TOP_TEXT hamnar siffrans topp 7 enheter (nedstapeln) under y och baslinjen 31 enheter under:
               mitten på 19 enheter. Skala sz/24. */
            text_stroke(600, y + rowh / 2 - 19 * sz / 24, sz, 24, sz, 24, alive ? 8 + i : dim, RIGHT_TEXT, buf);
            y += rowh;
        }
        if (phase == PHASE_ROUND) {
            if (joined[5] && cfg_screens == SCREENS_DUAL) touch_zones(410, 470, "< BLUE", "BLUE >");
        } else if (w >= 0) {
            snprintf(buf, sizeof buf, "%s WINS!", player_name[w]);
            text8(320, 412, 2, 8 + w, CENTER_TEXT, buf);
            text8(320, 444, 2, C_DIM, CENTER_TEXT, phase == PHASE_GAME_OVER ? "START: menu   SELECT: quit"
                                                                          : "START");
        } else if (phase == PHASE_ROUND_OVER) {
            text8(320, 420, 2, 15, CENTER_TEXT, "START: next round");
            text8(320, 446, 1, C_DIM, CENTER_TEXT, "SELECT: back to title");
        }
        break;
    }
    }
    bgi_pop();
}

/* alla fält som andra panelen visar: ritas om bara när något ändrats */
static int companion_signature(int *sig)
{
    int n = 0, i;
    sig[n++] = phase;
    sig[n++] = participating_count;
    sig[n++] = touch_bits | (mouse_bits & 3);
    for (i = 0; i < NPLAYERS; i++) {
        sig[n++] = joined[i];
        sig[n++] = dead[i];
        sig[n++] = score[i];
    }
    return n;
}

/* ===================================================================================================== */
/* bild */

static void free_view(View *v)
{
    free(v->xmap); free(v->ymap); free(v->xinv); free(v->yinv); free(v->xw); free(v->yw);
    memset(v, 0, sizeof *v);
}

static void make_maps(int *map, int *inv, int *wt, int dn, int sn)
{
    int d, s;
    /* närmaste granne med pixelcentrum, och för mjuk skalning: ytvikter mellan två källpixlar (dn >= sn) */
    for (d = 0; d < dn; d++) {
        long a = (long)d * sn * 256 / dn;          /* början av målpixeln i källan, 1/256 px */
        long b = (long)(d + 1) * sn * 256 / dn;    /* slutet */
        long i0 = a >> 8;
        map[d] = (int)(((2L * d + 1) * sn) / (2L * dn));
        if (wt) {
            if (b <= (i0 + 1) << 8 || i0 + 1 >= sn) { wt[d] = 256; map[d] = (int)i0; }
            else { wt[d] = (int)((((i0 + 1) << 8) - a) * 256 / (b - a)); map[d] = (int)i0; }
        }
    }
    for (s = 0, d = 0; s <= sn; s++) {
        while (d < dn && map[d] < s) d++;
        inv[s] = d;
    }
}

static void setup_view(View *v, SDL_Rect panel, int page)
{
    int dw, dh;
    free_view(v);
    v->panel = panel;
    v->page = page;
    /* 4:3 i panelen, centrerad */
    if (panel.w * 3 >= panel.h * 4) { dh = panel.h; dw = dh * 4 / 3; }
    else { dw = panel.w; dh = dw * 3 / 4; }
    v->dst.x = panel.x + (panel.w - dw) / 2;
    v->dst.y = panel.y + (panel.h - dh) / 2;
    v->dst.w = dw;
    v->dst.h = dh;
    v->xmap = malloc(sizeof(int) * dw);
    v->ymap = malloc(sizeof(int) * dh);
    v->xinv = malloc(sizeof(int) * (BGI_W + 1));
    v->yinv = malloc(sizeof(int) * (BGI_H + 1));
    if (cfg_smooth && dw >= BGI_W && dh >= BGI_H) {
        v->xw = malloc(sizeof(int) * dw);
        v->yw = malloc(sizeof(int) * dh);
    }
    make_maps(v->xmap, v->xinv, v->xw, dw, BGI_W);
    make_maps(v->ymap, v->yinv, v->yw, dh, BGI_H);
}

static void setup_layout(void)
{
    SDL_Rect top, bottom;
    int stacked;
    SDL_GetWindowSize(win, &win_w, &win_h);
    stacked = win_h >= win_w; /* två paneler över varandra (Banana-SDL stack, eller testfönster) */
    nviews = 0;
    if (stacked) {
        top.x = 0; top.y = 0; top.w = win_w; top.h = win_h / 2;
        bottom = top; bottom.y = win_h / 2; bottom.h = win_h - win_h / 2;
        setup_view(&views[0], top, cfg_screens == SCREENS_SWAP ? 1 : 0);
        setup_view(&views[1], bottom, cfg_screens == SCREENS_SWAP ? 0 : cfg_screens == SCREENS_SINGLE ? -1 : 1);
        nviews = 2;
    } else {
        top.x = 0; top.y = 0; top.w = win_w; top.h = win_h;
        setup_view(&views[0], top, 0);
        nviews = 1;
    }
    logf_("fönster %dx%d: %s, spelet %dx%d på %s panel%s", win_w, win_h,
          nviews == 2 ? "två paneler över varandra" : "en panel", views[cfg_screens == SCREENS_SWAP && nviews == 2].dst.w,
          views[cfg_screens == SCREENS_SWAP && nviews == 2].dst.h,
          nviews == 1 ? "den enda" : cfg_screens == SCREENS_SWAP ? "den nedre" : "den övre",
          cfg_smooth ? " (mjuk skalning)" : "");
    need_full = 1;
}

static inline Uint32 blend(Uint32 a, Uint32 b, int w) /* w/256 av a */
{
    Uint32 rb = ((a & 0xFF00FF) * w + (b & 0xFF00FF) * (256 - w)) >> 8 & 0xFF00FF;
    Uint32 g = ((a & 0x00FF00) * w + (b & 0x00FF00) * (256 - w)) >> 8 & 0x00FF00;
    return rb | g;
}

static void scale_region(SDL_Surface *s, View *v, int sx0, int sy0, int sx1, int sy1)
{
    const BgiPage *pg = &bgi_page[v->page];
    int dx0, dx1, dy0, dy1, x, y;
    if (v->xw) { /* mjuk: grannpixlarna påverkar */
        if (sx0 > 0) sx0--;
        if (sy0 > 0) sy0--;
        if (sx1 < BGI_W - 1) sx1++;
        if (sy1 < BGI_H - 1) sy1++;
    }
    dx0 = v->xinv[sx0]; dx1 = v->xinv[sx1 + 1];
    dy0 = v->yinv[sy0]; dy1 = v->yinv[sy1 + 1];
    for (y = dy0; y < dy1; y++) {
        Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + (size_t)(v->dst.y + y) * s->pitch) + v->dst.x;
        const unsigned char *src = pg->pix[v->ymap[y]];
        if (!v->xw) {
            for (x = dx0; x < dx1; x++) row[x] = lut[src[v->xmap[x]]];
        } else {
            int wy = v->yw[y], sy = v->ymap[y];
            const unsigned char *src2 = pg->pix[sy + 1 < BGI_H ? sy + 1 : sy];
            for (x = dx0; x < dx1; x++) {
                int sx = v->xmap[x], wx = v->xw[x], sx2 = sx + 1 < BGI_W ? sx + 1 : sx;
                Uint32 a = wx == 256 ? lut[src[sx]] : blend(lut[src[sx]], lut[src[sx2]], wx);
                if (wy != 256) {
                    Uint32 b = wx == 256 ? lut[src2[sx]] : blend(lut[src2[sx]], lut[src2[sx2]], wx);
                    a = blend(a, b, wy);
                }
                row[x] = a | 0xFF000000u;
            }
        }
    }
}

static void present(void)
{
    static int sig_prev[64], sig_n;
    SDL_Surface *ws = SDL_GetWindowSurface(win), *s;
    Uint64 t0 = now_us();
    int i, any = 0, full;
    int sig[64], n;

    if (!ws) return;
    if (ws->format->BytesPerPixel != 4) {
        if (!shadow || shadow->w != ws->w || shadow->h != ws->h) {
            SDL_FreeSurface(shadow);
            shadow = SDL_CreateRGBSurfaceWithFormat(0, ws->w, ws->h, 32, SDL_PIXELFORMAT_XRGB8888);
            need_full = 1;
        }
        s = shadow;
    } else s = ws;

    /* andra panelens innehåll */
    if (nviews == 2 && (views[0].page == 1 || views[1].page == 1)) {
        companion_colors();
        n = companion_signature(sig);
        if (n != sig_n || memcmp(sig, sig_prev, sizeof(int) * n)) {
            draw_companion();
            memcpy(sig_prev, sig, sizeof(int) * n);
            sig_n = n;
        }
    }

    if (lut_serial != bgi_palette_serial) {
        for (i = 0; i < 256; i++) lut[i] = SDL_MapRGB(s->format, bgi_rgb[i][0], bgi_rgb[i][1], bgi_rgb[i][2]);
        lut_serial = bgi_palette_serial;
        need_full = 1;
    }
    full = need_full;
    if (full) {
        SDL_FillRect(s, NULL, SDL_MapRGB(s->format, 0, 0, 0));
        any = 1;
    }
    for (i = 0; i < nviews; i++) {
        View *v = &views[i];
        BgiPage *pg;
        if (v->page < 0) continue;
        pg = &bgi_page[v->page];
        if (full) scale_region(s, v, 0, 0, BGI_W - 1, BGI_H - 1);
        else if (pg->dirty) scale_region(s, v, pg->dx0, pg->dy0, pg->dx1, pg->dy1);
        else continue;
        any = 1;
    }
    for (i = 0; i < BGI_PAGES; i++) bgi_page[i].dirty = 0;
    need_full = 0;
    if (!any) return;
    if (s != ws) SDL_BlitSurface(s, NULL, ws, NULL);
    SDL_UpdateWindowSurface(win);
    last_present_us = now_us();
    stat_presents++;
    stat_present_us += last_present_us - t0;
}

/* båda panelerna (hela fönstret) som PPM, för test-on-device.sh */
static void shot(void)
{
    SDL_Surface *ws = SDL_GetWindowSurface(win), *s = shadow ? shadow : ws;
    char path[PATH_MAX];
    FILE *f;
    int i, x, y;
    if (!s) return;
    need_full = 1;
    present(); /* allt på ytan */
    mkdir(cfg_shot_dir, 0777);
    for (i = 0; i < 10000; i++) {
        snprintf(path, sizeof path, "%s/zatacka-panels-%03d.ppm", cfg_shot_dir, i);
        if (access(path, F_OK) != 0) break;
    }
    f = fopen(path, "wb");
    if (!f) { logf_("skärmdump: %s: %s", path, strerror(errno)); return; }
    fprintf(f, "P6\n%d %d\n255\n", s->w, s->h);
    for (y = 0; y < s->h; y++) {
        const Uint32 *row = (const Uint32 *)((const Uint8 *)s->pixels + (size_t)y * s->pitch);
        for (x = 0; x < s->w; x++) {
            Uint8 rgb[3];
            SDL_GetRGB(row[x], s->format, &rgb[0], &rgb[1], &rgb[2]);
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    logf_("skärmdump: %s", path);
}

/* ===================================================================================================== */

void plat_phase(int p)
{
    phase = p;
}

const char *plat_controls_text(int id, const char *keyboard_text)
{
    return cfg_pad_labels && id >= 0 && id < NPLAYERS ? pad_labels[id] : keyboard_text;
}

const char *plat_data_dir(void) { return data_dir; }

void plat_pump(void)
{
    SDL_Event e;
    Uint64 now;
    while (SDL_PollEvent(&e)) handle_event(&e);
    if (script_n) run_script();
    update_inputs();
    now = now_us();
    if (now - last_present_us >= 16000) present();
    if (now - last_trigger_check_us > 300000) {
        last_trigger_check_us = now;
        if (cfg_shot_trigger && *cfg_shot_trigger && access(cfg_shot_trigger, F_OK) == 0) {
            unlink(cfg_shot_trigger);
            shot();
        }
    }
    if (cfg_stats && now - last_stats_us > 30000000) {
        double secs = (now - last_stats_us) / 1e6;
        if (last_stats_us)
            logf_("stats: %.1f tick/s, %.1f bildrutor/s, %.2f ms per bildruta (skalning + flip)",
                  stat_ticks / secs, stat_presents / secs,
                  stat_presents ? stat_present_us / 1000.0 / stat_presents : 0.0);
        last_stats_us = now;
        stat_ticks = stat_presents = 0;
        stat_present_us = 0;
    }
}

void plat_idle(void)
{
    plat_pump();
    sleep_us(4000);
}

void delay(unsigned ms)
{
    Uint64 now = now_us();
    /* deadlines i följd: spelets takt hålls även när en bildruta tar tid. Långt efter (paus, sömn, väntan på
       START): börja om från nu i stället för att köra ikapp. */
    if (!tick_deadline_us || now > tick_deadline_us + 100000) tick_deadline_us = now;
    tick_deadline_us += (Uint64)ms * 1000;
    stat_ticks++;
    for (;;) {
        Uint64 rem, until_present;
        plat_pump();
        now = now_us();
        if (now >= tick_deadline_us) break;
        rem = tick_deadline_us - now;
        until_present = last_present_us + 16000 > now ? last_present_us + 16000 - now : 0;
        sleep_us(rem < until_present || !until_present ? (rem < 16000 ? rem : 16000) : until_present);
    }
}

static void plat_quit(void)
{
    int i;
    for (i = 0; i < MAXPADS; i++) {
        if (pads[i].gc) SDL_GameControllerClose(pads[i].gc);
        else if (pads[i].js) SDL_JoystickClose(pads[i].js);
    }
    for (i = 0; i < 2; i++) free_view(&views[i]);
    SDL_FreeSurface(shadow);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit(); /* Banana-SDL: stänger DRM, menyn tar tillbaka skärmen */
    logf_("avslut");
}

/* getenv där tom sträng räknas som inte satt (run-zatacka.sh exporterar variabler som kan vara tomma) */
static const char *env(const char *name)
{
    const char *v = getenv(name);
    return v && *v ? v : NULL;
}

static void find_data_dir(const char *argv0)
{
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    char *slash;
    if (n > 0) buf[n] = 0;
    else snprintf(buf, sizeof buf, "%s", argv0 ? argv0 : "./zatacka");
    slash = strrchr(buf, '/');
    if (slash) { *slash = 0; snprintf(data_dir, sizeof data_dir, "%s", buf[0] ? buf : "/"); }
}

void plat_init(int argc, char **argv)
{
    const char *e;
    unsigned seed;
    int w = 0, h = 0, i;
    SDL_version linked;
    SDL_DisplayMode dm;
    Uint32 flags = 0;

    setvbuf(stderr, NULL, _IOLBF, 0);
    find_data_dir(argc > 0 ? argv[0] : NULL);

    e = env("ZATACKA_SCREENS");
    if (e && !strcmp(e, "swap")) cfg_screens = SCREENS_SWAP;
    else if (e && !strcmp(e, "single")) cfg_screens = SCREENS_SINGLE;
    e = env("ZATACKA_FILTER");
    cfg_smooth = e && !strcmp(e, "smooth");
    cfg_stats = !(e = env("ZATACKA_STATS")) || strcmp(e, "0");
    cfg_keyboard = !(e = env("ZATACKA_KEYBOARD")) || strcmp(e, "0");
    if ((e = getenv("ZATACKA_SHOT_TRIGGER"))) cfg_shot_trigger = e;
    if ((e = env("ZATACKA_SHOT_DIR"))) cfg_shot_dir = e;

    /* Borlands rand(): originalet anropar aldrig srand, så varje parti har samma startpositioner.
       ZATACKA_SEED=original ger det beteendet, ett tal ger det fröet, annars tiden. */
    e = env("ZATACKA_SEED");
    if (e && !strcmp(e, "original")) seed = 1;
    else if (e && *e) seed = (unsigned)strtoul(e, NULL, 0);
    else seed = (unsigned)time(NULL) ^ (unsigned)getpid() << 16;
    bc_srand(seed);

    /* pek ska inte bli musklick (blå spelare läser pekzonerna själv) */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "zatacka: SDL_Init: %s\n", SDL_GetError());
        exit(1);
    }
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0)
        logf_("ingen handkontroll (%s)", SDL_GetError());
    SDL_GameControllerAddMapping(RGDS_MAPPING);
    perf_freq = SDL_GetPerformanceFrequency();
    t_start_us = now_us();
    SDL_GetVersion(&linked);
    logf_("version %s, SDL %d.%d.%d, videodrivrutin %s, SDL_BANANA_PANELS=%s, frö %u%s", ZATACKA_GIT_REV,
          linked.major, linked.minor, linked.patch, SDL_GetCurrentVideoDriver(),
          getenv("SDL_BANANA_PANELS") ? getenv("SDL_BANANA_PANELS") : "-", seed, seed == 1 ? " (som originalet)" : "");

    /* fönstret: på BananaOS hela displayen (båda panelerna), annars ZATACKA_WINDOW eller 640x960 (två paneler) */
    if ((e = env("ZATACKA_WINDOW")) && sscanf(e, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
    } else if (getenv("SDL_BANANA_PANELS") && SDL_GetDesktopDisplayMode(0, &dm) == 0) {
        w = dm.w; h = dm.h; flags = SDL_WINDOW_BORDERLESS;
    } else {
        w = 640; h = cfg_screens == SCREENS_SINGLE ? 480 : 960;
    }
    win = SDL_CreateWindow("Achtung, die Kurve!", 0, 0, w, h, flags | SDL_WINDOW_SHOWN);
    if (!win) {
        fprintf(stderr, "zatacka: SDL_CreateWindow %dx%d: %s\n", w, h, SDL_GetError());
        SDL_Quit();
        exit(1);
    }
    SDL_ShowCursor(SDL_DISABLE);
    atexit(plat_quit);
    setup_layout();

    for (i = 0; i < SDL_NumJoysticks(); i++)
        if (!pad_already_open(i)) pad_open(i);
    e = env("ZATACKA_LABELS");
    cfg_pad_labels = e ? !strcmp(e, "pad") : (pads[0].js != NULL || getenv("SDL_BANANA_PANELS") != NULL);

    if ((e = env("ZATACKA_SCRIPT")) && *e) parse_script(e);
}

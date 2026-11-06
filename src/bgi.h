/*
 * Den delmängd av Borland Graphics Interface (graphics.h, BC++ 3.1) som ZATACKA.EXE använder, på en indexerad
 * 640x480-yta i minnet. Semantiken som spelet bygger på är bevarad: getpixel() på samma yta som allt ritas på
 * (kollisionen är "pixel > 7"), getimage/putimage-buffertar med BGI:s huvud (bredd-1, höjd-1), så att den nollade
 * img_bg-bufferten tolkas som en 1x1-bild precis som i originalet, och fyllnings-/textfärg som separata tillstånd.
 *
 * Utöver BGI: två "sidor" (0 = spelets skärm, 1 = andra panelens innehåll) och en smutsrektangel per sida, så att
 * plattformslagret bara skalar om det som ändrats.
 */
#ifndef ZATACKA_BGI_H
#define ZATACKA_BGI_H

#define BGI_W 640
#define BGI_H 480
#define BGI_PAGES 2

/* konstanter som i graphics.h */
enum { VGA = 9, VGAHI = 2 };
enum { grOk = 0, grFileNotFound = -3, grInvalidFont = -13 };
enum { SOLID_FILL = 1 };
enum { HORIZ_DIR = 0 };
enum { DEFAULT_FONT = 0, TRIPLEX_SCR_FONT = 7 };
enum { USER_CHAR_SIZE = 0 };
enum { COPY_PUT = 0 };
enum { LEFT_TEXT = 0, CENTER_TEXT = 1, RIGHT_TEXT = 2 };
enum { BOTTOM_TEXT = 0, TOP_TEXT = 2 };

#define MAXCOLORS 15
struct palettetype {
    unsigned char size;
    signed char colors[MAXCOLORS + 1];
};

typedef struct {
    unsigned char pix[BGI_H][BGI_W];
    int dirty, dx0, dy0, dx1, dy1; /* smutsrektangel, inklusive gränser */
} BgiPage;

extern BgiPage bgi_page[BGI_PAGES];
extern unsigned char bgi_rgb[256][3]; /* 8-bitars RGB per färgindex (spelet använder 0..15, sida 1 även fler) */
extern int bgi_palette_serial;        /* ökar när paletten ändras: allt måste skalas om */

/* initgraph letar efter fonten i den här katalogen (NULL = aktuell katalog), som originalet gör med "" */
void initgraph(int *driver, int *mode, const char *path);
int graphresult(void);
const char *grapherrormsg(int code);
void closegraph(void);

void getpalette(struct palettetype *pal);
void setrgbpalette(int color, int r6, int g6, int b6); /* 6-bitars VGA DAC-värden */

void setfillstyle(int pattern, int color);
void bar(int left, int top, int right, int bottom);
void setcolor(int color);
int getcolor(void);
void line(int x1, int y1, int x2, int y2);
unsigned getpixel(int x, int y);
void putpixel(int x, int y, int color);

unsigned imagesize(int left, int top, int right, int bottom);
void getimage(int left, int top, int right, int bottom, void *bitmap);
void putimage(int left, int top, const void *bitmap, int op);

void settextstyle(int font, int direction, int charsize);
void setusercharsize(int multx, int divx, int multy, int divy);
void settextjustify(int horiz, int vert);
void outtextxy(int x, int y, const char *text);
int textwidth(const char *text);
int textheight(const char *text);

/* tillägg */
void bgi_setpage(int page);   /* sidan som ritas på (och läses med getpixel) */
int bgi_getpage(void);
void bgi_mark_all(int page);
void bgi_set_rgb8(int color, int r, int g, int b);
void bgi_push(void);          /* sparar/återställer ritläget (sida, färger, font, justering); en nivå */
void bgi_pop(void);

#endif

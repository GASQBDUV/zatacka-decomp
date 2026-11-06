/*
 * BGI-delmängd för Zatacka (se bgi.h). Allt ritas i en indexerad 640x480-buffer per sida.
 *
 * Strokefonten (TSCR.CHR, "BGI Stroked Font V1.1") läses från fil som originalet gör. Format:
 *   text fram till 0x1A, sedan ett huvud (storlek, namn, datastorlek, versioner); på offset <storlek>:
 *   '+', antal tecken (u16), odef. (u8), första tecken (u8), offset till strokedata (u16), scan-flagga (u8),
 *   org_to_cap, org_to_base, org_to_dec (s8), 5 byte; sedan en offsettabell (u16 per tecken) och en breddtabell
 *   (u8 per tecken). Varje strokeinstruktion är två byte: bit 7 i båda är opkoden (00 slut, 01 scan, 10 flytta,
 *   11 rita), bit 0-6 är x och y som 7-bitars tal med tecken, y uppåt från baslinjen.
 */
#include "bgi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char font8x8_basic[128][8];

BgiPage bgi_page[BGI_PAGES];
unsigned char bgi_rgb[256][3];
int bgi_palette_serial;

static BgiPage *cur = &bgi_page[0];
static int cur_index;
static int result = grOk;
static int fill_color = 15, draw_color = 15;
static int font = DEFAULT_FONT, charsize = 1;
static int umx = 1, udx = 1, umy = 1, udy = 1; /* setusercharsize */
static int hjust = LEFT_TEXT, vjust = TOP_TEXT;

/* strokefonten */
static struct {
    int loaded, nchars, first, cap, base, dec;
    const unsigned short *offs; /* värdens byteordning efter inläsning */
    const unsigned char *widths, *strokes;
    unsigned char *file;
    unsigned short *offs_buf;
} chr;

static void mark(int x0, int y0, int x1, int y1)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= BGI_W) x1 = BGI_W - 1;
    if (y1 >= BGI_H) y1 = BGI_H - 1;
    if (x0 > x1 || y0 > y1) return;
    if (!cur->dirty) {
        cur->dirty = 1;
        cur->dx0 = x0; cur->dy0 = y0; cur->dx1 = x1; cur->dy1 = y1;
        return;
    }
    if (x0 < cur->dx0) cur->dx0 = x0;
    if (y0 < cur->dy0) cur->dy0 = y0;
    if (x1 > cur->dx1) cur->dx1 = x1;
    if (y1 > cur->dy1) cur->dy1 = y1;
}

void bgi_mark_all(int page)
{
    BgiPage *p = &bgi_page[page];
    p->dirty = 1; p->dx0 = 0; p->dy0 = 0; p->dx1 = BGI_W - 1; p->dy1 = BGI_H - 1;
}

void bgi_setpage(int page) { cur_index = page; cur = &bgi_page[page]; }

static struct { int page, fill, draw, font, size, umx, udx, umy, udy, hj, vj; } saved;
void bgi_push(void)
{
    saved.page = cur_index; saved.fill = fill_color; saved.draw = draw_color; saved.font = font;
    saved.size = charsize; saved.umx = umx; saved.udx = udx; saved.umy = umy; saved.udy = udy;
    saved.hj = hjust; saved.vj = vjust;
}
void bgi_pop(void)
{
    bgi_setpage(saved.page); fill_color = saved.fill; draw_color = saved.draw; font = saved.font;
    charsize = saved.size; umx = saved.umx; udx = saved.udx; umy = saved.umy; udy = saved.udy;
    hjust = saved.hj; vjust = saved.vj;
}
int bgi_getpage(void) { return cur_index; }

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }

static int load_chr(const char *dir)
{
    char path[1024];
    FILE *f;
    long n;
    unsigned char *d;
    size_t i;
    unsigned hs, so;
    int k;

    snprintf(path, sizeof path, "%s%sTSCR.CHR", dir && *dir ? dir : "", dir && *dir ? "/" : "");
    f = fopen(path, "rb");
    if (!f) return grFileNotFound;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = malloc(n > 0 ? n : 1);
    if (!d || fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); return grInvalidFont; }
    fclose(f);
    for (i = 0; i < (size_t)n && d[i] != 0x1A; i++) ;
    if (i + 3 >= (size_t)n || memcmp(d, "PK\b\b", 4) != 0) { free(d); return grInvalidFont; }
    hs = rd16(d + i + 1);
    if (hs + 16 >= (unsigned)n || d[hs] != '+') { free(d); return grInvalidFont; }
    chr.nchars = rd16(d + hs + 1);
    chr.first = d[hs + 4];
    so = rd16(d + hs + 5);
    chr.cap = (signed char)d[hs + 8];
    chr.base = (signed char)d[hs + 9];
    chr.dec = (signed char)d[hs + 10];
    if (hs + 16 + chr.nchars * 3u > (unsigned)n || hs + so > (unsigned)n) { free(d); return grInvalidFont; }
    chr.offs_buf = malloc(sizeof(unsigned short) * chr.nchars);
    for (k = 0; k < chr.nchars; k++) chr.offs_buf[k] = rd16(d + hs + 16 + 2 * k);
    chr.offs = chr.offs_buf;
    chr.widths = d + hs + 16 + 2 * chr.nchars;
    chr.strokes = d + hs + so;
    chr.file = d;
    chr.loaded = 1;
    return grOk;
}

void initgraph(int *driver, int *mode, const char *path)
{
    int i;
    memset(bgi_page, 0, sizeof bgi_page);
    /* BGI:s standardpalett för VGA i 6 bitar (EGA-färgerna); spelet skriver över 0..15 ändå */
    static const unsigned char ega[16][3] = {
        {0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42},
        {21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63}};
    for (i = 0; i < 16; i++) setrgbpalette(i, ega[i][0], ega[i][1], ega[i][2]);
    for (i = 0; i < BGI_PAGES; i++) bgi_mark_all(i);
    bgi_setpage(0);
    fill_color = draw_color = 15;
    font = DEFAULT_FONT; charsize = 1;
    result = load_chr(path);
    if (result == grFileNotFound && path && *path) result = load_chr(NULL);
    if (result != grOk) {
        /* originalet hittar fonten först när den väljs; spelet startar ändå. Strokefonten blir då 8x8-text. */
        fprintf(stderr, "bgi: TSCR.CHR: %s (strokefonten ersätts med 8x8-fonten)\n", grapherrormsg(result));
        result = grOk;
    }
    (void)driver; (void)mode;
}

int graphresult(void) { int r = result; result = grOk; return r; }

const char *grapherrormsg(int code)
{
    switch (code) {
    case grOk: return "No error";
    case grFileNotFound: return "Font file not found";
    case grInvalidFont: return "Invalid font file";
    default: return "Graphics error";
    }
}

void closegraph(void)
{
    free(chr.file);
    free(chr.offs_buf);
    memset(&chr, 0, sizeof chr);
}

void getpalette(struct palettetype *pal)
{
    int i;
    pal->size = 16;
    for (i = 0; i < 16; i++) pal->colors[i] = (signed char)i; /* färg i -> post i (DAC-omvägen behövs inte) */
}

void bgi_set_rgb8(int color, int r, int g, int b)
{
    if (color < 0 || color > 255) return;
    bgi_rgb[color][0] = (unsigned char)r;
    bgi_rgb[color][1] = (unsigned char)g;
    bgi_rgb[color][2] = (unsigned char)b;
    bgi_palette_serial++;
}

void setrgbpalette(int color, int r6, int g6, int b6)
{
    /* VGA:s DAC har 6 bitar per kanal: 0..63 -> 0..255 */
    bgi_set_rgb8(color, (r6 & 63) * 255 / 63, (g6 & 63) * 255 / 63, (b6 & 63) * 255 / 63);
}

void setfillstyle(int pattern, int color) { (void)pattern; fill_color = color; }
void setcolor(int color) { draw_color = color; }
int getcolor(void) { return draw_color; }

void bar(int left, int top, int right, int bottom)
{
    int y;
    if (left > right) { int t = left; left = right; right = t; }
    if (top > bottom) { int t = top; top = bottom; bottom = t; }
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right >= BGI_W) right = BGI_W - 1;
    if (bottom >= BGI_H) bottom = BGI_H - 1;
    if (left > right || top > bottom) return;
    for (y = top; y <= bottom; y++) memset(&cur->pix[y][left], fill_color, right - left + 1);
    mark(left, top, right, bottom);
}

static inline void plot(int x, int y, int c)
{
    if ((unsigned)x < BGI_W && (unsigned)y < BGI_H) cur->pix[y][x] = (unsigned char)c;
}

void putpixel(int x, int y, int color)
{
    plot(x, y, color);
    mark(x, y, x, y);
}

unsigned getpixel(int x, int y)
{
    if ((unsigned)x >= BGI_W || (unsigned)y >= BGI_H) return 0;
    return cur->pix[y][x];
}

void line(int x1, int y1, int x2, int y2)
{
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy, e2, x = x1, y = y1;
    for (;;) {
        plot(x, y, draw_color);
        if (x == x2 && y == y2) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx; }
        if (e2 <= dx) { err += dx; y += sy; }
    }
    mark(x1 < x2 ? x1 : x2, y1 < y2 ? y1 : y2, x1 > x2 ? x1 : x2, y1 > y2 ? y1 : y2);
}

/* BGI:s bildformat: u16 bredd-1, u16 höjd-1, sedan pixlarna (här en byte per pixel i stället för plan) */
unsigned imagesize(int left, int top, int right, int bottom)
{
    return 4 + (unsigned)(abs(right - left) + 1) * (unsigned)(abs(bottom - top) + 1);
}

void getimage(int left, int top, int right, int bottom, void *bitmap)
{
    unsigned char *b = bitmap;
    int w = right - left + 1, h = bottom - top + 1, x, y;
    b[0] = (unsigned char)(w - 1); b[1] = (unsigned char)((w - 1) >> 8);
    b[2] = (unsigned char)(h - 1); b[3] = (unsigned char)((h - 1) >> 8);
    b += 4;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) *b++ = (unsigned char)getpixel(left + x, top + y);
}

void putimage(int left, int top, const void *bitmap, int op)
{
    const unsigned char *b = bitmap;
    int w = rd16(b) + 1, h = rd16(b + 2) + 1, x, y;
    b += 4;
    (void)op; /* bara COPY_PUT används */
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) plot(left + x, top + y, *b++);
    mark(left, top, left + w - 1, top + h - 1);
}

void settextstyle(int f, int direction, int size)
{
    (void)direction; /* bara HORIZ_DIR */
    font = f;
    charsize = size;
}

void setusercharsize(int multx, int divx, int multy, int divy)
{
    umx = multx; udx = divx ? divx : 1;
    umy = multy; udy = divy ? divy : 1;
}

void settextjustify(int horiz, int vert) { hjust = horiz; vjust = vert; }

/* skalningen för strokefonten: USER_CHAR_SIZE = setusercharsize, storlek 4 = fontens egen storlek */
static void stroke_scale(int *mx, int *dx, int *my, int *dy)
{
    if (charsize == USER_CHAR_SIZE) { *mx = umx; *dx = udx; *my = umy; *dy = udy; }
    else { *mx = *my = charsize; *dx = *dy = 4; }
}

static int use_stroke(void) { return font != DEFAULT_FONT && chr.loaded; }
static int bitmap_size(void) { return font == DEFAULT_FONT ? (charsize > 0 ? charsize : 1) : 2; }

static int stroke_char_width(int c, int mx, int dx)
{
    c -= chr.first;
    if (c < 0 || c >= chr.nchars) return 0;
    return chr.widths[c] * mx / dx;
}

int textwidth(const char *text)
{
    int w = 0, mx, dx, my, dy;
    if (!use_stroke()) return (int)strlen(text) * 8 * bitmap_size();
    stroke_scale(&mx, &dx, &my, &dy);
    for (; *text; text++) w += stroke_char_width((unsigned char)*text, mx, dx);
    return w;
}

int textheight(const char *text)
{
    int mx, dx, my, dy;
    (void)text;
    if (!use_stroke()) return 8 * bitmap_size();
    stroke_scale(&mx, &dx, &my, &dy);
    return (chr.cap - chr.dec) * my / dy;
}

static int rdiv(int a, int m, int d) /* a*m/d avrundat */
{
    long v = (long)a * m;
    return (int)(v >= 0 ? (v + d / 2) / d : -((-v + d / 2) / d));
}

static void stroke_text(int x, int y, const char *text)
{
    int mx, dx, my, dy, base;
    stroke_scale(&mx, &dx, &my, &dy);
    /* TOP_TEXT: baslinjen ligger hela texthöjden (cap - dec) under y, inte bara versalhöjden. Uppmätt mot
       originalet i DOSBox: splashtexten (y = 170, höjd x2) har baslinjen på y = 232 = 170 + (24 + 7) * 2. */
    base = vjust == TOP_TEXT ? y + rdiv(chr.cap - chr.dec, my, dy) : y + rdiv(chr.dec, my, dy);
    for (; *text; text++) {
        int c = (unsigned char)*text - chr.first, penx = x, peny = base;
        const unsigned char *s;
        if (c < 0 || c >= chr.nchars) continue;
        for (s = chr.strokes + chr.offs[c];; s += 2) {
            int op = (s[0] >> 7) << 1 | s[1] >> 7;
            int sx = (signed char)(s[0] << 1) >> 1, sy = (signed char)(s[1] << 1) >> 1;
            int nx = x + sx * mx / dx, ny = base - sy * my / dy; /* trunkering som Borland: uppmätt mot DOSBox */
            if (op == 0) break;
            if (op == 3) {
                int save = draw_color;
                line(penx, peny, nx, ny);
                draw_color = save;
            }
            penx = nx; peny = ny;
        }
        x += stroke_char_width(c + chr.first, mx, dx);
    }
}

static void bitmap_text(int x, int y, const char *text)
{
    int s = bitmap_size(), len = (int)strlen(text), i, r, b;
    for (i = 0; i < len; i++, x += 8 * s) {
        unsigned c = (unsigned char)text[i];
        if (c >= 128) c = '?';
        for (r = 0; r < 8; r++)
            for (b = 0; b < 8; b++)
                if (font8x8_basic[c][r] >> b & 1) {
                    int px = x + b * s, py = y + r * s, u, v;
                    for (v = 0; v < s; v++)
                        for (u = 0; u < s; u++) plot(px + u, py + v, draw_color);
                }
    }
    mark(x - len * 8 * s, y, x - 1, y + 8 * s - 1);
}

void outtextxy(int x, int y, const char *text)
{
    int w = textwidth(text);
    if (hjust == CENTER_TEXT) x -= w / 2;
    else if (hjust == RIGHT_TEXT) x -= w;
    if (use_stroke()) {
        stroke_text(x, y, text);
    } else {
        if (vjust == BOTTOM_TEXT) y -= 8 * bitmap_size();
        bitmap_text(x, y, text);
    }
}

// Tiny strip-based 2D drawing for a 240x240 RGB565 panel.
// A canvas covers a horizontal band of the screen; every draw call takes
// screen coordinates and clips to the band, so a full page is rendered by
// running the same draw function once per band (no full framebuffer needed).
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SCREEN_W 240
#define SCREEN_H 240

typedef struct {
    uint32_t offset;  // byte offset into bitmap (4bpp, row-major, packed)
    uint8_t w, h;
    int8_t xoff, yoff;  // from pen x / top of line
    uint8_t adv;
} glyph_t;

typedef struct {
    const uint8_t *bits;
    const glyph_t *glyphs;
    uint8_t first;
    uint8_t count;
    uint8_t height;
    uint8_t ascent;
} font_t;

extern const font_t font_s, font_m, font_l, font_xl, font_xxl;

// Special glyphs
#define GLYPH_UP "\x7f"
#define GLYPH_DOWN "\x80"
#define GLYPH_DOT "\x81"
#define GLYPH_DEG "\x82"

typedef struct {
    uint16_t *buf;  // big-endian RGB565 (ready for SPI)
    int y0, h;      // band covered, screen coordinates; width is SCREEN_W
} canvas_t;

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

typedef enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT } align_t;

void gfx_fill(canvas_t *c, uint16_t color);
void gfx_rect(canvas_t *c, int x, int y, int w, int h, uint16_t color);
void gfx_round_rect(canvas_t *c, int x, int y, int w, int h, int r, uint16_t color);
void gfx_hline(canvas_t *c, int x, int y, int w, uint16_t color);
void gfx_vline(canvas_t *c, int x, int y, int h, uint16_t color);
void gfx_line(canvas_t *c, int x0, int y0, int x1, int y1, uint16_t color);
// Anti-aliased filled circle (centre and radius in pixels, may be fractional).
void gfx_disc(canvas_t *c, float cx, float cy, float r, uint16_t color);
void gfx_blend_pixel(canvas_t *c, int x, int y, uint16_t color, uint8_t alpha);
int gfx_text_width(const font_t *f, const char *s);
// y is the top of the text line. Returns the x after the last glyph.
int gfx_text(canvas_t *c, const font_t *f, int x, int y, const char *s, uint16_t color, align_t align);
// Formats v with dec decimals without using printf float support (absent in newlib-nano).
void fmt_fixed(char *out, int n, double v, int dec, bool plus);
uint16_t gfx_mix(uint16_t a, uint16_t b, uint8_t alpha);  // alpha 0..255 of b over a

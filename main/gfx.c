#include "gfx.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

void fmt_fixed(char *out, int n, double v, int dec, bool plus)
{
    if (dec < 0) dec = 0;
    if (dec > 6) dec = 6;
    const char *sign = v < 0 ? "-" : (plus ? "+" : "");
    double a = v < 0 ? -v : v;
    uint64_t scale = 1;
    for (int i = 0; i < dec; i++) scale *= 10;
    uint64_t iv = (uint64_t)(a * (double)scale + 0.5);
    unsigned long ip = (unsigned long)(iv / scale), fp = (unsigned long)(iv % scale);
    if (iv == 0) sign = plus ? "+" : "";
    if (dec) snprintf(out, n, "%s%lu.%0*lu", sign, ip, dec, fp);
    else snprintf(out, n, "%s%lu", sign, ip);
}

static inline uint16_t swap16(uint16_t v) { return (v >> 8) | (v << 8); }

uint16_t gfx_mix(uint16_t a, uint16_t b, uint8_t alpha)
{
    if (alpha == 0) return a;
    if (alpha == 255) return b;
    uint32_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    uint32_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    uint32_t r = (ar * (255 - alpha) + br * alpha) / 255;
    uint32_t g = (ag * (255 - alpha) + bg * alpha) / 255;
    uint32_t bl = (ab * (255 - alpha) + bb * alpha) / 255;
    return (r << 11) | (g << 5) | bl;
}

static inline bool in_band(canvas_t *c, int x, int y)
{
    return x >= 0 && x < SCREEN_W && y >= c->y0 && y < c->y0 + c->h;
}

static inline void put(canvas_t *c, int x, int y, uint16_t color)
{
    c->buf[(y - c->y0) * SCREEN_W + x] = swap16(color);
}

static inline uint16_t get(canvas_t *c, int x, int y)
{
    return swap16(c->buf[(y - c->y0) * SCREEN_W + x]);
}

void gfx_blend_pixel(canvas_t *c, int x, int y, uint16_t color, uint8_t alpha)
{
    if (!in_band(c, x, y) || alpha == 0) return;
    put(c, x, y, alpha == 255 ? color : gfx_mix(get(c, x, y), color, alpha));
}

void gfx_fill(canvas_t *c, uint16_t color)
{
    uint16_t v = swap16(color);
    for (int i = 0; i < SCREEN_W * c->h; i++) c->buf[i] = v;
}

void gfx_rect(canvas_t *c, int x, int y, int w, int h, uint16_t color)
{
    int x0 = x < 0 ? 0 : x, x1 = x + w > SCREEN_W ? SCREEN_W : x + w;
    int y0 = y < c->y0 ? c->y0 : y, y1 = y + h > c->y0 + c->h ? c->y0 + c->h : y + h;
    uint16_t v = swap16(color);
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = c->buf + (yy - c->y0) * SCREEN_W;
        for (int xx = x0; xx < x1; xx++) row[xx] = v;
    }
}

void gfx_round_rect(canvas_t *c, int x, int y, int w, int h, int r, uint16_t color)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int yy = y; yy < y + h; yy++) {
        if (yy < c->y0 || yy >= c->y0 + c->h) continue;
        int dy = 0;
        if (yy < y + r) dy = y + r - yy;
        else if (yy >= y + h - r) dy = yy - (y + h - r - 1);
        int inset = 0;
        if (dy) {
            // horizontal inset for the corner circle
            int rr = r * r, d2 = dy * dy;
            int k = 0;
            while (k < r && (r - k) * (r - k) + d2 > rr) k++;
            inset = k;
        }
        gfx_rect(c, x + inset, yy, w - 2 * inset, 1, color);
    }
}

void gfx_hline(canvas_t *c, int x, int y, int w, uint16_t color) { gfx_rect(c, x, y, w, 1, color); }
void gfx_vline(canvas_t *c, int x, int y, int h, uint16_t color) { gfx_rect(c, x, y, 1, h, color); }

// Xiaolin Wu anti-aliased line
void gfx_line(canvas_t *c, int x0, int y0, int x1, int y1, uint16_t color)
{
    int steep = abs(y1 - y0) > abs(x1 - x0);
    float fx0 = x0, fy0 = y0, fx1 = x1, fy1 = y1, t;
    if (steep) { t = fx0; fx0 = fy0; fy0 = t; t = fx1; fx1 = fy1; fy1 = t; }
    if (fx0 > fx1) { t = fx0; fx0 = fx1; fx1 = t; t = fy0; fy0 = fy1; fy1 = t; }
    float dx = fx1 - fx0, dy = fy1 - fy0;
    float grad = dx == 0 ? 1 : dy / dx;
    float y = fy0;
    for (int x = (int)fx0; x <= (int)fx1; x++) {
        int iy = (int)y;
        float f = y - iy;
        uint8_t a1 = (uint8_t)((1 - f) * 255), a2 = (uint8_t)(f * 255);
        if (steep) {
            gfx_blend_pixel(c, iy, x, color, a1);
            gfx_blend_pixel(c, iy + 1, x, color, a2);
        } else {
            gfx_blend_pixel(c, x, iy, color, a1);
            gfx_blend_pixel(c, x, iy + 1, color, a2);
        }
        y += grad;
    }
}

static const glyph_t *glyph_for(const font_t *f, unsigned char ch)
{
    if (ch < f->first || ch >= f->first + f->count) ch = '?';
    return &f->glyphs[ch - f->first];
}

int gfx_text_width(const font_t *f, const char *s)
{
    int w = 0;
    for (; *s; s++) w += glyph_for(f, (unsigned char)*s)->adv;
    return w;
}

int gfx_text(canvas_t *c, const font_t *f, int x, int y, const char *s, uint16_t color, align_t align)
{
    if (align == ALIGN_CENTER) x -= gfx_text_width(f, s) / 2;
    else if (align == ALIGN_RIGHT) x -= gfx_text_width(f, s);
    // Skip quickly if the whole line is outside the band
    if (y >= c->y0 + c->h || y + f->height <= c->y0) {
        return x + gfx_text_width(f, s);
    }
    for (; *s; s++) {
        const glyph_t *g = glyph_for(f, (unsigned char)*s);
        const uint8_t *bits = f->bits + g->offset;
        int gx = x + g->xoff, gy = y + g->yoff;
        for (int row = 0; row < g->h; row++) {
            int py = gy + row;
            if (py < c->y0 || py >= c->y0 + c->h) continue;
            for (int col = 0; col < g->w; col++) {
                int idx = row * g->w + col;
                uint8_t nib = (bits[idx >> 1] >> ((idx & 1) ? 0 : 4)) & 0x0F;
                if (nib) gfx_blend_pixel(c, gx + col, py, color, nib * 17);
            }
        }
        x += g->adv;
    }
    return x;
}

void gfx_disc(canvas_t *c, float cx, float cy, float r, uint16_t color)
{
    int y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
    int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1);
    if (y0 < c->y0) y0 = c->y0;
    if (y1 > c->y0 + c->h - 1) y1 = c->y0 + c->h - 1;
    for (int y = y0; y <= y1; y++) {
        float dy = y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx;
            float d = r - sqrtf(dx * dx + dy * dy) + 0.5f;  // coverage estimate
            if (d <= 0) continue;
            gfx_blend_pixel(c, x, y, color, d >= 1 ? 255 : (uint8_t)(d * 255));
        }
    }
}

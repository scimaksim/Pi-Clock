/*
 * sprites.h - Smoothly moving map markers (satellites overhead)
 *
 * The map is composited once a second. Markers that move visibly in a
 * second (low-orbit satellites in the local view travel ~10 px/s) are
 * instead published here as sprites: their position now and one second
 * later. The framebuffer loop draws them into each frame at the exact
 * time, and a sprite thread redraws just the small patches around them
 * ~30 times a second between frames, over the map and under the panels.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PIC_SPRITES_H
#define PIC_SPRITES_H

#include <cairo/cairo.h>
#include <time.h>

#define PIC_SPRITES_MAX 256

typedef struct {
    char   icon[32];          /* satellite name for pic_draw_sat_icon     */
    char   label[64];
    unsigned char known;      /* 2 landmark icon, 1 icon, 0 dot           */
    unsigned char up;         /* above 10 deg at home                     */
    unsigned char ring;       /* sunlit, up, home dark: "look up"         */
    unsigned char show_label;
    float  r, g, b, a;
    float  label_w;           /* text advance, px                         */
    double lat0, lon0;        /* at t0                                    */
    double lat1, lon1;        /* at t0 + 1 s                              */
} pic_sprite_t;

/* Set by the framebuffer loop when it runs the sprite thread; layers
 * draw their markers themselves when this is 0. */
void pic_sprites_enable(int on);
int  pic_sprites_enabled(void);

/* Replace the sprite set (positions for t0 and t0+1, symbol scale s,
 * font size fs, viewport generation). */
void pic_sprites_publish(const pic_sprite_t *sp, int n, time_t t0,
                         double s, double fs, unsigned gen);

/* Screen rectangles (x0, y0, x1, y1) covered by the sprites at time tf
 * (seconds, fractional); returns how many. 0 when the set is stale. */
int  pic_sprites_rects(double tf, int width, int height, int rects[][4], int max);

/* Draw all sprites at time tf; cr's user space = screen pixels. */
void pic_sprites_draw(cairo_t *cr, double tf, int width, int height);

#endif

/*
 * sprites.c - Smoothly moving map markers (see sprites.h)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "sprites.h"
#include "config.h"
#include "sat_icons.h"
#include <math.h>
#include <pthread.h>
#include <string.h>

static pthread_mutex_t sp_mutex = PTHREAD_MUTEX_INITIALIZER;
static pic_sprite_t sp[PIC_SPRITES_MAX];
static int    sp_n;
static time_t sp_t0;
static double sp_s = 3, sp_fs = 10;
static unsigned sp_gen;
static volatile int sp_on;

void pic_sprites_enable(int on) { sp_on = on; }
int  pic_sprites_enabled(void) { return sp_on; }

void pic_sprites_publish(const pic_sprite_t *list, int n, time_t t0,
                         double s, double fs, unsigned gen)
{
    if (n > PIC_SPRITES_MAX) n = PIC_SPRITES_MAX;
    pthread_mutex_lock(&sp_mutex);
    if (n > 0) memcpy(sp, list, (size_t)n * sizeof(*sp));
    sp_n = n;
    sp_t0 = t0;
    sp_s = s;
    sp_fs = fs;
    sp_gen = gen;
    pthread_mutex_unlock(&sp_mutex);
}

/* Valid set? Published within the last 2.5 s for the current view (the
 * layer republishes every second while it is on and in view). */
static int live(double tf)
{
    return sp_n > 0 && tf - (double)sp_t0 < 2.5 && tf - (double)sp_t0 > -1.0 &&
           sp_gen == pic_config.viewport_gen;
}

static void pos(const pic_sprite_t *p, double f, int width, int height,
                double *x, double *y, double *x0, double *y0)
{
    double dlon = fmod(p->lon1 - p->lon0 + 540.0, 360.0) - 180.0;
    double lat = p->lat0 + (p->lat1 - p->lat0) * f;
    double lon = p->lon0 + dlon * f;
    *x = pic_lon_to_x(lon, width);
    *y = pic_lat_to_y(lat, height);
    *x0 = pic_lon_to_x(p->lon0, width);
    *y0 = pic_lat_to_y(p->lat0, height);
    /* Never draw the lead-in across the map seam */
    if (fabs(*x - *x0) > width / 4.0) { *x0 = *x; *y0 = *y; }
}

static double frac(double tf)
{
    double f = tf - (double)sp_t0;
    return f < 0 ? 0 : f > 2.2 ? 2.2 : f;
}

int pic_sprites_rects(double tf, int width, int height, int rects[][4], int max)
{
    int i, n = 0;
    double f;
    pthread_mutex_lock(&sp_mutex);
    if (!live(tf)) {
        pthread_mutex_unlock(&sp_mutex);
        return 0;
    }
    f = frac(tf);
    for (i = 0; i < sp_n && n < max; i++) {
        const pic_sprite_t *p = &sp[i];
        double x, y, x0, y0, r = sp_s * (p->known == 2 ? 10.5 : p->known ? 7.5 : 5.5) + 3;
        double l, t, rr, b;
        pos(p, f, width, height, &x, &y, &x0, &y0);
        l = fmin(x, x0) - r;
        t = fmin(y, y0) - r;
        rr = fmax(x, x0) + r;
        b = fmax(y, y0) + r;
        if (p->show_label) {
            double lx = x + sp_s * (p->known == 2 ? 9 : p->known ? 6.5 : 4);
            rr = fmax(rr, lx + p->label_w + sp_fs * 0.4);
            t = fmin(t, y - sp_s * 2 - sp_fs * 1.15);
            b = fmax(b, y - sp_s * 2 + sp_fs * 0.5);
        }
        if (rr < 0 || b < 0 || l > width || t > height) continue;
        rects[n][0] = (int)floor(fmax(0, l));
        rects[n][1] = (int)floor(fmax(0, t));
        rects[n][2] = (int)ceil(fmin(width, rr));
        rects[n][3] = (int)ceil(fmin(height, b));
        n++;
    }
    pthread_mutex_unlock(&sp_mutex);
    return n;
}

void pic_sprites_draw(cairo_t *cr, double tf, int width, int height)
{
    int i;
    double f, s, fs;
    pthread_mutex_lock(&sp_mutex);
    if (!live(tf)) {
        pthread_mutex_unlock(&sp_mutex);
        return;
    }
    f = frac(tf);
    s = sp_s;
    fs = sp_fs;
    cairo_save(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs);
    for (i = 0; i < sp_n; i++) {
        const pic_sprite_t *p = &sp[i];
        double x, y, x0, y0;
        pos(p, f, width, height, &x, &y, &x0, &y0);
        if (x < -60 || y < -60 || x > width + 60 || y > height + 60) continue;
        /* lead-in: joins the trail (drawn once a second from t0) */
        if (x != x0 || y != y0) {
            cairo_new_path(cr);
            cairo_move_to(cr, x0, y0);
            cairo_line_to(cr, x, y);
            cairo_set_source_rgba(cr, p->r, p->g, p->b, p->a * 0.4);
            cairo_set_line_width(cr, s * 1.1);
            cairo_stroke(cr);
        }
        if (p->known) {
            double x1 = pic_lon_to_x(p->lon1, width), y1 = pic_lat_to_y(p->lat1, height);
            double ang = fabs(x1 - x0) < width / 4.0 ?
                         pic_sat_icon_angle(p->icon, x1 - x0, y1 - y0) : 0;
            pic_draw_sat_icon(cr, p->icon, x, y, s * (p->known == 2 ? 14 : 10), ang,
                              p->r, p->g, p->b);
        } else {
            cairo_new_path(cr);
            cairo_arc(cr, x, y, s * (p->up ? 2.2 : 1.6), 0, 2 * M_PI);
            cairo_set_source_rgba(cr, p->r, p->g, p->b, p->a);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
            cairo_set_line_width(cr, s * 0.5);
            cairo_stroke(cr);
        }
        if (p->ring) {
            cairo_new_path(cr);
            cairo_arc(cr, x, y, s * (p->known == 2 ? 9 : p->known ? 6.5 : 4.2), 0, 2 * M_PI);
            cairo_set_source_rgba(cr, p->r, p->g, p->b, 0.8);
            cairo_set_line_width(cr, s * 0.6);
            cairo_stroke(cr);
        }
        if (p->show_label) {
            double lx = x + s * (p->known == 2 ? 9 : p->known ? 6.5 : 4), ly = y - s * 2;
            cairo_new_path(cr);
            cairo_move_to(cr, lx, ly);
            cairo_text_path(cr, p->label);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.85);
            cairo_set_line_width(cr, fs * 0.32);
            cairo_stroke_preserve(cr);
            /* in Earth's shadow: light blue-grey, still readable at night */
            if ((p->r + p->g + p->b) / 3.0 < 0.7)
                cairo_set_source_rgba(cr, 0.74, 0.8, 0.93, p->up ? 1.0 : 0.9);
            else
                cairo_set_source_rgba(cr, p->r, p->g, p->b, p->up ? 1.0 : 0.9);
            cairo_fill(cr);
        }
    }
    cairo_restore(cr);
    pthread_mutex_unlock(&sp_mutex);
}

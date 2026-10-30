/*
 * feature.c - Background thread and config plumbing for features
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#define CONF_PATH "/data/etc/pi-clock-renderer.conf"

static void *feature_thread(void *arg)
{
    pic_feature_t *f = arg;

    if (f->needs_network && !pic_wait_for_network(f->name, &f->running))
        return NULL;

    while (pic_feature_running(f)) {
        int wait = f->work(f), s;
        if (wait < 1) wait = 1;
        __atomic_store_n(&f->wake, 0, __ATOMIC_RELEASE);
        for (s = 0; s < wait && !pic_feature_interrupted(f); s++)
            sleep(1);
    }
    return NULL;
}

int pic_feature_start(pic_feature_t *f)
{
    if (!f->work || pic_feature_running(f)) return 0;
    __atomic_store_n(&f->running, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&f->wake, 0, __ATOMIC_RELEASE);
    if (pthread_create(&f->thread, NULL, feature_thread, f) != 0) {
        __atomic_store_n(&f->running, 0, __ATOMIC_RELEASE);
        fprintf(stderr, "%s: failed to create thread\n", f->name);
        return -1;
    }
    printf("%s: fetcher thread started\n", f->name);
    return 0;
}

void pic_feature_stop(pic_feature_t *f)
{
    if (!pic_feature_running(f)) return;
    __atomic_store_n(&f->running, 0, __ATOMIC_RELEASE);
    if (f->interrupt) f->interrupt(f);
    pthread_join(f->thread, NULL);
    if (f->stopped) f->stopped(f);
    printf("%s: fetcher thread stopped\n", f->name);
}

void pic_feature_poke(pic_feature_t *f)
{
    __atomic_store_n(&f->wake, 1, __ATOMIC_RELEASE);
}

int pic_conf_str(const char *key, char *out, int out_size)
{
    const char *path = getenv("PIC_RENDERER_CONF");
    size_t klen = strlen(key);
    char line[512];
    int found = 0;
    FILE *fp;

    fp = fopen(path ? path : CONF_PATH, "r");
    if (!fp) return 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            char *v = line + klen + 1;
            size_t n = strcspn(v, "\r\n");
            v[n] = '\0';
            snprintf(out, (size_t)out_size, "%s", v);
            found = 1;   /* Last one wins, like the other readers */
        }
    }
    fclose(fp);
    return found;
}

int pic_conf_int(const char *key, int def)
{
    char buf[64];
    return pic_conf_str(key, buf, sizeof(buf)) && buf[0] ? atoi(buf) : def;
}

double pic_conf_num(const char *key, double def)
{
    char buf[64];
    return pic_conf_str(key, buf, sizeof(buf)) && buf[0] ? atof(buf) : def;
}

void pic_draw_text_pill(cairo_t *cr, double x, double y, const char *text,
                        double r, double g, double b, double bg_a)
{
    cairo_text_extents_t e;
    cairo_font_extents_t fe;
    double pad;

    cairo_font_extents(cr, &fe);
    cairo_text_extents(cr, text, &e);
    pad = fe.ascent * 0.3;
    cairo_set_source_rgba(cr, 0, 0, 0, bg_a);
    cairo_rectangle(cr, x - pad, y - fe.ascent - pad * 0.5,
                    e.x_advance + pad * 2, fe.ascent + fe.descent + pad);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, r, g, b, 1.0);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, text);
}

void pic_draw_legend_line(cairo_t *cr, int width, int height, int slot,
                          const char *text, double r, double g, double b)
{
    double fs = height / 72.0, y = height - fs * (3.2 + 1.75 * slot);
    cairo_text_extents_t e;

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    cairo_text_extents(cr, text, &e);
    pic_draw_text_pill(cr, (width - e.x_advance) / 2.0, y, text, r, g, b, 0.65);
}

void pic_fmt_until(char *out, int n, time_t now, time_t t)
{
    long d = (long)(t - now);
    if (d <= 0)            snprintf(out, (size_t)n, "now");
    else if (d < 3600)     snprintf(out, (size_t)n, "in %ldm", d / 60);
    else if (d < 86400)    snprintf(out, (size_t)n, "in %ldh %02ldm", d / 3600, (d % 3600) / 60);
    else                   snprintf(out, (size_t)n, "in %ldd %ldh", d / 86400, (d % 86400) / 3600);
}

void pic_fmt_hm(char *out, int n, time_t t)
{
    struct tm lt;
    char b[16];
    localtime_r(&t, &lt);
    strftime(b, sizeof(b), "%I:%M %p", &lt);
    snprintf(out, (size_t)n, "%s", b[0] == '0' ? b + 1 : b);
}

void pic_fmt_when(char *out, int n, time_t now, time_t t)
{
    struct tm a, b;
    char hm[16], d[16];
    localtime_r(&now, &a);
    localtime_r(&t, &b);
    pic_fmt_hm(hm, sizeof(hm), t);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) {
        snprintf(out, (size_t)n, "%s", hm);
    } else if (t > now && t - now < 6 * 86400) {
        strftime(d, sizeof(d), "%a", &b);
        snprintf(out, (size_t)n, "%s %s", d, hm);
    } else {
        strftime(d, sizeof(d), "%b %d", &b);
        if (d[4] == '0') memmove(d + 4, d + 5, strlen(d + 5) + 1);
        snprintf(out, (size_t)n, "%s", d);
    }
}

double pic_dist_km(double lat1, double lon1, double lat2, double lon2)
{
    double r = M_PI / 180.0;
    double dlat = (lat2 - lat1) * r, dlon = (lon2 - lon1) * r;
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(lat1 * r) * cos(lat2 * r) * sin(dlon / 2) * sin(dlon / 2);
    return 6371.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

void pic_fit_text(cairo_t *cr, char *text, double max_w)
{
    cairo_text_extents_t e;
    size_t full = strlen(text), len = full;
    cairo_text_extents(cr, text, &e);
    if (e.x_advance <= max_w) return;
    while (len > 1) {
        /* Step back over a whole UTF-8 sequence */
        do { len--; } while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80);
        if (len + 3 > full) continue;              /* room for "…" */
        memcpy(text + len, "\xe2\x80\xa6", 4);   /* "…" + NUL */
        cairo_text_extents(cr, text, &e);
        if (e.x_advance <= max_w) return;
    }
}

double pic_applet_title(cairo_t *cr, double width, const char *title)
{
    double fs = width / 12.0;
    if (cr) {
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs);
        cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.9);
        cairo_move_to(cr, 0, fs);
        cairo_show_text(cr, title);
        cairo_set_source_rgba(cr, 0.4, 0.4, 0.5, 0.3);
        cairo_set_line_width(cr, 0.5);
        cairo_move_to(cr, 0, fs + 4);
        cairo_line_to(cr, width, fs + 4);
        cairo_stroke(cr);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
    }
    return fs * 1.5;
}

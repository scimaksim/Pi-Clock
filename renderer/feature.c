/*
 * feature.c - Background thread and config plumbing for features
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE  /* pthread_timedjoin_np */
#include "feature.h"
#include "datastatus.h"
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

    pic_status_feature = f->name;      /* user of the sources it fetches */
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

void pic_feature_stop_all(pic_feature_t **f, int n, int timeout_s)
{
    struct timespec deadline;
    char need[64];
    int i;

    if (n > 64) n = 64;
    for (i = 0; i < n; i++) {
        need[i] = (char)pic_feature_running(f[i]);
        if (!need[i]) continue;
        __atomic_store_n(&f[i]->running, 0, __ATOMIC_RELEASE);
        if (f[i]->interrupt) f[i]->interrupt(f[i]);
    }
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_s;
    for (i = 0; i < n; i++) {
        if (!need[i]) continue;
        if (pthread_timedjoin_np(f[i]->thread, NULL, &deadline) == 0)
            printf("%s: fetcher thread stopped\n", f[i]->name);
        else
            printf("%s: still downloading at exit, not waiting\n", f[i]->name);
    }
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

/*
 * Bottom-centre notes stack upward from just above the map key in the
 * order they are drawn each frame, with no gaps for layers that are
 * off. pic_map_key_draw() (called once per frame after all legends)
 * resets the stack.
 */
static double legend_used;   /* rows of height/72*1.75 already taken */

/* Top of the news ticker bar (display.c), 0 when it's off. Weak so
 * test harnesses without display.c still link. */
extern double pic_ticker_bar_top __attribute__((weak));

static double legend_bottom(int height)
{
    double t = &pic_ticker_bar_top ? pic_ticker_bar_top : 0;
    return t > height * 0.5 && t < height ? t : height;
}

double pic_legend_take(int height, double rows)
{
    double fs = height / 72.0, y;
    /* Lowest row clears the map key row, which sits just above the
     * ticker (or the bottom edge) */
    y = legend_bottom(height) - fs * (3.2 + 1.75 * legend_used);
    legend_used += rows;
    return y;
}

void pic_draw_legend_line(cairo_t *cr, int width, int height, int slot,
                          const char *text, double r, double g, double b)
{
    double fs = height / 72.0, y = pic_legend_take(height, 1);
    (void)slot;   /* kept for callers; rows are now assigned in draw order */
    cairo_text_extents_t e;

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    cairo_text_extents(cr, text, &e);
    pic_draw_text_pill(cr, (width - e.x_advance) / 2.0, y, text, r, g, b, 0.65);
}

void pic_draw_legend_swatches(cairo_t *cr, int width, int height, const char *title,
                              const pic_swatch_t *items, int n)
{
    double fs = height / 72.0, y = pic_legend_take(height, 1);
    double sw = fs * 2.0, gap = fs * 0.45, sp = fs * 1.1, w = 0, x, pad;
    cairo_text_extents_t e;
    cairo_font_extents_t fe;
    int i;

    if (n <= 0) return;
    cairo_save(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    cairo_font_extents(cr, &fe);
    if (title && title[0]) {
        cairo_text_extents(cr, title, &e);
        w += e.x_advance + sp;
    }
    for (i = 0; i < n; i++) {
        cairo_text_extents(cr, items[i].label, &e);
        w += sw + gap + e.x_advance + (i + 1 < n ? sp : 0);
    }
    x = (width - w) / 2.0;
    pad = fe.ascent * 0.3;
    cairo_set_source_rgba(cr, 0, 0, 0, 0.65);
    cairo_rectangle(cr, x - pad, y - fe.ascent - pad * 0.5, w + pad * 2,
                    fe.ascent + fe.descent + pad);
    cairo_fill(cr);
    if (title && title[0]) {
        cairo_set_source_rgba(cr, 0.85, 0.87, 0.92, 1.0);
        cairo_move_to(cr, x, y);
        cairo_show_text(cr, title);
        cairo_text_extents(cr, title, &e);
        x += e.x_advance + sp;
    }
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (i = 0; i < n; i++) {
        double ly = y - fe.ascent * 0.35;
        cairo_new_path(cr);
        cairo_move_to(cr, x + fs * 0.15, ly);
        cairo_line_to(cr, x + sw - fs * 0.15, ly);
        if (items[i].dashed) {
            double dash[2] = { fs * 0.42, fs * 0.32 };
            cairo_set_dash(cr, dash, 2, 0);
        }
        cairo_set_source_rgba(cr, items[i].r, items[i].g, items[i].b, 1.0);
        cairo_set_line_width(cr, fs * 0.22);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
        x += sw + gap;
        cairo_set_source_rgba(cr, items[i].r, items[i].g, items[i].b, 1.0);
        cairo_move_to(cr, x, y);
        cairo_show_text(cr, items[i].label);
        cairo_text_extents(cr, items[i].label, &e);
        x += e.x_advance + sp;
    }
    cairo_restore(cr);
}

int pic_view_extent(const double full[4], int screen_w, int max_w,
                    double ext[4], int *w, int *h)
{
    double span = pic_config.view_span_lon, span_lat = pic_config.view_span_lat;
    double v0, v1, s0, s1;

    if (span < 0.0001) span = 360;
    if (span_lat < 0.0001) span_lat = 180;
    v0 = pic_config.view_center_lon - span * 0.65;
    v1 = pic_config.view_center_lon + span * 0.65;
    s0 = pic_config.view_center_lat - span_lat * 0.65;
    s1 = pic_config.view_center_lat + span_lat * 0.65;
    /* Wide or wrapping views: just use the whole extent */
    if (span > 180 || v0 < -180 || v1 > 180) {
        v0 = -180; v1 = 180; s0 = -90; s1 = 90;
    }
    ext[0] = full[0] > v0 ? full[0] : v0;
    ext[2] = full[2] < v1 ? full[2] : v1;
    ext[1] = full[1] > s0 ? full[1] : s0;
    ext[3] = full[3] < s1 ? full[3] : s1;
    if (ext[2] - ext[0] < 0.05 || ext[3] - ext[1] < 0.05) return 0;
    *w = (int)((ext[2] - ext[0]) / span * (screen_w > 0 ? screen_w : 1920));
    if (*w < 64) *w = 64;
    if (*w > max_w) *w = max_w;
    *h = (int)(*w * (ext[3] - ext[1]) / (ext[2] - ext[0]));
    if (*h < 16) *h = 16;
    if (*h > max_w) *h = max_w;
    return 1;
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

void pic_show_text_fit(cairo_t *cr, const char *text, double max_w)
{
    cairo_text_extents_t e;
    cairo_matrix_t fm;
    char buf[256];

    cairo_text_extents(cr, text, &e);
    if (e.x_advance <= max_w || max_w <= 0) {
        cairo_show_text(cr, text);
        return;
    }
    /* Shrink up to ~20%, then shorten with an ellipsis */
    cairo_get_font_matrix(cr, &fm);
    {
        double k = max_w / e.x_advance;
        cairo_matrix_t m = fm;
        if (k < 0.8) k = 0.8;
        cairo_matrix_scale(&m, k, k);
        cairo_set_font_matrix(cr, &m);
    }
    snprintf(buf, sizeof(buf), "%s", text);
    pic_fit_text(cr, buf, max_w);
    cairo_show_text(cr, buf);
    cairo_set_font_matrix(cr, &fm);
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


/* ── Map key ───────────────────────────────────────────────── */
#define MAP_KEY_MAX 10
static struct {
    char   label[48];
    double r, g, b, a;
    int    dashed;
} map_key[MAP_KEY_MAX];
static int map_key_n;

void pic_map_key_add(const char *label, double r, double g, double b,
                     double a, int dashed)
{
    int i;
    for (i = 0; i < map_key_n; i++)
        if (strcmp(map_key[i].label, label) == 0) return;
    if (map_key_n >= MAP_KEY_MAX) return;
    snprintf(map_key[map_key_n].label, sizeof(map_key[0].label), "%s", label);
    map_key[map_key_n].r = r; map_key[map_key_n].g = g;
    map_key[map_key_n].b = b; map_key[map_key_n].a = a;
    map_key[map_key_n].dashed = dashed;
    map_key_n++;
}

static void key_rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void pic_map_key_draw(cairo_t *cr, int width, int height, double bottom)
{
    double fs = height / 90.0, sw = fs * 2.2, gap = fs * 1.2, pad = fs * 0.6;
    double total = 0, x, y, h = fs * 1.7;
    int i, n = map_key_n;

    map_key_n = 0;
    legend_used = 0;
    if (n == 0) return;
    cairo_save(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs);
    for (i = 0; i < n; i++) {
        cairo_text_extents_t e;
        cairo_text_extents(cr, map_key[i].label, &e);
        total += sw + fs * 0.5 + e.x_advance + (i ? gap : 0);
    }
    x = (width - total) / 2.0;
    /* Just above the bottom edge, or above the news ticker if shown */
    (void)bottom;
    y = legend_bottom(height) - h - fs * 0.4;

    cairo_new_path(cr);
    key_rrect(cr, x - pad, y, total + pad * 2, h, h * 0.3);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.55);
    cairo_fill(cr);

    for (i = 0; i < n; i++) {
        cairo_text_extents_t e;
        double my = y + h / 2.0;
        double dsh[2] = { fs * 0.45, fs * 0.35 };
        if (i) x += gap;
        cairo_new_path(cr);
        cairo_move_to(cr, x, my);
        cairo_line_to(cr, x + sw, my);
        cairo_set_dash(cr, dsh, map_key[i].dashed ? 2 : 0, 0);
        cairo_set_line_width(cr, fs * 0.22);
        cairo_set_source_rgba(cr, map_key[i].r, map_key[i].g, map_key[i].b,
                              map_key[i].a < 0.8 ? 0.9 : map_key[i].a);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
        x += sw + fs * 0.5;
        cairo_text_extents(cr, map_key[i].label, &e);
        cairo_set_source_rgba(cr, 0.88, 0.9, 0.95, 0.95);
        cairo_move_to(cr, x, my + fs * 0.35);
        cairo_show_text(cr, map_key[i].label);
        x += e.x_advance;
    }
    cairo_restore(cr);
}

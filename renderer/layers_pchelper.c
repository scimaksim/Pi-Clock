/*
 * layers_pchelper.c - Map layers computed on a PC on the LAN
 *
 * The heavy jobs (radar rain nowcast, HRRR forecast maps, MistNet bird
 * migration) run in the Pi-Clock PC helper (helper/ in this repo). It
 * serves, per product, a manifest.json and a set of frames: raw 8-bit
 * palette indices over a lat/lon box, so the Pi keeps them in memory
 * as is and colours a frame only when it is drawn.
 *
 * Each product is its own layer: the frames are drawn into their box
 * (any zoom, either view), stepping through them once a second with a
 * pause on the last when the manifest asks for animation; a legend
 * shows the colour scale, the time of the frame on screen, and the
 * helper's note about home. Notes carry {t:EPOCH} and {in:EPOCH}
 * tokens, formatted here so "in 35 min" is always current.
 *
 * Settings: PC_HELPER_URL (e.g. http://192.168.1.50:8765). If the PC is
 * off or asleep the layer keeps its last frames while they're valid,
 * then draws nothing and says so in its legend.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "features_all.h"
#include "config.h"
#include "fetch.h"
#include "json.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PCH_MAXF      24
#define PCH_MAN_MAX   (96 * 1024)
#define PCH_OFFLINE_S (20 * 60)

typedef struct {
    const char *product;
    pthread_mutex_t mutex;
    /* Data from the helper */
    time_t   generated, expires, last_ok, last_try;
    int      w, h;
    double   bbox[4];                 /* lon0, lat0, lon1, lat1 */
    uint32_t pal[256];                /* premultiplied ARGB */
    uint32_t pal_rgb[256];            /* opaque, for the legend bar */
    int      npal;
    int      nf;
    unsigned char *frame[PCH_MAXF];
    time_t   ftime[PCH_MAXF];
    int      animate, hold_s, label, now_index;
    char     note[256];
    double   note_rgb[3];
    char     title[96];
    double   lv0, lv1;                /* legend value range */
    int      log_scale, nticks;
    double   ticks[10];
    /* State */
    int      state;                   /* 0 ok, 1 no URL, 2 offline */
    cairo_surface_t *surf;
    int      surf_frame;
    time_t   surf_gen;
} pch_t;

enum { LABEL_CLOCK, LABEL_RELATIVE, LABEL_NONE };

/* Parse a manifest into `out` (frames not loaded). 0 on success. */
static int parse_manifest(const char *js, size_t len, pch_t *out, char files[][48])
{
    pic_json_t j;
    int p, fr, i, lg;

    if (pic_json_parse(&j, js, len) != 0) return -1;
    out->generated = (time_t)pic_json_num(&j, pic_json_get(&j, 0, "generated"), 0);
    out->expires = (time_t)pic_json_num(&j, pic_json_get(&j, 0, "expires"), 0);
    out->w = (int)pic_json_num(&j, pic_json_get(&j, 0, "width"), 0);
    out->h = (int)pic_json_num(&j, pic_json_get(&j, 0, "height"), 0);
    p = pic_json_get(&j, 0, "bbox");
    for (i = 0; i < 4; i++) out->bbox[i] = pic_json_num(&j, pic_json_at(&j, p, i), 0);
    p = pic_json_get(&j, 0, "palette");
    out->npal = pic_json_size(&j, p);
    if (out->npal > 256) out->npal = 256;
    for (i = 0; i < out->npal; i++) {
        int c = pic_json_at(&j, p, i);
        unsigned r = (unsigned)pic_json_num(&j, pic_json_at(&j, c, 0), 0);
        unsigned g = (unsigned)pic_json_num(&j, pic_json_at(&j, c, 1), 0);
        unsigned b = (unsigned)pic_json_num(&j, pic_json_at(&j, c, 2), 0);
        unsigned a = (unsigned)pic_json_num(&j, pic_json_at(&j, c, 3), 0);
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        if (a > 255) a = 255;
        out->pal[i] = (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255);
        out->pal_rgb[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    fr = pic_json_get(&j, 0, "frames");
    out->nf = pic_json_size(&j, fr);
    if (out->nf > PCH_MAXF) out->nf = PCH_MAXF;
    for (i = 0; i < out->nf; i++) {
        int f = pic_json_at(&j, fr, i);
        pic_json_str(&j, pic_json_get(&j, f, "file"), files[i], 48);
        out->ftime[i] = (time_t)pic_json_num(&j, pic_json_get(&j, f, "time"), 0);
        /* file names come from the network: plain names only */
        if (strchr(files[i], '/') || strstr(files[i], "..") || !files[i][0]) {
            pic_json_free(&j);
            return -1;
        }
    }
    out->animate = pic_json_bool(&j, pic_json_get(&j, 0, "animate"), 0);
    out->hold_s = (int)ceil(pic_json_num(&j, pic_json_get(&j, 0, "hold_ms"), 2500) / 1000.0);
    {
        char l[16] = "";
        pic_json_str(&j, pic_json_get(&j, 0, "label"), l, sizeof(l));
        out->label = !strcmp(l, "relative") ? LABEL_RELATIVE :
                     !strcmp(l, "none") ? LABEL_NONE : LABEL_CLOCK;
    }
    out->now_index = (int)pic_json_num(&j, pic_json_get(&j, 0, "now_index"), -1);
    pic_json_str(&j, pic_json_get(&j, 0, "note"), out->note, sizeof(out->note));
    p = pic_json_get(&j, 0, "note_rgb");
    for (i = 0; i < 3; i++) out->note_rgb[i] = pic_json_num(&j, pic_json_at(&j, p, i), 0.85);
    out->title[0] = 0;
    out->nticks = 0;
    lg = pic_json_get(&j, 0, "legend");
    if (lg >= 0 && pic_json_is_object(&j, lg)) {
        int st = pic_json_get(&j, lg, "stops"), tk = pic_json_get(&j, lg, "ticks");
        int ns = pic_json_size(&j, st);
        pic_json_str(&j, pic_json_get(&j, lg, "title"), out->title, sizeof(out->title));
        out->log_scale = pic_json_bool(&j, pic_json_get(&j, lg, "log"), 0);
        out->lv0 = pic_json_num(&j, pic_json_at(&j, pic_json_at(&j, st, 0), 0), 0);
        out->lv1 = pic_json_num(&j, pic_json_at(&j, pic_json_at(&j, st, ns - 1), 0), 1);
        out->nticks = pic_json_size(&j, tk);
        if (out->nticks > 10) out->nticks = 10;
        for (i = 0; i < out->nticks; i++)
            out->ticks[i] = pic_json_num(&j, pic_json_at(&j, tk, i), 0);
    }
    pic_json_free(&j);
    return out->w > 0 && out->h > 0 && out->w * out->h <= 4000000 ? 0 : -1;
}

static int pch_work(pic_feature_t *f)
{
    pch_t *d = (pch_t *)f->data;
    char base[160] = "", url[320], tmp[96];
    static char man[PCH_MAN_MAX];
    char files[PCH_MAXF][48];
    pch_t nm;
    int n, i, ok = 1;
    unsigned char *fr[PCH_MAXF] = { 0 };
    time_t now = time(NULL);

    pic_conf_str("PC_HELPER_URL", base, sizeof(base));
    n = (int)strlen(base);
    while (n > 0 && base[n - 1] == '/') base[--n] = 0;
    if (!base[0] || strncmp(base, "http", 4) != 0) {
        pthread_mutex_lock(&d->mutex);
        d->state = 1;
        pthread_mutex_unlock(&d->mutex);
        return 60;
    }

    snprintf(url, sizeof(url), "%s/%s/manifest.json", base, d->product);
    n = pic_fetch_to_buf(url, man, sizeof(man) - 1);
    pthread_mutex_lock(&d->mutex);
    d->last_try = now;
    if (n <= 0) {
        if (now - d->last_ok > PCH_OFFLINE_S) d->state = 2;
        pthread_mutex_unlock(&d->mutex);
        return 60;
    }
    pthread_mutex_unlock(&d->mutex);
    man[n] = 0;

    memset(&nm, 0, sizeof(nm));
    if (parse_manifest(man, (size_t)n, &nm, files) != 0) {
        printf("%s: bad manifest from helper\n", d->product);
        return 120;
    }
    pthread_mutex_lock(&d->mutex);
    if (nm.generated == d->generated) {      /* nothing new */
        d->last_ok = now;
        d->state = 0;
        d->expires = nm.expires;
        pthread_mutex_unlock(&d->mutex);
        return 60;
    }
    pthread_mutex_unlock(&d->mutex);

    /* Frames */
    snprintf(tmp, sizeof(tmp), "/tmp/pi-clock-pch-%s.u8", d->product);
    for (i = 0; i < nm.nf && ok; i++) {
        struct stat st;
        FILE *fp;
        snprintf(url, sizeof(url), "%s/%s/%s", base, d->product, files[i]);
        if (pic_fetch_to_file(url, tmp, 30) != 0 || stat(tmp, &st) != 0 ||
            st.st_size != (off_t)nm.w * nm.h) {
            ok = 0;
            break;
        }
        fr[i] = malloc((size_t)nm.w * nm.h);
        fp = fopen(tmp, "rb");
        if (!fr[i] || !fp || fread(fr[i], 1, (size_t)nm.w * nm.h, fp) != (size_t)nm.w * nm.h)
            ok = 0;
        if (fp) fclose(fp);
    }
    unlink(tmp);
    if (!ok) {
        for (i = 0; i < PCH_MAXF; i++) free(fr[i]);
        printf("%s: frame download failed\n", d->product);
        return 60;
    }

    pthread_mutex_lock(&d->mutex);
    for (i = 0; i < PCH_MAXF; i++) {
        free(d->frame[i]);
        d->frame[i] = i < nm.nf ? fr[i] : NULL;
    }
    d->generated = nm.generated;
    d->expires = nm.expires;
    d->w = nm.w;
    d->h = nm.h;
    memcpy(d->bbox, nm.bbox, sizeof(d->bbox));
    memcpy(d->pal, nm.pal, sizeof(d->pal));
    memcpy(d->pal_rgb, nm.pal_rgb, sizeof(d->pal_rgb));
    d->npal = nm.npal;
    d->nf = nm.nf;
    memcpy(d->ftime, nm.ftime, sizeof(d->ftime));
    d->animate = nm.animate;
    d->hold_s = nm.hold_s < 1 ? 1 : nm.hold_s;
    d->label = nm.label;
    d->now_index = nm.now_index;
    memcpy(d->note, nm.note, sizeof(d->note));
    memcpy(d->note_rgb, nm.note_rgb, sizeof(d->note_rgb));
    memcpy(d->title, nm.title, sizeof(d->title));
    d->lv0 = nm.lv0;
    d->lv1 = nm.lv1;
    d->log_scale = nm.log_scale;
    d->nticks = nm.nticks;
    memcpy(d->ticks, nm.ticks, sizeof(d->ticks));
    d->last_ok = now;
    d->state = 0;
    d->surf_frame = -1;
    pthread_mutex_unlock(&d->mutex);
    printf("%s: %d frames %dx%d from the PC helper\n", d->product, nm.nf, nm.w, nm.h);
    return 60;
}

/* Frame on screen now: animated = one per second with a pause on the
 * last; otherwise the latest frame not after now (or the first). */
static int current_frame(const pch_t *d, time_t now)
{
    int i, k = 0;
    if (d->nf <= 0) return -1;
    if (d->animate && d->nf > 1) {
        int cycle = d->nf + d->hold_s;
        k = (int)(now % cycle);
        return k >= d->nf ? d->nf - 1 : k;
    }
    for (i = 0; i < d->nf; i++)
        if (d->ftime[i] <= now) k = i;
    return k;
}

static int usable(const pch_t *d, time_t now)
{
    return d->state == 0 && d->nf > 0 && now < d->expires + 15 * 60;
}

static void pch_render(cairo_t *cr, int width, int height, time_t now, void *ud)
{
    pch_t *d = (pch_t *)ud;
    double x0, x1, y0, y1;
    int k;

    if (!d) return;
    pthread_mutex_lock(&d->mutex);
    if (!usable(d, now) || (k = current_frame(d, now)) < 0 || !d->frame[k]) {
        pthread_mutex_unlock(&d->mutex);
        return;
    }
    if (!d->surf || d->surf_frame != k || d->surf_gen != d->generated ||
        cairo_image_surface_get_width(d->surf) != d->w ||
        cairo_image_surface_get_height(d->surf) != d->h) {
        unsigned char *px;
        int stride, x, y;
        if (d->surf && (cairo_image_surface_get_width(d->surf) != d->w ||
                        cairo_image_surface_get_height(d->surf) != d->h)) {
            cairo_surface_destroy(d->surf);
            d->surf = NULL;
        }
        if (!d->surf) d->surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, d->w, d->h);
        if (cairo_surface_status(d->surf) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(d->surf);
            d->surf = NULL;
            pthread_mutex_unlock(&d->mutex);
            return;
        }
        cairo_surface_flush(d->surf);
        px = cairo_image_surface_get_data(d->surf);
        stride = cairo_image_surface_get_stride(d->surf);
        for (y = 0; y < d->h; y++) {
            uint32_t *row = (uint32_t *)(px + (size_t)y * stride);
            const unsigned char *src = d->frame[k] + (size_t)y * d->w;
            for (x = 0; x < d->w; x++)
                row[x] = src[x] < d->npal ? d->pal[src[x]] : 0;
        }
        cairo_surface_mark_dirty(d->surf);
        d->surf_frame = k;
        d->surf_gen = d->generated;
    }
    x0 = pic_lon_to_x(d->bbox[0], width);
    x1 = pic_lon_to_x(d->bbox[2], width);
    y0 = pic_lat_to_y(d->bbox[3], height);
    y1 = pic_lat_to_y(d->bbox[1], height);
    if (x1 < x0) x1 += pic_lon_to_x(180, width) - pic_lon_to_x(-180, width);
    if (x1 > x0 + 1 && y1 > y0 + 1 && x1 > 0 && x0 < width && y1 > 0 && y0 < height) {
        cairo_save(cr);
        cairo_rectangle(cr, x0, y0, x1 - x0, y1 - y0);
        cairo_clip(cr);
        cairo_translate(cr, x0, y0);
        cairo_scale(cr, (x1 - x0) / d->w, (y1 - y0) / d->h);
        cairo_set_source_surface(cr, d->surf, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    pthread_mutex_unlock(&d->mutex);
}

/* Replace {t:EPOCH} (clock time) and {in:EPOCH} (relative) tokens */
static void format_note(const char *in, char *out, size_t n, time_t now)
{
    size_t o = 0;
    while (*in && o + 1 < n) {
        long long v;
        int used = 0;
        char kind[4] = "";
        if (*in == '{' && sscanf(in, "{%3[a-z]:%lld}%n", kind, &v, &used) == 2 && used > 0) {
            char buf[32];
            if (!strcmp(kind, "t")) {
                pic_fmt_hm(buf, sizeof(buf), (time_t)v);
            } else {
                long m = (long)((v - now) / 60);
                if (m <= 0) snprintf(buf, sizeof(buf), "now");
                else if (m < 60) snprintf(buf, sizeof(buf), "in %ld min", m);
                else snprintf(buf, sizeof(buf), "in %ld h %02ld min", m / 60, m % 60);
            }
            o += (size_t)snprintf(out + o, n - o, "%s", buf);
            if (o >= n) o = n - 1;
            in += used;
            continue;
        }
        out[o++] = *in++;
    }
    out[o] = 0;
}

static double tick_pos(const pch_t *d, double v)
{
    if (d->log_scale && v > 0 && d->lv0 > 0)
        return (log10(v) - log10(d->lv0)) / (log10(d->lv1) - log10(d->lv0));
    return (v - d->lv0) / (d->lv1 - d->lv0);
}

static void pch_legend(cairo_t *cr, int width, int height, time_t now, void *ud)
{
    pch_t *d = (pch_t *)ud;
    double fs = height / 90.0, bw = fs * 16, bh = fs * 0.75, pad = fs * 0.6;
    double x0, y, tw, lw, boxw, bx;
    char note[300] = "", when[48] = "", title[96];
    cairo_text_extents_t e;
    int i, k, state, has_bar;

    if (!d) return;
    pthread_mutex_lock(&d->mutex);
    state = d->state;
    if (state == 1) {
        pthread_mutex_unlock(&d->mutex);
        pic_draw_legend_line(cr, width, height, 1,
                             "PC layers: set the PC helper address in the dashboard",
                             0.8, 0.8, 0.85);
        return;
    }
    if (!usable(d, now)) {
        int had = d->last_ok > 0;
        pthread_mutex_unlock(&d->mutex);
        if (had || state == 2) {
            snprintf(note, sizeof(note), "%s: PC helper offline", d->product);
            pic_draw_legend_line(cr, width, height, 1, note, 0.7, 0.7, 0.75);
        }
        return;
    }
    format_note(d->note, note, sizeof(note), now);
    k = current_frame(d, now);
    if (k >= 0 && d->label != LABEL_NONE) {
        if (d->label == LABEL_RELATIVE) {
            long m = (long)lround((double)(d->ftime[k] - now) / 60.0);
            const char *kind = d->now_index >= 0 && k > d->now_index ? "forecast" : "radar";
            if (labs(m) < 3) snprintf(when, sizeof(when), "%s now", kind);
            else snprintf(when, sizeof(when), "%s %+ld min", kind, m);
        } else {
            struct tm lt;
            char hm[16];
            localtime_r(&d->ftime[k], &lt);
            pic_fmt_hm(hm, sizeof(hm), d->ftime[k]);
            strftime(when, sizeof(when), "%a ", &lt);
            strncat(when, hm, sizeof(when) - strlen(when) - 1);
        }
    }
    has_bar = d->title[0] && d->npal > 1 && d->nf > 0;
    memcpy(title, d->title, sizeof(title));

    /* Note first (it stacks below the bar) */
    if (note[0])
        pic_draw_legend_line(cr, width, height, 1, note, d->note_rgb[0], d->note_rgb[1],
                             d->note_rgb[2]);
    if (!has_bar) {
        pthread_mutex_unlock(&d->mutex);
        return;
    }

    cairo_save(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    cairo_text_extents(cr, title, &e);
    tw = e.x_advance + fs * 0.8;
    cairo_text_extents(cr, when[0] ? when : "", &e);
    lw = when[0] ? e.x_advance + fs * 1.0 : 0;
    boxw = tw + bw + lw + pad * 2;
    x0 = (width - boxw) / 2.0;
    {
        double f72 = height / 72.0;
        double bottom = pic_legend_take(height, 1.5) + f72 * 0.45;
        y = bottom - (bh + fs * 1.75) + fs * 0.2;
    }
    cairo_new_path(cr);
    cairo_rectangle(cr, x0, y - fs * 0.2, boxw, bh + fs * 1.75);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.9, 0.92, 0.96, 0.95);
    cairo_move_to(cr, x0 + pad, y + bh * 0.9);
    cairo_show_text(cr, title);

    bx = x0 + pad + tw;
    for (i = 1; i < d->npal; i++) {
        uint32_t c = d->pal_rgb[i];
        cairo_rectangle(cr, bx + bw * (i - 1) / (d->npal - 1), y, bw / (d->npal - 1) + 0.6, bh);
        cairo_set_source_rgb(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0,
                             (c & 255) / 255.0);
        cairo_fill(cr);
    }
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs * 0.8);
    cairo_set_source_rgba(cr, 0.85, 0.87, 0.9, 0.95);
    for (i = 0; i < d->nticks; i++) {
        char lab[16];
        double p = tick_pos(d, d->ticks[i]), xx;
        if (p < -0.01 || p > 1.01) continue;
        snprintf(lab, sizeof(lab), "%g", d->ticks[i]);
        cairo_text_extents(cr, lab, &e);
        xx = bx + bw * p - e.x_advance / 2;
        if (xx < bx) xx = bx;
        if (xx + e.x_advance > bx + bw) xx = bx + bw - e.x_advance;
        cairo_move_to(cr, xx, y + bh + fs * 1.05);
        cairo_show_text(cr, lab);
    }
    if (when[0]) {
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs);
        cairo_set_source_rgba(cr, 1.0, 0.9, 0.55, 0.95);
        cairo_move_to(cr, bx + bw + fs * 0.6, y + bh * 0.9);
        cairo_show_text(cr, when);
    }
    cairo_restore(cr);
    pthread_mutex_unlock(&d->mutex);
}

static void pch_stopped(pic_feature_t *f)
{
    pch_t *d = (pch_t *)f->data;
    int i;
    pthread_mutex_lock(&d->mutex);
    for (i = 0; i < PCH_MAXF; i++) {
        free(d->frame[i]);
        d->frame[i] = NULL;
    }
    d->nf = 0;
    d->generated = 0;
    d->last_ok = 0;
    if (d->surf) cairo_surface_destroy(d->surf);
    d->surf = NULL;
    pthread_mutex_unlock(&d->mutex);
}

/* ── The products ─────────────────────────────────────────────────── */

#define PCH(var, prod, every)                                                \
    static pch_t var##_d = { .product = prod, .surf_frame = -1 };            \
    static pic_feature_t var##_f = {                                         \
        .name = prod, .layer = pch_render, .legend = pch_legend,             \
        .interval = every, .extra_mb = 8, .work = pch_work, .needs_network = 1, \
        .stopped = pch_stopped, .data = &var##_d,                            \
    };                                                                       \
    pic_feature_t *pic_feature_pch_##var(void)                               \
    {                                                                        \
        static int inited;                                                   \
        if (!inited) { pthread_mutex_init(&var##_d.mutex, NULL); inited = 1; } \
        return &var##_f;                                                     \
    }

/* Animated products redraw every second (one frame per second);
 * the single-frame frost map once a minute */
PCH(nowcast, "nowcast", 1)
PCH(clouds, "hrrr_clouds", 1)
PCH(smoke, "hrrr_smoke", 1)
PCH(gust, "hrrr_gust", 1)
PCH(frost, "hrrr_frost", 60)
PCH(birds, "birds", 1)

/*
 * localview.c - Global / local view switching and local detail
 *
 * See localview.h. Three parts:
 *
 *   1. View manager: reads VIEW_MODE / LOCAL_ZOOM / VIEW_*_MIN and
 *      sets pic_config.local_zoom, flipping between views on a timer
 *      in cycle mode.
 *   2. Regional base maps: in the local view, NASA GIBS Blue Marble
 *      (day) and VIIRS Black Marble (night) are fetched for the
 *      visible area at screen resolution and used in place of the
 *      world maps, which are too coarse at 8-24x. Kept in memory so
 *      cycling back is instant; refetched only if the area changes.
 *   3. Local Detail layer: Natural Earth 1:10m coastlines, country
 *      borders, state/province lines and towns (geodata/localmap.dat,
 *      built by generate_localmap.py), drawn when zoomed in.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "localview.h"
#include "config.h"
#include "fetch.h"
#include "stb_image.h"
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── 1. View manager ───────────────────────────────────────────── */

enum { VIEW_GLOBAL, VIEW_LOCAL, VIEW_CYCLE };

static int    view_mode = VIEW_GLOBAL;
static double local_zoom = 8;
static int    global_min = 5, local_min = 5;
static int    showing_local;
static time_t switch_at;
static int    cycle_started;     /* switch_at is live for the cycle */

void pic_view_reload(time_t now)
{
    char m[16] = "";
    pic_conf_str("VIEW_MODE", m, sizeof(m));
    view_mode = !strcmp(m, "local") ? VIEW_LOCAL :
                !strcmp(m, "cycle") ? VIEW_CYCLE : VIEW_GLOBAL;
    local_zoom = pic_conf_num("LOCAL_ZOOM", 8);
    if (local_zoom < 2) local_zoom = 2;
    if (local_zoom > 30) local_zoom = 30;
    global_min = pic_conf_int("VIEW_GLOBAL_MIN", 5);
    local_min = pic_conf_int("VIEW_LOCAL_MIN", 5);
    if (global_min < 1) global_min = 1;
    if (local_min < 1) local_min = 1;
    /*
     * Any dashboard save reloads this. When auto-switching, stay in the
     * view now on screen (so layer changes can be seen in it) and keep
     * the timer, only shortening it if the new duration is shorter.
     */
    if (view_mode == VIEW_CYCLE) {
        time_t max_at = now + (time_t)(showing_local ? local_min : global_min) * 60;
        if (!cycle_started || switch_at > max_at) switch_at = max_at;
        cycle_started = 1;
    } else {
        showing_local = view_mode == VIEW_LOCAL;
        cycle_started = 0;
    }
    pic_config.local_zoom = showing_local ? local_zoom : 0;
    printf("view: mode=%s local_zoom=%.0fx global=%dmin local=%dmin\n",
           view_mode == VIEW_LOCAL ? "local" : view_mode == VIEW_CYCLE ? "cycle" : "global",
           local_zoom, global_min, local_min);
}

int pic_view_update(time_t now)
{
    if (view_mode != VIEW_CYCLE || now < switch_at) return 0;
    showing_local = !showing_local;
    switch_at = now + (time_t)(showing_local ? local_min : global_min) * 60;
    pic_config.local_zoom = showing_local ? local_zoom : 0;
    pic_config_recompute_viewport();
    printf("view: switched to %s view\n", showing_local ? "local" : "global");
    return 1;
}

int pic_view_is_local(void)
{
    return showing_local;
}

/* ── 2. Regional base maps ─────────────────────────────────────── */

#define GIBS_BASE \
    "https://gibs.earthdata.nasa.gov/wms/epsg4326/best/wms.cgi" \
    "?SERVICE=WMS&REQUEST=GetMap&VERSION=1.1.1&LAYERS=%s&STYLES=" \
    "&SRS=EPSG:4326&BBOX=%.4f,%.4f,%.4f,%.4f&WIDTH=%d&HEIGHT=%d" \
    "&FORMAT=image/jpeg%s"
#define REG_TMP "/tmp/pi-clock-regional.jpg"

typedef struct {
    cairo_surface_t *world;      /* World map this image stands in for */
    cairo_surface_t *img;
    double lon0, lon1, lat0, lat1;
} regional_t;

static pthread_mutex_t reg_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned reg_gen;         /* Bumped when new imagery arrives */

unsigned pic_regional_generation(void)
{
    return __atomic_load_n(&reg_gen, __ATOMIC_ACQUIRE);
}
static regional_t reg[2];        /* 0 = night, 1 = day */
static int reg_screen_w = 1920, reg_screen_h = 1080;

void pic_regional_set_world(cairo_surface_t *night, cairo_surface_t *day)
{
    pthread_mutex_lock(&reg_mutex);
    reg[0].world = night;
    reg[1].world = day;
    pthread_mutex_unlock(&reg_mutex);
}

cairo_pattern_t *pic_regional_pattern(cairo_surface_t *src, int width,
                                      int height)
{
    double span_lon = pic_config.view_span_lon, span_lat = pic_config.view_span_lat;
    double vlon0 = pic_config.view_center_lon - span_lon / 2;
    double vlat1 = pic_config.view_center_lat + span_lat / 2;
    cairo_pattern_t *pat = NULL;
    cairo_matrix_t m;
    int i;

    if (!src || pic_config.local_zoom <= 1.0) return NULL;
    pthread_mutex_lock(&reg_mutex);
    reg_screen_w = width;
    reg_screen_h = height;
    for (i = 0; i < 2; i++) {
        regional_t *r = &reg[i];
        double rw, rh;
        if (r->world != src || !r->img) continue;
        /* Must cover the whole viewport */
        if (vlon0 < r->lon0 || vlon0 + span_lon > r->lon1 ||
            vlat1 > r->lat1 || vlat1 - span_lat < r->lat0)
            continue;
        rw = cairo_image_surface_get_width(r->img);
        rh = cairo_image_surface_get_height(r->img);
        pat = cairo_pattern_create_for_surface(r->img);   /* holds a ref */
        cairo_matrix_init(&m,
                          span_lon * rw / ((r->lon1 - r->lon0) * width), 0, 0,
                          span_lat * rh / ((r->lat1 - r->lat0) * height),
                          (vlon0 - r->lon0) * rw / (r->lon1 - r->lon0),
                          (r->lat1 - vlat1) * rh / (r->lat1 - r->lat0));
        cairo_pattern_set_matrix(pat, &m);
        cairo_pattern_set_extend(pat, CAIRO_EXTEND_PAD);
        cairo_pattern_set_filter(pat, CAIRO_FILTER_BILINEAR);
        break;
    }
    pthread_mutex_unlock(&reg_mutex);
    return pat;
}

static cairo_surface_t *load_jpeg_surface(const char *path, int want_w)
{
    int w, h, ch, x, y, stride;
    unsigned char *px = stbi_load(path, &w, &h, &ch, 3), *d;
    cairo_surface_t *s;

    if (!px) return NULL;
    if (w != want_w) {
        stbi_image_free(px);
        return NULL;
    }
    s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        stbi_image_free(px);
        return NULL;
    }
    cairo_surface_flush(s);
    d = cairo_image_surface_get_data(s);
    stride = cairo_image_surface_get_stride(s);
    for (y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(d + (size_t)y * stride);
        const unsigned char *p = px + (size_t)y * w * 3;
        for (x = 0; x < w; x++, p += 3)
            row[x] = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
    }
    cairo_surface_mark_dirty(s);
    stbi_image_free(px);
    return s;
}

static int localbase_work(pic_feature_t *f)
{
    static const struct { const char *layer, *extra; } src[2] = {
        { "VIIRS_Black_Marble", "&TIME=2016-01-01" },
        { "BlueMarble_NextGeneration", "" },
    };
    double z = pic_config.local_zoom, span_lon, span_lat, cx, cy;
    double lon0, lon1, lat0, lat1;
    int sw, sh, w, h, i, s;
    unsigned gen = pic_config.viewport_gen;

    if (z <= 1.0) {
        /* Global view right now: compute the local area anyway so the
         * imagery is ready when the cycle switches */
        z = local_zoom;
    }
    span_lon = 360.0 / z;
    span_lat = 180.0 / z;
    cx = pic_config.qth_lon;
    cy = pic_config.qth_lat;
    if (cy + span_lat / 2 > 90) cy = 90 - span_lat / 2;
    if (cy - span_lat / 2 < -90) cy = -90 + span_lat / 2;
    /* 10% margin so small pans still fit */
    lon0 = cx - span_lon * 0.6; lon1 = cx + span_lon * 0.6;
    lat0 = cy - span_lat * 0.6; lat1 = cy + span_lat * 0.6;
    if (lat0 < -90) lat0 = -90;
    if (lat1 > 90) lat1 = 90;
    if (lon0 < -180 || lon1 > 180 || z < 3) goto wait;   /* not supported */

    pthread_mutex_lock(&reg_mutex);
    sw = reg_screen_w;
    sh = reg_screen_h;
    s = reg[0].img && reg[1].img && reg[0].lon0 == lon0 && reg[0].lat0 == lat0 &&
        reg[0].lon1 == lon1 && reg[0].lat1 == lat1;
    pthread_mutex_unlock(&reg_mutex);
    if (s) goto wait;   /* already have this area */

    w = (int)(sw * 1.2);
    h = (int)(sh * 1.2);
    if (w > 2400) { h = h * 2400 / w; w = 2400; }

    for (i = 0; i < 2 && !pic_feature_interrupted(f); i++) {
        char url[512];
        cairo_surface_t *img = NULL;
        snprintf(url, sizeof(url), GIBS_BASE, src[i].layer, lon0, lat0,
                 lon1, lat1, w, h, src[i].extra);
        if (pic_fetch_to_file(url, REG_TMP, 90) == 0)
            img = load_jpeg_surface(REG_TMP, w);
        unlink(REG_TMP);
        if (!img) {
            printf("localview: %s fetch failed\n", src[i].layer);
            return 300;
        }
        pthread_mutex_lock(&reg_mutex);
        if (reg[i].img) cairo_surface_destroy(reg[i].img);
        reg[i].img = img;
        reg[i].lon0 = lon0; reg[i].lon1 = lon1;
        reg[i].lat0 = lat0; reg[i].lat1 = lat1;
        pthread_mutex_unlock(&reg_mutex);
        printf("localview: %s %dx%d for %.1f,%.1f..%.1f,%.1f\n",
               src[i].layer, w, h, lon0, lat0, lon1, lat1);
    }
    /* Make the base maps re-render with the new imagery */
    __atomic_add_fetch(&reg_gen, 1, __ATOMIC_RELEASE);

wait:
    for (s = 0; s < 3600; s++) {
        if (pic_feature_interrupted(f)) return 1;
        if (pic_config.viewport_gen != gen) return 2;
        sleep(1);
    }
    return 1;
}

static int localbase_wanted(pic_feature_t *f)
{
    (void)f;
    return view_mode != VIEW_GLOBAL;
}

static void localbase_stopped(pic_feature_t *f)
{
    int i;
    (void)f;
    pthread_mutex_lock(&reg_mutex);
    for (i = 0; i < 2; i++) {
        if (reg[i].img) cairo_surface_destroy(reg[i].img);
        reg[i].img = NULL;
    }
    pthread_mutex_unlock(&reg_mutex);
}

static pic_feature_t localbase_def = {
    .name = "localbase", .wanted = localbase_wanted,
    .work = localbase_work, .needs_network = 1, .stopped = localbase_stopped,
};

pic_feature_t *pic_feature_localbase(void)
{
    return &localbase_def;
}

/* ── 3. Local Detail layer ─────────────────────────────────────── */

typedef struct {
    uint8_t  kind;
    float    lon0, lat0, lon1, lat1;
    uint32_t n;
    float   *pts;      /* lon,lat interleaved */
} lm_line_t;

typedef struct {
    float    lon, lat;
    uint32_t pop;
    uint8_t  rank, flags;
    char     name[64];
} lm_place_t;

static lm_line_t  *lm_lines;
static uint32_t    lm_nlines;
static lm_place_t *lm_places;
static uint32_t    lm_nplaces;
static float      *lm_pool;
static int         lm_labels = 1;
static pic_feature_t localmap_def;   /* defined at the end */

int pic_localmap_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    char magic[8];
    long size;
    uint32_t i, used = 0;
    unsigned char hdr[24];

    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "PCLMAP1", 8) ||
        fread(&lm_nlines, 4, 1, f) != 1 || lm_nlines > 200000)
        goto bad;
    lm_lines = calloc(lm_nlines, sizeof(lm_line_t));
    lm_pool = malloc((size_t)size);   /* points fit in the file size */
    if (!lm_lines || !lm_pool) goto bad;
    for (i = 0; i < lm_nlines; i++) {
        lm_line_t *l = &lm_lines[i];
        if (fread(hdr, 1, 24, f) != 24) goto bad;
        l->kind = hdr[0];
        memcpy(&l->lon0, hdr + 4, 4);
        memcpy(&l->lat0, hdr + 8, 4);
        memcpy(&l->lon1, hdr + 12, 4);
        memcpy(&l->lat1, hdr + 16, 4);
        memcpy(&l->n, hdr + 20, 4);
        if ((long)(used + l->n * 2) * 4 > size) goto bad;
        l->pts = lm_pool + used;
        if (fread(l->pts, 8, l->n, f) != l->n) goto bad;
        used += l->n * 2;
    }
    if (fread(&lm_nplaces, 4, 1, f) != 1 || lm_nplaces > 50000) goto bad;
    lm_places = calloc(lm_nplaces, sizeof(lm_place_t));
    if (!lm_places) goto bad;
    for (i = 0; i < lm_nplaces; i++) {
        unsigned char ph[15];
        lm_place_t *p = &lm_places[i];
        if (fread(ph, 1, 15, f) != 15) goto bad;
        memcpy(&p->lon, ph, 4);
        memcpy(&p->lat, ph + 4, 4);
        memcpy(&p->pop, ph + 8, 4);
        p->rank = ph[12];
        p->flags = ph[13];
        if (ph[14] > 63 || fread(p->name, 1, ph[14], f) != ph[14]) goto bad;
        p->name[ph[14]] = '\0';
    }
    fclose(f);
    printf("localmap: %u lines, %u places\n", lm_nlines, lm_nplaces);
    return 0;
bad:
    fclose(f);
    free(lm_lines); free(lm_pool); free(lm_places);
    lm_lines = NULL; lm_pool = NULL; lm_places = NULL;
    lm_nlines = lm_nplaces = 0;
    fprintf(stderr, "localmap: bad or missing %s\n", path);
    return -1;
}

static int cmp_pop(const void *a, const void *b)
{
    const lm_place_t *x = *(const lm_place_t * const *)a;
    const lm_place_t *y = *(const lm_place_t * const *)b;
    return (y->pop > x->pop) - (y->pop < x->pop);
}

void pic_layer_render_localmap(cairo_t *cr, int width, int height,
                               time_t now, void *ud)
{
    double z = pic_zoom_factor();
    double span = pic_config.view_span_lon, span_lat = pic_config.view_span_lat;
    double vlon0 = pic_config.view_center_lon - span / 2, vlon1 = vlon0 + span;
    double vlat1 = pic_config.view_center_lat + span_lat / 2, vlat0 = vlat1 - span_lat;
    double ppd = width / span, fs = height / 80.0;
    uint32_t i, k;
    int pass;

    (void)now; (void)ud;
    if (z < 2.5 || !lm_lines) return;   /* world view: 110m borders suffice */

    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    /* States first (under), then coast, then country borders */
    for (pass = 0; pass < 3; pass++) {
        int kind = pass == 0 ? 2 : pass == 1 ? 0 : 1;
        if (kind == 2) {
            double d[2] = { 6, 4 };
            cairo_set_dash(cr, d, 2, 0);
            cairo_set_source_rgba(cr, 0.85, 0.85, 0.9, 0.55);
            cairo_set_line_width(cr, 1.0);
        } else if (kind == 0) {
            cairo_set_dash(cr, NULL, 0, 0);
            cairo_set_source_rgba(cr, 0.75, 0.88, 1.0, 0.75);
            cairo_set_line_width(cr, 1.1);
        } else {
            cairo_set_dash(cr, NULL, 0, 0);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.85);
            cairo_set_line_width(cr, 1.6);
        }
        for (i = 0; i < lm_nlines; i++) {
            const lm_line_t *l = &lm_lines[i];
            if (l->kind != kind || l->lon1 < vlon0 || l->lon0 > vlon1 ||
                l->lat1 < vlat0 || l->lat0 > vlat1)
                continue;
            for (k = 0; k < l->n; k++) {
                double x = (l->pts[2 * k] - vlon0) * ppd;
                double y = (vlat1 - l->pts[2 * k + 1]) * height / span_lat;
                if (k == 0) cairo_move_to(cr, x, y);
                else cairo_line_to(cr, x, y);
            }
            cairo_stroke(cr);
        }
    }
    cairo_set_dash(cr, NULL, 0, 0);

    /* Towns: biggest first, skipping any that would overlap */
    if (lm_labels && lm_nplaces) {
        static const lm_place_t **vis;
        static double (*boxes)[4];
        int nvis = 0, nbox = 0, maxlab;
        if (!vis) vis = malloc(lm_nplaces * sizeof(*vis));
        if (!boxes) boxes = malloc(400 * sizeof(*boxes));
        if (!vis || !boxes) return;
        for (i = 0; i < lm_nplaces; i++) {
            const lm_place_t *p = &lm_places[i];
            if (p->lon >= vlon0 && p->lon <= vlon1 && p->lat >= vlat0 && p->lat <= vlat1)
                vis[nvis++] = p;
        }
        qsort(vis, (size_t)nvis, sizeof(*vis), cmp_pop);
        maxlab = z >= 12 ? 80 : z >= 6 ? 50 : 30;
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        for (i = 0; i < (uint32_t)nvis && nbox < maxlab && nbox < 400; i++) {
            const lm_place_t *p = vis[i];
            double x = (p->lon - vlon0) * ppd, y = (vlat1 - p->lat) * height / span_lat;
            double f = p->pop > 1000000 ? fs * 1.15 : p->pop > 200000 ? fs : fs * 0.85;
            cairo_text_extents_t e;
            double bx0, by0, bx1, by1;
            int b, hit = 0;
            cairo_set_font_size(cr, f);
            cairo_text_extents(cr, p->name, &e);
            bx0 = x - 4; by0 = y - f; bx1 = x + 8 + e.x_advance; by1 = y + f * 0.4;
            for (b = 0; b < nbox && !hit; b++)
                hit = !(bx1 < boxes[b][0] || bx0 > boxes[b][2] ||
                        by1 < boxes[b][1] || by0 > boxes[b][3]);
            if (hit) continue;
            boxes[nbox][0] = bx0; boxes[nbox][1] = by0;
            boxes[nbox][2] = bx1; boxes[nbox][3] = by1;
            nbox++;
            cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
            cairo_arc(cr, x, y, f * 0.32, 0, 2 * M_PI);
            cairo_fill(cr);
            if (p->flags & 3) cairo_set_source_rgba(cr, 1.0, 0.85, 0.4, 1);
            else cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
            cairo_arc(cr, x, y, f * 0.22, 0, 2 * M_PI);
            cairo_fill(cr);
            /* Halo text */
            cairo_move_to(cr, x + f * 0.45, y + f * 0.35);
            cairo_text_path(cr, p->name);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.75);
            cairo_set_line_width(cr, f * 0.22);
            cairo_stroke_preserve(cr);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
            cairo_fill(cr);
        }
    }
}

int pic_localmap_drawing(void)
{
    return lm_lines && localmap_def.layer_on && pic_zoom_factor() >= 2.5;
}

static void localmap_reload(pic_feature_t *f)
{
    (void)f;
    lm_labels = pic_conf_int("LOCAL_TOWNS", 1);
}

static pic_feature_t localmap_def = {
    .name = "localmap", .layer = pic_layer_render_localmap, .interval = 0,
    .extra_mb = 6, .reload = localmap_reload, .default_on = 1,
};

pic_feature_t *pic_feature_localmap(void)
{
    return &localmap_def;
}

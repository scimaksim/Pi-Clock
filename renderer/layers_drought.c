/*
 * layers_drought.c - US Drought Monitor
 *
 * The weekly US Drought Monitor map (released Thursdays): D0 abnormally
 * dry (yellow) through D4 exceptional drought (dark red), drawn as
 * translucent areas, with the category at home noted at the bottom.
 *
 * The current GeoJSON is ~17 MB, too big to tokenise on a Pi, so it is
 * scanned as a stream: each polygon's outer ring is read straight from
 * the text and thinned to ~2 km spacing.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "geopoly.h"
#include "config.h"
#include "fetch.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define USDM_URL "https://droughtmonitor.unl.edu/data/json/usdm_current.json"
#define USDM_TMP "/tmp/pi-clock-usdm.json"
#define MAX_RINGS 4000
#define MAX_VERT  120000
#define MIN_STEP  0.02      /* degrees between kept vertices */

static const unsigned dm_rgb[5] = { 0xFFFF00, 0xFCD37F, 0xFFAA00, 0xE60000, 0x730000 };

static struct {
    pthread_mutex_t mutex;
    pic_geopoly_t   poly;
    int             ok;
} dr;

/*
 * Stream-parse USDM GeoJSON. Features arrive in DM order; each has a
 * MultiPolygon geometry followed by properties with "DM". Rings are
 * added with group = DM + 1 once that feature's DM is seen.
 */
int pic_drought_parse(const char *js, pic_geopoly_t *out)
{
    static float la[40000], lo[40000];
    const char *p = js;
    int added = 0;

    pic_geopoly_clear(out);
    while ((p = strstr(p, "\"coordinates\"")) != NULL) {
        const char *q = strchr(p, '[');
        int depth = 0, first_ring_of_poly = 0, n = 0, start_ring = out->nrings;
        const char *dm;
        int cat, r;

        if (!q) break;
        /* Walk the nested arrays: depth 1 = MultiPolygon, 2 = polygon,
         * 3 = ring, 4 = point. Keep ring 0 of each polygon. */
        for (; *q; q++) {
            if (*q == '[') {
                depth++;
                if (depth == 2) first_ring_of_poly = 1;
                if (depth == 3) n = 0;
                if (depth == 4) {
                    char *e;
                    double x = strtod(q + 1, &e), y;
                    if (*e != ',') continue;
                    y = strtod(e + 1, &e);
                    if (first_ring_of_poly && n < 40000) {
                        if (n == 0 || fabs(x - lo[n - 1]) + fabs(y - la[n - 1]) >= MIN_STEP) {
                            lo[n] = (float)x;
                            la[n] = (float)y;
                            n++;
                        }
                    }
                }
            } else if (*q == ']') {
                if (depth == 3) {
                    if (first_ring_of_poly && n >= 4) {
                        pic_ring_t st;
                        memset(&st, 0, sizeof(st));
                        if (pic_geopoly_add_ring(out, la, lo, n, &st) == 0) added++;
                    }
                    first_ring_of_poly = 0;
                }
                depth--;
                if (depth == 0) break;
            }
        }
        if (!*q) break;
        /* Category for the rings just added */
        dm = strstr(q, "\"DM\"");
        cat = dm ? atoi(strchr(dm, ':') + 1) : 0;
        if (cat < 0) cat = 0;
        if (cat > 4) cat = 4;
        for (r = start_ring; r < out->nrings; r++) {
            out->rings[r].group = cat + 1;
            out->rings[r].fill = dm_rgb[cat];
            out->rings[r].fill_a = 0.35f;
            out->rings[r].stroke = dm_rgb[cat];
            out->rings[r].stroke_a = 0.6f;
        }
        p = q;
    }
    return added;
}

static int drought_work(pic_feature_t *f)
{
    static pic_geopoly_t tmp;
    FILE *fp;
    long size;
    char *buf;
    int n = -1;

    (void)f;
    if (!tmp.rings && pic_geopoly_init(&tmp, MAX_RINGS, MAX_VERT) != 0) return 3600;
    if (pic_fetch_to_file(USDM_URL, USDM_TMP, 120) != 0) {
        unlink(USDM_TMP);
        printf("drought: fetch failed\n");
        return 1800;
    }
    fp = fopen(USDM_TMP, "rb");
    if (!fp) return 1800;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    rewind(fp);
    buf = size > 0 && size < 64L * 1024 * 1024 ? malloc((size_t)size + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)size, fp) == (size_t)size) {
        buf[size] = '\0';
        n = pic_drought_parse(buf, &tmp);
    }
    fclose(fp);
    free(buf);
    unlink(USDM_TMP);
    if (n <= 0) return 1800;
    pthread_mutex_lock(&dr.mutex);
    pic_geopoly_copy(&dr.poly, &tmp);
    dr.ok = 1;
    pthread_mutex_unlock(&dr.mutex);
    printf("drought: %d areas, %d vertices\n", n, tmp.nvert);
    return 12 * 3600;   /* weekly map; check twice a day */
}

void pic_layer_render_drought(cairo_t *cr, int width, int height,
                              time_t now, void *ud)
{
    (void)now; (void)ud;
    pthread_mutex_lock(&dr.mutex);
    pic_geopoly_draw(cr, &dr.poly, width, height, 1, height / 900.0);
    pthread_mutex_unlock(&dr.mutex);
}

void pic_layer_render_drought_note(cairo_t *cr, int width, int height,
                                   time_t now, void *ud)
{
    static const char *names[6] = { "no drought", "D0 abnormally dry",
        "D1 moderate drought", "D2 severe drought", "D3 extreme drought",
        "D4 exceptional drought" };
    char t[96];
    int g;
    (void)now; (void)ud;
    pthread_mutex_lock(&dr.mutex);
    if (!dr.ok) {
        pthread_mutex_unlock(&dr.mutex);
        return;
    }
    g = pic_geopoly_max_group_at(&dr.poly, pic_config.qth_lat, pic_config.qth_lon);
    pthread_mutex_unlock(&dr.mutex);
    if (g < 0) g = 0;
    snprintf(t, sizeof(t), "Drought Monitor \xc2\xb7 home: %s", names[g > 5 ? 5 : g]);
    pic_draw_legend_line(cr, width, height, 6, t, 1.0, 0.9, 0.6);
}

static pic_feature_t def = {
    .name = "drought", .layer = pic_layer_render_drought,
    .legend = pic_layer_render_drought_note, .interval = 600,
    .extra_mb = 4, .work = drought_work, .needs_network = 1,
};

pic_feature_t *pic_feature_drought(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&dr.mutex, NULL);
        pic_geopoly_init(&dr.poly, MAX_RINGS, MAX_VERT);
        inited = 1;
    }
    return &def;
}

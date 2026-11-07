/*
 * layers_spc.c - NOAA Storm Prediction Center convective outlook
 *
 * Draws the SPC categorical outlook (general thunder → high risk) as
 * translucent polygons in SPC's own colours, with a small legend
 * naming the risk at the home QTH. Day 1 by default; SPC_DAY=2|3
 * shows the later outlooks.
 *
 * Data: https://www.spc.noaa.gov/products/outlook/
 *   day{1,2,3}otlk_cat.nolyr.geojson - FeatureCollection, one
 *   MultiPolygon per category with properties DN (1-8 ordering),
 *   LABEL (TSTM/MRGL/SLGT/ENH/MDT/HIGH), LABEL2, fill, stroke,
 *   VALID/EXPIRE (YYYYMMDDHHMM UTC).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE  /* timegm */
#include "feature.h"
#include "geopoly.h"
#include "config.h"
#include "fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPC_MAX_RESPONSE (2 * 1024 * 1024)
#define SPC_MAX_RINGS    256
#define SPC_MAX_VERT     30000

typedef struct {
    pthread_mutex_t mutex;
    pic_geopoly_t   poly;
    char            labels[10][32];  /* LABEL2 by DN                    */
    time_t          valid, expire;
    int             day;
    int             min_dn;          /* SPC_MIN_RISK: hide below this  */
    int             ok;
} spc_data_t;

static spc_data_t spc;

static time_t parse_spc_time(const char *s)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (sscanf(s, "%4d%2d%2d%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min) != 5)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return timegm(&tm);
}

/* Exposed for the test harness. Returns rings parsed or -1. */
int pic_spc_parse(const char *js, size_t len, pic_geopoly_t *out,
                  char labels[10][32], time_t *valid, time_t *expire)
{
    pic_json_t j;
    int feats, i, n, total = 0;

    if (pic_json_parse(&j, js, len) < 0) return -1;
    pic_geopoly_clear(out);
    feats = pic_json_get(&j, 0, "features");
    n = pic_json_size(&j, feats);
    for (i = 0; i < n; i++) {
        int f = pic_json_at(&j, feats, i);
        int props = pic_json_get(&j, f, "properties");
        pic_ring_t st;
        char buf[64];
        int dn = (int)pic_json_num(&j, pic_json_get(&j, props, "DN"), 0);

        memset(&st, 0, sizeof(st));
        st.group = dn;
        pic_json_str(&j, pic_json_get(&j, props, "fill"), buf, sizeof(buf));
        st.fill = pic_parse_hex_rgb(buf, 0x888888);
        pic_json_str(&j, pic_json_get(&j, props, "stroke"), buf, sizeof(buf));
        st.stroke = pic_parse_hex_rgb(buf, 0x444444);
        st.fill_a = 0.38f;
        st.stroke_a = 0.95f;
        if (dn >= 0 && dn < 10 && labels)
            pic_json_str(&j, pic_json_get(&j, props, "LABEL2"),
                         labels[dn], 32);
        if (valid && pic_json_str(&j, pic_json_get(&j, props, "VALID"), buf, sizeof(buf)) > 0)
            *valid = parse_spc_time(buf);
        if (expire && pic_json_str(&j, pic_json_get(&j, props, "EXPIRE"), buf, sizeof(buf)) > 0)
            *expire = parse_spc_time(buf);
        total += pic_geopoly_add_geojson(out, &j, pic_json_get(&j, f, "geometry"), &st);
    }
    pic_json_free(&j);
    return total;
}

static int spc_work(pic_feature_t *f)
{
    static pic_geopoly_t tmp;
    static char labels[10][32];
    char url[128];
    char *buf;
    time_t valid = 0, expire = 0;
    int n, rings, day;

    (void)f;
    if (!tmp.rings && pic_geopoly_init(&tmp, SPC_MAX_RINGS, SPC_MAX_VERT) != 0)
        return 600;
    buf = malloc(SPC_MAX_RESPONSE);
    if (!buf) return 600;

    pthread_mutex_lock(&spc.mutex);
    day = spc.day;
    pthread_mutex_unlock(&spc.mutex);
    snprintf(url, sizeof(url),
             "https://www.spc.noaa.gov/products/outlook/day%dotlk_cat.nolyr.geojson",
             day);

    n = pic_fetch_to_buf(url, buf, SPC_MAX_RESPONSE - 1);
    if (n <= 0) {
        free(buf);
        printf("spc: fetch failed\n");
        return 300;
    }
    buf[n] = '\0';
    memset(labels, 0, sizeof(labels));
    rings = pic_spc_parse(buf, (size_t)n, &tmp, labels, &valid, &expire);
    free(buf);
    if (rings < 0) {
        printf("spc: parse failed\n");
        return 600;
    }

    pthread_mutex_lock(&spc.mutex);
    if (spc.day == day) {
        pic_geopoly_copy(&spc.poly, &tmp);
        memcpy(spc.labels, labels, sizeof(labels));
        spc.valid = valid;
        spc.expire = expire;
        spc.ok = 1;
    }
    pthread_mutex_unlock(&spc.mutex);
    printf("spc: day %d outlook, %d polygons\n", day, rings);
    return 30 * 60;  /* SPC issues several updates a day */
}

static void spc_reload(pic_feature_t *f)
{
    int day = pic_conf_int("SPC_DAY", 1);
    int minr = pic_conf_int("SPC_MIN_RISK", 2);
    if (day < 1 || day > 3) day = 1;
    pthread_mutex_lock(&spc.mutex);
    if (day != spc.day) {
        pic_geopoly_clear(&spc.poly);
        spc.ok = 0;
        spc.day = day;
        pic_feature_poke(f);
    }
    spc.min_dn = minr;
    pthread_mutex_unlock(&spc.mutex);
}

void pic_layer_render_spc(cairo_t *cr, int width, int height,
                          time_t now, void *ud)
{
    (void)now; (void)ud;
    pthread_mutex_lock(&spc.mutex);
    pic_geopoly_draw(cr, &spc.poly, width, height, spc.min_dn,
                     height / 600.0);
    pthread_mutex_unlock(&spc.mutex);
}

/* Legend: "SPC Day 1 outlook · home: Marginal Risk", bottom centre */
void pic_layer_render_spc_legend(cairo_t *cr, int width, int height,
                                 time_t now, void *ud)
{
    char text[128], at[48];
    int g;

    (void)ud;
    pthread_mutex_lock(&spc.mutex);
    if (!spc.ok || (spc.expire && now > spc.expire + 3600)) {
        pthread_mutex_unlock(&spc.mutex);
        return;
    }
    g = pic_geopoly_max_group_at(&spc.poly, pic_config.qth_lat,
                                 pic_config.qth_lon);
    if (g >= 2 && g < 10 && spc.labels[g][0])
        snprintf(at, sizeof(at), "%s", spc.labels[g]);
    else
        snprintf(at, sizeof(at), "no thunder risk");
    snprintf(text, sizeof(text), "SPC Day %d outlook \xc2\xb7 home: %s",
             spc.day, at);
    pthread_mutex_unlock(&spc.mutex);

    pic_draw_legend_line(cr, width, height, 0, text,
                         g >= 4 ? 1.0 : 0.85, g >= 4 ? 0.75 : 0.95,
                         g >= 4 ? 0.4 : 0.85);
}

static pic_feature_t feature_spc_def = {
    .name = "spc", .layer = pic_layer_render_spc,
    .legend = pic_layer_render_spc_legend, .interval = 60, .extra_mb = 3,
    .data = &spc, .reload = spc_reload, .work = spc_work,
    .needs_network = 1,
};

pic_feature_t *pic_feature_spc(void)
{
    if (!spc.poly.rings) {
        pthread_mutex_init(&spc.mutex, NULL);
        pic_geopoly_init(&spc.poly, SPC_MAX_RINGS, SPC_MAX_VERT);
        spc.day = 1;
        spc.min_dn = 2;
    }
    return &feature_spc_def;
}

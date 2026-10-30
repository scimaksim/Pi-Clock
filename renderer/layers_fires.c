/*
 * layers_fires.c - Active fire hotspots and smoke plumes
 *
 * Hotspots: NASA FIRMS VIIRS 375 m detections from the last 24 hours
 * (NOAA-20 and Suomi NPP) over the contiguous US, Hawaii and nearby
 * Mexico/Canada, drawn as glowing dots sized by fire radiative power.
 * The no-key FIRMS feeds are regional; this uses the US one.
 *
 * Smoke: NOAA Hazard Mapping System analyst-drawn smoke polygons
 * (light / medium / heavy) for today (UTC), or yesterday's until
 * today's first analysis is published. Each density class is drawn
 * as one union so overlapping analyses don't stack.
 *
 * Settings: FIRE_SMOKE=0|1 (default 1), FIRE_MIN_FRP=MW (default 0).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE  /* memmem */
#include "feature.h"
#include "geopoly.h"
#include "config.h"
#include "fetch.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIRMS_BASE "https://firms.modaps.eosdis.nasa.gov/data/active_fire/"
static const char *firms_urls[] = {
    FIRMS_BASE "noaa-20-viirs-c2/csv/J1_VIIRS_C2_USA_contiguous_and_Hawaii_24h.csv",
    FIRMS_BASE "suomi-npp-viirs-c2/csv/SUOMI_VIIRS_C2_USA_contiguous_and_Hawaii_24h.csv",
};
#define HMS_FMT "https://satepsanone.nesdis.noaa.gov/pub/FIRE/web/HMS/" \
                "Smoke_Polygons/KML/%04d/%02d/hms_smoke%04d%02d%02d.kml"

#define MAX_RESPONSE (3 * 1024 * 1024)
#define MAX_FIRES    12000
#define SMOKE_RINGS  1500
#define SMOKE_VERT   120000

typedef struct { float lat, lon, frp; } fire_t;

typedef struct {
    pthread_mutex_t mutex;
    fire_t       *fires;
    int           nfires;
    pic_geopoly_t smoke;
    int           show_smoke;
    double        min_frp;
    time_t        fetched;
} fires_t;

static fires_t fd;

/* Parse a FIRMS CSV (columns located by header). Appends to out. */
int pic_fires_parse_csv(const char *csv, fire_t *out, int n, int max)
{
    int c_lat = -1, c_lon = -1, c_frp = -1, col = 0;
    const char *p = csv, *eol;

    eol = strchr(p, '\n');
    if (!eol) return n;
    while (p < eol) {
        size_t len = strcspn(p, ",\r\n");
        if (len == 8 && !strncmp(p, "latitude", 8)) c_lat = col;
        else if (len == 9 && !strncmp(p, "longitude", 9)) c_lon = col;
        else if (len == 3 && !strncmp(p, "frp", 3)) c_frp = col;
        p += len;
        if (*p == ',') p++;
        else break;
        col++;
    }
    if (c_lat < 0 || c_lon < 0) return n;

    for (p = eol + 1; *p && n < max; ) {
        double lat = 999, lon = 999, frp = 0;
        col = 0;
        eol = strchr(p, '\n');
        if (!eol) eol = p + strlen(p);
        while (p < eol) {
            if (col == c_lat) lat = atof(p);
            else if (col == c_lon) lon = atof(p);
            else if (col == c_frp) frp = atof(p);
            p += strcspn(p, ",\n");
            if (*p == ',') p++;
            col++;
        }
        if (fabs(lat) <= 90 && fabs(lon) <= 180) {
            out[n].lat = (float)lat;
            out[n].lon = (float)lon;
            out[n].frp = (float)frp;
            n++;
        }
        p = *eol ? eol + 1 : eol;
    }
    return n;
}

/* Parse HMS smoke KML placemarks into rings (group 1..3 = density). */
int pic_fires_parse_kml(const char *kml, pic_geopoly_t *out)
{
    static float lat[8192], lon[8192];
    const char *p = kml;
    int rings = 0;

    while ((p = strstr(p, "<Placemark")) != NULL) {
        const char *end = strstr(p, "</Placemark>");
        const char *c, *ce;
        pic_ring_t st;
        int n = 0;

        if (!end) break;
        memset(&st, 0, sizeof(st));
        st.group = memmem(p, (size_t)(end - p), "Smoke_Heavy", 11) ? 3 :
                   memmem(p, (size_t)(end - p), "Smoke_Medium", 12) ? 2 : 1;
        c = memmem(p, (size_t)(end - p), "<coordinates>", 13);
        ce = c ? memmem(c, (size_t)(end - c), "</coordinates>", 14) : NULL;
        if (c && ce) {
            char *q;
            c += 13;
            while (c < ce && n < 8192) {
                double lo = strtod(c, &q), la;
                if (q == c || *q != ',') break;
                la = strtod(q + 1, &q);
                lat[n] = (float)la;
                lon[n] = (float)lo;
                n++;
                /* Skip the altitude and whitespace */
                c = q;
                while (c < ce && *c != ' ' && *c != '\n' && *c != '\t' && *c != '\r') c++;
                while (c < ce && (*c == ' ' || *c == '\n' || *c == '\t' || *c == '\r')) c++;
            }
            if (pic_geopoly_add_ring(out, lat, lon, n, &st) == 0) rings++;
        }
        p = end;
    }
    return rings;
}

static int fires_work(pic_feature_t *f)
{
    static pic_geopoly_t smoke;
    static fire_t *tmp;
    char *buf, url[256];
    int n = 0, i, got = 0, rings = -1, smoke_on;
    time_t now = time(NULL);

    (void)f;
    if (!tmp) tmp = malloc(MAX_FIRES * sizeof(fire_t));
    if (!smoke.rings) pic_geopoly_init(&smoke, SMOKE_RINGS, SMOKE_VERT);
    buf = malloc(MAX_RESPONSE);
    if (!tmp || !buf || !smoke.rings) {
        free(buf);
        return 600;
    }

    for (i = 0; i < 2; i++) {
        int len = pic_fetch_to_buf(firms_urls[i], buf, MAX_RESPONSE - 1);
        if (len <= 0) continue;
        buf[len] = '\0';
        n = pic_fires_parse_csv(buf, tmp, n, MAX_FIRES);
        got++;
    }

    pthread_mutex_lock(&fd.mutex);
    smoke_on = fd.show_smoke;
    pthread_mutex_unlock(&fd.mutex);

    if (smoke_on) {
        int back;
        for (back = 0; back <= 1 && rings <= 0; back++) {
            time_t t = now - back * 86400;
            struct tm tm;
            int len;
            gmtime_r(&t, &tm);
            snprintf(url, sizeof(url), HMS_FMT, tm.tm_year + 1900,
                     tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_mon + 1,
                     tm.tm_mday);
            len = pic_fetch_to_buf(url, buf, MAX_RESPONSE - 1);
            if (len <= 0) continue;
            buf[len] = '\0';
            pic_geopoly_clear(&smoke);
            rings = pic_fires_parse_kml(buf, &smoke);
        }
    }
    free(buf);

    pthread_mutex_lock(&fd.mutex);
    if (got) {
        memcpy(fd.fires, tmp, (size_t)n * sizeof(fire_t));
        fd.nfires = n;
        fd.fetched = now;
    }
    if (rings >= 0) pic_geopoly_copy(&fd.smoke, &smoke);
    else if (!smoke_on) pic_geopoly_clear(&fd.smoke);
    pthread_mutex_unlock(&fd.mutex);

    printf("fires: %d hotspots, %d smoke polygons\n", n, rings);
    return got ? 30 * 60 : 5 * 60;
}

static void fires_reload(pic_feature_t *f)
{
    int smoke = pic_conf_int("FIRE_SMOKE", 1) != 0;
    pthread_mutex_lock(&fd.mutex);
    if (smoke && !fd.show_smoke) pic_feature_poke(f);
    fd.show_smoke = smoke;
    fd.min_frp = pic_conf_num("FIRE_MIN_FRP", 0);
    pthread_mutex_unlock(&fd.mutex);
}

void pic_layer_render_fires(cairo_t *cr, int width, int height,
                            time_t now, void *ud)
{
    double base = height / 600.0, z = 1.0 + 0.4 * (pic_zoom_factor() - 1.0);
    int i, cls;

    (void)now; (void)ud;
    pthread_mutex_lock(&fd.mutex);

    /* Smoke: one union per density, light first */
    if (fd.show_smoke && fd.smoke.nrings) {
        static const double alpha[4] = { 0, 0.22, 0.34, 0.48 };
        for (cls = 1; cls <= 3; cls++) {
            int any = 0, r;
            for (r = 0; r < fd.smoke.nrings; r++) {
                pic_ring_t *ring = &fd.smoke.rings[r];
                ring->fill = 0x8a7f72;
                ring->fill_a = ring->group == cls ? 1.0f : 0.0f;
                ring->stroke_a = 0;
                any |= ring->group == cls;
            }
            if (!any) continue;
            cairo_push_group(cr);
            pic_geopoly_draw(cr, &fd.smoke, width, height, cls, 0);
            cairo_pop_group_to_source(cr);
            cairo_paint_with_alpha(cr, alpha[cls]);
        }
    }

    /* Hotspots */
    for (i = 0; i < fd.nfires; i++) {
        const fire_t *h = &fd.fires[i];
        double x, y, r;
        if (h->frp < fd.min_frp) continue;
        x = pic_lon_to_x(h->lon, width);
        y = pic_lat_to_y(h->lat, height);
        if (x < -10 || x > width + 10 || y < -10 || y > height + 10) continue;
        /* Small at world scale: most detections are farm and brush
         * burns of a few MW; big fires get a glow. */
        r = base * z * (0.7 + sqrt(h->frp > 0 ? h->frp : 0) * 0.12);
        if (r > base * 3.5 * z) r = base * 3.5 * z;
        if (h->frp >= 50) {
            cairo_set_source_rgba(cr, 1.0, 0.45, 0.05, 0.3);
            cairo_arc(cr, x, y, r * 2.0, 0, 2 * M_PI);
            cairo_fill(cr);
        }
        cairo_set_source_rgba(cr, 1.0, h->frp >= 50 ? 0.95 : 0.55,
                              h->frp >= 50 ? 0.4 : 0.1, 0.95);
        cairo_arc(cr, x, y, r, 0, 2 * M_PI);
        cairo_fill(cr);
    }
    pthread_mutex_unlock(&fd.mutex);
}

static pic_feature_t feature_def = {
    .name = "fires", .layer = pic_layer_render_fires, .interval = 60,
    .extra_mb = 3, .reload = fires_reload, .work = fires_work,
    .needs_network = 1,
};

pic_feature_t *pic_feature_fires(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&fd.mutex, NULL);
        fd.fires = calloc(MAX_FIRES, sizeof(fire_t));
        pic_geopoly_init(&fd.smoke, SMOKE_RINGS, SMOKE_VERT);
        fd.show_smoke = 1;
        inited = 1;
    }
    return &feature_def;
}

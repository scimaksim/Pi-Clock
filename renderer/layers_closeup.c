/*
 * layers_closeup.c - The close-up view of home (see closeup.h)
 *
 * closeup    Base map for the close-up: the USGS aerial photo (public
 *            domain, The National Map) for exactly the view, with lakes
 *            and named creeks (USGS NHD), main roads and town names
 *            (Census TIGER) drawn over it. Fetched when the view is first
 *            needed and again every 30 days; dimmed by the Sun's height.
 *
 * watershed  Live, from the Pi-Dash server on the PC (PIDASH_URL), which
 *            fetches each source once for the house and holds the keys:
 *              /api/watershed  USGS creek gauges against normal for the
 *                              date, and the NC State Climate Office
 *                              ECONet station at the Lake Wheeler Rd
 *                              field lab
 *              /api/ncorbit    NASA FIRMS fire detections in NC
 *            The clock itself only asks USGS once for where the gauges
 *            are. Icons move: water runs along the creeks (faster with
 *            more flow), gauges send out ripples, the station's cups
 *            turn with the wind, rain falls while it rains, fires glow.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "closeup.h"
#include "feature.h"
#include "config.h"
#include "fetch.h"
#include "image.h"
#include "json.h"
#include "astro.h"
#include "labels.h"
#include "localview.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ================================================================ parsers */

static double jnum(const pic_json_t *j, int o, const char *k)
{
    return pic_json_num(j, pic_json_get(j, o, k), NAN);
}

int pic_cu_parse_watershed(const char *js, size_t len, cu_wsh_t *out)
{
    pic_json_t j;
    int sites, st, i, e;
    memset(out, 0, sizeof(*out));
    out->st.air = out->st.soil = out->st.moist = out->st.rh = out->st.wind = out->st.sr = NAN;
    if (!js || pic_json_parse_max(&j, js, len, 200000) != 0) return -1;
    if (!pic_json_is_object(&j, 0)) { pic_json_free(&j); return -1; }
    sites = pic_json_get(&j, 0, "sites");
    if (!pic_json_is_array(&j, sites)) { pic_json_free(&j); return -1; }
    for (i = 0, e = sites + 1; i < pic_json_size(&j, sites) && e > 0; i++, e = pic_json_next(&j, e)) {
        cu_site_t *s;
        int c;
        if (out->n >= CU_MAX_SITES) break;
        s = &out->s[out->n];
        memset(s, 0, sizeof(*s));
        pic_json_str(&j, pic_json_get(&j, e, "site"), s->site, sizeof(s->site));
        pic_json_str(&j, pic_json_get(&j, e, "role"), s->role, sizeof(s->role));
        pic_json_str(&j, pic_json_get(&j, e, "name"), s->name, sizeof(s->name));
        if (!s->site[0]) continue;
        s->flow = jnum(&j, e, "flow");
        s->normal = jnum(&j, e, "normal");
        {
            double t = jnum(&j, e, "t");
            s->t = isnan(t) ? 0 : (time_t)t;
        }
        c = pic_json_get(&j, e, "cls");
        if (pic_json_is_object(&j, c)) {
            pic_json_str(&j, pic_json_get(&j, c, "k"), s->cls, sizeof(s->cls));
            pic_json_str(&j, pic_json_get(&j, c, "txt"), s->cls_txt, sizeof(s->cls_txt));
        }
        out->n++;
    }
    st = pic_json_get(&j, 0, "station");
    if (pic_json_is_object(&j, st)) {
        int L = pic_json_get(&j, st, "latest");
        static const char *const var[6] = { "airtemp", "soiltemp", "soilmoist", "rh", "windspeed", "sr" };
        double *dst[6] = { &out->st.air, &out->st.soil, &out->st.moist, &out->st.rh, &out->st.wind, &out->st.sr };
        out->st.ok = 1;
        for (i = 0; i < 6; i++) {
            int c = pic_json_get(&j, L, var[i]);
            *dst[i] = jnum(&j, c, "v");
            if (!out->st.at[0]) pic_json_str(&j, pic_json_get(&j, c, "t"), out->st.at, sizeof(out->st.at));
        }
        out->st.rain_2h = jnum(&j, st, "rain_2h");
        out->st.rain_24h = jnum(&j, st, "rain_24h");
        out->st.rain_7d = jnum(&j, st, "rain_7d");
        out->st.rain_30d = jnum(&j, st, "rain_30d");
        out->st.lat = jnum(&j, st, "lat");
        out->st.lon = jnum(&j, st, "lon");
        out->st.has_loc = !isnan(out->st.lat) && !isnan(out->st.lon) &&
                          fabs(out->st.lat) <= 90 && fabs(out->st.lon) <= 180;
    }
    pic_json_str(&j, pic_json_get(&j, 0, "station_error"), out->st.err, sizeof(out->st.err));
    pic_json_free(&j);
    return out->n;
}

int pic_cu_parse_fires(const char *js, size_t len, cu_fire_t *out, int max, int *hours)
{
    pic_json_t j;
    int arr, i, e, n = 0;
    if (hours) *hours = 0;
    if (!js || pic_json_parse_max(&j, js, len, 400000) != 0) return -1;
    if (!pic_json_is_object(&j, 0)) { pic_json_free(&j); return -1; }
    if (hours) {
        double h = jnum(&j, 0, "fire_hours");
        *hours = isnan(h) ? 48 : (int)h;
    }
    arr = pic_json_get(&j, 0, "fires");
    if (!pic_json_is_array(&j, arr)) { pic_json_free(&j); return 0; }   /* no fire data */
    for (i = 0, e = arr + 1; i < pic_json_size(&j, arr) && e > 0 && n < max; i++, e = pic_json_next(&j, e)) {
        double la = jnum(&j, e, "lat"), lo = jnum(&j, e, "lon"), t = jnum(&j, e, "t"), frp = jnum(&j, e, "frp");
        if (isnan(la) || isnan(lo) || fabs(la) > 90 || fabs(lo) > 180) continue;
        out[n].lat = (float)la;
        out[n].lon = (float)lo;
        out[n].t = isnan(t) ? 0 : (time_t)t;
        out[n].frp = isnan(frp) ? 0 : (float)frp;
        out[n].night = pic_json_bool(&j, pic_json_get(&j, e, "night"), 0);
        n++;
    }
    pic_json_free(&j);
    return n;
}

int pic_cu_parse_usgs_sites(const char *rdb, cu_site_t *s, int n)
{
    const char *p = rdb;
    int col_no = -1, col_la = -1, col_lo = -1, header = 1, found = 0;
    while (p && *p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[1024], *f[32], *q;
        int nf = 0, k;
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        p = e ? e + 1 : NULL;
        if (line[0] == '#' || !line[0]) continue;
        for (q = line, f[nf++] = q; *q && nf < 32; q++)
            if (*q == '\t') { *q = '\0'; f[nf++] = q + 1; }
        for (k = 0; k < nf; k++) {     /* trim a trailing \r */
            size_t l = strlen(f[k]);
            if (l && f[k][l - 1] == '\r') f[k][l - 1] = '\0';
        }
        if (header == 1) {             /* column names */
            for (k = 0; k < nf; k++) {
                if (!strcmp(f[k], "site_no")) col_no = k;
                else if (!strcmp(f[k], "dec_lat_va")) col_la = k;
                else if (!strcmp(f[k], "dec_long_va")) col_lo = k;
            }
            header = 2;
            continue;
        }
        if (header == 2) { header = 0; continue; }   /* column formats */
        if (col_no < 0 || col_la < 0 || col_lo < 0 || col_no >= nf || col_la >= nf || col_lo >= nf) break;
        for (k = 0; k < n; k++) {
            double la, lo;
            char *end1, *end2;
            if (strcmp(s[k].site, f[col_no]) != 0) continue;
            la = strtod(f[col_la], &end1);
            lo = strtod(f[col_lo], &end2);
            if (end1 == f[col_la] || end2 == f[col_lo] || fabs(la) > 90 || fabs(lo) > 180) continue;
            if (!s[k].has_loc) found++;
            s[k].lat = la; s[k].lon = lo; s[k].has_loc = 1;
        }
    }
    return found;
}

/* Coordinates array at tok: [x,y] pairs (depth 1) handed to cb */
static void coords_line(const pic_json_t *j, int arr, const char *name, int ring,
                        cu_line_cb cb, void *ud)
{
    int n = pic_json_size(j, arr), i, e, m = 0;
    double *xy;
    if (n < 2) return;
    xy = malloc((size_t)n * 2 * sizeof(double));
    if (!xy) return;
    for (i = 0, e = arr + 1; i < n && e > 0; i++, e = pic_json_next(j, e)) {
        if (!pic_json_is_array(j, e) || pic_json_size(j, e) < 2) continue;
        xy[2 * m] = pic_json_num(j, e + 1, NAN);
        xy[2 * m + 1] = pic_json_num(j, e + 2, NAN);
        if (!isnan(xy[2 * m]) && !isnan(xy[2 * m + 1])) m++;
    }
    if (m >= 2) cb(name, xy, m, ring, ud);
    free(xy);
}

int pic_cu_geo_lines(const char *js, size_t len, const char *name_key, cu_line_cb cb, void *ud)
{
    pic_json_t j;
    int feats, i, e, n = 0;
    if (!js || pic_json_parse_max(&j, js, len, 3000000) != 0) return -1;
    feats = pic_json_get(&j, 0, "features");
    if (!pic_json_is_array(&j, feats)) { pic_json_free(&j); return -1; }
    for (i = 0, e = feats + 1; i < pic_json_size(&j, feats) && e > 0; i++, e = pic_json_next(&j, e)) {
        int g = pic_json_get(&j, e, "geometry"), c = pic_json_get(&j, g, "coordinates"), a, b, k, kk;
        char name[64] = "", type[24] = "";
        if (name_key) {   /* USGS layers differ in the key's case: gnis_name, GNIS_NAME */
            int pr = pic_json_get(&j, e, "properties"), t = pic_json_get(&j, pr, name_key);
            if (t < 0) {
                char up[32];
                size_t q;
                for (q = 0; name_key[q] && q < sizeof(up) - 1; q++)
                    up[q] = (char)(name_key[q] >= 'a' && name_key[q] <= 'z' ? name_key[q] - 32 : name_key[q]);
                up[q] = '\0';
                t = pic_json_get(&j, pr, up);
            }
            pic_json_str(&j, t, name, sizeof(name));
        }
        if (!strcmp(name, "null")) name[0] = '\0';
        pic_json_str(&j, pic_json_get(&j, g, "type"), type, sizeof(type));
        if (!pic_json_is_array(&j, c)) continue;
        n++;
        if (!strcmp(type, "LineString")) {
            coords_line(&j, c, name, 0, cb, ud);
        } else if (!strcmp(type, "MultiLineString") || !strcmp(type, "Polygon")) {
            int ring = type[0] == 'P';
            for (k = 0, a = c + 1; k < pic_json_size(&j, c) && a > 0; k++, a = pic_json_next(&j, a))
                coords_line(&j, a, name, ring, cb, ud);
        } else if (!strcmp(type, "MultiPolygon")) {
            for (k = 0, a = c + 1; k < pic_json_size(&j, c) && a > 0; k++, a = pic_json_next(&j, a))
                for (kk = 0, b = a + 1; kk < pic_json_size(&j, a) && b > 0; kk++, b = pic_json_next(&j, b))
                    coords_line(&j, b, name, 1, cb, ud);
        }
    }
    pic_json_free(&j);
    return n;
}

int pic_cu_places(const char *js, size_t len, cu_place_t *out, int max)
{
    pic_json_t j;
    int feats, i, e, n = 0;
    if (!js || pic_json_parse_max(&j, js, len, 400000) != 0) return -1;
    feats = pic_json_get(&j, 0, "features");
    if (!pic_json_is_array(&j, feats)) { pic_json_free(&j); return -1; }
    for (i = 0, e = feats + 1; i < pic_json_size(&j, feats) && e > 0 && n < max; i++, e = pic_json_next(&j, e)) {
        int p = pic_json_get(&j, e, "properties");
        cu_place_t *q = &out[n];
        memset(q, 0, sizeof(*q));
        pic_json_str(&j, pic_json_get(&j, p, "BASENAME"), q->name, sizeof(q->name));
        q->lat = jnum(&j, p, "CENTLAT");
        q->lon = jnum(&j, p, "CENTLON");
        q->area = jnum(&j, p, "AREALAND");
        if (!q->name[0] || isnan(q->lat) || isnan(q->lon)) continue;
        if (isnan(q->area)) q->area = 0;
        n++;
    }
    pic_json_free(&j);
    return n;
}

time_t pic_cu_local_time(const char *s)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (!s || sscanf(s, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                     &tm.tm_hour, &tm.tm_min, &tm.tm_sec) < 5)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

/* ================================================================ base map */

#define CU_DIR     "/data/cache/pi-clock-closeup"
#define CU_REFRESH (30 * 86400)
#define BW 1920
#define BH 1080

/* Main water courses, kept as lines for the flow animation */
#define CU_MAX_CREEK_PTS 20000
static const char *const CREEKS[] = { "Swift Creek", "Neuse River", NULL };

static struct {
    pthread_mutex_t m;
    cairo_surface_t *photo, *ink;    /* BW x BH, for ext                    */
    double ext[4];                   /* west, south, east, north            */
    float  *cx;                      /* creek points: lon, lat pairs        */
    int    *cstart, *cname, ncl, npt;   /* line starts and CREEKS index    */
    double lbox[128][4];             /* base labels, in BW x BH pixels      */
    int    nlbox;
    char   status[96];
} cb;

static void view_ext(double ext[4])
{
    double v[4];
    pic_config_viewport_for(pic_config.closeup_zoom, v);
    ext[0] = v[2] - v[0] / 2; ext[2] = v[2] + v[0] / 2;
    ext[1] = v[3] - v[1] / 2; ext[3] = v[3] + v[1] / 2;
}

static int same_ext(const double a[4], const double b[4])
{
    int k;
    for (k = 0; k < 4; k++)
        if (fabs(a[k] - b[k]) > 1e-6) return 0;
    return 1;
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 40L * 1024 * 1024 || !(b = malloc((size_t)n + 1))) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    b[n] = '\0';
    fclose(f);
    if (len) *len = (size_t)n;
    return b;
}

/* The files for this extent, fetched if missing, old or for another extent */
#define CU_IMG_URL "https://basemap.nationalmap.gov/arcgis/rest/services/USGSImageryOnly/MapServer/export"
static const struct { const char *file, *url; } SRC[] = {
    { "img.jpg", CU_IMG_URL },
    { "water.json", "https://hydro.nationalmap.gov/arcgis/rest/services/nhd/MapServer/12/query?"
                    "where=areasqkm%3E0.03&outFields=gnis_name,areasqkm" },
    /* lake names: the detailed layer above leaves some big lakes unnamed */
    { "lakes.json", "https://hydro.nationalmap.gov/arcgis/rest/services/nhd/MapServer/10/query?"
                    "where=areasqkm%3E0.3&outFields=gnis_name" },
    { "flow.json", "https://hydro.nationalmap.gov/arcgis/rest/services/nhd/MapServer/6/query?"
                   "where=gnis_name%20IS%20NOT%20NULL&outFields=gnis_name" },
    { "road1.json", "https://tigerweb.geo.census.gov/arcgis/rest/services/TIGERweb/Transportation/MapServer/2/query?"
                    "where=1%3D1&outFields=NAME" },
    { "road2.json", "https://tigerweb.geo.census.gov/arcgis/rest/services/TIGERweb/Transportation/MapServer/3/query?"
                    "where=1%3D1&outFields=NAME" },
    { "road3.json", "https://tigerweb.geo.census.gov/arcgis/rest/services/TIGERweb/Transportation/MapServer/6/query?"
                    "where=1%3D1&outFields=NAME" },
    { "places.json", "https://tigerweb.geo.census.gov/arcgis/rest/services/TIGERweb/Places_CouSub_ConCity_SubMCD/MapServer/4/query?"
                     "where=1%3D1&outFields=BASENAME,CENTLAT,CENTLON,AREALAND&returnGeometry=false" },
};
#define NSRC ((int)(sizeof(SRC) / sizeof(SRC[0])))

static int fetch_sources(const double ext[4], int *changed)
{
    char key[96], have[96] = "", path[160], tmp[168], url[1024], bbox[80], q[400];
    FILE *f;
    int i, ok = 1;
    time_t now = time(NULL);
    struct stat st;

    *changed = 0;
    mkdir("/data/cache", 0755);
    mkdir(CU_DIR, 0755);
    snprintf(bbox, sizeof(bbox), "%.5f,%.5f,%.5f,%.5f", ext[0], ext[1], ext[2], ext[3]);
    snprintf(key, sizeof(key), "%s", bbox);
    if ((f = fopen(CU_DIR "/extent.txt", "r")) != NULL) {
        if (!fgets(have, sizeof(have), f)) have[0] = '\0';
        fclose(f);
        have[strcspn(have, "\n")] = '\0';
    }
    /* simplified to about a third of a screen pixel */
    snprintf(q, sizeof(q), "geometry=%s&geometryType=esriGeometryEnvelope&inSR=4326&outSR=4326"
             "&spatialRel=esriSpatialRelIntersects&geometryPrecision=5"
             "&maxAllowableOffset=%.6f&f=geojson", bbox, (ext[2] - ext[0]) / BW / 3);
    for (i = 0; i < NSRC; i++) {
        snprintf(path, sizeof(path), CU_DIR "/%s", SRC[i].file);
        if (!strcmp(have, key) && stat(path, &st) == 0 && st.st_size > 0 &&
            now - st.st_mtime < CU_REFRESH && now >= st.st_mtime)
            continue;
        snprintf(tmp, sizeof(tmp), "%s.new", path);
        if (i == 0)   /* the photo: as many rows as the degrees' aspect needs */
            snprintf(url, sizeof(url), "%s?bbox=%s&bboxSR=4326&imageSR=4326&size=%d,%d&format=jpg&f=image",
                     SRC[i].url, bbox, BW, (int)lround(BW * (ext[3] - ext[1]) / (ext[2] - ext[0])));
        else
            snprintf(url, sizeof(url), "%s&%s", SRC[i].url, q);
        if (pic_fetch_to_file(url, tmp, 120) == 0 && stat(tmp, &st) == 0 && st.st_size > 0) {
            char head[4] = "";
            FILE *h = fopen(tmp, "rb");
            if (h) { if (fread(head, 1, 3, h) != 3) head[0] = '\0'; fclose(h); }
            /* a JPEG starts FF D8; GeoJSON with "{" (an error page doesn't) */
            if ((i == 0 && (unsigned char)head[0] == 0xFF && (unsigned char)head[1] == 0xD8) ||
                (i > 0 && head[0] == '{')) {
                rename(tmp, path);
                *changed = 1;
                continue;
            }
        }
        unlink(tmp);
        printf("closeup: could not fetch %s\n", SRC[i].file);
        if (stat(path, &st) != 0 || strcmp(have, key) != 0) ok = 0;   /* nothing usable */
    }
    if (ok && strcmp(have, key) != 0 && (f = fopen(CU_DIR "/extent.txt", "w")) != NULL) {
        fprintf(f, "%s\n", key);
        fclose(f);
    }
    return ok;
}

/* Drawing state for the build */
typedef struct {
    cairo_t *cr;
    double ext[4];
    int kind;                 /* 0 water body, 1 creek, 2-4 roads */
    float *cx; int *cstart, *cname, ncl, npt;
} build_t;

static double bx(const build_t *b, double lon) { return (lon - b->ext[0]) / (b->ext[2] - b->ext[0]) * BW; }
static double by(const build_t *b, double lat) { return (b->ext[3] - lat) / (b->ext[3] - b->ext[1]) * BH; }

static void line_cb(const char *name, const double *xy, int n, int ring, void *ud)
{
    build_t *b = ud;
    cairo_t *cr = b->cr;
    int k, big = 0;
    for (k = 0; CREEKS[k]; k++)
        if (!strcmp(name, CREEKS[k])) big = 1;
    cairo_move_to(cr, bx(b, xy[0]), by(b, xy[1]));
    for (k = 1; k < n; k++) cairo_line_to(cr, bx(b, xy[2 * k]), by(b, xy[2 * k + 1]));
    if (b->kind == 0) {
        if (ring) cairo_close_path(cr);
        cairo_set_source_rgba(cr, 90 / 255., 170 / 255., 1, .47);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 140 / 255., 200 / 255., 1, .9);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
    } else if (b->kind == 1) {
        int major = big || !strcmp(name, "Middle Creek") || !strcmp(name, "Walnut Creek") ||
                    !strcmp(name, "Black Creek");
        cairo_set_source_rgba(cr, 120 / 255., 190 / 255., 1, major ? .9 : .6);
        cairo_set_line_width(cr, major ? 3.0 : 1.4);
        cairo_stroke(cr);
        /* keep the main courses for the animation */
        if (big && b->cx && b->npt + n <= CU_MAX_CREEK_PTS && b->ncl < 4000) {
            int ci = 0;
            for (k = 0; CREEKS[k]; k++) if (!strcmp(name, CREEKS[k])) ci = k;
            b->cstart[b->ncl] = b->npt;
            b->cname[b->ncl] = ci;
            b->ncl++;
            for (k = 0; k < n; k++) {
                b->cx[2 * b->npt] = (float)xy[2 * k];
                b->cx[2 * b->npt + 1] = (float)xy[2 * k + 1];
                b->npt++;
            }
        }
    } else {
        static const double col[3][4] = { { 1, .84, .47, .6 }, { 1, 1, 1, .35 }, { 1, 1, 1, .24 } };
        static const double wd[3] = { 4.2, 3.2, 2.2 };
        const double *c = col[b->kind - 2];
        cairo_set_source_rgba(cr, c[0], c[1], c[2], c[3]);
        cairo_set_line_width(cr, wd[b->kind - 2]);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_stroke(cr);
    }
}

/* Lake label positions: centroid of each named lake's biggest ring */
typedef struct { char name[48]; double sx, sy, area; } lake_t;
typedef struct { lake_t l[64]; int n; const double *ext; } lakes_t;

static void lake_cb(const char *name, const double *xy, int n, int ring, void *ud)
{
    lakes_t *L = ud;
    double a = 0, cx = 0, cy = 0;
    char nm[48];
    const char *c;
    int k, i;
    if (!name[0] || name[0] == ' ' || !ring || n < 3) return;
    /* gazetteer order "Johnson, Lake" -> "Lake Johnson" */
    c = strstr(name, ", ");
    if (c && !strchr(c + 2, ','))
        snprintf(nm, sizeof(nm), "%s %.*s", c + 2, (int)(c - name), name);
    else
        snprintf(nm, sizeof(nm), "%s", name);
    for (k = 0; k < n; k++) {     /* shoelace, in degrees */
        int m = (k + 1) % n;
        double cr_ = xy[2 * k] * xy[2 * m + 1] - xy[2 * m] * xy[2 * k + 1];
        a += cr_;
        cx += (xy[2 * k] + xy[2 * m]) * cr_;
        cy += (xy[2 * k + 1] + xy[2 * m + 1]) * cr_;
    }
    if (fabs(a) < 1e-9) return;
    cx /= 3 * a; cy /= 3 * a; a = fabs(a) / 2;
    if (a < 3e-5) return;             /* about 0.3 km2 at this latitude */
    for (i = 0; i < L->n; i++)
        if (!strcmp(L->l[i].name, nm)) {
            if (a > L->l[i].area) { L->l[i].sx = cx; L->l[i].sy = cy; L->l[i].area = a; }
            return;
        }
    if (L->n >= 64) return;
    snprintf(L->l[L->n].name, sizeof(L->l[L->n].name), "%s", nm);
    L->l[L->n].sx = cx; L->l[L->n].sy = cy; L->l[L->n].area = a;
    L->n++;
}

/* Text with a dark halo; skipped if it would overlap a placed label */
typedef struct { double x0, y0, x1, y1; } box_t;
static int placed(box_t *bx_, int *nb, double x, double y, double w, double h)
{
    int i;
    for (i = 0; i < *nb; i++)
        if (x < bx_[i].x1 && x + w > bx_[i].x0 && y < bx_[i].y1 && y + h > bx_[i].y0) return 0;
    if (*nb < 128) { bx_[*nb].x0 = x - 4; bx_[*nb].y0 = y - 4; bx_[*nb].x1 = x + w + 4; bx_[*nb].y1 = y + h + 4; (*nb)++; }
    return 1;
}

static void halo_text(cairo_t *cr, double x, double y, const char *s, double r, double g, double b, double a)
{
    cairo_move_to(cr, x, y);
    cairo_text_path(cr, s);
    cairo_set_source_rgba(cr, 0, 0, 0, .75);
    cairo_set_line_width(cr, 3.5);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);
    cairo_move_to(cr, x, y);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_show_text(cr, s);
}

static int build(const double ext[4])
{
    static const char *const roads[3] = { "road3.json", "road2.json", "road1.json" };
    cairo_surface_t *img, *photo, *ink;
    cairo_t *cr;
    build_t b;
    lakes_t *lk;
    cu_place_t *pl;
    box_t boxes[128];
    char *js, path[160];
    size_t len;
    int i, np, nb = 0;

    img = pic_image_load(CU_DIR "/img.jpg");
    if (!img) return -1;
    photo = cairo_image_surface_create(CAIRO_FORMAT_RGB24, BW, BH);
    ink = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, BW, BH);
    lk = calloc(1, sizeof(*lk));
    pl = calloc(64, sizeof(*pl));
    memset(&b, 0, sizeof(b));
    b.cx = malloc(CU_MAX_CREEK_PTS * 2 * sizeof(float));
    b.cstart = malloc(4000 * sizeof(int));
    b.cname = malloc(4000 * sizeof(int));
    if (cairo_surface_status(photo) || cairo_surface_status(ink) || !lk || !pl || !b.cx || !b.cstart || !b.cname) {
        cairo_surface_destroy(img); cairo_surface_destroy(photo); cairo_surface_destroy(ink);
        free(lk); free(pl); free(b.cx); free(b.cstart); free(b.cname);
        return -1;
    }
    /* the photo, stretched to the clock's projection */
    cr = cairo_create(photo);
    cairo_scale(cr, (double)BW / cairo_image_surface_get_width(img),
                (double)BH / cairo_image_surface_get_height(img));
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(img);

    cr = cairo_create(ink);
    memcpy(b.ext, ext, sizeof(b.ext));
    b.cr = cr;
    for (i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), CU_DIR "/%s", roads[i]);
        if ((js = slurp(path, &len)) != NULL) {
            b.kind = 4 - i;
            pic_cu_geo_lines(js, len, NULL, line_cb, &b);
            free(js);
        }
    }
    if ((js = slurp(CU_DIR "/flow.json", &len)) != NULL) {
        b.kind = 1;
        pic_cu_geo_lines(js, len, "gnis_name", line_cb, &b);
        free(js);
    }
    if ((js = slurp(CU_DIR "/water.json", &len)) != NULL) {
        b.kind = 0;
        pic_cu_geo_lines(js, len, "gnis_name", line_cb, &b);
        lk->ext = ext;
        pic_cu_geo_lines(js, len, "gnis_name", lake_cb, lk);
        free(js);
    }
    if ((js = slurp(CU_DIR "/lakes.json", &len)) != NULL) {
        pic_cu_geo_lines(js, len, "gnis_name", lake_cb, lk);
        free(js);
    }

    /* town names, the big ones in capitals; lake names below their lakes */
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    np = (js = slurp(CU_DIR "/places.json", &len)) != NULL ? pic_cu_places(js, len, pl, 64) : 0;
    free(js);
    for (i = 0; i < np; i++) {
        char up[48];
        int k, big = pl[i].area > 100e6;
        cairo_text_extents_t te;
        double x = bx(&b, pl[i].lon), y = by(&b, pl[i].lat);
        if (x < 40 || x > BW - 40 || y < 30 || y > BH - 30) continue;
        for (k = 0; pl[i].name[k] && k < 47; k++)
            up[k] = big && pl[i].name[k] >= 'a' && pl[i].name[k] <= 'z' ? (char)(pl[i].name[k] - 32) : pl[i].name[k];
        up[k] = '\0';
        cairo_set_font_size(cr, big ? 23 : 16);
        cairo_text_extents(cr, up, &te);
        if (x - te.width / 2 < 12 || x + te.width / 2 > BW - 12) continue;
        if (!placed(boxes, &nb, x - te.width / 2, y - te.height, te.width, te.height)) continue;
        halo_text(cr, x - te.width / 2, y, up, .92, .94, .96, .95);
    }
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 14);
    for (i = 0; i < lk->n; i++) {
        cairo_text_extents_t te;
        double x = bx(&b, lk->l[i].sx), y = by(&b, lk->l[i].sy) + 28;
        cairo_text_extents(cr, lk->l[i].name, &te);
        if (x - te.width / 2 < 12 || x + te.width / 2 > BW - 12 || y < 30 || y > BH - 30) continue;
        if (!placed(boxes, &nb, x - te.width / 2, y - te.height, te.width, te.height)) continue;
        halo_text(cr, x - te.width / 2, y, lk->l[i].name, .72, .87, 1, 1);
    }
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11);
    {
        const char *src = "Imagery: USGS The National Map \xc2\xb7 Water: USGS NHD \xc2\xb7 Roads, towns: US Census TIGER";
        cairo_text_extents_t te;
        cairo_text_extents(cr, src, &te);
        halo_text(cr, BW - te.width - 16, BH - 8, src, .62, .66, .7, 1);
    }
    cairo_destroy(cr);
    cairo_surface_flush(photo);
    cairo_surface_flush(ink);

    pthread_mutex_lock(&cb.m);
    for (i = 0, cb.nlbox = 0; i < nb && i < 128; i++, cb.nlbox++) {
        cb.lbox[i][0] = boxes[i].x0; cb.lbox[i][1] = boxes[i].y0;
        cb.lbox[i][2] = boxes[i].x1; cb.lbox[i][3] = boxes[i].y1;
    }
    if (cb.photo) cairo_surface_destroy(cb.photo);
    if (cb.ink) cairo_surface_destroy(cb.ink);
    free(cb.cx); free(cb.cstart); free(cb.cname);
    cb.photo = photo; cb.ink = ink;
    memcpy(cb.ext, ext, sizeof(cb.ext));
    cb.cx = b.cx; cb.cstart = b.cstart; cb.cname = b.cname; cb.ncl = b.ncl; cb.npt = b.npt;
    cb.status[0] = '\0';
    pthread_mutex_unlock(&cb.m);
    printf("closeup: base map built (%d places, %d lakes named, %d creek lines)\n", np, lk->n, b.ncl);
    free(lk);
    free(pl);
    return 0;
}

static int base_work(pic_feature_t *f)
{
    double ext[4], have[4];
    int changed, built;
    (void)f;
    if (pic_config.closeup_zoom <= 1.0) return 300;     /* close-up view off */
    view_ext(ext);
    if (!fetch_sources(ext, &changed)) {
        pthread_mutex_lock(&cb.m);
        snprintf(cb.status, sizeof(cb.status), "Close-up map: couldn't fetch the map data, trying again soon");
        pthread_mutex_unlock(&cb.m);
        return 1800;
    }
    pthread_mutex_lock(&cb.m);
    memcpy(have, cb.ext, sizeof(have));
    built = cb.photo != NULL;
    pthread_mutex_unlock(&cb.m);
    if (!built || changed || !same_ext(have, ext))
        if (build(ext) != 0) return 1800;
    return 3600;
}

static void base_render(cairo_t *cr, int w, int h, time_t now, void *ud)
{
    double ext[4], alt, dim;
    (void)ud;
    if (!pic_view_is_closeup()) return;
    view_ext(ext);
    pthread_mutex_lock(&cb.m);
    if (cb.photo && cb.ink && same_ext(cb.ext, ext)) {
        cairo_save(cr);
        cairo_scale(cr, (double)w / BW, (double)h / BH);
        cairo_set_source_surface(cr, cb.photo, 0, 0);
        cairo_paint(cr);
        /* dimmed by the Sun's height: a dark photo at night, so the
         * live icons stand out */
        alt = pic_astro_sun_alt(now, (ext[1] + ext[3]) / 2, (ext[0] + ext[2]) / 2);
        dim = alt > 6 ? .36 : alt < -12 ? .72 : .36 + (6 - alt) / 18 * .36;
        cairo_set_source_rgba(cr, 0, .02, .05, dim);
        cairo_paint(cr);
        cairo_set_source_surface(cr, cb.ink, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
        {   /* the town and lake names: others' labels keep off them */
            int own = pic_labels_owner("closeup"), i;
            pic_labels_begin(own);
            for (i = 0; i < cb.nlbox; i++) {
                double b[4] = { cb.lbox[i][0] * w / BW, cb.lbox[i][1] * h / BH,
                                cb.lbox[i][2] * w / BW, cb.lbox[i][3] * h / BH };
                pic_labels_claim(own, b);
            }
        }
    } else {
        cairo_set_source_rgb(cr, .06, .09, .12);
        cairo_paint(cr);
    }
    pthread_mutex_unlock(&cb.m);
}

static void base_legend(cairo_t *cr, int w, int h, time_t now, void *ud)
{
    char s[96];
    (void)now; (void)ud;
    if (!pic_view_is_closeup()) return;
    pthread_mutex_lock(&cb.m);
    snprintf(s, sizeof(s), "%s", cb.photo ? cb.status : "Close-up map: fetching the aerial photo and map data\xe2\x80\xa6");
    pthread_mutex_unlock(&cb.m);
    if (s[0]) pic_draw_legend_line(cr, w, h, 0, s, .8, .85, .9);
}

static int base_idle(time_t now, void *d)
{
    (void)now; (void)d;
    return !pic_view_is_closeup();
}

/* Settings saved: if the close-up was just switched on or moved, fetch
 * its map now rather than at the next 5-minute check */
static void base_reload(pic_feature_t *f)
{
    static double last_zoom = -1, last_lat, last_lon;
    if (pic_config.closeup_zoom != last_zoom || pic_config.closeup_lat != last_lat ||
        pic_config.closeup_lon != last_lon) {
        last_zoom = pic_config.closeup_zoom;
        last_lat = pic_config.closeup_lat;
        last_lon = pic_config.closeup_lon;
        if (last_zoom > 1.0) pic_feature_poke(f);
    }
}

static pic_feature_t f_base = {
    .name = "closeup", .layer = base_render, .legend = base_legend, .idle = base_idle,
    .interval = 60, .extra_mb = 18, .work = base_work, .reload = base_reload,
    .needs_network = 1, .default_on = 1,
};

pic_feature_t *pic_feature_closeup(void)
{
    static int inited;
    if (!inited) { pthread_mutex_init(&cb.m, NULL); inited = 1; }
    return &f_base;
}

/* ================================================================ live layer */

static struct {
    pthread_mutex_t m;
    char      url[160];
    cu_wsh_t  w;
    time_t    w_at, f_at;            /* last good fetches                   */
    cu_fire_t fires[CU_MAX_FIRES];
    int       nfires, fire_hours, have_fires;
    char      status[120];
} lv;

#define SITES_FILE CU_DIR "/sites.txt"

/* Where the gauges are: once from USGS, then from the saved file */
static void locate_sites(cu_wsh_t *w)
{
    char url[400], ids[200] = "", line[128], *buf;
    FILE *f;
    int i, need = 0, n;
    if ((f = fopen(SITES_FILE, "r")) != NULL) {
        while (fgets(line, sizeof(line), f)) {
            char id[16];
            double la, lo;
            if (sscanf(line, "%15s %lf %lf", id, &la, &lo) != 3) continue;
            for (i = 0; i < w->n; i++)
                if (!strcmp(w->s[i].site, id)) { w->s[i].lat = la; w->s[i].lon = lo; w->s[i].has_loc = 1; }
        }
        fclose(f);
    }
    for (i = 0; i < w->n; i++)
        if (!w->s[i].has_loc) {
            size_t l = strlen(ids);
            /* USGS site numbers are digits only */
            if (strspn(w->s[i].site, "0123456789") != strlen(w->s[i].site) || l + 20 > sizeof(ids)) continue;
            snprintf(ids + l, sizeof(ids) - l, "%s%s", l ? "," : "", w->s[i].site);
            need++;
        }
    if (!need) return;
    snprintf(url, sizeof(url), "https://waterservices.usgs.gov/nwis/site/?format=rdb&sites=%s&siteOutput=expanded&siteStatus=all", ids);
    buf = malloc(256 * 1024);
    if (!buf) return;
    n = pic_fetch_to_buf(url, buf, 256 * 1024 - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (pic_cu_parse_usgs_sites(buf, w->s, w->n) > 0) {
            mkdir("/data/cache", 0755);
            mkdir(CU_DIR, 0755);
            if ((f = fopen(SITES_FILE, "a")) != NULL) {
                for (i = 0; i < w->n; i++)
                    if (w->s[i].has_loc) fprintf(f, "%s %.7f %.7f\n", w->s[i].site, w->s[i].lat, w->s[i].lon);
                fclose(f);
            }
        }
    }
    free(buf);
}

static int live_work(pic_feature_t *f)
{
    static cu_wsh_t w;
    static cu_fire_t fires[CU_MAX_FIRES];
    char url[200], base[160];
    char *buf;
    int n, hours = 0;
    time_t now = time(NULL), f_at;
    (void)f;
    if (pic_config.closeup_zoom <= 1.0) return 300;
    pthread_mutex_lock(&lv.m);
    snprintf(base, sizeof(base), "%s", lv.url);
    f_at = lv.f_at;
    pthread_mutex_unlock(&lv.m);
    if (!base[0]) {
        pthread_mutex_lock(&lv.m);
        snprintf(lv.status, sizeof(lv.status), "Creeks and field lab: set the Pi-Dash server address (Map view settings)");
        pthread_mutex_unlock(&lv.m);
        return 600;
    }
    buf = malloc(2 * 1024 * 1024);
    if (!buf) return 300;
    snprintf(url, sizeof(url), "%s/api/watershed", base);
    n = pic_fetch_to_buf(url, buf, 2 * 1024 * 1024 - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (pic_cu_parse_watershed(buf, (size_t)n, &w) >= 0) {
            locate_sites(&w);
            pthread_mutex_lock(&lv.m);
            lv.w = w;
            lv.w_at = now;
            lv.status[0] = '\0';
            pthread_mutex_unlock(&lv.m);
        } else n = 0;
    }
    if (n <= 0) {
        pthread_mutex_lock(&lv.m);
        snprintf(lv.status, sizeof(lv.status), "Creeks and field lab: the Pi-Dash server has no watershed data yet");
        pthread_mutex_unlock(&lv.m);
    }
    if (now - f_at >= 1800 || now < f_at) {
        snprintf(url, sizeof(url), "%s/api/ncorbit", base);
        n = pic_fetch_to_buf(url, buf, 2 * 1024 * 1024 - 1);
        if (n > 0) {
            buf[n] = '\0';
            n = pic_cu_parse_fires(buf, (size_t)n, fires, CU_MAX_FIRES, &hours);
            if (n >= 0) {
                pthread_mutex_lock(&lv.m);
                memcpy(lv.fires, fires, (size_t)n * sizeof(cu_fire_t));
                lv.nfires = n;
                lv.fire_hours = hours;
                lv.have_fires = 1;
                lv.f_at = now;
                pthread_mutex_unlock(&lv.m);
            }
        }
    }
    free(buf);
    return 600;
}

static void live_reload(pic_feature_t *f)
{
    char u[160] = "";
    pic_conf_str("PIDASH_URL", u, sizeof(u));
    pthread_mutex_lock(&lv.m);
    if (strcmp(u, lv.url)) {
        snprintf(lv.url, sizeof(lv.url), "%s", u);
        lv.w_at = lv.f_at = 0;
        lv.have_fires = 0;
        pic_feature_poke(f);
    }
    pthread_mutex_unlock(&lv.m);
}

static void cls_colour(const char *k, double c[3])
{
    static const struct { const char *k; double c[3]; } T[] = {
        { "much-below", { 1, .54, .36 } }, { "below", { 1, .81, .42 } }, { "normal", { .37, .89, .54 } },
        { "above", { .44, .71, 1 } }, { "much-above", { .24, .49, 1 } },
    };
    int i;
    c[0] = .66; c[1] = .71; c[2] = .77;
    for (i = 0; i < 5; i++)
        if (!strcmp(k, T[i].k)) { memcpy(c, T[i].c, sizeof(T[i].c)); return; }
}

static void fmt_flow(char *o, size_t n, double q)
{
    if (isnan(q)) snprintf(o, n, "\xe2\x80\x94");
    else if (q < 10) snprintf(o, n, "%.1f", q);
    else snprintf(o, n, "%.0f", q);
}

/* Gauge: a dark disc with two little waves, two ripples spreading out */
static void icon_gauge(cairo_t *cr, double x, double y, double s, const double c[3], double phase)
{
    int k, i;
    cairo_new_path(cr);
    for (k = 0; k < 2; k++) {
        double p = fmod(phase + k * .5, 1.0);
        cairo_new_sub_path(cr);
        cairo_arc(cr, x, y, s * (1 + 1.5 * p), 0, 2 * M_PI);
        cairo_set_source_rgba(cr, c[0], c[1], c[2], .65 * (1 - p));
        cairo_set_line_width(cr, s * .14);
        cairo_stroke(cr);
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, x, y, s, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, .04, .08, .12, .92);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, c[0], c[1], c[2]);
    cairo_set_line_width(cr, s * .2);
    cairo_stroke(cr);
    for (k = -1; k <= 1; k += 2) {
        for (i = 0; i <= 12; i++) {
            double xx = x - s * .6 + i * s * .1, yy = y + k * s * .28 + s * .18 * sin(i / 12.0 * 2 * M_PI + phase * 2 * M_PI);
            if (i) cairo_line_to(cr, xx, yy); else cairo_move_to(cr, xx, yy);
        }
        cairo_set_source_rgb(cr, .6, .83, 1);
        cairo_set_line_width(cr, s * .13);
        cairo_stroke(cr);
    }
}

/* Weather mast: anemometer cups turning (angle from the wind), the Sun */
static void icon_station(cairo_t *cr, double x, double y, double s, double angle, int raining, time_t now)
{
    int k;
    cairo_new_path(cr);
    cairo_set_source_rgb(cr, .9, .92, .94);
    cairo_set_line_width(cr, s * .16);
    cairo_move_to(cr, x, y + s); cairo_line_to(cr, x, y - s);
    cairo_move_to(cr, x - s * .7, y + s); cairo_line_to(cr, x + s * .7, y + s);
    cairo_stroke(cr);
    for (k = 0; k < 3; k++) {
        double a = angle + k * 2 * M_PI / 3, cx = x + s * .7 * cos(a), cy = y - s + s * .25 * sin(a);
        cairo_move_to(cr, x, y - s); cairo_line_to(cr, cx, cy);
        cairo_set_source_rgb(cr, .9, .92, .94);
        cairo_set_line_width(cr, s * .1);
        cairo_stroke(cr);
        cairo_save(cr);
        cairo_new_path(cr);
        cairo_translate(cr, cx, cy);
        cairo_scale(cr, 1, .75);
        cairo_arc(cr, 0, 0, s * .22, 0, 2 * M_PI);
        cairo_restore(cr);
        cairo_set_source_rgb(cr, 1, .8, .35);
        cairo_fill(cr);
    }
    if (raining) {   /* drops falling past the mast */
        for (k = 0; k < 3; k++) {
            double yy = y - s * .4 + fmod(now + k * .33 * 3, 3.0) / 3 * s * 1.4;
            cairo_move_to(cr, x + s * (.5 + .35 * k), yy);
            cairo_line_to(cr, x + s * (.4 + .35 * k), yy + s * .35);
        }
        cairo_set_source_rgb(cr, .5, .82, 1);
        cairo_set_line_width(cr, s * .12);
        cairo_stroke(cr);
    }
}

static void card_text(cairo_t *cr, double x, double y, const char *t, double sz, int bold,
                      double r, double g, double b)
{
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, sz);
    cairo_move_to(cr, x, y);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_show_text(cr, t);
}

static void live_render(cairo_t *cr, int w, int h, time_t now, void *ud)
{
    double s = h / 1080.0, wrap = pic_wrap_threshold_px(w);
    int own = pic_labels_owner("watershed"), i, k;
    double qout = NAN, qriv = NAN;
    (void)ud;
    if (!pic_view_is_closeup()) return;
    pic_labels_begin(own);
    pthread_mutex_lock(&lv.m);

    for (i = 0; i < lv.w.n; i++) {
        if (!strcmp(lv.w.s[i].role, "out")) qout = lv.w.s[i].flow;
        if (!strcmp(lv.w.s[i].role, "river")) qriv = lv.w.s[i].flow;
    }
    /* Water running down Swift Creek and the Neuse: dashes move faster
     * with more flow (from the gauge below the dam, and on the Neuse) */
    pthread_mutex_lock(&cb.m);
    for (i = 0; i < cb.ncl; i++) {
        int a = cb.cstart[i], e = i + 1 < cb.ncl ? cb.cstart[i + 1] : cb.npt;
        double q = cb.cname[i] == 0 ? qout : qriv, spd, dash[2];
        double fresh = !pic_data_stale(lv.w_at, now, 3600);
        if (e - a < 2) continue;
        spd = (!fresh || isnan(q)) ? 0 : (8 + 12 * log10(q + 1)) * s;
        dash[0] = 5 * s; dash[1] = 15 * s;
        for (k = a; k < e; k++) {
            double x = pic_lon_to_x(cb.cx[2 * k], w), y = pic_lat_to_y(cb.cx[2 * k + 1], h);
            if (k > a && fabs(x - pic_lon_to_x(cb.cx[2 * k - 2], w)) >= wrap) cairo_move_to(cr, x, y);
            else if (k == a) cairo_move_to(cr, x, y);
            else cairo_line_to(cr, x, y);
        }
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_dash(cr, dash, 2, -fmod(now * spd, 20 * s));
        cairo_set_source_rgba(cr, .78, .92, 1, spd > 0 ? .95 : .45);
        cairo_set_line_width(cr, 2.6 * s);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }
    pthread_mutex_unlock(&cb.m);

    /* Fires seen from space in view, glowing; older ones fainter */
    for (i = 0; i < lv.nfires; i++) {
        const cu_fire_t *fi = &lv.fires[i];
        double x = pic_lon_to_x(fi->lon, w), y = pic_lat_to_y(fi->lat, h), age, a, r;
        cairo_pattern_t *g;
        if (x < -20 || x > w + 20 || y < -20 || y > h + 20) continue;
        age = fi->t ? difftime(now, fi->t) / 86400.0 : 0;
        a = age < 0 ? 1 : fmax(.35, 1 - age * .15);
        r = (6 + 4 * log10(fi->frp + 1)) * s * (1 + .25 * sin(now * 2.1 + i));
        cairo_new_path(cr);
        g = cairo_pattern_create_radial(x, y, 0, x, y, r * 1.8);
        cairo_pattern_add_color_stop_rgba(g, 0, 1, .95, .7, a);
        cairo_pattern_add_color_stop_rgba(g, .45, 1, .54, .16, .85 * a);
        cairo_pattern_add_color_stop_rgba(g, 1, 1, .3, .1, 0);
        cairo_set_source(cr, g);
        cairo_arc(cr, x, y, r * 1.8, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }

    /* Gauges */
    for (i = 0; i < lv.w.n; i++) {
        const cu_site_t *g = &lv.w.s[i];
        double x, y, c[3], ox, oy, bw, bh, fs = 15 * s;
        char l1[64], l2[96], l3[64], q1[16], q2[16];
        cairo_text_extents_t e1, e2, e3;
        int old;
        if (!g->has_loc) continue;
        x = pic_lon_to_x(g->lon, w); y = pic_lat_to_y(g->lat, h);
        if (x < 0 || x > w || y < 0 || y > h) continue;
        old = pic_data_stale(g->t, now, 3 * 3600) || pic_data_stale(lv.w_at, now, 3600);
        cls_colour(old ? "" : g->cls, c);
        icon_gauge(cr, x, y, 15 * s, c, old ? 0 : fmod(now / 3.0, 1.0));
        { double bx_[4] = { x - 20 * s, y - 20 * s, x + 20 * s, y + 20 * s }; pic_labels_claim(own, bx_); }
        snprintf(l1, sizeof(l1), "%s", g->name);
        fmt_flow(q1, sizeof(q1), g->flow);
        fmt_flow(q2, sizeof(q2), g->normal);
        if (old) {
            char hm[24] = "";
            if (g->t) pic_fmt_hm(hm, sizeof(hm), g->t);
            snprintf(l2, sizeof(l2), "no recent reading%s%s", hm[0] ? " since " : "", hm);
            l3[0] = '\0';
        } else {
            char txt[48];
            snprintf(txt, sizeof(txt), "%s", g->cls_txt);
            {   /* "about normal for the date" -> "about normal" */
                char *p = strstr(txt, " for the date");
                if (p) *p = '\0';
            }
            snprintf(l2, sizeof(l2), "%s ft\xc2\xb3/s \xc2\xb7 %s", q1, txt[0] ? txt : "flow");
            if (!isnan(g->normal)) snprintf(l3, sizeof(l3), "normal for the date \xe2\x89\x88 %s", q2);
            else l3[0] = '\0';
        }
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs);
        cairo_text_extents(cr, l1, &e1);
        cairo_set_font_size(cr, fs * .9);
        cairo_text_extents(cr, l2, &e2);
        cairo_text_extents(cr, l3, &e3);
        bw = fmax(e1.x_advance, fmax(e2.x_advance, e3.x_advance)) + 12 * s;
        bh = fs * (l3[0] ? 3.6 : 2.5);
        pic_labels_place(own, x, y, 24 * s, bw, bh, w, h, &ox, &oy);
        cairo_new_path(cr);
        cairo_rectangle(cr, ox, oy, bw, bh);
        cairo_set_source_rgba(cr, .02, .05, .09, .62);
        cairo_fill(cr);
        card_text(cr, ox + 6 * s, oy + fs * 1.05, l1, fs, 1, .92, .95, .97);
        card_text(cr, ox + 6 * s, oy + fs * 2.15, l2, fs * .9, 0, c[0], c[1], c[2]);
        if (l3[0]) card_text(cr, ox + 6 * s, oy + fs * 3.15, l3, fs * .82, 0, .63, .69, .75);
    }

    /* The field-lab station card */
    if (lv.w.st.ok && !pic_data_stale(lv.w_at, now, 3 * 3600)) {
        const cu_station_t *st = &lv.w.st;
        time_t at = pic_cu_local_time(st->at);
        int old = pic_data_stale(at, now, 2 * 3600);
        double cw = 390 * s, ch = 150 * s, cx, cy, mx, my, fs = 15 * s, ang;
        char v[6][24], hm[24] = "", t[96];
        static const char *const lab[6] = { "Air", "Soil 4 in", "Soil moisture", "Humidity", "Wind", "Sunshine" };
        double vals[6] = { st->air, st->soil, st->moist, st->rh, st->wind, st->sr };
        if (st->has_loc) {
            mx = pic_lon_to_x(st->lon, w); my = pic_lat_to_y(st->lat, h);
            { double bx_[4] = { mx - 20 * s, my - 25 * s, mx + 20 * s, my + 25 * s }; pic_labels_claim(own, bx_); }
            pic_labels_place(own, mx, my, 30 * s, cw, ch, w, h, &cx, &cy);
        } else {   /* position unknown: a card under the clock */
            mx = my = -1;
            cx = 28 * s; cy = 130 * s;
            { double bx_[4] = { cx, cy, cx + cw, cy + ch }; pic_labels_claim(own, bx_); }
        }
        cairo_new_path(cr);
        cairo_rectangle(cr, cx, cy, cw, ch);
        cairo_set_source_rgba(cr, .03, .05, .09, .84);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, .47, .75, 1, .7);
        cairo_set_line_width(cr, 2 * s);
        cairo_stroke(cr);
        /* the cups turn with the wind: up to a third of a turn a second */
        ang = isnan(st->wind) || old ? 0 : fmod(now * fmin(st->wind * .2, 2.1), 2 * M_PI);
        if (mx >= 0) icon_station(cr, mx, my, 16 * s, ang, !old && st->rain_2h > 0, now);
        icon_station(cr, cx + 22 * s, cy + 30 * s, 11 * s, ang, !old && st->rain_2h > 0, now);
        if (at) pic_fmt_hm(hm, sizeof(hm), at);
        snprintf(t, sizeof(t), "NC State field lab \xc2\xb7 Lake Wheeler Rd%s%s", old ? " \xc2\xb7 as of " : "", old ? hm : "");
        card_text(cr, cx + 42 * s, cy + 26 * s, t, fs, 1, .6, .82, 1);
        snprintf(v[0], 24, isnan(vals[0]) ? "\xe2\x80\x94" : "%.0f\xc2\xb0" "F", vals[0]);
        snprintf(v[1], 24, isnan(vals[1]) ? "\xe2\x80\x94" : "%.0f\xc2\xb0" "F", vals[1]);
        snprintf(v[2], 24, isnan(vals[2]) ? "\xe2\x80\x94" : "%.0f%%", vals[2] * 100);
        snprintf(v[3], 24, isnan(vals[3]) ? "\xe2\x80\x94" : "%.0f%%", vals[3]);
        snprintf(v[4], 24, isnan(vals[4]) ? "\xe2\x80\x94" : "%.0f mph", vals[4]);
        snprintf(v[5], 24, isnan(vals[5]) ? "\xe2\x80\x94" : "%.0f W/m\xc2\xb2", vals[5]);
        for (k = 0; k < 6; k++) {
            double x = cx + 16 * s + (k % 3) * 110 * s, y = cy + 58 * s + (k / 3) * 40 * s;
            card_text(cr, x, y, lab[k], fs * .72, 0, .55, .61, .67);
            card_text(cr, x, y + 18 * s, v[k], fs * 1.1, 1, old ? .6 : 1, old ? .63 : 1, old ? .66 : 1);
        }
        if (!isnan(st->rain_7d) && !isnan(st->rain_30d)) {
            snprintf(t, sizeof(t), "Rain: %.2f\" today \xc2\xb7 %.2f\" this week \xc2\xb7 %.2f\" in 30 days%s%s",
                     isnan(st->rain_24h) ? 0 : st->rain_24h, st->rain_7d, st->rain_30d,
                     !old && hm[0] ? " \xc2\xb7 " : "", !old ? hm : "");
            card_text(cr, cx + 16 * s, cy + ch - 12 * s, t, fs * .72, 0, .5, .82, 1);
        }
    }
    pthread_mutex_unlock(&lv.m);
}

static void live_legend(cairo_t *cr, int w, int h, time_t now, void *ud)
{
    char s[220], fire[96];
    int i, inview = 0;
    (void)ud;
    if (!pic_view_is_closeup()) return;
    pthread_mutex_lock(&lv.m);
    for (i = 0; i < lv.nfires; i++) {
        double x = pic_lon_to_x(lv.fires[i].lon, w), y = pic_lat_to_y(lv.fires[i].lat, h);
        if (x >= 0 && x <= w && y >= 0 && y <= h) inview++;
    }
    if (!lv.have_fires || pic_data_stale(lv.f_at, now, 3 * 3600))
        fire[0] = '\0';
    else if (inview)
        snprintf(fire, sizeof(fire), " \xc2\xb7 %d fire%s seen from space here in %d %s (NASA FIRMS)", inview, inview == 1 ? "" : "s",
                 lv.fire_hours > 48 ? lv.fire_hours / 24 : lv.fire_hours, lv.fire_hours > 48 ? "days" : "h");
    else
        snprintf(fire, sizeof(fire), " \xc2\xb7 no fires seen from space here in %d %s (NASA FIRMS)",
                 lv.fire_hours > 48 ? lv.fire_hours / 24 : lv.fire_hours, lv.fire_hours > 48 ? "days" : "h");
    if (lv.status[0] && pic_data_stale(lv.w_at, now, 3600))
        snprintf(s, sizeof(s), "%s", lv.status);
    else
        snprintf(s, sizeof(s), "Creek flow vs. normal for the date (USGS) \xc2\xb7 field lab: NC State Climate Office%s", fire);
    pthread_mutex_unlock(&lv.m);
    pic_draw_legend_line(cr, w, h, 0, s, .85, .9, .95);
}

static int live_idle(time_t now, void *d)
{
    (void)now; (void)d;
    return !pic_view_is_closeup();
}

static pic_feature_t f_live = {
    .name = "watershed", .layer = live_render, .legend = live_legend, .idle = live_idle,
    .interval = 1, .extra_mb = 1, .work = live_work, .reload = live_reload,
    .needs_network = 1, .default_on = 1,
};

pic_feature_t *pic_feature_watershed(void)
{
    static int inited;
    if (!inited) { pthread_mutex_init(&lv.m, NULL); inited = 1; }
    return &f_live;
}

/*
 * season.c - Current-season daytime imagery
 *
 * The classic daytime map is NASA's Blue Marble, a cloud-free mosaic of
 * summer 2004: forever green. This builds a current-season picture
 * instead, from the last SEASON_DAYS days of VIIRS daily surface
 * reflectance (true colour, NOAA-20 and Suomi NPP, ~1 km, via GIBS).
 *
 * The daily images are full of clouds. For each pixel:
 *
 *   1. Pass 1 finds the two darkest blue-channel values over all the
 *      observations. Clouds are bright in blue; ground, vegetation and
 *      lying snow (if it has been there for the whole window) are the
 *      darkest repeatable values. The second darkest is the reference,
 *      so a single cloud shadow can't set it (unless it is far brighter
 *      than the darkest, i.e. only one view was clear).
 *   2. Pass 2 walks the observations newest first and takes the first
 *      two that are close to the reference (not cloud, not shadow),
 *      averaging them. So the picture follows the newest clear view of
 *      each place: autumn colour, leaf drop, harvest, green-up, burn
 *      scars and lasting snow appear within days.
 *
 * Water comes from the Blue Marble (open-ocean VIIRS pixels are full
 * of glint and cloud specks), using the OpenStreetMap land mask with
 * its anti-aliased edge as the blend weight. Places with no clear
 * view in the window (polar night, persistent cloud) keep the Blue
 * Marble.
 *
 * Tiles are cached on disk, so after the first build only the newest
 * day's tiles are downloaded (about 6 MB a day for the world and the
 * local view together).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "season.h"
#include "config.h"
#include "fetch.h"
#include "localview.h"
#include "stb_image.h"
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SEASON_DAYS 12
#define GIBS_WMTS "https://gibs.earthdata.nasa.gov/wmts/epsg4326/best"
#define SR_LAYER  "L3_SurfaceReflectance_BandsM5-M4-M3_Daily"

static const char *sats[] = { "NOAA20", "SNPP" };
#define NSATS 2

static char cache_dir[128];
static pthread_mutex_t build_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Settings ─────────────────────────────────────────────────── */

static volatile int season_on = 1;

int pic_season_enabled(void)
{
    return season_on;
}

void pic_season_reload(void)
{
    int on = pic_conf_int("BASEMAP_SEASON", 1) != 0;
    if (on != season_on) {
        season_on = on;
        pic_regional_bump();     /* redraw with the other imagery */
    }
}

/* ── Tile cache ───────────────────────────────────────────────── */

static int dir_ok(const char *d)
{
    char t[192];
    FILE *f;
    if (mkdir(d, 0755) != 0 && errno != EEXIST) return 0;
    snprintf(t, sizeof(t), "%s/.w", d);
    f = fopen(t, "w");
    if (!f) return 0;
    fclose(f);
    unlink(t);
    return 1;
}

static void pick_cache_dir(void)
{
    if (cache_dir[0]) return;
    mkdir("/data/cache", 0755);
    if (dir_ok("/data/cache/pi-clock-season"))
        snprintf(cache_dir, sizeof(cache_dir), "/data/cache/pi-clock-season");
    else if (dir_ok("/var/cache/pi-clock-season"))
        snprintf(cache_dir, sizeof(cache_dir), "/var/cache/pi-clock-season");
    else
        snprintf(cache_dir, sizeof(cache_dir), "/tmp/pi-clock-season");
    mkdir(cache_dir, 0755);
    printf("season: tile cache in %s\n", cache_dir);
}

/* Remove day tiles older than the window (land masks are kept) */
static void prune_cache(const char *oldest)
{
    DIR *d = opendir(cache_dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        const char *u = strchr(e->d_name, '_');
        char p[400];
        /* <sat>_<YYYY-MM-DD>_... */
        if (!u || strlen(u) < 11 || strncmp(e->d_name, "land_", 5) == 0 ||
            u[5] != '-' || u[8] != '-')
            continue;
        if (strncmp(u + 1, oldest, 10) >= 0) continue;
        snprintf(p, sizeof(p), "%s/%s", cache_dir, e->d_name);
        unlink(p);
    }
    closedir(d);
}

/*
 * One build's download state. After a run of failed downloads the
 * build stops downloading and finishes from the cache, so a network or
 * GIBS outage costs a few seconds, not one timeout per tile.
 */
typedef struct {
    int fails;          /* consecutive network failures            */
    int offline;        /* stopped downloading for this build      */
} dl_t;

#define DL_MAX_FAILS 6
#define MISS_RETRY   (24 * 3600)   /* re-check tiles GIBS didn't have */
#define DAY_FINAL    (36 * 3600)   /* a day's tiles are complete by then */

/*
 * Make sure a tile is in the cache. Returns 0 if the file is there.
 *
 *   fetch    - download if needed (pass 2 reads only what pass 1 had)
 *   day_end  - for a daily tile, the end of its day (UTC); a copy saved
 *              before the day was fully processed is fetched again once
 *              (keeping the old one if that fails). 0 for static tiles.
 *
 * A tile GIBS answers "not found" for is remembered for a day, so gaps
 * (a satellite's missing day, open-ocean land-mask tiles) aren't asked
 * for on every build. Network failures are never remembered.
 */
static int get_tile(dl_t *dl, const char *url, const char *path, int fetch,
                    time_t day_end)
{
    char miss[300], tmp[300];
    struct stat st;
    time_t now = time(NULL);
    int have, rc;

    have = stat(path, &st) == 0 && st.st_size > 0;
    if (have && (!day_end || st.st_mtime >= day_end + DAY_FINAL ||
                 now < day_end + DAY_FINAL))
        return 0;
    if (!fetch || dl->offline) return have ? 0 : -1;
    snprintf(miss, sizeof(miss), "%s.none", path);
    if (!have && stat(miss, &st) == 0) {
        /* Recent days are still being processed: look again sooner */
        int retry = day_end && now < day_end + DAY_FINAL ? MISS_RETRY / 4 : MISS_RETRY;
        if (now - st.st_mtime < retry) return -1;
        unlink(miss);
    }
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    rc = pic_fetch_to_file_rc(url, tmp, 30);
    if (rc == 0 && stat(tmp, &st) == 0 && st.st_size > 100) {
        rename(tmp, path);
        dl->fails = 0;
        return 0;
    }
    unlink(tmp);
    if (rc == 8) {
        /* The server answered: it just doesn't have this tile (yet) */
        dl->fails = 0;
        if (!have) {
            FILE *f = fopen(miss, "w");
            if (f) fclose(f);
        }
    } else if (++dl->fails >= DL_MAX_FAILS) {
        dl->offline = 1;
        printf("season: downloads failing, finishing from the cache\n");
    }
    return have ? 0 : -1;
}

/* ── Compositing ──────────────────────────────────────────────── */

typedef struct {
    int w, h;
    double lon0, lat1, dlon, dlat;     /* per target pixel          */
    int level;
    double span;                        /* tile span, degrees        */
    int r0, r1, c0, c1;                 /* tile range                */
    unsigned char *m1, *m2;             /* two darkest blue values   */
    unsigned char *rgb, *cnt, *land;    /* result, uses, land alpha  */
} comp_t;

/* Target pixel rectangle covered by tile (r, c) */
static int tile_rect(const comp_t *c, int r, int col, int *x0, int *x1, int *y0, int *y1)
{
    double tl = -180 + col * c->span, tr = tl + c->span;
    double tt = 90 - r * c->span, tb = tt - c->span;
    *x0 = (int)ceil((tl - c->lon0) / c->dlon - 0.5);
    *x1 = (int)ceil((tr - c->lon0) / c->dlon - 0.5);
    *y0 = (int)ceil((c->lat1 - tt) / c->dlat - 0.5);
    *y1 = (int)ceil((c->lat1 - tb) / c->dlat - 0.5);
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > c->w) *x1 = c->w;
    if (*y1 > c->h) *y1 = c->h;
    return *x0 < *x1 && *y0 < *y1;
}

/* Source pixel (in a 512x512 tile) for target pixel x/y */
static inline int src_x(const comp_t *c, int col, int x)
{
    double lon = c->lon0 + (x + 0.5) * c->dlon;
    int sx = (int)((lon - (-180 + col * c->span)) / c->span * 512);
    return sx < 0 ? 0 : sx > 511 ? 511 : sx;
}

static inline int src_y(const comp_t *c, int r, int y)
{
    double lat = c->lat1 - (y + 0.5) * c->dlat;
    int sy = (int)(((90 - r * c->span) - lat) / c->span * 512);
    return sy < 0 ? 0 : sy > 511 ? 511 : sy;
}

static void load_land(comp_t *c, dl_t *dl, pic_feature_t *f)
{
    int r, col, x, y, x0, x1, y0, y1;
    for (r = c->r0; r <= c->r1; r++)
        for (col = c->c0; col <= c->c1; col++) {
            char url[256], path[256];
            unsigned char *px;
            int w, h, ch;
            if (f && pic_feature_interrupted(f)) return;
            if (!tile_rect(c, r, col, &x0, &x1, &y0, &y1)) continue;
            snprintf(url, sizeof(url), "%s/OSM_Land_Mask/default/250m/%d/%d/%d.png",
                     GIBS_WMTS, c->level, r, col);
            snprintf(path, sizeof(path), "%s/land_%d_%d_%d.png", cache_dir, c->level, r, col);
            /* No tile = no land in it (open ocean) */
            if (get_tile(dl, url, path, 1, 0) != 0) continue;
            px = stbi_load(path, &w, &h, &ch, 4);
            if (!px || w != 512 || h != 512) {
                if (px) stbi_image_free(px);
                unlink(path);
                continue;
            }
            for (y = y0; y < y1; y++) {
                int sy = src_y(c, r, y);
                for (x = x0; x < x1; x++) {
                    unsigned char a = px[((size_t)sy * 512 + src_x(c, col, x)) * 4 + 3];
                    c->land[(size_t)y * c->w + x] = a;
                }
            }
            stbi_image_free(px);
        }
}

/* One observation tile: pass 1 (darkest blues) or pass 2 (pick) */
static void apply_tile(comp_t *c, const unsigned char *px, int r, int col, int pass)
{
    int x0, x1, y0, y1, x, y;
    if (!tile_rect(c, r, col, &x0, &x1, &y0, &y1)) return;
    for (y = y0; y < y1; y++) {
        const unsigned char *row = px + (size_t)src_y(c, r, y) * 512 * 3;
        size_t o = (size_t)y * c->w;
        for (x = x0; x < x1; x++) {
            size_t i = o + x;
            const unsigned char *p;
            int b, ref;
            if (!c->land[i]) continue;
            p = row + src_x(c, col, x) * 3;
            if (p[0] + p[1] + p[2] < 24) continue;     /* no data */
            b = p[2];
            if (pass == 1) {
                if (b < c->m1[i]) { c->m2[i] = c->m1[i]; c->m1[i] = (unsigned char)b; }
                else if (b < c->m2[i]) c->m2[i] = (unsigned char)b;
                continue;
            }
            if (c->cnt[i] >= 2) continue;
            /* Second darkest, unless it is far brighter than the
             * darkest: then only one view was clear (cloudy tropics) */
            ref = c->m2[i];
            if (ref == 255 || ref > c->m1[i] * 1.5 + 12) ref = c->m1[i];
            if (b > ref * 1.25 + 10 || b < ref * 0.7 - 6) continue;
            if (c->cnt[i] == 0) {
                memcpy(c->rgb + i * 3, p, 3);
            } else {
                unsigned char *q = c->rgb + i * 3;
                q[0] = (unsigned char)((q[0] + p[0] + 1) / 2);
                q[1] = (unsigned char)((q[1] + p[1] + 1) / 2);
                q[2] = (unsigned char)((q[2] + p[2] + 1) / 2);
            }
            c->cnt[i]++;
        }
    }
}

int pic_season_composite(cairo_surface_t *dst, double lon0, double lat0,
                         double lon1, double lat1, pic_feature_t *f,
                         char *newest, int newest_len)
{
    comp_t c;
    dl_t dl = { 0, 0 };
    char days[SEASON_DAYS][16];
    time_t day_end[SEASON_DAYS];
    time_t now = time(NULL);
    int d, s, pass, r, col, x, y, stride, tiles = 0, used = 0;
    size_t n;
    unsigned char *data;
    double ppd;
    int ok = -1;

    if (!dst || cairo_image_surface_get_format(dst) != CAIRO_FORMAT_RGB24) return -1;
    /* One build at a time (the world and local maps would otherwise
     * double the peak RAM); waiting stays interruptible */
    while (pthread_mutex_trylock(&build_mutex) != 0) {
        if (f && pic_feature_interrupted(f)) return -1;
        sleep(1);
    }
    pick_cache_dir();
    memset(&c, 0, sizeof(c));
    c.w = cairo_image_surface_get_width(dst);
    c.h = cairo_image_surface_get_height(dst);
    c.lon0 = lon0;
    c.lat1 = lat1;
    c.dlon = (lon1 - lon0) / c.w;
    c.dlat = (lat1 - lat0) / c.h;
    /* Coarsest level at least ~90% of the target resolution (max 7) */
    ppd = 1.0 / c.dlon;
    for (c.level = 0; c.level < 7; c.level++)
        if (512.0 / (288.0 / (1 << c.level)) >= ppd * 0.9) break;
    c.span = 288.0 / (1 << c.level);
    c.c0 = (int)floor((lon0 + 180) / c.span);
    c.c1 = (int)floor((lon1 + 180 - 1e-9) / c.span);
    c.r0 = (int)floor((90 - lat1) / c.span);
    c.r1 = (int)floor((90 - lat0 - 1e-9) / c.span);
    if (c.c0 < 0) c.c0 = 0;
    if (c.r0 < 0) c.r0 = 0;
    {
        int maxc = (int)ceil(360.0 / c.span) - 1, maxr = (int)ceil(180.0 / c.span) - 1;
        if (c.c1 > maxc) c.c1 = maxc;
        if (c.r1 > maxr) c.r1 = maxr;
    }

    n = (size_t)c.w * c.h;
    c.m1 = malloc(n);
    c.m2 = malloc(n);
    c.cnt = calloc(n, 1);
    c.land = calloc(n, 1);
    c.rgb = malloc(n * 3);
    if (!c.m1 || !c.m2 || !c.cnt || !c.land || !c.rgb) goto out;
    memset(c.m1, 255, n);
    memset(c.m2, 255, n);

    for (d = 0; d < SEASON_DAYS; d++) {
        time_t t = now - (time_t)(d + 1) * 86400;
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(days[d], sizeof(days[d]), "%Y-%m-%d", &tm);
        tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
        day_end[d] = timegm(&tm) + 86400;
    }
    prune_cache(days[SEASON_DAYS - 1]);

    load_land(&c, &dl, f);
    if (f && pic_feature_interrupted(f)) goto out;

    for (pass = 1; pass <= 2; pass++)
        for (d = 0; d < SEASON_DAYS; d++)          /* newest first */
            for (s = 0; s < NSATS; s++)
                for (r = c.r0; r <= c.r1; r++)
                    for (col = c.c0; col <= c.c1; col++) {
                        char url[300], path[256];
                        int x0, x1, y0, y1, w, h, ch;
                        unsigned char *px;
                        if (f && pic_feature_interrupted(f)) goto out;
                        if (!tile_rect(&c, r, col, &x0, &x1, &y0, &y1)) continue;
                        snprintf(url, sizeof(url), "%s/VIIRS_%s_%s/default/%s/500m/%d/%d/%d.jpg",
                                 GIBS_WMTS, sats[s], SR_LAYER, days[d], c.level, r, col);
                        snprintf(path, sizeof(path), "%s/%s_%s_%d_%d_%d.jpg",
                                 cache_dir, sats[s], days[d], c.level, r, col);
                        if (get_tile(&dl, url, path, pass == 1, day_end[d]) != 0) continue;
                        px = stbi_load(path, &w, &h, &ch, 3);
                        if (!px || w != 512 || h != 512) {
                            if (px) stbi_image_free(px);
                            unlink(path);
                            continue;
                        }
                        if (pass == 1) {
                            tiles++;
                            if (newest && !newest[0])
                                snprintf(newest, newest_len, "%s", days[d]);
                        }
                        apply_tile(&c, px, r, col, pass);
                        stbi_image_free(px);
                    }

    if (tiles == 0) {
        printf("season: no imagery available\n");
        goto out;
    }

    /* Which pixels have a usable view. Cloud that never cleared in
     * the tropics stays white-grey or hazy (snow there is only on a few
     * high peaks): treat it as no view. */
    for (y = 0; y < c.h; y++) {
        int trop = fabs(c.lat1 - (y + 0.5) * c.dlat) < 35;
        for (x = 0; x < c.w; x++) {
            size_t i = (size_t)y * c.w + x;
            const unsigned char *q = c.rgb + i * 3;
            int mn, mx, ok = c.land[i] && c.cnt[i];
            if (ok && trop) {
                mn = q[0] < q[1] ? q[0] : q[1];
                mx = q[0] > q[1] ? q[0] : q[1];
                if (q[2] < mn) mn = q[2];
                if (q[2] > mx) mx = q[2];
                if (mn > 70 && mx - mn < 28) ok = 0;
            }
            c.m1[i] = (unsigned char)ok;
        }
    }
    /* Share of usable pixels in a 7x7 box (m2 = row sums), so patchy
     * areas blend smoothly into the Blue Marble instead of speckling */
    for (y = 0; y < c.h; y++) {
        const unsigned char *m = c.m1 + (size_t)y * c.w;
        unsigned char *o = c.m2 + (size_t)y * c.w;
        int sum = 0;
        for (x = 0; x < 3 && x < c.w; x++) sum += m[x];
        for (x = 0; x < c.w; x++) {
            if (x + 3 < c.w) sum += m[x + 3];
            if (x - 4 >= 0) sum -= m[x - 4];
            o[x] = (unsigned char)sum;
        }
    }

    /* Blend into the Blue Marble already in dst */
    cairo_surface_flush(dst);
    data = cairo_image_surface_get_data(dst);
    stride = cairo_image_surface_get_stride(dst);
    for (y = 0; y < c.h; y++) {
        uint32_t *row = (uint32_t *)(data + (size_t)y * stride);
        int ya = y - 3 < 0 ? 0 : y - 3, yb = y + 3 >= c.h ? c.h - 1 : y + 3;
        for (x = 0; x < c.w; x++) {
            size_t i = (size_t)y * c.w + x;
            unsigned a = c.land[i], share = 0, n = 0, br, bg, bb;
            uint32_t p = row[x];
            const unsigned char *q;
            int yy;
            if (!a || !c.m1[i]) continue;
            for (yy = ya; yy <= yb; yy++) share += c.m2[(size_t)yy * c.w + x];
            n = (unsigned)(yb - ya + 1) * 7;
            /* full weight from 60% usable, none below 30% */
            share = share * 100 / n;
            if (share < 30) continue;
            if (share < 60) a = a * (share - 30) / 30;
            q = c.rgb + i * 3;
            br = (p >> 16) & 255; bg = (p >> 8) & 255; bb = p & 255;
            br = (q[0] * a + br * (255 - a)) / 255;
            bg = (q[1] * a + bg * (255 - a)) / 255;
            bb = (q[2] * a + bb * (255 - a)) / 255;
            row[x] = 0xFF000000u | (br << 16) | (bg << 8) | bb;
            used++;
        }
    }
    cairo_surface_mark_dirty(dst);
    printf("season: %dx%d level %d, %d tiles, %d px from %s back %d days\n",
           c.w, c.h, c.level, tiles, used, newest ? newest : "?", SEASON_DAYS);
    ok = 0;

out:
    free(c.m1); free(c.m2); free(c.cnt); free(c.land); free(c.rgb);
    pthread_mutex_unlock(&build_mutex);
    return ok;
}

/* ── World map feature ────────────────────────────────────────── */

static pthread_mutex_t w_mutex = PTHREAD_MUTEX_INITIALIZER;
static cairo_surface_t *w_classic;     /* Blue Marble (not owned)       */
static cairo_surface_t *w_season;      /* Composite (owned)             */
static char w_newest[16];

void pic_season_set_world(cairo_surface_t *blue_marble)
{
    pthread_mutex_lock(&w_mutex);
    w_classic = blue_marble;
    pthread_mutex_unlock(&w_mutex);
}

cairo_surface_t *pic_season_world_for(cairo_surface_t *src)
{
    cairo_surface_t *s = NULL;
    if (!season_on) return NULL;
    pthread_mutex_lock(&w_mutex);
    if (src && src == w_classic && w_season)
        s = cairo_surface_reference(w_season);
    pthread_mutex_unlock(&w_mutex);
    return s;
}

const char *pic_season_world_date(void)
{
    return w_newest;
}

static int season_work(pic_feature_t *f)
{
    cairo_surface_t *bm, *img;
    char newest[16] = "";
    int w, h, s;
    cairo_t *cr;

    pthread_mutex_lock(&w_mutex);
    bm = w_classic ? cairo_surface_reference(w_classic) : NULL;
    pthread_mutex_unlock(&w_mutex);
    if (!bm) return 600;

    /* 2048x1024 is past the ~2 km detail of the world-scale tiles and
     * keeps RAM modest (the Blue Marble may be the 4k image) */
    w = cairo_image_surface_get_width(bm);
    if (w > 2048) w = 2048;
    h = w / 2;
    img = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(img);
        cairo_surface_destroy(bm);
        return 1800;
    }
    cr = cairo_create(img);
    cairo_scale(cr, (double)w / cairo_image_surface_get_width(bm),
                (double)h / cairo_image_surface_get_height(bm));
    cairo_set_source_surface(cr, bm, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(bm);

    if (pic_season_composite(img, -180, -90, 180, 90, f, newest, sizeof(newest)) != 0) {
        cairo_surface_destroy(img);
        return pic_feature_interrupted(f) ? 1 : 1800;
    }
    pthread_mutex_lock(&w_mutex);
    if (w_season) cairo_surface_destroy(w_season);
    w_season = img;
    snprintf(w_newest, sizeof(w_newest), "%s", newest);
    pthread_mutex_unlock(&w_mutex);
    pic_regional_bump();

    /* New imagery appears daily; rebuild every six hours (only new
     * tiles are downloaded) */
    for (s = 0; s < 6 * 3600; s++) {
        if (pic_feature_interrupted(f)) return 1;
        sleep(1);
    }
    return 1;
}

static int season_wanted(pic_feature_t *f)
{
    (void)f;
    return season_on;
}

static void season_reload(pic_feature_t *f)
{
    (void)f;
    pic_season_reload();
}

static void season_stopped(pic_feature_t *f)
{
    (void)f;
    pthread_mutex_lock(&w_mutex);
    if (w_season) cairo_surface_destroy(w_season);
    w_season = NULL;
    pthread_mutex_unlock(&w_mutex);
}

static pic_feature_t season_def = {
    .name = "season", .wanted = season_wanted, .reload = season_reload,
    .work = season_work, .needs_network = 1, .stopped = season_stopped,
};

pic_feature_t *pic_feature_season(void)
{
    return &season_def;
}

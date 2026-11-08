/*
 * layers_overhead.c - Satellites in view right now
 *
 * Every low-orbit satellite in the SatNOGS database (about 1,600,
 * mostly small science and amateur satellites, plus the ISS and other
 * crewed stations), CelesTrak's ~150 brightest satellites when it can
 * be reached, and AMSAT's list, moved live on the map. Meant for the local view, where it shows what is passing
 * over the region at this moment:
 *
 * Names and trails are coloured by state (cool colours, to stand out
 * from the warm city lights of the night map):
 *   aqua    sunlit and above 10° from home while home is dark
 *           (look up: you can see it)
 *   blue    sunlit
 *   rose    in Earth's shadow (trail dashed)
 *
 * Each satellite has a short trail showing where it came from in the
 * last minute. Names are shown for the ones above home's horizon
 * and, space permitting, the rest. A note at the bottom counts them.
 *
 * Only low orbits (> 11 revolutions a day) are drawn: geostationary
 * and GPS satellites barely move and would clutter the view.
 *
 * TLEs are cached in /tmp and refreshed once a day: CelesTrak blocks
 * addresses that download the same data more than about every 2 h.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "features_all.h"
#include "sat_icons.h"
#include "skycover.h"
#include "sprites.h"
#include "satellite.h"
#include "solar.h"
#include "config.h"
#include "fetch.h"
#include "spacetrack.h"
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#define OV_MAX       2000
#define OV_DIR       "/tmp/pi-clock-cache"
#define OV_VISUAL    OV_DIR "/visual.tle"
#define OV_AMSAT     OV_DIR "/amsat.tle"
#define OV_SATNOGS   OV_DIR "/satnogs.tle"
#define OV_REFRESH   (24 * 3600)
#define OV_RETRY     (6 * 3600)
#define URL_VISUAL   "https://celestrak.org/NORAD/elements/gp.php?GROUP=visual&FORMAT=tle"
#define URL_AMSAT    "https://www.amsat.org/tle/current/nasabare.txt"
#define URL_SATNOGS  "https://db.satnogs.org/api/tle/?format=json"

double pic_sat_look_angles(const pic_sat_t *s, time_t t, double qlat,
                           double qlon, double *az, int *sunlit);

typedef struct {
    pic_sat_t sat;
    char      label[24];
} ov_sat_t;

static struct {
    pthread_mutex_t mutex;
    ov_sat_t *sats;
    int       n;
    int       in_view, visible;   /* from the last render, for the note */
    int       hidden, cloud;      /* lit and up but clouded out; cloud % */
    unsigned  gen;                /* bumped when the list is replaced  */
} ov = { PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0, 0, 0, -1, 0 };

/* "STARLINK-1234" -> "Starlink-1234", "ISS (ZARYA)" -> "ISS";
 * short all-caps runs (NOAA, SL-16, R/B) stay as they are */
static void tidy_name(const char *in, char *out, size_t n)
{
    char buf[160];
    size_t i = 0, k, len;

    while (*in == ' ') in++;
    if (in[0] == '0' && in[1] == ' ') in += 2;   /* "0 NAME" 3LE form */
    snprintf(buf, sizeof(buf), "%s", in);
    buf[strcspn(buf, "\r\n")] = '\0';
    for (len = strlen(buf); len > 0 && buf[len - 1] == ' '; len--) buf[len - 1] = '\0';
    if (!strncmp(buf, "ISS (", 5)) { snprintf(out, n, "ISS"); return; }
    if (!strncmp(buf, "CSS (", 5)) { snprintf(out, n, "Tiangong"); return; }
    if (!strcmp(buf, "HST")) { snprintf(out, n, "Hubble"); return; }
    while (buf[i]) {
        if (isalpha((unsigned char)buf[i])) {
            size_t j = i;
            while (isalpha((unsigned char)buf[j])) j++;
            if (j - i > 4)
                for (k = i + 1; k < j; k++)
                    buf[k] = (char)tolower((unsigned char)buf[k]);
            i = j;
        } else {
            i++;
        }
    }
    snprintf(out, n, "%s", buf);
}

/*
 * Parse a TLE file (2- or 3-line form) into list[], skipping
 * duplicates and high orbits. Lines are matched by their "1 "/"2 "
 * prefix, so a long or missing name line can't throw the rest off.
 */
static int load_tles(const char *path, ov_sat_t *list, int n, int max)
{
    FILE *fp = fopen(path, "r");
    char line[160], name[160] = "", l1[160] = "";
    double now_jd = time(NULL) / 86400.0 + 2440587.5;
    int i;

    if (!fp) return n;
    while (n < max && fgets(line, sizeof(line), fp)) {
        ov_sat_t *e = &list[n];
        double age;
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '1' && line[1] == ' ' && strlen(line) >= 69) {
            snprintf(l1, sizeof(l1), "%s", line);
            continue;
        }
        if (!(line[0] == '2' && line[1] == ' ' && strlen(line) >= 69)) {
            snprintf(name, sizeof(name), "%s", line);   /* name line */
            l1[0] = '\0';
            continue;
        }
        if (!l1[0]) continue;
        memset(e, 0, sizeof(*e));
        if (pic_sat_parse_tle(&e->sat, l1, line) != 0) { l1[0] = '\0'; continue; }
        e->sat.norad_id = pic_tle_catnr(l1);
        tidy_name(name[0] ? name : "Satellite", e->label, sizeof(e->label));
        /* Not yet identified pieces of a rideshare launch are catalogued
         * as "OBJECT A", "OBJECT B"...: add the launch's COSPAR ID
         * (from line 1) so they can be looked up and told apart */
        if (!strncmp(e->label, "Object ", 7) && strlen(e->label) <= 10 &&
            isdigit((unsigned char)l1[9]) && isdigit((unsigned char)l1[11])) {
            char piece[4] = "";
            int k;
            for (k = 0; k < 3 && isalpha((unsigned char)l1[14 + k]); k++)
                piece[k] = l1[14 + k];
            piece[k] = '\0';
            /* "Object 2022-167E" */
            snprintf(e->label, sizeof(e->label), "Object %s%.2s-%.3s%s",
                     l1[9] >= '5' ? "19" : "20", l1 + 9, l1 + 11, piece);
        }
        snprintf(e->sat.name, sizeof(e->sat.name), "%s", e->label);
        l1[0] = '\0';
        name[0] = '\0';
        if (e->sat.mean_motion < 11.0) continue;      /* not LEO */
        /* Stale elements: a low-orbit TLE is far off after a few weeks,
         * and lists keep ones for objects that have since re-entered
         * (a decaying object, > 16 rev/day, is gone within days) */
        age = now_jd - e->sat.epoch_jd;
        if (age > 21.0 || (e->sat.mean_motion > 16.0 && age > 3.0)) continue;
        for (i = 0; i < n; i++)
            if (list[i].sat.norad_id == e->sat.norad_id) break;
        if (i < n) {
            /* Same object from another list: keep the newer elements */
            if (e->sat.epoch_jd > list[i].sat.epoch_jd) {
                char keep[24];
                snprintf(keep, sizeof(keep), "%s", list[i].label);
                list[i] = *e;
                if (strncmp(keep, "Object ", 7) != 0) {   /* keep a real name */
                    snprintf(list[i].label, sizeof(list[i].label), "%s", keep);
                    snprintf(list[i].sat.name, sizeof(list[i].sat.name), "%s", keep);
                }
            }
            continue;
        }
        n++;
    }
    fclose(fp);
    return n;
}

/*
 * A download failed: keep using the old copy, and date it so the next
 * attempt is OV_RETRY from now rather than at the next hourly check
 * (CelesTrak blocks addresses that keep asking). -1 if there is none.
 */
static int keep_stale(const char *path)
{
    struct utimbuf t;
    if (access(path, R_OK) != 0) return -1;
    t.actime = t.modtime = time(NULL) - OV_REFRESH + OV_RETRY;
    utime(path, &t);
    return 0;
}

static int fresh(const char *path, time_t max_age)
{
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 500 &&
           time(NULL) - st.st_mtime < max_age;
}

/* Download url to path unless a recent copy exists. 0 if a copy exists. */
static int refresh(const char *url, const char *path)
{
    char tmp[128];
    if (fresh(path, OV_REFRESH)) return 0;
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if (pic_fetch_to_file(url, tmp, 60) == 0 && rename(tmp, path) == 0)
        return 0;
    unlink(tmp);
    return keep_stale(path);
}

/*
 * CelesTrak's brightest-satellites list. Its NORAD numbers are kept in
 * /data/cache each time it downloads, so when CelesTrak can't be
 * reached the same satellites can come from Space-Track instead.
 */
#define OV_VISUAL_IDS "/data/cache/pi-clock-visual-ids.txt"

static void save_visual_ids(void)
{
    FILE *in = fopen(OV_VISUAL, "r"), *out;
    char line[160];
    int n = 0;
    if (!in) return;
    mkdir("/data/cache", 0755);
    out = fopen(OV_VISUAL_IDS ".new", "w");
    if (!out) { fclose(in); return; }
    while (fgets(line, sizeof(line), in))
        if (line[0] == '1' && line[1] == ' ') {
            int id = atoi(line + 2);
            if (id > 0) fprintf(out, "%s%d", n++ ? "," : "", id);
        }
    fclose(in);
    fclose(out);
    if (n > 0) rename(OV_VISUAL_IDS ".new", OV_VISUAL_IDS);
    else unlink(OV_VISUAL_IDS ".new");
}

/* Bright naked-eye satellites when CelesTrak's list isn't available:
 * Jay Respler's visual list (satobs.org/britelst.html, the source of
 * CelesTrak's group) plus Envisat, Tianhe and BlueWalker 3. Objects
 * that have since re-entered simply return nothing. */
static const char BRIGHT_IDS[] =
    "694,733,877,1843,2802,3019,3230,3597,3669,3835,4119,4327,4393,4813,4814,4849"
    ",5118,5282,5328,5560,5730,6073,6153,6155,6212,7004,7338,8063,8128,8459,8745,"
    "10114,10363,10561,10861,10967,11056,11155,11156,11267,11285,11286,11332,1157"
    "4,11672,11849,11933,12054,12139,12389,12465,12585,12904,13068,13154,13402,13"
    "403,13819,14207,14208,14372,14484,14699,14819,14820,15354,15483,15772,15945,"
    "16111,16182,16496,16792,16882,17295,17567,17589,17590,17912,17973,18153,1818"
    "7,18749,18958,19046,19120,19210,19257,19573,19574,19650,19764,20261,20262,20"
    "323,20390,20453,20465,20466,20511,20580,20625,20663,20666,20775,20967,21088,"
    "21397,21422,21423,21574,21610,21701,21819,21820,21867,21876,21938,22220,2228"
    "5,22286,22566,22626,22803,22830,23087,23088,23343,23405,23560,23561,23705,24"
    "298,24883,25018,25400,25407,25544,25723,25732,25860,25861,25887,26775,26873,"
    "26874,27386,48274,53807";

static int refresh_visual(void)
{
    char tmp[128], ids[1600] = "", q[1800];
    FILE *f;
    if (fresh(OV_VISUAL, OV_REFRESH)) return 0;
    snprintf(tmp, sizeof(tmp), "%s.new", OV_VISUAL);
    if (pic_fetch_to_file(URL_VISUAL, tmp, 60) == 0 && rename(tmp, OV_VISUAL) == 0) {
        save_visual_ids();
        return 0;
    }
    unlink(tmp);
    if ((f = fopen(OV_VISUAL_IDS, "r")) != NULL) {
        if (!fgets(ids, sizeof(ids), f)) ids[0] = 0;
        fclose(f);
        ids[strcspn(ids, "\r\n")] = 0;
    }
    if (!ids[0]) snprintf(ids, sizeof(ids), "%s", BRIGHT_IDS);
    if (ids[0] && strlen(ids) < sizeof(ids) - 2) {
        snprintf(q, sizeof(q), ST_Q_IDS, ids);
        if (pic_spacetrack_fetch(q, tmp) == 0 && rename(tmp, OV_VISUAL) == 0) return 0;
        unlink(tmp);
    }
    return keep_stale(OV_VISUAL);
}

/* SatNOGS JSON -> 3-line TLE file. Returns 0 when a copy exists. */
static int refresh_satnogs(void)
{
    char tmp[] = OV_DIR "/satnogs.json";
    FILE *in, *out;
    char *buf, *p;
    long len;
    int n = 0;

    if (fresh(OV_SATNOGS, OV_REFRESH)) return 0;
    if (pic_fetch_to_file(URL_SATNOGS, tmp, 90) != 0) {
        unlink(tmp);
        return keep_stale(OV_SATNOGS);
    }
    in = fopen(tmp, "rb");
    if (!in) return -1;
    fseek(in, 0, SEEK_END);
    len = ftell(in);
    fseek(in, 0, SEEK_SET);
    buf = len > 0 && len < 8 * 1024 * 1024 ? malloc((size_t)len + 1) : NULL;
    if (!buf || fread(buf, 1, (size_t)len, in) != (size_t)len) {
        free(buf);
        fclose(in);
        unlink(tmp);
        return -1;
    }
    fclose(in);
    unlink(tmp);
    buf[len] = '\0';
    out = fopen(OV_SATNOGS ".new", "w");
    if (!out) { free(buf); return -1; }
    for (p = buf; (p = strstr(p, "\"tle0\"")) != NULL; ) {
        char f[3][96];
        const char *keys[3] = { "\"tle0\"", "\"tle1\"", "\"tle2\"" };
        int k, ok = 1;
        for (k = 0; k < 3 && ok; k++) {
            char *q = strstr(p, keys[k]), *v;
            size_t m;
            if (!q) { ok = 0; break; }
            v = strchr(q + 6, '"');
            if (!v) { ok = 0; break; }
            v++;
            m = strcspn(v, "\"");
            if (m >= sizeof(f[k])) m = sizeof(f[k]) - 1;
            memcpy(f[k], v, m);
            f[k][m] = '\0';
            if (k == 2) p = v + m;
        }
        if (!ok) break;
        if (f[1][0] == '1' && f[2][0] == '2') {
            fprintf(out, "%s\n%s\n%s\n", f[0], f[1], f[2]);
            n++;
        }
    }
    fclose(out);
    free(buf);
    if (n < 10 || rename(OV_SATNOGS ".new", OV_SATNOGS) != 0) {
        unlink(OV_SATNOGS ".new");
        return keep_stale(OV_SATNOGS);
    }
    return 0;
}

static int overhead_work(pic_feature_t *f)
{
    ov_sat_t *list;
    int n = 0, got = 0;

    (void)f;
    pic_skycover_refresh(time(NULL));
    mkdir(OV_DIR, 0755);
    if (refresh_visual() == 0) got++;
    if (refresh_satnogs() == 0) got++;
    if (refresh(URL_AMSAT, OV_AMSAT) == 0) got++;
    list = calloc(OV_MAX, sizeof(*list));
    if (!list) return 600;
    /* Order sets the name used for duplicates: CelesTrak's are best */
    n = load_tles(OV_VISUAL, list, n, OV_MAX);
    n = load_tles(OV_SATNOGS, list, n, OV_MAX);
    n = load_tles(OV_AMSAT, list, n, OV_MAX);
    pthread_mutex_lock(&ov.mutex);
    free(ov.sats);
    ov.sats = list;
    ov.n = n;
    ov.gen++;
    pthread_mutex_unlock(&ov.mutex);
    printf("overhead: %d low-orbit satellites (%d/3 sources)\n", n, got);
    /* Recheck hourly: refresh() only downloads once a copy is a day
     * old, and a failed source waits OV_RETRY (keep_stale) */
    return got ? 3600 : OV_RETRY;
}

void pic_layer_render_overhead(cairo_t *cr, int width, int height,
                               time_t now, void *ud)
{
    double span = pic_config.view_span_lon, span_lat = pic_config.view_span_lat;
    double clon = pic_config.view_center_lon, clat = pic_config.view_center_lat;
    double qlat = pic_config.qth_lat, qlon = pic_config.qth_lon;
    double s = height / 360.0 * (1 + 0.15 * (pic_symbol_zoom() - 1));
    double fs = height / 100.0, wrap = pic_wrap_threshold_px(width);
    static double boxes[OV_MAX][4];
    int i, nbox = 0, in_view = 0, visible = 0, hidden = 0, home_dark, pass, sky, cloud;
    pic_solar_position_t sp = pic_solar_position(now);

    (void)ud;
    if (span < 0.0001) span = 360;
    if (span_lat < 0.0001) span_lat = 180;
    {   /* Sun elevation at home: is it dark enough to see satellites? */
        double la = qlat * M_PI / 180, d = sp.declination;
        double ha = sp.gha + qlon * M_PI / 180;
        double el = asin(sin(la) * sin(d) + cos(la) * cos(d) * cos(ha)) * 180 / M_PI;
        home_dark = el < -6;
    }
    /* "Visible from home" only when the sky there is clear enough */
    sky = pic_sky_clear(now, now, &cloud);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    pthread_mutex_lock(&ov.mutex);
    /*
     * Redrawn every second for smooth motion. Propagating all ~1,700
     * satellites each second would be wasteful on a Pi, so every 10 s
     * (or when the view or the list changes) all are checked and those
     * near the view (a margin wider than 10 s of LEO travel) become the
     * candidates; in between only the candidates are propagated.
     */
    {
        static struct { int i; double lat, lon, el; int lit; } vis[OV_MAX];
        static pic_sprite_t spl[PIC_SPRITES_MAX];
        static int spmap[OV_MAX];
        int sprites, nsp;
        static int cand[OV_MAX], ncand = -1;
        static time_t cand_t;
        static double cand_key[3];
        static unsigned cand_gen;
        int nv = 0, j, full, nnew = 0, cnt;
        double key[3] = { clon, clat, span };
        full = ncand < 0 || now - cand_t >= 10 || now < cand_t || cand_gen != ov.gen ||
               memcmp(key, cand_key, sizeof(key)) != 0;
        cnt = full ? ov.n : ncand;
        for (j = 0; j < cnt; j++) {
            double lat, lon, alt, dlon, mlon, mlat;
            i = full ? j : cand[j];
            if (i >= ov.n) continue;
            pic_sat_position_at(&ov.sats[i].sat, now, &lat, &lon, &alt);
            if (alt < 0) continue;              /* decayed since its TLE */
            dlon = fmod(lon - clon + 540.0, 360.0) - 180.0;
            if (full) {
                /* 0.55 of the span plus ~1 deg (10 s of travel) */
                mlon = span * 0.55 + 1.0 / fmax(0.2, cos(lat * M_PI / 180));
                mlat = span_lat * 0.55 + 1.0;
                if (fabs(dlon) <= mlon && fabs(lat - clat) <= mlat)
                    cand[nnew++] = i;
            }
            if (fabs(dlon) > span * 0.55 || fabs(lat - clat) > span_lat * 0.55)
                continue;
            vis[nv].i = i; vis[nv].lat = lat; vis[nv].lon = lon;
            vis[nv].lit = 0;
            vis[nv].el = pic_sat_look_angles(&ov.sats[i].sat, now, qlat, qlon,
                                             NULL, &vis[nv].lit);
            nv++;
        }
        if (full) {
            ncand = nnew;
            cand_t = now;
            memcpy(cand_key, key, sizeof(key));
            cand_gen = ov.gen;
        }
        /* Smooth mode: the framebuffer loop draws the markers and labels
         * (sprites.c) at ~30 fps from positions now and 1 s on; this
         * layer keeps the trails and decides which labels fit */
        sprites = pic_sprites_enabled();
        nsp = 0;
        /* Pass 0: marks and trails; 1: labels of sats above home's
         * horizon (and named ones); 2: other labels where they fit */
        for (pass = 0; pass < 3; pass++)
            for (j = 0; j < nv; j++) {
                const pic_sat_t *st = &ov.sats[vis[j].i].sat;
                double x, y, r, g, b, a, lr, lg, lb, la, el = vis[j].el;
                int lit = vis[j].lit, up, k, known;

                up = el > 10;
                x = pic_lon_to_x(vis[j].lon, width);
                y = pic_lat_to_y(vis[j].lat, height);
                if (lit && up && home_dark && sky == PIC_SKY_CLEAR) { r = 1.0; g = 0.85; b = 0.25; a = 1.0; }
                else if (lit)               { r = 0.95; g = 0.95; b = 0.97; a = 0.9; }
                else                        { r = 0.55; g = 0.58; b = 0.65; a = 0.7; }
                la = pic_sat_label_rgb(r, g, b, &lr, &lg, &lb);
                /* 2: landmark (ISS, Tiangong, Hubble), drawn large and
                 * always labelled; 1: everyone else as a realistic icon
                 * when zoomed in (dots when the view is wider than 60 deg) */
                known = pic_sat_is_landmark(st->name) ? 2 : span <= 60 ? 1 : 0;

                if (pass == 0) {
                    double px = x, py = y;
                    in_view++;
                    if (lit && up && home_dark) {
                        if (sky == PIC_SKY_CLEAR) visible++;
                        else hidden++;
                    }
                    /* Trail: last 60 s, fading */
                    for (k = 1; k <= 3; k++) {
                        double la2, lo2, al2, x2, y2;
                        pic_sat_position_at(st, now - k * 20, &la2, &lo2, &al2);
                        x2 = pic_lon_to_x(lo2, width);
                        y2 = pic_lat_to_y(la2, height);
                        if (fabs(x2 - px) > wrap) break;
                        cairo_new_path(cr);
                        cairo_move_to(cr, px, py);
                        cairo_line_to(cr, x2, y2);
                        /* dark halo, then the trail in the name colour;
                         * dashed while in Earth's shadow */
                        if (!lit) {
                            double dash[2] = { s * 3.0, s * 2.2 };
                            cairo_set_dash(cr, dash, 2, 0);
                        }
                        cairo_set_source_rgba(cr, 0, 0, 0, 0.45 * (1.0 - 0.25 * k));
                        cairo_set_line_width(cr, s * (2.4 - 0.4 * k));
                        cairo_stroke_preserve(cr);
                        cairo_set_source_rgba(cr, lr, lg, lb, la * (0.9 - 0.2 * k));
                        cairo_set_line_width(cr, s * (1.3 - 0.25 * k));
                        cairo_stroke(cr);
                        cairo_set_dash(cr, NULL, 0, 0);
                        px = x2; py = y2;
                    }
                    if (sprites) {
                        spmap[j] = -1;
                        if (nsp < PIC_SPRITES_MAX && x > -50 && y > -50 &&
                            x < width + 50 && y < height + 50) {
                            pic_sprite_t *q = &spl[nsp];
                            double la1, lo1, al1;
                            memset(q, 0, sizeof(*q));
                            snprintf(q->icon, sizeof(q->icon), "%s", st->name);
                            snprintf(q->label, sizeof(q->label), "%s", ov.sats[vis[j].i].label);
                            q->known = (unsigned char)known;
                            q->up = (unsigned char)up;
                            q->ring = (unsigned char)(lit && up && home_dark && sky == PIC_SKY_CLEAR);
                            q->r = (float)r; q->g = (float)g; q->b = (float)b; q->a = (float)a;
                            q->lat0 = vis[j].lat; q->lon0 = vis[j].lon;
                            pic_sat_position_at(st, now + 1, &la1, &lo1, &al1);
                            q->lat1 = la1; q->lon1 = lo1;
                            spmap[j] = nsp++;
                        }
                    } else if (known) {
                        double la1, lo1, al1, ang = 0;
                        pic_sat_position_at(st, now + 1, &la1, &lo1, &al1);
                        if (fabs(pic_lon_to_x(lo1, width) - x) < wrap)
                            ang = pic_sat_icon_angle(st->name, pic_lon_to_x(lo1, width) - x,
                                                     pic_lat_to_y(la1, height) - y);
                        pic_draw_sat_icon(cr, st->name, x, y, s * (known == 2 ? 14 : 10), ang,
                                          r, g, b);
                        if (known == 2) {
                            int aboard = pic_sat_kind(st->name) == PIC_SAT_ISS ? pic_crew_aboard("ISS") :
                                         pic_sat_kind(st->name) == PIC_SAT_TIANGONG ? pic_crew_aboard("Tiangong") : -1;
                            pic_draw_crew_badge(cr, x + s * 7.5, y + s * 6.0, s * 5.0, aboard, lr, lg, lb);
                        }
                    } else {
                        cairo_new_path(cr);
                        cairo_arc(cr, x, y, s * (up ? 2.2 : 1.6), 0, 2 * M_PI);
                        cairo_set_source_rgba(cr, r, g, b, a);
                        cairo_fill_preserve(cr);
                        cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
                        cairo_set_line_width(cr, s * 0.5);
                        cairo_stroke(cr);
                    }
                    if (!sprites && lit && up && home_dark) {   /* ring: look up now */
                        cairo_new_path(cr);
                        cairo_arc(cr, x, y, s * (known == 2 ? 9 : known ? 6.5 : 4.2), 0, 2 * M_PI);
                        cairo_set_source_rgba(cr, lr, lg, lb, 0.9);
                        cairo_set_line_width(cr, s * 0.6);
                        cairo_stroke(cr);
                    }
                } else {
                    cairo_text_extents_t e;
                    double lx, ly, bx[4];
                    const char *lab = ov.sats[vis[j].i].label;
                    int q, clash = 0;
                    if ((pass == 1) != (up || known == 2)) continue;
                    if (x < 0 || x > width || y < 0 || y > height) continue;
                    cairo_text_extents(cr, lab, &e);
                    lx = x + s * (known == 2 ? 9 : known ? 6.5 : 4);
                    ly = y - s * 2;
                    bx[0] = lx - 2; bx[1] = ly - fs; bx[2] = lx + e.x_advance + 2; bx[3] = ly + fs * 0.35;
                    for (q = 0; q < nbox && !clash; q++)
                        clash = !(bx[2] < boxes[q][0] || bx[0] > boxes[q][2] ||
                                  bx[3] < boxes[q][1] || bx[1] > boxes[q][3]);
                    if (clash || nbox >= OV_MAX) continue;
                    memcpy(boxes[nbox++], bx, sizeof(bx));
                    if (sprites) {
                        if (spmap[j] >= 0) {
                            spl[spmap[j]].show_label = 1;
                            spl[spmap[j]].label_w = (float)e.x_advance;
                        }
                        continue;
                    }
                    cairo_new_path(cr);
                    cairo_move_to(cr, lx, ly);
                    cairo_text_path(cr, lab);
                    cairo_set_source_rgba(cr, 0, 0, 0, 0.85);
                    cairo_set_line_width(cr, fs * 0.32);
                    cairo_stroke_preserve(cr);
                    cairo_set_source_rgba(cr, lr, lg, lb, up ? la : la * 0.9);
                    cairo_fill(cr);
                }
            }
        if (sprites)
            pic_sprites_publish(spl, nsp, now, s, fs, pic_config.viewport_gen);
    }
    ov.in_view = in_view;
    ov.visible = visible;
    ov.hidden = hidden;
    ov.cloud = cloud;
    pthread_mutex_unlock(&ov.mutex);
}

static void overhead_note(cairo_t *cr, int width, int height, time_t now,
                          void *ud)
{
    char text[128];
    int n, v, h, cl, total;

    (void)now; (void)ud;
    pthread_mutex_lock(&ov.mutex);
    n = ov.in_view;
    v = ov.visible;
    h = ov.hidden;
    cl = ov.cloud;
    total = ov.n;
    pthread_mutex_unlock(&ov.mutex);
    if (!total) return;
    if (v)
        snprintf(text, sizeof(text),
                 "Overhead now: %d satellite%s in view \xc2\xb7 %d visible from home (aqua)",
                 n, n == 1 ? "" : "s", v);
    else if (h && cl >= 0)
        snprintf(text, sizeof(text),
                 "Overhead now: %d satellite%s in view \xc2\xb7 %d sunlit above home, "
                 "hidden by clouds (%d%%)", n, n == 1 ? "" : "s", h, cl);
    else if (h)
        snprintf(text, sizeof(text),
                 "Overhead now: %d satellite%s in view \xc2\xb7 %d sunlit above home "
                 "(no cloud forecast)", n, n == 1 ? "" : "s", h);
    else
        snprintf(text, sizeof(text), "Overhead now: %d satellite%s in view",
                 n, n == 1 ? "" : "s");
    pic_draw_legend_line(cr, width, height, 0, text, 0.45, 0.95, 0.9);
    {   /* what the trail colours and dashes mean (pic_sat_label_rgb) */
        static const pic_swatch_t key[] = {
            { "Visible from home", 0.30, 1.0, 0.92, 0 },
            { "Sunlit", 0.52, 0.80, 1.0, 0 },
            { "In Earth's shadow", 0.86, 0.52, 0.66, 1 },
        };
        pic_draw_legend_swatches(cr, width, height, "Satellites:", key, 3);
    }
}

static pic_feature_t feature_def = {
    .name = "overhead", .layer = pic_layer_render_overhead,
    .legend = overhead_note,
    .interval = 1, .extra_mb = 1,
    .work = overhead_work, .needs_network = 1,
};

pic_feature_t *pic_feature_overhead(void)
{
    return &feature_def;
}

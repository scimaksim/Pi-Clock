/*
 * layers_overhead.c - Satellites in view right now
 *
 * Every low-orbit satellite in the SatNOGS database (about 1,600,
 * mostly small science and amateur satellites, plus the ISS and other
 * crewed stations), CelesTrak's ~150 brightest satellites when it can
 * be reached, and AMSAT's list, moved live on the map. Meant for the local view, where it shows what is passing
 * over the region at this moment:
 *
 *   yellow  sunlit and above 10° from home while home is dark
 *           (look up: you can see it)
 *   white   sunlit
 *   grey    in Earth's shadow
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
#include "satellite.h"
#include "solar.h"
#include "config.h"
#include "fetch.h"
#include <ctype.h>
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
} ov = { PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0, 0 };

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
    int i;

    if (!fp) return n;
    while (n < max && fgets(line, sizeof(line), fp)) {
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
        memset(&list[n], 0, sizeof(list[n]));
        if (pic_sat_parse_tle(&list[n].sat, l1, line) != 0) { l1[0] = '\0'; continue; }
        list[n].sat.norad_id = atoi(l1 + 2);
        l1[0] = '\0';
        if (list[n].sat.mean_motion < 11.0) continue;      /* not LEO */
        for (i = 0; i < n; i++)
            if (list[i].sat.norad_id == list[n].sat.norad_id) break;
        if (i < n) continue;
        tidy_name(name[0] ? name : "Satellite", list[n].label, sizeof(list[n].label));
        snprintf(list[n].sat.name, sizeof(list[n].sat.name), "%s", list[n].label);
        n++;
        name[0] = '\0';
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
    mkdir(OV_DIR, 0755);
    if (refresh(URL_VISUAL, OV_VISUAL) == 0) got++;
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
    int i, nbox = 0, in_view = 0, visible = 0, home_dark, pass;
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
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    pthread_mutex_lock(&ov.mutex);
    /* One SGP4 step per satellite per frame; keep the ones in view */
    {
        static struct { int i; double lat, lon, el; int lit; } vis[OV_MAX];
        int nv = 0, j;
        for (i = 0; i < ov.n; i++) {
            double lat, lon, alt, dlon;
            pic_sat_position_at(&ov.sats[i].sat, now, &lat, &lon, &alt);
            dlon = fmod(lon - clon + 540.0, 360.0) - 180.0;
            if (fabs(dlon) > span * 0.55 || fabs(lat - clat) > span_lat * 0.55)
                continue;
            vis[nv].i = i; vis[nv].lat = lat; vis[nv].lon = lon;
            vis[nv].lit = 0;
            vis[nv].el = pic_sat_look_angles(&ov.sats[i].sat, now, qlat, qlon,
                                             NULL, &vis[nv].lit);
            nv++;
        }
        /* Pass 0: marks and trails; 1: labels of sats above home's
         * horizon (and named ones); 2: other labels where they fit */
        for (pass = 0; pass < 3; pass++)
            for (j = 0; j < nv; j++) {
                const pic_sat_t *st = &ov.sats[vis[j].i].sat;
                double x, y, r, g, b, a, el = vis[j].el;
                int lit = vis[j].lit, up, k, known;

                up = el > 10;
                x = pic_lon_to_x(vis[j].lon, width);
                y = pic_lat_to_y(vis[j].lat, height);
                if (lit && up && home_dark) { r = 1.0; g = 0.85; b = 0.25; a = 1.0; }
                else if (lit)               { r = 0.95; g = 0.95; b = 0.97; a = 0.9; }
                else                        { r = 0.55; g = 0.58; b = 0.65; a = 0.7; }
                known = pic_sat_kind(st->name) != PIC_SAT_GENERIC;

                if (pass == 0) {
                    double px = x, py = y;
                    in_view++;
                    if (lit && up && home_dark) visible++;
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
                        cairo_set_source_rgba(cr, r, g, b, a * (0.55 - 0.15 * k));
                        cairo_set_line_width(cr, s * (1.4 - 0.3 * k));
                        cairo_stroke(cr);
                        px = x2; py = y2;
                    }
                    if (known) {
                        pic_draw_sat_icon(cr, st->name, x, y, s * 14, 0, r, g, b);
                    } else {
                        cairo_new_path(cr);
                        cairo_arc(cr, x, y, s * (up ? 2.2 : 1.6), 0, 2 * M_PI);
                        cairo_set_source_rgba(cr, r, g, b, a);
                        cairo_fill_preserve(cr);
                        cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
                        cairo_set_line_width(cr, s * 0.5);
                        cairo_stroke(cr);
                    }
                    if (lit && up && home_dark) {   /* ring: look up now */
                        cairo_new_path(cr);
                        cairo_arc(cr, x, y, s * (known ? 9 : 4.2), 0, 2 * M_PI);
                        cairo_set_source_rgba(cr, r, g, b, 0.8);
                        cairo_set_line_width(cr, s * 0.6);
                        cairo_stroke(cr);
                    }
                } else {
                    cairo_text_extents_t e;
                    double lx, ly, bx[4];
                    const char *lab = ov.sats[vis[j].i].label;
                    int q, clash = 0;
                    if ((pass == 1) != (up || known)) continue;
                    if (x < 0 || x > width || y < 0 || y > height) continue;
                    cairo_text_extents(cr, lab, &e);
                    lx = x + s * (known ? 9 : 4);
                    ly = y - s * 2;
                    bx[0] = lx - 2; bx[1] = ly - fs; bx[2] = lx + e.x_advance + 2; bx[3] = ly + fs * 0.35;
                    for (q = 0; q < nbox && !clash; q++)
                        clash = !(bx[2] < boxes[q][0] || bx[0] > boxes[q][2] ||
                                  bx[3] < boxes[q][1] || bx[1] > boxes[q][3]);
                    if (clash || nbox >= OV_MAX) continue;
                    memcpy(boxes[nbox++], bx, sizeof(bx));
                    cairo_new_path(cr);
                    cairo_move_to(cr, lx, ly);
                    cairo_text_path(cr, lab);
                    cairo_set_source_rgba(cr, 0, 0, 0, 0.75);
                    cairo_set_line_width(cr, fs * 0.25);
                    cairo_stroke_preserve(cr);
                    cairo_set_source_rgba(cr, r, g, b, up ? 1.0 : 0.85);
                    cairo_fill(cr);
                }
            }
    }
    ov.in_view = in_view;
    ov.visible = visible;
    pthread_mutex_unlock(&ov.mutex);
}

static void overhead_note(cairo_t *cr, int width, int height, time_t now,
                          void *ud)
{
    char text[128];
    int n, v, total;

    (void)now; (void)ud;
    pthread_mutex_lock(&ov.mutex);
    n = ov.in_view;
    v = ov.visible;
    total = ov.n;
    pthread_mutex_unlock(&ov.mutex);
    if (!total) return;
    if (v)
        snprintf(text, sizeof(text),
                 "Overhead now: %d satellite%s in view \xc2\xb7 %d visible from home (yellow)",
                 n, n == 1 ? "" : "s", v);
    else
        snprintf(text, sizeof(text), "Overhead now: %d satellite%s in view",
                 n, n == 1 ? "" : "s");
    pic_draw_legend_line(cr, width, height, 0, text, 0.95, 0.9, 0.6);
}

static pic_feature_t feature_def = {
    .name = "overhead", .layer = pic_layer_render_overhead,
    .legend = overhead_note,
    .interval = 5, .extra_mb = 1,
    .work = overhead_work, .needs_network = 1,
};

pic_feature_t *pic_feature_overhead(void)
{
    return &feature_def;
}

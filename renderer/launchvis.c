/*
 * launchvis.c - Will a launch be visible from home?
 *
 * East-coast launches (Cape Canaveral, Kennedy, Wallops) can be seen
 * from much of the eastern US: a few minutes after liftoff the rocket
 * is 100-200 km up and rises over the horizon several hundred km away.
 * At night the engine shows as a bright moving star; in twilight the
 * plume, lit by the sun high above a dark sky, can bloom into the
 * "jellyfish".
 *
 * The prediction:
 *  1. Trajectory azimuth from the target orbit: the inclination (ISS
 *     51.6°, Starlink shells 43° / 53°, GPS 55°, Amazon Leo 51.9°,
 *     geostationary transfer ≈ due east, sun-synchronous south) gives
 *     the azimuth through sin(az) = cos(i) / cos(lat). Florida flies the
 *     north-east branch, Wallops the south-east one.
 *  2. A typical ascent to low orbit (altitude and downrange distance
 *     by time, close to a Falcon 9 profile) along that great circle.
 *  3. Every 5 s up to second-stage cut-off: elevation from home, sun
 *     altitude at home (dark enough below -4°), and whether the rocket
 *     is above Earth's shadow (sunlit plume).
 *
 * It is an estimate (real trajectories dog-leg, and the profile varies
 * by rocket and payload), good to a minute or so and a compass point.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "launchvis.h"
#include "astro.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#define R_EARTH 6371.0
#define D2R (M_PI / 180.0)

/* Typical ascent to low orbit: time (s), altitude (km), downrange (km) */
static const double prof[][3] = {
    {   0,   0.0,    0.0 }, {  30,   2.5,    0.3 }, {  60,  10.0,    3.0 },
    {  90,  24.0,   12.0 }, { 120,  43.0,   32.0 }, { 150,  66.0,   70.0 },
    { 180,  88.0,  110.0 }, { 210, 108.0,  150.0 }, { 240, 125.0,  195.0 },
    { 300, 152.0,  320.0 }, { 360, 170.0,  480.0 }, { 420, 182.0,  700.0 },
    { 480, 192.0,  990.0 }, { 540, 200.0, 1380.0 },
};

static int has(const char *s, const char *w)
{
    size_t n = strlen(w), i, j;
    if (!s) return 0;
    for (i = 0; s[i]; i++) {
        for (j = 0; j < n && s[i + j]; j++)
            if (tolower((unsigned char)s[i + j]) != w[j]) break;
        if (j == n) return 1;
    }
    return 0;
}

const char *pic_launchvis_compass(double az)
{
    static const char *p[16] = { "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                 "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW" };
    az = fmod(az + 360.0, 360.0);
    return p[(int)((az + 11.25) / 22.5) % 16];
}

/* ── Learned inclinations ─────────────────────────────────────── */

#define FAM_MAX 64
static struct { char family[48]; double inc; int n; } fam[FAM_MAX];
static int nfam;

void pic_launchvis_family(const char *rocket, const char *mission, char *out, size_t n)
{
    char m[64], r[24];
    size_t i, k = 0, len;
    /* Rocket: first word(s) up to a digit run, e.g. "Falcon 9 Block 5"
     * -> "Falcon 9", "Vulcan VC4S" -> "Vulcan", "Atlas V 551" -> "Atlas V" */
    for (i = 0; rocket && rocket[i] && k < sizeof(r) - 1; i++) {
        if (!strncmp(rocket + i, " Block", 6) || !strncmp(rocket + i, " VC", 3) ||
            (rocket[i] == ' ' && isdigit((unsigned char)rocket[i + 1]) && i > 0 &&
             isdigit((unsigned char)rocket[i - 1])))
            break;
        r[k++] = rocket[i];
    }
    r[k] = '\0';
    /* Mission: drop " (…)" and a trailing "-NN" / " NN" / "-NN-NN" */
    snprintf(m, sizeof(m), "%s", mission ? mission : "");
    if (strchr(m, '(')) *strchr(m, '(') = '\0';
    len = strlen(m);
    while (len > 0 && (m[len - 1] == ' ')) m[--len] = '\0';
    for (;;) {
        size_t l = len;
        while (l > 0 && isdigit((unsigned char)m[l - 1])) l--;
        if (l == len || l == 0 || (m[l - 1] != '-' && m[l - 1] != ' ')) break;
        len = l - 1;
        m[len] = '\0';
    }
    /* Starlink: keep the shell number ("Starlink Group 10") */
    if (!strncasecmp(mission ? mission : "", "Starlink Group ", 15)) {
        snprintf(m, sizeof(m), "Starlink Group %d", atoi(mission + 15));
    }
    snprintf(out, n, "%s|%s", r, m);
}

int pic_launchvis_learned(const char *family, double *inclination)
{
    int i;
    for (i = 0; i < nfam; i++)
        if (!strcmp(fam[i].family, family) && fam[i].n > 0) {
            *inclination = fam[i].inc;
            return 1;
        }
    return 0;
}

void pic_launchvis_learn(const char *family, double inclination, int objects)
{
    int i;
    if (inclination < 0 || inclination > 180 || objects <= 0) return;
    for (i = 0; i < nfam; i++)
        if (!strcmp(fam[i].family, family)) break;
    if (i == nfam) {
        if (nfam == FAM_MAX) return;
        memset(&fam[nfam], 0, sizeof(fam[nfam]));
        snprintf(fam[nfam].family, sizeof(fam[nfam].family), "%s", family);
        nfam++;
    }
    /* Running mean weighted by objects, so a big Starlink batch counts
     * more than one stray piece */
    fam[i].inc = (fam[i].inc * fam[i].n + inclination * objects) / (fam[i].n + objects);
    fam[i].n += objects;
    if (fam[i].n > 1000) fam[i].n = 1000;
}

int pic_launchvis_load(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[128];
    if (!f) return 0;
    nfam = 0;
    while (nfam < FAM_MAX && fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        if (sscanf(tab + 1, "%lf %d", &fam[nfam].inc, &fam[nfam].n) != 2) continue;
        snprintf(fam[nfam].family, sizeof(fam[nfam].family), "%s", line);
        nfam++;
    }
    fclose(f);
    return nfam;
}

int pic_launchvis_save(const char *path)
{
    char tmp[300];
    FILE *f;
    int i;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) return -1;
    for (i = 0; i < nfam; i++) fprintf(f, "%s\t%.3f %d\n", fam[i].family, fam[i].inc, fam[i].n);
    fclose(f);
    return rename(tmp, path);
}

/* Target inclination (degrees), or -1 (due east) / -2 (polar, south) */
static double target_inclination(const char *orbit, const char *mission, const char *rocket)
{
    const char *g;
    char family[64];
    double learned;
    pic_launchvis_family(rocket, mission, family, sizeof(family));
    if (pic_launchvis_learned(family, &learned)) {
        /* A geostationary transfer or polar orbit: the launch direction,
         * not the inclination, is what the heuristics encode */
        if (learned > 85) return -2;
        return learned;
    }
    if (has(mission, "crew") || has(mission, "crs-") || has(mission, "spx-") ||
        has(mission, "cygnus") || has(mission, "axiom") || has(mission, "ax-") ||
        has(mission, "dragon") || has(mission, "starliner") || has(mission, "dream chaser") ||
        has(mission, "iss") || has(orbit, "iss"))
        return 51.6;
    if (has(orbit, "gto") || has(orbit, "geo") || has(orbit, "gso") || has(orbit, "heo") ||
        has(orbit, "lunar") || has(orbit, "tli") || has(orbit, "helio") ||
        has(orbit, "l1") || has(orbit, "l2") || has(orbit, "mars") || has(orbit, "deep") ||
        has(orbit, "escape"))
        return -1;
    if (has(orbit, "sso") || has(orbit, "po") || has(orbit, "polar"))
        return -2;
    if (has(mission, "gps") || has(orbit, "meo")) return 55.0;
    if ((g = strstr(mission ? mission : "", "Group ")) != NULL) {
        int shell = atoi(g + 6);              /* Starlink Group 6-12 */
        return (shell == 6 || shell == 12) ? 43.0 : 53.0;
    }
    if (has(mission, "starlink")) return 43.0;
    if (has(mission, "amazon leo") || has(mission, "kuiper")) return 51.9;
    if (has(mission, "transporter") || has(mission, "bandwagon")) return 45.0;
    return 45.0;                              /* other low orbit: a guess */
}

int pic_launchvis_azimuth(double lat, double lon, const char *orbit,
                          const char *mission, const char *rocket, double *az)
{
    double inc = target_inclination(orbit, mission, rocket), s;
    (void)lon;
    if (inc == -1) { *az = 95.0; return 1; }                   /* due east */
    if (inc == -2) { *az = lat > 33 ? 175.0 : 160.0; return 1; } /* south  */
    if (inc <= fabs(lat) + 0.5) { *az = 90.0; return 1; }
    s = cos(inc * D2R) / cos(lat * D2R);
    if (s > 1) s = 1;
    *az = asin(s) / D2R;
    /* Wallops flies the south-east branch, over the ocean */
    if (lat > 33) *az = 180.0 - *az;
    return 1;
}

void pic_launchvis_position(double lat, double lon, double az, double t,
                            double *rlat, double *rlon, double *alt_km)
{
    int n = (int)(sizeof(prof) / sizeof(prof[0])), i;
    double alt, dr, f, d, la = lat * D2R, lo = lon * D2R, b = az * D2R, la2;

    if (t <= 0) { alt = 0; dr = 0; }
    else if (t >= prof[n - 1][0]) { alt = prof[n - 1][1]; dr = prof[n - 1][2]; }
    else {
        for (i = 1; i < n && prof[i][0] < t; i++) ;
        f = (t - prof[i - 1][0]) / (prof[i][0] - prof[i - 1][0]);
        alt = prof[i - 1][1] + f * (prof[i][1] - prof[i - 1][1]);
        dr = prof[i - 1][2] + f * (prof[i][2] - prof[i - 1][2]);
    }
    d = dr / R_EARTH;
    la2 = asin(sin(la) * cos(d) + cos(la) * sin(d) * cos(b));
    *rlat = la2 / D2R;
    *rlon = (lo + atan2(sin(b) * sin(d) * cos(la), cos(d) - sin(la) * sin(la2))) / D2R;
    *alt_km = alt;
}

double pic_launchvis_look(double hlat, double hlon, double rlat, double rlon,
                          double alt_km, double *az)
{
    double hl = hlat * D2R, hn = hlon * D2R, rl = rlat * D2R, rn = rlon * D2R;
    double r = R_EARTH + alt_km, p[3], o[3], v[3], e, n, u, d;

    p[0] = r * cos(rl) * cos(rn); p[1] = r * cos(rl) * sin(rn); p[2] = r * sin(rl);
    o[0] = R_EARTH * cos(hl) * cos(hn); o[1] = R_EARTH * cos(hl) * sin(hn);
    o[2] = R_EARTH * sin(hl);
    v[0] = p[0] - o[0]; v[1] = p[1] - o[1]; v[2] = p[2] - o[2];
    d = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    e = -sin(hn) * v[0] + cos(hn) * v[1];
    n = -sin(hl) * cos(hn) * v[0] - sin(hl) * sin(hn) * v[1] + cos(hl) * v[2];
    u = cos(hl) * cos(hn) * v[0] + cos(hl) * sin(hn) * v[1] + sin(hl) * v[2];
    if (az) *az = fmod(atan2(e, n) / D2R + 360.0, 360.0);
    return d > 0 ? asin(u / d) / D2R : 90.0;
}

int pic_launchvis_predict(double lat, double lon, const char *orbit,
                          const char *mission, const char *rocket,
                          time_t net, double hlat, double hlon,
                          pic_launchvis_t *out)
{
    int t, in = 0, lit_dark = 0, n_vis = 0;

    memset(out, 0, sizeof(*out));
    out->t_start = out->t_end = -1;
    if (!pic_launchvis_azimuth(lat, lon, orbit, mission, rocket, &out->az_launch))
        return 0;
    out->known = 1;
    for (t = 30; t <= PIC_LV_T_END; t += 5) {
        double rl, rn, h, az, el, sun_home, sun_r, dip;
        pic_launchvis_position(lat, lon, out->az_launch, t, &rl, &rn, &h);
        el = pic_launchvis_look(hlat, hlon, rl, rn, h, &az);
        if (el < PIC_LV_MIN_EL) { in = 0; continue; }
        sun_home = pic_astro_sun_alt(net + t, hlat, hlon);
        if (sun_home > -4.0) { out->daytime = 1; in = 0; continue; }
        /* Above Earth's shadow: the sun is less far below the horizon
         * under the rocket than the dip of the horizon from its height */
        sun_r = pic_astro_sun_alt(net + t, rl, rn);
        dip = acos(R_EARTH / (R_EARTH + h)) / D2R;
        if (sun_r > -dip) lit_dark = 1;
        if (!in && out->t_start < 0) { out->t_start = t; out->az_start = az; }
        if (out->t_start >= 0 && (in || t == out->t_start)) {
            out->t_end = t;
            out->az_end = az;
        }
        in = 1;
        n_vis++;
        if (el > out->max_el) out->max_el = el;
    }
    out->visible = n_vis >= 4;            /* at least ~20 s above the trees */
    out->sunlit = out->visible && lit_dark;
    if (out->visible) out->daytime = 0;
    return 1;
}

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

/* Target inclination (degrees), or -1 (due east) / -2 (polar, south) */
static double target_inclination(const char *orbit, const char *mission, const char *rocket)
{
    const char *g;
    (void)rocket;
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

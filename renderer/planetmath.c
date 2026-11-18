/*
 * planetmath.c - Geometry and time for the other-world geochrons
 *
 *   - Horizons observer tables -> sub-points in each map's conventions
 *   - shadows of moons on their planet (ray from the Sun past the moon
 *     meeting the planet's ellipsoid)
 *   - Jupiter System II -> System III longitudes (IAU rotation models)
 *   - Mars time: Mars Sol Date, Coordinated Mars Time and the solar
 *     longitude Ls, after Allison & McEwen (2000) / NASA GISS Mars24
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "planetview.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define D2R (M_PI / 180.0)

static double wrap180(double x)
{
    x = fmod(x + 180.0, 360.0);
    if (x < 0) x += 360.0;
    return x - 180.0;
}

/* planetographic -> planetocentric latitude for flattening f */
static double centric_lat(double lat, double f)
{
    double k = (1.0 - f) * (1.0 - f);
    if (fabs(lat) >= 89.999) return lat;
    return atan(k * tan(lat * D2R)) / D2R;
}

static const char *MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";

int pic_horizons_parse(const char *txt, int west, double f, int centric,
                       pic_subpt_t *obs, pic_subpt_t *sun, int max, double au_km)
{
    const char *p = txt ? strstr(txt, "$$SOE") : NULL, *end;
    int n = 0;
    if (!p) return 0;
    end = strstr(p, "$$EOE");
    p = strchr(p, '\n');
    while (p && *p && (!end || p < end) && n < max) {
        char line[512], mon[4] = "";
        const char *nl = strchr(p + 1, '\n');
        size_t len;
        int y, d, hh, mm, fi = 0;
        double v[8];
        char *tok, *sp, *mp;
        struct tm tm;

        p++;
        len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p = nl;
        if (strncmp(line, "$$EOE", 5) == 0) break;
        if (sscanf(line, " %d-%3s-%d %d:%d", &y, mon, &d, &hh, &mm) != 5) continue;
        mp = strstr(MONTHS, mon);
        if (!mp || mon[0] == 0) continue;
        memset(&tm, 0, sizeof(tm));
        tm.tm_year = y - 1900;
        tm.tm_mon = (int)(mp - MONTHS) / 3;
        tm.tm_mday = d;
        tm.tm_hour = hh;
        tm.tm_min = mm;
        /* fields after the date: blank solar/lunar presence flags, then numbers */
        tok = strtok_r(line, ",", &sp);
        while ((tok = strtok_r(NULL, ",", &sp)) != NULL && fi < 8) {
            char *e;
            double x = strtod(tok, &e);
            if (e != tok) v[fi++] = x;
        }
        /* obs lon, obs lat, [sun lon, sun lat,] range(au), ... */
        if (fi < (sun ? 5 : 3)) continue;
        {
            double t = (double)timegm(&tm);
            double lon = v[0], lat = v[1], r = sun ? v[4] : v[2];
            if (west) lon = -lon;
            obs[n].t = t;
            obs[n].lon = wrap180(lon);
            obs[n].lat = centric ? centric_lat(lat, f) : lat;
            obs[n].r = r * au_km;
            if (sun) {
                double sl = west ? -v[2] : v[2];
                sun[n].t = t;
                sun[n].lon = wrap180(sl);
                sun[n].lat = centric ? centric_lat(v[3], f) : v[3];
                sun[n].r = 0;
            }
        }
        n++;
    }
    return n;
}

int pic_subpt_at(const pic_subpt_t *s, int n, double t, pic_subpt_t *out)
{
    int i;
    for (i = 0; i + 1 < n; i++)
        if (s[i].t <= t && t <= s[i + 1].t) {
            double f = (s[i + 1].t > s[i].t) ? (t - s[i].t) / (s[i + 1].t - s[i].t) : 0;
            double dl = wrap180(s[i + 1].lon - s[i].lon);
            out->t = t;
            out->lon = wrap180(s[i].lon + f * dl);
            out->lat = s[i].lat + f * (s[i + 1].lat - s[i].lat);
            out->r = s[i].r + f * (s[i + 1].r - s[i].r);
            return 1;
        }
    return 0;
}

int pic_shadow_point(double lon, double lat, double r, double slon, double slat,
                     double a, double b, double *olon, double *olat)
{
    double M[3], S[3], A, B, C, disc, s, P[3];
    double cl = cos(lat * D2R), cs = cos(slat * D2R);
    M[0] = r * cl * cos(lon * D2R);
    M[1] = r * cl * sin(lon * D2R);
    M[2] = r * sin(lat * D2R);
    S[0] = cs * cos(slon * D2R);
    S[1] = cs * sin(slon * D2R);
    S[2] = sin(slat * D2R);
    /* P(s) = M - s S on x^2/a^2 + y^2/a^2 + z^2/b^2 = 1, nearest s > 0 */
    A = (S[0] * S[0] + S[1] * S[1]) / (a * a) + S[2] * S[2] / (b * b);
    B = -2.0 * ((M[0] * S[0] + M[1] * S[1]) / (a * a) + M[2] * S[2] / (b * b));
    C = (M[0] * M[0] + M[1] * M[1]) / (a * a) + M[2] * M[2] / (b * b) - 1.0;
    disc = B * B - 4 * A * C;
    if (disc < 0 || C < 0) return 0;           /* misses, or the body is inside */
    s = (-B - sqrt(disc)) / (2 * A);
    if (s <= 0) return 0;                       /* the planet is sunward of it */
    P[0] = M[0] - s * S[0];
    P[1] = M[1] - s * S[1];
    P[2] = M[2] - s * S[2];
    *olon = atan2(P[1], P[0]) / D2R;
    *olat = atan2(P[2], sqrt(P[0] * P[0] + P[1] * P[1])) / D2R;
    return 1;
}

/* Days from J2000.0 (TT) for a UTC unix time */
static double tt_offset(double t)
{
    /* TT - UTC = 32.184 s + TAI-UTC (leap seconds) */
    static const struct { double t; int ls; } L[] = {
        { 915148800, 32 }, { 1136073600, 33 }, { 1230768000, 34 },
        { 1341100800, 35 }, { 1435708800, 36 }, { 1483228800, 37 },
    };
    int i, ls = 32;
    for (i = 0; i < (int)(sizeof(L) / sizeof(L[0])); i++)
        if (t >= L[i].t) ls = L[i].ls;
    return 32.184 + ls;
}

static double jd_tt(double t)
{
    return (t + tt_offset(t)) / 86400.0 + 2440587.5;
}

double pic_jupiter_sys2_to_sys3(double l2, double t)
{
    /* IAU: System II W = 43.3 + 870.270 d, System III W = 284.95 + 870.536 d
     * (d days from J2000 TDB). A point fixed in System II has west
     * longitude l3 = l2 + W3 - W2 in System III. */
    double d = jd_tt(t) - 2451545.0;
    double l3 = l2 + (284.95 - 43.3) + (870.536 - 870.270) * d;
    l3 = fmod(l3, 360.0);
    return l3 < 0 ? l3 + 360.0 : l3;
}

double pic_mars_msd(double t)
{
    /* Mars24 (NASA GISS), eq. C-2 */
    return (jd_tt(t) - 2451549.5) / 1.0274912517 + 44796.0 - 0.0009626;
}

double pic_mars_mtc(double t)
{
    double m = pic_mars_msd(t);
    return 24.0 * (m - floor(m));
}

double pic_mars_ls(double t)
{
    static const double A[7] = { 0.0071, 0.0057, 0.0039, 0.0037, 0.0021, 0.0020, 0.0018 };
    static const double TAU[7] = { 2.2353, 2.7543, 1.1177, 15.7866, 2.1354, 2.4694, 32.8493 };
    static const double PHI[7] = { 49.409, 168.173, 191.837, 21.736, 15.704, 95.528, 49.095 };
    double dt = jd_tt(t) - 2451545.0, M, a, pbs = 0, v, ls;
    int i;
    M = (19.3871 + 0.52402073 * dt) * D2R;
    a = 270.3871 + 0.524038496 * dt;
    for (i = 0; i < 7; i++)
        pbs += A[i] * cos((0.985626 * dt / TAU[i] + PHI[i]) * D2R);
    v = (10.691 + 3.0e-7 * dt) * sin(M) + 0.623 * sin(2 * M) + 0.050 * sin(3 * M) +
        0.005 * sin(4 * M) + 0.0005 * sin(5 * M) + pbs;
    ls = fmod(a + v, 360.0);
    return ls < 0 ? ls + 360.0 : ls;
}

/* Mars equation of time, degrees (true minus mean sun), Allison &
 * McEwen 2000 (Mars24): up to about +10 / -13 degrees over the year. */
double pic_mars_eot(double t)
{
    double dt = jd_tt(t) - 2451545.0;
    double ls = pic_mars_ls(t) * D2R;
    double afms = fmod(270.3871 + 0.524038496 * dt, 360.0);
    double vm = pic_mars_ls(t) - (afms < 0 ? afms + 360.0 : afms);      /* equation of centre */
    while (vm > 180) vm -= 360;
    while (vm < -180) vm += 360;
    return 2.861 * sin(2 * ls) - 0.071 * sin(4 * ls) + 0.035 * sin(6 * ls) - vm;
}

/*
 * eclipse.c - Solar and lunar eclipse geometry
 *
 * Solar: the classical Besselian method (Explanatory Supplement to the
 * Astronomical Almanac, ch. 11). For an observer at geodetic latitude
 * phi and longitude lambda (east +), with H = mu + lambda:
 *   xi   = rho cos phi' sin H
 *   eta  = rho sin phi' cos d - rho cos phi' cos H sin d
 *   zeta = rho sin phi' sin d + rho cos phi' cos H cos d
 *   L1'  = l1 - zeta tan f1,  L2' = l2 - zeta tan f2
 *   m    = |(x - xi, y - eta)|
 * The observer is in the penumbra when m < L1', in the umbra (total)
 * when m < -L2' (L2' < 0), in the antumbra (annular) when m < L2'.
 * mu is built from apparent sidereal time at UT, so no Delta-T term.
 *
 * Checked against published circumstances (tests/test_eclipse.c):
 * greatest eclipse positions within 0.02 deg, central durations within
 * 1-2 s, Raleigh's 2024 partial (78%).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "eclipse.h"
#include <math.h>
#include <stddef.h>

#define D2R (M_PI / 180.0)
#define FLAT (1.0 / 298.257223563)
#define BA (1.0 - FLAT)                 /* polar / equatorial radius */
#define E2 (FLAT * (2.0 - FLAT))

static double poly(const double *c, int n, double h)
{
    double v = 0;
    while (n-- > 0) v = v * h + c[n];
    return v;
}

const pic_ecl_solar_t *pic_ecl_solar_at(time_t t)
{
    int i;
    for (i = 0; i < pic_ecl_nsolar; i++)
        if (t >= pic_ecl_solar[i].p1 && t <= pic_ecl_solar[i].p4) return &pic_ecl_solar[i];
    return NULL;
}

const pic_ecl_solar_t *pic_ecl_solar_next(time_t t)
{
    int i;
    for (i = 0; i < pic_ecl_nsolar; i++)
        if (pic_ecl_solar[i].p4 >= t) return &pic_ecl_solar[i];
    return NULL;
}

const pic_ecl_solar_t *pic_ecl_solar_prev(time_t t)
{
    int i;
    for (i = pic_ecl_nsolar - 1; i >= 0; i--)
        if (pic_ecl_solar[i].p4 < t) return &pic_ecl_solar[i];
    return NULL;
}

/* lunar: from first penumbral contact to last */
const pic_ecl_lunar_t *pic_ecl_lunar_at(time_t t)
{
    int i;
    for (i = 0; i < pic_ecl_nlunar; i++)
        if (pic_ecl_lunar[i].c[0] && t >= pic_ecl_lunar[i].c[0] && t <= pic_ecl_lunar[i].c[1])
            return &pic_ecl_lunar[i];
    return NULL;
}

const pic_ecl_lunar_t *pic_ecl_lunar_next(time_t t)
{
    int i;
    for (i = 0; i < pic_ecl_nlunar; i++)
        if (pic_ecl_lunar[i].c[1] >= t) return &pic_ecl_lunar[i];
    return NULL;
}

const pic_ecl_lunar_t *pic_ecl_lunar_prev(time_t t)
{
    int i;
    for (i = pic_ecl_nlunar - 1; i >= 0; i--)
        if (pic_ecl_lunar[i].c[1] && pic_ecl_lunar[i].c[1] < t) return &pic_ecl_lunar[i];
    return NULL;
}

void pic_ecl_solar_prep(const pic_ecl_solar_t *e, double t, pic_ecl_state_t *s)
{
    double h = (t - (double)e->t0) / 3600.0, d = poly(e->d, 3, h) * D2R;
    s->x = poly(e->x, 4, h);
    s->y = poly(e->y, 4, h);
    s->sd = sin(d);
    s->cd = cos(d);
    s->mu = poly(e->mu, 2, h);
    s->l1 = poly(e->l1, 3, h);
    s->l2 = poly(e->l2, 3, h);
    s->tf1 = e->tf1;
    s->tf2 = e->tf2;
}

void pic_ecl_geocentric(double lat, double *rs, double *rc)
{
    double u = atan(BA * tan(lat * D2R));
    *rs = BA * sin(u);
    *rc = cos(u);
}

double pic_ecl_overlap(double r, double s)
{
    double a1, a2, k;
    if (s >= 1.0 + r) return 0.0;
    if (s <= fabs(1.0 - r)) return r < 1.0 ? r * r : 1.0;
    a1 = acos((s * s + 1.0 - r * r) / (2.0 * s));
    a2 = acos((s * s + r * r - 1.0) / (2.0 * s * r));
    k = (-s + 1.0 + r) * (s + 1.0 - r) * (s - 1.0 + r) * (s + 1.0 + r);
    return (a1 + r * r * a2 - 0.5 * sqrt(k > 0 ? k : 0)) / M_PI;
}

/* Core: fills the local view from precomputed state */
static void local_core(const pic_ecl_state_t *s, double rs, double rc, double lon,
                       pic_ecl_local_t *o)
{
    double H = (s->mu + lon) * D2R, sh = sin(H), ch = cos(H);
    double xi = rc * sh;
    double eta = rs * s->cd - rc * ch * s->sd;
    double zeta = rs * s->sd + rc * ch * s->cd;
    double L1 = s->l1 - zeta * s->tf1, L2 = s->l2 - zeta * s->tf2;
    double u = s->x - xi, v = s->y - eta, m = sqrt(u * u + v * v);
    double sr = (L1 + L2) / 2.0;                 /* solar radius on the plane */

    o->sun_up = zeta > 0;
    o->moon_r = (L1 - L2) / (L1 + L2);
    o->central = m < fabs(L2) ? (L2 < 0 ? 1 : 2) : 0;
    o->magnitude = m < L1 ? (L1 - m) / (L1 + L2) : 0.0;
    o->obscuration = m < L1 ? pic_ecl_overlap(o->moon_r, m / sr) : 0.0;
    /* the Moon sits on the shadow-axis side of the Sun: +u east */
    o->dx = sr > 0 ? u / sr : 0;
    o->dy = sr > 0 ? v / sr : 0;
    if (!o->sun_up) {
        o->obscuration = o->magnitude = 0;
        o->central = 0;
    }
}

void pic_ecl_solar_local(const pic_ecl_solar_t *e, double t, double lat, double lon,
                         pic_ecl_local_t *out)
{
    pic_ecl_state_t s;
    double rs, rc;
    pic_ecl_solar_prep(e, t, &s);
    pic_ecl_geocentric(lat, &rs, &rc);
    local_core(&s, rs, rc, lon, out);
}

double pic_ecl_obscuration_fast(const pic_ecl_state_t *s, double rs, double rc,
                                double lon, int *central)
{
    pic_ecl_local_t o;
    local_core(s, rs, rc, lon, &o);
    if (central) *central = o.central;
    return o.obscuration;
}

int pic_ecl_solar_centre(const pic_ecl_solar_t *e, double t, double *lat, double *lon)
{
    pic_ecl_state_t s;
    double rho1, y1, sd1, cd1, B, sp1, th, phi1, l;
    pic_ecl_solar_prep(e, t, &s);
    rho1 = sqrt(1.0 - E2 * s.cd * s.cd);
    y1 = s.y / rho1;
    sd1 = s.sd / rho1;
    cd1 = sqrt(1.0 - E2) * s.cd / rho1;
    B = 1.0 - s.x * s.x - y1 * y1;
    if (B < 0) return 0;
    B = sqrt(B);
    sp1 = B * sd1 + y1 * cd1;
    th = atan2(s.x, B * cd1 - y1 * sd1) / D2R;
    phi1 = asin(sp1 > 1 ? 1 : sp1 < -1 ? -1 : sp1);
    *lat = atan(tan(phi1) / BA) / D2R;
    l = fmod(th - s.mu + 540.0, 360.0);
    if (l < 0) l += 360.0;
    *lon = l - 180.0;
    return 1;
}

void pic_ecl_lunar_state(const pic_ecl_lunar_t *e, double t, pic_ecl_lunar_state_t *s)
{
    double h = (t - (double)e->t0) / 3600.0, dist, l;
    s->x = poly(e->x, 4, h);
    s->y = poly(e->y, 4, h);
    s->ru = poly(e->ru, 3, h);
    s->rp = poly(e->rp, 3, h);
    s->sm = poly(e->sm, 3, h);
    s->sublat = poly(e->dec, 3, h);
    l = fmod(-poly(e->gha, 2, h) + 540.0, 360.0);
    if (l < 0) l += 360.0;
    s->sublon = l - 180.0;
    dist = sqrt(s->x * s->x + s->y * s->y);
    s->umbral = (s->ru + s->sm - dist) / (2.0 * s->sm);
    s->penumbral = (s->rp + s->sm - dist) / (2.0 * s->sm);
}

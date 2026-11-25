/*
 * eclipse.h - Solar and lunar eclipses, 2024-2040
 *
 * Solar eclipses come as Besselian elements (the Moon's shadow axis on
 * the fundamental plane), lunar ones as the Moon's place relative to
 * Earth's shadow, both generated from JPL DE421 (tools/eclgen) and fitted
 * as polynomials in UT hours from t0 (UT).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PIC_ECLIPSE_H
#define PIC_ECLIPSE_H

#include <time.h>

typedef struct {
    const char *date;          /* YYYY-MM-DD (UT, greatest eclipse)        */
    char   type;               /* 'T' total, 'A' annular, 'H' hybrid, 'P'  */
    long long t0, tmax;        /* polynomial epoch; greatest eclipse (UT) */
    long long p1, p4;          /* penumbra first / last touches the Earth  */
    int    central;
    double gamma;
    double x[4], y[4];         /* fundamental-plane shadow axis (Earth radii) */
    double d[3];               /* declination of the axis (deg)            */
    double mu[2];              /* its Greenwich hour angle (deg)           */
    double l1[3], l2[3];       /* penumbra / umbra radius on the plane    */
    double tf1, tf2;           /* tan of the cone half-angles              */
    long long c1, c2;          /* the axis meets the Earth (central path)  */
} pic_ecl_solar_t;

typedef struct {
    const char *date;
    int    kind;               /* 0 penumbral, 1 partial, 2 total          */
    double umag;               /* umbral magnitude at greatest eclipse     */
    long long t0, tmax;
    long long c[6];            /* P1 P4 U1 U4 U2 U3 (0 when not reached)    */
    double x[4], y[4];         /* Moon minus shadow centre on the sky (deg) */
    double ru[3], rp[3], sm[3];/* umbra, penumbra, Moon radii (deg)         */
    double dec[3], gha[2];     /* the Moon's declination and GHA (deg)     */
} pic_ecl_lunar_t;

extern const pic_ecl_solar_t pic_ecl_solar[];
extern const int pic_ecl_nsolar;
extern const pic_ecl_lunar_t pic_ecl_lunar[];
extern const int pic_ecl_nlunar;

/* The eclipse under way at t (NULL if none), or, with the _next forms,
 * the first one ending after t */
const pic_ecl_solar_t *pic_ecl_solar_at(time_t t);
const pic_ecl_lunar_t *pic_ecl_lunar_at(time_t t);
const pic_ecl_solar_t *pic_ecl_solar_next(time_t t);
const pic_ecl_lunar_t *pic_ecl_lunar_next(time_t t);
/* The last one that ended before t */
const pic_ecl_solar_t *pic_ecl_solar_prev(time_t t);
const pic_ecl_lunar_t *pic_ecl_lunar_prev(time_t t);

/* What an observer sees of a solar eclipse */
typedef struct {
    double obscuration;   /* share of the Sun's disc covered, 0..1          */
    double magnitude;     /* share of its diameter covered                  */
    int    central;       /* 1 inside the umbra (total), 2 antumbra (annular) */
    int    sun_up;        /* Sun above the (geometric) horizon               */
    double dx, dy;        /* Moon centre from Sun centre, in solar radii;   */
                          /* +x east (sky left), +y north                    */
    double moon_r;        /* Moon radius in solar radii                      */
} pic_ecl_local_t;

/* at time t (seconds UT, fractional ok), latitude/longitude in degrees */
void pic_ecl_solar_local(const pic_ecl_solar_t *e, double t, double lat, double lon,
                         pic_ecl_local_t *out);

/* Fast form for grids: rho sin phi', rho cos phi' of the latitude, and
 * the elements at t (pic_ecl_solar_prep) */
typedef struct {
    double x, y, sd, cd, mu, l1, l2, tf1, tf2;
} pic_ecl_state_t;
void pic_ecl_solar_prep(const pic_ecl_solar_t *e, double t, pic_ecl_state_t *s);
void pic_ecl_geocentric(double lat, double *rs, double *rc);
double pic_ecl_obscuration_fast(const pic_ecl_state_t *s, double rs, double rc,
                                double lon, int *central);

/* Where the shadow axis meets the Earth at t; 0 if it misses */
int pic_ecl_solar_centre(const pic_ecl_solar_t *e, double t, double *lat, double *lon);

/* Lunar eclipse at t: Moon offset from the shadow centre (deg, +x east),
 * shadow and Moon radii, and the sub-lunar point */
typedef struct {
    double x, y, ru, rp, sm, sublat, sublon;
    double umbral;        /* share of the Moon's diameter inside the umbra  */
    double penumbral;     /* ... inside the penumbra                        */
} pic_ecl_lunar_state_t;
void pic_ecl_lunar_state(const pic_ecl_lunar_t *e, double t, pic_ecl_lunar_state_t *s);

/* Disc-overlap area of a Moon of radius r (Sun radius 1) at separation s */
double pic_ecl_overlap(double r, double s);

#endif /* PIC_ECLIPSE_H */

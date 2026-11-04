/*
 * satellite.c - Satellite tracking: TLE fetch + SGP4 propagation
 *
 * Downloads TLE data from Celestrak for registered satellites,
 * parses the two-line element sets, and propagates orbits with
 * SGP4 (near-Earth form, WGS-72 constants as the elements are made
 * with) to compute lat/lon/alt on the WGS-84 ellipsoid.
 *
 * Orbits with a period of 225 minutes or more (GPS, geostationary)
 * would need the deep-space SDP4 terms; those fall back to a secular
 * J2 model, which is fine for satellites that barely move on the map.
 *
 * References:
 *   - Hoots & Roehrich, "Spacetrack Report No. 3" (1980)
 *   - Vallado et al., "Revisiting Spacetrack Report #3" (2006)
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "satellite.h"
#include "config.h"
#include "fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#include "math_utils.h"
#define TWO_PI (2.0 * M_PI)

/* WGS84 Earth constants */
#define EARTH_R_KM  6378.137       /* Equatorial radius (km) */
#define EARTH_MU    398600.4418    /* Gravitational parameter (km^3/s^2) */
#define J2          0.00108263     /* Second zonal harmonic */
#define MINUTES_PER_DAY  1440.0

/* TLE fetch interval (1 hour) */
#define TLE_FETCH_INTERVAL 3600

static void sgp4_init(pic_sat_t *s);

static pthread_t sat_thread;
static volatile int sat_running = 0;
static pic_satlist_t *sat_list;

/*
 * parse_tle - Parse a standard two-line element set.
 *
 * Line 1: 1 NNNNNC YYYYY.DDDDDDDD  ...  BSTAR ...
 * Line 2: 2 NNNNN INC RAAN ECC ARGP MA MM ...
 */
static int parse_tle(pic_sat_t *sat, const char *line1, const char *line2)
{
    double epoch_yr, epoch_day;
    double inc, raan, ecc, argp, ma, mm;
    char ecc_str[8];
    int yr;

    if (strlen(line1) < 69 || strlen(line2) < 69) return -1;
    if (line1[0] != '1' || line2[0] != '2') return -1;

    /* Line 1: epoch and B* */
    {
        char yr_str[3], day_str[13], bstar_str[12];
        int bstar_exp;

        memcpy(yr_str, line1 + 18, 2); yr_str[2] = '\0';
        yr = atoi(yr_str);
        /* TLE epoch year: 2-digit. NORAD convention: < 57 = 21st century,
         * >= 57 = 20th century. Based on Sputnik launch year (1957). */
        epoch_yr = (yr < 57) ? 2000 + yr : 1900 + yr;

        memcpy(day_str, line1 + 20, 12); day_str[12] = '\0';
        epoch_day = atof(day_str);

        /* B* drag term: mantissa in cols 54-59, exponent in 60-61 */
        memcpy(bstar_str, line1 + 53, 6); bstar_str[6] = '\0';
        bstar_exp = atoi(line1 + 59);
        sat->bstar = atof(bstar_str) * 1e-5 * pow(10.0, bstar_exp);
    }

    /* Convert epoch to Julian date */
    {
        /* Jan 1 of epoch year as JD, then add day-of-year */
        int y = (int)epoch_yr;
        double jd_jan1 = 367.0 * y
            - (int)(7.0 * (y + (int)(10.0 / 12.0)) / 4.0)
            + (int)(275.0 / 9.0)
            + 1721013.5;
        sat->epoch_jd = jd_jan1 + epoch_day;
    }

    /* Line 2: orbital elements */
    {
        char buf[16];

        memcpy(buf, line2 + 8, 8); buf[8] = '\0';
        inc = atof(buf);

        memcpy(buf, line2 + 17, 8); buf[8] = '\0';
        raan = atof(buf);

        /* Eccentricity: implied leading "0." */
        memcpy(ecc_str, line2 + 26, 7); ecc_str[7] = '\0';
        ecc = atof(ecc_str) * 1e-7;

        memcpy(buf, line2 + 34, 8); buf[8] = '\0';
        argp = atof(buf);

        memcpy(buf, line2 + 43, 8); buf[8] = '\0';
        ma = atof(buf);

        memcpy(buf, line2 + 52, 11); buf[11] = '\0';
        mm = atof(buf);
    }

    sat->inclination = inc * DEG_TO_RAD;
    sat->raan = raan * DEG_TO_RAD;
    sat->eccentricity = ecc;
    sat->arg_perigee = argp * DEG_TO_RAD;
    sat->mean_anomaly = ma * DEG_TO_RAD;
    sat->mean_motion = mm;  /* revs/day */
    sat->tle_loaded = 1;
    sgp4_init(sat);

    return 0;
}

/* ── SGP4, near-Earth (Hoots & Roehrich 1980; Vallado et al. 2006) ── */

/* WGS-72: the constants TLEs are generated with */
#define SG_RE     6378.135
#define SG_MU     398600.8
#define SG_J2     0.001082616
#define SG_J3    -0.00000253881
#define SG_J4    -0.00000165597

static double sg_xke(void) { return 60.0 / sqrt(SG_RE * SG_RE * SG_RE / SG_MU); }

/* Precompute the SGP4 constants for a parsed element set */
static void sgp4_init(pic_sat_t *s)
{
    const double x2o3 = 2.0 / 3.0, j3oj2 = SG_J3 / SG_J2, xke = sg_xke();
    double ecco = s->eccentricity, inclo = s->inclination, argpo = s->arg_perigee;
    double no_k = s->mean_motion * TWO_PI / MINUTES_PER_DAY, bstar = s->bstar;
    double eccsq, omeosq, rteosq, cosio, cosio2, ak, d1, del, adel, ao, sinio, po,
           con42, posq, rp, sfour, qzms24, perige, pinvsq, tsi, eta, etasq,
           eeta, psisq, coef, coef1, cc2, cc3, cosio4, temp1, temp2, temp3, xhdot1;

    s->sg_ok = 0;
    if (s->mean_motion <= 0 || ecco >= 1.0) return;
    if (MINUTES_PER_DAY / s->mean_motion >= 225.0) return;   /* deep space */

    eccsq = ecco * ecco;
    omeosq = 1.0 - eccsq;
    rteosq = sqrt(omeosq);
    cosio = cos(inclo);
    cosio2 = cosio * cosio;
    /* Un-Kozai the mean motion */
    ak = pow(xke / no_k, x2o3);
    d1 = 0.75 * SG_J2 * (3.0 * cosio2 - 1.0) / (rteosq * omeosq);
    del = d1 / (ak * ak);
    adel = ak * (1.0 - del * del - del * (1.0 / 3.0 + 134.0 * del * del / 81.0));
    del = d1 / (adel * adel);
    s->sg_no = no_k / (1.0 + del);
    ao = pow(xke / s->sg_no, x2o3);
    s->sg_ao = ao;
    sinio = sin(inclo);
    po = ao * omeosq;
    con42 = 1.0 - 5.0 * cosio2;
    s->sg_con41 = -con42 - cosio2 - cosio2;
    posq = po * po;
    rp = ao * (1.0 - ecco);
    s->sg_isimp = rp < (220.0 / SG_RE + 1.0);

    /* Atmospheric density parameters, lowered for low perigees */
    sfour = 78.0 / SG_RE + 1.0;
    qzms24 = pow((120.0 - 78.0) / SG_RE, 4);
    perige = (rp - 1.0) * SG_RE;
    if (perige < 156.0) {
        sfour = perige - 78.0;
        if (perige < 98.0) sfour = 20.0;
        qzms24 = pow((120.0 - sfour) / SG_RE, 4);
        sfour = sfour / SG_RE + 1.0;
    }
    pinvsq = 1.0 / posq;
    tsi = 1.0 / (ao - sfour);
    eta = ao * ecco * tsi;
    etasq = eta * eta;
    eeta = ecco * eta;
    psisq = fabs(1.0 - etasq);
    coef = qzms24 * pow(tsi, 4);
    coef1 = coef / pow(psisq, 3.5);
    cc2 = coef1 * s->sg_no * (ao * (1.0 + 1.5 * etasq + eeta * (4.0 + etasq)) +
          0.375 * SG_J2 * tsi / psisq * s->sg_con41 * (8.0 + 3.0 * etasq * (8.0 + etasq)));
    s->sg_cc1 = bstar * cc2;
    cc3 = ecco > 1.0e-4 ? -2.0 * coef * tsi * j3oj2 * s->sg_no * sinio / ecco : 0.0;
    s->sg_x1mth2 = 1.0 - cosio2;
    s->sg_cc4 = 2.0 * s->sg_no * coef1 * ao * omeosq *
        (eta * (2.0 + 0.5 * etasq) + ecco * (0.5 + 2.0 * etasq) -
         SG_J2 * tsi / (ao * psisq) *
         (-3.0 * s->sg_con41 * (1.0 - 2.0 * eeta + etasq * (1.5 - 0.5 * eeta)) +
          0.75 * s->sg_x1mth2 * (2.0 * etasq - eeta * (1.0 + etasq)) * cos(2.0 * argpo)));
    s->sg_cc5 = 2.0 * coef1 * ao * omeosq * (1.0 + 2.75 * (etasq + eeta) + eeta * etasq);
    cosio4 = cosio2 * cosio2;
    temp1 = 1.5 * SG_J2 * pinvsq * s->sg_no;
    temp2 = 0.5 * temp1 * SG_J2 * pinvsq;
    temp3 = -0.46875 * SG_J4 * pinvsq * pinvsq * s->sg_no;
    s->sg_mdot = s->sg_no + 0.5 * temp1 * rteosq * s->sg_con41 +
                 0.0625 * temp2 * rteosq * (13.0 - 78.0 * cosio2 + 137.0 * cosio4);
    s->sg_argpdot = -0.5 * temp1 * con42 + 0.0625 * temp2 * (7.0 - 114.0 * cosio2 + 395.0 * cosio4) +
                    temp3 * (3.0 - 36.0 * cosio2 + 49.0 * cosio4);
    xhdot1 = -temp1 * cosio;
    s->sg_nodedot = xhdot1 + (0.5 * temp2 * (4.0 - 19.0 * cosio2) + 2.0 * temp3 * (3.0 - 7.0 * cosio2)) * cosio;
    s->sg_omgcof = bstar * cc3 * cos(argpo);
    s->sg_xmcof = ecco > 1.0e-4 ? -x2o3 * coef * bstar / eeta : 0.0;
    s->sg_nodecf = 3.5 * omeosq * xhdot1 * s->sg_cc1;
    s->sg_t2cof = 1.5 * s->sg_cc1;
    if (fabs(cosio + 1.0) > 1.5e-12)
        s->sg_xlcof = -0.25 * j3oj2 * sinio * (3.0 + 5.0 * cosio) / (1.0 + cosio);
    else
        s->sg_xlcof = -0.25 * j3oj2 * sinio * (3.0 + 5.0 * cosio) / 1.5e-12;
    s->sg_aycof = -0.5 * j3oj2 * sinio;
    s->sg_eta = eta;
    s->sg_delmo = pow(1.0 + eta * cos(s->mean_anomaly), 3);
    s->sg_sinmao = sin(s->mean_anomaly);
    s->sg_x7thm1 = 7.0 * cosio2 - 1.0;
    if (!s->sg_isimp) {
        double cc1sq = s->sg_cc1 * s->sg_cc1, temp;
        s->sg_d2 = 4.0 * ao * tsi * cc1sq;
        temp = s->sg_d2 * tsi * s->sg_cc1 / 3.0;
        s->sg_d3 = (17.0 * ao + sfour) * temp;
        s->sg_d4 = 0.5 * temp * ao * tsi * (221.0 * ao + 31.0 * sfour) * s->sg_cc1;
        s->sg_t3cof = s->sg_d2 + 2.0 * cc1sq;
        s->sg_t4cof = 0.25 * (3.0 * s->sg_d3 + s->sg_cc1 * (12.0 * s->sg_d2 + 10.0 * cc1sq));
        s->sg_t5cof = 0.2 * (3.0 * s->sg_d4 + 12.0 * s->sg_cc1 * s->sg_d3 +
                      6.0 * s->sg_d2 * s->sg_d2 + 15.0 * cc1sq * (2.0 * s->sg_d2 + cc1sq));
    }
    s->sg_ok = 1;
}

/* TEME position (km) at tsince minutes from epoch; 0 if decayed */
static int sgp4_teme(const pic_sat_t *s, double t, double r[3])
{
    const double x2o3 = 2.0 / 3.0, xke = sg_xke();
    double xmdf, argpdf, nodedf, argpm, mm, t2, nodem, tempa, tempe, templ,
           am, nm, em, xlm, axnl, aynl, xl, u, eo1, tem5, sineo1 = 0, coseo1 = 1,
           ecose, esine, el2, pl, rl, betal, temp, sinu, cosu, su, sin2u, cos2u,
           temp1, temp2, mrt, xnode, xinc, sinip, cosip, sinsu, cossu, snod, cnod,
           sini, cosi, xmx, xmy;
    int ktr;

    xmdf = s->mean_anomaly + s->sg_mdot * t;
    argpdf = s->arg_perigee + s->sg_argpdot * t;
    nodedf = s->raan + s->sg_nodedot * t;
    argpm = argpdf;
    mm = xmdf;
    t2 = t * t;
    nodem = nodedf + s->sg_nodecf * t2;
    tempa = 1.0 - s->sg_cc1 * t;
    tempe = s->bstar * s->sg_cc4 * t;
    templ = s->sg_t2cof * t2;
    if (!s->sg_isimp) {
        double delomg = s->sg_omgcof * t, t3 = t2 * t, t4 = t3 * t;
        double delm = s->sg_xmcof * (pow(1.0 + s->sg_eta * cos(xmdf), 3) - s->sg_delmo);
        temp = delomg + delm;
        mm = xmdf + temp;
        argpm = argpdf - temp;
        tempa = tempa - s->sg_d2 * t2 - s->sg_d3 * t3 - s->sg_d4 * t4;
        tempe = tempe + s->bstar * s->sg_cc5 * (sin(mm) - s->sg_sinmao);
        templ = templ + s->sg_t3cof * t3 + t4 * (s->sg_t4cof + t * s->sg_t5cof);
    }
    nm = s->sg_no;
    em = s->eccentricity;
    if (tempa <= 0) return 0;
    am = pow(xke / nm, x2o3) * tempa * tempa;
    nm = xke / pow(am, 1.5);
    em = em - tempe;
    if (em >= 1.0 || em < -0.001) return 0;
    if (em < 1.0e-6) em = 1.0e-6;
    mm = mm + s->sg_no * templ;
    xlm = mm + argpm + nodem;
    nodem = fmod(nodem, TWO_PI);
    argpm = fmod(argpm, TWO_PI);
    xlm = fmod(xlm, TWO_PI);
    (void)nm;

    /* Long-period periodics */
    sinip = sin(s->inclination);
    cosip = cos(s->inclination);
    axnl = em * cos(argpm);
    temp = 1.0 / (am * (1.0 - em * em));
    aynl = em * sin(argpm) + temp * s->sg_aycof;
    xl = xlm + temp * s->sg_xlcof * axnl;

    /* Kepler's equation */
    u = fmod(xl - nodem, TWO_PI);
    eo1 = u;
    tem5 = 9999.9;
    for (ktr = 1; fabs(tem5) >= 1.0e-12 && ktr <= 10; ktr++) {
        sineo1 = sin(eo1);
        coseo1 = cos(eo1);
        tem5 = 1.0 - coseo1 * axnl - sineo1 * aynl;
        tem5 = (u - aynl * coseo1 + axnl * sineo1 - eo1) / tem5;
        if (fabs(tem5) >= 0.95) tem5 = tem5 > 0.0 ? 0.95 : -0.95;
        eo1 += tem5;
    }

    /* Short-period periodics */
    ecose = axnl * coseo1 + aynl * sineo1;
    esine = axnl * sineo1 - aynl * coseo1;
    el2 = axnl * axnl + aynl * aynl;
    pl = am * (1.0 - el2);
    if (pl < 0.0) return 0;
    rl = am * (1.0 - ecose);
    betal = sqrt(1.0 - el2);
    temp = esine / (1.0 + betal);
    sinu = am / rl * (sineo1 - aynl - axnl * temp);
    cosu = am / rl * (coseo1 - axnl + aynl * temp);
    su = atan2(sinu, cosu);
    sin2u = (cosu + cosu) * sinu;
    cos2u = 1.0 - 2.0 * sinu * sinu;
    temp = 1.0 / pl;
    temp1 = 0.5 * SG_J2 * temp;
    temp2 = temp1 * temp;
    mrt = rl * (1.0 - 1.5 * temp2 * betal * s->sg_con41) + 0.5 * temp1 * s->sg_x1mth2 * cos2u;
    su = su - 0.25 * temp2 * s->sg_x7thm1 * sin2u;
    xnode = nodem + 1.5 * temp2 * cosip * sin2u;
    xinc = s->inclination + 1.5 * temp2 * cosip * sinip * cos2u;
    if (mrt < 1.0) return 0;                   /* below the surface */

    sinsu = sin(su); cossu = cos(su);
    snod = sin(xnode); cnod = cos(xnode);
    sini = sin(xinc); cosi = cos(xinc);
    xmx = -snod * cosi;
    xmy = cnod * cosi;
    r[0] = mrt * SG_RE * (xmx * sinsu + cnod * cossu);
    r[1] = mrt * SG_RE * (xmy * sinsu + snod * cossu);
    r[2] = mrt * SG_RE * (sini * sinsu);
    return 1;
}

/* Greenwich mean sidereal time (IAU-82), radians */
static double gmst_rad(double jd)
{
    double tut1 = (jd - 2451545.0) / 36525.0;
    double sec = -6.2e-6 * tut1 * tut1 * tut1 + 0.093104 * tut1 * tut1 +
                 (876600.0 * 3600.0 + 8640184.812866) * tut1 + 67310.54841;
    double g = fmod(sec * DEG_TO_RAD / 240.0, TWO_PI);
    return g < 0 ? g + TWO_PI : g;
}

/* Inertial (TEME/ECI) position → WGS-84 geodetic lat/lon (deg), alt (km) */
static void eci_to_geodetic(const double r[3], double jd,
                            double *lat, double *lon, double *alt)
{
    const double a = 6378.137, f = 1.0 / 298.257223563, e2 = f * (2.0 - f);
    double g = gmst_rad(jd), x, y, z = r[2], p, phi, N = a, h = 0, l;
    int i;

    x = r[0] * cos(g) + r[1] * sin(g);
    y = -r[0] * sin(g) + r[1] * cos(g);
    p = sqrt(x * x + y * y);
    phi = atan2(z, p * (1.0 - e2));
    for (i = 0; i < 5; i++) {
        double sp = sin(phi);
        N = a / sqrt(1.0 - e2 * sp * sp);
        h = p / cos(phi) - N;
        phi = atan2(z, p * (1.0 - e2 * N / (N + h)));
    }
    l = atan2(y, x);
    *lat = phi * RAD_TO_DEG;
    *lon = l * RAD_TO_DEG;
    *alt = h;
}

/*
 * sgp4_at_time - Position at a given time: SGP4 for near-Earth orbits,
 *                otherwise a simplified secular J2 model.
 *
 * Returns lat/lon (degrees) and altitude (km).
 */
static void sgp4_at_time(const pic_sat_t *sat, double jd,
                         double *out_lat, double *out_lon, double *out_alt)
{
    double tsince_min;
    double n0, a0, e0, i0, w0, om0, m0;
    double n, a, e, w, om, M, E, v;
    double r, u, lat_rad, lon_rad, gmst;

    tsince_min = (jd - sat->epoch_jd) * MINUTES_PER_DAY;

    if (sat->sg_ok) {
        double rr[3];
        if (sgp4_teme(sat, tsince_min, rr)) {
            eci_to_geodetic(rr, jd, out_lat, out_lon, out_alt);
            return;
        }
        /* decayed: report it at the surface so callers can skip it */
        *out_lat = 0; *out_lon = 0; *out_alt = -1;
        return;
    }

    /* Convert mean motion from rev/day to rad/min */
    n0 = sat->mean_motion * TWO_PI / MINUTES_PER_DAY; /* rad/min */
    /* Semi-major axis from Kepler's 3rd law: a = (mu/n²)^(1/3).
     * mu is in km³/s², n is in rad/min — multiply mu by 3600 to
     * convert to km³/min². Result a0 is in km. */
    a0 = pow((EARTH_MU * 3600.0) / (n0 * n0), 1.0 / 3.0);
    e0 = sat->eccentricity;
    i0 = sat->inclination;
    w0 = sat->arg_perigee;
    om0 = sat->raan;
    m0 = sat->mean_anomaly;

    /* Secular J2 perturbations */
    {
        double p = a0 * (1.0 - e0 * e0);
        double cosi = cos(i0);
        double sini = sin(i0);
        double j2_f = 1.5 * J2 * (EARTH_R_KM / p) * (EARTH_R_KM / p);

        double om_dot = -j2_f * n0 * cosi;
        double w_dot = j2_f * n0 * (2.0 - 2.5 * sini * sini);

        n = n0;
        a = a0;
        e = e0;
        w = w0 + w_dot * tsince_min;
        om = om0 + om_dot * tsince_min;
        M = m0 + n * tsince_min;
    }

    /* Kepler's equation */
    E = M;
    {
        int iter;
        for (iter = 0; iter < 10; iter++) {
            double dE = (M - E + e * sin(E)) / (1.0 - e * cos(E));
            E += dE;
            if (fabs(dE) < 1e-10) break;
        }
    }

    v = 2.0 * atan2(sqrt(1.0 + e) * sin(E / 2.0),
                     sqrt(1.0 - e) * cos(E / 2.0));
    r = a * (1.0 - e * cos(E));
    u = v + w;

    /* Orbital plane → ECI → geodetic */
    {
        double x_orb = r * cos(u);
        double y_orb = r * sin(u);
        double cos_om = cos(om), sin_om = sin(om);
        double cos_i = cos(i0), sin_i = sin(i0);

        double x_eci = x_orb * cos_om - y_orb * cos_i * sin_om;
        double y_eci = x_orb * sin_om + y_orb * cos_i * cos_om;
        double z_eci = y_orb * sin_i;

        lat_rad = atan2(z_eci, sqrt(x_eci * x_eci + y_eci * y_eci));

        /* Greenwich Mean Sidereal Time (GMST) in degrees.
         * IAU 1982 formula. T = Julian centuries since J2000.0.
         * 2451545.0 = J2000.0 (noon, 1 Jan 2000).
         * 36525.0 = days per Julian century.
         * Ref: Astronomical Almanac, B7. */
        {
            double T = (jd - 2451545.0) / 36525.0;
            gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0)
                   + 0.000387933 * T * T;
            gmst = fmod(gmst, 360.0);
            if (gmst < 0) gmst += 360.0;
        }

        lon_rad = atan2(y_eci, x_eci) - gmst * DEG_TO_RAD;
        while (lon_rad > PI) lon_rad -= TWO_PI;
        while (lon_rad < -PI) lon_rad += TWO_PI;
    }

    *out_lat = lat_rad * RAD_TO_DEG;
    *out_lon = lon_rad * RAD_TO_DEG;
    *out_alt = r - EARTH_R_KM;
}

/*
 * sgp4_propagate - Compute current position and ground track.
 *
 * Calculates the satellite's current position plus a ground track
 * showing 25% of the previous orbit and 75% of the current orbit.
 */
static void sgp4_propagate(pic_sat_t *sat, time_t now)
{
    double jd_now, orbit_period_min;
    int i;

    if (!sat->tle_loaded) return;

    jd_now = 2440587.5 + (double)now / 86400.0;

    /* Current position */
    sgp4_at_time(sat, jd_now, &sat->lat, &sat->lon, &sat->alt_km);

    /* Footprint radius: the angular distance from the sub-satellite point
     * to the horizon, i.e., where elevation angle = 0°.
     * acos(R / (R + alt)) gives the half-angle of the coverage cone. */
    if (sat->alt_km > 0) {
        sat->footprint_r = acos(EARTH_R_KM / (EARTH_R_KM + sat->alt_km))
                           * RAD_TO_DEG;
    } else {
        sat->footprint_r = 0;
    }

    /* Orbital period in minutes */
    orbit_period_min = MINUTES_PER_DAY / sat->mean_motion;

    /* Ground track: 25% past + 75% future of one orbit.
     * SAT_TRACK_PTS points spread over the full orbit period.
     * track_now_idx marks where "now" is in the array. */
    {
        double past_min = orbit_period_min * 0.25;
        double future_min = orbit_period_min * 0.75;
        double total_min = past_min + future_min;
        double step = total_min / (SAT_TRACK_PTS - 1);

        sat->track_now_idx = (int)(past_min / step);
        sat->track_count = SAT_TRACK_PTS;

        for (i = 0; i < SAT_TRACK_PTS; i++) {
            double t_offset = -past_min + i * step;  /* minutes from now */
            double jd_pt = jd_now + t_offset / MINUTES_PER_DAY;
            double dummy_alt;

            sgp4_at_time(sat, jd_pt,
                         &sat->track_lat[i], &sat->track_lon[i],
                         &dummy_alt);
        }
    }
}

/* TLE cache file — stored on tmpfs so it survives service restarts
 * but not reboots. Avoids hammering Celestrak on every restart. */
#define TLE_CACHE "/tmp/pi-clock-tles.txt"

/* Bulk TLE URL — fetches ALL amateur satellites in one request.
 * One TLS handshake instead of 20+ individual ones.
 * AMSAT is used instead of Celestrak — Celestrak's TLS handshake
 * is too slow for BusyBox ssl_client on Pi Zero (times out). */
#define TLE_BULK_URL "https://www.amsat.org/tle/current/nasabare.txt"

/* Individual TLE API — used as fallback for satellites not in the
 * AMSAT amateur catalog (e.g., Meteor-M weather sats). Returns JSON
 * with line1/line2 fields. Works reliably from BusyBox wget on Pi. */
#define TLE_API_URL "https://tle.ivanstanojevic.me/api/tle/%d"

/*
 * find_tle_in_cache - Search the cached TLE data for a NORAD ID.
 * TLE format: 3 lines per satellite (name, line1, line2).
 * Line 1 columns 3-7 contain the NORAD catalog number.
 */
static int find_tle_in_cache(const char *cache, int norad_id,
                             char *l1, char *l2)
{
    const char *p = cache;
    char id_str[8];

    snprintf(id_str, sizeof(id_str), "%05d", norad_id);

    while (*p) {
        /* Skip name line */
        while (*p && *p != '\n') p++;
        if (*p) p++;

        /* This should be line 1 — check NORAD ID at columns 3-7 */
        if (*p == '1' && p[1] == ' ' && strncmp(p + 2, id_str, 5) == 0) {
            /* Found it — extract line 1 and line 2 */
            int i;
            for (i = 0; *p && *p != '\n' && i < 79; i++) l1[i] = *p++;
            l1[i] = '\0';
            if (*p == '\n') p++;
            for (i = 0; *p && *p != '\n' && i < 79; i++) l2[i] = *p++;
            l2[i] = '\0';
            return 0;
        }

        /* Skip line 1 */
        while (*p && *p != '\n') p++;
        if (*p) p++;
        /* Skip line 2 */
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return -1;
}

/* Shared TLE cache — loaded once from file, cleared on refresh */
static char *tle_cache = NULL;
static size_t tle_cache_size = 0;

static void tle_cache_clear(void)
{
    free(tle_cache);
    tle_cache = NULL;
    tle_cache_size = 0;
}

/*
 * fetch_tle - Load TLE for a satellite from the cache.
 */
static int fetch_tle(pic_sat_t *sat)
{
    char l1[80] = "", l2[80] = "";

    /* Load cache if not already loaded */
    if (!tle_cache) {
        FILE *f = fopen(TLE_CACHE, "r");
        if (f) {
            long fsz;
            fseek(f, 0, SEEK_END);
            fsz = ftell(f);
            if (fsz <= 0 || fsz > 5 * 1024 * 1024) {
                /* TLE files are typically <500KB. Reject obviously
                 * wrong sizes (ftell returns -1 on error). */
                fclose(f);
                return -1;
            }
            tle_cache_size = (size_t)fsz;
            fseek(f, 0, SEEK_SET);
            tle_cache = malloc(tle_cache_size + 1);
            if (tle_cache) {
                if (fread(tle_cache, 1, tle_cache_size, f) != tle_cache_size) {
                    free(tle_cache);
                    tle_cache = NULL;
                    fclose(f);
                    return -1;
                }
                tle_cache[tle_cache_size] = '\0';
            }
            fclose(f);
        }
    }

    if (!tle_cache) return -1;

    if (find_tle_in_cache(tle_cache, sat->norad_id, l1, l2) == 0) {
        if (parse_tle(sat, l1, l2) == 0) {
            printf("satellite: TLE loaded for %s (NORAD %d, MM=%.4f rev/day)\n",
                   sat->name, sat->norad_id, sat->mean_motion);
            return 0;
        }
    }

    return -1;
}

/*
 * extract_json_string - Extract a JSON string value by key.
 *
 * Searches for "key":"value" and copies value into out (up to
 * out_size-1 chars). Handles escaped quotes. Returns 0 on success.
 */
static int extract_json_string(const char *json, const char *key,
                               char *out, int out_size)
{
    char needle[64];
    const char *p, *end;
    int len;

    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);

    /* Find closing quote (skip escaped quotes) */
    end = p;
    while (*end && !(*end == '"' && *(end - 1) != '\\')) end++;
    if (!*end) return -1;

    len = (int)(end - p);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, len);
    out[len] = '\0';

    /* Unescape \/ (JSON forward slash escaping) */
    {
        char *s = out;
        char *w = out;
        while (*s) {
            if (*s == '\\' && *(s + 1) == '/') { *w++ = '/'; s += 2; }
            else *w++ = *s++;
        }
        *w = '\0';
    }

    return 0;
}

/*
 * fetch_tle_individual - Fetch a single TLE by NORAD ID.
 *
 * Network only: fills l1/l2 (each at least 80 bytes) and does not
 * touch the satellite list, so the caller can run it without holding
 * the list mutex (the renderer locks that mutex every frame).
 *
 * Used as a fallback when the AMSAT bulk catalog doesn't contain a
 * satellite (e.g., Meteor-M weather satellites are not amateur).
 * Uses tle.ivanstanojevic.me which returns JSON with line1/line2
 * fields and works reliably with BusyBox wget on the Pi.
 */
static int fetch_tle_individual(int norad_id, char *l1, char *l2)
{
    char url[256];
    char buf[1024];
    int n;

    snprintf(url, sizeof(url), TLE_API_URL, norad_id);
    n = pic_fetch_to_buf(url, buf, sizeof(buf));
    if (n <= 0) return -1;

    /* Extract TLE lines from JSON response */
    if (extract_json_string(buf, "line1", l1, 80) != 0 ||
        extract_json_string(buf, "line2", l2, 80) != 0) {
        return -1;
    }

    return 0;
}

/*
 * fetch_all_tles - Download bulk TLE catalog and parse for our satellites.
 *
 * First fetches the AMSAT amateur satellite catalog (one request for
 * all amateur sats). Then, for any enabled satellite not found in the
 * AMSAT catalog, falls back to Celestrak individual TLE lookup. This
 * handles non-amateur satellites like Meteor-M weather sats.
 */
static void fetch_all_tles(void)
{
    int i, loaded = 0, fallback = 0;

    printf("satellite: fetching amateur TLE catalog...\n");

    /* Download bulk TLE catalog — one request for everything */
    if (pic_fetch_to_file(TLE_BULK_URL, TLE_CACHE, 30) != 0) {
        printf("satellite: bulk TLE download failed, trying cache\n");
    }

    /* Clear in-memory cache so fetch_tle re-reads the fresh file */
    tle_cache_clear();

    /* Parse TLEs from cache for each registered satellite.
     * got[] records which satellites were refreshed in THIS cycle,
     * so ones only available via individual fetch are refreshed too
     * (previously a satellite with any old TLE was skipped forever). */
    {
        int got[SAT_MAX] = {0};
        int count;

        pthread_mutex_lock(&sat_list->mutex);
        count = sat_list->count;
        for (i = 0; i < count; i++) {
            if (!sat_running) break;
            if (!sat_list->sats[i].enabled) continue;
            if (fetch_tle(&sat_list->sats[i]) == 0) {
                got[i] = 1;
                loaded++;
            }
        }
        pthread_mutex_unlock(&sat_list->mutex);

        /* Fallback: fetch individually for satellites not in AMSAT
         * (weather sats, space stations, Hubble, etc.). Each is a
         * separate HTTPS request, so the network I/O happens WITHOUT
         * the list mutex held — otherwise the render loop stalls for
         * the length of every TLS handshake. */
        for (i = 0; i < count; i++) {
            int norad_id, enabled;
            char l1[80], l2[80];

            if (!sat_running) break;
            if (got[i]) continue;

            pthread_mutex_lock(&sat_list->mutex);
            enabled = sat_list->sats[i].enabled;
            norad_id = sat_list->sats[i].norad_id;
            pthread_mutex_unlock(&sat_list->mutex);
            if (!enabled) continue;

            if (fetch_tle_individual(norad_id, l1, l2) != 0) continue;

            pthread_mutex_lock(&sat_list->mutex);
            if (parse_tle(&sat_list->sats[i], l1, l2) == 0) {
                printf("satellite: TLE loaded for %s (NORAD %d, MM=%.4f rev/day)\n",
                       sat_list->sats[i].name, norad_id,
                       sat_list->sats[i].mean_motion);
                loaded++;
                fallback++;
            }
            pthread_mutex_unlock(&sat_list->mutex);
        }
    }

    sat_list->last_tle_update = time(NULL);
    printf("satellite: TLE fetch complete (%d loaded", loaded);
    if (fallback > 0) printf(", %d via individual fetch", fallback);
    printf(")\n");
}

static void *sat_thread_func(void *arg)
{
    (void)arg;

    /* Lower priority so TLS handshakes don't starve the renderer */
    nice(15);

    if (!pic_wait_for_network("satellite", &sat_running)) {
        printf("satellite: will retry in %d seconds\n", TLE_FETCH_INTERVAL);
    } else {
        fetch_all_tles();
    }

    while (sat_running) {
        int elapsed = 0;
        while (sat_running && elapsed < TLE_FETCH_INTERVAL) {
            sleep(1);       /* 1 s steps so shutdown is prompt */
            elapsed += 1;
            /* Config reload can reset last_tle_update to trigger
             * an immediate refetch for newly enabled satellites */
            if (sat_list->last_tle_update == 0) break;
        }
        if (sat_running) {
            fetch_all_tles();
        }
    }

    return NULL;
}

void pic_sat_init(pic_satlist_t *list)
{
    memset(list, 0, sizeof(*list));
    pthread_mutex_init(&list->mutex, NULL);
}

int pic_sat_add(pic_satlist_t *list, const char *config_key,
                const char *name, int norad_id)
{
    pic_sat_t *s;

    if (list->count >= SAT_MAX) return -1;

    s = &list->sats[list->count];
    strncpy(s->config_key, config_key, SAT_NAME_LEN - 1);
    strncpy(s->name, name, SAT_NAME_LEN - 1);
    s->norad_id = norad_id;
    s->enabled = 0;  /* Off until config file says otherwise */

    list->count++;
    printf("satellite: registered '%s' [%s] (NORAD %d)\n",
           name, config_key, norad_id);
    return 0;
}

#define SAT_CONF "/data/etc/pi-clock-satellites.conf"

/*
 * pic_sat_load_config - Apply enable/disable settings from config file.
 *
 * After updating the enabled flags, fetches TLEs for any satellite
 * that is now enabled but doesn't have orbital data yet. This handles
 * the case where a satellite was disabled at startup (TLE skipped)
 * and then enabled via the dashboard mid-run.
 */
void pic_sat_load_config(pic_satlist_t *list)
{
    FILE *f;
    char line[128];
    int i;
    int need_fetch = 0;

    f = fopen(SAT_CONF, "r");
    if (!f) return;

    printf("satellite: loading config from %s\n", SAT_CONF);

    while (fgets(line, sizeof(line), f)) {
        char key[32];
        int enabled;

        if (line[0] == '#' || line[0] == '\n') continue;

        if (sscanf(line, "%31[^=]=%d", key, &enabled) == 2) {
            for (i = 0; i < list->count; i++) {
                if (strcmp(list->sats[i].config_key, key) == 0) {
                    list->sats[i].enabled = enabled;
                    printf("satellite: '%s' enabled=%d\n", key, enabled);
                    /* Flag if newly enabled without TLE data */
                    if (enabled && !list->sats[i].tle_loaded)
                        need_fetch = 1;
                    break;
                }
            }
        }
    }

    fclose(f);

    /* If satellites were newly enabled, signal the background thread
     * to fetch their TLEs. Never do network I/O in the main thread —
     * TLS handshakes on Pi Zero take seconds and block rendering. */
    if (need_fetch) {
        printf("satellite: newly enabled satellites — background thread will fetch TLEs\n");
        /* Reset last_tle_update to trigger an immediate fetch cycle */
        list->last_tle_update = 0;
    }
}

int pic_sat_start(pic_satlist_t *list)
{
    if (sat_running) return 0;

    sat_list = list;
    sat_running = 1;

    if (pthread_create(&sat_thread, NULL, sat_thread_func, NULL) != 0) {
        sat_running = 0;
        fprintf(stderr, "satellite: failed to create thread\n");
        return -1;
    }

    /* Thread is joinable — pic_sat_stop() calls pthread_join */
    printf("satellite: tracking thread started\n");
    return 0;
}

void pic_sat_stop(void)
{
    if (!sat_running) return;
    sat_running = 0;
    pthread_join(sat_thread, NULL);
}

static int propagate_logged = 0;

void pic_sat_propagate(pic_satlist_t *list, time_t now)
{
    int i;

    pthread_mutex_lock(&list->mutex);
    for (i = 0; i < list->count; i++) {
        if (list->sats[i].enabled && list->sats[i].tle_loaded) {
            sgp4_propagate(&list->sats[i], now);

            /* Log first propagation result for debugging */
            if (!propagate_logged) {
                printf("satellite: %s pos=%.2f,%.2f alt=%.0fkm fp=%.1f°\n",
                       list->sats[i].name,
                       list->sats[i].lat, list->sats[i].lon,
                       list->sats[i].alt_km, list->sats[i].footprint_r);
            }
        }
    }
    if (!propagate_logged && list->count > 0) propagate_logged = 1;
    pthread_mutex_unlock(&list->mutex);
}

/* ── Public helpers for other modules (e.g. Starlink trains) ───── */

int pic_sat_parse_tle(pic_sat_t *sat, const char *line1, const char *line2)
{
    return parse_tle(sat, line1, line2);
}

void pic_sat_position_at(const pic_sat_t *sat, time_t t,
                         double *lat, double *lon, double *alt_km)
{
    /* Unix time → Julian date (2440587.5 = 1970-01-01T00:00Z) */
    double jd = 2440587.5 + (double)t / 86400.0;
    sgp4_at_time(sat, jd, lat, lon, alt_km);
}

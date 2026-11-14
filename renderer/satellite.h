/*
 * satellite.h - Satellite tracking via TLE + SGP4
 *
 * Downloads TLE data from Celestrak, propagates orbits using a
 * simplified SGP4 model, and provides current satellite positions
 * for rendering on the map.
 *
 * Supported satellites are configurable — defaults include the ISS
 * and common amateur radio satellites.
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_SATELLITE_H
#define PIC_SATELLITE_H

#include <time.h>
#include <pthread.h>

#define SAT_MAX 32
#define SAT_NAME_LEN 32
#define SAT_TRACK_PTS 120   /* Ground track points (past + future) */

/*
 * pic_sat_t - A tracked satellite.
 */
typedef struct {
    char name[SAT_NAME_LEN];  /* Common name (e.g. "ISS")           */
    char config_key[SAT_NAME_LEN]; /* Config key (e.g. "iss")       */
    int  norad_id;            /* NORAD catalog number                */
    int  enabled;             /* Non-zero if tracking this satellite */

    /* TLE orbital elements (parsed from two-line element set) */
    double epoch_jd;          /* Epoch as Julian date                */
    double inclination;       /* Inclination (radians)               */
    double raan;              /* Right ascension of ascending node   */
    double eccentricity;      /* Eccentricity                        */
    double arg_perigee;       /* Argument of perigee (radians)       */
    double mean_anomaly;      /* Mean anomaly at epoch (radians)     */
    double mean_motion;       /* Mean motion (revs/day)              */
    double bstar;             /* B* drag term                        */
    int    tle_loaded;        /* Non-zero if TLE data is valid       */

    /* SGP4 (near-Earth) constants from the elements, set when the
     * TLE is parsed (satellite.c); sg_ok = 0 for high orbits, which
     * use a simpler secular model instead */
    int    sg_ok, sg_isimp;
    double sg_no, sg_ao, sg_con41, sg_x1mth2, sg_x7thm1, sg_cc1, sg_cc4, sg_cc5,
           sg_d2, sg_d3, sg_d4, sg_delmo, sg_eta, sg_sinmao, sg_mdot,
           sg_argpdot, sg_nodedot, sg_omgcof, sg_xmcof, sg_nodecf, sg_t2cof,
           sg_t3cof, sg_t4cof, sg_t5cof, sg_xlcof, sg_aycof;

    /* Current computed position */
    double lat;               /* Sub-satellite latitude (degrees)    */
    double lon;               /* Sub-satellite longitude (degrees)   */
    double alt_km;            /* Altitude above Earth (km)           */
    double footprint_r;       /* Footprint radius (degrees)          */

    /* Ground track: past 25% + future 75% of one orbit */
    double track_lat[SAT_TRACK_PTS];
    double track_lon[SAT_TRACK_PTS];
    int    track_now_idx;     /* Index of "now" in the track array   */
    int    track_count;       /* Number of valid track points        */
} pic_sat_t;

/*
 * pic_satlist_t - List of tracked satellites.
 */
typedef struct {
    pic_sat_t sats[SAT_MAX];
    int count;
    time_t last_tle_update;
    pthread_mutex_t mutex;
} pic_satlist_t;

void pic_sat_init(pic_satlist_t *list);

/*
 * pic_sat_add - Register a satellite to track.
 */
int pic_sat_add(pic_satlist_t *list, const char *config_key,
                const char *name, int norad_id);

/*
 * pic_sat_start - Start the background TLE fetch + propagation thread.
 */
int pic_sat_start(pic_satlist_t *list);
void pic_sat_stop(void);

/*
 * pic_sat_propagate - Update all satellite positions for the given time.
 * Called from the render loop.
 */
void pic_sat_propagate(pic_satlist_t *list, time_t now);

/*
 * pic_sat_load_config - Load satellite enable/disable from config.
 * File: /data/etc/pi-clock-satellites.conf, format: name=0|1
 */
void pic_sat_load_config(pic_satlist_t *list);

/*
 * pic_sat_parse_tle - Load orbital elements from a two-line element set
 * into `sat` (other fields untouched). Returns 0 on success.
 */
int pic_sat_parse_tle(pic_sat_t *sat, const char *line1, const char *line2);

/*
 * Catalog number from TLE line 1 (columns 3-7). Handles Alpha-5, used
 * since the 5-digit numbers ran out: "A0983" = 100983 (A=10, B=11 ...
 * Z=33, skipping I and O).
 */
int pic_tle_catnr(const char *line1);

/*
 * pic_sat_position_at - Sub-satellite point and altitude at time t,
 * using the same simplified SGP4 as the tracker.
 */
void pic_sat_position_at(const pic_sat_t *sat, time_t t,
                         double *lat, double *lon, double *alt_km);

#endif /* PIC_SATELLITE_H */

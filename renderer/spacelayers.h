/*
 * spacelayers.h - Space-Track data for the re-entry, close-approach,
 * geostationary-belt and debris layers (read from SPACEDATA_DIR)
 *
 * The parsing here has no drawing in it so the tests can feed it
 * made-up replies.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PIC_SPACELAYERS_H
#define PIC_SPACELAYERS_H

#include <stddef.h>
#include <time.h>
#include "satellite.h"

#define SL_MAX_RE    12
#define SL_MAX_CJ    12
#define SL_MAX_GEO   1600
#define SL_MAX_GRP   4
#define SL_MAX_PIECE 300
#define SL_CJ_HOURS  6      /* close approaches shown this far ahead  */
#define SL_RE_KEEP   (24 * 3600)  /* re-entries kept on the map after */

typedef struct {
    int    id;
    char   name[32];
    time_t decay;          /* predicted (or past) re-entry time       */
    int    window_min;     /* ± uncertainty in minutes                */
    double lat, lon;       /* best-estimate point (lon -180..180)     */
    double incl;           /* degrees                                 */
    int    descending;
    int    has_sat;        /* elements found in objects.tle           */
    pic_sat_t sat;
} sl_reentry_t;

typedef struct {
    int    id1, id2;
    char   name1[32], name2[32];
    time_t tca;
    double tca_frac;       /* fractional second of TCA                */
    double rng_m;          /* predicted miss distance                 */
    double pc;             /* collision probability, 0 if not given   */
    int    has_sat;        /* both element sets found                 */
    pic_sat_t s1, s2;
} sl_conj_t;

typedef struct {
    char   key[12];        /* launch designator, e.g. "2026-051"      */
    char   name[32];       /* parent name without " DEB"              */
    char   country[8];
    int    year;           /* launch year                             */
    int    count;          /* pieces among the newest catalogued      */
    int    newest;         /* highest catalogue number in the group   */
} sl_group_t;

typedef struct {
    int    group;
    pic_sat_t sat;
} sl_piece_t;

typedef struct {
    char   name[24];
    pic_sat_t sat;
} sl_geo_t;

typedef struct {
    long   total, payload, rocket, debris;
    int    week;           /* predicted to re-enter in the next 7 days */
    int    ok;
} sl_stats_t;

/* "2026-11-08 4:56:00" / "2026-11-10T12:23:56.101" (UTC) -> time_t,
 * fractional seconds to *frac if given; 0 on failure */
time_t sl_parse_time(const char *s, double *frac);

/* Latest TIP message per object; keeps re-entries after now - keep,
 * sorted by time. Returns count. */
int sl_parse_tip(const char *js, time_t now, time_t keep, sl_reentry_t *out, int max);

/* Newest public conjunction message per pair with TCA in (now,
 * now + hours], sorted by TCA. */
int sl_parse_cdm(const char *js, time_t now, int hours, sl_conj_t *out, int max);

/* Debris groups (by launch) among the newest catalogued pieces, those
 * with at least min_pieces, newest group first. */
int sl_debris_groups(const char *js, int min_pieces, sl_group_t *out, int max);

/* Catalogue numbers of the pieces in the first `ngroups` groups, as a
 * comma list (for the element-set query). */
void sl_debris_ids(const char *js, const sl_group_t *g, int ngroups, char *list, size_t n);

/* Orbital totals from the boxscore ("ALL" row) and the number of
 * decay predictions in [now, now + 7 days]. */
int sl_parse_stats(const char *box, const char *decay, time_t now, sl_stats_t *st);

/* Ground track of a circular orbit of inclination `incl` that passes
 * (lat, lon) at t = 0 (descending if asked), at `alt_km`; tau in
 * minutes. Used for the re-entry window along the orbit. */
void sl_incl_track(double lat, double lon, double incl, int descending,
                   double alt_km, double tau_min, double *olat, double *olon);

/* "FENGYUN 1C DEB" -> "Fengyun 1C debris", "SL-8 R/B" -> "SL-8 rocket body" */
void sl_tidy_name(const char *in, char *out, size_t n);
#endif

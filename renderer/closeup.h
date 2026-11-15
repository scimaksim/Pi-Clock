/*
 * closeup.h - The close-up view of home (VIEW_CLOSEUP_MIN)
 *
 * Two layers drawn only in that view (layers_closeup.c):
 *   closeup    USGS aerial photo with lakes and creeks (USGS NHD), main
 *              roads and town names (Census TIGER), fetched once for the
 *              view and kept for 30 days.
 *   watershed  Live: creek gauges against normal for the date (USGS),
 *              the NC State Climate Office field-lab station, and fires
 *              seen from space (NASA FIRMS), all read from the Pi-Dash
 *              server on the PC (PIDASH_URL), which fetches them once
 *              for the whole house and keeps the API keys.
 *
 * The parsers are public for the tests.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PIC_CLOSEUP_H
#define PIC_CLOSEUP_H

#include <stddef.h>
#include <time.h>

#define CU_MAX_SITES 8
#define CU_MAX_FIRES 400

typedef struct {
    char   site[16], role[8], name[48];
    char   cls[12], cls_txt[48];    /* "much-below" ... and USGS's wording */
    double flow, normal;            /* ft3/s, NAN if none                  */
    time_t t;                       /* time of the reading                 */
    double lat, lon;                /* from USGS site info                 */
    int    has_loc;
} cu_site_t;

typedef struct {
    int    ok;                      /* station block present               */
    double air, soil, moist, rh, wind, sr;   /* NAN if missing             */
    char   at[24];                  /* "2026-11-15 14:10:00" (local time)  */
    double rain_2h, rain_24h, rain_7d, rain_30d;
    double lat, lon;
    int    has_loc;
    char   err[96];
} cu_station_t;

typedef struct {
    cu_site_t    s[CU_MAX_SITES];
    int          n;
    cu_station_t st;
} cu_wsh_t;

typedef struct {
    float  lat, lon, frp;
    time_t t;
    int    night;
} cu_fire_t;

typedef struct {
    char   name[40];
    double lat, lon, area;          /* land area, m2                       */
} cu_place_t;

/* Pi-Dash /api/watershed reply. Returns sites read, -1 if unreadable. */
int pic_cu_parse_watershed(const char *js, size_t len, cu_wsh_t *out);

/* Pi-Dash /api/ncorbit reply: fires (newest first) and the hours they
 * cover. Returns fires read, -1 if unreadable. */
int pic_cu_parse_fires(const char *js, size_t len, cu_fire_t *out, int max, int *hours);

/* USGS site service, rdb format: fills lat/lon of the matching sites.
 * Returns how many were located. */
int pic_cu_parse_usgs_sites(const char *rdb, cu_site_t *s, int n);

/* GeoJSON lines and polygon rings: cb(name, lonlat pairs, points, ring,
 * ud) per line. Returns features read, -1 if unreadable. */
typedef void (*cu_line_cb)(const char *name, const double *xy, int n, int ring, void *ud);
int pic_cu_geo_lines(const char *js, size_t len, const char *name_key, cu_line_cb cb, void *ud);

/* TIGER places (attributes only). Returns places read, -1 if unreadable. */
int pic_cu_places(const char *js, size_t len, cu_place_t *out, int max);

/* ECONet local time "YYYY-MM-DD HH:MM:SS" -> time_t (local), 0 if bad */
time_t pic_cu_local_time(const char *s);

#endif

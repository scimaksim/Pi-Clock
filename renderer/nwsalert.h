/*
 * nwsalert.h - US National Weather Service active alerts
 *
 * Two feeds from api.weather.gov (GeoJSON, no key; User-Agent required):
 *
 *   1. Map:  severe and extreme alerts nationwide. Storm-based warnings
 *            (tornado, severe thunderstorm, flash flood, special marine,
 *            ...) carry polygons, which are drawn on the map. Zone-based
 *            alerts without a polygon are skipped for the map.
 *
 *   2. Home: every alert in effect at the QTH point, of any severity,
 *            including zone-based watches, warnings and advisories.
 *            These drive an on-screen banner.
 *
 * Outside the US the home query simply returns nothing.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_NWSALERT_H
#define PIC_NWSALERT_H

#include <pthread.h>
#include <time.h>

#define NWS_MAX_MAP_ALERTS   400
#define NWS_MAX_VERTICES     40000
#define NWS_MAX_HOME_ALERTS  8

/* CAP severity, ordered */
enum { NWS_SEV_UNKNOWN = 0, NWS_SEV_MINOR, NWS_SEV_MODERATE,
       NWS_SEV_SEVERE, NWS_SEV_EXTREME };

typedef struct {
    char   event[48];         /* e.g. "Tornado Warning"                */
    int    severity;          /* NWS_SEV_*                             */
    time_t expires;           /* When it ends (0 = unknown)            */
    int    ends_known;        /* 1: hazard end time; 0: only when this
                               * message lapses (hazard may go on)     */
    float  r, g, b;           /* Display colour                        */
    int    first_vertex;      /* Index into the shared vertex pool     */
    int    nvertex;           /* Polygon ring length (0 = no polygon)  */
} pic_nws_alert_t;

typedef struct {
    /* Map polygons */
    pic_nws_alert_t map[NWS_MAX_MAP_ALERTS];
    int             nmap;
    float           vlat[NWS_MAX_VERTICES];
    float           vlon[NWS_MAX_VERTICES];
    int             nvert;

    /* Alerts at the home QTH (sorted most severe first) */
    pic_nws_alert_t home[NWS_MAX_HOME_ALERTS];
    int             nhome;

    /* Settings */
    int    show_banner;       /* Banner for alerts at QTH              */
    int    banner_min_sev;    /* Minimum severity shown in the banner  */
    int    radius_km;         /* Map: only alerts within this distance
                               * of QTH (0 = nationwide)               */
    time_t last_fetched;
    time_t home_fetched;      /* last good reply for home's alerts */
    pthread_mutex_t mutex;
} pic_nws_t;

void pic_nws_init(pic_nws_t *data);
int  pic_nws_start(pic_nws_t *data);
void pic_nws_stop(void);

/* Keys: WX_BANNER=0|1, WX_BANNER_MIN=1..4, WX_RADIUS_KM=0..5000 */
void pic_nws_reload(pic_nws_t *data);

/* Colour for an event name (NWS hazard map colours where known). */
void pic_nws_event_colour(const char *event, int severity,
                          float *r, float *g, float *b);

/* Parse an ISO 8601 timestamp with UTC offset; 0 on failure. */
time_t pic_nws_parse_time(const char *s);

/*
 * Parsers, exposed for testing. Each returns the number of alerts kept.
 * parse_map appends polygons (filtered by radius around qth when
 * radius_km > 0); parse_home fills `out` sorted by severity.
 */
int pic_nws_parse_map(const char *json, size_t len, pic_nws_t *out,
                      double qth_lat, double qth_lon, int radius_km);
int pic_nws_parse_home(const char *json, size_t len,
                       pic_nws_alert_t *out, int max);

#endif /* PIC_NWSALERT_H */

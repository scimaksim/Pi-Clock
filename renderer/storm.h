/*
 * storm.h - Active tropical cyclones from GDACS
 *
 * Fetches current tropical cyclones worldwide from the Global Disaster
 * Alert and Coordination System (GDACS, European Commission JRC). For
 * Atlantic and eastern Pacific storms GDACS carries the US National
 * Hurricane Center forecast; elsewhere it uses JTWC and regional
 * centres. For each storm we keep:
 *
 *   - current position, maximum sustained wind and GDACS alert level
 *   - the track points (observed + forecast) in time order
 *   - the forecast uncertainty cone outline
 *
 * Data source: https://www.gdacs.org/gdacsapi/ (GeoJSON, no key)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_STORM_H
#define PIC_STORM_H

#include <pthread.h>
#include <time.h>

#define PIC_MAX_STORMS     12
#define STORM_TRACK_MAX    64
#define STORM_CONE_MAX     480

/* Saffir-Simpson style categories derived from max wind (km/h). */
enum {
    STORM_CAT_TD = 0,   /* Tropical depression  < 63 km/h  */
    STORM_CAT_TS,       /* Tropical storm      63-118      */
    STORM_CAT_1,        /* 119-153 */
    STORM_CAT_2,        /* 154-177 */
    STORM_CAT_3,        /* 178-208 */
    STORM_CAT_4,        /* 209-251 */
    STORM_CAT_5         /* >= 252  */
};

typedef struct {
    float lat, lon;
    unsigned char forecast;   /* 1 if this point is a forecast        */
    unsigned char cat;        /* Category of the segment leaving it   */
} pic_storm_pt_t;

typedef struct {
    char   name[32];          /* e.g. "MILTON"                         */
    char   source[8];         /* NOAA / JTWC / ...                     */
    double wind_kmh;          /* Max sustained wind (km/h)             */
    int    cat;               /* STORM_CAT_*                           */
    int    alert;             /* GDACS alert: 0 green, 1 orange, 2 red */
    double lat, lon;          /* Current position                      */

    pic_storm_pt_t track[STORM_TRACK_MAX];
    int    ntrack;

    float  cone_lat[STORM_CONE_MAX];
    float  cone_lon[STORM_CONE_MAX];
    int    ncone;

    double dist_now_km;       /* Distance from QTH now (-1 = unknown)  */
    double dist_min_km;       /* Closest forecast approach to QTH      */
} pic_storm_t;

typedef struct {
    pic_storm_t storms[PIC_MAX_STORMS];
    int         count;
    int         show_cone;        /* Draw forecast uncertainty cone   */
    int         show_track;       /* Draw past + forecast track       */
    int         min_cat;          /* Hide storms below this category  */
    time_t      last_fetched;
    pthread_mutex_t mutex;
} pic_storms_t;

void pic_storm_init(pic_storms_t *data);
int  pic_storm_start(pic_storms_t *data);
void pic_storm_stop(void);

/* Keys: STORM_CONE=0|1, STORM_TRACK=0|1, STORM_MIN_CAT=0..6 */
void pic_storm_reload(pic_storms_t *data);

/* Category from max sustained wind in km/h. */
int pic_storm_category(double wind_kmh);

/*
 * Parsers, exposed for testing.
 *
 * pic_storm_parse_events: scan a GDACS event list for current tropical
 * cyclones; fills geometry URLs (each up to url_size bytes) and returns
 * the number found.
 *
 * pic_storm_parse_geometry: fill one storm from a getgeometry response.
 * Returns 0 on success.
 */
int pic_storm_parse_events(const char *json, size_t len,
                           char urls[][256], int max);
int pic_storm_parse_geometry(const char *json, size_t len,
                             pic_storm_t *out,
                             double qth_lat, double qth_lon);

#endif /* PIC_STORM_H */

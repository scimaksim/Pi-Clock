/*
 * airquality.h - US AQI around the QTH from the Open-Meteo Air Quality API
 *
 * Samples the US Air Quality Index (and PM2.5 / ozone at the QTH) on a
 * coarse grid centred on the home location, for a smooth regional
 * overlay — wildfire smoke plumes and ozone episodes show up clearly —
 * plus the local reading for an on-map badge.
 *
 * Data: Open-Meteo Air Quality API (CAMS models), free, no key, CC BY 4.0
 *       https://air-quality-api.open-meteo.com/v1/air-quality
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_AIRQUALITY_H
#define PIC_AIRQUALITY_H

#include <pthread.h>
#include <time.h>

/* Grid: AQ_NX x AQ_NY points, AQ_STEP degrees apart, centred on QTH.
 * 17 x 11 at 1.5 deg covers ~24 x 15 degrees (e.g. most of the
 * eastern US from North Carolina). */
#define AQ_NX   17
#define AQ_NY   11
#define AQ_STEP 1.5

typedef struct {
    float  aqi[AQ_NY][AQ_NX];   /* US AQI, -1 = missing; row 0 = north */
    double lat0, lon0;          /* Grid centre (QTH at fetch time)     */
    int    have_grid;

    int    qth_aqi;             /* -1 = unknown */
    float  qth_pm25;            /* ug/m3 */
    float  qth_ozone;           /* ug/m3 */
    int    show_badge;          /* Draw the QTH AQI badge              */
    time_t fetched;
    pthread_mutex_t mutex;
} pic_airquality_t;

void pic_aq_init(pic_airquality_t *data);
int  pic_aq_start(pic_airquality_t *data);
void pic_aq_stop(void);

/* Keys: AQ_BADGE=0|1 */
void pic_aq_reload(pic_airquality_t *data);

/* EPA category colour and name for an AQI value. */
void pic_aq_colour(int aqi, double *r, double *g, double *b);
const char *pic_aq_category(int aqi);

/*
 * Parse an Open-Meteo response (single object or list) and store the
 * "current.us_aqi" of each location into vals[0..max). Also returns
 * PM2.5 / ozone of the first location. Returns locations parsed.
 */
int pic_aq_parse(const char *json, size_t len, float *vals, int max,
                 float *pm25_first, float *ozone_first);

#endif /* PIC_AIRQUALITY_H */

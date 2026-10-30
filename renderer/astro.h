/*
 * astro.h - Observing conditions: Astrospheric forecast + sun/moon times
 *
 * Two parts:
 *
 *  1. Astrospheric (Pro) Data API forecast for the QTH: hourly cloud
 *     cover, transparency (includes smoke) and astronomical seeing,
 *     each with Astrospheric's own display colour. Needs the user's
 *     API key, stored root-only in /data/etc/pi-clock-astrospheric.key
 *     (written by the dashboard; never in the shared renderer config).
 *     Forecast domain: North America, Greenland, Iceland, Ireland, UK.
 *
 *  2. Local sun/moon calculations (no network): altitude at the QTH,
 *     next rise/set, astronomical twilight, moon illumination.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_ASTRO_H
#define PIC_ASTRO_H

#include <pthread.h>
#include <time.h>

#define ASTRO_MAX_HOURS 96
#define ASTRO_KEY_PATH  "/data/etc/pi-clock-astrospheric.key"

typedef struct {
    time_t   t;              /* UTC start of the forecast hour   */
    float    cloud, transp, seeing;
    unsigned cloud_rgb, transp_rgb, seeing_rgb;   /* 0xRRGGBB */
} pic_astro_hour_t;

typedef struct {
    pic_astro_hour_t hours[ASTRO_MAX_HOURS];
    int              nhours;
    time_t           fetched;        /* Last successful forecast      */
    int              credits_left;   /* As reported by the API (-1)   */
    char             status[64];     /* Shown when there's no data    */
    int              interval_h;     /* Fetch interval, hours         */
    int              cloud_clear;    /* "Clear" threshold, % cloud    */
    volatile int     refetch;        /* Set on reload: fetch now      */
    pthread_mutex_t  mutex;
} pic_astro_t;

void pic_astro_init(pic_astro_t *data);

/* The renderer's forecast buffer, for other features that want cloud
 * cover (e.g. aurora alerts). NULL until pic_astro_init. Lock its
 * mutex while reading. */
extern pic_astro_t *pic_astro_shared;
int  pic_astro_start(pic_astro_t *data);
void pic_astro_stop(void);

/* Keys: ASTRO_INTERVAL_H=1..12, ASTRO_CLEAR_PCT=5..60 */
void pic_astro_reload(pic_astro_t *data);

/* Parse a GetForecastData response. Returns hours parsed (0 = none). */
int pic_astro_parse_forecast(const char *json, size_t len,
                             pic_astro_hour_t *out, int max,
                             int *credits_left);

/* ── Local sun / moon ─────────────────────────────────────────── */

/* Geocentric altitude in degrees at the given place and time. */
double pic_astro_sun_alt(time_t t, double lat, double lon);
double pic_astro_moon_alt(time_t t, double lat, double lon);

/* Moon illuminated fraction 0..1 and whether it is waxing. */
double pic_astro_moon_illum(time_t t, int *waxing);

/*
 * pic_astro_next_crossing - First time after t0 (within `hours`) that
 * the body's altitude crosses `h0` degrees in the given direction
 * (rising = 1: going up through h0). Returns 0 if none.
 * body: 0 = sun, 1 = moon. Accurate to ~10 seconds.
 */
time_t pic_astro_next_crossing(int body, time_t t0, double lat, double lon,
                               double h0, int rising, int hours);

/* Standard altitudes for rise/set and twilight (degrees) */
#define ASTRO_SUN_H0        (-0.833)
#define ASTRO_MOON_H0       (0.125)
#define ASTRO_TWILIGHT_H0   (-18.0)

#endif /* PIC_ASTRO_H */

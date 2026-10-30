/*
 * aircraft.h - Live aircraft positions (ADS-B)
 *
 * Sources (one is used; the local receiver wins when configured):
 *
 *   - adsb.lol community network (free, no key): every aircraft within
 *     a radius (up to 250 nm) of the QTH, plus optionally all military
 *     aircraft worldwide.
 *   - A local receiver on the LAN running readsb / dump1090 / tar1090
 *     (http://host:port/data/aircraft.json), e.g. a Pi with a
 *     FlightAware Pro Stick.
 *
 * Positions are dead-reckoned between fetches from ground speed and
 * track so the map moves smoothly.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_AIRCRAFT_H
#define PIC_AIRCRAFT_H

#include <pthread.h>
#include <time.h>

#define AC_MAX 1500

typedef struct {
    float  lat, lon;        /* Position at `t_pos`                       */
    float  track;           /* Degrees true, -1 unknown                  */
    float  gs;              /* Ground speed, knots                       */
    int    alt;             /* Barometric altitude ft, -1 = on ground    */
    char   flight[10];      /* Callsign, trimmed                         */
    char   type[6];         /* ICAO type, e.g. "B738"                    */
    unsigned char military; /* dbFlags bit 0 or from /v2/mil             */
    unsigned char emergency;/* squawk 7500/7600/7700 or emergency set    */
} pic_ac_t;

typedef struct {
    pic_ac_t ac[AC_MAX];
    int      count;
    time_t   t_pos;         /* When positions were valid                 */
    int      radius_nm;     /* Area around QTH (adsb.lol)                */
    int      show_mil;      /* Also fetch military aircraft worldwide    */
    int      labels;        /* 0 none, 1 emergencies/military, 2 all     */
    char     local[96];     /* "host:port" of a local receiver, or ""    */
    char     source[24];    /* "adsb.lol" / "local receiver"             */
    pthread_mutex_t mutex;
} pic_aircraft_t;

void pic_ac_init(pic_aircraft_t *data);
int  pic_ac_start(pic_aircraft_t *data);
void pic_ac_stop(void);

/* Keys: AIRCRAFT_RADIUS_NM=25..250, AIRCRAFT_MIL=0|1,
 *       AIRCRAFT_LABELS=0..2, AIRCRAFT_LOCAL=host[:port] */
void pic_ac_reload(pic_aircraft_t *data);

/*
 * Parse an adsb.lol v2 ("ac") or readsb ("aircraft") response, appending
 * to out[*count..max). `mil` forces the military flag. Duplicate hex
 * codes already present are skipped. Returns number added.
 */
int pic_ac_parse(const char *json, size_t len, pic_ac_t *out, int *count,
                 int max, int mil, char (*hexes)[8]);

#endif /* PIC_AIRCRAFT_H */

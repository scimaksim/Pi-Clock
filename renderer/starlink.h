/*
 * starlink.h - Recently launched Starlink "trains"
 *
 * For the first days after launch a Starlink batch flies in a tight
 * line ("train") at low altitude and is easily visible to the naked
 * eye. This module downloads CelesTrak's "launched in the last 30 days"
 * catalogue, groups STARLINK objects by launch (international
 * designator), keeps the most recent launches within the age limit,
 * and predicts the next pass of each train that is visible from the
 * QTH: train above 10 deg elevation, sunlit, observer in darkness.
 *
 * Data: CelesTrak GP data (https://celestrak.org/NORAD/elements/)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_STARLINK_H
#define PIC_STARLINK_H

#include <pthread.h>
#include <time.h>
#include "satellite.h"

#define SL_MAX_TRAINS   3
#define SL_MAX_PER_TRAIN 32

typedef struct {
    char      launch[12];        /* International designator, e.g. "2026-201" */
    time_t    launched;          /* Approximate launch time (from TLE epochs)  */
    pic_sat_t sats[SL_MAX_PER_TRAIN];
    int       nsats;

    /* Next visible pass of the train from the QTH (0 = none in 48 h) */
    time_t    pass_start, pass_max, pass_end;
    double    pass_max_el;       /* Peak elevation, degrees */
} pic_sl_train_t;

typedef struct {
    pic_sl_train_t trains[SL_MAX_TRAINS];
    int            ntrains;
    int            max_age_days;  /* Ignore launches older than this */
    time_t         fetched;
    pthread_mutex_t mutex;
} pic_starlink_t;

void pic_sl_init(pic_starlink_t *data);
int  pic_sl_start(pic_starlink_t *data);
void pic_sl_stop(void);

/* Keys: STARLINK_MAX_AGE_D=1..30 */
void pic_sl_reload(pic_starlink_t *data);

/*
 * Parse a 3-line TLE catalogue and build trains (most recent first).
 * Returns number of trains. Exposed for testing.
 */
int pic_sl_build_trains(const char *tle_text, pic_sl_train_t *out, int max,
                        time_t now, int max_age_days);

/*
 * Predict the next visible pass of a train (uses its lead satellite).
 * Fills pass_* fields; searches `hours` ahead from t0.
 */
void pic_sl_predict_pass(pic_sl_train_t *t, time_t t0, int hours,
                         double qth_lat, double qth_lon);

/* Elevation (deg) of a satellite seen from the QTH, and whether it is
 * sunlit (cylindrical Earth shadow). */
double pic_sl_elevation(const pic_sat_t *s, time_t t,
                        double qth_lat, double qth_lon, int *sunlit);

#endif /* PIC_STARLINK_H */

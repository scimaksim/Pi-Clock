/*
 * skycover.h - Cloud cover over home, for "is it visible?" questions
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PIC_SKYCOVER_H
#define PIC_SKYCOVER_H

#include <stddef.h>
#include <time.h>

#define PIC_SKY_UNKNOWN (-1)
#define PIC_SKY_CLOUDY  0
#define PIC_SKY_CLEAR   1

/* Average cloud cover (%) at home over [t0, t1]: the Astrospheric
 * forecast when it covers that time, else Open-Meteo; -1 unknown */
int pic_sky_cloud(time_t t0, time_t t1);

/* PIC_SKY_CLEAR when cloud cover is at most PASS_CLOUD_MAX (default
 * 50%), PIC_SKY_CLOUDY above it, PIC_SKY_UNKNOWN with no forecast.
 * *pct (optional) gets the cover. */
int pic_sky_clear(time_t t0, time_t t1, int *pct);

/* Fetch Open-Meteo's hourly cloud cover for home if the copy is over
 * an hour old. Call from a worker thread (it blocks on the network). */
void pic_skycover_refresh(time_t now);

/* Parse an Open-Meteo hourly cloud_cover reply into the store
 * (tests); returns the number of hours */
int pic_skycover_parse(const char *js, size_t len, time_t now);

/* Test hook: set hours directly (n hours from t0, one value each) */
void pic_skycover_set(time_t t0, const int *cloud, int n);

#endif /* PIC_SKYCOVER_H */

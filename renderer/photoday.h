/*
 * photoday.h - Picture of the day view
 *
 * A view in the cycle (VIEW_PHOTO_MIN minutes) showing one of the day's
 * space pictures, a different one each cycle: NASA's Astronomy Picture
 * of the Day, AstroBin's Image of the Day, and the picture-of-the-week
 * and -month series of ESO, NOIRLab, ESA/Webb and ESA/Hubble, with
 * NASA's Image of the Day. Everything comes from the house's Pi-Dash
 * server (PIDASH_URL), which already fetches these feeds and serves the
 * images through its cache, so the clock adds no requests to the
 * sources.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_PHOTODAY_H
#define PIC_PHOTODAY_H

#include <cairo/cairo.h>
#include <stddef.h>
#include <time.h>
#include "feature.h"

#define PD_MAX 8

typedef struct {
    char   key[12];     /* "apod", "astrobin", "eso", ...: cache file name */
    char   src[48];     /* "NASA Astronomy Picture of the Day"             */
    char   title[160];
    char   credit[160]; /* "" when the feed gives none                     */
    char   text[480];
    char   img[640];    /* Pi-Dash path, "/api/img?u=...&w=1920"           */
    time_t t;           /* when published (UTC)                            */
    int    max_days;    /* shown while no older than this                  */
} pd_item_t;

/* Parsers for the Pi-Dash replies (exposed for the tests). Each returns
 * the number of items written (0 when none is current), -1 on bad JSON. */
int pic_pd_parse_apod(const char *js, size_t len, time_t now, pd_item_t *out);
int pic_pd_parse_astrobin(const char *js, size_t len, time_t now, pd_item_t *out);
int pic_pd_parse_spacefeed(const char *js, size_t len, time_t now,
                           pd_item_t *out, int max);

/* Collapse whitespace and drop spaces before punctuation, in place */
void pic_pd_tidy(char *s);

/* An item is current while now - t <= max_days days (plus 12 h) */
int pic_pd_current(const pd_item_t *it, time_t now);

pic_feature_t *pic_feature_photoday(void);

/* Replace the list of pictures and show item `cur` (the tests;
 * images are read from $PIC_PHOTODAY_DIR/<key>.jpg) */
void pic_photoday_set_items(const pd_item_t *it, int n, int cur);

/* Draw the view (main thread). */
void pic_photoday_render(cairo_t *cr, int w, int h, time_t now);

#endif /* PIC_PHOTODAY_H */

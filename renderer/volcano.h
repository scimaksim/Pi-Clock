/*
 * volcano.h - Active volcano data from the Smithsonian / USGS
 *             Weekly Volcanic Activity Report
 *
 * Fetches the CAP (Common Alerting Protocol) edition of the Weekly
 * Volcanic Activity Report published by the Smithsonian Global
 * Volcanism Program and the USGS Volcano Hazards Program. The feed
 * lists every volcano with notable activity in the past week, with
 * its location and whether the activity is new or continuing.
 *
 * Data source: Smithsonian GVP (volcano.si.edu)
 * Format: CAP 1.1 XML, one <info> block per volcano (~20-40 entries)
 * Updated: weekly (Wednesdays/Thursdays)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_VOLCANO_H
#define PIC_VOLCANO_H

#include <pthread.h>
#include <time.h>

/* The weekly report typically lists 15-40 volcanoes. */
#define PIC_MAX_VOLCANOES 96

typedef struct {
    double lat;             /* Summit latitude (degrees)              */
    double lon;             /* Summit longitude (degrees)             */
    int    new_activity;    /* 1 = "New Activity/Unrest" section,
                             * 0 = "Continuing Activity"               */
    char   name[48];        /* Volcano name, UTF-8                     */
    char   country[48];     /* Country / area description, UTF-8       */
} pic_volcano_t;

/*
 * pic_volcanoes_t - Shared volcano buffer.
 *
 * Thread-safe: the background fetcher replaces the contents, the
 * renderer reads them.
 */
typedef struct {
    pic_volcano_t items[PIC_MAX_VOLCANOES];
    int           count;
    int           show_labels;      /* Draw volcano names            */
    int           new_only;         /* Hide "continuing" volcanoes   */
    time_t        last_fetched;
    pthread_mutex_t mutex;
} pic_volcanoes_t;

void pic_volcano_init(pic_volcanoes_t *data);

/*
 * pic_volcano_start - Start the background fetch thread.
 * Fetches immediately, then every 6 hours. Returns 0 on success.
 */
int  pic_volcano_start(pic_volcanoes_t *data);
void pic_volcano_stop(void);

/*
 * pic_volcano_reload - Re-read settings from the renderer config.
 * Keys: VOLCANO_LABELS=0|1, VOLCANO_NEW_ONLY=0|1
 */
void pic_volcano_reload(pic_volcanoes_t *data);

/*
 * pic_volcano_parse_cap - Parse a CAP XML document into items.
 * Exposed for testing. Returns the number of volcanoes parsed.
 */
int pic_volcano_parse_cap(const char *xml, pic_volcano_t *out, int max);

#endif /* PIC_VOLCANO_H */

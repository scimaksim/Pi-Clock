/*
 * features_all.h - Constructors for the pic_feature_t overlays
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PIC_FEATURES_ALL_H
#define PIC_FEATURES_ALL_H

#include "feature.h"

pic_feature_t *pic_feature_spc(void);       /* SPC convective outlook  */
pic_feature_t *pic_feature_launches(void);  /* Rocket launches         */
pic_feature_t *pic_feature_dsn(void);       /* Deep Space Network      */
pic_feature_t *pic_feature_passes(void);    /* Visible ISS/CSS passes  */
pic_feature_t *pic_feature_satclouds(void); /* GOES/Himawari IR clouds */
pic_feature_t *pic_feature_fires(void);     /* Fire hotspots + smoke   */
pic_feature_t *pic_feature_tides(void);     /* NOAA tide predictions   */
pic_feature_t *pic_feature_planets(void);   /* Planets + sky events    */
pic_feature_t *pic_feature_ships(void);     /* AIS vessel positions    */
pic_feature_t *pic_feature_aurorawatch(void);    /* Aurora view line   */
pic_feature_t *pic_feature_hanotify(void);       /* Home Assistant alerts */
pic_feature_t *pic_feature_lsr(void);            /* Storm reports      */
pic_feature_t *pic_feature_rainfall(void);       /* MRMS rain totals   */
pic_feature_t *pic_feature_rivers(void);         /* River gauges       */
pic_feature_t *pic_feature_ebird(void);          /* Rare birds         */
pic_feature_t *pic_feature_drought(void);        /* Drought Monitor    */
pic_feature_t *pic_feature_coast(void);          /* Buoys panel        */
pic_feature_t *pic_feature_backyard(void);       /* Backyard panel     */
pic_feature_t *pic_feature_twilight(void);       /* Twilight lines     */
pic_feature_t *pic_feature_darksites(void);      /* Dark-sky sites     */
pic_feature_t *pic_feature_overhead(void);       /* Live sats in view  */
pic_feature_t *pic_feature_sst(void);            /* Sea-surface temp   */
pic_feature_t *pic_feature_seasons(void);        /* Seasons panel      */
pic_feature_t *pic_feature_analemma(void);       /* Analemma panel     */
/* Layers computed by the PC helper (layers_pchelper.c) */
pic_feature_t *pic_feature_pch_nowcast(void);
pic_feature_t *pic_feature_pch_clouds(void);
pic_feature_t *pic_feature_pch_smoke(void);
pic_feature_t *pic_feature_pch_gust(void);
pic_feature_t *pic_feature_pch_frost(void);
pic_feature_t *pic_feature_pch_birds(void);

/* ── Data shared with the Home Assistant notifier ─────────────── */

typedef struct {
    char   sat[16];
    time_t start, max, end;
    double max_el;
    double az_start, az_end;
    int    sat_idx;          /* Internal: which TLE produced it */
} pic_pass_t;

/* Copy the predicted visible passes (sorted by start). */
int pic_passes_get(pic_pass_t *out, int max);

/* Upcoming launches from nearby pads predicted to be visible from home
 * (layers_launches.c), soonest first */
#include "launchvis.h"
typedef struct {
    char   rocket[40], mission[64], pad[48], status[16];
    time_t net;
    int    precise;          /* NET known to the minute */
    pic_launchvis_t vis;
} pic_launch_alert_t;
int pic_launches_visible(pic_launch_alert_t *out, int max);

typedef struct {
    int    have_data;
    float  kp_now;
    int    now_level;     /* 0 none, 1 photo, 2 visible (dark at home) */
    time_t next_t;        /* First forecast night block meeting photo  */
    float  next_kp;
    int    next_level;
    int    clouds_now, clouds_next;   /* Astrospheric %, -1 unknown   */
} pic_aurora_home_t;

void pic_aurora_home(time_t now, pic_aurora_home_t *out);

/* Phone-alert settings shared by passes/aurora "wanted" hooks */
#ifndef HA_WEBHOOK_PATH
#define HA_WEBHOOK_PATH "/data/etc/pi-clock-ha-webhook"
#endif
int pic_hanotify_wants(const char *flag_key);
pic_feature_t *pic_feature_lightpollution(void); /* Sky brightness map */

#endif

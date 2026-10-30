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

/* ── Data shared with the Home Assistant notifier ─────────────── */

typedef struct {
    char   sat[16];
    time_t start, max, end;
    double max_el;
    double az_start, az_end;
} pic_pass_t;

/* Copy the predicted visible passes (sorted by start). */
int pic_passes_get(pic_pass_t *out, int max);

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
#define HA_WEBHOOK_PATH "/data/etc/pi-clock-ha-webhook"
int pic_hanotify_wants(const char *flag_key);
pic_feature_t *pic_feature_lightpollution(void); /* Sky brightness map */

#endif

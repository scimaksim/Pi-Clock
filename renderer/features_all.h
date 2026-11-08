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
pic_feature_t *pic_feature_planets(void);
pic_feature_t *pic_feature_eclipse(void);   /* Live eclipse shadows    */
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
pic_feature_t *pic_feature_events(void);         /* Coming-up strip    */
pic_feature_t *pic_feature_health(void);         /* Vital-signs log    */
pic_feature_t *pic_feature_crew(void);           /* People in space    */
pic_feature_t *pic_feature_reentries(void);      /* Space-Track TIP     */
pic_feature_t *pic_feature_conjunctions(void);   /* Close approaches   */
pic_feature_t *pic_feature_geobelt(void);        /* Geostationary belt */
pic_feature_t *pic_feature_debris(void);         /* Newest debris      */
/* People aboard "ISS" / "Tiangong" (crew.c); -1 unknown */
int pic_crew_aboard(const char *station);
int pic_crew_parse(const char *js, size_t len);
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

/* Next major meteor shower peak evening (layers_planets.c); 0 if none */
time_t pic_planets_next_shower(time_t now, char *name, size_t n, int *zhr);
/* When rain reaches home / eases, from the PC helper nowcast; 0 unknown */
time_t pic_pch_rain_change(time_t now, int *raining);

/*
 * Upcoming events worth stepping outside for (applet_events.c): the
 * strip along the bottom of the map and the view switcher use these.
 */
typedef enum { PIC_EV_PASS, PIC_EV_LAUNCH, PIC_EV_AURORA, PIC_EV_SHOWER,
               PIC_EV_MOONRISE, PIC_EV_MOONSET, PIC_EV_DARK, PIC_EV_RAIN } pic_event_kind_t;
typedef struct {
    pic_event_kind_t kind;
    time_t start, end;       /* end 0 = instant */
    char   text[64];         /* "Hubble pass", "Falcon 9 launch" */
    char   detail[48];       /* "max 38 deg W->S", "look SSE" */
    int    outdoors;         /* worth going outside for right at start */
} pic_event_t;
int pic_events_collect(time_t now, pic_event_t *out, int max);

/* The Pi's own vital signs (health.c) */
#define PIC_HEALTH_PATH "/data/cache/pi-clock-health.csv"
typedef struct {
    int    restarts_24h;
    double temp_c, max_temp_24h;
    long   avail_mb, rss_mb, data_free_mb;
} pic_health_t;
int pic_health_summary(time_t now, pic_health_t *out);

/* Phone-alert settings shared by passes/aurora "wanted" hooks */
#ifndef HA_WEBHOOK_PATH
#define HA_WEBHOOK_PATH "/data/etc/pi-clock-ha-webhook"
#endif
int pic_hanotify_wants(const char *flag_key);
pic_feature_t *pic_feature_lightpollution(void); /* Sky brightness map */

#endif

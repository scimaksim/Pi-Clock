/*
 * planetview.c - The Moon, Mars and Jupiter as full-screen geochrons
 *
 * When the view cycle (localview.c) shows another world, display.c
 * hides the Earth layers and paints this instead:
 *
 *   - a global map (LRO/LROC colour Moon, USGS Viking MDIM colour Mars,
 *     Cassini Jupiter), equirectangular, planetocentric latitude,
 *     east longitude, 0 deg in the middle
 *   - day and night from the sub-solar point (on the Moon the night
 *     side is lit by earthshine, strongest where Earth is overhead)
 *   - every lander, rover and crash site at its published coordinates,
 *     drawn as a rendered 3D sprite (NASA 3D models) or a shaded icon,
 *     with callouts placed so they don't overlap
 *   - every orbiter at its JPL Horizons position, with its ground track
 *     for the last and next ~45 minutes, and the moons of Mars and
 *     Jupiter with their shadows on the planet
 *   - Mars: Curiosity and Perseverance from JPL's own rover feeds
 *     (end-of-drive localizations), each in an inset on the HiRISE
 *     base map with its traverse and the latest drive replayed
 *   - Jupiter: the 2000 Cassini map shifted so the Great Red Spot sits
 *     at its present longitude (System II estimate, settable)
 *
 * Positions come from a fetch thread (pic_feature_worlds): Horizons
 * observer tables from each orbiter / moon (sub-observer point,
 * sub-solar point, range), refreshed to cover the next few hours, and
 * the MMGIS JSON + map tiles for the rovers. Everything is cached in
 * CACHE_DIR so a restart without network still draws.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "planetview.h"
#include "craft_icons.h"
#include "fetch.h"
#include "image.h"
#include "feature.h"
#include "display.h"
#include "localview.h"
#include <cairo/cairo.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#define D2R (M_PI / 180.0)
#define WORLDS_DIR "/usr/share/pi-clock/worlds"
#define CACHE_DIR  "/data/cache/pi-clock-worlds"
#define HZ_URL "https://ssd.jpl.nasa.gov/api/horizons.api?format=text" \
    "&COMMAND=%%27%s%%27&CENTER=%%27500@%s%%27&MAKE_EPHEM=YES&EPHEM_TYPE=OBSERVER" \
    "&START_TIME=%%27%s%%27&STOP_TIME=%%27%s%%27&STEP_SIZE=%%27%d%%20m%%27" \
    "&QUANTITIES=%%2714,15,20%%27&CSV_FORMAT=YES&ANG_FORMAT=DEG&EXTRA_PREC=YES"
#define MMGIS "https://mars.nasa.gov/mmgis-maps/"
#define HZ_BEFORE (2 * 3600)      /* series cover now-2h .. now+6h       */
#define HZ_AFTER  (6 * 3600)
#define HZ_MARGIN (3 * 3600)      /* refetch when less than 3 h is left   */
#define HZ_BUF    (160 * 1024)

extern double pic_ticker_bar_top __attribute__((weak));

static const char *dir_worlds(void)
{
    const char *e = getenv("PIC_WORLDS_DIR");
    return e && *e ? e : WORLDS_DIR;
}

static const char *dir_cache(void)
{
    const char *e = getenv("PIC_WORLDS_CACHE");
    return e && *e ? e : CACHE_DIR;
}

static double wrap180(double x)
{
    x = fmod(x + 180.0, 360.0);
    if (x < 0) x += 360.0;
    return x - 180.0;
}

static double clampd(double x, double a, double b)
{
    return x < a ? a : x > b ? b : x;
}

/* ── 1. Worlds ─────────────────────────────────────────────────── */

typedef struct {
    const char *key, *name;
    double a, b, mean;        /* equatorial / polar / mean radius, km  */
    int west;                 /* Horizons longitudes west-positive      */
    const char *credit;
} world_t;

static const world_t WORLDS[PIC_WORLD_N] = {
    [PIC_WORLD_MOON] = { "moon", "Moon", 1737.4, 1737.4, 1737.4, 0,
        "Map: NASA/GSFC/Arizona State Univ. (LRO LROC WAC colour, LOLA relief, SVS CGI Moon Kit) \xc2\xb7 "
        "Sites: LROC / NSSDCA \xc2\xb7 Orbiters: JPL Horizons" },
    [PIC_WORLD_MARS] = { "mars", "Mars", 3396.19, 3376.20, 3389.5, 1,
        "Map: NASA/USGS Viking MDIM colour, MGS MOLA relief \xc2\xb7 Rovers: NASA/JPL-Caltech MMGIS, "
        "HiRISE NASA/JPL-Caltech/UArizona \xc2\xb7 Orbiters & moons: JPL Horizons" },
    [PIC_WORLD_JUPITER] = { "jupiter", "Jupiter", 71492, 66854, 69911, 1,
        "Map: NASA/JPL/Space Science Institute (Cassini, Dec 2000) \xc2\xb7 "
        "Juno & moons: JPL Horizons" },
};

int pic_world_by_name(const char *name)
{
    int i;
    if (!name) return 0;
    for (i = 1; i < PIC_WORLD_N; i++)
        if (strcasecmp(name, WORLDS[i].key) == 0) return i;
    return 0;
}

const char *pic_world_name(int world)
{
    return world > 0 && world < PIC_WORLD_N ? WORLDS[world].name : "Earth";
}

/* ── 2. Surface sites ──────────────────────────────────────────── */

enum { ST_ENDED, ST_ACTIVE, ST_UNKNOWN, ST_CRASH };

typedef struct {
    int world;
    const char *name;     /* callout line 1                            */
    const char *when;     /* callout line 2                            */
    double lat, lon;      /* planetocentric, east-positive             */
    int kind, kind2;      /* icon (kind2 >= 0: a second, smaller one)  */
    const char *sprite;   /* rendered 3D model, if there is one        */
    int status;
    int approx;           /* position uncertain / approximate          */
} site_t;

#define NOK (-1)
static const site_t SITES[] = {
    /* The Moon: LROC-identified hardware (Wagner et al.) and NSSDCA */
    { PIC_WORLD_MOON, "Luna 2", "impact 1959", 29.1, 0.0, PIC_CRAFT_CRASH, NOK, NULL, ST_CRASH, 1 },
    { PIC_WORLD_MOON, "Luna 9", "1966 \xc2\xb7 first soft landing", 7.13, -64.37, PIC_CRAFT_LUNA_ALS, NOK, NULL, ST_ENDED, 1 },
    { PIC_WORLD_MOON, "Luna 13", "1966", 18.87, -62.05, PIC_CRAFT_LUNA13, NOK, NULL, ST_ENDED, 1 },
    { PIC_WORLD_MOON, "Surveyor 1", "1966", -2.4745, -43.3398, PIC_CRAFT_SURVEYOR, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Surveyor 5", "1967", 1.4551, 23.1943, PIC_CRAFT_SURVEYOR, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Surveyor 6", "1967", 0.4742, -1.4275, PIC_CRAFT_SURVEYOR, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Surveyor 7", "1968", -40.9812, -11.5127, PIC_CRAFT_SURVEYOR, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Apollo 11", "1969", 0.67416, 23.47314, PIC_CRAFT_APOLLO_LM, NOK, "apollo_lm", ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Apollo 12 + Surveyor 3", "1969 \xc2\xb7 1967", -3.0128, -23.4219, PIC_CRAFT_APOLLO_LM, PIC_CRAFT_SURVEYOR, "apollo_lm", ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Apollo 14", "1971", -3.64589, -17.47194, PIC_CRAFT_APOLLO_LM, NOK, "apollo_lm", ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Apollo 15", "1971 \xc2\xb7 first rover", 26.13239, 3.63330, PIC_CRAFT_APOLLO_LM, PIC_CRAFT_APOLLO_LM_LRV, "apollo_lm", ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Apollo 16", "1972", -8.9734, 15.5011, PIC_CRAFT_APOLLO_LM, PIC_CRAFT_APOLLO_LM_LRV, "apollo_lm", ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Apollo 17", "1972", 20.1911, 30.7723, PIC_CRAFT_APOLLO_LM, PIC_CRAFT_APOLLO_LM_LRV, "apollo_lm", ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Luna 16", "1970 \xc2\xb7 sample return", -0.5137, 56.3638, PIC_CRAFT_LUNA_SR, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Lunokhod 1", "Luna 17 \xc2\xb7 1970", 38.3150, -35.0081, PIC_CRAFT_LUNOKHOD, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Luna 20", "1972 \xc2\xb7 sample return", 3.7863, 56.6242, PIC_CRAFT_LUNA_SR, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Lunokhod 2", "Luna 21 \xc2\xb7 1973 \xc2\xb7 39 km", 25.8323, 30.9222, PIC_CRAFT_LUNOKHOD, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Luna 24", "1976 \xc2\xb7 sample return", 12.7142, 62.2129, PIC_CRAFT_LUNA_SR, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Chang'e 3 + Yutu", "2013", 44.1214, -19.5117, PIC_CRAFT_CHANGE_LANDER, PIC_CRAFT_YUTU, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Chang'e 4 + Yutu-2", "2019 \xc2\xb7 far side", -45.4562, 177.5885, PIC_CRAFT_CHANGE_LANDER, PIC_CRAFT_YUTU, NULL, ST_UNKNOWN, 0 },
    { PIC_WORLD_MOON, "Chang'e 5", "2020 \xc2\xb7 sample return", 43.0576, -51.9161, PIC_CRAFT_CHANGE5, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Chang'e 6", "2024 \xc2\xb7 far-side samples", -41.6385, -153.9852, PIC_CRAFT_CHANGE5, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Chandrayaan-3", "Vikram + Pragyan \xc2\xb7 2023", -69.3741, 32.32, PIC_CRAFT_VIKRAM, PIC_CRAFT_PRAGYAN, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "SLIM", "2024 \xc2\xb7 landed on its nose", -13.3160, 25.2510, PIC_CRAFT_SLIM, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "IM-1 Odysseus", "2024 \xc2\xb7 on its side", -80.13, 1.44, PIC_CRAFT_NOVA_C, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "IM-2 Athena", "2025 \xc2\xb7 tipped over", -84.7906, 29.1957, PIC_CRAFT_NOVA_C, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Blue Ghost 1", "2025", 18.5623, 61.8103, PIC_CRAFT_BLUE_GHOST, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Beresheet", "crash 2019", 32.5956, 19.3496, PIC_CRAFT_CRASH, NOK, NULL, ST_CRASH, 0 },
    { PIC_WORLD_MOON, "Vikram (Ch-2)", "crash 2019", -70.8810, 22.7840, PIC_CRAFT_CRASH, NOK, NULL, ST_CRASH, 0 },
    { PIC_WORLD_MOON, "Hakuto-R M1", "crash 2023", 47.581, 44.094, PIC_CRAFT_CRASH, NOK, NULL, ST_CRASH, 0 },
    { PIC_WORLD_MOON, "Luna 25", "crash 2023", -57.865, 61.360, PIC_CRAFT_CRASH, NOK, NULL, ST_CRASH, 0 },
    { PIC_WORLD_MOON, "Hakuto-R M2", "crash 2025", 60.4445, -4.5880, PIC_CRAFT_CRASH, NOK, NULL, ST_CRASH, 0 },

    /* Mars (planetocentric, east) */
    { PIC_WORLD_MARS, "Mars 3", "1971 \xc2\xb7 probable site", -45.0, -158.0, PIC_CRAFT_MARS3, NOK, NULL, ST_ENDED, 1 },
    { PIC_WORLD_MARS, "Viking 1", "1976", 22.269, -47.951, PIC_CRAFT_VIKING_LANDER, NOK, "viking", ST_ENDED, 0 },
    { PIC_WORLD_MARS, "Viking 2", "1976", 47.6673, 134.2809, PIC_CRAFT_VIKING_LANDER, NOK, "viking", ST_ENDED, 0 },
    { PIC_WORLD_MARS, "Pathfinder + Sojourner", "1997", 19.095, -33.253, PIC_CRAFT_PATHFINDER, PIC_CRAFT_SOJOURNER, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MARS, "Beagle 2", "2003", 11.517, 90.437, PIC_CRAFT_BEAGLE2, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MARS, "Spirit", "2004\xe2\x80\x93" "2010 \xc2\xb7 7.7 km", -14.6, 175.5, PIC_CRAFT_MER, NOK, "mer", ST_ENDED, 0 },
    { PIC_WORLD_MARS, "Opportunity", "2004\xe2\x80\x93" "2018 \xc2\xb7 landing site", -1.9462, -5.5266, PIC_CRAFT_MER, NOK, "mer", ST_ENDED, 0 },
    { PIC_WORLD_MARS, "Phoenix", "2008", 68.22, -125.75, PIC_CRAFT_PHOENIX, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MARS, "Schiaparelli", "crash 2016", -2.07, -6.21, PIC_CRAFT_CRASH, NOK, NULL, ST_CRASH, 0 },
    { PIC_WORLD_MARS, "InSight", "2018\xe2\x80\x93" "2022", 4.50238, 135.62345, PIC_CRAFT_PHOENIX, NOK, "insight", ST_ENDED, 0 },
    { PIC_WORLD_MARS, "Zhurong", "2021\xe2\x80\x93" "22 \xc2\xb7 landing site", 25.066, 109.925, PIC_CRAFT_ZHURONG, NOK, NULL, ST_ENDED, 0 },

    /* Jupiter (System III, planetocentric) */
    { PIC_WORLD_JUPITER, "Galileo probe", "entry 1995", 6.5, -4.46, PIC_CRAFT_CRASH, NOK, NULL, ST_CRASH, 1 },
};
#define NSITES ((int)(sizeof(SITES) / sizeof(SITES[0])))

/* ── 3. Bodies with Horizons ephemerides ───────────────────────── */

enum { B_SUNREF, B_ORBITER, B_MOON, B_AUX };

typedef struct {
    int world;
    const char *key;       /* cache file name                           */
    const char *name;
    const char *target;    /* COMMAND = the planet                      */
    const char *center;    /* observer: the craft / moon                */
    int step;              /* minutes                                   */
    int type;
    int kind;              /* craft icon, or PIC_MOON_* for moons      */
    const char *sprite;
    double radius;         /* moons: km (for the shadow)                */
    /* runtime */
    pic_subpt_t *obs, *sun;
    int n;
    double t0, t1;         /* coverage                                  */
    time_t tried;
} body_t;

#define RT NULL, NULL, 0, 0, 0, 0
static body_t BODIES[] = {
    { PIC_WORLD_MOON, "moon_earth", "Earth", "301", "399", 10, B_SUNREF, 0, NULL, 0, RT },
    { PIC_WORLD_MOON, "moon_lro", "LRO", "301", "-85", 1, B_ORBITER, PIC_CRAFT_LRO, "lro", 0, RT },
    { PIC_WORLD_MOON, "moon_ch2", "Chandrayaan-2", "301", "-152", 1, B_ORBITER, PIC_CRAFT_CHANDRAYAAN2, NULL, 0, RT },
    { PIC_WORLD_MOON, "moon_danuri", "Danuri", "301", "-155", 1, B_ORBITER, PIC_CRAFT_DANURI, NULL, 0, RT },
    { PIC_WORLD_MARS, "mars_phobos", "Phobos", "499", "401", 2, B_MOON, PIC_MOON_PHOBOS, NULL, 11.08, RT },
    { PIC_WORLD_MARS, "mars_deimos", "Deimos", "499", "402", 2, B_MOON, PIC_MOON_DEIMOS, NULL, 6.2, RT },
    { PIC_WORLD_MARS, "mars_mro", "MRO", "499", "-74", 1, B_ORBITER, PIC_CRAFT_MRO, "mro", 0, RT },
    { PIC_WORLD_MARS, "mars_ody", "Mars Odyssey", "499", "-53", 1, B_ORBITER, PIC_CRAFT_ODYSSEY, "odyssey", 0, RT },
    { PIC_WORLD_MARS, "mars_mex", "Mars Express", "499", "-41", 1, B_ORBITER, PIC_CRAFT_MEX, NULL, 0, RT },
    { PIC_WORLD_MARS, "mars_tgo", "ExoMars TGO", "499", "-143", 1, B_ORBITER, PIC_CRAFT_TGO, NULL, 0, RT },
    { PIC_WORLD_MARS, "mars_hope", "Hope", "499", "-62", 1, B_ORBITER, PIC_CRAFT_HOPE, NULL, 0, RT },
    { PIC_WORLD_JUPITER, "jup_io", "Io", "599", "501", 2, B_MOON, PIC_MOON_IO, NULL, 1821.6, RT },
    { PIC_WORLD_JUPITER, "jup_europa", "Europa", "599", "502", 2, B_MOON, PIC_MOON_EUROPA, NULL, 1560.8, RT },
    { PIC_WORLD_JUPITER, "jup_ganymede", "Ganymede", "599", "503", 2, B_MOON, PIC_MOON_GANYMEDE, NULL, 2634.1, RT },
    { PIC_WORLD_JUPITER, "jup_callisto", "Callisto", "599", "504", 2, B_MOON, PIC_MOON_CALLISTO, NULL, 2410.3, RT },
    { PIC_WORLD_JUPITER, "jup_juno", "Juno", "599", "-61", 5, B_ORBITER, PIC_CRAFT_JUNO, "juno", 0, RT },
    /* Juno's orbit 50 days ahead (to find the next close pass), and that
     * pass at fine steps */
    { PIC_WORLD_JUPITER, "jup_juno_orbit", "Juno orbit", "599", "-61", 120, B_AUX, 0, NULL, 0, RT },
    { PIC_WORLD_JUPITER, "jup_juno_pj", "Juno close pass", "599", "-61", 2, B_AUX, 0, NULL, 0, RT },
    /* distance from Earth, for the radio delay */
    { PIC_WORLD_MARS, "mars_earth", "Earth-Mars", "499", "399", 60, B_AUX, 0, NULL, 0, RT },
    { PIC_WORLD_JUPITER, "jup_earth", "Earth-Jupiter", "599", "399", 60, B_AUX, 0, NULL, 0, RT },
};
#define NBODIES ((int)(sizeof(BODIES) / sizeof(BODIES[0])))

static pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;

/* Where each world takes its sub-solar point from: the closest
 * observer, so light time doesn't matter (Earth for the Moon: 1.3 s) */
static const char *SUNREF[PIC_WORLD_N] = { NULL, "moon_earth", "mars_phobos", "jup_io" };

static body_t *body_by_key(const char *key)
{
    int i;
    for (i = 0; key && i < NBODIES; i++)
        if (strcmp(BODIES[i].key, key) == 0) return &BODIES[i];
    return NULL;
}

static double world_f(int w)
{
    return (WORLDS[w].a - WORLDS[w].b) / WORLDS[w].a;
}

/* Radius of world w's ellipsoid at planetocentric latitude lat (km),
 * for altitudes above the surface / the 1-bar level */
static double world_radius(int w, double lat)
{
    double a = WORLDS[w].a, b = WORLDS[w].b, c = cos(lat * D2R), s2 = sin(lat * D2R);
    return a * b / sqrt(b * b * c * c + a * a * s2 * s2);
}

/* Parse a Horizons reply into b (called with data_lock NOT held) */
static int body_load_text(body_t *b, const char *txt)
{
    int max = 0, n;
    const char *p;
    pic_subpt_t *obs, *sun;
    for (p = txt; (p = strchr(p, '\n')) != NULL; p++) max++;
    if (max < 2) return 0;
    obs = calloc((size_t)max, sizeof(*obs));
    sun = calloc((size_t)max, sizeof(*sun));
    if (!obs || !sun) { free(obs); free(sun); return 0; }
    n = pic_horizons_parse(txt, WORLDS[b->world].west, world_f(b->world),
                           b->world != PIC_WORLD_MOON, obs, sun, max, 149597870.7);
    if (n < 2) { free(obs); free(sun); return 0; }
    pthread_mutex_lock(&data_lock);
    free(b->obs); free(b->sun);
    b->obs = obs; b->sun = sun; b->n = n;
    b->t0 = obs[0].t; b->t1 = obs[n - 1].t;
    pthread_mutex_unlock(&data_lock);
    return n;
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 8 * 1024 * 1024) { fclose(f); return NULL; }
    buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    if (buf) buf[n] = 0;
    if (len) *len = buf ? (size_t)n : 0;
    fclose(f);
    return buf;
}

static int write_file(const char *path, const char *data, size_t n)
{
    char tmp[300];
    FILE *f;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) return -1;
    if (fwrite(data, 1, n, f) != n) { fclose(f); unlink(tmp); return -1; }
    fclose(f);
    return rename(tmp, path);
}

static void body_load_cache(body_t *b)
{
    char path[256], *txt;
    snprintf(path, sizeof(path), "%s/hz_%s.txt", dir_cache(), b->key);
    txt = slurp(path, NULL);
    if (txt) body_load_text(b, txt);
    free(txt);
}

/* Unit vector of (lon, lat) */
static void unitv(double lon, double lat, double v[3])
{
    v[0] = cos(lat * D2R) * cos(lon * D2R);
    v[1] = cos(lat * D2R) * sin(lon * D2R);
    v[2] = sin(lat * D2R);
}

/*
 * Position in a series at t, interpolated on the sphere (not in
 * lon/lat, which goes wrong over the poles where polar orbiters
 * spend half their time). Caller holds data_lock.
 */
static int series_at(const pic_subpt_t *s, int n, double t, pic_subpt_t *out)
{
    int lo = 0, hi = n - 1;
    double f, a[3], b[3], v[3], m;
    if (n < 2 || t < s[0].t || t > s[n - 1].t) return 0;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (s[mid].t <= t) lo = mid; else hi = mid;
    }
    f = s[hi].t > s[lo].t ? (t - s[lo].t) / (s[hi].t - s[lo].t) : 0;
    unitv(s[lo].lon, s[lo].lat, a);
    unitv(s[hi].lon, s[hi].lat, b);
    v[0] = a[0] + f * (b[0] - a[0]);
    v[1] = a[1] + f * (b[1] - a[1]);
    v[2] = a[2] + f * (b[2] - a[2]);
    m = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (m < 1e-9) return 0;
    out->t = t;
    out->lon = atan2(v[1], v[0]) / D2R;
    out->lat = asin(clampd(v[2] / m, -1, 1)) / D2R;
    out->r = s[lo].r + f * (s[hi].r - s[lo].r);
    return 1;
}

/* Sub-solar point of world w at t; 0 if unknown. Caller holds data_lock. */
static int sun_at(int w, double t, double *lon, double *lat)
{
    body_t *b = body_by_key(SUNREF[w]);
    pic_subpt_t p;
    int i;
    if (b && b->sun && series_at(b->sun, b->n, t, &p)) {
        *lon = p.lon; *lat = p.lat;
        return 1;
    }
    for (i = 0; i < NBODIES; i++)      /* any other observer of w (not Earth: light time) */
        if (BODIES[i].world == w && BODIES[i].sun && strcmp(BODIES[i].center, "399") &&
            series_at(BODIES[i].sun, BODIES[i].n, t, &p)) {
            *lon = p.lon; *lat = p.lat;
            return 1;
        }
    return 0;
}

/* ── 4. Mars rovers (JPL MMGIS feeds) ──────────────────────────── */

typedef struct {
    const char *key, *name, *mission, *sprite;
    int kind, naif;               /* DSN target id (NAIF, no sign)      */
    double land_t, land_lon;      /* for today's mission sol            */
    /* runtime (under data_lock) */
    int have;
    double lat, lon, yaw, dist_total;
    int sol;
    char rmc[24];
    double *pts;                  /* traverse lon,lat pairs             */
    int npts;
    int *feat_start, nfeat;       /* index of each drive's first point  */
    int *feat_sol;
    double last_len;
    int last_sol;
    /* inset tiles */
    int z, tx0, ty0, tw, th;      /* tile block (XYZ rows)              */
    double ox, oy;                /* inset origin in zoom-z pixels      */
    int tiles_ready;
    cairo_surface_t *inset;       /* built on the render thread         */
    /* latest raw image from the rover's cameras */
    char photo_url[320], photo_cam[40], photo_date[24];
    int photo_sol, photo_rank, photo_gen, photo_loaded;
    cairo_surface_t *photo;
    int inset_z, inset_tx0, inset_ty0;
    time_t checked;
    /* the picture's camera model (CAHV, rover frame) and the rover's
     * attitude (quaternion rover -> site: north, east, down) */
    int geom;
    double cC[3], cA[3], cH[3], cV[3], q[4];
    double sub_x0, sub_y0, sub_w, sub_h, img_scale;   /* subframe, downsampling */
    /* zoomed view: tiles at ZOOM_Z around the rover */
    int ztx0, zty0, ztw, zth, ztiles_ready;
    double zox, zoy;
    cairo_surface_t *zoom;
    double zoom_ox, zoom_oy;
    /* elevation patch around the rover (tools/dem_patch.py) */
    float *dem;
    int dem_w, dem_h;
    double dem_lat, dem_lon, dem_step;
    time_t dem_mtime;
} rover_t;

static rover_t ROVERS[] = {
    { .key = "MSL", .name = "Curiosity", .mission = "MSL", .sprite = "curiosity",
      .kind = PIC_CRAFT_MSL, .naif = 76, .land_t = 1344230277.0, .land_lon = 137.4417 },
    { .key = "M20", .name = "Perseverance", .mission = "M20", .sprite = "perseverance",
      .kind = PIC_CRAFT_MSL, .naif = 168, .land_t = 1613681700.0, .land_lon = 77.4509 },
};
#define NROVERS 2

static const char *ROVER_TILES[NROVERS] = {
    MMGIS "MSL/Layers/MSL_Gale_HiRISE-LRGB_78quads",
    MMGIS "M20/Layers/M20_HiRISE_RGB_CTX_mosaic_25cm_v7_Merge_LERC_clip",
};

static struct { int have; double lat, lon, total_km; int flight; } heli;

/* Curiosity's latest published weather (REMS, Centro de Astrobiologia
 * via mars.nasa.gov): newest sol first in the feed */
#define MSL_WX_URL "https://mars.nasa.gov/rss/api/?feed=weather&category=msl&feedtype=json"
static struct {
    int have, sol;
    char date[16], tmin[8], tmax[8], pres[8], sky[24];
} wx;

static int json_str(const char *p, const char *key, char *out, int n);

static void wx_parse(const char *txt)
{
    const char *p = txt ? strstr(txt, "\"soles\"") : NULL;
    char sol[12] = "";
    if (!p) return;
    pthread_mutex_lock(&data_lock);
    json_str(p, "\"sol\"", sol, sizeof(sol));
    json_str(p, "\"terrestrial_date\"", wx.date, sizeof(wx.date));
    json_str(p, "\"min_temp\"", wx.tmin, sizeof(wx.tmin));
    json_str(p, "\"max_temp\"", wx.tmax, sizeof(wx.tmax));
    json_str(p, "\"pressure\"", wx.pres, sizeof(wx.pres));
    json_str(p, "\"atmo_opacity\"", wx.sky, sizeof(wx.sky));
    wx.sol = atoi(sol);
    wx.have = wx.sol > 0 && wx.date[0];
    pthread_mutex_unlock(&data_lock);
}

/* Find "key" (with quotes) after p and read the number after ':' */
static int json_num(const char *p, const char *key, double *out)
{
    const char *k = strstr(p, key);
    char *e;
    if (!k) return 0;
    k = strchr(k + strlen(key), ':');
    if (!k) return 0;
    k++;
    while (*k == ' ' || *k == '"') k++;
    *out = strtod(k, &e);
    return e != k;
}

static int json_str(const char *p, const char *key, char *out, int n)
{
    const char *k = strstr(p, key), *q;
    int i = 0;
    if (!k) return 0;
    k = strchr(k + strlen(key), ':');
    if (!k) return 0;
    q = strchr(k, '"');
    if (!q) return 0;
    for (q++; *q && *q != '"' && i < n - 1; q++) out[i++] = *q;
    out[i] = 0;
    return 1;
}

/* Today's mission sol: local mean solar days since the landing sol 0 */
static int mission_sol(const rover_t *r, double t)
{
    double a = pic_mars_msd(t) + r->land_lon / 360.0;
    double b = pic_mars_msd(r->land_t) + r->land_lon / 360.0;
    return (int)floor(a) - (int)floor(b);
}

static int rover_parse_current(rover_t *r, const char *txt)
{
    double lat, lon, sol, yaw = 0, dist = 0;
    char rmc[24] = "";
    if (!json_num(txt, "\"lat\"", &lat) || !json_num(txt, "\"lon\"", &lon) ||
        !json_num(txt, "\"sol\"", &sol)) return 0;
    json_num(txt, "\"yaw\"", &yaw);
    json_num(txt, "\"dist_total_m\"", &dist);
    json_str(txt, "\"RMC\"", rmc, sizeof(rmc));
    pthread_mutex_lock(&data_lock);
    r->have = 1;
    r->lat = lat; r->lon = lon; r->yaw = yaw; r->dist_total = dist;
    r->sol = (int)sol;
    snprintf(r->rmc, sizeof(r->rmc), "%s", rmc);
    pthread_mutex_unlock(&data_lock);
    return 1;
}

/* Traverse GeoJSON: LineStrings of [lon, lat, z], one per drive */
static int rover_parse_traverse(rover_t *r, const char *txt)
{
    const char *p = txt;
    int cap = 4096, n = 0, fcap = 512, nf = 0, cur_sol = -1;
    double *pts = malloc(sizeof(double) * 2 * (size_t)cap), last_len = 0, len;
    int *fs = malloc(sizeof(int) * (size_t)fcap), *fsol = malloc(sizeof(int) * (size_t)fcap);
    if (!pts || !fs || !fsol) goto fail;
    for (;;) {
        const char *c = strstr(p, "\"coordinates\"");
        const char *props = strstr(p, "\"properties\"");
        double sol;
        if (!c) break;
        /* properties of this feature (they come before its geometry) */
        if (props && props < c) {
            cur_sol = json_num(props, "\"sol\"", &sol) && strstr(props, "\"sol\"") < c ? (int)sol : -1;
            len = 0;
            if ((json_num(props, "\"length\"", &len) && strstr(props, "\"length\"") < c) ||
                (json_num(props, "\"Length\"", &len) && strstr(props, "\"Length\"") < c))
                last_len = len;
        }
        if (nf >= fcap) {
            int *a, *b2;
            fcap *= 2;
            a = realloc(fs, sizeof(int) * (size_t)fcap);
            if (a) fs = a;
            b2 = realloc(fsol, sizeof(int) * (size_t)fcap);
            if (b2) fsol = b2;
            if (!a || !b2) goto fail;
        }
        fs[nf] = n;
        fsol[nf] = cur_sol;
        nf++;
        p = strchr(c, '[');
        if (!p) break;
        p++;
        /* [[lon, lat, z], ...] */
        while (*p) {
            char *e;
            double lo, la;
            while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') p++;
            if (*p == ']') { p++; break; }
            if (*p != '[') break;
            p++;
            lo = strtod(p, &e);
            if (e == p) break;
            p = e;
            while (*p == ' ' || *p == ',') p++;
            la = strtod(p, &e);
            if (e == p) break;
            p = e;
            while (*p && *p != ']') p++;
            if (*p) p++;
            if (n >= cap) {
                double *q;
                cap *= 2;
                q = realloc(pts, sizeof(double) * 2 * (size_t)cap);
                if (!q) goto fail;
                pts = q;
            }
            pts[2 * n] = lo;
            pts[2 * n + 1] = la;
            n++;
        }
    }
    if (n < 2 || nf < 1) goto fail;
    pthread_mutex_lock(&data_lock);
    free(r->pts); free(r->feat_start); free(r->feat_sol);
    r->pts = pts; r->npts = n;
    r->feat_start = fs; r->feat_sol = fsol; r->nfeat = nf;
    r->last_len = last_len;
    r->last_sol = fsol[nf - 1];
    pthread_mutex_unlock(&data_lock);
    return n;
fail:
    free(pts); free(fs); free(fsol);
    return 0;
}

static void heli_parse(const char *txt)
{
    double lat, lon, tot = 0, fl = 0;
    if (!json_num(txt, "\"Lat\"", &lat) || !json_num(txt, "\"Lon\"", &lon)) return;
    json_num(txt, "\"Total_km\"", &tot);
    json_num(txt, "\"Flight\"", &fl);
    pthread_mutex_lock(&data_lock);
    heli.have = 1; heli.lat = lat; heli.lon = lon; heli.total_km = tot; heli.flight = (int)fl;
    pthread_mutex_unlock(&data_lock);
}

/* Web Mercator pixel coordinates (MMGIS tiles), 256 px tiles */
static void merc_px(double lon, double lat, int z, double *x, double *y)
{
    double n = 256.0 * (double)(1 << z), r = clampd(lat, -85, 85) * D2R;
    *x = (lon + 180.0) / 360.0 * n;
    *y = (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * n;
}

/* Inset size in screen pixels at a 1080-line display */
#define INSET_W 430
#define INSET_H 290

/* Zoom so the last few drives fill about half the inset */
static int rover_zoom(const rover_t *r)
{
    int z, k0;
    double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
    if (!r->pts || r->nfeat < 1) return 16;
    k0 = r->nfeat >= 4 ? r->feat_start[r->nfeat - 4] : 0;
    for (z = 18; z >= 13; z--) {
        int i;
        x0 = y0 = 1e30; x1 = y1 = -1e30;
        for (i = k0; i < r->npts; i++) {
            double x, y;
            merc_px(r->pts[2 * i], r->pts[2 * i + 1], z, &x, &y);
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
        if (x1 - x0 < INSET_W * 0.55 && y1 - y0 < INSET_H * 0.5) return z > 17 ? 17 : z;
    }
    return 13;
}

/* Choose the tile block for the inset around the rover (fetch thread) */
static void rover_tiles_plan(rover_t *r)
{
    double cx, cy;
    int z = rover_zoom(r);
    merc_px(r->lon, r->lat, z, &cx, &cy);
    r->z = z;
    r->ox = floor(cx - INSET_W / 2.0);
    r->oy = floor(cy - INSET_H / 2.0);
    r->tx0 = (int)floor(r->ox / 256.0);
    r->ty0 = (int)floor(r->oy / 256.0);
    r->tw = (int)floor((r->ox + INSET_W - 1) / 256.0) - r->tx0 + 1;
    r->th = (int)floor((r->oy + INSET_H - 1) / 256.0) - r->ty0 + 1;
}

static void tile_path(char *out, size_t n, const rover_t *r, int z, int x, int y)
{
    snprintf(out, n, "%s/tile_%s_%d_%d_%d.png", dir_cache(), r->key, z, x, y);
}

/* ── Deep Space Network: who is talking to Earth right now ─────────
 * NASA DSN Now (eyes.nasa.gov/dsn/data/dsn.xml), fetched every minute
 * while another world is on screen. A dish counts when its up- or
 * downlink is active; targets are matched by NAIF id. */
#define DSN_NOW_URL "https://eyes.nasa.gov/dsn/data/dsn.xml"
#define MAX_LINKS 24
typedef struct {
    int id;                /* NAIF id without the sign (74 = MRO)       */
    char site[12];         /* Goldstone / Madrid / Canberra             */
    char dish[16];         /* DSS14 -> "DSS-14"                          */
    double down_bps;
    int down, up;
    double rtlt;           /* round-trip light time, s                  */
} dsn_link_t;
static dsn_link_t links[MAX_LINKS];
static int nlinks;
static time_t links_at;

static int xml_attr(const char *tag, const char *end, const char *name, char *out, int n)
{
    char pat[48];
    const char *p;
    int i = 0;
    snprintf(pat, sizeof(pat), " %s=\"", name);
    p = strstr(tag, pat);
    if (!p || p > end) return 0;
    for (p += strlen(pat); *p && *p != '"' && p < end && i < n - 1; p++) out[i++] = *p;
    out[i] = 0;
    return 1;
}

static int dsn_now_parse(const char *xml, dsn_link_t *out, int max)
{
    const char *p = xml;
    char site[12] = "";
    int n = 0;
    while ((p = strchr(p, '<')) != NULL && n < max) {
        const char *e = strchr(p, '>');
        if (!e) break;
        if (strncmp(p, "<station ", 9) == 0) {
            xml_attr(p, e, "friendlyName", site, sizeof(site));
        } else if (strncmp(p, "<dish ", 6) == 0) {
            const char *dend = strstr(e, "</dish>"), *c;
            dsn_link_t L;
            char b[24], dn[12] = "";
            int k, have_target = 0;
            if (!dend) dend = e;
            memset(&L, 0, sizeof(L));
            xml_attr(p, e, "name", dn, sizeof(dn));
            /* "DSS14" -> "DSS-14" */
            if (strncmp(dn, "DSS", 3) == 0) snprintf(L.dish, sizeof(L.dish), "DSS-%s", dn + 3);
            else snprintf(L.dish, sizeof(L.dish), "%s", dn);
            snprintf(L.site, sizeof(L.site), "%s", site);
            for (c = e; (c = strchr(c + 1, '<')) != NULL && c < dend; c = strchr(c, '>')) {
                const char *ce = strchr(c, '>');
                if (!ce) break;
                if (!strncmp(c, "<downSignal ", 12) && xml_attr(c, ce, "active", b, sizeof(b)) && !strcmp(b, "true")) {
                    L.down = 1;
                    if (xml_attr(c, ce, "dataRate", b, sizeof(b))) L.down_bps = fmax(L.down_bps, atof(b));
                } else if (!strncmp(c, "<upSignal ", 10) && xml_attr(c, ce, "active", b, sizeof(b)) && !strcmp(b, "true")) {
                    L.up = 1;
                } else if (!strncmp(c, "<target ", 8) && !have_target && xml_attr(c, ce, "id", b, sizeof(b))) {
                    L.id = atoi(b);
                    if (xml_attr(c, ce, "rtlt", b, sizeof(b))) L.rtlt = atof(b);
                    have_target = L.id > 0;
                }
            }
            if (have_target && (L.down || L.up)) {
                /* one entry per spacecraft: keep the fastest downlink */
                for (k = 0; k < n; k++) if (out[k].id == L.id) break;
                if (k == n) out[n++] = L;
                else if (L.down_bps > out[k].down_bps) out[k] = L;
            }
            p = dend;
            continue;
        }
        p = e;
    }
    return n;
}

static void dsn_fetch(void)
{
    char *buf = malloc(256 * 1024);
    static dsn_link_t tmp[MAX_LINKS];
    int got, n;
    if (!buf) return;
    got = pic_fetch_to_buf(DSN_NOW_URL, buf, 256 * 1024 - 1);
    if (got > 0) {
        buf[got] = 0;
        n = dsn_now_parse(buf, tmp, MAX_LINKS);
        pthread_mutex_lock(&data_lock);
        memcpy(links, tmp, sizeof(tmp));
        nlinks = n;
        links_at = time(NULL);
        pthread_mutex_unlock(&data_lock);
    }
    free(buf);
}

/* The link for NAIF id (no sign), if live (caller holds data_lock) */
static const dsn_link_t *dsn_link(int id)
{
    int i;
    if (time(NULL) - links_at > 300) return NULL;      /* stale: show nothing */
    for (i = 0; i < nlinks; i++) if (links[i].id == id) return &links[i];
    return NULL;
}

static void fmt_bps(char *out, size_t n, double bps)
{
    if (bps >= 1e6) snprintf(out, n, "%.1f Mb/s", bps / 1e6);
    else if (bps >= 1e3) snprintf(out, n, "%.0f kb/s", bps / 1e3);
    else if (bps > 0) snprintf(out, n, "%.0f b/s", bps);
    else out[0] = 0;
}

/* "DSN Canberra DSS-34 \u2193 2.6 Mb/s" */
static void fmt_link(char *out, size_t n, const dsn_link_t *L)
{
    char r[24];
    fmt_bps(r, sizeof(r), L->down_bps);
    if (L->down && r[0])
        snprintf(out, n, "DSN %s %s \xe2\x86\x93 %s", L->site, L->dish, r);
    else
        snprintf(out, n, "DSN %s %s %s", L->site, L->dish, L->up && L->down ? "\xe2\x86\x91\xe2\x86\x93 two-way" : L->up ? "\xe2\x86\x91 uplink" : "\xe2\x86\x93 downlink");
}

/* ── 5. Fetch thread ───────────────────────────────────────────── */

static unsigned want_mask;        /* bit w: world w is in the cycle     */
static pic_feature_t worlds_feature;
static int dust_on = 1;           /* MARS_DUST_CLIM: typical-dust haze   */
static int labels_on = 1;         /* VIEW_WORLD_LABELS: feature names */
static int relief_on = 1;         /* VIEW_WORLD_RELIEF: shaded relief */
static int zoom_on = 1;           /* VIEW_MARS_ZOOM: rover close-ups */
static double grs_l2_set = -1;    /* JUPITER_GRS_L2 (System II), -1 = estimate */

static void worlds_reload(pic_feature_t *f)
{
    char m[64] = "", *tok, *sp;
    unsigned mask = 0;
    int k;
    (void)f;
    pic_conf_str("VIEW_MODE", m, sizeof(m));
    if ((k = pic_world_by_name(m)) != 0) mask |= 1u << k;
    m[0] = 0;
    pic_conf_str("VIEW_WORLDS", m, sizeof(m));
    for (tok = strtok_r(m, ",", &sp); tok; tok = strtok_r(NULL, ",", &sp))
        if ((k = pic_world_by_name(tok)) != 0) mask |= 1u << k;
    if (mask & ~want_mask) pic_feature_poke(&worlds_feature);   /* fetch now */
    want_mask = mask;
    grs_l2_set = pic_conf_num("JUPITER_GRS_L2", -1);
    dust_on = pic_conf_int("MARS_DUST_CLIM", 1) != 0;
    labels_on = pic_conf_int("VIEW_WORLD_LABELS", 1) != 0;
    relief_on = pic_conf_int("VIEW_WORLD_RELIEF", 1) != 0;
    zoom_on = pic_conf_int("VIEW_MARS_ZOOM", 1) != 0;
}

static int worlds_wanted(pic_feature_t *f)
{
    worlds_reload(f);
    return want_mask != 0;
}

/* Plan the inset's tiles and see whether they are all on disk */
static int rover_tiles_check(rover_t *r)
{
    int x, y, all = 1;
    char path[256];
    struct stat st;
    pthread_mutex_lock(&data_lock);
    if (r->have) rover_tiles_plan(r);
    pthread_mutex_unlock(&data_lock);
    if (!r->have) return 0;
    for (y = r->ty0; y < r->ty0 + r->th; y++)
        for (x = r->tx0; x < r->tx0 + r->tw; x++) {
            tile_path(path, sizeof(path), r, r->z, x, y);
            if (stat(path, &st) != 0 || st.st_size <= 100) all = 0;
        }
    pthread_mutex_lock(&data_lock);
    r->tiles_ready = all;
    pthread_mutex_unlock(&data_lock);
    return all;
}

static int rover_photo_choose(rover_t *r, const char *txt);

static pthread_once_t cache_once = PTHREAD_ONCE_INIT;

static void fetch_zoom_tiles(rover_t *r);
static void zoom_tiles_check(rover_t *r);

static void load_all_cache(void)
{
    int i;
    char path[256], *txt;
    for (i = 0; i < NBODIES; i++) body_load_cache(&BODIES[i]);
    for (i = 0; i < NROVERS; i++) {
        snprintf(path, sizeof(path), "%s/%s_current.json", dir_cache(), ROVERS[i].key);
        if ((txt = slurp(path, NULL)) != NULL) { rover_parse_current(&ROVERS[i], txt); free(txt); }
        snprintf(path, sizeof(path), "%s/%s_traverse.json", dir_cache(), ROVERS[i].key);
        if ((txt = slurp(path, NULL)) != NULL) { rover_parse_traverse(&ROVERS[i], txt); free(txt); }
        snprintf(path, sizeof(path), "%s/%s_raw.json", dir_cache(), ROVERS[i].key);
        if ((txt = slurp(path, NULL)) != NULL) { rover_photo_choose(&ROVERS[i], txt); free(txt); }
    }
    snprintf(path, sizeof(path), "%s/heli_current.json", dir_cache());
    if ((txt = slurp(path, NULL)) != NULL) { heli_parse(txt); free(txt); }
    snprintf(path, sizeof(path), "%s/msl_weather.json", dir_cache());
    if ((txt = slurp(path, NULL)) != NULL) { wx_parse(txt); free(txt); }
    for (i = 0; i < NROVERS; i++) rover_tiles_check(&ROVERS[i]);
    for (i = 0; i < NROVERS; i++) zoom_tiles_check(&ROVERS[i]);
    if (getenv("PIC_PV_DSN_FILE")) {            /* tests: a saved dsn.xml */
        txt = slurp(getenv("PIC_PV_DSN_FILE"), NULL);
        if (txt) {
            nlinks = dsn_now_parse(txt, links, MAX_LINKS);
            links_at = time(NULL);
            free(txt);
        }
    }
}

static int fetch_body_window(body_t *b, time_t now, time_t a, time_t z)
{
    char url[1024], t0[32], t1[32], path[256];
    char *buf;
    int got;
    strftime(t0, sizeof(t0), "%Y-%m-%d%%20%H:%M", gmtime(&a));
    strftime(t1, sizeof(t1), "%Y-%m-%d%%20%H:%M", gmtime(&z));
    snprintf(url, sizeof(url), HZ_URL, b->target, b->center, t0, t1, b->step);
    buf = malloc(HZ_BUF);
    if (!buf) return 0;
    b->tried = now;
    got = pic_fetch_to_buf(url, buf, HZ_BUF);
    if (got > 0 && strstr(buf, "$$SOE") && body_load_text(b, buf) > 1) {
        snprintf(path, sizeof(path), "%s/hz_%s.txt", dir_cache(), b->key);
        write_file(path, buf, strlen(buf));
        printf("worlds: %s ephemeris updated\n", b->name);
        free(buf);
        return 1;
    }
    printf("worlds: no ephemeris for %s\n", b->name);
    free(buf);
    return 0;
}

static int fetch_body(body_t *b, time_t now)
{
    return fetch_body_window(b, now, now - HZ_BEFORE, now + HZ_AFTER);
}

/*
 * Juno's next close pass (perijove): the lowest range from the fine
 * series if it covers it, else from the 2-hourly orbit series. Only
 * passes from 3 h ago on count. Caller holds data_lock.
 */
static int juno_next_pj(double now, double *t_pj, pic_subpt_t *at)
{
    body_t *o = body_by_key("jup_juno_orbit"), *f = body_by_key("jup_juno_pj");
    int i, best = -1;
    if (!o || !o->obs || o->n < 3) return 0;
    for (i = 1; i + 1 < o->n; i++)
        if (o->obs[i].t > now - 3 * 3600 && o->obs[i].r <= o->obs[i - 1].r &&
            o->obs[i].r <= o->obs[i + 1].r) { best = i; break; }
    if (best < 0) return 0;
    *t_pj = o->obs[best].t;
    *at = o->obs[best];
    if (f && f->obs && f->n > 2 && f->t0 < *t_pj && f->t1 > *t_pj) {
        int k = 0;
        for (i = 1; i < f->n; i++) if (f->obs[i].r < f->obs[k].r) k = i;
        *t_pj = f->obs[k].t;
        *at = f->obs[k];
    }
    return 1;
}

/*
 * Latest raw images (NASA/JPL-Caltech raw image feeds, public domain).
 * The newest sol decides; within 3 sols of it a scenic camera wins
 * (Mastcam(-Z) > Navcam > Hazcam > arm cameras), else simply the
 * newest frame (e.g. a SuperCam/ChemCam close-up).
 */
#define M20_RAW_URL "https://mars.nasa.gov/rss/api/?feed=raw_images&category=mars2020" \
    "&feedtype=json&num=100&page=0&order=sol+desc"
#define MSL_RAW_URL "https://mars.nasa.gov/api/v1/raw_image_items/?order=sol%20desc,date_taken%20desc" \
    "&per_page=100&page=0&condition_1=msl:mission"

static const struct { const char *pre, *name; int rank; const char *credit; } CAMS[] = {
    { "MCZ_LEFT", "Mastcam-Z left", 0, "NASA/JPL-Caltech/ASU" }, { "MCZ_RIGHT", "Mastcam-Z right", 0, "NASA/JPL-Caltech/ASU" },
    { "MAST_LEFT", "Mastcam left", 0, "NASA/JPL-Caltech/MSSS" }, { "MAST_RIGHT", "Mastcam right", 0, "NASA/JPL-Caltech/MSSS" },
    { "NAVCAM_LEFT", "Navcam left", 1, "NASA/JPL-Caltech" }, { "NAVCAM_RIGHT", "Navcam right", 1, "NASA/JPL-Caltech" },
    { "NAV_LEFT", "Navcam left", 1, "NASA/JPL-Caltech" }, { "NAV_RIGHT", "Navcam right", 1, "NASA/JPL-Caltech" },
    { "FRONT_HAZCAM", "Front Hazcam", 2, "NASA/JPL-Caltech" }, { "REAR_HAZCAM", "Rear Hazcam", 2, "NASA/JPL-Caltech" },
    { "FHAZ", "Front Hazcam", 2, "NASA/JPL-Caltech" }, { "RHAZ", "Rear Hazcam", 2, "NASA/JPL-Caltech" },
    { "SHERLOC_WATSON", "WATSON", 3, "NASA/JPL-Caltech" }, { "MAHLI", "MAHLI", 3, "NASA/JPL-Caltech/MSSS" },
    { "SUPERCAM_RMI", "SuperCam close-up", 5, "NASA/JPL-Caltech/LANL/CNES/IRAP" }, { "CHEMCAM_RMI", "ChemCam close-up", 5, "NASA/JPL-Caltech/LANL" },
    { "SKYCAM", "SkyCam", 6, "NASA/JPL-Caltech" }, { "CACHECAM", "CacheCam", 6, "NASA/JPL-Caltech" }, { "MARDI", "MARDI", 6, "NASA/JPL-Caltech/MSSS" },
};

static int cam_rank(const char *ins, const char **name)
{
    unsigned i;
    for (i = 0; i < sizeof(CAMS) / sizeof(CAMS[0]); i++)
        if (strncmp(ins, CAMS[i].pre, strlen(CAMS[i].pre)) == 0) {
            if (name) *name = CAMS[i].name;
            return CAMS[i].rank;
        }
    if (name) *name = ins;
    return 7;
}

static const char *cam_credit(const char *name)
{
    unsigned i;
    for (i = 0; i < sizeof(CAMS) / sizeof(CAMS[0]); i++)
        if (strcmp(name, CAMS[i].name) == 0) return CAMS[i].credit;
    return "NASA/JPL-Caltech";
}

/* Walk the objects of the JSON array after `key`, calling fn on each
 * (as a NUL-terminated copy) */
static void json_each(const char *txt, const char *key, void (*fn)(const char *, void *), void *ud)
{
    const char *p = strstr(txt, key);
    char *obj;
    if (!p || !(p = strchr(p, '['))) return;
    obj = malloc(16384);
    if (!obj) return;
    for (p++; *p && *p != ']';) {
        const char *st;
        int depth = 0, instr = 0;
        while (*p && *p != '{' && *p != ']') p++;
        if (*p != '{') break;
        st = p;
        for (; *p; p++) {
            if (instr) { if (*p == '\\' && p[1]) p++; else if (*p == '"') instr = 0; continue; }
            if (*p == '"') instr = 1;
            else if (*p == '{') depth++;
            else if (*p == '}' && --depth == 0) { p++; break; }
        }
        if ((size_t)(p - st) < 16384) {
            memcpy(obj, st, (size_t)(p - st));
            obj[p - st] = 0;
            fn(obj, ud);
        }
    }
    free(obj);
}

typedef struct { int msl, best_sol, max_sol, rank; char url[320], ins[40], date[24], cahv[600], att[96], sub[48], dim[48]; } pick_t;

static void raw_scan_max(const char *o, void *ud)
{
    pick_t *k = ud;
    double sol;
    if (json_num(o, "\"sol\"", &sol) && sol > k->max_sol) k->max_sol = (int)sol;
}

static void raw_pick(const char *o, void *ud)
{
    pick_t *k = ud;
    double sol;
    char ins[40] = "", url[320] = "", date[24] = "", st[16] = "";
    int rank;
    if (!json_num(o, "\"sol\"", &sol) || sol < k->max_sol - 3) return;
    if (k->msl) {
        char rect[40] = "";
        int a, b2, w = 0, h = 0;
        json_str(o, "\"instrument\"", ins, sizeof(ins));
        json_str(o, "\"url\"", url, sizeof(url));
        json_str(o, "\"date_taken\"", date, sizeof(date));
        json_str(o, "\"subframe_rect\"", rect, sizeof(rect));
        if (sscanf(rect, "(%d,%d,%d,%d)", &a, &b2, &w, &h) != 4 || w < 512 || h < 256) return;
    } else {
        const char *f = strstr(o, "\"image_files\"");
        json_str(o, "\"instrument\"", ins, sizeof(ins));
        json_str(o, "\"sample_type\"", st, sizeof(st));
        json_str(o, "\"date_taken_utc\"", date, sizeof(date));
        if (strcmp(st, "Full") || !f) return;
        if (!json_str(f, "\"large\"", url, sizeof(url))) json_str(f, "\"medium\"", url, sizeof(url));
    }
    if (strncmp(url, "https://", 8) || !ins[0]) return;
    rank = cam_rank(ins, NULL);
    /* lower rank wins; then the newer sol; then the later time */
    if (!k->url[0] || rank < k->rank ||
        (rank == k->rank && ((int)sol > k->best_sol ||
                             ((int)sol == k->best_sol && strcmp(date, k->date) > 0)))) {
        k->rank = rank;
        k->best_sol = (int)sol;
        snprintf(k->url, sizeof(k->url), "%s", url);
        snprintf(k->ins, sizeof(k->ins), "%s", ins);
        snprintf(k->date, sizeof(k->date), "%s", date);
        /* pointing: camera model, attitude, subframe (both feeds) */
        k->cahv[0] = k->att[0] = k->sub[0] = k->dim[0] = 0;
        json_str(o, "\"camera_model_component_list\"", k->cahv, sizeof(k->cahv));
        json_str(o, "\"attitude\"", k->att, sizeof(k->att));
        if (k->msl) {
            json_str(o, "\"subframe_rect\"", k->sub, sizeof(k->sub));
        } else {
            json_str(o, "\"subframeRect\"", k->sub, sizeof(k->sub));
            json_str(o, "\"dimension\"", k->dim, sizeof(k->dim));
        }
    }
}

/* "(a,b,c);(d,e,f);..." -> the first four vectors (CAHV) */
static int parse_cahv(const char *s, double C[3], double A[3], double H[3], double V[3])
{
    double v[12];
    int i = 0;
    const char *p = s;
    while (i < 12 && (p = strchr(p, '(')) != NULL) {
        if (sscanf(p, "(%lf,%lf,%lf)", &v[i], &v[i + 1], &v[i + 2]) != 3) return 0;
        i += 3;
        p++;
    }
    if (i < 12) return 0;
    memcpy(C, v, sizeof(double) * 3); memcpy(A, v + 3, sizeof(double) * 3);
    memcpy(H, v + 6, sizeof(double) * 3); memcpy(V, v + 9, sizeof(double) * 3);
    return 1;
}

/* Choose the picture from a raw-image feed reply into r (no download) */
static int rover_photo_choose(rover_t *r, const char *txt)
{
    pick_t k;
    const char *name;
    memset(&k, 0, sizeof(k));
    k.msl = r == &ROVERS[0];
    k.max_sol = -1;
    json_each(txt, k.msl ? "\"items\"" : "\"images\"", raw_scan_max, &k);
    if (k.max_sol < 0) return 0;
    json_each(txt, k.msl ? "\"items\"" : "\"images\"", raw_pick, &k);
    if (!k.url[0]) return 0;
    cam_rank(k.ins, &name);
    pthread_mutex_lock(&data_lock);
    if (strcmp(r->photo_url, k.url)) r->photo_gen++;
    snprintf(r->photo_url, sizeof(r->photo_url), "%s", k.url);
    snprintf(r->photo_cam, sizeof(r->photo_cam), "%s", name);
    {
        static const char *MON[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
        int y, mo, d, hh, mm;
        if (sscanf(k.date, "%d-%d-%dT%d:%d", &y, &mo, &d, &hh, &mm) == 5 && mo >= 1 && mo <= 12)
            snprintf(r->photo_date, sizeof(r->photo_date), "%d %s %02d:%02d", d, MON[mo - 1], hh, mm);
        else
            snprintf(r->photo_date, sizeof(r->photo_date), "%.16s", k.date);
    }
    r->photo_sol = k.best_sol;
    r->photo_rank = k.rank;
    {
        double x0, y0, w, h, dw = 0, dh = 0;
        r->geom = parse_cahv(k.cahv, r->cC, r->cA, r->cH, r->cV) &&
                  sscanf(k.att, "(%lf,%lf,%lf,%lf)", &r->q[0], &r->q[1], &r->q[2], &r->q[3]) == 4 &&
                  sscanf(k.sub, "(%lf,%lf,%lf,%lf)", &x0, &y0, &w, &h) == 4 && w > 0 && h > 0;
        if (r->geom) {
            r->sub_x0 = x0 - 1; r->sub_y0 = y0 - 1; r->sub_w = w; r->sub_h = h;
            r->img_scale = 1;
            if (sscanf(k.dim, "(%lf,%lf)", &dw, &dh) == 2 && dw > 0 && dw < w) r->img_scale = dw / w;
        }
    }
    pthread_mutex_unlock(&data_lock);
    return 1;
}

static void photo_path(char *out, size_t n, const rover_t *r)
{
    snprintf(out, n, "%s/%s_photo.jpg", dir_cache(), r->key);
}

static void fetch_rover_photo(rover_t *r)
{
    char path[256], tmp[300], url[320], *txt;
    snprintf(path, sizeof(path), "%s/%s_raw.json", dir_cache(), r->key);
    snprintf(tmp, sizeof(tmp), "%s.dl", path);
    if (pic_fetch_to_file(r == &ROVERS[0] ? MSL_RAW_URL : M20_RAW_URL, tmp, 90) != 0) { unlink(tmp); return; }
    txt = slurp(tmp, NULL);
    if (txt && rover_photo_choose(r, txt)) rename(tmp, path);
    else unlink(tmp);
    free(txt);
    /* download the picture when it changed */
    pthread_mutex_lock(&data_lock);
    snprintf(url, sizeof(url), "%s", r->photo_url);
    pthread_mutex_unlock(&data_lock);
    if (url[0]) {
        char mark[300], *old;
        snprintf(mark, sizeof(mark), "%s/%s_photo.url", dir_cache(), r->key);
        old = slurp(mark, NULL);
        photo_path(path, sizeof(path), r);
        if (!old || strcmp(old, url) || access(path, R_OK) != 0) {
            snprintf(tmp, sizeof(tmp), "%s.dl", path);
            if (pic_fetch_to_file(url, tmp, 90) == 0) {
                rename(tmp, path);
                write_file(mark, url, strlen(url));
                pthread_mutex_lock(&data_lock);
                r->photo_gen++;
                pthread_mutex_unlock(&data_lock);
                printf("worlds: %s latest image %s\n", r->name, url);
            } else {
                unlink(tmp);
            }
        }
        free(old);
    }
}


static int fetch_rover(rover_t *r, time_t now)
{
    char url[300], path[256], *buf;
    int sol_before, n, x, y, fetched = 0;
    char rmc_before[24];
    struct stat st;

    r->checked = now;
    buf = malloc(8192);
    if (!buf) return 0;
    snprintf(url, sizeof(url), MMGIS "%s/Layers/json/%s_waypoints_current.json", r->mission, r->key);
    pthread_mutex_lock(&data_lock);
    sol_before = r->sol;
    snprintf(rmc_before, sizeof(rmc_before), "%s", r->rmc);
    pthread_mutex_unlock(&data_lock);
    n = pic_fetch_to_buf(url, buf, 8192);
    if (n > 0 && rover_parse_current(r, buf)) {
        snprintf(path, sizeof(path), "%s/%s_current.json", dir_cache(), r->key);
        write_file(path, buf, strlen(buf));
        fetched = 1;
    }
    free(buf);

    /* The full traverse (1-2 MB) only when the rover has moved */
    snprintf(path, sizeof(path), "%s/%s_traverse.json", dir_cache(), r->key);
    if (fetched && (stat(path, &st) != 0 || r->sol != sol_before || strcmp(r->rmc, rmc_before) ||
                    now - st.st_mtime > 7 * 86400)) {
        char tmp[300];
        snprintf(url, sizeof(url), MMGIS "%s/Layers/json/%s_traverse.json", r->mission, r->key);
        snprintf(tmp, sizeof(tmp), "%s.dl", path);
        if (pic_fetch_to_file(url, tmp, 120) == 0) {
            char *txt = slurp(tmp, NULL);
            if (txt && rover_parse_traverse(r, txt) > 1) rename(tmp, path);
            else unlink(tmp);
            free(txt);
        }
    }

    fetch_rover_photo(r);

    /* Inset tiles */
    pthread_mutex_lock(&data_lock);
    if (r->have) rover_tiles_plan(r);
    pthread_mutex_unlock(&data_lock);
    if (!r->have) return fetched;
    {
        int all = 1, nt = 1 << r->z;
        for (y = r->ty0; y < r->ty0 + r->th; y++)
            for (x = r->tx0; x < r->tx0 + r->tw; x++) {
                tile_path(path, sizeof(path), r, r->z, x, y);
                if (stat(path, &st) == 0 && st.st_size > 100) continue;
                /* MMGIS serves TMS rows (y counted from the south) */
                snprintf(url, sizeof(url), "%s/%d/%d/%d.png", ROVER_TILES[r - ROVERS],
                         r->z, x, nt - 1 - y);
                if (pic_fetch_to_file(url, path, 60) != 0) { unlink(path); all = 0; }
            }
        pthread_mutex_lock(&data_lock);
        r->tiles_ready = all;
        pthread_mutex_unlock(&data_lock);
    }
    if (zoom_on) fetch_zoom_tiles(r);
    return fetched;
}

static int worlds_work(pic_feature_t *f)
{
    time_t now = time(NULL);
    int i, did = 0;
    (void)f;
    mkdir("/data/cache", 0755);
    mkdir(dir_cache(), 0755);
    pthread_once(&cache_once, load_all_cache);

    if (pic_view_world()) {
        static time_t dsn_at;
        if (now - dsn_at >= 60) {
            dsn_at = now;
            dsn_fetch();
        }
    }

    for (i = 0; i < NBODIES && !did; i++) {
        body_t *b = &BODIES[i];
        double t1;
        if (!(want_mask & (1u << b->world))) continue;
        if (b->type == B_AUX) {
            double tpj = 0, f0 = 0, f1 = 0;
            pic_subpt_t at;
            int have_pj;
            if (now - b->tried < 3600) continue;
            pthread_mutex_lock(&data_lock);
            t1 = b->n ? b->t1 : 0;
            have_pj = juno_next_pj((double)now, &tpj, &at);
            f0 = b->t0; f1 = b->t1;
            pthread_mutex_unlock(&data_lock);
            if (strcmp(b->key, "mars_earth") == 0 || strcmp(b->key, "jup_earth") == 0) {
                if (t1 > now + HZ_MARGIN) continue;
                fetch_body(b, now);
                did = 1;
            } else if (strcmp(b->key, "jup_juno_orbit") == 0) {
                if (t1 > now + 30 * 86400.0) continue;
                fetch_body_window(b, now, now - 3600, now + 50 * 86400);
                did = 1;
            } else if (have_pj && (!b->n || f0 > tpj - 2 * 3600 || f1 < tpj + 2 * 3600)) {
                time_t c = (time_t)tpj;
                fetch_body_window(b, now, c - 3 * 3600, c + 3 * 3600);
                did = 1;
            }
            continue;
        }
        pthread_mutex_lock(&data_lock);
        t1 = b->n ? b->t1 : 0;
        pthread_mutex_unlock(&data_lock);
        if (t1 > now + HZ_MARGIN) continue;
        if (now - b->tried < 900) continue;            /* failed recently */
        fetch_body(b, now);
        did = 1;
    }
    if (!did && (want_mask & (1u << PIC_WORLD_MARS))) {
        for (i = 0; i < NROVERS && !did; i++)
            if (now - ROVERS[i].checked >= 1800) {
                fetch_rover(&ROVERS[i], now);
                did = 1;
            }
        if (!did) {
            /* Curiosity's weather: the feed is ~1.7 MB (every sol since
             * landing), newest first; keep only its head */
            static time_t wx_at;
            if (now - wx_at > 6 * 3600) {
                char path[256], tmp[300];
                wx_at = now;
                snprintf(path, sizeof(path), "%s/msl_weather.json", dir_cache());
                snprintf(tmp, sizeof(tmp), "%s.dl", path);
                if (pic_fetch_to_file(MSL_WX_URL, tmp, 90) == 0) {
                    FILE *f = fopen(tmp, "rb");
                    char *head = malloc(32768);
                    size_t n = (f && head) ? fread(head, 1, 32767, f) : 0;
                    if (f) fclose(f);
                    if (head && n > 0) {
                        head[n] = 0;
                        if (strstr(head, "\"soles\"")) {
                            wx_parse(head);
                            write_file(path, head, n);
                        }
                    }
                    free(head);
                }
                unlink(tmp);
                did = 1;
            }
        }
        if (!did) {
            static time_t heli_at;
            if (now - heli_at > 86400) {
                char path[256], *buf = malloc(8192);
                heli_at = now;
                if (buf && pic_fetch_to_buf(MMGIS "M20/Layers/json/m20_heli_waypoints_current.json",
                                            buf, 8192) > 0) {
                    heli_parse(buf);
                    snprintf(path, sizeof(path), "%s/heli_current.json", dir_cache());
                    write_file(path, buf, strlen(buf));
                }
                free(buf);
            }
        }
    }
    return did ? 3 : pic_view_world() ? 20 : 120;
}

static pic_feature_t worlds_feature = {
    .name = "worlds",
    .work = worlds_work,
    .reload = worlds_reload,
    .wanted = worlds_wanted,
    .needs_network = 1,
};

pic_feature_t *pic_feature_worlds(void)
{
    return &worlds_feature;
}

/* ── 6. Drawing helpers ────────────────────────────────────────── */

typedef struct { double x0, y0, x1, y1; } box_t;

typedef struct {
    int W, H;              /* screen                                   */
    double mx, my, mw, mh; /* map rectangle                            */
    double s;              /* scale vs 1080 lines                      */
    box_t used[160];
    int nused;
} frame_t;

static void map_xy(const frame_t *F, double lon, double lat, double *x, double *y)
{
    *x = F->mx + (wrap180(lon) + 180.0) / 360.0 * F->mw;
    *y = F->my + (90.0 - lat) / 180.0 * F->mh;
}

static int box_hits(const frame_t *F, box_t b)
{
    int i;
    if (b.x0 < F->mx + 2 || b.x1 > F->mx + F->mw - 2 || b.y0 < F->my + 2 || b.y1 > F->my + F->mh - 2)
        return 1;
    for (i = 0; i < F->nused; i++)
        if (b.x0 < F->used[i].x1 && b.x1 > F->used[i].x0 &&
            b.y0 < F->used[i].y1 && b.y1 > F->used[i].y0) return 1;
    return 0;
}

static void box_add(frame_t *F, double x0, double y0, double x1, double y1)
{
    if (F->nused < (int)(sizeof(F->used) / sizeof(F->used[0]))) {
        box_t b = { x0, y0, x1, y1 };
        F->used[F->nused++] = b;
    }
}

static void text_outline(cairo_t *cr, double x, double y, const char *s,
                         double r, double g, double b, double a)
{
    cairo_move_to(cr, x, y);
    cairo_text_path(cr, s);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.75 * a);
    cairo_set_line_width(cr, 3.0);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);
    cairo_move_to(cr, x, y);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_show_text(cr, s);
    cairo_new_path(cr);
}

static double text_w(cairo_t *cr, const char *s)
{
    cairo_text_extents_t e;
    cairo_text_extents(cr, s, &e);
    return e.x_advance;
}

/* Sprites (NASA 3D models rendered offline), loaded once */
static struct { char name[24]; cairo_surface_t *s; int failed; } sprites[16];

static cairo_surface_t *sprite_get(const char *name)
{
    int i;
    char path[256];
    if (!name) return NULL;
    for (i = 0; i < 16 && sprites[i].name[0]; i++)
        if (strcmp(sprites[i].name, name) == 0) return sprites[i].failed ? NULL : sprites[i].s;
    if (i == 16) return NULL;
    snprintf(sprites[i].name, sizeof(sprites[i].name), "%s", name);
    snprintf(path, sizeof(path), "%s/sprites/%s.png", dir_worlds(), name);
    sprites[i].s = pic_image_load(path);
    sprites[i].failed = sprites[i].s == NULL;
    return sprites[i].s;
}

/* Sprite or vector icon about `size` px across, centred */
static void draw_thing(cairo_t *cr, const char *sprite, int kind, double x, double y,
                       double size, double dim)
{
    cairo_surface_t *s = sprite_get(sprite);
    if (s) {
        double w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
        double k = size * 1.15 / (w > h ? w : h);
        cairo_save(cr);
        cairo_translate(cr, x - w * k / 2, y - h * k / 2);
        cairo_scale(cr, k, k);
        cairo_set_source_surface(cr, s, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        if (dim > 0) cairo_paint_with_alpha(cr, 1.0 - 0.35 * dim);
        else cairo_paint(cr);
        cairo_restore(cr);
    } else {
        pic_draw_craft(cr, (pic_craft_t)kind, x, y, size, dim);
    }
}

static void status_rgb(int st, double *r, double *g, double *b)
{
    switch (st) {
    case ST_ACTIVE:  *r = 0.45; *g = 1.0;  *b = 0.55; break;
    case ST_UNKNOWN: *r = 1.0;  *g = 0.85; *b = 0.35; break;
    case ST_CRASH:   *r = 1.0;  *g = 0.42; *b = 0.32; break;
    default:         *r = 0.86; *g = 0.88; *b = 0.92; break;
    }
}

/*
 * A site: a dot at the exact place, the craft in a callout nearby
 * (a short leader line when it had to move off the spot), name and
 * year under it. Positions tried in rings around the dot until one
 * is clear of everything drawn so far.
 */
static void draw_callout(cairo_t *cr, frame_t *F, double px, double py,
                         const char *sprite, int kind, int kind2, const char *l1,
                         const char *l2, int st, double isz)
{
    double fs1 = 11.5 * F->s, fs2 = 9.5 * F->s, tw, tw2, bw, bh, cx = px, cy = py;
    double r, g, b, dim = st == ST_ACTIVE ? 0 : 0.25;
    double iw = isz * (kind2 >= 0 ? 1.55 : 1.0);
    box_t best = { 0, 0, 0, 0 };
    int ring, d, found = 0;

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs1);
    tw = text_w(cr, l1);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs2);
    tw2 = l2 ? text_w(cr, l2) : 0;
    bw = fmax(iw, fmax(tw, tw2)) + 4;
    bh = isz + fs1 + (l2 ? fs2 + 2 : 0) + 4;

    /* rings of 16 directions; offsets scaled to the callout's own size
     * so it just clears the dot */
    for (ring = 0; ring < 9 && !found; ring++) {
        double rad = ring * 9.0 * F->s;
        for (d = 0; d < (ring ? 16 : 1) && !found; d++) {
            double a = d * M_PI / 8 - M_PI / 2, ca = cos(a), sa = sin(a);
            double ox = ring ? ca * (rad + bw * 0.5 * fabs(ca)) : 0;
            double oy = ring ? sa * (rad + bh * 0.5 * fabs(sa)) : 0;
            box_t bx;
            cx = px + ox;
            cy = py + oy;
            bx.x0 = cx - bw / 2; bx.x1 = cx + bw / 2;
            bx.y0 = cy - isz / 2 - 2; bx.y1 = bx.y0 + bh;
            if (!box_hits(F, bx)) { best = bx; found = 1; }
        }
    }
    if (!found) {                 /* crowded: just the dot and a small label */
        cairo_set_font_size(cr, fs2);
        status_rgb(st, &r, &g, &b);
        cairo_arc(cr, px, py, 2.6 * F->s, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, r, g, b, 0.95);
        cairo_fill(cr);
        return;
    }
    box_add(F, best.x0, best.y0, best.x1, best.y1);

    status_rgb(st, &r, &g, &b);
    if (hypot(cx - px, cy - py) > 3) {
        double ang = atan2(cy - py, cx - px);
        double ex = cx - cos(ang) * isz * 0.45, ey = cy - sin(ang) * isz * 0.45;
        cairo_move_to(cr, px, py);
        cairo_line_to(cr, ex, ey);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
        cairo_set_line_width(cr, 2.6 * F->s);
        cairo_stroke_preserve(cr);
        cairo_new_path(cr);
        cairo_move_to(cr, px, py);
        cairo_line_to(cr, ex, ey);
        cairo_set_source_rgba(cr, r, g, b, 0.7);
        cairo_set_line_width(cr, 1.1 * F->s);
        cairo_stroke(cr);
    }
    if (kind2 >= 0) {
        draw_thing(cr, sprite, kind, cx - isz * 0.25, cy, isz, dim);
        draw_thing(cr, NULL, kind2, cx + isz * 0.42, cy + isz * 0.12, isz * 0.62, dim);
    } else {
        draw_thing(cr, sprite, kind, cx, cy, isz, dim);
    }
    /* the exact spot */
    cairo_arc(cr, px, py, 3.0 * F->s, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
    cairo_fill(cr);
    cairo_arc(cr, px, py, 2.0 * F->s, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, r, g, b, 1);
    cairo_fill(cr);

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs1);
    text_outline(cr, cx - tw / 2, cy + isz / 2 + fs1, l1, r, g, b, 1);
    if (l2) {
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fs2);
        text_outline(cr, cx - tw2 / 2, cy + isz / 2 + fs1 + fs2 + 2, l2, 0.85, 0.87, 0.9, 0.95);
    }
}

/* ── Mars dust climatology ──────────────────────────────────────
 * Typical column dust for the season: the Mars Climate Database
 * daily dust scenarios (Montabone et al. 2015, 2020) averaged over
 * MY 29-33, 35, 36 (years without a global storm), 5-degree Ls bins,
 * 3x3 degree grid, visible optical depth x 50 as bytes. Not live. */
static unsigned char *dust;
static int dust_nb, dust_w, dust_h, dust_tried;

static int dust_load(void)
{
    char path[256], *buf;
    size_t n;
    if (dust || dust_tried) return dust != NULL;
    dust_tried = 1;
    snprintf(path, sizeof(path), "%s/mars_dust_clim.bin", dir_worlds());
    buf = slurp(path, &n);
    if (buf && n > 10 && memcmp(buf, "MDST", 4) == 0) {
        unsigned char *u = (unsigned char *)buf;
        dust_nb = u[4] | u[5] << 8;
        dust_w = u[6] | u[7] << 8;
        dust_h = u[8] | u[9] << 8;
        if (n == 10 + (size_t)dust_nb * dust_w * dust_h) {
            dust = malloc(n - 10);
            if (dust) memcpy(dust, buf + 10, n - 10);
        }
    }
    free(buf);
    return dust != NULL;
}

/* Typical visible optical depth at (lon, lat) for Ls; also the mean */
static double dust_tau(int x, int y, double ls)
{
    double fb = ls / (360.0 / dust_nb) - 0.5, f;
    int b0 = (int)floor(fb), b1;
    f = fb - b0;
    b0 = (b0 % dust_nb + dust_nb) % dust_nb;
    b1 = (b0 + 1) % dust_nb;
    return ((1 - f) * dust[((size_t)b0 * dust_h + y) * dust_w + x] +
            f * dust[((size_t)b1 * dust_h + y) * dust_w + x]) / 50.0;
}

static double draw_dust(cairo_t *cr, frame_t *F, double ls)
{
    cairo_surface_t *g;
    unsigned char *px;
    int x, y, st;
    double sum = 0;
    if (!dust_load()) return -1;
    g = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, dust_w, dust_h);
    if (cairo_surface_status(g)) { cairo_surface_destroy(g); return -1; }
    px = cairo_image_surface_get_data(g);
    st = cairo_image_surface_get_stride(g);
    for (y = 0; y < dust_h; y++)
        for (x = 0; x < dust_w; x++) {
            double tau = dust_tau(x, y, ls), a;
            unsigned char *p = px + y * st + x * 4;
            sum += tau;
            /* a haze that starts to show above the clear-season floor */
            a = 0.62 * (1.0 - exp(-fmax(0, tau - 0.2) * 1.4)) * 255.0;
            p[3] = (unsigned char)a;
            p[2] = (unsigned char)(a * 0.86);    /* tan: 0.86 0.66 0.46 */
            p[1] = (unsigned char)(a * 0.66);
            p[0] = (unsigned char)(a * 0.46);
        }
    cairo_surface_mark_dirty(g);
    cairo_save(cr);
    cairo_rectangle(cr, F->mx, F->my, F->mw, F->mh);
    cairo_clip(cr);
    /* grid cells are centred on 1.5 deg + 3k (lat) and -180 + 3k (lon) */
    cairo_translate(cr, F->mx - F->mw / dust_w / 2, F->my);
    cairo_scale(cr, F->mw / dust_w, F->mh / dust_h);
    cairo_set_source_surface(cr, g, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(g);
    return sum / (dust_w * dust_h);
}

/* ── 7. The map, day and night ─────────────────────────────────── */

static cairo_surface_t *map_surf;
static int map_world, map_w, map_h;

/* Shaded relief: an 8-bit factor map (128 = unchanged) computed from
 * the laser-altimeter elevations (Moon: LRO LOLA; Mars: MGS MOLA),
 * lit from the north-west at 45 deg, multiplied into the colour map. */
static void apply_relief(cairo_surface_t *map, int w, int mw, int mh)
{
    char path[256];
    cairo_surface_t *rel;
    unsigned char *m, *r;
    int sm, sr, x, y;
    snprintf(path, sizeof(path), "%s/%s_relief.jpg", dir_worlds(), WORLDS[w].key);
    if (access(path, R_OK) != 0) return;
    rel = pic_image_load_scaled(path, mw, mh);
    if (!rel) return;
    cairo_surface_flush(map);
    cairo_surface_flush(rel);
    m = cairo_image_surface_get_data(map);
    r = cairo_image_surface_get_data(rel);
    sm = cairo_image_surface_get_stride(map);
    sr = cairo_image_surface_get_stride(rel);
    for (y = 0; y < mh; y++) {
        uint32_t *pm = (uint32_t *)(m + (size_t)y * sm);
        const uint32_t *pr = (const uint32_t *)(r + (size_t)y * sr);
        for (x = 0; x < mw; x++) {
            uint32_t c = pm[x], f = (pr[x] >> 8) & 0xff;      /* green channel */
            uint32_t rr = ((c >> 16) & 0xff) * f / 128, gg = ((c >> 8) & 0xff) * f / 128, bb = (c & 0xff) * f / 128;
            if (rr > 255) rr = 255;
            if (gg > 255) gg = 255;
            if (bb > 255) bb = 255;
            pm[x] = (c & 0xff000000u) | (rr << 16) | (gg << 8) | bb;
        }
    }
    cairo_surface_mark_dirty(map);
    cairo_surface_destroy(rel);
}

static cairo_surface_t *map_get(int w, int mw, int mh)
{
    char path[256];
    static time_t failed_at;
    static int map_relief = -1;
    if (map_world == w && map_w == mw && map_h == mh && map_relief == relief_on &&
        (map_surf || time(NULL) - failed_at < 300)) return map_surf;
    if (map_surf) cairo_surface_destroy(map_surf);
    map_surf = NULL;
    snprintf(path, sizeof(path), "%s/%s_%d.jpg", dir_worlds(), WORLDS[w].key, mw > 1920 ? 3840 : 1920);
    map_surf = pic_image_load_scaled(path, mw, mh);
    map_world = w; map_w = mw; map_h = mh; map_relief = relief_on;
    if (!map_surf) { printf("worlds: no map %s\n", path); failed_at = time(NULL); }
    else if (relief_on && cairo_image_surface_get_format(map_surf) != CAIRO_FORMAT_A8) apply_relief(map_surf, w, mw, mh);
    return map_surf;
}

/* Great Red Spot: where it is in the Cassini map, and its System II
 * longitude now (S&T/JUPOS: 91 deg on 2026-06-01, drifting +1.75 deg
 * a month), unless set in the dashboard */
#define GRS_MAP_XF  0.3652      /* fraction of the map width from the left */
#define GRS_LAT    (-20.87)

static double grs_l2(time_t now, int *estimated)
{
    if (grs_l2_set >= 0 && grs_l2_set < 360) { *estimated = 0; return grs_l2_set; }
    *estimated = 1;
    return fmod(91.0 + 1.75 * ((double)now - 1780272000.0) / (30.4375 * 86400.0) + 720.0, 360.0);
}

static void draw_map(cairo_t *cr, frame_t *F, int w, time_t now, double *grs_lon)
{
    cairo_surface_t *m = map_get(w, (int)F->mw, (int)F->mh);
    double shift = 0;
    if (w == PIC_WORLD_JUPITER) {
        int est;
        double l3w = pic_jupiter_sys2_to_sys3(grs_l2(now, &est), (double)now);
        double lon = wrap180(-l3w);               /* east-positive */
        shift = ((lon + 180.0) / 360.0 - GRS_MAP_XF) * F->mw;
        *grs_lon = lon;
    }
    if (!m) {
        cairo_set_source_rgb(cr, 0.08, 0.08, 0.1);
        cairo_rectangle(cr, F->mx, F->my, F->mw, F->mh);
        cairo_fill(cr);
        return;
    }
    cairo_save(cr);
    cairo_rectangle(cr, F->mx, F->my, F->mw, F->mh);
    cairo_clip(cr);
    shift = fmod(shift, F->mw);
    if (shift < 0) shift += F->mw;
    cairo_set_source_surface(cr, m, F->mx + shift, F->my);
    cairo_paint(cr);
    if (shift > 0) {
        cairo_set_source_surface(cr, m, F->mx + shift - F->mw, F->my);
        cairo_paint(cr);
    }
    cairo_restore(cr);
}

/*
 * Night side: a coarse darkness grid scaled up smoothly. Sharp
 * terminator on the airless Moon, a few degrees of twilight on Mars
 * (dust) and Jupiter. Moon nights are lit by earthshine: brighter
 * nearer the sub-Earth point and when Earth is "full" (new Moon).
 */
static void draw_night(cairo_t *cr, frame_t *F, int w, double slon, double slat,
                       double elon, double elat, double earth_phase)
{
    int gw = (int)(F->mw / 4), gh = (int)(F->mh / 4), x, y, stride;
    cairo_surface_t *g = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, gw, gh);
    unsigned char *px;
    double S[3], E[3];
    if (cairo_surface_status(g)) { cairo_surface_destroy(g); return; }
    unitv(slon, slat, S);
    unitv(elon, elat, E);
    px = cairo_image_surface_get_data(g);
    stride = cairo_image_surface_get_stride(g);
    for (y = 0; y < gh; y++) {
        double lat = 90.0 - (y + 0.5) * 180.0 / gh;
        for (x = 0; x < gw; x++) {
            double lon = -180.0 + (x + 0.5) * 360.0 / gw, N[3], el, lit, nb, dark, a;
            unsigned char *p = px + y * stride + x * 4;
            unitv(lon, lat, N);
            el = asin(clampd(N[0] * S[0] + N[1] * S[1] + N[2] * S[2], -1, 1)) / D2R;
            if (w == PIC_WORLD_MOON) {
                double ce = N[0] * E[0] + N[1] * E[1] + N[2] * E[2];
                lit = clampd((el + 0.3) / 0.8, 0, 1);
                nb = 0.40 + 0.20 * earth_phase * (ce > 0 ? ce : 0);
            } else if (w == PIC_WORLD_MARS) {
                lit = clampd((el + 4.0) / 6.0, 0, 1);
                lit = lit * lit * (3 - 2 * lit);
                nb = 0.45;
            } else {
                lit = clampd((el + 1.5) / 3.0, 0, 1);
                lit = lit * lit * (3 - 2 * lit);
                nb = 0.45;
            }
            dark = (1.0 - lit) * (1.0 - nb);
            a = dark * 255.0;
            /* premultiplied: a deep blue-black night */
            p[3] = (unsigned char)a;
            p[2] = (unsigned char)(a * 0.0);
            p[1] = (unsigned char)(a * 0.02);
            p[0] = (unsigned char)(a * 0.06);
        }
    }
    cairo_surface_mark_dirty(g);
    cairo_save(cr);
    cairo_rectangle(cr, F->mx, F->my, F->mw, F->mh);
    cairo_clip(cr);
    cairo_translate(cr, F->mx, F->my);
    cairo_scale(cr, F->mw / gw, F->mh / gh);
    cairo_set_source_surface(cr, g, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(g);
}

static void draw_grid(cairo_t *cr, frame_t *F, int w)
{
    int k;
    char s[16];
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
    for (k = -150; k <= 150; k += 30) {
        double x, y;
        map_xy(F, k, 0, &x, &y);
        cairo_move_to(cr, floor(x) + 0.5, F->my);
        cairo_line_to(cr, floor(x) + 0.5, F->my + F->mh);
    }
    for (k = -60; k <= 60; k += 30) {
        double x, y;
        map_xy(F, 0, k, &x, &y);
        cairo_move_to(cr, F->mx, floor(y) + 0.5);
        cairo_line_to(cr, F->mx + F->mw, floor(y) + 0.5);
    }
    cairo_stroke(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10 * F->s);
    for (k = -150; k <= 150; k += 30) {
        double x, y;
        map_xy(F, k, 0, &x, &y);
        snprintf(s, sizeof(s), "%d\xc2\xb0%s", abs(k), k < 0 ? "W" : k > 0 ? "E" : "");
        text_outline(cr, x + 3, F->my + F->mh * 0.5 - 4, s, 1, 1, 1, 0.45);
    }
    for (k = -60; k <= 60; k += 30) {
        double x, y;
        if (!k) continue;
        map_xy(F, -180, k, &x, &y);
        snprintf(s, sizeof(s), "%d\xc2\xb0%s", abs(k), k < 0 ? "S" : "N");
        text_outline(cr, x + 4, y - 3, s, 1, 1, 1, 0.45);
    }
    (void)w;
}

/* Sun overhead: a small sun; Earth overhead (Moon only): blue disc */
static void draw_sun_mark(cairo_t *cr, frame_t *F, double lon, double lat)
{
    double x, y, r = 7 * F->s;
    int k;
    map_xy(F, lon, lat, &x, &y);
    cairo_set_source_rgba(cr, 1, 0.85, 0.3, 0.95);
    cairo_set_line_width(cr, 1.6 * F->s);
    for (k = 0; k < 8; k++) {
        double a = k * M_PI / 4;
        cairo_move_to(cr, x + cos(a) * r * 1.4, y + sin(a) * r * 1.4);
        cairo_line_to(cr, x + cos(a) * r * 2.0, y + sin(a) * r * 2.0);
    }
    cairo_stroke(cr);
    cairo_arc(cr, x, y, r, 0, 2 * M_PI);
    cairo_fill(cr);
    box_add(F, x - r * 2, y - r * 2, x + r * 2, y + r * 2);
}

/* ── 8. Orbiters and moons ─────────────────────────────────────── */

static void draw_track(cairo_t *cr, frame_t *F, const body_t *b, double t, double before, double after)
{
    double dt = b->step * 60.0 / 2, tt, lastx = 0;
    int have = 0, pass;
    if (dt > 120) dt = 120;
    for (pass = 0; pass < 2; pass++) {
        double ta = pass ? t : t - before, tb = pass ? t + after : t;
        have = 0;
        cairo_new_path(cr);
        for (tt = ta; tt <= tb + 1e-6; tt += dt) {
            pic_subpt_t p;
            double x, y;
            if (!series_at(b->obs, b->n, tt, &p)) { have = 0; continue; }
            map_xy(F, p.lon, p.lat, &x, &y);
            if (have && fabs(x - lastx) < F->mw / 2) cairo_line_to(cr, x, y);
            else cairo_move_to(cr, x, y);
            have = 1;
            lastx = x;
        }
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        if (pass) {
            static const double dash[] = { 5, 4 };
            double d2[2] = { dash[0] * F->s, dash[1] * F->s };
            cairo_set_dash(cr, d2, 2, 0);
            cairo_set_source_rgba(cr, 0.55, 0.9, 1.0, 0.85);
            cairo_set_line_width(cr, 1.6 * F->s);
        } else {
            cairo_set_source_rgba(cr, 0.55, 0.9, 1.0, 0.35);
            cairo_set_line_width(cr, 1.4 * F->s);
        }
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }
}

static void fmt_alt(char *out, size_t n, double km)
{
    if (km >= 100000) snprintf(out, n, "%.2f million km", km / 1e6);
    else if (km >= 10000) snprintf(out, n, "%.0f,%03.0f km", floor(km / 1000), fmod(km, 1000));
    else snprintf(out, n, "%.0f km", km);
}

/* Expanding rings: this craft is in contact with Earth right now */
static void draw_link_ring(cairo_t *cr, frame_t *F, double x, double y, double r0, double t)
{
    int k;
    for (k = 0; k < 2; k++) {
        double ph = fmod(t / 4.0 + k * 0.5, 1.0);
        cairo_new_path(cr);
        cairo_arc(cr, x, y, r0 + ph * 14 * F->s, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0.5, 1.0, 0.6, 0.75 * (1 - ph));
        cairo_set_line_width(cr, 1.6 * F->s);
        cairo_stroke(cr);
    }
}

/* Year each orbiter arrived in orbit (shown with its altitude) */
static int orbit_since(const char *key)
{
    static const struct { const char *k; int y; } Y[] = {
        { "moon_lro", 2009 }, { "moon_ch2", 2019 }, { "moon_danuri", 2022 },
        { "mars_ody", 2001 }, { "mars_mex", 2003 }, { "mars_mro", 2006 },
        { "mars_tgo", 2016 }, { "mars_hope", 2021 }, { "jup_juno", 2016 } };
    size_t i;
    for (i = 0; i < sizeof(Y) / sizeof(Y[0]); i++)
        if (!strcmp(Y[i].k, key)) return Y[i].y;
    return 0;
}

static void draw_orbiters(cairo_t *cr, frame_t *F, int w, double t, int tracks)
{
    int i;
    for (i = 0; i < NBODIES; i++) {
        body_t *b = &BODIES[i];
        pic_subpt_t p;
        double x, y, before = 45 * 60, after = 50 * 60, isz = 34 * F->s;
        char alt[48], l1[64];
        if (b->world != w || b->type != B_ORBITER || !b->obs) continue;
        if (!series_at(b->obs, b->n, t, &p)) continue;
        if (w == PIC_WORLD_JUPITER) { before = 5 * 3600; after = 5 * 3600; }
        if (tracks) { draw_track(cr, F, b, t, before, after); continue; }
        map_xy(F, p.lon, p.lat, &x, &y);
        fmt_alt(alt, sizeof(alt), p.r - world_radius(w, p.lat));
        if (orbit_since(b->key)) {
            char tmp[48];
            snprintf(tmp, sizeof(tmp), "since %d \xc2\xb7 %s", orbit_since(b->key), alt);
            snprintf(alt, sizeof(alt), "%s", tmp);
        }
        snprintf(l1, sizeof(l1), "%s", b->name);
        {
            double fs = 11.5 * F->s, tw, tw2;
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, fs);
            tw = text_w(cr, l1);
            const dsn_link_t *L = dsn_link(-atoi(b->center));
            if (L) draw_link_ring(cr, F, x, y, isz * 0.62, t);
            draw_thing(cr, b->sprite, b->kind, x, y, isz, 0);
            text_outline(cr, x + isz * 0.6, y - 2 * F->s, l1, 0.6, 0.93, 1.0, 1);
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 9.5 * F->s);
            tw2 = text_w(cr, alt);
            text_outline(cr, x + isz * 0.6, y + 10 * F->s, alt, 0.85, 0.9, 0.95, 0.95);
            if (L) {
                char lk[96];
                fmt_link(lk, sizeof(lk), L);
                cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
                cairo_set_font_size(cr, 9.5 * F->s);
                tw2 = fmax(tw2, text_w(cr, lk));
                text_outline(cr, x + isz * 0.6, y + 21 * F->s, lk, 0.5, 1.0, 0.6, 1);
            }
            box_add(F, x - isz / 2, y - isz / 2, x + isz * 0.6 + fmax(tw, tw2) + 2, y + isz / 2 + (L ? 10 * F->s : 0));
        }
    }
}

/* Juno's next close pass: the ground track either side of it, and the
 * point of closest approach with its time and height. Caller holds
 * data_lock. */
static void draw_pj(cairo_t *cr, frame_t *F, double tpj, const pic_subpt_t *at)
{
    body_t *f = body_by_key("jup_juno_pj");
    static const double dot[] = { 2, 4 };
    double d2[2] = { dot[0] * F->s, dot[1] * F->s }, tt, lastx = 0, x, y;
    int have = 0;
    char l1[64], l2[64], h[32];
    time_t tp = (time_t)tpj;
    if (f && f->obs && f->t0 <= tpj - 5400 && f->t1 >= tpj + 5400) {
        cairo_new_path(cr);
        for (tt = tpj - 5400; tt <= tpj + 5400; tt += 60) {
            pic_subpt_t p;
            if (!series_at(f->obs, f->n, tt, &p)) { have = 0; continue; }
            map_xy(F, p.lon, p.lat, &x, &y);
            if (have && fabs(x - lastx) < F->mw / 2) cairo_line_to(cr, x, y);
            else cairo_move_to(cr, x, y);
            have = 1;
            lastx = x;
        }
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_dash(cr, d2, 2, 0);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
        cairo_set_line_width(cr, 3.6 * F->s);
        cairo_stroke_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 0.65, 0.25, 1);
        cairo_set_line_width(cr, 2.2 * F->s);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }
    map_xy(F, at->lon, at->lat, &x, &y);
    cairo_new_path(cr);
    cairo_arc(cr, x, y, 4.5 * F->s, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
    cairo_fill(cr);
    cairo_arc(cr, x, y, 3.2 * F->s, 0, 2 * M_PI);
    cairo_set_source_rgb(cr, 1.0, 0.65, 0.25);
    cairo_fill(cr);
    strftime(h, sizeof(h), "%d %b %H:%M UTC", gmtime(&tp));
    snprintf(l1, sizeof(l1), "Juno closest %s (predicted)", h);
    fmt_alt(h, sizeof(h), at->r - world_radius(PIC_WORLD_JUPITER, at->lat));
    snprintf(l2, sizeof(l2), "%s above the cloud tops", h);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 11 * F->s);
    text_outline(cr, x + 8 * F->s, y - 2 * F->s, l1, 1.0, 0.75, 0.4, 1);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9.5 * F->s);
    text_outline(cr, x + 8 * F->s, y + 10 * F->s, l2, 0.9, 0.9, 0.9, 0.95);
    box_add(F, x - 6, y - 14 * F->s, x + 170 * F->s, y + 14 * F->s);
}

/* Moons over Mars / Jupiter: the moon where it is overhead, and its
 * shadow where the line from the Sun past it meets the planet */
static void draw_moons(cairo_t *cr, frame_t *F, int w, double t, double slon, double slat, int have_sun)
{
    int i;
    for (i = 0; i < NBODIES; i++) {
        body_t *b = &BODIES[i];
        pic_subpt_t p;
        double x, y, olon, olat, rr;
        char l2[48];
        if (b->world != w || b->type != B_MOON || !b->obs) continue;
        if (!series_at(b->obs, b->n, t, &p)) continue;
        /* shadow: true size (umbra + penumbra ~ the moon's own size
         * for the Galileans; a ~60 km blur for Phobos) */
        if (have_sun && pic_shadow_point(p.lon, p.lat, p.r, slon, slat,
                                         WORLDS[w].a, WORLDS[w].b, &olon, &olat)) {
            double sx, sy, km_per_px = 2 * M_PI * WORLDS[w].a / F->mw, sr;
            double rad = w == PIC_WORLD_MARS ? b->radius + (p.r - WORLDS[w].mean) * 0.0046 : b->radius;
            cairo_pattern_t *g;
            sr = fmax(rad / km_per_px, 3.0 * F->s);
            map_xy(F, olon, olat, &sx, &sy);
            g = cairo_pattern_create_radial(sx, sy, 0, sx, sy, sr * 1.4);
            cairo_pattern_add_color_stop_rgba(g, 0, 0, 0, 0, 0.92);
            cairo_pattern_add_color_stop_rgba(g, 0.6, 0, 0, 0, 0.85);
            cairo_pattern_add_color_stop_rgba(g, 1, 0, 0, 0, 0);
            /* the footprint stretches along the sunlight the lower the
             * Sun is there (shadows near the terminator are long) */
            {
                double N[3], Sd[3], e[3], nn[3], ci, az;
                unitv(olon, olat, N);
                unitv(slon, slat, Sd);
                e[0] = -sin(olon * D2R); e[1] = cos(olon * D2R); e[2] = 0;
                nn[0] = -sin(olat * D2R) * cos(olon * D2R);
                nn[1] = -sin(olat * D2R) * sin(olon * D2R);
                nn[2] = cos(olat * D2R);
                ci = N[0] * Sd[0] + N[1] * Sd[1] + N[2] * Sd[2];
                az = atan2(-(Sd[0] * nn[0] + Sd[1] * nn[1] + Sd[2] * nn[2]),
                           Sd[0] * e[0] + Sd[1] * e[1] + Sd[2] * e[2]);
                cairo_save(cr);
                cairo_translate(cr, sx, sy);
                cairo_scale(cr, 1.0 / fmax(cos(olat * D2R), 0.2), 1);
                cairo_rotate(cr, az);
                cairo_scale(cr, 1.0 / fmax(ci, 0.2), 1);
                cairo_translate(cr, -sx, -sy);
            }
            cairo_set_source(cr, g);
            cairo_arc(cr, sx, sy, sr * 1.4, 0, 2 * M_PI);
            cairo_fill(cr);
            cairo_restore(cr);
            cairo_pattern_destroy(g);
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_ITALIC, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 9.5 * F->s);
            snprintf(l2, sizeof(l2), "%s's shadow", b->name);
            text_outline(cr, sx + sr + 4 * F->s, sy + 4 * F->s, l2, 0.9, 0.9, 0.9, 0.9);
        }
        map_xy(F, p.lon, p.lat, &x, &y);
        rr = (w == PIC_WORLD_MARS ? 7.5 : 9.0) * F->s;
        pic_draw_small_moon(cr, b->kind, x, y, rr);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11 * F->s);
        text_outline(cr, x + rr + 4 * F->s, y - 1 * F->s, b->name, 1, 0.95, 0.85, 1);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 9.5 * F->s);
        fmt_alt(l2, sizeof(l2), p.r - world_radius(w, p.lat));
        text_outline(cr, x + rr + 4 * F->s, y + 10 * F->s, l2, 0.9, 0.9, 0.9, 0.9);
        box_add(F, x - rr, y - rr, x + rr + 90 * F->s, y + rr + 4);
    }
}

/* ── 9. Rover insets ───────────────────────────────────────────── */

static void inset_build(rover_t *r)
{
    int x, y;
    cairo_t *c;
    if (r->inset && r->inset_z == r->z && r->inset_tx0 == r->tx0 && r->inset_ty0 == r->ty0) return;
    if (r->inset) cairo_surface_destroy(r->inset);
    r->inset = cairo_image_surface_create(CAIRO_FORMAT_RGB24, r->tw * 256, r->th * 256);
    if (cairo_surface_status(r->inset)) { cairo_surface_destroy(r->inset); r->inset = NULL; return; }
    c = cairo_create(r->inset);
    cairo_set_source_rgb(c, 0.2, 0.15, 0.12);
    cairo_paint(c);
    for (y = 0; y < r->th; y++)
        for (x = 0; x < r->tw; x++) {
            char path[256];
            cairo_surface_t *t;
            tile_path(path, sizeof(path), r, r->z, r->tx0 + x, r->ty0 + y);
            t = pic_image_load(path);
            if (!t) continue;
            cairo_set_source_surface(c, t, x * 256, y * 256);
            cairo_paint(c);
            cairo_surface_destroy(t);
        }
    cairo_destroy(c);
    r->inset_z = r->z; r->inset_tx0 = r->tx0; r->inset_ty0 = r->ty0;
}

static void rrect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 1.5 * M_PI);
    cairo_close_path(cr);
}

/* Scale bar: a round number of metres about a fifth of the inset */
static void scale_bar(cairo_t *cr, double x, double y, double m_per_px, double k, double s)
{
    static const double steps[] = { 10, 20, 50, 100, 200, 500, 1000, 2000 };
    double want = INSET_W * 0.22 * m_per_px, m = steps[0], len;
    int i;
    char t[24];
    for (i = 0; i < 8; i++) if (steps[i] <= want) m = steps[i];
    len = m / m_per_px * k;
    cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
    cairo_rectangle(cr, x - 1, y - 4 * s, len + 2, 7 * s);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
    cairo_rectangle(cr, x, y - 3 * s, len, 5 * s);
    cairo_fill(cr);
    snprintf(t, sizeof(t), m >= 1000 ? "%.0f km" : "%.0f m", m >= 1000 ? m / 1000 : m);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 10 * s);
    text_outline(cr, x + len + 5 * s, y + 3 * s, t, 1, 1, 1, 1);
}

static void draw_inset(cairo_t *cr, frame_t *F, rover_t *r, double bx, double by, double now_f)
{
    double s = F->s, W = INSET_W * s, H = INSET_H * s, k = s, px, py, rx, ry;
    double tlh = (wx.have ? 50 : 38) * s, mpp;
    int photo_mode;
    int i;
    char t1[96], t2[192];

    /* leader from the rover on the main map to the inset */
    map_xy(F, r->lon, r->lat, &px, &py);
    cairo_move_to(cr, px, py);
    cairo_line_to(cr, bx + W / 2, by);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
    cairo_set_line_width(cr, 3 * s);
    cairo_stroke(cr);
    cairo_move_to(cr, px, py);
    cairo_line_to(cr, bx + W / 2, by);
    cairo_set_source_rgba(cr, 0.45, 1, 0.55, 0.85);
    cairo_set_line_width(cr, 1.2 * s);
    cairo_stroke(cr);

    /* frame */
    rrect(cr, bx - 3 * s, by - 3 * s, W + 6 * s, H + tlh + 6 * s, 7 * s);
    cairo_set_source_rgba(cr, 0.04, 0.05, 0.07, 0.88);
    cairo_fill(cr);
    box_add(F, bx - 4 * s, by - 4 * s, bx + W + 4 * s, by + H + tlh + 4 * s);

    /* the latest picture, loaded when a new one arrives */
    if (r->photo_gen != r->photo_loaded) {
        char path[256];
        if (r->photo) cairo_surface_destroy(r->photo);
        photo_path(path, sizeof(path), r);
        r->photo = pic_image_load(path);
        r->photo_loaded = r->photo_gen;
    }
    /* map and picture take turns, 12 s each */
    photo_mode = r->photo && r->photo_url[0] && fmod(now_f + (r == &ROVERS[0] ? 0 : 6), 24.0) >= 12.0;

    cairo_save(cr);
    cairo_rectangle(cr, bx, by, W, H);
    cairo_clip(cr);
    if (photo_mode) {
        double pw = cairo_image_surface_get_width(r->photo), ph = cairo_image_surface_get_height(r->photo);
        /* scenic frames fill the box; close-ups are shown whole */
        double sc = r->photo_rank <= 3 ? fmax(W / pw, H / ph) : fmin(W / pw, H / ph);
        char lab[96];
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
        cairo_save(cr);
        cairo_translate(cr, bx + (W - pw * sc) / 2, by + (H - ph * sc) / 2);
        cairo_scale(cr, sc, sc);
        cairo_set_source_surface(cr, r->photo, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
        snprintf(lab, sizeof(lab), "LATEST RAW IMAGE \xc2\xb7 %s", r->photo_cam);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 10 * s);
        text_outline(cr, bx + 8 * s, by + 15 * s, lab, 1, 1, 1, 0.95);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 8.5 * s);
        text_outline(cr, bx + W - text_w(cr, cam_credit(r->photo_cam)) - 7 * s, by + H - 7 * s,
                     cam_credit(r->photo_cam), 1, 1, 1, 0.85);
    } else {
    if (r->tiles_ready) inset_build(r);
    if (r->inset) {
        cairo_save(cr);
        cairo_translate(cr, bx, by);
        cairo_scale(cr, k, k);
        cairo_set_source_surface(cr, r->inset, r->tx0 * 256.0 - r->ox, r->ty0 * 256.0 - r->oy);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        cairo_set_source_rgb(cr, 0.18, 0.13, 0.1);
        cairo_paint(cr);
    }
    /* the whole traverse, then the latest drive replayed */
    if (r->pts) {
        int last0 = r->nfeat ? r->feat_start[r->nfeat - 1] : 0;
        cairo_new_path(cr);
        for (i = 0; i < r->npts; i++) {
            double x, y;
            merc_px(r->pts[2 * i], r->pts[2 * i + 1], r->z, &x, &y);
            x = bx + (x - r->ox) * k;
            y = by + (y - r->oy) * k;
            if (i == 0 || (r->nfeat && i == r->feat_start[0])) cairo_move_to(cr, x, y);
            else cairo_line_to(cr, x, y);
        }
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
        cairo_set_line_width(cr, 4.5 * s);
        cairo_stroke_preserve(cr);
        cairo_set_source_rgba(cr, 1.0, 0.82, 0.25, 0.95);
        cairo_set_line_width(cr, 2.0 * s);
        cairo_stroke(cr);
        /* the latest drive, a moving dot, 8 s of driving then a pause */
        if (r->npts - last0 >= 2) {
            double total = 0, ph = fmod(now_f, 10.0) / 8.0, want, acc = 0;
            double xs[2], ys[2];
            for (i = last0; i + 1 < r->npts; i++) {
                double ax, ay, cx2, cy2;
                merc_px(r->pts[2 * i], r->pts[2 * i + 1], r->z, &ax, &ay);
                merc_px(r->pts[2 * i + 2], r->pts[2 * i + 3], r->z, &cx2, &cy2);
                total += hypot(cx2 - ax, cy2 - ay);
            }
            cairo_new_path(cr);
            for (i = last0; i < r->npts; i++) {
                double x, y;
                merc_px(r->pts[2 * i], r->pts[2 * i + 1], r->z, &x, &y);
                x = bx + (x - r->ox) * k;
                y = by + (y - r->oy) * k;
                if (i == last0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
            }
            cairo_set_source_rgba(cr, 0.45, 1, 0.55, 1);
            cairo_set_line_width(cr, 2.6 * s);
            cairo_stroke(cr);
            if (ph > 1) ph = 1;
            want = ph * total;
            xs[0] = xs[1] = ys[0] = ys[1] = 0;
            for (i = last0; i + 1 < r->npts; i++) {
                double ax, ay, cx2, cy2, seg;
                merc_px(r->pts[2 * i], r->pts[2 * i + 1], r->z, &ax, &ay);
                merc_px(r->pts[2 * i + 2], r->pts[2 * i + 3], r->z, &cx2, &cy2);
                seg = hypot(cx2 - ax, cy2 - ay);
                if (acc + seg >= want || i + 2 == r->npts) {
                    double f = seg > 0 ? clampd((want - acc) / seg, 0, 1) : 1;
                    xs[0] = ax + (cx2 - ax) * f;
                    ys[0] = ay + (cy2 - ay) * f;
                    break;
                }
                acc += seg;
            }
            if (ph < 1) {
                double x = bx + (xs[0] - r->ox) * k, y = by + (ys[0] - r->oy) * k;
                cairo_arc(cr, x, y, 4.5 * s, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
                cairo_fill(cr);
                cairo_arc(cr, x, y, 3.2 * s, 0, 2 * M_PI);
                cairo_set_source_rgb(cr, 1, 1, 1);
                cairo_fill(cr);
            }
        }
    }
    /* the rover itself at its published position, nose along its yaw */
    merc_px(r->lon, r->lat, r->z, &rx, &ry);
    rx = bx + (rx - r->ox) * k;
    ry = by + (ry - r->oy) * k;
    {
        double a = (r->yaw - 90.0) * D2R, L = 16 * s;
        cairo_move_to(cr, rx, ry);
        cairo_line_to(cr, rx + cos(a) * L, ry + sin(a) * L);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
        cairo_set_line_width(cr, 4 * s);
        cairo_stroke(cr);
        cairo_move_to(cr, rx, ry);
        cairo_line_to(cr, rx + cos(a) * L, ry + sin(a) * L);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_set_line_width(cr, 1.8 * s);
        cairo_stroke(cr);
        cairo_arc(cr, rx, ry, 5 * s, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
        cairo_fill(cr);
        cairo_arc(cr, rx, ry, 3.6 * s, 0, 2 * M_PI);
        cairo_set_source_rgb(cr, 0.45, 1, 0.55);
        cairo_fill(cr);
    }
    mpp = 2 * M_PI * 3396190.0 * cos(r->lat * D2R) / (256.0 * (1 << r->z));
    scale_bar(cr, bx + 10 * s, by + H - 12 * s, mpp, k, s);
    {
        double ty = by + 14 * s;
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 9 * s);
        text_outline(cr, bx + W - text_w(cr, "N \xe2\x86\x91") - 8 * s, ty, "N \xe2\x86\x91", 1, 1, 1, 0.9);
    }
    }   /* map */
    cairo_restore(cr);

    /* sprite + captions under the picture */
    draw_thing(cr, r->sprite, r->kind, bx + 26 * s, by + H + tlh / 2, 46 * s, 0);
    snprintf(t1, sizeof(t1), "%s \xc2\xb7 sol %d \xc2\xb7 %.2f km driven", r->name,
             mission_sol(r, now_f), r->dist_total / 1000.0);
    if (photo_mode)
        snprintf(t2, sizeof(t2), "Picture: %s \xc2\xb7 sol %d \xc2\xb7 taken %s UTC \xc2\xb7 raw, unprocessed",
                 r->photo_cam, r->photo_sol, r->photo_date);
    else if (r->last_sol >= 0)
        snprintf(t2, sizeof(t2), "Position: end of sol %d drive (%.0f m) \xc2\xb7 %.4f\xc2\xb0%c %.4f\xc2\xb0" "E",
                 r->last_sol > r->sol ? r->last_sol : r->sol, r->last_len,
                 fabs(r->lat), r->lat < 0 ? 'S' : 'N', r->lon);
    else
        snprintf(t2, sizeof(t2), "Position: sol %d localization (%.0f m drive) \xc2\xb7 %.4f\xc2\xb0%c %.4f\xc2\xb0" "E",
                 r->sol, r->last_len, fabs(r->lat), r->lat < 0 ? 'S' : 'N', r->lon);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12.5 * s);
    text_outline(cr, bx + 56 * s, by + H + 17 * s, t1, 0.55, 1, 0.62, 1);
    {
        const dsn_link_t *L = dsn_link(r->naif);
        if (L) {
            char lk[96];
            double tx = bx + 56 * s + text_w(cr, t1) + 10 * s;
            fmt_link(lk, sizeof(lk), L);
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 9.5 * s);
            if (tx + text_w(cr, lk) < bx + W) text_outline(cr, tx, by + H + 17 * s, lk, 0.5, 1.0, 0.6, 1);
        }
    }
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9.5 * s);
    text_outline(cr, bx + 56 * s, by + H + 32 * s, t2, 0.85, 0.87, 0.9, 0.95);
    if (r == &ROVERS[0] && wx.have) {
        char t3[160];
        snprintf(t3, sizeof(t3), "Gale weather, sol %d (%s): %s to %s \xc2\xb0" "C \xc2\xb7 %s Pa \xc2\xb7 %s",
                 wx.sol, wx.date, wx.tmin, wx.tmax, wx.pres, wx.sky);
        text_outline(cr, bx + 56 * s, by + H + 45 * s, t3, 0.8, 0.85, 1.0, 0.95);
    }
}

/* ── 10. Header, key, credits ──────────────────────────────────── */

static void draw_header(cairo_t *cr, frame_t *F, const char *title, const char *line)
{
    double fs = 26 * F->s, fs2 = 13 * F->s, tw, tw2, y = F->my - 12 * F->s;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    tw = text_w(cr, title);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs2);
    tw2 = text_w(cr, line);
    {
        double x = (F->W - tw - 14 * F->s - tw2) / 2;
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs);
        text_outline(cr, x, y, title, 1, 1, 1, 1);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fs2);
        text_outline(cr, x + tw + 14 * F->s, y - 2 * F->s, line, 0.85, 0.88, 0.92, 1);
    }
}

static void draw_key(cairo_t *cr, frame_t *F, int w, const char *credit, int dusty, int pjk)
{
    double xlim, y_first;
    static const struct { const char *t; int st; } rows[] = {
        { "operating", ST_ACTIVE }, { "mission ended", ST_ENDED },
        { "status unconfirmed", ST_UNKNOWN }, { "crash / impact", ST_CRASH } };
    double s = F->s, x = F->mx + 12 * s, y, fs = 10 * s, cx;
    double bottom = F->my + F->mh;
    if (w == PIC_WORLD_MARS && !zoom_on) x = F->mx + (2 * INSET_W + 50) * s;   /* right of the rover insets */
    int i;
    if (&pic_ticker_bar_top && pic_ticker_bar_top > 0 && pic_ticker_bar_top < bottom) bottom = pic_ticker_bar_top;
    y = bottom - 30 * s;
    y_first = y;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs);
    cx = x;
    for (i = 0; i < 4; i++) {
        double r, g, b;
        if (w == PIC_WORLD_JUPITER && (i == 1 || i == 2)) continue;
        status_rgb(rows[i].st, &r, &g, &b);
        cairo_arc(cr, cx + 4 * s, y - 3.5 * s, 3.2 * s, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
        cairo_fill(cr);
        cairo_arc(cr, cx + 4 * s, y - 3.5 * s, 2.3 * s, 0, 2 * M_PI);
        cairo_set_source_rgb(cr, r, g, b);
        cairo_fill(cr);
        text_outline(cr, cx + 11 * s, y, rows[i].t, 0.92, 0.92, 0.95, 1);
        cx += 11 * s + text_w(cr, rows[i].t) + 14 * s;
    }
    /* orbit track */
    {
        static const double dash[] = { 5, 4 };
        double d2[2] = { dash[0] * s, dash[1] * s };
        cairo_move_to(cr, cx, y - 3.5 * s);
        cairo_line_to(cr, cx + 16 * s, y - 3.5 * s);
        cairo_set_source_rgba(cr, 0.55, 0.9, 1.0, 0.4);
        cairo_set_line_width(cr, 1.6 * s);
        cairo_stroke(cr);
        cairo_move_to(cr, cx + 18 * s, y - 3.5 * s);
        cairo_line_to(cr, cx + 36 * s, y - 3.5 * s);
        cairo_set_dash(cr, d2, 2, 0);
        cairo_set_source_rgba(cr, 0.55, 0.9, 1.0, 0.9);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
        text_outline(cr, cx + 42 * s, y, w == PIC_WORLD_JUPITER ?
                     "ground track, last / next 5 h" : "ground track, last / next ~45 min",
                     0.92, 0.92, 0.95, 1);
        cx += 42 * s + text_w(cr, w == PIC_WORLD_JUPITER ? "ground track, last / next 5 h" :
                              "ground track, last / next ~45 min") + 14 * s;
    }
    xlim = F->mx + F->mw - 20 * s;
    if (time(NULL) - links_at < 300 && nlinks > 0) {
        const char *t = "talking to Earth now (NASA DSN Now)";
        if (cx + 16 * s + text_w(cr, t) > xlim) { cx = x; y -= 15 * s; }
        cairo_new_path(cr);
        cairo_arc(cr, cx + 6 * s, y - 3.5 * s, 5 * s, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 0.5, 1.0, 0.6, 0.9);
        cairo_set_line_width(cr, 1.6 * s);
        cairo_stroke(cr);
        text_outline(cr, cx + 16 * s, y, t, 0.92, 0.92, 0.95, 1);
        cx += 16 * s + text_w(cr, t) + 14 * s;
    }
    if (dusty) {
        const char *t = "tan haze: typical dust for the season (climatology, not live)";
        if (cx + 22 * s + text_w(cr, t) > xlim) { cx = x; y -= 15 * s; }
        cairo_rectangle(cr, cx, y - 8 * s, 16 * s, 9 * s);
        cairo_set_source_rgba(cr, 0.86, 0.66, 0.46, 0.7);
        cairo_fill(cr);
        text_outline(cr, cx + 22 * s, y, t, 0.92, 0.92, 0.95, 1);
    }
    if (pjk) {
        static const double dot[] = { 2, 4 };
        double d2[2] = { dot[0] * s, dot[1] * s };
        cairo_move_to(cr, cx, y - 3.5 * s);
        cairo_line_to(cr, cx + 20 * s, y - 3.5 * s);
        cairo_set_dash(cr, d2, 2, 0);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_source_rgba(cr, 1.0, 0.65, 0.25, 1);
        cairo_set_line_width(cr, 2.2 * s);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
        text_outline(cr, cx + 26 * s, y, "Juno's next close pass (track \xc2\xb1" "90 min)", 0.92, 0.92, 0.95, 1);
    }
    cairo_set_font_size(cr, 9 * s);
    text_outline(cr, x, y_first + 15 * s, credit, 0.75, 0.77, 0.8, 0.9);
    box_add(F, x - 4, y - 14 * s, x + 900 * s, y_first + 18 * s);
}


/* ── 10b. Feature names (optional: VIEW_WORLD_LABELS) ──────────── */

/* Mars and the Moon: IAU names, centres and sizes from the USGS
 * Gazetteer of Planetary Nomenclature. tier 1: large regions (spaced
 * capitals); tier 2: features, mark 1 = circle at true size (craters,
 * volcanoes), mark 2 = dot. THARSIS sits a little north-west of the
 * Tharsis Montes centre so it doesn't cover Pavonis Mons. */
typedef struct { int world, tier; const char *name; double lat, lon, km; int mark; } wlabel_t;
static const wlabel_t WLABELS[] = {
    { PIC_WORLD_MARS, 1, "THARSIS", 5.5703, -126.5835, 2058.9, 0 },
    { PIC_WORLD_MARS, 1, "ARABIA TERRA", 21.2490, 5.7185, 4851.7, 0 },
    { PIC_WORLD_MARS, 1, "UTOPIA PLANITIA", 46.7363, 117.5168, 3560.4, 0 },
    { PIC_WORLD_MARS, 1, "HELLAS", -42.4301, 70.5025, 2299.2, 0 },
    { PIC_WORLD_MARS, 1, "ARGYRE", -49.8406, -43.3098, 892.9, 0 },
    { PIC_WORLD_MARS, 1, "AMAZONIS PLANITIA", 25.7461, -162.9128, 2809.0, 0 },
    { PIC_WORLD_MARS, 1, "ACIDALIA PLANITIA", 49.7600, -20.7400, 3505.0, 0 },
    { PIC_WORLD_MARS, 1, "ELYSIUM PLANITIA", 2.9790, 154.7372, 3000.8, 0 },
    { PIC_WORLD_MARS, 1, "TERRA CIMMERIA", -32.6795, 147.7458, 5855.9, 0 },
    { PIC_WORLD_MARS, 1, "TERRA SIRENUM", -39.4946, -154.1521, 3635.2, 0 },
    { PIC_WORLD_MARS, 1, "NOACHIS TERRA", -50.4072, -5.1567, 5519.4, 0 },
    { PIC_WORLD_MARS, 1, "SYRTIS MAJOR", 9.2007, 67.1030, 1214.9, 0 },
    { PIC_WORLD_MARS, 1, "ISIDIS PLANITIA", 13.9357, 88.3772, 1224.6, 0 },
    { PIC_WORLD_MARS, 1, "CHRYSE PLANITIA", 28.4333, -40.3073, 1542.4, 0 },
    { PIC_WORLD_MARS, 1, "VALLES MARINERIS", -14.0059, -58.5877, 3761.3, 0 },
    { PIC_WORLD_MARS, 1, "ARCADIA PLANITIA", 49.0200, -171.8500, 2245.0, 0 },
    { PIC_WORLD_MARS, 1, "TEMPE TERRA", 38.6877, -70.6143, 1954.9, 0 },
    { PIC_WORLD_MARS, 1, "TERRA SABAEA", 2.7188, 51.3035, 4688.4, 0 },
    { PIC_WORLD_MARS, 1, "VASTITAS BOREALIS", 87.7297, 32.5298, 2002.9, 0 },
    { PIC_WORLD_MARS, 2, "Olympus Mons", 18.6528, -133.8025, 610.1, 1 },
    { PIC_WORLD_MARS, 2, "Arsia Mons", -8.2571, -120.0925, 470.0, 1 },
    { PIC_WORLD_MARS, 2, "Pavonis Mons", 1.4801, -112.9624, 366.5, 1 },
    { PIC_WORLD_MARS, 2, "Ascraeus Mons", 11.9216, -104.0808, 456.4, 1 },
    { PIC_WORLD_MARS, 2, "Alba Mons", 41.0820, -110.7094, 548.0, 1 },
    { PIC_WORLD_MARS, 2, "Elysium Mons", 25.0232, 147.2138, 401.0, 1 },
    { PIC_WORLD_MARS, 2, "Jezero crater", 18.4082, 77.6873, 47.5, 1 },
    { PIC_WORLD_MARS, 2, "Gale crater", -5.3672, 137.8110, 154.1, 1 },
    { PIC_WORLD_MARS, 2, "Gusev crater", -14.5308, 175.5244, 158.1, 1 },
    { PIC_WORLD_MARS, 2, "Huygens crater", -13.8819, 55.5817, 467.3, 1 },
    { PIC_WORLD_MARS, 2, "Schiaparelli crater", -2.7138, 16.7716, 458.5, 1 },
    { PIC_WORLD_MARS, 2, "Noctis Labyrinthus", -6.3625, -101.1889, 1190.3, 2 },
    { PIC_WORLD_MARS, 2, "Lyot crater", 50.4671, 29.3413, 221.5, 1 },
    { PIC_WORLD_MARS, 2, "Holden crater", -26.0400, -34.0190, 152.7, 1 },
    { PIC_WORLD_MOON, 1, "OCEANUS PROCELLARUM", 20.6714, -56.6774, 2592.2, 0 },
    { PIC_WORLD_MOON, 1, "MARE IMBRIUM", 34.7244, -14.9086, 1145.5, 0 },
    { PIC_WORLD_MOON, 1, "MARE SERENITATIS", 27.2879, 18.3596, 674.3, 0 },
    { PIC_WORLD_MOON, 1, "MARE TRANQUILLITATIS", 8.3487, 30.8346, 875.7, 0 },
    { PIC_WORLD_MOON, 1, "MARE CRISIUM", 16.1774, 59.1037, 555.9, 0 },
    { PIC_WORLD_MOON, 1, "MARE FECUNDITATIS", -7.8350, 53.6691, 840.4, 0 },
    { PIC_WORLD_MOON, 1, "MARE NECTARIS", -15.1852, 34.6021, 339.4, 0 },
    { PIC_WORLD_MOON, 1, "MARE NUBIUM", -20.5894, -17.2872, 714.5, 0 },
    { PIC_WORLD_MOON, 1, "MARE HUMORUM", -24.4785, -38.5716, 419.7, 0 },
    { PIC_WORLD_MOON, 1, "MARE FRIGORIS", 57.5923, -0.0064, 1446.4, 0 },
    { PIC_WORLD_MOON, 1, "MARE ORIENTALE", -19.8655, -94.6703, 294.2, 0 },
    { PIC_WORLD_MOON, 1, "MARE MOSCOVIENSE", 27.2816, 148.1225, 275.6, 0 },
    { PIC_WORLD_MOON, 1, "MARE AUSTRALE", -47.7705, 91.9854, 996.8, 0 },
    { PIC_WORLD_MOON, 1, "MARE SMYTHII", -1.7088, 87.0495, 374.0, 0 },
    { PIC_WORLD_MOON, 1, "MARE MARGINIS", 12.7019, 86.5153, 357.6, 0 },
    { PIC_WORLD_MOON, 1, "MARE INGENII", -33.2458, 164.8268, 282.2, 0 },
    { PIC_WORLD_MOON, 1, "MONTES APENNINUS", 19.8714, 0.0253, 599.7, 0 },
    { PIC_WORLD_MOON, 2, "Tycho", -43.2958, -11.2153, 85.3, 1 },
    { PIC_WORLD_MOON, 2, "Copernicus", 9.6209, -20.0786, 96.1, 1 },
    { PIC_WORLD_MOON, 2, "Kepler", 8.1210, -38.0087, 29.5, 1 },
    { PIC_WORLD_MOON, 2, "Aristarchus", 23.7299, -47.4901, 40.0, 1 },
    { PIC_WORLD_MOON, 2, "Plato", 51.6192, -9.3825, 100.7, 1 },
    { PIC_WORLD_MOON, 2, "Clavius", -58.6228, -14.7275, 230.8, 1 },
    { PIC_WORLD_MOON, 2, "Grimaldi", -5.3800, -68.3600, 235.0, 1 },
    { PIC_WORLD_MOON, 2, "Tsiolkovskiy", -20.3789, 128.9715, 184.4, 1 },
    { PIC_WORLD_MOON, 2, "Korolev", -4.1913, -157.4082, 423.4, 1 },
    { PIC_WORLD_MOON, 2, "Hertzsprung", 1.3651, -128.6561, 536.4, 1 },
    { PIC_WORLD_MOON, 2, "Apollo", -35.6874, -151.4777, 524.2, 1 },
    { PIC_WORLD_MOON, 2, "Shackleton", -89.6700, 129.7800, 20.9, 1 },
    { PIC_WORLD_MOON, 2, "Langrenus", -8.8604, 61.0380, 132.0, 1 },
    { PIC_WORLD_MOON, 2, "Petavius", -25.3914, 60.7776, 184.1, 1 },
    { PIC_WORLD_MOON, 2, "Aitken", -16.4430, 172.9583, 129.7, 1 },
};
#define NWLABELS ((int)(sizeof(WLABELS) / sizeof(WLABELS[0])))

/* Jupiter: belts and zones where they lie on this map (measured from
 * the Cassini map's brightness profile; the GRS sits in the STrZ) */
static const struct { double lat; const char *name; } JBANDS[] = {
    { 0, "EQUATORIAL ZONE" }, { 12, "NORTH EQUATORIAL BELT" }, { -13, "SOUTH EQUATORIAL BELT" },
    { 20.5, "NORTH TROPICAL ZONE" }, { -24, "SOUTH TROPICAL ZONE" },
    { 56, "NORTH POLAR REGION" }, { -56, "SOUTH POLAR REGION" },
};


static int utf8_len(const char *p)
{
    if ((*p & 0xE0) == 0xC0) return 2;
    if ((*p & 0xF0) == 0xE0) return 3;
    if ((*p & 0xF8) == 0xF0) return 4;
    return 1;
}

/* Letter-spaced text width */
static double spaced_w(cairo_t *cr, const char *s, double gap)
{
    double w = 0;
    char ch[5];
    const char *p = s;
    while (*p) {
        int n = utf8_len(p);
        memcpy(ch, p, (size_t)n);
        ch[n] = 0;
        w += text_w(cr, ch) + (p[n] ? gap : 0);
        p += n;
    }
    return w;
}

static void spaced_text(cairo_t *cr, double x, double y, const char *s, double gap, double a)
{
    char ch[5];
    const char *p = s;
    while (*p) {
        int n = utf8_len(p);
        memcpy(ch, p, (size_t)n);
        ch[n] = 0;
        text_outline(cr, x, y, ch, 1.0, 0.96, 0.88, a);
        x += text_w(cr, ch) + gap;
        p += n;
    }
}

static int region_label(cairo_t *cr, frame_t *F, double x, double y, const char *s, double fs, double a)
{
    double gap = fs * 0.32, w, h = fs;
    box_t b;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    static const double nudge[][2] = { { 0, 0 }, { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 },
                                       { 0, -2 }, { 0, 2 }, { -1, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 } };
    int k;
    w = spaced_w(cr, s, gap);
    /* the name may shift a little (up to ~2 lines / a third of its
     * width) to make room for the mission callouts */
    for (k = 0; k < (int)(sizeof(nudge) / sizeof(nudge[0])); k++) {
        double cx = x + nudge[k][0] * w / 3, cy = y + nudge[k][1] * h * 1.4;
        cx = fmin(fmax(cx, F->mx + 6 * F->s + w / 2), F->mx + F->mw - 6 * F->s - w / 2);
        b.x0 = cx - w / 2; b.x1 = cx + w / 2; b.y0 = cy - h * 0.6; b.y1 = cy + h * 0.6;
        if (box_hits(F, b)) continue;
        spaced_text(cr, b.x0, cy + fs * 0.35, s, gap, a);
        box_add(F, b.x0, b.y0, b.x1, b.y1);
        return 1;
    }
    return 0;
}

static void feature_label(cairo_t *cr, frame_t *F, int w, const wlabel_t *L)
{
    static const double side[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    double s = F->s, x, y, circ = 2 * M_PI * WORLDS[w].mean;
    double r = L->mark == 1 ? fmax(3 * s, fmin(18 * s, L->km / circ * F->mw / 2)) : 2.5 * s;
    double fs = 11 * s, tw, tw2;
    char sz[24];
    int k;
    map_xy(F, L->lon, L->lat, &x, &y);
    snprintf(sz, sizeof(sz), "%.0f km", L->km);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    tw = text_w(cr, L->name);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9.5 * s);
    tw2 = text_w(cr, sz);
    tw = fmax(tw, tw2);
    for (k = 0; k < 4; k++) {
        double lx, ly;
        box_t b;
        if (side[k][0] > 0) lx = x + r + 5 * s;
        else if (side[k][0] < 0) lx = x - r - 5 * s - tw;
        else lx = x - tw / 2;
        ly = side[k][0] != 0 ? y - 2 * s : side[k][1] > 0 ? y + r + 12 * s : y - r - 16 * s;
        b.x0 = lx - 2; b.x1 = lx + tw + 2; b.y0 = ly - 11 * s; b.y1 = ly + 13 * s;
        if (box_hits(F, b)) continue;
        cairo_new_path(cr);
        cairo_arc(cr, x, y, r, 0, 2 * M_PI);
        if (L->mark == 1) {
            cairo_set_source_rgba(cr, 0, 0, 0, 0.45);
            cairo_set_line_width(cr, 2.6 * s);
            cairo_stroke_preserve(cr);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.75);
            cairo_set_line_width(cr, 1.1 * s);
            cairo_stroke(cr);
        } else {
            cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
            cairo_fill(cr);
        }
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs);
        text_outline(cr, lx, ly, L->name, 1, 1, 1, 0.92);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 9.5 * s);
        text_outline(cr, lx, ly + 11 * s, sz, 0.88, 0.9, 0.93, 0.85);
        box_add(F, b.x0, b.y0, b.x1, b.y1);
        box_add(F, x - r, y - r, x + r, y + r);
        return;
    }
}

/* tier 1: big regions (drawn before the mission callouts, which make
 * room for them); tier 2: features (after, in the space left) */
static void draw_labels(cairo_t *cr, frame_t *F, int w, int tier)
{
    double s = F->s;
    int i, pass;
    if (!labels_on) return;
    if (w == PIC_WORLD_JUPITER) {
        if (tier != 1) return;
        for (i = 0; i < (int)(sizeof(JBANDS) / sizeof(JBANDS[0])); i++) {
            double y = F->my + (90.0 - JBANDS[i].lat) / 180.0 * F->mh;
            region_label(cr, F, F->mx + 150 * s, y, JBANDS[i].name, 11 * s, 0.8);
            region_label(cr, F, F->mx + F->mw - 150 * s, y, JBANDS[i].name, 11 * s, 0.8);
        }
        return;
    }
    for (pass = tier; pass == tier; pass++)
        for (i = 0; i < NWLABELS; i++) {
            const wlabel_t *L = &WLABELS[i];
            if (L->world != w || L->tier != pass) continue;
            if (pass == 2) {
                feature_label(cr, F, w, L);
            } else {
                double x, y;
                double fs = (L->km > 2500 || (w == PIC_WORLD_MOON && L->km > 600)) ? 13.5 * s : 11 * s;
                map_xy(F, L->lon, L->lat, &x, &y);
                region_label(cr, F, x, y, L->name, fs, 0.72);
            }
        }
}


/* ── 10c. Mars: zoomed-in rover views ──────────────────────────── */

/*
 * In the Mars slot the view takes turns: the whole planet, then each
 * rover's surroundings close up (HiRISE colour at ZOOM_Z) with its
 * latest picture. The camera model and rover attitude published with
 * the picture give the direction and width of the view (the wedge);
 * with an elevation patch around the rover (tools/dem_patch.py: USGS
 * 1 m Gale / 20 m Jezero models) each image column is traced over the
 * terrain to outline the ground actually in the picture, and the
 * skyline band ("A") is marked on both the map and the picture.
 */
#define ZOOM_Z   16
#define ZOOM_W   1920
#define ZOOM_H   960
#define ZOOM_RX  0.38          /* rover at 38% of the width, mid-height */
#define FP_COLS  48
#define R_MARS_EQ 3396190.0    /* sphere used by the elevation models */


static void zoom_tiles_plan(rover_t *r)
{
    double cx, cy;
    merc_px(r->lon, r->lat, ZOOM_Z, &cx, &cy);
    r->zox = floor(cx - ZOOM_W * ZOOM_RX);
    r->zoy = floor(cy - ZOOM_H / 2.0);
    r->ztx0 = (int)floor(r->zox / 256.0);
    r->zty0 = (int)floor(r->zoy / 256.0);
    r->ztw = (int)floor((r->zox + ZOOM_W - 1) / 256.0) - r->ztx0 + 1;
    r->zth = (int)floor((r->zoy + ZOOM_H - 1) / 256.0) - r->zty0 + 1;
}

/* Are all the zoomed view's tiles on disk? (at start-up) */
static void zoom_tiles_check(rover_t *r)
{
    int x, y, all = 1;
    char path[256];
    struct stat st;
    if (!r->have) return;
    zoom_tiles_plan(r);
    for (y = r->zty0; y < r->zty0 + r->zth; y++)
        for (x = r->ztx0; x < r->ztx0 + r->ztw; x++) {
            tile_path(path, sizeof(path), r, ZOOM_Z, x, y);
            if (stat(path, &st) != 0 || st.st_size <= 100) all = 0;
        }
    r->ztiles_ready = all;
}

/* Fetch thread: download the zoomed view's tiles (once per rover move) */
static void fetch_zoom_tiles(rover_t *r)
{
    int x, y, all = 1, nt = 1 << ZOOM_Z, tx0, ty0, tw, th;
    char path[256], url[320];
    struct stat st;
    pthread_mutex_lock(&data_lock);
    if (!r->have) { pthread_mutex_unlock(&data_lock); return; }
    zoom_tiles_plan(r);
    tx0 = r->ztx0; ty0 = r->zty0; tw = r->ztw; th = r->zth;
    pthread_mutex_unlock(&data_lock);
    for (y = ty0; y < ty0 + th; y++)
        for (x = tx0; x < tx0 + tw; x++) {
            tile_path(path, sizeof(path), r, ZOOM_Z, x, y);
            if (stat(path, &st) == 0 && st.st_size > 100) continue;
            snprintf(url, sizeof(url), "%s/%d/%d/%d.png", ROVER_TILES[r - ROVERS], ZOOM_Z, x, nt - 1 - y);
            if (pic_fetch_to_file(url, path, 60) != 0) { unlink(path); all = 0; }
        }
    pthread_mutex_lock(&data_lock);
    r->ztiles_ready = all;
    pthread_mutex_unlock(&data_lock);
}

/* Render thread: compose the tiles once per rover position */
static cairo_surface_t *zoom_get(rover_t *r)
{
    int x, y;
    cairo_t *c;
    if (!r->ztiles_ready) return NULL;
    if (r->zoom && r->zoom_ox == r->zox && r->zoom_oy == r->zoy) return r->zoom;
    if (r->zoom) cairo_surface_destroy(r->zoom);
    r->zoom = cairo_image_surface_create(CAIRO_FORMAT_RGB24, ZOOM_W, ZOOM_H);
    c = cairo_create(r->zoom);
    cairo_set_source_rgb(c, 0.1, 0.07, 0.05);
    cairo_paint(c);
    for (y = r->zty0; y < r->zty0 + r->zth; y++)
        for (x = r->ztx0; x < r->ztx0 + r->ztw; x++) {
            char path[256];
            cairo_surface_t *t;
            tile_path(path, sizeof(path), r, ZOOM_Z, x, y);
            t = pic_image_load(path);
            if (!t) continue;
            cairo_set_source_surface(c, t, x * 256.0 - r->zox, y * 256.0 - r->zoy);
            cairo_paint(c);
            cairo_surface_destroy(t);
        }
    cairo_destroy(c);
    r->zoom_ox = r->zox;
    r->zoom_oy = r->zoy;
    return r->zoom;
}

/* Elevation patch: "PDEM", int32 w, h, double lat, lon (centre),
 * step (m), then w*h float32 metres, row 0 = north edge */
static void dem_load(rover_t *r)
{
    char path[256];
    struct stat st;
    FILE *f;
    char mg[4];
    int32_t wh[2];
    double hd[3];
    snprintf(path, sizeof(path), "%s/%s_dem.bin", dir_cache(), r->key);
    if (stat(path, &st) != 0) return;
    if (r->dem && st.st_mtime == r->dem_mtime) return;
    f = fopen(path, "rb");
    if (!f) return;
    if (fread(mg, 1, 4, f) == 4 && !memcmp(mg, "PDEM", 4) && fread(wh, 4, 2, f) == 2 &&
        fread(hd, 8, 3, f) == 3 && wh[0] > 10 && wh[1] > 10 && wh[0] < 8000 && wh[1] < 8000) {
        float *d = malloc((size_t)wh[0] * wh[1] * sizeof(float));
        if (d && fread(d, sizeof(float), (size_t)wh[0] * wh[1], f) == (size_t)wh[0] * wh[1]) {
            free(r->dem);
            r->dem = d;
            r->dem_w = wh[0]; r->dem_h = wh[1];
            r->dem_lat = hd[0]; r->dem_lon = hd[1]; r->dem_step = hd[2];
            r->dem_mtime = st.st_mtime;
        } else {
            free(d);
        }
    }
    fclose(f);
}

static void qrot(const double q[4], const double v[3], double out[3])
{
    double w = q[0], x = q[1], y = q[2], z = q[3];
    double t[3] = { 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0]) };
    out[0] = v[0] + w * t[0] + (y * t[2] - z * t[1]);
    out[1] = v[1] + w * t[1] + (z * t[0] - x * t[2]);
    out[2] = v[2] + w * t[2] + (x * t[1] - y * t[0]);
}

static double vdot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* Ray through full-frame pixel (px, py), in the site frame (N, E, down) */
static void pixel_ray(const rover_t *r, double px, double py, double out[3])
{
    double a[3], b[3], v[3], n;
    int i;
    for (i = 0; i < 3; i++) { a[i] = r->cV[i] - py * r->cA[i]; b[i] = r->cH[i] - px * r->cA[i]; }
    v[0] = a[1] * b[2] - a[2] * b[1];
    v[1] = a[2] * b[0] - a[0] * b[2];
    v[2] = a[0] * b[1] - a[1] * b[0];
    if (vdot(v, r->cA) < 0) { v[0] = -v[0]; v[1] = -v[1]; v[2] = -v[2]; }
    n = sqrt(vdot(v, v));
    for (i = 0; i < 3; i++) v[i] /= n;
    qrot(r->q, v, out);
}

/* Elevation (m) at north/east offset (m) from the rover; NAN outside */
static double dem_at(const rover_t *r, double n_m, double e_m)
{
    double coslat = cos(r->dem_lat * D2R);
    double dn = (r->lat - r->dem_lat) * D2R * R_MARS_EQ + n_m;
    double de = (r->lon - r->dem_lon) * D2R * R_MARS_EQ * coslat + e_m;
    double row = r->dem_h / 2.0 - dn / r->dem_step - 0.5, col = r->dem_w / 2.0 + de / r->dem_step - 0.5;
    int i = (int)floor(row), j = (int)floor(col);
    double fr = row - i, fc = col - j;
    const float *p;
    if (i < 0 || j < 0 || i >= r->dem_h - 1 || j >= r->dem_w - 1) return NAN;
    p = r->dem + (size_t)i * r->dem_w + j;                 /* bilinear */
    return (p[0] * (1 - fc) + p[1] * fc) * (1 - fr) + (p[r->dem_w] * (1 - fc) + p[r->dem_w + 1] * fc) * fr;
}

typedef struct {
    int ok, n;
    double near_d[FP_COLS + 1], far_d[FP_COLS + 1], az[FP_COLS + 1];
    double band_px[FP_COLS + 1], band_top[FP_COLS + 1], band_bot[FP_COLS + 1];
    int band_ok[FP_COLS + 1];
    double dmin, dmax, nmin, az_c, el_c, fov, el_top, hcam;
    int sky;                   /* the picture reaches the skyline        */
} footprint_t;

/* Trace each image column over the terrain */
static void footprint_calc(const rover_t *r, footprint_t *fp)
{
    double C_site[3], hcam, z0, g0;
    int k;
    memset(fp, 0, sizeof(*fp));
    if (!r->geom) return;
    {   /* centre direction and width (edge columns at mid-height) */
        double c[3], lft[3], rgt[3], my = r->sub_y0 + r->sub_h / 2;
        pixel_ray(r, r->sub_x0 + r->sub_w / 2, my, c);
        pixel_ray(r, r->sub_x0, my, lft);
        pixel_ray(r, r->sub_x0 + r->sub_w - 1, my, rgt);
        fp->az_c = fmod(atan2(c[1], c[0]) / D2R + 360, 360);
        fp->el_c = -asin(c[2]) / D2R;
        fp->fov = acos(fmax(-1, fmin(1, vdot(lft, rgt)))) / D2R;
        pixel_ray(r, r->sub_x0 + r->sub_w / 2, r->sub_y0, c);
        fp->el_top = -asin(c[2]) / D2R;
    }
    qrot(r->q, r->cC, C_site);
    hcam = fmax(0.5, fmin(3.0, -C_site[2]));
    fp->hcam = hcam;
    fp->ok = 1;
    if (!r->dem) return;
    g0 = dem_at(r, 0, 0);
    if (isnan(g0)) return;
    z0 = g0 + hcam;
    fp->dmin = 1e9; fp->dmax = 0; fp->nmin = 1e9;
    for (k = 0; k <= FP_COLS; k++) {
        double px = r->sub_x0 + r->sub_w * k / FP_COLS, bot[3], top[3], tb, tt, dn, de, d, best = -1e9;
        double first = 0, last = 0;
        pixel_ray(r, px, r->sub_y0 + r->sub_h - 1, bot);
        pixel_ray(r, px, r->sub_y0, top);
        fp->az[k] = atan2(bot[1], bot[0]);
        dn = cos(fp->az[k]); de = sin(fp->az[k]);
        tb = -asin(bot[2]); tt = -asin(top[2]);
        for (d = 0.25; d < 6000; d += d < 10 ? 0.1 : d < 50 ? 1 : d < 300 ? 2 : d < 1500 ? 4 : 8) {
            double g = dem_at(r, dn * d, de * d), ang;
            if (isnan(g)) break;
            ang = atan2(g - z0, d);
            if (ang > best) {                 /* visible from the camera */
                best = ang;
                if (ang >= tb && ang <= tt) { if (!first) first = d; last = d; }
            }
        }
        fp->near_d[k] = first;
        fp->far_d[k] = last;
        if (last > 0) {
            fp->n++;
            if (last < fp->dmin) fp->dmin = last;
            if (last > fp->dmax) fp->dmax = last;
            if (first < fp->nmin) fp->nmin = first;
        }
    }
    if (!fp->n) return;
    /* the band just below the far edge, projected into the picture */
    for (k = 0; k <= FP_COLS; k++) {
        double d, top = 1e9, botm = -1e9, sumx = 0;
        int cnt = 0;
        double qi[4] = { r->q[0], -r->q[1], -r->q[2], -r->q[3] };
        if (fp->far_d[k] <= 0) continue;
        for (d = fp->far_d[k] * 0.8; d <= fp->far_d[k] + 0.5; d += fmax(1.0, fp->far_d[k] / 60)) {
            double g = dem_at(r, cos(fp->az[k]) * d, sin(fp->az[k]) * d), v[3], P[3], pa;
            int i;
            if (isnan(g)) continue;
            /* site frame, origin at the rover's ground point (z down) */
            v[0] = cos(fp->az[k]) * d; v[1] = sin(fp->az[k]) * d; v[2] = -(g - g0);
            qrot(qi, v, P);                                 /* rover frame */
            for (i = 0; i < 3; i++) P[i] -= r->cC[i];
            pa = vdot(P, r->cA);
            if (pa <= 0) continue;
            {
                double x = vdot(P, r->cH) / pa - r->sub_x0, y = vdot(P, r->cV) / pa - r->sub_y0;
                sumx += x; cnt++;
                if (y < top) top = y;
                if (y > botm) botm = y;
            }
        }
        if (cnt) {
            fp->band_ok[k] = 1;
            fp->band_px[k] = sumx / cnt;
            fp->band_top[k] = top;
            fp->band_bot[k] = botm;
        }
    }
    fp->sky = fp->dmax > 50;
    if (!fp->sky) fp->dmin = fp->nmin;     /* close-up: from its nearest ground */
}

static const char *compass16(double az)
{
    static const char *N[] = { "north", "north-northeast", "northeast", "east-northeast", "east",
        "east-southeast", "southeast", "south-southeast", "south", "south-southwest", "southwest",
        "west-southwest", "west", "west-northwest", "northwest", "north-northwest" };
    return N[(int)floor(fmod(az + 11.25 + 360, 360) / 22.5) % 16];
}

static void zoom_xy(const rover_t *r, double lon, double lat, double sc, double *x, double *y)
{
    double px, py;
    merc_px(lon, lat, ZOOM_Z, &px, &py);
    *x = (px - r->zoom_ox) * sc;
    *y = (py - r->zoom_oy) * sc;
}

/* offset (m north, m east) from the rover -> lon, lat */
static void off_ll(const rover_t *r, double n_m, double e_m, double *lon, double *lat)
{
    *lat = r->lat + n_m / (R_MARS_EQ * D2R);
    *lon = r->lon + e_m / (R_MARS_EQ * D2R * cos(r->lat * D2R));
}


/* The zoomed view of one rover. Returns 0 when it isn't ready. */
static int render_zoom(cairo_t *cr, frame_t *F, rover_t *r, double t)
{
    cairo_surface_t *map = zoom_get(r);
    double s = F->s, sc = F->mw / (double)ZOOM_W, rx, ry, mpp;
    footprint_t fp;
    char title[64], line[256];
    int k;
    const int A_R = 90, A_G = 230, A_B = 255;     /* box A colour */
    if (!map || !r->have) return 0;
    dem_load(r);
    footprint_calc(r, &fp);
    mpp = 2 * M_PI * R_MARS_EQ * cos(r->lat * D2R) / (256.0 * (1 << ZOOM_Z)) / sc;   /* m per screen px */

    cairo_save(cr);
    cairo_translate(cr, F->mx, F->my);
    cairo_rectangle(cr, 0, 0, F->mw, F->mh);
    cairo_clip(cr);
    cairo_save(cr);
    cairo_scale(cr, sc, sc);
    cairo_set_source_surface(cr, map, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
    zoom_xy(r, r->lon, r->lat, sc, &rx, &ry);

    /* route of the last drives */
    if (r->pts && r->nfeat > 0) {
        int i0 = r->feat_start[r->nfeat > 12 ? r->nfeat - 12 : 0], i;
        for (i = i0; i < r->npts; i++) {
            double x, y;
            zoom_xy(r, r->pts[2 * i], r->pts[2 * i + 1], sc, &x, &y);
            if (i == i0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
        }
        cairo_set_source_rgba(cr, 1.0, 0.8, 0.24, 0.9);
        cairo_set_line_width(cr, 3 * s);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_stroke(cr);
    }

    /* the rover */
    draw_thing(cr, r->sprite, r->kind, rx, ry, 56 * s, 0);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 15 * s);
    text_outline(cr, rx + 34 * s, ry - 6 * s, r->name, 0.5, 1.0, 0.6, 1);
    {
        char l2[96];
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11 * s);
        snprintf(l2, sizeof(l2), "sol %d \xc2\xb7 %.2f km driven", mission_sol(r, t), r->dist_total / 1000);
        text_outline(cr, rx + 34 * s, ry + 10 * s, l2, 0.9, 0.92, 0.95, 1);
    }

    /* the wedge: direction and width of the picture */
    if (fp.ok) {
        double reach = 1100 / mpp, a0 = fp.az[0], a1 = fp.az[FP_COLS];
        /* aimed at the ground: the wedge ends where the top of the
         * picture meets it (the traced ground, else flat ground) */
        if (fp.n && !fp.sky) reach = fmax(fp.dmax * 1.3, 12 * s * mpp) / mpp;
        else if (!fp.n && fp.el_top < -2) reach = fmin(1100, fp.hcam / tan(-fp.el_top * D2R) * 1.3) / mpp;
        if (reach < 14 * s) reach = 14 * s;
        cairo_pattern_t *g = cairo_pattern_create_radial(rx, ry, 0, rx, ry, reach);
        cairo_pattern_add_color_stop_rgba(g, 0, 1, 0.69, 0.2, 0.32);
        cairo_pattern_add_color_stop_rgba(g, 1, 1, 0.69, 0.2, 0.04);
        cairo_move_to(cr, rx, ry);
        for (k = 0; k <= 30; k++) {
            double a = a0 + (a1 - a0) * k / 30.0;   /* screen: x east, y south */
            if (fabs(a1 - a0) > M_PI) a = a0 + (a1 - a0 - (a1 > a0 ? 2 * M_PI : -2 * M_PI)) * k / 30.0;
            cairo_line_to(cr, rx + reach * sin(a), ry - reach * cos(a));
        }
        cairo_close_path(cr);
        cairo_set_source(cr, g);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(g);
        cairo_set_source_rgba(cr, 1, 0.75, 0.3, 0.85);
        cairo_set_line_width(cr, 1.6 * s);
        cairo_stroke(cr);
    }

    /* the ground in the picture, and the band A below its far edge */
    if (fp.n) {
        double bx0 = 1e9, by0 = 1e9, bx1 = -1e9, by1 = -1e9;
        int first = 1;
        for (k = 0; k <= FP_COLS; k++) {
            double lon, lat, x, y;
            if (fp.near_d[k] <= 0) continue;
            off_ll(r, cos(fp.az[k]) * fp.near_d[k], sin(fp.az[k]) * fp.near_d[k], &lon, &lat);
            zoom_xy(r, lon, lat, sc, &x, &y);
            if (first) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
            first = 0;
        }
        for (k = FP_COLS; k >= 0; k--) {
            double lon, lat, x, y;
            if (fp.far_d[k] <= 0) continue;
            off_ll(r, cos(fp.az[k]) * fp.far_d[k], sin(fp.az[k]) * fp.far_d[k], &lon, &lat);
            zoom_xy(r, lon, lat, sc, &x, &y);
            cairo_line_to(cr, x, y);
        }
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
        cairo_set_line_width(cr, 2.4 * s);
        cairo_stroke(cr);
        for (k = 0; k <= FP_COLS; k++) {
            double f;
            if (fp.far_d[k] <= 0) continue;
            for (f = 0.8; f <= 1.001; f += 0.2) {
                double lon, lat, x, y, d = fp.far_d[k] * f;
                off_ll(r, cos(fp.az[k]) * d, sin(fp.az[k]) * d, &lon, &lat);
                zoom_xy(r, lon, lat, sc, &x, &y);
                bx0 = fmin(bx0, x); bx1 = fmax(bx1, x); by0 = fmin(by0, y); by1 = fmax(by1, y);
            }
        }
        if (!fp.sky) {             /* a close-up: box the ground patch itself */
            bx0 = 1e9; by0 = 1e9; bx1 = -1e9; by1 = -1e9;
            for (k = 0; k <= FP_COLS; k++) {
                double f;
                if (fp.far_d[k] <= 0) continue;
                for (f = 0; f <= 1.001; f += 0.5) {
                    double lon, lat, x, y, d = fp.near_d[k] + (fp.far_d[k] - fp.near_d[k]) * f;
                    off_ll(r, cos(fp.az[k]) * d, sin(fp.az[k]) * d, &lon, &lat);
                    zoom_xy(r, lon, lat, sc, &x, &y);
                    bx0 = fmin(bx0, x); bx1 = fmax(bx1, x); by0 = fmin(by0, y); by1 = fmax(by1, y);
                }
            }
            if (bx1 - bx0 < 20 * s) { double c = (bx0 + bx1) / 2; bx0 = c - 10 * s; bx1 = c + 10 * s; }
            if (by1 - by0 < 20 * s) { double c = (by0 + by1) / 2; by0 = c - 10 * s; by1 = c + 10 * s; }
        }
        if (bx1 > bx0) {
            char lab[120];
            bx0 -= 8 * s; by0 -= 8 * s; bx1 += 8 * s; by1 += 8 * s;
            cairo_rectangle(cr, bx0, by0, bx1 - bx0, by1 - by0);
            cairo_set_source_rgba(cr, A_R / 255.0, A_G / 255.0, A_B / 255.0, 1);
            cairo_set_line_width(cr, 2.6 * s);
            cairo_stroke(cr);
            if (fp.sky)
                snprintf(lab, sizeof(lab), "A  the skyline in the picture, %.0f\xe2\x80\x93%.0f m %s",
                         floor(fp.dmin / 10) * 10, ceil(fp.dmax / 10) * 10, compass16(fp.az_c));
            else
                snprintf(lab, sizeof(lab), "A  the ground in the picture, %.0f\xe2\x80\x93%.0f m %s of the rover",
                         fmax(0, floor(fp.dmin)), ceil(fp.dmax), compass16(fp.az_c));
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, 13 * s);
            text_outline(cr, bx0, by1 + 16 * s, lab, A_R / 255.0, A_G / 255.0, A_B / 255.0, 1);
        }
    }

    /* north arrow and scale bar */
    cairo_move_to(cr, 40 * s, 30 * s);
    cairo_line_to(cr, 48 * s, 52 * s);
    cairo_line_to(cr, 40 * s, 47 * s);
    cairo_line_to(cr, 32 * s, 52 * s);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
    cairo_fill(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12 * s);
    text_outline(cr, 35 * s, 66 * s, "N", 1, 1, 1, 1);
    {
        double L = 500 / mpp, y = F->mh - 66 * s;
        cairo_rectangle(cr, 24 * s, y, L, 5 * s);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
        cairo_fill(cr);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11 * s);
        text_outline(cr, 24 * s, y + 18 * s, "500 m", 1, 1, 1, 1);
    }

    /* the picture */
    if (r->photo_gen != r->photo_loaded) {
        char path[256];
        if (r->photo) cairo_surface_destroy(r->photo);
        photo_path(path, sizeof(path), r);
        r->photo = pic_image_load(path);
        r->photo_loaded = r->photo_gen;
    }
    if (r->photo) {
        double iw = cairo_image_surface_get_width(r->photo), ih = cairo_image_surface_get_height(r->photo);
        double PW = 860 * s, PH = PW * ih / iw, px, py, k2, cap_x;
        char c1[160], c2[200];
        if (PH > 560 * s) { PH = 560 * s; PW = PH * iw / ih; }
        px = F->mw - PW - 24 * s; py = 24 * s;
        k2 = PW / iw;
        {   /* the caption panel is at least as wide as its text */
            double cw;
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 11 * s);
            cw = fmax(PW, 760 * s);
            cairo_rectangle(cr, F->mw - 24 * s - cw - 4 * s, py + PH, cw + 8 * s, 58 * s);
            cairo_set_source_rgba(cr, 0.04, 0.04, 0.055, 0.92);
            cairo_fill(cr);
            cap_x = F->mw - 24 * s - cw;
        }
        cairo_rectangle(cr, px - 4 * s, py - 4 * s, PW + 8 * s, PH + 8 * s);
        cairo_set_source_rgba(cr, 0.04, 0.04, 0.055, 0.92);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 1, 0.7, 0.24, 1);
        cairo_set_line_width(cr, 3 * s);
        cairo_stroke(cr);
        cairo_save(cr);
        cairo_translate(cr, px, py);
        cairo_scale(cr, k2, k2);
        cairo_set_source_surface(cr, r->photo, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
        /* A on the picture (the image may be a reduced copy of the frame) */
        if (fp.sky && r->geom) {
            double pk = iw / r->sub_w, x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
            int any = 0;
            for (k = 0; k <= FP_COLS; k++) {
                if (!fp.band_ok[k] || fp.band_px[k] < 0 || fp.band_px[k] > r->sub_w) continue;
                any = 1;
                x0 = fmin(x0, fp.band_px[k]); x1 = fmax(x1, fp.band_px[k]);
                y0 = fmin(y0, fp.band_top[k]); y1 = fmax(y1, fp.band_bot[k]);
            }
            if (any) {
                y0 = fmax(y0, 0); y1 = fmin(y1, r->sub_h);
                cairo_rectangle(cr, px + x0 * pk * k2 + 2 * s, py + y0 * pk * k2 - 6 * s,
                                (x1 - x0) * pk * k2 - 4 * s, (y1 - y0) * pk * k2 + 12 * s);
                cairo_set_source_rgba(cr, A_R / 255.0, A_G / 255.0, A_B / 255.0, 1);
                cairo_set_line_width(cr, 2.6 * s);
                cairo_stroke(cr);
                cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
                cairo_set_font_size(cr, 15 * s);
                text_outline(cr, px + x0 * pk * k2 + 6 * s, py + y1 * pk * k2 + 22 * s, "A",
                             A_R / 255.0, A_G / 255.0, A_B / 255.0, 1);
            }
        }
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 12 * s);
        text_outline(cr, px + 10 * s, py + 18 * s, "LATEST PICTURE", 1, 1, 1, 1);
        if (fp.ok) {
            char tilt[48];
            if (fabs(fp.el_c) < 3) snprintf(tilt, sizeof(tilt), "aimed level");
            else snprintf(tilt, sizeof(tilt), "aimed %.0f\xc2\xb0 %s level", fabs(fp.el_c), fp.el_c > 0 ? "above" : "below");
            snprintf(c1, sizeof(c1), "Looking %s (%.0f\xc2\xb0), %s \xc2\xb7 %.0f\xc2\xb0 wide%s",
                     compass16(fp.az_c), fp.az_c, tilt, fp.fov,
                     fp.n ? (fp.sky ? " \xc2\xb7 box A = the same skyline on the map" :
                                      " \xc2\xb7 box A on the map = this patch of ground") : "");
        } else {
            snprintf(c1, sizeof(c1), "Pointing not published for this picture");
        }
        snprintf(c2, sizeof(c2), "%s \xc2\xb7 sol %d \xc2\xb7 taken %s UTC \xc2\xb7 raw, unprocessed \xc2\xb7 %s",
                 r->photo_cam, r->photo_sol, r->photo_date, cam_credit(r->photo_cam));
        cairo_set_font_size(cr, 13 * s);
        text_outline(cr, cap_x + 4 * s, py + PH + 24 * s, c1, 1, 0.78, 0.36, 1);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11 * s);
        text_outline(cr, cap_x + 4 * s, py + PH + 44 * s, c2, 0.86, 0.88, 0.92, 1);
    }

    /* where on Mars */
    {
        cairo_surface_t *g = map_get(PIC_WORLD_MARS, 1920, 960);
        double LW = 300 * s, LH = 150 * s, lx = F->mw - LW - 24 * s, ly = F->mh - LH - 60 * s, x, y;
        if (g) {
            cairo_save(cr);
            cairo_translate(cr, lx, ly);
            cairo_scale(cr, LW / cairo_image_surface_get_width(g), LH / cairo_image_surface_get_height(g));
            cairo_set_source_surface(cr, g, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
            cairo_rectangle(cr, lx, ly, LW, LH);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.5);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
            x = lx + (r->lon + 180) / 360 * LW;
            y = ly + (90 - r->lat) / 180 * LH;
            cairo_rectangle(cr, x - 4 * s, y - 4 * s, 8 * s, 8 * s);
            cairo_set_source_rgba(cr, 1, 0.8, 0.3, 1);
            cairo_set_line_width(cr, 2 * s);
            cairo_stroke(cr);
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 10 * s);
            snprintf(line, sizeof(line), "%s, %.1f\xc2\xb0%c %.1f\xc2\xb0%c", r == &ROVERS[0] ? "Gale crater" : "near Jezero crater",
                     fabs(r->lat), r->lat < 0 ? 'S' : 'N', fabs(r->lon), r->lon < 0 ? 'W' : 'E');
            text_outline(cr, lx + 6 * s, ly + 14 * s, line, 1, 1, 1, 1);
        }
    }

    /* key and credits */
    {
        double y = F->mh - 30 * s, x = 350 * s;
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 10 * s);
        cairo_move_to(cr, x, y - 3 * s); cairo_line_to(cr, x + 22 * s, y - 3 * s);
        cairo_set_source_rgba(cr, 1.0, 0.8, 0.24, 1); cairo_set_line_width(cr, 3 * s); cairo_stroke(cr);
        text_outline(cr, x + 28 * s, y, "route of the last 12 drives", 0.9, 0.92, 0.95, 1);
        x += 180 * s;
        cairo_move_to(cr, x, y - 3 * s); cairo_line_to(cr, x + 18 * s, y - 9 * s); cairo_line_to(cr, x + 18 * s, y + 3 * s);
        cairo_close_path(cr); cairo_set_source_rgba(cr, 1, 0.69, 0.2, 0.7); cairo_fill(cr);
        text_outline(cr, x + 24 * s, y, "camera direction and width (from its pointing data)", 0.9, 0.92, 0.95, 1);
        x += 290 * s;
        if (fp.n) {
            cairo_rectangle(cr, x, y - 8 * s, 16 * s, 10 * s);
            cairo_set_source_rgba(cr, 1, 1, 1, 1); cairo_set_line_width(cr, 2 * s); cairo_stroke(cr);
            text_outline(cr, x + 22 * s, y, "ground in the picture, traced over the elevation model", 0.9, 0.92, 0.95, 1);
        }
        cairo_set_font_size(cr, 9 * s);
        snprintf(line, sizeof(line), "Map: NASA/JPL-Caltech/UArizona HiRISE via MMGIS \xc2\xb7 %s \xc2\xb7 Picture, position & route: NASA/JPL-Caltech",
                 r == &ROVERS[0] ? "Elevation: USGS/JPL MSL Gale merged DEM 1 m" : "Elevation: USGS/JPL Mars 2020 Jezero CTX DEM 20 m");
        text_outline(cr, 350 * s, y + 15 * s, line, 0.72, 0.74, 0.78, 1);
    }
    cairo_restore(cr);

    snprintf(title, sizeof(title), "MARS \xc2\xb7 %s", r == &ROVERS[0] ? "CURIOSITY" : "PERSEVERANCE");
    snprintf(line, sizeof(line), "%s \xc2\xb7 sol %d \xc2\xb7 map: HiRISE colour, 25 cm per pixel",
             r == &ROVERS[0] ? "Gale crater, on Mount Sharp" : "near Jezero crater",
             mission_sol(r, t));
    draw_header(cr, F, title, line);
    return 1;
}

/* ── 11. The view ──────────────────────────────────────────────── */

static const char *mars_season(double ls)
{
    if (ls < 90) return "northern spring";
    if (ls < 180) return "northern summer";
    if (ls < 270) return "northern autumn";
    return "northern winter";
}

void pic_planetview_render(cairo_t *cr, int width, int height, int w, time_t now)
{
    frame_t *F = calloc(1, sizeof(*F));
    double t = (double)now, slon = 0, slat = 0, elon = 0, elat = 0, ephase = 0, grs_lon = 0;
    int have_sun, have_earth = 0, have_pj = 0, i;
    double dust_mean = -1, t_pj = 0;
    pic_subpt_t pj;
    char title[64], line[256];
    struct timespec ts;
    double now_f;

    if (!F || w <= 0 || w >= PIC_WORLD_N) { free(F); return; }
    pthread_once(&cache_once, load_all_cache);
    clock_gettime(CLOCK_REALTIME, &ts);
    now_f = ts.tv_sec + ts.tv_nsec / 1e9;
    if (fabs(now_f - t) > 5) now_f = t;      /* tests pass a fixed time */
    if (getenv("PIC_PV_TF")) now_f = atof(getenv("PIC_PV_TF"));
    F->W = width; F->H = height;
    F->s = height / 1080.0;
    F->mw = width;
    F->mh = width / 2.0;
    if (F->mh > height - 110 * F->s) { F->mh = height - 110 * F->s; F->mw = 2 * F->mh; }
    F->mx = (width - F->mw) / 2;
    F->my = (height - F->mh) / 2;
    F->mx = floor(F->mx); F->my = floor(F->my);
    F->mw = floor(F->mw); F->mh = floor(F->mh);

    cairo_save(cr);
    cairo_set_source_rgb(cr, 0.02, 0.02, 0.03);
    cairo_paint(cr);

    pthread_mutex_lock(&data_lock);
    /* Mars: the whole planet, then each rover close up, 40 s each */
    if (w == PIC_WORLD_MARS && zoom_on) {
        int phase = (int)(((long long)t / 40) % 3);
        if (getenv("PIC_PV_ZOOM")) phase = atoi(getenv("PIC_PV_ZOOM"));
        if (phase > 0 && render_zoom(cr, F, phase == 1 ? &ROVERS[1] : &ROVERS[0], t)) {
            pthread_mutex_unlock(&data_lock);
            cairo_restore(cr);
            free(F);
            return;
        }
    }
    have_sun = sun_at(w, t, &slon, &slat);
    if (w == PIC_WORLD_MOON) {
        body_t *e = body_by_key("moon_earth");
        pic_subpt_t p;
        if (e && e->obs && series_at(e->obs, e->n, t, &p)) {
            double S[3], E[3];
            elon = p.lon; elat = p.lat; have_earth = 1;
            unitv(slon, slat, S);
            unitv(elon, elat, E);
            /* Earth's lit fraction seen from the Moon = 1 - Moon's */
            ephase = 1.0 - (1.0 + S[0] * E[0] + S[1] * E[1] + S[2] * E[2]) / 2.0;
        }
    }
    if (!have_sun && w == PIC_WORLD_MARS) {
        /* no ephemeris yet: mean sun from Mars time (within ~2 deg) */
        slon = wrap180(-(pic_mars_mtc(t) - 12.0) * 15.0);
        slat = asin(sin(25.19 * D2R) * sin(pic_mars_ls(t) * D2R)) / D2R;
        have_sun = 1;
    }

    draw_map(cr, F, w, now, &grs_lon);
    if (w == PIC_WORLD_MARS && dust_on) dust_mean = draw_dust(cr, F, pic_mars_ls(t));
    if (have_sun) draw_night(cr, F, w, slon, slat, elon, elat, have_earth ? ephase : 0);
    draw_grid(cr, F, w);

    /* header */
    snprintf(title, sizeof(title), "%s", w == PIC_WORLD_MOON ? "THE MOON" :
             w == PIC_WORLD_MARS ? "MARS" : "JUPITER");
    line[0] = 0;
    if (w == PIC_WORLD_MOON) {
        if (have_earth)
            snprintf(line, sizeof(line), "Moon %.0f%% lit from Earth \xc2\xb7 Earth overhead at "
                     "%.1f\xc2\xb0%c %.1f\xc2\xb0%c \xc2\xb7 Sun overhead at %.1f\xc2\xb0%c %.1f\xc2\xb0%c",
                     (1 - ephase) * 100, fabs(elat), elat < 0 ? 'S' : 'N', fabs(elon), elon < 0 ? 'W' : 'E',
                     fabs(slat), slat < 0 ? 'S' : 'N', fabs(slon), slon < 0 ? 'W' : 'E');
        else snprintf(line, sizeof(line), "waiting for JPL Horizons data");
    } else if (w == PIC_WORLD_MARS) {
        double ls = pic_mars_ls(t), mtc = pic_mars_mtc(t);
        /* Large dust storms come mostly in southern spring and summer */
        snprintf(line, sizeof(line), "Ls %.0f\xc2\xb0, %s \xc2\xb7 %s \xc2\xb7 "
                 "Mars time (MTC) %02d:%02d \xc2\xb7 Sun overhead at %.1f\xc2\xb0%c %.1f\xc2\xb0%c",
                 ls, mars_season(ls),
                 ls >= 180 ? "dust-storm season (Ls 180\xe2\x80\x93" "360\xc2\xb0)" :
                             "quiet dust season (storms mostly Ls 180\xe2\x80\x93" "360\xc2\xb0)",
                 (int)mtc, (int)(fmod(mtc, 1) * 60),
                 fabs(slat), slat < 0 ? 'S' : 'N', fabs(slon), slon < 0 ? 'W' : 'E');
    } else {
        int est;
        double l2 = grs_l2(now, &est);
        char pjs[96] = "";
        have_pj = juno_next_pj(t, &t_pj, &pj);
        if (have_pj) {
            time_t tp = (time_t)t_pj;
            char when[32];
            double dt = t_pj - t;
            strftime(when, sizeof(when), "%d %b %H:%M UTC", gmtime(&tp));
            if (dt > 86400)
                snprintf(pjs, sizeof(pjs), " \xc2\xb7 Juno's next close pass in %d d %d h (%s)",
                         (int)(dt / 86400), (int)fmod(dt / 3600, 24), when);
            else if (dt > 0)
                snprintf(pjs, sizeof(pjs), " \xc2\xb7 Juno's close pass in %d h %02d min (%s)",
                         (int)(dt / 3600), (int)fmod(dt / 60, 60), when);
            else
                snprintf(pjs, sizeof(pjs), " \xc2\xb7 Juno's close pass %d min ago", (int)(-dt / 60));
        }
        snprintf(line, sizeof(line), "Great Red Spot at System II %.0f\xc2\xb0%s (III %.0f\xc2\xb0W)%s "
                 "\xc2\xb7 Sun overhead at %.1f\xc2\xb0%c %.1f\xc2\xb0%c",
                 l2, est ? " est." : "", fmod(-grs_lon + 360, 360), pjs,
                 fabs(slat), slat < 0 ? 'S' : 'N', fabs(slon), slon < 0 ? 'W' : 'E');
    }
    /* one-way radio delay from Earth, from JPL's geocentric range */
    {
        body_t *e = body_by_key(w == PIC_WORLD_MOON ? "moon_earth" :
                                w == PIC_WORLD_MARS ? "mars_earth" : "jup_earth");
        pic_subpt_t p;
        if (e && e->obs && series_at(e->obs, e->n, t, &p) && p.r > 0) {
            double lt = p.r / 299792.458;
            size_t n = strlen(line);
            if (lt < 60)
                snprintf(line + n, sizeof(line) - n, " \xc2\xb7 radio delay %.2f s", lt);
            else
                snprintf(line + n, sizeof(line) - n, " \xc2\xb7 radio delay %d min %02d s",
                         (int)(lt / 60), (int)fmod(lt, 60));
        }
    }
    draw_header(cr, F, title, line);

    draw_key(cr, F, w, WORLDS[w].credit, dust_mean >= 0, have_pj);

    if (have_sun) draw_sun_mark(cr, F, slon, slat);
    if (have_earth) {
        double x, y, r = 7 * F->s;
        cairo_pattern_t *g;
        map_xy(F, elon, elat, &x, &y);
        g = cairo_pattern_create_radial(x - r * 0.3, y - r * 0.3, r * 0.1, x, y, r);
        cairo_pattern_add_color_stop_rgb(g, 0, 0.6, 0.8, 1);
        cairo_pattern_add_color_stop_rgb(g, 1, 0.1, 0.3, 0.75);
        cairo_arc(cr, x, y, r, 0, 2 * M_PI);
        cairo_set_source(cr, g);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(g);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.8);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 10.5 * F->s);
        text_outline(cr, x + r + 4 * F->s, y + 4 * F->s, "Earth overhead", 0.7, 0.85, 1, 1);
        box_add(F, x - r, y - r, x + r + 95 * F->s, y + r);
    }
    if (w == PIC_WORLD_JUPITER) {
        double x, y;
        map_xy(F, grs_lon, GRS_LAT, &x, &y);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, 11 * F->s);
        text_outline(cr, x - text_w(cr, "Great Red Spot") / 2, y + 30 * F->s, "Great Red Spot",
                     1, 0.88, 0.8, 1);
        box_add(F, x - 50 * F->s, y - 20 * F->s, x + 50 * F->s, y + 34 * F->s);
    }

    /* moons and orbiters (they move: placed first, callouts avoid them) */
    draw_moons(cr, F, w, t, slon, slat, have_sun);
    draw_orbiters(cr, F, w, t, 1);
    if (have_pj) draw_pj(cr, F, t_pj, &pj);

    /* Mars rover insets (when the close-up views are off): over the
     * orbit tracks, under the callouts (which keep clear of them) */
    if (w == PIC_WORLD_MARS && !zoom_on) {
        double s = F->s, by = F->my + F->mh - (INSET_H + 50 + 12) * s;
        double bx[2];
        if (&pic_ticker_bar_top && pic_ticker_bar_top > 0 && pic_ticker_bar_top < F->my + F->mh)
            by -= F->my + F->mh - pic_ticker_bar_top;
        /* bottom left, over the plains of the far west (Amazonis,
         * Tharsis' southern flank) rather than Elysium and Hellas */
        bx[0] = F->mx + 14 * s;                                /* Perseverance, 77E */
        bx[1] = bx[0] + (INSET_W + 22) * s;                    /* Curiosity, 137E */
        for (i = 0; i < NROVERS; i++) {
            rover_t *r = &ROVERS[i];
            if (!r->have) continue;
            /* Perseverance (index 1) sits left of Curiosity (index 0) */
            draw_inset(cr, F, r, bx[i == 0 ? 1 : 0], by, now_f);
        }
    }

    /* orbiters fly over everything on the surface, insets included */
    draw_orbiters(cr, F, w, t, 0);


    draw_labels(cr, F, w, 1);

    /* surface sites: live rovers first, then by list order */
    if (w == PIC_WORLD_MARS) {
        for (i = 0; i < NROVERS; i++) {
            rover_t *r = &ROVERS[i];
            double x, y;
            char l2[48];
            if (!r->have) continue;
            map_xy(F, r->lon, r->lat, &x, &y);
            snprintf(l2, sizeof(l2), "sol %d \xc2\xb7 %s", mission_sol(r, t),
                     dsn_link(r->naif) ? "talking to Earth now" : "operating");
            draw_callout(cr, F, x, y, r->sprite, r->kind, -1, r->name, l2, ST_ACTIVE, 40 * F->s);
        }
        if (heli.have) {
            double x, y;
            map_xy(F, heli.lon, heli.lat, &x, &y);
            draw_callout(cr, F, x, y, "ingenuity", PIC_CRAFT_INGENUITY, -1, "Ingenuity",
                         "72 flights \xc2\xb7 2021\xe2\x80\x93" "24", ST_ENDED, 30 * F->s);
        }
    }
    for (i = 0; i < NSITES; i++) {
        const site_t *st = &SITES[i];
        double x, y;
        if (st->world != w) continue;
        map_xy(F, st->lon, st->lat, &x, &y);
        draw_callout(cr, F, x, y, st->sprite, st->kind, st->kind2, st->name, st->when,
                     st->status, (st->status == ST_CRASH ? 22 : 32) * F->s);
    }
    draw_labels(cr, F, w, 2);
    pthread_mutex_unlock(&data_lock);
    cairo_restore(cr);
    free(F);
}

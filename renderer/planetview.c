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
        "Map: NASA/GSFC/Arizona State Univ. (LRO LROC WAC colour, SVS CGI Moon Kit) \xc2\xb7 "
        "Sites: LROC / NSSDCA \xc2\xb7 Orbiters: JPL Horizons" },
    [PIC_WORLD_MARS] = { "mars", "Mars", 3396.19, 3376.20, 3389.5, 1,
        "Map: NASA/USGS Viking MDIM colour mosaic \xc2\xb7 Rovers: NASA/JPL-Caltech MMGIS, "
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
    { PIC_WORLD_MOON, "Luna 13", "1966", 18.87, -62.05, PIC_CRAFT_LUNA_ALS, NOK, NULL, ST_ENDED, 1 },
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
    { PIC_WORLD_MOON, "Chang'e 5", "2020 \xc2\xb7 sample return", 43.0576, -51.9161, PIC_CRAFT_CHANGE_LANDER, NOK, NULL, ST_ENDED, 0 },
    { PIC_WORLD_MOON, "Chang'e 6", "2024 \xc2\xb7 far-side samples", -41.6385, -153.9852, PIC_CRAFT_CHANGE_LANDER, NOK, NULL, ST_ENDED, 0 },
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

enum { B_SUNREF, B_ORBITER, B_MOON };

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
    for (i = 0; i < NBODIES; i++)      /* any other observer of w */
        if (BODIES[i].world == w && BODIES[i].sun &&
            series_at(BODIES[i].sun, BODIES[i].n, t, &p)) {
            *lon = p.lon; *lat = p.lat;
            return 1;
        }
    return 0;
}

/* ── 4. Mars rovers (JPL MMGIS feeds) ──────────────────────────── */

typedef struct {
    const char *key, *name, *mission, *sprite;
    int kind;
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
    int inset_z, inset_tx0, inset_ty0;
    time_t checked;
} rover_t;

static rover_t ROVERS[] = {
    { .key = "MSL", .name = "Curiosity", .mission = "MSL", .sprite = "curiosity",
      .kind = PIC_CRAFT_MSL, .land_t = 1344230277.0, .land_lon = 137.4417 },
    { .key = "M20", .name = "Perseverance", .mission = "M20", .sprite = "perseverance",
      .kind = PIC_CRAFT_MSL, .land_t = 1613681700.0, .land_lon = 77.4509 },
};
#define NROVERS 2

static const char *ROVER_TILES[NROVERS] = {
    MMGIS "MSL/Layers/MSL_Gale_HiRISE-LRGB_78quads",
    MMGIS "M20/Layers/M20_HiRISE_RGB_CTX_mosaic_25cm_v7_Merge_LERC_clip",
};

static struct { int have; double lat, lon, total_km; int flight; } heli;

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

/* ── 5. Fetch thread ───────────────────────────────────────────── */

static unsigned want_mask;        /* bit w: world w is in the cycle     */
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
    want_mask = mask;
    grs_l2_set = pic_conf_num("JUPITER_GRS_L2", -1);
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

static pthread_once_t cache_once = PTHREAD_ONCE_INIT;

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
    }
    snprintf(path, sizeof(path), "%s/heli_current.json", dir_cache());
    if ((txt = slurp(path, NULL)) != NULL) { heli_parse(txt); free(txt); }
    for (i = 0; i < NROVERS; i++) rover_tiles_check(&ROVERS[i]);
}

static int fetch_body(body_t *b, time_t now)
{
    char url[1024], t0[32], t1[32], path[256];
    char *buf;
    time_t a = now - HZ_BEFORE, z = now + HZ_AFTER;
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

    for (i = 0; i < NBODIES && !did; i++) {
        body_t *b = &BODIES[i];
        double t1;
        if (!(want_mask & (1u << b->world))) continue;
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
    return did ? 3 : 120;
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

/* ── 7. The map, day and night ─────────────────────────────────── */

static cairo_surface_t *map_surf;
static int map_world, map_w, map_h;

static cairo_surface_t *map_get(int w, int mw, int mh)
{
    char path[256];
    static time_t failed_at;
    if (map_world == w && map_w == mw && map_h == mh &&
        (map_surf || time(NULL) - failed_at < 300)) return map_surf;
    if (map_surf) cairo_surface_destroy(map_surf);
    map_surf = NULL;
    snprintf(path, sizeof(path), "%s/%s_%d.jpg", dir_worlds(), WORLDS[w].key, mw > 1920 ? 3840 : 1920);
    map_surf = pic_image_load_scaled(path, mw, mh);
    map_world = w; map_w = mw; map_h = mh;
    if (!map_surf) { printf("worlds: no map %s\n", path); failed_at = time(NULL); }
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
                lit = clampd((el + 0.3) / 1.2, 0, 1);
                nb = 0.14 + 0.30 * earth_phase * (ce > 0 ? ce : 0);
            } else if (w == PIC_WORLD_MARS) {
                lit = clampd((el + 7.0) / 10.0, 0, 1);
                lit = lit * lit * (3 - 2 * lit);
                nb = 0.16;
            } else {
                lit = clampd((el + 3.0) / 6.0, 0, 1);
                lit = lit * lit * (3 - 2 * lit);
                nb = 0.16;
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
        fmt_alt(alt, sizeof(alt), p.r - WORLDS[w].mean);
        snprintf(l1, sizeof(l1), "%s", b->name);
        {
            double fs = 11.5 * F->s, tw, tw2;
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(cr, fs);
            tw = text_w(cr, l1);
            draw_thing(cr, b->sprite, b->kind, x, y, isz, 0);
            text_outline(cr, x + isz * 0.6, y - 2 * F->s, l1, 0.6, 0.93, 1.0, 1);
            cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 9.5 * F->s);
            tw2 = text_w(cr, alt);
            text_outline(cr, x + isz * 0.6, y + 10 * F->s, alt, 0.85, 0.9, 0.95, 0.95);
            box_add(F, x - isz / 2, y - isz / 2, x + isz * 0.6 + fmax(tw, tw2) + 2, y + isz / 2);
        }
    }
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
            cairo_save(cr);
            cairo_translate(cr, sx, sy);
            cairo_scale(cr, 1.0 / fmax(cos(olat * D2R), 0.2), 1);
            cairo_translate(cr, -sx, -sy);
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
        fmt_alt(l2, sizeof(l2), p.r - WORLDS[w].mean);
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
    double tlh = 38 * s, mpp;
    int i;
    char t1[96], t2[128];

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

    cairo_save(cr);
    cairo_rectangle(cr, bx, by, W, H);
    cairo_clip(cr);
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
    cairo_restore(cr);

    /* sprite + captions under the picture */
    draw_thing(cr, r->sprite, r->kind, bx + 26 * s, by + H + tlh / 2, 46 * s, 0);
    snprintf(t1, sizeof(t1), "%s \xc2\xb7 sol %d \xc2\xb7 %.2f km driven", r->name,
             mission_sol(r, now_f), r->dist_total / 1000.0);
    if (r->last_sol >= 0)
        snprintf(t2, sizeof(t2), "Position: end of sol %d drive (%.0f m) \xc2\xb7 %.4f\xc2\xb0%c %.4f\xc2\xb0" "E",
                 r->last_sol > r->sol ? r->last_sol : r->sol, r->last_len,
                 fabs(r->lat), r->lat < 0 ? 'S' : 'N', r->lon);
    else
        snprintf(t2, sizeof(t2), "Position: sol %d localization (%.0f m drive) \xc2\xb7 %.4f\xc2\xb0%c %.4f\xc2\xb0" "E",
                 r->sol, r->last_len, fabs(r->lat), r->lat < 0 ? 'S' : 'N', r->lon);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 12.5 * s);
    text_outline(cr, bx + 56 * s, by + H + 17 * s, t1, 0.55, 1, 0.62, 1);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 9.5 * s);
    text_outline(cr, bx + 56 * s, by + H + 32 * s, t2, 0.85, 0.87, 0.9, 0.95);
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

static void draw_key(cairo_t *cr, frame_t *F, int w, const char *credit)
{
    static const struct { const char *t; int st; } rows[] = {
        { "operating", ST_ACTIVE }, { "mission ended", ST_ENDED },
        { "status unconfirmed", ST_UNKNOWN }, { "crash / impact", ST_CRASH } };
    double s = F->s, x = F->mx + 12 * s, y, fs = 10 * s, cx;
    double bottom = F->my + F->mh;
    int i;
    if (&pic_ticker_bar_top && pic_ticker_bar_top > 0 && pic_ticker_bar_top < bottom) bottom = pic_ticker_bar_top;
    y = bottom - 30 * s;
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
    }
    cairo_set_font_size(cr, 9 * s);
    text_outline(cr, x, y + 15 * s, credit, 0.75, 0.77, 0.8, 0.9);
    box_add(F, x - 4, y - 14 * s, x + 900 * s, y + 18 * s);
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
    int have_sun, have_earth = 0, i;
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
    if (have_sun) draw_night(cr, F, w, slon, slat, elon, elat, have_earth ? ephase : 0);
    draw_grid(cr, F, w);

    /* header */
    snprintf(title, sizeof(title), "%s", w == PIC_WORLD_MOON ? "THE MOON" :
             w == PIC_WORLD_MARS ? "MARS" : "JUPITER");
    line[0] = 0;
    if (w == PIC_WORLD_MOON) {
        if (have_earth)
            snprintf(line, sizeof(line), "Moon %.0f%% lit from Earth \xc2\xb7 Earth overhead at "
                     "%.1f\xc2\xb0%c %.1f\xc2\xb0%c (libration) \xc2\xb7 Sun overhead at %.1f\xc2\xb0%c %.1f\xc2\xb0%c",
                     (1 - ephase) * 100, fabs(elat), elat < 0 ? 'S' : 'N', fabs(elon), elon < 0 ? 'W' : 'E',
                     fabs(slat), slat < 0 ? 'S' : 'N', fabs(slon), slon < 0 ? 'W' : 'E');
        else snprintf(line, sizeof(line), "waiting for JPL Horizons data");
    } else if (w == PIC_WORLD_MARS) {
        double ls = pic_mars_ls(t), mtc = pic_mars_mtc(t);
        snprintf(line, sizeof(line), "Ls %.0f\xc2\xb0 (%s) \xc2\xb7 Mars Sol Date %.0f \xc2\xb7 "
                 "Coordinated Mars Time %02d:%02d \xc2\xb7 Sun overhead at %.1f\xc2\xb0%c %.1f\xc2\xb0%c",
                 ls, mars_season(ls), floor(pic_mars_msd(t)), (int)mtc, (int)(fmod(mtc, 1) * 60),
                 fabs(slat), slat < 0 ? 'S' : 'N', fabs(slon), slon < 0 ? 'W' : 'E');
    } else {
        int est;
        double l2 = grs_l2(now, &est);
        snprintf(line, sizeof(line), "Great Red Spot at System II %.0f\xc2\xb0%s, System III %.0f\xc2\xb0W "
                 "\xc2\xb7 bands from Cassini, Dec 2000 \xc2\xb7 Sun overhead at %.1f\xc2\xb0%c %.1f\xc2\xb0W",
                 l2, est ? " (estimate)" : "", fmod(-grs_lon + 360, 360),
                 fabs(slat), slat < 0 ? 'S' : 'N', fmod(-slon + 360, 360));
    }
    draw_header(cr, F, title, line);

    draw_key(cr, F, w, WORLDS[w].credit);

    /* orbiters fly over everything on the surface, insets included */
    draw_orbiters(cr, F, w, t, 0);

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

    /* Mars rover insets: over the orbit tracks, under the callouts
     * (which keep clear of them) */
    if (w == PIC_WORLD_MARS) {
        double s = F->s, by = F->my + F->mh - (INSET_H + 38 + 12) * s;
        double bx[2];
        if (&pic_ticker_bar_top && pic_ticker_bar_top > 0 && pic_ticker_bar_top < F->my + F->mh)
            by -= F->my + F->mh - pic_ticker_bar_top;
        bx[1] = F->mx + F->mw - (INSET_W + 14) * s;            /* Curiosity, 137E */
        bx[0] = bx[1] - (INSET_W + 22) * s;                    /* Perseverance, 77E */
        for (i = 0; i < NROVERS; i++) {
            rover_t *r = &ROVERS[i];
            if (!r->have) continue;
            /* Perseverance (index 1) sits left of Curiosity (index 0) */
            draw_inset(cr, F, r, bx[i == 0 ? 1 : 0], by, now_f);
        }
    }


    /* surface sites: live rovers first, then by list order */
    if (w == PIC_WORLD_MARS) {
        for (i = 0; i < NROVERS; i++) {
            rover_t *r = &ROVERS[i];
            double x, y;
            char l2[48];
            if (!r->have) continue;
            map_xy(F, r->lon, r->lat, &x, &y);
            snprintf(l2, sizeof(l2), "sol %d \xc2\xb7 operating", mission_sol(r, t));
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
    pthread_mutex_unlock(&data_lock);
    cairo_restore(cr);
    free(F);
}

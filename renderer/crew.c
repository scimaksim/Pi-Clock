/*
 * crew.c - Who is in space: people aboard the ISS and Tiangong
 *
 * Launch Library 2's space-station list carries the live crew count
 * of each active station (updated by the same volunteers who keep the
 * launch schedule; it changes within hours of a docking or landing).
 * Fetched every 6 hours (and on start); the counts feed the little
 * person-and-number badge drawn beside the station icons on the map
 * and in the Visible Passes panel.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "features_all.h"
#include "fetch.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CREW_URL "https://ll.thespacedevs.com/2.3.0/space_stations/?status__ids=1&mode=detailed&limit=10"
#define CREW_MAX_RESPONSE (256 * 1024)

static pthread_mutex_t crew_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { char name[16]; int aboard; } crew[6];
static int ncrew;
static time_t crew_at;

/* Map a station's catalogue name to the short name used on the map */
static const char *short_station(const char *name)
{
    if (strstr(name, "International")) return "ISS";
    if (strstr(name, "Tiangong")) return "Tiangong";
    return NULL;
}

int pic_crew_parse(const char *js, size_t len)
{
    pic_json_t j;
    struct { char name[16]; int aboard; } tmp[6];
    int res, n, i, k = 0;
    if (pic_json_parse(&j, js, len) < 0) return -1;
    res = pic_json_get(&j, 0, "results");
    n = res >= 0 ? pic_json_size(&j, res) : 0;
    for (i = 0; i < n && k < 6; i++) {
        int r = pic_json_at(&j, res, i);
        char name[96];
        const char *s;
        double c;
        pic_json_str(&j, pic_json_get(&j, r, "name"), name, sizeof(name));
        c = pic_json_num(&j, pic_json_get(&j, r, "onboard_crew"), -1);
        s = short_station(name);
        if (!s || c < 0) continue;
        snprintf(tmp[k].name, sizeof(tmp[k].name), "%s", s);
        tmp[k].aboard = (int)c;
        k++;
    }
    pic_json_free(&j);
    if (k == 0) return -1;                 /* keep what we had */
    pthread_mutex_lock(&crew_mu);
    memcpy(crew, tmp, sizeof(tmp[0]) * (size_t)k);
    ncrew = k;
    pthread_mutex_unlock(&crew_mu);
    return k;
}

int pic_crew_aboard(const char *station)
{
    int i, n = -1;
    pthread_mutex_lock(&crew_mu);
    for (i = 0; i < ncrew; i++)
        if (!strcasecmp(crew[i].name, station)) n = crew[i].aboard;
    pthread_mutex_unlock(&crew_mu);
    return n;
}

static int crew_work(pic_feature_t *f)
{
    char *buf = malloc(CREW_MAX_RESPONSE);
    int n;
    (void)f;
    if (!buf) return 3600;
    n = pic_fetch_to_buf(CREW_URL, buf, CREW_MAX_RESPONSE - 1);
    if (n > 0) {
        int k = pic_crew_parse(buf, (size_t)n);
        if (k > 0) {
            crew_at = time(NULL);
            printf("crew: ISS %d aboard, Tiangong %d aboard\n",
                   pic_crew_aboard("ISS"), pic_crew_aboard("Tiangong"));
        } else {
            printf("crew: no station data in the reply\n");
        }
    } else {
        printf("crew: fetch failed\n");
    }
    free(buf);
    return n > 0 ? 6 * 3600 : 30 * 60;
}

static int crew_wanted(pic_feature_t *f) { (void)f; return 1; }

static pic_feature_t feature_def = {
    .name = "crew", .work = crew_work, .wanted = crew_wanted, .needs_network = 1,
};

pic_feature_t *pic_feature_crew(void)
{
    return &feature_def;
}

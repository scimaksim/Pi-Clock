/*
 * airquality.c - Open-Meteo US AQI grid fetcher
 *
 * The QTH plus the AQ_NX x AQ_NY grid are requested in chunks of up to
 * 100 locations (comma-separated latitude/longitude lists; the API then
 * returns a JSON list in the same order). Open-Meteo counts each
 * location as one call: ~188 locations every 90 minutes is ~3,000 calls
 * a day, well inside the free 10,000/day.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "airquality.h"
#include "datastatus.h"
#include "config.h"
#include "fetch.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define AQ_URL "https://air-quality-api.open-meteo.com/v1/air-quality"
#define FETCH_INTERVAL (90 * 60)
#define RETRY_INTERVAL (10 * 60)
#define CHUNK 100
#define MAX_RESPONSE (512 * 1024)

static pthread_t aq_thread;
static volatile int aq_running = 0;
static pic_airquality_t *cfg_data = NULL;

void pic_aq_init(pic_airquality_t *data)
{
    memset(data, 0, sizeof(*data));
    data->qth_aqi = -1;
    data->show_badge = 1;
    pthread_mutex_init(&data->mutex, NULL);
}

/* EPA AQI colours */
void pic_aq_colour(int aqi, double *r, double *g, double *b)
{
    if      (aqi <= 50)  { *r = 0.00; *g = 0.89; *b = 0.00; }  /* Good       */
    else if (aqi <= 100) { *r = 1.00; *g = 1.00; *b = 0.00; }  /* Moderate   */
    else if (aqi <= 150) { *r = 1.00; *g = 0.49; *b = 0.00; }  /* USG        */
    else if (aqi <= 200) { *r = 1.00; *g = 0.00; *b = 0.00; }  /* Unhealthy  */
    else if (aqi <= 300) { *r = 0.56; *g = 0.25; *b = 0.59; }  /* Very unh.  */
    else                 { *r = 0.49; *g = 0.00; *b = 0.14; }  /* Hazardous  */
}

const char *pic_aq_category(int aqi)
{
    if (aqi <= 50)  return "Good";
    if (aqi <= 100) return "Moderate";
    if (aqi <= 150) return "Unhealthy (sensitive)";
    if (aqi <= 200) return "Unhealthy";
    if (aqi <= 300) return "Very Unhealthy";
    return "Hazardous";
}

int pic_aq_parse(const char *js, size_t len, float *vals, int max,
                 float *pm25_first, float *ozone_first)
{
    pic_json_t j;
    int n, i;

    if (pic_json_parse(&j, js, len) != 0) return 0;

    /* One location → object; several → list of objects */
    if (pic_json_is_array(&j, 0)) {
        n = pic_json_size(&j, 0);
    } else if (pic_json_is_object(&j, 0)) {
        n = 1;
    } else {
        pic_json_free(&j);
        return 0;
    }
    if (n > max) n = max;

    for (i = 0; i < n; i++) {
        int o = pic_json_is_array(&j, 0) ? pic_json_at(&j, 0, i) : 0;
        int cur = pic_json_get(&j, o, "current");
        vals[i] = (float)pic_json_num(&j, pic_json_get(&j, cur, "us_aqi"), -1);
        if (i == 0) {
            if (pm25_first)
                *pm25_first = (float)pic_json_num(&j, pic_json_get(&j, cur, "pm2_5"), -1);
            if (ozone_first)
                *ozone_first = (float)pic_json_num(&j, pic_json_get(&j, cur, "ozone"), -1);
        }
    }

    pic_json_free(&j);
    return n;
}

static void *aq_thread_func(void *arg)
{
    pic_status_feature = "airquality";   /* data-source status (datastatus.c) */
    char *buf, *url;
    (void)arg;

    buf = malloc(MAX_RESPONSE);
    url = malloc(8192);
    if (!buf || !url) { free(buf); free(url); return NULL; }

    if (!pic_wait_for_network("airquality", &aq_running)) {
        free(buf); free(url);
        return NULL;
    }

    while (aq_running) {
        /* Location list: index 0 = QTH, then the grid row-major */
        static double lat[1 + AQ_NX * AQ_NY], lon[1 + AQ_NX * AQ_NY];
        static float vals[1 + AQ_NX * AQ_NY];
        double qlat = pic_config.qth_lat, qlon = pic_config.qth_lon;
        int total = 1 + AQ_NX * AQ_NY, got = 0, base, i, x, y, wait;
        float pm25 = -1, ozone = -1;

        lat[0] = qlat; lon[0] = qlon;
        for (y = 0; y < AQ_NY; y++) {
            for (x = 0; x < AQ_NX; x++) {
                double la = qlat + ((AQ_NY - 1) / 2.0 - y) * AQ_STEP;
                double lo = qlon + (x - (AQ_NX - 1) / 2.0) * AQ_STEP;
                if (la > 85) la = 85;
                if (la < -85) la = -85;
                while (lo > 180) lo -= 360;
                while (lo < -180) lo += 360;
                lat[1 + y * AQ_NX + x] = la;
                lon[1 + y * AQ_NX + x] = lo;
            }
        }
        for (i = 0; i < total; i++) vals[i] = -1;

        for (base = 0; base < total && aq_running; base += CHUNK) {
            int cnt = total - base < CHUNK ? total - base : CHUNK;
            int pos, n;

            pos = snprintf(url, 8192, "%s?latitude=", AQ_URL);
            for (i = 0; i < cnt; i++)
                pos += snprintf(url + pos, 8192 - pos, "%s%.2f",
                                i ? "," : "", lat[base + i]);
            pos += snprintf(url + pos, 8192 - pos, "&longitude=");
            for (i = 0; i < cnt; i++)
                pos += snprintf(url + pos, 8192 - pos, "%s%.2f",
                                i ? "," : "", lon[base + i]);
            snprintf(url + pos, 8192 - pos,
                     "&current=us_aqi%s", base == 0 ? ",pm2_5,ozone" : "");

            n = pic_fetch_to_buf(url, buf, MAX_RESPONSE);
            if (n > 0) {
                int k = pic_aq_parse(buf, (size_t)n, vals + base, cnt,
                                     base == 0 ? &pm25 : NULL,
                                     base == 0 ? &ozone : NULL);
                got += k;
            }
        }

        wait = RETRY_INTERVAL;
        if (got > 0) {
            pthread_mutex_lock(&cfg_data->mutex);
            for (y = 0; y < AQ_NY; y++)
                for (x = 0; x < AQ_NX; x++)
                    cfg_data->aqi[y][x] = vals[1 + y * AQ_NX + x];
            cfg_data->lat0 = qlat;
            cfg_data->lon0 = qlon;
            cfg_data->have_grid = 1;
            cfg_data->qth_aqi = vals[0] >= 0 ? (int)(vals[0] + 0.5f) : -1;
            cfg_data->qth_pm25 = pm25;
            cfg_data->qth_ozone = ozone;
            cfg_data->fetched = time(NULL);
            pthread_mutex_unlock(&cfg_data->mutex);
            if (got == total) wait = FETCH_INTERVAL;
            printf("airquality: %d/%d points, QTH AQI %.0f\n", got, total, vals[0]);
        } else {
            printf("airquality: fetch failed\n");
        }

        /* Refetch early if the QTH moves (e.g. corrected in the dashboard) */
        for (i = 0; i < wait && aq_running; i++) {
            sleep(1);
            if (pic_config.qth_lat != qlat || pic_config.qth_lon != qlon)
                break;
        }
    }

    free(buf);
    free(url);
    return NULL;
}

int pic_aq_start(pic_airquality_t *data)
{
    if (aq_running) return 0;
    cfg_data = data;
    pic_aq_reload(data);
    aq_running = 1;
    if (pthread_create(&aq_thread, NULL, aq_thread_func, NULL) != 0) {
        aq_running = 0;
        return -1;
    }
    printf("airquality: fetcher thread started\n");
    return 0;
}

void pic_aq_stop(void)
{
    if (!aq_running) return;
    aq_running = 0;
    pthread_join(aq_thread, NULL);
    cfg_data = NULL;
}

void pic_aq_reload(pic_airquality_t *data)
{
    FILE *f;
    char line[128];
    int badge = 1;

    f = fopen("/data/etc/pi-clock-renderer.conf", "r");
    if (f) {
        while (fgets(line, sizeof(line), f))
            if (strncmp(line, "AQ_BADGE=", 9) == 0)
                badge = atoi(line + 9) != 0;
        fclose(f);
    }
    pthread_mutex_lock(&data->mutex);
    data->show_badge = badge;
    pthread_mutex_unlock(&data->mutex);
}

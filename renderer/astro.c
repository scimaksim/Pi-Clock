/*
 * astro.c - Astrospheric forecast fetcher + local sun/moon events
 *
 * Astrospheric Data API v2 (Pro):
 *   POST https://v2-api-public.astrospheric.com/api/GetForecastData
 *   { "APIKey": "...", "Latitude": .., "Longitude": ..,
 *     "Variables": ["Cloud","Transparency","Seeing"],
 *     "ForecastLength": 48 }
 * Response: one array per variable, each entry
 *   { "Value": { "ValueColor": "#RRGGBB", "ActualValue": n },
 *     "HourOffset": n, "UTCForecastHour": "2026-10-30T18:00:00Z" }
 * plus "APICreditsRemainingToday". This request costs about 65 credits
 * (Cloud 15 + Transparency 30 + Seeing 20); a repeat before the next
 * model run is billed a flat 15. At the default 3-hour interval that
 * is ~16,000 of the 100,000 monthly Pro credits.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "astro.h"
#include "config.h"
#include "fetch.h"
#include "json.h"
#include "lunar.h"
#include "math_utils.h"
#include "nwsalert.h"   /* pic_nws_parse_time: ISO 8601 parser */
#include "solar.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#define ASTRO_URL "https://v2-api-public.astrospheric.com/api/GetForecastData"
#define MAX_RESPONSE (512 * 1024)
#define RETRY_INTERVAL (20 * 60)

static pthread_t astro_thread;
static volatile int astro_running = 0;
static pic_astro_t *cfg_data = NULL;

void pic_astro_init(pic_astro_t *data)
{
    memset(data, 0, sizeof(*data));
    data->interval_h = 3;
    data->cloud_clear = 20;
    data->credits_left = -1;
    snprintf(data->status, sizeof(data->status), "Waiting for forecast");
    pthread_mutex_init(&data->mutex, NULL);
}

/* ── Local sun / moon ─────────────────────────────────────────── */

static double altitude(double dec, double gha, double lat, double lon)
{
    double la = lat * DEG_TO_RAD;
    double ha = lon * DEG_TO_RAD + gha;   /* local hour angle */
    double s = sin(la) * sin(dec) + cos(la) * cos(dec) * cos(ha);
    if (s > 1) s = 1;
    if (s < -1) s = -1;
    return asin(s) * RAD_TO_DEG;
}

double pic_astro_sun_alt(time_t t, double lat, double lon)
{
    pic_solar_position_t s = pic_solar_position(t);
    return altitude(s.declination, s.gha, lat, lon);
}

double pic_astro_moon_alt(time_t t, double lat, double lon)
{
    pic_lunar_position_t m = pic_lunar_position(t);
    return altitude(m.declination, m.gha, lat, lon);
}

double pic_astro_moon_illum(time_t t, int *waxing)
{
    pic_lunar_position_t m = pic_lunar_position(t);
    if (waxing) *waxing = m.elongation < PI;
    return m.phase;   /* lunar.c's phase is the illuminated fraction */
}

static double body_alt(int body, time_t t, double lat, double lon)
{
    return body ? pic_astro_moon_alt(t, lat, lon)
                : pic_astro_sun_alt(t, lat, lon);
}

time_t pic_astro_next_crossing(int body, time_t t0, double lat, double lon,
                               double h0, int rising, int hours)
{
    const int step = 300;   /* 5 min: no double crossings this short */
    time_t t = t0, end = t0 + (time_t)hours * 3600;
    double prev = body_alt(body, t, lat, lon) - h0;

    while (t < end) {
        time_t tn = t + step;
        double cur = body_alt(body, tn, lat, lon) - h0;
        int crossed = rising ? (prev < 0 && cur >= 0)
                             : (prev >= 0 && cur < 0);
        if (crossed) {
            /* Bisect to ~10 s */
            time_t a = t, b = tn;
            while (b - a > 10) {
                time_t m = a + (b - a) / 2;
                double vm = body_alt(body, m, lat, lon) - h0;
                if ((vm >= 0) == (rising != 0)) b = m;
                else a = m;
            }
            return b;
        }
        prev = cur;
        t = tn;
    }
    return 0;
}

/* ── Forecast parsing ─────────────────────────────────────────── */

static unsigned parse_rgb(const pic_json_t *j, int tok)
{
    char hex[16];
    if (pic_json_str(j, tok, hex, sizeof(hex)) < 7 || hex[0] != '#')
        return 0x808080;
    return (unsigned)strtoul(hex + 1, NULL, 16) & 0xFFFFFF;
}

/* One hour's value for a variable object {"ValueColor","ActualValue"} */
static void read_var(const pic_json_t *j, int obj, float *val, unsigned *rgb)
{
    if (obj < 0) return;
    *val = (float)pic_json_num(j, pic_json_get(j, obj, "ActualValue"), -1);
    *rgb = parse_rgb(j, pic_json_get(j, obj, "ValueColor"));
}

static void hour_defaults(pic_astro_hour_t *h)
{
    memset(h, 0, sizeof(*h));
    h->cloud = h->transp = h->seeing = -1;
    h->cloud_rgb = h->transp_rgb = h->seeing_rgb = 0x404040;
}

/*
 * The live v2 API returns one list of hours, each carrying its variables:
 *   "HourlyForecast": [ { "UTCForecastHour": "2026-10-30T15:00:00Z",
 *                         "HourOffset": 0,
 *                         "Cloud": { "ValueColor": "#003E7E",
 *                                    "ActualValue": 0.0 },
 *                         "Transparency": {...}, "Seeing": {...} }, ... ]
 * The published docs instead show one array per variable with entries
 *   { "Value": { "ValueColor", "ActualValue" }, "UTCForecastHour" };
 * that layout is still accepted as a fallback.
 */
int pic_astro_parse_forecast(const char *js, size_t len,
                             pic_astro_hour_t *out, int max,
                             int *credits_left)
{
    static const char *vars[3] = { "Cloud", "Transparency", "Seeing" };
    pic_json_t j;
    int hourly, n, i, v;

    if (pic_json_parse(&j, js, len) != 0) return 0;
    if (!pic_json_is_object(&j, 0)) { pic_json_free(&j); return 0; }

    if (credits_left) {
        int c = pic_json_get(&j, 0, "APICreditsRemaining");
        if (c < 0) c = pic_json_get(&j, 0, "APICreditsRemainingToday");
        *credits_left = (int)pic_json_num(&j, c, -1);
    }

    hourly = pic_json_get(&j, 0, "HourlyForecast");
    if (pic_json_is_array(&j, hourly)) {
        n = pic_json_size(&j, hourly);
        if (n > max) n = max;
        for (i = 0; i < n; i++) {
            int e = pic_json_at(&j, hourly, i);
            char ts[40];
            hour_defaults(&out[i]);
            pic_json_str(&j, pic_json_get(&j, e, "UTCForecastHour"), ts, sizeof(ts));
            out[i].t = pic_nws_parse_time(ts);
            read_var(&j, pic_json_get(&j, e, "Cloud"), &out[i].cloud, &out[i].cloud_rgb);
            read_var(&j, pic_json_get(&j, e, "Transparency"), &out[i].transp, &out[i].transp_rgb);
            read_var(&j, pic_json_get(&j, e, "Seeing"), &out[i].seeing, &out[i].seeing_rgb);
            if (!out[i].t) { n = i; break; }
        }
        pic_json_free(&j);
        return n;
    }

    /* Documented layout: one array per variable */
    {
        int arr[3];
        for (v = 0; v < 3; v++) arr[v] = pic_json_get(&j, 0, vars[v]);
        n = pic_json_size(&j, arr[0]);   /* Cloud defines the timeline */
        if (n > max) n = max;

        for (i = 0; i < n; i++) {
            pic_astro_hour_t *h = &out[i];
            int e = pic_json_at(&j, arr[0], i);
            char ts[40];

            hour_defaults(h);
            pic_json_str(&j, pic_json_get(&j, e, "UTCForecastHour"), ts, sizeof(ts));
            h->t = pic_nws_parse_time(ts);
            read_var(&j, pic_json_get(&j, e, "Value"), &h->cloud, &h->cloud_rgb);

            for (v = 1; v < 3; v++) {
                int k, m = pic_json_size(&j, arr[v]);
                for (k = 0; k < m; k++) {
                    int f = pic_json_at(&j, arr[v], k);
                    char ts2[40];
                    pic_json_str(&j, pic_json_get(&j, f, "UTCForecastHour"),
                                 ts2, sizeof(ts2));
                    if (strcmp(ts, ts2) != 0) continue;
                    if (v == 1) read_var(&j, pic_json_get(&j, f, "Value"), &h->transp, &h->transp_rgb);
                    else        read_var(&j, pic_json_get(&j, f, "Value"), &h->seeing, &h->seeing_rgb);
                    break;
                }
            }
            if (!h->t) { n = i; break; }
        }
    }

    pic_json_free(&j);
    return n;
}

/*
 * describe_bad_response - Turn an unparseable reply into a short status
 * line. Prefers the API's own error text ({"ErrorInfo": "..."} and
 * similar), otherwise shows the start of the body. The raw reply is
 * saved to /tmp (tmpfs, root-only) for troubleshooting; replies never
 * contain the API key.
 */
static void describe_bad_response(const char *buf, int n, char *out, size_t size)
{
    static const char *keys[] = { "ErrorInfo", "Error", "error", "Message",
                                  "message", NULL };
    pic_json_t j;
    char msg[96] = "";
    int k;
    FILE *f;

    f = fopen("/tmp/pi-clock-astro-response.txt", "w");
    if (f) {
        fchmod(fileno(f), 0600);
        fwrite(buf, 1, (size_t)n, f);
        fclose(f);
    }
    printf("astro: unexpected response (%d bytes): %.300s\n", n, buf);

    if (pic_json_parse(&j, buf, (size_t)n) == 0) {
        if (pic_json_is_object(&j, 0))
            for (k = 0; keys[k] && !msg[0]; k++)
                pic_json_str(&j, pic_json_get(&j, 0, keys[k]), msg, sizeof(msg));
        else
            pic_json_str(&j, 0, msg, sizeof(msg));   /* bare JSON string */
        pic_json_free(&j);
    }
    if (!msg[0]) {
        /* Not JSON (e.g. an HTML error page): show the first text */
        int i, w = 0;
        for (i = 0; i < n && w < (int)sizeof(msg) - 1; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (c >= 0x20 && c < 0x7f) msg[w++] = (char)c;
            else if (w && msg[w - 1] != ' ') msg[w++] = ' ';
        }
        msg[w] = '\0';
    }
    snprintf(out, size, "Astrospheric: %.50s", msg[0] ? msg : "empty reply");
}

/* ── Thread ───────────────────────────────────────────────────── */

/* Read the API key; accept only plausible key characters so the value
 * can be embedded in JSON without escaping. */
static int read_key(char *key, size_t size)
{
    FILE *f = fopen(ASTRO_KEY_PATH, "r");
    size_t n = 0;
    int c;

    key[0] = '\0';
    if (!f) return -1;
    while ((c = fgetc(f)) != EOF && n < size - 1) {
        if (isalnum(c) || c == '-' || c == '_') key[n++] = (char)c;
        else if (c == '\n' || c == '\r' || c == ' ') break;
        else { n = 0; break; }
    }
    fclose(f);
    key[n] = '\0';
    return n >= 8 ? 0 : -1;
}

static void set_status(const char *msg)
{
    pthread_mutex_lock(&cfg_data->mutex);
    snprintf(cfg_data->status, sizeof(cfg_data->status), "%s", msg);
    pthread_mutex_unlock(&cfg_data->mutex);
}

static void *astro_thread_func(void *arg)
{
    char *buf;
    static pic_astro_hour_t temp[ASTRO_MAX_HOURS];
    (void)arg;

    buf = malloc(MAX_RESPONSE);
    if (!buf) return NULL;

    if (!pic_wait_for_network("astro", &astro_running)) {
        free(buf);
        return NULL;
    }

    while (astro_running) {
        char key[160], body[320];
        int wait = RETRY_INTERVAL, i, n, credits = -1, interval;
        double lat = pic_config.qth_lat, lon = pic_config.qth_lon;

        cfg_data->refetch = 0;
        pthread_mutex_lock(&cfg_data->mutex);
        interval = cfg_data->interval_h;
        pthread_mutex_unlock(&cfg_data->mutex);

        if (read_key(key, sizeof(key)) != 0) {
            set_status("Add Astrospheric API key in dashboard");
        } else if (lat == 0.0 && lon == 0.0) {
            set_status("Set your QTH location first");
        } else {
            snprintf(body, sizeof(body),
                     "{\"APIKey\":\"%s\",\"Latitude\":%.4f,\"Longitude\":%.4f,"
                     "\"Variables\":[\"Cloud\",\"Transparency\",\"Seeing\"],"
                     "\"ForecastLength\":48}", key, lat, lon);
            n = pic_fetch_post_json(ASTRO_URL, body, buf, MAX_RESPONSE);
            memset(key, 0, sizeof(key));
            memset(body, 0, sizeof(body));

            if (n <= 0) {
                set_status("Astrospheric unreachable or key rejected");
            } else if (strstr(buf, "No Data") && n < 64) {
                set_status("QTH outside Astrospheric forecast area");
                wait = interval * 3600;
            } else {
                n = pic_astro_parse_forecast(buf, (size_t)n, temp,
                                             ASTRO_MAX_HOURS, &credits);
                if (n > 0) {
                    pthread_mutex_lock(&cfg_data->mutex);
                    memcpy(cfg_data->hours, temp, (size_t)n * sizeof(temp[0]));
                    cfg_data->nhours = n;
                    cfg_data->fetched = time(NULL);
                    cfg_data->credits_left = credits;
                    cfg_data->status[0] = '\0';
                    pthread_mutex_unlock(&cfg_data->mutex);
                    wait = interval * 3600;
                    printf("astro: %d forecast hours, %d credits left\n",
                           n, credits);
                } else {
                    char why[64];
                    describe_bad_response(buf, (int)strlen(buf), why, sizeof(why));
                    set_status(why);
                }
            }
        }

        for (i = 0; i < wait && astro_running && !cfg_data->refetch; i++)
            sleep(1);
    }

    free(buf);
    return NULL;
}

int pic_astro_start(pic_astro_t *data)
{
    if (astro_running) return 0;
    cfg_data = data;
    pic_astro_reload(data);
    astro_running = 1;
    if (pthread_create(&astro_thread, NULL, astro_thread_func, NULL) != 0) {
        astro_running = 0;
        return -1;
    }
    printf("astro: fetcher thread started\n");
    return 0;
}

void pic_astro_stop(void)
{
    if (!astro_running) return;
    astro_running = 0;
    pthread_join(astro_thread, NULL);
    cfg_data = NULL;
}

void pic_astro_reload(pic_astro_t *data)
{
    FILE *f;
    char line[128];
    int interval = 3, clear = 20;

    f = fopen("/data/etc/pi-clock-renderer.conf", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "ASTRO_INTERVAL_H=", 17) == 0)
                interval = atoi(line + 17);
            else if (strncmp(line, "ASTRO_CLEAR_PCT=", 16) == 0)
                clear = atoi(line + 16);
        }
        fclose(f);
    }
    if (interval < 1) interval = 1;
    if (interval > 12) interval = 12;
    if (clear < 5) clear = 5;
    if (clear > 60) clear = 60;

    pthread_mutex_lock(&data->mutex);
    data->interval_h = interval;
    data->cloud_clear = clear;
    pthread_mutex_unlock(&data->mutex);

    /* Refetch immediately only if the key or the QTH changed — every
     * dashboard save triggers a reload, and each call spends credits. */
    {
        static time_t last_mtime = -1;
        static double last_lat = 999, last_lon = 999;
        struct stat st;
        time_t mtime = (stat(ASTRO_KEY_PATH, &st) == 0) ? st.st_mtime : 0;

        if (mtime != last_mtime || pic_config.qth_lat != last_lat ||
            pic_config.qth_lon != last_lon) {
            if (last_mtime != -1) data->refetch = 1;
            last_mtime = mtime;
            last_lat = pic_config.qth_lat;
            last_lon = pic_config.qth_lon;
        }
    }
}

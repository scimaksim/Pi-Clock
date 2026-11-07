/*
 * applet_backyard.c - Backyard conditions, frost watch and soil
 *
 *   Backyard
 *   56°F  75%  wind NE 6 mph
 *   Soil 61°F at 2 in
 *    Sat  Sun  Mon  Tue  Wed  Thu  Fri      <- overnight lows,
 *     50   47   53   53   56   61   58         frost-coloured
 *   No frost in the next 7 nights
 *   Rain 0.00" yesterday · 0.07" next 7 days
 *
 * Open-Meteo forecast for the QTH (no key), every 30 minutes. A low
 * of 36°F or less is flagged as frost possible, 32°F or less as a
 * freeze.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "config.h"
#include "fetch.h"
#include "json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OM_FMT "https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f" \
    "&current=temperature_2m,relative_humidity_2m,wind_speed_10m," \
    "wind_direction_10m,soil_temperature_6cm" \
    "&daily=temperature_2m_min,precipitation_sum&past_days=1&forecast_days=8" \
    "&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch" \
    "&timezone=auto"

static struct {
    pthread_mutex_t mutex;
    int    ok;
    double temp, rh, wind, wdir, soil;
    double low[9], rain[9];
    char   day[9][4];
    int    ndays;
} by;

int pic_backyard_parse(const char *js, size_t len)
{
    pic_json_t j;
    int cur, daily, mins, rains, times, n, i;
    if (pic_json_parse(&j, js, len) < 0) return -1;
    cur = pic_json_get(&j, 0, "current");
    daily = pic_json_get(&j, 0, "daily");
    if (cur < 0 || daily < 0) {
        pic_json_free(&j);
        return -1;
    }
    mins = pic_json_get(&j, daily, "temperature_2m_min");
    rains = pic_json_get(&j, daily, "precipitation_sum");
    times = pic_json_get(&j, daily, "time");
    n = pic_json_size(&j, mins);
    if (n > 9) n = 9;
    pthread_mutex_lock(&by.mutex);
    by.temp = pic_json_num(&j, pic_json_get(&j, cur, "temperature_2m"), NAN);
    by.rh = pic_json_num(&j, pic_json_get(&j, cur, "relative_humidity_2m"), NAN);
    by.wind = pic_json_num(&j, pic_json_get(&j, cur, "wind_speed_10m"), NAN);
    by.wdir = pic_json_num(&j, pic_json_get(&j, cur, "wind_direction_10m"), NAN);
    by.soil = pic_json_num(&j, pic_json_get(&j, cur, "soil_temperature_6cm"), NAN);
    for (i = 0; i < n; i++) {
        char d[16] = "";
        struct tm tm;
        by.low[i] = pic_json_num(&j, pic_json_at(&j, mins, i), NAN);
        by.rain[i] = pic_json_num(&j, pic_json_at(&j, rains, i), 0);
        pic_json_str(&j, pic_json_at(&j, times, i), d, sizeof(d));
        memset(&tm, 0, sizeof(tm));
        if (sscanf(d, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) {
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            tm.tm_hour = 12;
            mktime(&tm);
            strftime(by.day[i], sizeof(by.day[i]), "%a", &tm);
        }
    }
    by.ndays = n;
    by.ok = 1;
    pthread_mutex_unlock(&by.mutex);
    pic_json_free(&j);
    return 0;
}

static int backyard_work(pic_feature_t *f)
{
    char url[512], *buf = malloc(64 * 1024);
    int n;
    (void)f;
    if (!buf) return 300;
    snprintf(url, sizeof(url), OM_FMT, pic_config.qth_lat, pic_config.qth_lon);
    n = pic_fetch_to_buf(url, buf, 64 * 1024 - 1);
    if (n > 0) {
        buf[n] = '\0';
        n = pic_backyard_parse(buf, (size_t)n);
    }
    free(buf);
    return n == 0 ? 30 * 60 : 5 * 60;
}

static const char *compass(double d)
{
    static const char *p[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return isnan(d) ? "" : p[(int)((d + 22.5) / 45.0) % 8];
}

double pic_applet_render_backyard(cairo_t *cr, double width, time_t now,
                                  void *ud)
{
    double fs = width / 17.0, line_h = fs * 1.3, y, cw;
    double total = pic_applet_title(NULL, width, "") + fs * 0.3 + line_h * 4 + fs * 2.4;
    char line[128];
    int i, first_frost = -1, cols;
    double rain_next = 0;

    (void)now; (void)ud;
    if (!cr) return total;
    y = pic_applet_title(cr, width, "Backyard") + fs * 0.3;
    pthread_mutex_lock(&by.mutex);
    if (!by.ok) {
        pthread_mutex_unlock(&by.mutex);
        cairo_set_font_size(cr, fs);
        cairo_set_source_rgba(cr, 0.6, 0.6, 0.65, 0.9);
        cairo_move_to(cr, 0, y + fs);
        cairo_show_text(cr, "Loading\xe2\x80\xa6");
        return total;
    }
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    snprintf(line, sizeof(line), "%.0f\xc2\xb0" "F  %.0f%%  wind %s %.0f mph",
             by.temp, by.rh, compass(by.wdir), by.wind);
    cairo_set_source_rgba(cr, 0.95, 0.95, 0.97, 0.95);
    cairo_move_to(cr, 0, y + fs);
    pic_show_text_fit(cr, line, width);
    y += line_h;

    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs * 0.9);
    snprintf(line, sizeof(line), "Soil %.0f\xc2\xb0" "F at 2 in", by.soil);
    cairo_set_source_rgba(cr, 0.85, 0.75, 0.55, 0.95);
    cairo_move_to(cr, 0, y + fs);
    pic_show_text_fit(cr, line, width);
    y += line_h;

    /* Overnight lows: day i's minimum ≈ the night before it, so start
     * at tomorrow (index 2; 0 = yesterday, 1 = today) */
    cols = by.ndays - 2;
    if (cols > 7) cols = 7;
    cw = cols > 0 ? width / cols : width;
    for (i = 0; i < cols; i++) {
        double lo = by.low[i + 2];
        cairo_text_extents_t e;
        char v[8];
        cairo_set_font_size(cr, fs * 0.75);
        cairo_set_source_rgba(cr, 0.6, 0.62, 0.7, 0.9);
        cairo_text_extents(cr, by.day[i + 2], &e);
        cairo_move_to(cr, i * cw + (cw - e.x_advance) / 2, y + fs * 0.8);
        cairo_show_text(cr, by.day[i + 2]);
        snprintf(v, sizeof(v), "%.0f", lo);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, fs);
        if (lo <= 32)      cairo_set_source_rgba(cr, 0.75, 0.5, 1.0, 1);
        else if (lo <= 36) cairo_set_source_rgba(cr, 0.45, 0.75, 1.0, 1);
        else               cairo_set_source_rgba(cr, 0.9, 0.9, 0.92, 0.9);
        cairo_text_extents(cr, v, &e);
        cairo_move_to(cr, i * cw + (cw - e.x_advance) / 2, y + fs * 2.0);
        cairo_show_text(cr, v);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        if (first_frost < 0 && lo <= 36) first_frost = i + 2;
    }
    y += fs * 2.4;

    cairo_set_font_size(cr, fs * 0.9);
    if (first_frost >= 0) {
        double lo = by.low[first_frost];
        /* Day i's minimum comes early on day i: "Fri night" for Sat */
        snprintf(line, sizeof(line), "%s possible %s night (%.0f\xc2\xb0" "F)",
                 lo <= 32 ? "Freeze" : "Frost", by.day[first_frost - 1], lo);
        cairo_set_source_rgba(cr, 0.55, 0.8, 1.0, 1);
    } else {
        snprintf(line, sizeof(line), "No frost in the next %d nights", cols);
        cairo_set_source_rgba(cr, 0.6, 0.9, 0.6, 0.95);
    }
    cairo_move_to(cr, 0, y + fs);
    pic_show_text_fit(cr, line, width);
    y += line_h;

    for (i = 2; i < by.ndays; i++) rain_next += by.rain[i];
    snprintf(line, sizeof(line), "Rain %.2f\" yesterday \xc2\xb7 %.2f\" next %d days",
             by.rain[0], rain_next, by.ndays - 2);
    cairo_set_source_rgba(cr, 0.55, 0.7, 1.0, 0.9);
    cairo_move_to(cr, 0, y + fs);
    pic_show_text_fit(cr, line, width);
    pthread_mutex_unlock(&by.mutex);
    return total;
}

static pic_feature_t def = {
    .name = "backyard", .is_applet = 1, .applet = pic_applet_render_backyard,
    .label = "Backyard", .interval = 60, .applet_w = 220,
    .side = APPLET_SIDE_LEFT, .work = backyard_work, .needs_network = 1,
};

pic_feature_t *pic_feature_backyard(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&by.mutex, NULL);
        inited = 1;
    }
    return &def;
}

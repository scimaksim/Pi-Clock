/*
 * ha_notify.c - Send sky alerts to Home Assistant
 *
 * Posts JSON to a Home Assistant webhook (an automation with a
 * webhook trigger) so HA can push a phone notification, flash a
 * light or speak on a speaker:
 *
 *   pass             - a visible ISS / Tiangong / Hubble pass starts in
 *                      HA_PASS_LEAD_MIN minutes (default 10) and peaks
 *                      at least HA_PASS_MIN_EL degrees (default 20)
 *   aurora_now       - aurora may be visible / photographable from
 *                      home right now (it's dark and home is north of
 *                      the view or photo line)
 *   aurora_forecast  - the 3-day Kp forecast reaches the level home
 *                      needs within the next 24 hours
 *
 * Every payload has "event", "title" and "message" (ready to use as
 * a notification) plus event-specific fields. The webhook URL lives
 * root-only in /data/etc/pi-clock-ha-webhook (set in the dashboard).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "features_all.h"
#include "config.h"
#include "fetch.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int read_webhook(char *url, size_t size)
{
    FILE *f = fopen(HA_WEBHOOK_PATH, "r");
    size_t n;
    if (!f) return -1;
    if (!fgets(url, (int)size, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    n = strcspn(url, "\r\n");
    url[n] = '\0';
    /* Only plain http(s) URLs: the value is passed to wget */
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) return -1;
    for (n = 0; url[n]; n++)
        if (!isalnum((unsigned char)url[n]) && !strchr(":/._-[]", url[n]))
            return -1;
    return 0;
}

int pic_hanotify_wants(const char *flag_key)
{
    struct stat st;
    if (stat(HA_WEBHOOK_PATH, &st) != 0 || st.st_size < 10) return 0;
    return flag_key ? pic_conf_int(flag_key, 1) != 0
                    : pic_conf_int("HA_NOTIFY_PASSES", 1) ||
                      pic_conf_int("HA_NOTIFY_AURORA", 1);
}

static const char *compass8(double az)
{
    static const char *p[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return p[(int)((az + 22.5) / 45.0) % 8];
}

static void iso(char *out, size_t n, time_t t)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static int post(const char *json)
{
    char url[300], reply[256];
    int r;
    if (read_webhook(url, sizeof(url)) != 0) return -1;
    r = pic_fetch_post_json(url, json, reply, sizeof(reply));
    memset(url, 0, sizeof(url));
    printf("hanotify: sent %.60s... (%s)\n", json, r >= 0 ? "ok" : "failed");
    return r >= 0 ? 0 : -1;
}

/* Remember what was sent so each event goes out once */
static struct { char sat[16]; time_t start; } sent_pass[24];
static int n_sent_pass;
static int last_now_level;
static time_t last_now_sent, last_fc_t;
static int last_fc_level;

static int check_passes(time_t now)
{
    pic_pass_t p[24];
    int n = pic_passes_get(p, 24), i, k, lead, min_el, sent = 0;

    if (!pic_conf_int("HA_NOTIFY_PASSES", 1)) return 0;
    lead = pic_conf_int("HA_PASS_LEAD_MIN", 10);
    min_el = pic_conf_int("HA_PASS_MIN_EL", 20);
    for (i = 0; i < n; i++) {
        char json[640], a[16], b[16], st[24];
        long until = (long)(p[i].start - now);
        int dup = 0;

        if (p[i].max_el < min_el || until <= 0 || until > lead * 60L) continue;
        for (k = 0; k < n_sent_pass; k++)
            if (!strcmp(sent_pass[k].sat, p[i].sat) &&
                labs((long)(sent_pass[k].start - p[i].start)) < 120)
                dup = 1;
        if (dup) continue;

        pic_fmt_hm(a, sizeof(a), p[i].start);
        pic_fmt_hm(b, sizeof(b), p[i].end);
        iso(st, sizeof(st), p[i].start);
        snprintf(json, sizeof(json),
                 "{\"source\":\"pi-clock\",\"event\":\"pass\","
                 "\"title\":\"%s pass in %ld min\","
                 "\"message\":\"%s visible %s\\u2013%s, up to %d\\u00b0, %s \\u2192 %s. Look %s.\","
                 "\"sat\":\"%s\",\"start\":\"%s\",\"minutes\":%ld,"
                 "\"duration_min\":%d,\"max_el\":%d,\"from\":\"%s\",\"to\":\"%s\"}",
                 p[i].sat, (until + 30) / 60,
                 p[i].sat, a, b, (int)(p[i].max_el + 0.5),
                 compass8(p[i].az_start), compass8(p[i].az_end),
                 compass8(p[i].az_start),
                 p[i].sat, st, (until + 30) / 60,
                 (int)((p[i].end - p[i].start + 30) / 60), (int)(p[i].max_el + 0.5),
                 compass8(p[i].az_start), compass8(p[i].az_end));
        if (post(json) == 0) {
            int slot;
            if (n_sent_pass == 24) {   /* drop the oldest */
                memmove(sent_pass, sent_pass + 1, 23 * sizeof(sent_pass[0]));
                n_sent_pass = 23;
            }
            slot = n_sent_pass++;
            snprintf(sent_pass[slot].sat, sizeof(sent_pass[slot].sat), "%s", p[i].sat);
            sent_pass[slot].start = p[i].start;
            sent++;
        }
    }
    return sent;
}

static void clouds_text(char *out, size_t n, int c)
{
    if (c >= 0) snprintf(out, n, " Clouds %d%%.", c);
    else out[0] = '\0';
}

static void check_aurora(time_t now)
{
    pic_aurora_home_t h;
    char json[640], cl[24], when[24], st[24];

    if (!pic_conf_int("HA_NOTIFY_AURORA", 1)) return;
    pic_aurora_home(now, &h);
    if (!h.have_data) return;

    /* Happening now: on a rise in level, or every 3 h while it lasts */
    if (h.now_level == 0) {
        last_now_level = 0;
    } else if (h.now_level > last_now_level || now - last_now_sent > 3 * 3600) {
        clouds_text(cl, sizeof(cl), h.clouds_now);
        snprintf(json, sizeof(json),
                 "{\"source\":\"pi-clock\",\"event\":\"aurora_now\","
                 "\"title\":\"Aurora possible now!\","
                 "\"message\":\"Aurora may be %s from home right now (Kp %.1f). Look north.%s\","
                 "\"level\":\"%s\",\"kp\":%.1f,\"clouds\":%d}",
                 h.now_level == 2 ? "visible low in the north" : "caught in long-exposure photos",
                 h.kp_now, cl, h.now_level == 2 ? "visible" : "photo",
                 h.kp_now, h.clouds_now);
        if (post(json) == 0) {
            last_now_level = h.now_level;
            last_now_sent = now;
        }
    }

    /* Forecast within 24 h: once per forecast block (or if it rises) */
    if (h.next_t && h.next_t < now + 24 * 3600 &&
        (h.next_t != last_fc_t || h.next_level > last_fc_level)) {
        clouds_text(cl, sizeof(cl), h.clouds_next);
        pic_fmt_when(when, sizeof(when), now, h.next_t);
        iso(st, sizeof(st), h.next_t);
        snprintf(json, sizeof(json),
                 "{\"source\":\"pi-clock\",\"event\":\"aurora_forecast\","
                 "\"title\":\"Aurora watch\","
                 "\"message\":\"Kp %.0f forecast from %s \\u2014 aurora %s from home.%s\","
                 "\"level\":\"%s\",\"kp\":%.1f,\"when\":\"%s\",\"clouds\":%d}",
                 h.next_kp, when,
                 h.next_level == 2 ? "may be visible" : "possible in photos",
                 cl, h.next_level == 2 ? "visible" : "photo",
                 h.next_kp, st, h.clouds_next);
        if (post(json) == 0) {
            last_fc_t = h.next_t;
            last_fc_level = h.next_level;
        }
    }
}

static int hanotify_work(pic_feature_t *f)
{
    time_t now = time(NULL);
    (void)f;
    check_passes(now);
    check_aurora(now);
    return 30;
}

static int hanotify_wanted(pic_feature_t *f)
{
    (void)f;
    return pic_hanotify_wants(NULL);
}

static pic_feature_t feature_def = {
    .name = "hanotify", .wanted = hanotify_wanted, .work = hanotify_work,
    .needs_network = 1,
};

pic_feature_t *pic_feature_hanotify(void)
{
    return &feature_def;
}

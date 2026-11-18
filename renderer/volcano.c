/*
 * volcano.c - Smithsonian / USGS Weekly Volcanic Activity Report fetcher
 *
 * Runs a background thread that downloads the CAP edition of the
 * weekly report and stores one entry per volcano for the volcano
 * map layer.
 *
 * CAP structure (simplified, one <alert> holding many <info>):
 *
 *   <alert xmlns="urn:oasis:names:tc:emergency:cap:1.1">
 *     <info>
 *       <eventCode><valueName>Volcano Name</valueName>
 *                  <value>Krakatau</value></eventCode>
 *       <eventCode><valueName>New Activity</valueName>
 *                  <value>Yes</value></eventCode>
 *       <headline>Volcanic activity report for ...</headline>
 *       <area>
 *         <areaDesc>Indonesia</areaDesc>
 *         <circle>-6.1009,105.4233 0</circle>   <- "lat,lon radius"
 *       </area>
 *     </info>
 *     ...
 *   </alert>
 *
 * Like the other fetchers, this uses simple string scanning rather
 * than an XML library: we need four fields from a stable format.
 * The feed is declared ISO-8859-1, so text is converted to UTF-8
 * for Cairo.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE  /* memmem */
#include "volcano.h"
#include "datastatus.h"
#include "config.h"
#include "fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define GVP_CAP_URL "https://volcano.si.edu/news/WeeklyVolcanoCAP.xml"

/* The report changes weekly; 6 hours keeps it fresh without
 * hammering the Smithsonian's server. Failed fetches retry sooner. */
#define FETCH_INTERVAL       (6 * 3600)
#define FETCH_RETRY_INTERVAL (15 * 60)

/* Each <info> carries a long description paragraph, so the document
 * is larger than the entry count suggests (~100-200 KB). */
#define MAX_RESPONSE (1024 * 1024)

static pthread_t volcano_thread;
static volatile int volcano_running = 0;
static pic_volcanoes_t *cfg_data = NULL;

void pic_volcano_init(pic_volcanoes_t *data)
{
    memset(data, 0, sizeof(*data));
    data->show_labels = 1;
    pthread_mutex_init(&data->mutex, NULL);
}

/*
 * copy_text - Copy XML element text into a UTF-8 buffer.
 *
 * Decodes the five predefined XML entities and numeric references
 * in the ASCII range, converts ISO-8859-1 bytes (0x80-0xFF) to
 * two-byte UTF-8, and trims surrounding whitespace.
 */
static void copy_text(const char *src, const char *end,
                      char *out, int out_size)
{
    int w = 0;

    while (src < end && (*src == ' ' || *src == '\n' ||
                         *src == '\r' || *src == '\t'))
        src++;

    while (src < end && w < out_size - 1) {
        unsigned char c = (unsigned char)*src;

        if (c == '&') {
            const char *semi = memchr(src, ';', (size_t)(end - src));
            if (semi && semi - src <= 8) {
                int code = -1;
                if      (strncmp(src, "&amp;", 5) == 0)  code = '&';
                else if (strncmp(src, "&lt;", 4) == 0)   code = '<';
                else if (strncmp(src, "&gt;", 4) == 0)   code = '>';
                else if (strncmp(src, "&quot;", 6) == 0) code = '"';
                else if (strncmp(src, "&apos;", 6) == 0) code = '\'';
                else if (src[1] == '#') code = atoi(src + 2);
                if (code > 0 && code < 0x80) {
                    out[w++] = (char)code;
                    src = semi + 1;
                    continue;
                }
            }
        }

        if (c >= 0x80) {
            /* ISO-8859-1 → UTF-8 */
            if (w + 2 >= out_size) break;
            out[w++] = (char)(0xC0 | (c >> 6));
            out[w++] = (char)(0x80 | (c & 0x3F));
        } else if (c >= 0x20) {
            out[w++] = (char)c;
        }
        src++;
    }

    while (w > 0 && out[w - 1] == ' ') w--;
    out[w] = '\0';
}

/*
 * find_element - Locate <tag>text</tag> within [p, end).
 * Sets *text and *text_end to the element's content. Returns the
 * position after the closing tag, or NULL if not found.
 */
static const char *find_element(const char *p, const char *end,
                                const char *tag,
                                const char **text, const char **text_end)
{
    char open[40], close[40];
    const char *s, *e;
    size_t len = (size_t)(end - p);

    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    s = memmem(p, len, open, strlen(open));
    if (!s) return NULL;
    s += strlen(open);
    e = memmem(s, (size_t)(end - s), close, strlen(close));
    if (!e) return NULL;

    *text = s;
    *text_end = e;
    return e + strlen(close);
}

/*
 * event_code_value - Find the <value> of the <eventCode> whose
 * <valueName> equals `name`, within one <info> block.
 */
static int event_code_value(const char *p, const char *end,
                            const char *name, char *out, int out_size)
{
    const char *t, *te;
    const char *next;

    while ((next = find_element(p, end, "eventCode", &t, &te)) != NULL) {
        const char *nt, *nte, *vt, *vte;
        if (find_element(t, te, "valueName", &nt, &nte) &&
            find_element(t, te, "value", &vt, &vte) &&
            (size_t)(nte - nt) == strlen(name) &&
            strncmp(nt, name, (size_t)(nte - nt)) == 0) {
            copy_text(vt, vte, out, out_size);
            return 0;
        }
        p = next;
    }
    return -1;
}

int pic_volcano_parse_cap(const char *xml, pic_volcano_t *out, int max)
{
    const char *p = xml;
    int count = 0;

    while (count < max) {
        const char *info, *info_end, *t, *te;
        pic_volcano_t v;
        char flag[16];

        info = strstr(p, "<info>");
        if (!info) break;
        info_end = strstr(info, "</info>");
        if (!info_end) break;
        p = info_end + 7;

        memset(&v, 0, sizeof(v));

        /* Location: <circle>lat,lon radius</circle>, falling back
         * to <point> in case the feed ever switches element. */
        if (!find_element(info, info_end, "circle", &t, &te) &&
            !find_element(info, info_end, "point", &t, &te))
            continue;
        if (sscanf(t, "%lf,%lf", &v.lat, &v.lon) != 2)
            continue;
        if (v.lat < -90 || v.lat > 90 || v.lon < -180 || v.lon > 180)
            continue;

        if (event_code_value(info, info_end, "Volcano Name",
                             v.name, sizeof(v.name)) != 0)
            continue;

        if (event_code_value(info, info_end, "New Activity",
                             flag, sizeof(flag)) == 0)
            v.new_activity = (flag[0] == 'Y' || flag[0] == 'y');

        if (find_element(info, info_end, "areaDesc", &t, &te))
            copy_text(t, te, v.country, sizeof(v.country));

        /* The report occasionally lists a volcano twice (e.g. two
         * observatories). Keep one entry, preferring "new". */
        {
            int i, dup = 0;
            for (i = 0; i < count; i++) {
                if (strcmp(out[i].name, v.name) == 0) {
                    out[i].new_activity |= v.new_activity;
                    dup = 1;
                    break;
                }
            }
            if (dup) continue;
        }

        out[count++] = v;
    }

    return count;
}

static void *volcano_thread_func(void *arg)
{
    pic_status_feature = "volcano";   /* data-source status (datastatus.c) */
    char *buf;

    (void)arg;

    buf = malloc(MAX_RESPONSE);
    if (!buf) {
        fprintf(stderr, "volcano: failed to allocate fetch buffer\n");
        return NULL;
    }

    if (!pic_wait_for_network("volcano", &volcano_running)) {
        free(buf);
        return NULL;
    }

    while (volcano_running) {
        int n, wait = FETCH_RETRY_INTERVAL;

        n = pic_fetch_to_buf(GVP_CAP_URL, buf, MAX_RESPONSE - 1);
        if (n > 0) {
            static pic_volcano_t temp[PIC_MAX_VOLCANOES];
            int count;

            buf[n] = '\0';
            count = pic_volcano_parse_cap(buf, temp, PIC_MAX_VOLCANOES);

            if (count > 0) {
                pthread_mutex_lock(&cfg_data->mutex);
                memcpy(cfg_data->items, temp,
                       (size_t)count * sizeof(pic_volcano_t));
                cfg_data->count = count;
                cfg_data->last_fetched = time(NULL);
                pthread_mutex_unlock(&cfg_data->mutex);
                wait = FETCH_INTERVAL;
            }
            printf("volcano: parsed %d volcanoes from weekly report\n",
                   count);
        } else {
            printf("volcano: fetch failed, retrying in %ds\n", wait);
        }

        {
            int s;
            for (s = 0; s < wait && volcano_running; s++)
                sleep(1);
        }
    }

    free(buf);
    return NULL;
}

int pic_volcano_start(pic_volcanoes_t *data)
{
    if (volcano_running) return 0;

    cfg_data = data;
    pic_volcano_reload(data);
    volcano_running = 1;

    if (pthread_create(&volcano_thread, NULL,
                       volcano_thread_func, NULL) != 0) {
        volcano_running = 0;
        fprintf(stderr, "volcano: failed to create thread\n");
        return -1;
    }

    printf("volcano: fetcher thread started\n");
    return 0;
}

void pic_volcano_stop(void)
{
    if (!volcano_running) return;
    volcano_running = 0;
    pthread_join(volcano_thread, NULL);
    cfg_data = NULL;
    printf("volcano: fetcher thread stopped\n");
}

void pic_volcano_reload(pic_volcanoes_t *data)
{
    FILE *f;
    char line[256];
    int labels = 1, new_only = 0;

    f = fopen("/data/etc/pi-clock-renderer.conf", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "VOLCANO_LABELS=", 15) == 0)
                labels = atoi(line + 15) != 0;
            else if (strncmp(line, "VOLCANO_NEW_ONLY=", 17) == 0)
                new_only = atoi(line + 17) != 0;
        }
        fclose(f);
    }

    pthread_mutex_lock(&data->mutex);
    data->show_labels = labels;
    data->new_only = new_only;
    pthread_mutex_unlock(&data->mutex);

    printf("volcano: labels=%d new_only=%d\n", labels, new_only);
}

/*
 * layers_ships.c - Live ship positions (AIS)
 *
 * Streams AIS position reports from aisstream.io over a WebSocket
 * and draws each vessel as a small arrow along its heading (moving)
 * or a dot (at anchor / moored). Names appear when zoomed in.
 *
 * aisstream.io needs a free API key, entered in the dashboard and
 * stored root-only at /data/etc/pi-clock-aisstream.key. The
 * subscription covers the visible map when zoomed in, or a box
 * around the QTH when the whole world is shown (a global stream is
 * more than a Pi should parse).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#define _GNU_SOURCE
#include "feature.h"
#include "config.h"
#include "json.h"
#include "ws.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define AIS_HOST     "stream.aisstream.io"
#define AIS_PATH     "/v0/stream"
#define AIS_KEY_PATH "/data/etc/pi-clock-aisstream.key"
#define MAX_SHIPS    3000
#define STALE_S      (30 * 60)

typedef struct {
    int    mmsi;
    float  lat, lon, cog, sog, hdg;
    time_t seen;
    char   name[21];
} ship_t;

typedef struct {
    pthread_mutex_t mutex;
    ship_t  ships[MAX_SHIPS];
    int     n;
    int     labels;
    int     status;            /* 0 ok, 1 no key, 2 connect error */
    double  box[4];            /* lat0, lon0, lat1, lon1 subscribed */
} ships_t;

static ships_t sh;
static pthread_mutex_t fd_mutex = PTHREAD_MUTEX_INITIALIZER;
static int active_fd = -1;

static int read_key(char *key, size_t size)
{
    FILE *f = fopen(AIS_KEY_PATH, "r");
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

/* Subscription box: viewport when zoomed in, else around the QTH */
static void choose_box(double b[4])
{
    double span = pic_config.view_span_lon, span_lat = pic_config.view_span_lat;
    if (span > 0.0001 && span <= 150) {
        b[0] = pic_config.view_center_lat - span_lat / 2;
        b[1] = pic_config.view_center_lon - span / 2;
        b[2] = pic_config.view_center_lat + span_lat / 2;
        b[3] = pic_config.view_center_lon + span / 2;
    } else {
        b[0] = pic_config.qth_lat - 10;
        b[1] = pic_config.qth_lon - 14;
        b[2] = pic_config.qth_lat + 10;
        b[3] = pic_config.qth_lon + 14;
    }
    if (b[0] < -90) b[0] = -90;
    if (b[2] > 90) b[2] = 90;
    if (b[1] < -180) b[1] = -180;
    if (b[3] > 180) b[3] = 180;
}

/* Parse one aisstream message and update the table. Returns 1 if used. */
int pic_ships_handle(const char *js, size_t len, time_t now)
{
    pic_json_t j;
    int meta, msg, rep, i, slot = -1, oldest = 0, mmsi;
    char type[48];
    ship_t s;

    if (pic_json_parse_max(&j, js, len, 256) < 0) return 0;
    pic_json_str(&j, pic_json_get(&j, 0, "MessageType"), type, sizeof(type));
    meta = pic_json_get(&j, 0, "MetaData");
    msg = pic_json_get(&j, 0, "Message");
    rep = pic_json_get(&j, msg, type);
    if (meta < 0 || rep < 0) {
        pic_json_free(&j);
        return 0;
    }
    memset(&s, 0, sizeof(s));
    mmsi = (int)pic_json_num(&j, pic_json_get(&j, meta, "MMSI"), 0);
    s.mmsi = mmsi;
    s.lat = (float)pic_json_num(&j, pic_json_get(&j, meta, "latitude"), 999);
    s.lon = (float)pic_json_num(&j, pic_json_get(&j, meta, "longitude"), 999);
    s.cog = (float)pic_json_num(&j, pic_json_get(&j, rep, "Cog"), 360);
    s.sog = (float)pic_json_num(&j, pic_json_get(&j, rep, "Sog"), 0);
    s.hdg = (float)pic_json_num(&j, pic_json_get(&j, rep, "TrueHeading"), 511);
    pic_json_str(&j, pic_json_get(&j, meta, "ShipName"), s.name, sizeof(s.name));
    pic_json_free(&j);
    for (i = (int)strlen(s.name); i > 0 && s.name[i - 1] == ' '; i--)
        s.name[i - 1] = '\0';
    if (!mmsi || fabsf(s.lat) > 90 || fabsf(s.lon) > 180) return 0;
    s.seen = now;

    pthread_mutex_lock(&sh.mutex);
    for (i = 0; i < sh.n; i++) {
        if (sh.ships[i].mmsi == mmsi) { slot = i; break; }
        if (sh.ships[i].seen < sh.ships[oldest].seen) oldest = i;
    }
    if (slot < 0) slot = sh.n < MAX_SHIPS ? sh.n++ : oldest;
    if (!s.name[0] && sh.ships[slot].mmsi == mmsi)
        memcpy(s.name, sh.ships[slot].name, sizeof(s.name));
    sh.ships[slot] = s;
    pthread_mutex_unlock(&sh.mutex);
    return 1;
}

static int ships_work(pic_feature_t *f)
{
    char key[96], sub[512];
    unsigned char *buf;
    pic_ws_t ws;
    double box[4];
    unsigned gen;
    static int errors;          /* consecutive server errors */
    int server_error = 0;
    time_t last_prune = 0;
    int msgs = 0;

    if (read_key(key, sizeof(key)) != 0) {
        pthread_mutex_lock(&sh.mutex);
        sh.status = 1;
        pthread_mutex_unlock(&sh.mutex);
        return 60;   /* Check again for a newly entered key */
    }

    gen = pic_config.viewport_gen;
    choose_box(box);
    if (pic_ws_connect(&ws, AIS_HOST, 443, AIS_PATH, 60) != 0) {
        pthread_mutex_lock(&sh.mutex);
        sh.status = 2;
        pthread_mutex_unlock(&sh.mutex);
        return 120;
    }
    snprintf(sub, sizeof(sub),
             "{\"APIKey\":\"%s\",\"BoundingBoxes\":[[[%.3f,%.3f],[%.3f,%.3f]]],"
             "\"FilterMessageTypes\":[\"PositionReport\","
             "\"StandardClassBPositionReport\"]}",
             key, box[0], box[1], box[2], box[3]);
    memset(key, 0, sizeof(key));
    if (pic_ws_send_text(&ws, sub) != 0) {
        memset(sub, 0, sizeof(sub));
        pic_ws_close(&ws);
        return 120;
    }
    memset(sub, 0, sizeof(sub));
    pthread_mutex_lock(&fd_mutex);
    active_fd = ws.fd;
    pthread_mutex_unlock(&fd_mutex);
    printf("ships: subscribed %.1f,%.1f to %.1f,%.1f\n",
           box[0], box[1], box[2], box[3]);

    pthread_mutex_lock(&sh.mutex);
    sh.status = 0;
    memcpy(sh.box, box, sizeof(box));
    pthread_mutex_unlock(&sh.mutex);

    server_error = 0;
    buf = malloc(16384);
    if (!buf) {
        pic_ws_close(&ws);
        return 60;
    }
    /* Stream until stopped, the viewport moves, or the link drops */
    while (!pic_feature_interrupted(f) && pic_config.viewport_gen == gen) {
        int n = pic_ws_read(&ws, buf, 16384);
        time_t now = time(NULL);
        if (n < 0) break;
        if (n > 0) {
            /* Errors arrive as {"error": "..."} */
            if (!memcmp(buf, "{\"error\"", 8)) {
                printf("ships: server error: %.80s\n", (char *)buf);
                server_error = 1;
                break;
            }
            msgs += pic_ships_handle((char *)buf, (size_t)n, now);
        }
        if (now - last_prune > 60) {
            int i, w = 0;
            pthread_mutex_lock(&sh.mutex);
            for (i = 0; i < sh.n; i++)
                if (now - sh.ships[i].seen < STALE_S) sh.ships[w++] = sh.ships[i];
            sh.n = w;
            pthread_mutex_unlock(&sh.mutex);
            last_prune = now;
        }
    }
    free(buf);
    pthread_mutex_lock(&fd_mutex);
    active_fd = -1;
    pthread_mutex_unlock(&fd_mutex);
    pic_ws_close(&ws);
    printf("ships: stream closed after %d reports\n", msgs);
    if (pic_feature_interrupted(f) || pic_config.viewport_gen != gen) return 1;
    /* A server error (bad key, too many connections) backs off
     * 2, 4, 8 ... up to 30 minutes instead of retrying every 30 s */
    if (server_error) {
        int wait = 120 << (errors < 4 ? errors : 4);
        errors++;
        return wait > 1800 ? 1800 : wait;
    }
    errors = 0;
    return msgs > 0 ? 30 : 120;
}

static void ships_reload(pic_feature_t *f)
{
    (void)f;
    pthread_mutex_lock(&sh.mutex);
    sh.labels = pic_conf_int("SHIP_LABELS", 1);
    pthread_mutex_unlock(&sh.mutex);
}

/* Unblock a pending read so the layer turns off promptly */
static void ships_interrupt(pic_feature_t *f)
{
    (void)f;
    pthread_mutex_lock(&fd_mutex);
    if (active_fd >= 0) shutdown(active_fd, SHUT_RDWR);
    pthread_mutex_unlock(&fd_mutex);
}

static void ships_stopped(pic_feature_t *f)
{
    (void)f;
    pthread_mutex_lock(&sh.mutex);
    sh.n = 0;
    pthread_mutex_unlock(&sh.mutex);
}

void pic_layer_render_ships(cairo_t *cr, int width, int height,
                            time_t now, void *ud)
{
    double zoom = pic_zoom_factor();
    double s = height / 260.0 * (1.0 + 0.4 * (pic_symbol_zoom() - 1.0));
    double fs = height / 95.0;
    int i, labels;

    (void)now; (void)ud;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs);
    pthread_mutex_lock(&sh.mutex);
    labels = sh.labels && zoom >= 2.5;
    /* key / connection status: ships_note() */
    for (i = 0; i < sh.n; i++) {
        const ship_t *v = &sh.ships[i];
        double x = pic_lon_to_x(v->lon, width), y = pic_lat_to_y(v->lat, height);
        double dir;
        if (x < -5 || x > width + 5 || y < -5 || y > height + 5) continue;

        if (v->sog < 0.5) {
            cairo_set_source_rgba(cr, 0.6, 0.65, 0.7, 0.8);
            cairo_new_path(cr);
            cairo_arc(cr, x, y, s * 0.55, 0, 2 * M_PI);
            cairo_fill(cr);
            continue;
        }
        dir = (v->hdg < 360 ? v->hdg : v->cog < 360 ? v->cog : 0) * M_PI / 180.0;
        cairo_save(cr);
        cairo_translate(cr, x, y);
        cairo_rotate(cr, dir);
        cairo_move_to(cr, 0, -s * 1.6);
        cairo_line_to(cr, s * 0.8, s * 1.0);
        cairo_line_to(cr, 0, s * 0.5);
        cairo_line_to(cr, -s * 0.8, s * 1.0);
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, 0.3, 0.9, 0.85, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
        cairo_set_line_width(cr, s * 0.2);
        cairo_stroke(cr);
        cairo_restore(cr);
        if (labels && v->name[0])
            pic_draw_text_pill(cr, x + s * 1.6, y + fs * 0.35, v->name,
                               0.75, 0.95, 0.92, 0.5);
    }
    pthread_mutex_unlock(&sh.mutex);
}

/* Key missing / no connection: a note in the bottom legend stack
 * (drawn as a legend so it can't overlap the others) */
static void ships_note(cairo_t *cr, int width, int height, time_t now, void *ud)
{
    int st;
    (void)now; (void)ud;
    pthread_mutex_lock(&sh.mutex);
    st = sh.status;
    pthread_mutex_unlock(&sh.mutex);
    if (st == 1 || st == 2)
        pic_draw_legend_line(cr, width, height, 2,
                             st == 1 ? "Ships: add an aisstream.io key in the dashboard"
                                     : "Ships: can't reach aisstream.io",
                             1.0, 0.8, 0.5);
}

static pic_feature_t feature_def = {
    .name = "ships", .layer = pic_layer_render_ships, .interval = 10,
    .legend = ships_note,
    .extra_mb = 2, .reload = ships_reload, .work = ships_work,
    .needs_network = 1, .stopped = ships_stopped,
    .interrupt = ships_interrupt,
};

pic_feature_t *pic_feature_ships(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&sh.mutex, NULL);
        sh.labels = 1;
        inited = 1;
    }
    return &feature_def;
}

/*
 * radar.c - IEM NEXRAD mosaic fetcher
 *
 * See radar.h. Each cycle:
 *   1. Work out how many screen pixels the radar extent covers at the
 *      current zoom, and request the WMS image at that size (capped).
 *   2. Download to tmpfs, decode with stb_image, premultiply alpha.
 *   3. Swap the new surface in under the mutex and free the old one.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "radar.h"
#include "datastatus.h"
#include "config.h"
#include "fetch.h"
#include "feature.h"
#include "stb_image.h"   /* declarations only; implemented in image.c */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RADAR_WMS_FMT \
    "https://mesonet.agron.iastate.edu/cgi-bin/wms/nexrad/n0q.cgi" \
    "?SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap&LAYERS=nexrad-n0q-900913" \
    "&STYLES=&SRS=EPSG:4326&BBOX=%.1f,%.1f,%.1f,%.1f" \
    "&WIDTH=%d&HEIGHT=%d&FORMAT=image/png&TRANSPARENT=true"

/*
 * IEM's N0Q colour ramp: index i is -32 + 0.5 i dBZ (index 0, black,
 * is "no echo" and is transparent in the WMS reply). Read from the
 * palette of IEM's national composite (n0q_0.png, 14 Nov 2026). The
 * WMS returns these exact colours, so each pixel maps back to its
 * reflectivity.
 */
static const unsigned char N0Q_RAMP[256][3] = {
    {  0,  0,  0}, {133,113,143}, {133,114,143}, {134,115,141},
    {135,117,139}, {135,118,139}, {136,119,137}, {137,121,135},
    {137,122,135}, {138,123,133}, {139,125,132}, {139,126,132},
    {140,127,130}, {141,129,128}, {141,130,128}, {142,131,126},
    {143,132,124}, {143,133,124}, {144,135,123}, {145,136,121},
    {145,137,121}, {146,139,119}, {147,141,117}, {150,145, 83},
    {152,148, 87}, {155,151, 91}, {157,154, 96}, {160,157,100},
    {163,160,104}, {165,163,109}, {168,166,113}, {170,169,118},
    {173,172,122}, {176,175,126}, {178,178,131}, {183,184,140},
    {186,187,144}, {189,190,148}, {191,193,153}, {194,196,157},
    {196,199,162}, {199,202,166}, {202,205,170}, {204,208,175},
    {210,212,180}, {207,210,180}, {201,204,180}, {198,201,180},
    {195,199,180}, {192,196,180}, {189,193,180}, {185,190,180},
    {182,187,180}, {179,185,180}, {176,182,180}, {173,179,180},
    {170,176,180}, {164,171,180}, {160,168,180}, {157,165,180},
    {154,162,180}, {151,160,180}, {148,157,180}, {145,154,180},
    {148,155,181}, {144,152,180}, {140,149,179}, {136,146,178},
    {128,140,176}, {124,137,175}, {120,134,174}, {116,131,172},
    {112,128,171}, {108,125,170}, {103,121,169}, { 99,118,168},
    { 95,115,167}, { 91,112,166}, { 87,109,164}, { 79,103,162},
    { 75,100,161}, { 71, 97,160}, { 67, 94,159}, { 65, 91,158},
    { 67, 97,162}, { 69,104,166}, { 72,111,170}, { 74,118,174},
    { 77,125,178}, { 79,132,182}, { 81,139,187}, { 86,153,195},
    { 89,159,199}, { 91,166,203}, { 94,173,207}, { 96,180,212},
    { 98,187,216}, {101,194,220}, {103,201,224}, {106,208,228},
    {111,214,232}, {104,214,215}, { 89,214,179}, { 82,214,162},
    { 75,214,144}, { 67,214,126}, { 60,214,109}, { 53,214, 91},
    { 17,213, 24}, { 17,209, 23}, { 16,205, 23}, { 16,200, 22},
    { 16,196, 22}, { 15,188, 21}, { 15,183, 20}, { 14,179, 20},
    { 14,175, 19}, { 14,171, 19}, { 13,166, 18}, { 13,162, 18},
    { 13,158, 17}, { 12,153, 17}, { 12,149, 16}, { 12,145, 16},
    { 11,136, 15}, { 11,132, 14}, { 10,128, 14}, { 10,124, 13},
    { 10,119, 13}, {  9,115, 12}, {  9,111, 12}, {  9,107, 11},
    {  8,102, 11}, {  8, 98, 10}, {  9, 94,  9}, { 50,115,  8},
    { 70,125,  8}, { 91,136,  7}, {111,146,  7}, {132,157,  6},
    {152,168,  6}, {173,178,  5}, {193,189,  5}, {214,199,  4},
    {234,210,  4}, {255,226,  0}, {255,216,  0}, {255,211,  0},
    {255,206,  0}, {255,201,  0}, {255,196,  0}, {255,192,  0},
    {255,187,  0}, {255,182,  0}, {255,177,  0}, {255,172,  0},
    {255,167,  0}, {255,162,  0}, {255,153,  0}, {255,148,  0},
    {255,143,  0}, {255,138,  0}, {255,133,  0}, {255,128,  0},
    {255,  0,  0}, {248,  0,  0}, {241,  0,  0}, {234,  0,  0},
    {227,  0,  0}, {213,  0,  0}, {205,  0,  0}, {198,  0,  0},
    {191,  0,  0}, {184,  0,  0}, {177,  0,  0}, {170,  0,  0},
    {163,  0,  0}, {155,  0,  0}, {148,  0,  0}, {141,  0,  0},
    {127,  0,  0}, {120,  0,  0}, {113,  0,  0}, {255,255,255},
    {255,245,255}, {255,234,255}, {255,223,255}, {255,212,255},
    {255,201,255}, {255,190,255}, {255,179,255}, {255,157,255},
    {255,146,255}, {255,117,255}, {252,107,253}, {249, 96,250},
    {246, 86,247}, {243, 75,244}, {240, 64,241}, {237, 54,239},
    {234, 43,236}, {231, 32,233}, {225, 11,227}, {178,  0,255},
    {172,  0,252}, {164,  0,247}, {155,  0,244}, {147,  0,239},
    {136,  0,234}, {131,  0,232}, {121,  0,226}, {114,  0,221},
    {105,  0,219}, {  5,236,240}, {  5,235,240}, {  5,234,240},
    {  5,221,224}, {  5,220,224}, {  5,219,224}, {  5,205,208},
    {  5,204,208}, {  4,189,192}, {  4,188,192}, {  4,187,192},
    {  4,174,176}, {  4,173,176}, {  4,158,160}, {  4,157,160},
    {  4,156,160}, {  3,142,144}, {  3,141,144}, {  3,140,144},
    {  3,126,128}, {  3,125,128}, {  3,111,112}, {  3,110,112},
    {  3,109,112}, {  2, 95, 96}, {  2, 94, 96}, {  2, 79, 80},
    {  2, 78, 80}, {  2, 77, 80}, {  2, 63, 64}, {  2, 62, 64},
    {  2, 61, 64}, {  1, 48, 48}, {  1, 47, 48}, {  1, 32, 32},
    {  1, 31, 32}, {  1, 30, 32}, { 58,103,181}, { 58,102,181},
    { 58,101,181}, { 58,100,181}, { 58, 99,181}, { 58, 98,181},
};

#define RADAR_TMP        "/tmp/pi-clock-radar.png"
#define FETCH_INTERVAL   (5 * 60)
#define RETRY_INTERVAL   60
#define MAX_IMAGE_W      2400   /* ~8 MB ARGB at 2400x1062 */
#define MIN_IMAGE_W      360

static pthread_t radar_thread;
static volatile int radar_running = 0;
static pic_radar_t *cfg_data = NULL;

void pic_radar_init(pic_radar_t *data)
{
    memset(data, 0, sizeof(*data));
    data->show_time = 1;
    data->min_dbz = RADAR_MIN_DBZ_DEFAULT;
    pthread_mutex_init(&data->mutex, NULL);
}

void pic_radar_free(pic_radar_t *data)
{
    pthread_mutex_lock(&data->mutex);
    if (data->image) cairo_surface_destroy(data->image);
    data->image = NULL;
    pthread_mutex_unlock(&data->mutex);
}

cairo_surface_t *pic_radar_decode_png(const char *path)
{
    int w, h, ch, x, y, stride;
    unsigned char *px, *dst;
    cairo_surface_t *s;

    px = stbi_load(path, &w, &h, &ch, 4);
    if (!px) return NULL;

    s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        stbi_image_free(px);
        cairo_surface_destroy(s);
        return NULL;
    }

    cairo_surface_flush(s);
    dst = cairo_image_surface_get_data(s);
    stride = cairo_image_surface_get_stride(s);

    /* Cairo ARGB32 is premultiplied — radar is mostly transparent,
     * so (unlike the opaque basemaps) the multiply matters here. */
    for (y = 0; y < h; y++) {
        const unsigned char *src = px + (size_t)y * w * 4;
        unsigned int *row = (unsigned int *)(dst + (size_t)y * stride);
        for (x = 0; x < w; x++) {
            unsigned a = src[x * 4 + 3];
            unsigned r = src[x * 4 + 0] * a / 255;
            unsigned g = src[x * 4 + 1] * a / 255;
            unsigned b = src[x * 4 + 2] * a / 255;
            row[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(s);
    stbi_image_free(px);
    return s;
}

/* Exact RGB -> ramp index, or -1. Small open-addressed table built on
 * first use; only the fetch thread (and the tests) call this. */
static int ramp_index(unsigned r, unsigned g, unsigned b)
{
    static short slot[1024];
    static int built;
    unsigned key = (r << 16) | (g << 8) | b, h;
    int i;

    if (!built) {
        for (i = 0; i < 1024; i++) slot[i] = -1;
        for (i = 1; i < 256; i++) {
            unsigned k = ((unsigned)N0Q_RAMP[i][0] << 16) |
                         ((unsigned)N0Q_RAMP[i][1] << 8) | N0Q_RAMP[i][2];
            h = (k * 2654435761u) >> 22;
            while (slot[h] >= 0) h = (h + 1) & 1023;
            slot[h] = (short)i;
        }
        built = 1;
    }
    for (h = (key * 2654435761u) >> 22; slot[h] >= 0; h = (h + 1) & 1023) {
        i = slot[h];
        if ((((unsigned)N0Q_RAMP[i][0] << 16) | ((unsigned)N0Q_RAMP[i][1] << 8) |
             N0Q_RAMP[i][2]) == key)
            return i;
    }
    return -1;
}

long pic_radar_filter(cairo_surface_t *s, double min_dbz, int despeckle,
                      long *opaque_out, long *unknown_out)
{
    int w, h, stride, x, y;
    unsigned char *data, *cls;
    long opaque = 0, unknown = 0, removed = 0;
    int min_idx = (int)ceil((min_dbz + 32.0) * 2.0);   /* first index kept */

    if (opaque_out) *opaque_out = 0;
    if (unknown_out) *unknown_out = 0;
    if (!s || cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) return 0;
    cairo_surface_flush(s);
    w = cairo_image_surface_get_width(s);
    h = cairo_image_surface_get_height(s);
    stride = cairo_image_surface_get_stride(s);
    data = cairo_image_surface_get_data(s);
    if (!data || w <= 0 || h <= 0) return 0;
    /* 0 empty, 1 too weak, 2 kept, 3 kept (colour not in the ramp) */
    cls = calloc((size_t)w * h, 1);
    if (!cls) return 0;

    for (y = 0; y < h; y++) {
        const unsigned int *row = (const unsigned int *)(data + (size_t)y * stride);
        for (x = 0; x < w; x++) {
            unsigned p = row[x], a = p >> 24;
            int i;
            if (a == 0) continue;
            opaque++;
            /* Only fully opaque pixels carry an exact ramp colour */
            i = a == 255 ? ramp_index((p >> 16) & 255, (p >> 8) & 255, p & 255) : -1;
            if (i < 0) { unknown++; cls[(size_t)y * w + x] = 3; }
            else cls[(size_t)y * w + x] = i < min_idx ? 1 : 2;
        }
    }
    if (opaque_out) *opaque_out = opaque;
    if (unknown_out) *unknown_out = unknown;

    /* If many colours don't match, IEM has changed its ramp: dBZ can't
     * be read, so leave the image as it came. */
    if (opaque > 0 && unknown * 20 > opaque) {
        free(cls);
        return -1;
    }

    /* Isolated returns: keep a pixel only if at least 4 of its 8
     * neighbours are kept too. Precipitation is contiguous; clutter,
     * birds and insects that survive the threshold are scattered. */
    if (despeckle) {
        unsigned char *keep = malloc((size_t)w * h);
        if (keep) {
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++) {
                    int n = 0, dx, dy;
                    size_t k = (size_t)y * w + x;
                    keep[k] = 1;
                    if (cls[k] != 2) continue;
                    for (dy = -1; dy <= 1; dy++)
                        for (dx = -1; dx <= 1; dx++) {
                            int xx = x + dx, yy = y + dy;
                            if ((dx || dy) && xx >= 0 && yy >= 0 && xx < w && yy < h &&
                                cls[(size_t)yy * w + xx] >= 2)
                                n++;
                        }
                    if (n < 4) keep[k] = 0;
                }
            for (y = 0; y < h * w; y++)
                if (!keep[y]) cls[y] = 1;
            free(keep);
        }
    }

    for (y = 0; y < h; y++) {
        unsigned int *row = (unsigned int *)(data + (size_t)y * stride);
        for (x = 0; x < w; x++)
            if (cls[(size_t)y * w + x] == 1) { row[x] = 0; removed++; }
    }
    free(cls);
    cairo_surface_mark_dirty(s);
    return removed;
}

/* Request size: the radar extent's on-screen width, within limits. */
static void request_size(int screen_w, int *w, int *h)
{
    double span = pic_config.view_span_lon;
    double px;

    if (screen_w <= 0) screen_w = 1920;
    if (span < 0.0001) span = 360.0;
    px = (RADAR_EAST - RADAR_WEST) / span * screen_w;

    *w = (int)px;
    if (*w < MIN_IMAGE_W) *w = MIN_IMAGE_W;
    if (*w > MAX_IMAGE_W) *w = MAX_IMAGE_W;
    *h = (int)lround(*w * (RADAR_NORTH - RADAR_SOUTH) /
                     (RADAR_EAST - RADAR_WEST));
}

static void *radar_thread_func(void *arg)
{
    pic_status_feature = "radar";   /* data-source status (datastatus.c) */
    (void)arg;

    if (!pic_wait_for_network("radar", &radar_running))
        return NULL;

    while (radar_running) {
        char url[512];
        int w, h, i, sw, wait = RETRY_INTERVAL;
        const char *why = "download failed";
        unsigned int gen = pic_config.viewport_gen;
        cairo_surface_t *img = NULL;
        double new_ext[4] = { 0, 0, 0, 0 }, new_span = 0;

        pthread_mutex_lock(&cfg_data->mutex);
        sw = cfg_data->screen_w;
        pthread_mutex_unlock(&cfg_data->mutex);
        /* Only the part of the US mosaic that's on screen, at about
         * screen resolution, so zooming in gets sharper radar */
        {
            static const double full[4] = { RADAR_WEST, RADAR_SOUTH, RADAR_EAST, RADAR_NORTH };
            double ext[4];
            if (!pic_view_extent(full, sw, MAX_IMAGE_W, ext, &w, &h)) {
                memcpy(ext, full, sizeof(ext));
                request_size(sw, &w, &h);
            }
            {   /* the request carries one decimal: place the image on
                 * exactly the box that was asked for */
                int q;
                for (q = 0; q < 4; q++) ext[q] = round(ext[q] * 10.0) / 10.0;
            }
            snprintf(url, sizeof(url), RADAR_WMS_FMT,
                     ext[0], ext[1], ext[2], ext[3], w, h);
            why = "download failed";
            if (pic_fetch_to_file(url, RADAR_TMP, 60) == 0) {   /* the server can take over 30 s */
                img = pic_radar_decode_png(RADAR_TMP);
                why = "reply was not a usable image";
            }
            if (img) {
                /* Drop echoes too weak to be precipitation (ground
                 * clutter, birds, insects; IEM leaves these in over
                 * large blocks in winter). Speckle removal only when
                 * a pixel is ~2 km or finer: on the world map a pixel
                 * is ~20 km and one pixel can be a real storm. */
                long op, unk, gone;
                double dpp = (ext[2] - ext[0]) / (w > 0 ? w : 1), min_dbz;
                pthread_mutex_lock(&cfg_data->mutex);
                min_dbz = cfg_data->min_dbz;
                pthread_mutex_unlock(&cfg_data->mutex);
                gone = pic_radar_filter(img, min_dbz, dpp <= 0.02, &op, &unk);
                if (gone < 0)
                    printf("radar: %ld of %ld pixels not in the N0Q colour ramp; "
                           "shown unfiltered\n", unk, op);
                else if (op > 0)
                    printf("radar: %ld of %ld echo pixels dropped (below %.0f dBZ%s)\n",
                           gone, op, min_dbz, dpp <= 0.02 ? " or isolated" : "");
            }
            unlink(RADAR_TMP);
            if (img) {
                memcpy(new_ext, ext, sizeof(ext));
                new_span = pic_config.view_span_lon;
            }
        }

        if (img) {
            cairo_surface_t *old;
            pthread_mutex_lock(&cfg_data->mutex);
            old = cfg_data->image;
            /* Image and its extent change together */
            memcpy(cfg_data->ext, new_ext, sizeof(new_ext));
            cfg_data->span = new_span;
            cfg_data->image = img;
            cfg_data->fetched = time(NULL);
            pthread_mutex_unlock(&cfg_data->mutex);
            if (old) cairo_surface_destroy(old);
            wait = FETCH_INTERVAL;
            printf("radar: mosaic updated (%dx%d)\n", w, h);
        } else {
            printf("radar: %s, retrying in %ds\n", why, wait);
        }

        /* Sleep, but refetch early if the zoom/pan changed so the
         * image resolution follows the viewport. */
        for (i = 0; i < wait && radar_running; i++) {
            sleep(1);
            if (pic_config.viewport_gen != gen && i > 5) break;
        }
    }
    return NULL;
}

int pic_radar_start(pic_radar_t *data)
{
    if (radar_running) return 0;
    cfg_data = data;
    pic_radar_reload(data);
    radar_running = 1;
    if (pthread_create(&radar_thread, NULL, radar_thread_func, NULL) != 0) {
        radar_running = 0;
        fprintf(stderr, "radar: failed to create thread\n");
        return -1;
    }
    printf("radar: fetcher thread started\n");
    return 0;
}

void pic_radar_stop(void)
{
    pic_radar_t *d = cfg_data;
    if (!radar_running) return;
    radar_running = 0;
    pthread_join(radar_thread, NULL);
    /* Release the image so a disabled layer costs no memory */
    if (d) pic_radar_free(d);
    cfg_data = NULL;
    printf("radar: fetcher thread stopped\n");
}

void pic_radar_reload(pic_radar_t *data)
{
    FILE *f;
    char line[128];
    int show_time = 1;
    double min_dbz = RADAR_MIN_DBZ_DEFAULT;

    f = fopen("/data/etc/pi-clock-renderer.conf", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "RADAR_TIME=", 11) == 0)
                show_time = atoi(line + 11) != 0;
            else if (strncmp(line, "RADAR_MIN_DBZ=", 14) == 0) {
                char *end;
                double v = strtod(line + 14, &end);
                if (end != line + 14 && v >= -32 && v <= 40) min_dbz = v;
            }
        }
        fclose(f);
    }
    pthread_mutex_lock(&data->mutex);
    data->show_time = show_time;
    data->min_dbz = min_dbz;
    pthread_mutex_unlock(&data->mutex);
}

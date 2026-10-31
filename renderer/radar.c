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
    (void)arg;

    if (!pic_wait_for_network("radar", &radar_running))
        return NULL;

    while (radar_running) {
        char url[512];
        int w, h, i, sw, wait = RETRY_INTERVAL;
        unsigned int gen = pic_config.viewport_gen;
        cairo_surface_t *img = NULL;

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
            snprintf(url, sizeof(url), RADAR_WMS_FMT,
                     ext[0], ext[1], ext[2], ext[3], w, h);
            if (pic_fetch_to_file(url, RADAR_TMP, 30) == 0)
                img = pic_radar_decode_png(RADAR_TMP);
            unlink(RADAR_TMP);
            if (img) {
                pthread_mutex_lock(&cfg_data->mutex);
                memcpy(cfg_data->ext, ext, sizeof(ext));
                cfg_data->span = pic_config.view_span_lon;
                pthread_mutex_unlock(&cfg_data->mutex);
            }
        }

        if (img) {
            cairo_surface_t *old;
            pthread_mutex_lock(&cfg_data->mutex);
            old = cfg_data->image;
            cfg_data->image = img;
            cfg_data->fetched = time(NULL);
            pthread_mutex_unlock(&cfg_data->mutex);
            if (old) cairo_surface_destroy(old);
            wait = FETCH_INTERVAL;
            printf("radar: mosaic updated (%dx%d)\n", w, h);
        } else {
            printf("radar: fetch/decode failed, retrying in %ds\n", wait);
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

    f = fopen("/data/etc/pi-clock-renderer.conf", "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "RADAR_TIME=", 11) == 0)
                show_time = atoi(line + 11) != 0;
        }
        fclose(f);
    }
    pthread_mutex_lock(&data->mutex);
    data->show_time = show_time;
    pthread_mutex_unlock(&data->mutex);
}

/*
 * radar.h - Live US NEXRAD radar mosaic
 *
 * Downloads the NEXRAD base reflectivity (N0Q) national composite from
 * the Iowa Environmental Mesonet (IEM) Web Map Service every 5 minutes.
 * The WMS renders the mosaic in EPSG:4326 — the same equirectangular
 * projection as the Pi-Clock map — as a transparent PNG at exactly the
 * pixel size the current viewport needs, so no reprojection is needed.
 *
 * Coverage: contiguous US (plus edges of Canada/Mexico in range of
 * US radars). The composite itself is refreshed every 5 minutes.
 *
 * Data: Iowa Environmental Mesonet, Iowa State University
 *       https://mesonet.agron.iastate.edu/ (NWS NEXRAD Level III)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_RADAR_H
#define PIC_RADAR_H

#include <cairo/cairo.h>
#include <pthread.h>
#include <time.h>

/* Geographic extent requested from the WMS (degrees) */
#define RADAR_WEST   -126.0
#define RADAR_EAST    -65.0
#define RADAR_SOUTH    23.0
#define RADAR_NORTH    50.0

typedef struct {
    cairo_surface_t *image;     /* Latest mosaic (owned), NULL if none */
    time_t           fetched;   /* When `image` was downloaded         */
    int              show_time; /* Draw "Radar hh:mm" stamp             */
    int              screen_w;  /* Output width, set by the renderer    */
    double           ext[4];    /* W, S, E, N actually covered by image  */
    double           span;      /* view_span_lon the image was made for  */
    double           min_dbz;   /* Weaker echoes are dropped            */
    pthread_mutex_t  mutex;
} pic_radar_t;

void pic_radar_init(pic_radar_t *data);
int  pic_radar_start(pic_radar_t *data);
void pic_radar_stop(void);
void pic_radar_free(pic_radar_t *data);

/* Echoes below this are dropped by default. Rain is usually 15 dBZ and
 * up; light snow can be 5-15. 10 dBZ plus speckle removal clears the
 * clear-air returns IEM keeps over large blocks in winter. */
#define RADAR_MIN_DBZ_DEFAULT 10.0

/* Keys: RADAR_TIME=0|1, RADAR_MIN_DBZ=<dBZ> (-32 keeps everything) */
void pic_radar_reload(pic_radar_t *data);

/*
 * pic_radar_decode_png - Decode a PNG file to a premultiplied ARGB32
 * Cairo surface (stb_image). Exposed for testing; NULL on failure.
 */
cairo_surface_t *pic_radar_decode_png(const char *path);

/*
 * pic_radar_filter - Make transparent every pixel of an N0Q image
 * (from pic_radar_decode_png) weaker than min_dbz and, with
 * despeckle, every kept pixel with fewer than 4 kept neighbours.
 * Returns pixels removed, or -1 (image untouched) when over 5% of
 * the echo pixels aren't colours of IEM's ramp. *opaque and *unknown
 * get the echo-pixel and unmatched-pixel counts (either may be NULL).
 */
long pic_radar_filter(cairo_surface_t *s, double min_dbz, int despeckle,
                      long *opaque, long *unknown);

#endif /* PIC_RADAR_H */

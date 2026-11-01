/*
 * feature.h - Shared plumbing for data-driven layers and applets
 *
 * Most overlays follow the same shape: a background thread that
 * fetches something every N minutes while the layer (or applet) is
 * enabled, a mutex-protected snapshot the renderer reads, and a few
 * settings in the renderer config. pic_feature_t captures that so a
 * new overlay only supplies its work and draw functions, and
 * display.c starts, reloads and stops every feature from one table.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_FEATURE_H
#define PIC_FEATURE_H

#include <pthread.h>
#include <time.h>
#include "layer.h"
#include "applet.h"
#include "config.h"

typedef struct pic_feature pic_feature_t;

struct pic_feature {
    const char *name;           /* Layer or applet name                 */
    int         is_applet;      /* 0 = map layer, 1 = side applet        */

    /* Layer registration */
    pic_layer_render_fn layer;
    pic_layer_render_fn legend; /* Optional, drawn at full opacity       */
    int         interval;       /* Seconds between re-renders            */
    int         extra_mb;       /* RAM budget beyond the surface         */
    int         default_on;     /* Layer on for fresh installs           */

    /* Applet registration */
    pic_applet_render_fn applet;
    const char *label;
    int         applet_w;       /* Minimum applet width (px at 1080p) */
    int         side;

    void       *data;           /* Passed to the render functions        */
    volatile int layer_on;      /* Set by display.c: layer enabled now   */

    /* Called on the main thread when settings may have changed. */
    void (*reload)(pic_feature_t *f);

    /*
     * One unit of background work (normally one fetch). Runs on the
     * feature's own thread. Returns seconds to wait before the next
     * call. NULL = no thread (render-only feature).
     */
    int  (*work)(pic_feature_t *f);
    int  needs_network;

    /*
     * Optional: return non-zero to keep the thread running even when
     * the layer/applet is off (e.g. phone alerts need pass data).
     */
    int  (*wanted)(pic_feature_t *f);

    /* Optional: wake a thread blocked in I/O so stop doesn't wait. */
    void (*interrupt)(pic_feature_t *f);

    /* Optional: called after the thread has stopped (free caches). */
    void (*stopped)(pic_feature_t *f);

    /* Thread state (managed by feature.c) */
    pthread_t        thread;
    volatile int     running;
    volatile int     wake;      /* Set to cut the current sleep short     */
};

/* Thread flags are shared between the main and fetch threads */
static inline int pic_feature_running(pic_feature_t *f)
{
    return __atomic_load_n(&f->running, __ATOMIC_ACQUIRE);
}

/* True when a work function should return early (stop or poke). */
static inline int pic_feature_interrupted(pic_feature_t *f)
{
    return !pic_feature_running(f) || __atomic_load_n(&f->wake, __ATOMIC_ACQUIRE);
}

int  pic_feature_start(pic_feature_t *f);
void pic_feature_stop(pic_feature_t *f);

/*
 * Shutdown: signal every feature at once, then wait up to timeout_s
 * in total for their threads. Threads still stuck in a download are
 * left to die with the process, so exit never waits on the network.
 */
void pic_feature_stop_all(pic_feature_t **f, int n, int timeout_s);

/* Ask a running feature to do its work now (e.g. after a reload). */
void pic_feature_poke(pic_feature_t *f);

/*
 * Config helpers: read KEY=value from the renderer config
 * (/data/etc/pi-clock-renderer.conf, or $PIC_RENDERER_CONF).
 */
int    pic_conf_str(const char *key, char *out, int out_size);
int    pic_conf_int(const char *key, int def);
double pic_conf_num(const char *key, double def);

/* Text helpers shared by the overlays */
void pic_draw_text_pill(cairo_t *cr, double x, double y, const char *text,
                        double r, double g, double b, double bg_a);

/*
 * Bottom-centre status line for layer legends. Slots stack upward so
 * several layers' notes never overlap each other or the side panels:
 * 0 SPC, 1 light pollution, 2 ships, 3 aurora, 4 rain totals,
 * 5 eBird, 6 drought, 7 dark sites.
 */
void pic_draw_legend_line(cairo_t *cr, int width, int height, int slot,
                          const char *text, double r, double g, double b);

/*
 * Part of a fixed data extent worth fetching for the current view:
 * the extent clipped to the viewport (plus a 15% margin), and the
 * image size that gives about screen resolution (max_w cap). ext is
 * {west, south, east, north}. Returns 0 if the extent is off-screen.
 */
int pic_view_extent(const double full[4], int screen_w, int max_w,
                    double ext[4], int *w, int *h);

/* Short relative time: "in 3h 20m", "in 2d 4h", "now" */
void pic_fmt_until(char *out, int n, time_t now, time_t t);

/* "7:35 PM", "Tue 7:35 PM" when not today, "Nov 12" beyond a week */
void pic_fmt_hm(char *out, int n, time_t t);
void pic_fmt_when(char *out, int n, time_t now, time_t t);

/* Great-circle distance in km */
double pic_dist_km(double lat1, double lon1, double lat2, double lon2);

/* Shorten `text` in place with an ellipsis so it fits max_w. */
void pic_fit_text(cairo_t *cr, char *text, double max_w);

/*
 * Applet title in the common style (bold, width/12, rule beneath).
 * cr may be NULL (measure only). Returns the height used.
 */
double pic_applet_title(cairo_t *cr, double width, const char *title);

/*
 * Map key: a single row along the bottom of the map explaining the
 * lines currently drawn. Legend callbacks add entries each frame
 * (duplicates are ignored); display.c draws the row once after all
 * legends and clears it. Main thread only.
 */
void pic_map_key_add(const char *label, double r, double g, double b,
                     double a, int dashed);
/* bottom: y the row must stay above (ticker top), or 0 */
void pic_map_key_draw(cairo_t *cr, int width, int height, double bottom);

/*
 * True when an image fetched for a view `fetched_span` degrees wide no
 * longer fits the current view (after a zoom or a global/local
 * switch). Callers skip the image until the refetch for the new view
 * lands.
 */
static inline int pic_view_image_stale(double fetched_span)
{
    double span = pic_config.view_span_lon;
    if (span < 0.0001) span = 360.0;
    /* Zoomed out: the image would be a box in the middle. Zoomed far
     * in: it would be blown up into blocks. Either way skip it until
     * the image for the new view arrives (a few seconds). */
    return fetched_span > 0 &&
           (span > fetched_span * 1.2 || span < fetched_span / 2.0);
}

#endif /* PIC_FEATURE_H */

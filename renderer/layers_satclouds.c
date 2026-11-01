/*
 * layers_satclouds.c - Real satellite cloud imagery
 *
 * Geostationary infrared (ABI/AHI band 13, 10.3 µm) from NASA GIBS,
 * turned into a cloud overlay: warm (clear) surfaces become fully
 * transparent, cloud tops fade in with height (colder = whiter), and
 * the coldest storm tops can keep GIBS's colour enhancement
 * (SATCLOUD_STYLE=enhanced) or render white (=white, default).
 *
 * Coverage comes from three satellites, each used only for the
 * longitudes nearest its sub-point so the distorted limb is never
 * drawn: Himawari (Asia/Australia), GOES-West (Pacific) and
 * GOES-East (Americas/Atlantic). GIBS carries no Meteosat imagery,
 * so Europe, Africa and the Indian Ocean (about 5°W-70°E) stay clear.
 *
 * The request covers just the visible viewport at screen resolution,
 * so zooming in fetches sharper imagery. Band 13 is only ~2-5 km per
 * pixel in GIBS, so it is fetched no finer than that and smoothed up
 * (blocky otherwise). When zoomed in (view under ~60° wide) daylit
 * areas use the 1 km red visible band instead, which shows far more
 * cloud detail; IR still covers night. Refreshed every 10 minutes
 * (GIBS lags real time by ~1 hour).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "config.h"
#include "solar.h"
#include "fetch.h"
#include "stb_image.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GIBS_FMT \
    "https://gibs.earthdata.nasa.gov/wms/epsg4326/best/wms.cgi" \
    "?SERVICE=WMS&REQUEST=GetMap&VERSION=1.1.1&LAYERS=%s&STYLES=" \
    "&SRS=EPSG:4326&BBOX=%.3f,%.3f,%.3f,%.3f&WIDTH=%d&HEIGHT=%d" \
    "&FORMAT=image/png&TRANSPARENT=TRUE&TIME=%s"
#define SC_TMP     "/tmp/pi-clock-satcloud.png"
#define MAX_W      2048
#define LAT_LIMIT  72.0   /* Beyond this the geostationary view is junk */

static const struct { const char *layer, *vis; double lon0, lon1, sub; } bands[] = {
    { "GOES-East_ABI_Band13_Clean_Infrared", "GOES-East_ABI_Band2_Red_Visible_1km",  -106.2,  -5.0,  -75.2 },
    { "GOES-West_ABI_Band13_Clean_Infrared", "GOES-West_ABI_Band2_Red_Visible_1km",  -180.0, -106.2, -137.2 },
    { "Himawari_AHI_Band13_Clean_Infrared",  "Himawari_AHI_Band3_Red_Visible_1km",     70.5,  180.0,  140.7 },
};

#define IR_DEG_PX   0.04    /* Band 13 detail in GIBS (~4-5 km)       */
#define VIS_DEG_PX  0.009   /* Visible band detail (~1 km)            */
#define VIS_MAX_SPAN 60.0   /* Use the visible band below this width  */

/*
 * Bilinear upscale of an RGBA image (alpha-weighted so transparent
 * no-data pixels don't darken edges). Returns a new malloc'd buffer.
 */
static unsigned char *upscale_rgba(const unsigned char *src, int w, int h,
                                   int W, int H)
{
    unsigned char *dst = malloc((size_t)W * H * 4);
    int x, y, c;

    if (!dst) return NULL;
    for (y = 0; y < H; y++) {
        double fy = (y + 0.5) * h / H - 0.5;
        int y0 = (int)floor(fy), y1;
        double ty = fy - y0;
        if (y0 < 0) { y0 = 0; ty = 0; }
        y1 = y0 + 1 < h ? y0 + 1 : h - 1;
        for (x = 0; x < W; x++) {
            double fx = (x + 0.5) * w / W - 0.5;
            int x0 = (int)floor(fx), x1;
            double tx = fx - x0, wt[4], a = 0, acc[3] = { 0, 0, 0 };
            const unsigned char *p[4];
            int k;
            if (x0 < 0) { x0 = 0; tx = 0; }
            x1 = x0 + 1 < w ? x0 + 1 : w - 1;
            p[0] = src + ((size_t)y0 * w + x0) * 4; wt[0] = (1 - tx) * (1 - ty);
            p[1] = src + ((size_t)y0 * w + x1) * 4; wt[1] = tx * (1 - ty);
            p[2] = src + ((size_t)y1 * w + x0) * 4; wt[2] = (1 - tx) * ty;
            p[3] = src + ((size_t)y1 * w + x1) * 4; wt[3] = tx * ty;
            for (k = 0; k < 4; k++) {
                double wa = wt[k] * p[k][3];
                a += wa;
                for (c = 0; c < 3; c++) acc[c] += wa * p[k][c];
            }
            {
                unsigned char *d = dst + ((size_t)y * W + x) * 4;
                for (c = 0; c < 3; c++)
                    d[c] = a > 0 ? (unsigned char)(acc[c] / a + 0.5) : 0;
                d[3] = (unsigned char)(a + 0.5);
            }
        }
    }
    return dst;
}

typedef struct {
    pthread_mutex_t  mutex;
    cairo_surface_t *image;     /* Premultiplied ARGB cloud overlay  */
    double lon0, lon1, lat0, lat1;
    double span;                /* view_span_lon the image was made for */
    int    enhanced;
    int    screen_w, screen_h;
    time_t fetched;
} satclouds_t;

static satclouds_t sc;

/*
 * Map one GIBS enhanced-IR pixel to a premultiplied cloud pixel.
 * Grey (r≈g≈b) is the temperature ramp: ~80-100 for warm ground and
 * ocean, brighter for colder cloud. Saturated colours mark the
 * coldest tops (< about -30 °C).
 */
uint32_t pic_satcloud_pixel(unsigned r, unsigned g, unsigned b, unsigned a,
                            int enhanced)
{
    unsigned mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    unsigned mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double al, c;

    if (a < 128) return 0;
    if (mx - mn < 25) {
        double v = (r + g + b) / 3.0;
        al = (v - 118.0) / 90.0;
        if (al <= 0) return 0;
        if (al > 1) al = 1;
        al *= 0.88;
        c = 205.0 + 50.0 * al;
        r = g = b = (unsigned)c;
    } else if (enhanced) {
        al = 0.9;
    } else {
        al = 0.9;
        r = g = b = 250;
    }
    return ((uint32_t)(al * 255) << 24) |
           ((uint32_t)(r * al) << 16) | ((uint32_t)(g * al) << 8) |
           (uint32_t)(b * al);
}

/*
 * Blit an RGBA PNG into the composite at column x0, filling only
 * pixels not already covered (have[] marks covered tile pixels), so
 * an older time step can patch tiles missing from a newer one.
 * *expect gets the number of pixels the satellite can see (well
 * inside its disk). Returns the covered count, or -1 on failure.
 */
static int blit_png(const char *path, cairo_surface_t *dst, int x0,
                    int want_w, int enhanced, double lat0, double lat1,
                    double lon0, double lon1, double sub,
                    unsigned char *have, int *expect)
{
    int w, h, ch, x, y, dw, dh, stride, covered = 0;
    unsigned char *px = stbi_load(path, &w, &h, &ch, 4), *d;

    if (!px) return -1;
    dw = cairo_image_surface_get_width(dst);
    dh = cairo_image_surface_get_height(dst);
    stride = cairo_image_surface_get_stride(dst);
    d = cairo_image_surface_get_data(dst);
    if (w > want_w || w < 2 || h < 2) {
        stbi_image_free(px);
        return -1;
    }
    if (w < want_w || h < dh) {     /* fetched coarser: smooth it up */
        unsigned char *up = upscale_rgba(px, w, h, want_w, dh);
        stbi_image_free(px);
        if (!up) return -1;
        px = up;
        w = want_w;
        h = dh;
    }
    *expect = 0;
    for (y = 0; y < h && y < dh; y++) {
        uint32_t *row = (uint32_t *)(d + (size_t)y * stride);
        const unsigned char *s = px + (size_t)y * w * 4;
        double clat = cos((lat1 - (y + 0.5) / h * (lat1 - lat0)) * M_PI / 180);
        for (x = 0; x < w && x0 + x < dw; x++, s += 4)
            if (x0 + x >= 0) {
                /* Fade out toward the limb (55-72° from the sub-
                 * satellite point), where the view grazes the Earth
                 * and cold ground reads as cloud. */
                double lon = lon0 + (x + 0.5) / w * (lon1 - lon0);
                double c = acos(clat * cos((lon - sub) * M_PI / 180)) * 180 / M_PI;
                double fade = c < 55 ? 1.0 : (72 - c) / 17.0;
                uint32_t p;
                unsigned char *hv = &have[(size_t)y * w + x];
                if (fade < 0) fade = 0;
                if (c < 70) (*expect)++;
                if (*hv) { covered += c < 70; continue; }
                if (s[3] < 128) continue;
                *hv = 1;
                covered += c < 70;
                p = pic_satcloud_pixel(s[0], s[1], s[2], s[3], enhanced);
                if (fade < 1.0) {
                    uint32_t a = (uint32_t)((p >> 24) * fade);
                    uint32_t r = (uint32_t)(((p >> 16) & 255) * fade);
                    uint32_t g = (uint32_t)(((p >> 8) & 255) * fade);
                    uint32_t b = (uint32_t)((p & 255) * fade);
                    p = (a << 24) | (r << 16) | (g << 8) | b;
                }
                row[x0 + x] = p;
            }
    }
    stbi_image_free(px);
    return covered;
}

/*
 * Daytime detail: blend the 1 km red visible band over the IR result
 * where the sun is up (fading in from 2° to 12° elevation). Bright
 * reflectance becomes cloud in the same white style as the IR.
 * vhave[] marks pixels already filled by a newer time step. Returns
 * the number of daylit pixels still missing, or -1 on failure.
 */
static int blend_visible(const char *path, cairo_surface_t *dst, int x0,
                         int want_w, double lat0, double lat1,
                         double lon0, double lon1, double sub, time_t when,
                         unsigned char *vhave)
{
    int w, h, ch, x, y, dh, stride, missing = 0;
    unsigned char *px = stbi_load(path, &w, &h, &ch, 4), *d;
    pic_solar_position_t sp = pic_solar_position(when);
    double sd = sin(sp.declination), cd = cos(sp.declination);

    if (!px) return -1;
    dh = cairo_image_surface_get_height(dst);
    stride = cairo_image_surface_get_stride(dst);
    d = cairo_image_surface_get_data(dst);
    if (w > want_w || w < 2 || h < 2) {
        stbi_image_free(px);
        return -1;
    }
    if (w < want_w || h < dh) {
        unsigned char *up = upscale_rgba(px, w, h, want_w, dh);
        stbi_image_free(px);
        if (!up) return -1;
        px = up;
        w = want_w;
        h = dh;
    }
    for (y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(d + (size_t)y * stride);
        const unsigned char *s = px + (size_t)y * w * 4;
        double lat = (lat1 - (y + 0.5) / h * (lat1 - lat0)) * M_PI / 180;
        double sl = sin(lat), cl = cos(lat);
        for (x = 0; x < w; x++, s += 4) {
            double lon = lon0 + (x + 0.5) / w * (lon1 - lon0);
            double el = asin(sl * sd + cl * cd * cos(sp.gha + lon * M_PI / 180)) * 180 / M_PI;
            double day = (el - 2.0) / 10.0, c, fade, al;
            uint32_t p, q;
            unsigned v;
            if (day <= 0 || vhave[(size_t)y * w + x]) continue;
            if (s[3] < 128) { missing++; continue; }
            if (day > 1) day = 1;
            /* Same limb fade as the IR */
            c = acos(cl * cos((lon - sub) * M_PI / 180)) * 180 / M_PI;
            fade = c < 55 ? 1.0 : (72 - c) / 17.0;
            if (fade <= 0) continue;
            v = s[0];
            /* Ground reads ~40-110; cloud brighter. Soft ramp. */
            al = (v - 100.0) / 95.0;
            if (al < 0) al = 0;
            if (al > 1) al = 1;
            al *= 0.9 * fade;
            c = 205.0 + 50.0 * al;
            p = ((uint32_t)(al * 255) << 24) | ((uint32_t)(c * al) << 16) |
                ((uint32_t)(c * al) << 8) | (uint32_t)(c * al);
            q = row[x0 + x];
            if (day < 1.0) {        /* blend toward IR across twilight */
                uint32_t r = 0;
                int sh;
                for (sh = 0; sh < 32; sh += 8) {
                    double a = (p >> sh) & 255, b = (q >> sh) & 255;
                    r |= (uint32_t)(a * day + b * (1 - day) + 0.5) << sh;
                }
                p = r;
            }
            row[x0 + x] = p;
            vhave[(size_t)y * w + x] = 1;
        }
    }
    stbi_image_free(px);
    return missing;
}

static int satclouds_work(pic_feature_t *f)
{
    unsigned gen = pic_config.viewport_gen;
    double span = pic_config.view_span_lon, span_lat = pic_config.view_span_lat;
    double lon0, lon1, lat0, lat1;
    int sw, sh, enhanced, W, H, i, k, ok = 0;
    cairo_surface_t *img;

    (void)f;
    pthread_mutex_lock(&sc.mutex);
    sw = sc.screen_w > 0 ? sc.screen_w : 1920;
    sh = sc.screen_h > 0 ? sc.screen_h : 1080;
    enhanced = sc.enhanced;
    pthread_mutex_unlock(&sc.mutex);

    if (span < 0.0001) span = 360.0;
    if (span_lat < 0.0001) span_lat = 180.0;
    lon0 = pic_config.view_center_lon - span / 2;
    lon1 = pic_config.view_center_lon + span / 2;
    lat0 = pic_config.view_center_lat - span_lat / 2;
    lat1 = pic_config.view_center_lat + span_lat / 2;
    if (lat0 < -LAT_LIMIT) lat0 = -LAT_LIMIT;
    if (lat1 > LAT_LIMIT) lat1 = LAT_LIMIT;
    if (lat1 <= lat0) return 600;

    W = (int)lround(sw * (lon1 - lon0) / span);
    if (W > MAX_W) W = MAX_W;
    if (W < 360) W = 360;
    /* Keep the image's aspect equal to its on-screen aspect */
    H = (int)lround(W * ((lat1 - lat0) * sh / span_lat) /
                    ((lon1 - lon0) * sw / span));
    if (H < 16) H = 16;
    if (H > 1200) H = 1200;

    img = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(img);
        return 600;
    }
    cairo_surface_flush(img);

    for (i = 0; i < (int)(sizeof(bands) / sizeof(bands[0])); i++) {
        for (k = -1; k <= 1; k++) {
            double b0 = bands[i].lon0 + k * 360.0, b1 = bands[i].lon1 + k * 360.0;
            double i0 = lon0 > b0 ? lon0 : b0, i1 = lon1 < b1 ? lon1 : b1;
            int x0, x1, tries, expect = 0, got = -1, irw, irh;
            char url[600], when[24], used_when[24] = "";
            time_t used_t = 0;
            unsigned char *have;

            if (i1 - i0 < 0.2) continue;
            x0 = (int)lround((i0 - lon0) / (lon1 - lon0) * W);
            x1 = (int)lround((i1 - lon0) / (lon1 - lon0) * W);
            if (x1 - x0 < 2) continue;
            /* Band 13 has no more detail than ~IR_DEG_PX: ask for no
             * finer than that and let blit_png smooth it up */
            irw = x1 - x0;
            if ((i1 - i0) / irw < IR_DEG_PX) irw = (int)ceil((i1 - i0) / IR_DEG_PX);
            if (irw < 2) irw = 2;
            irh = (int)lround((double)H * irw / (x1 - x0));
            if (irh < 2) irh = 2;
            /*
             * GIBS's newest time steps are often still being ingested
             * and come back with whole tiles missing. Ask for a time
             * ~80 minutes old, then patch any gaps from up to five
             * earlier 10-minute steps.
             */
            have = calloc((size_t)(x1 - x0) * H, 1);
            if (!have) continue;
            for (tries = 0; tries < 6 && !pic_feature_interrupted(f) &&
                            pic_config.viewport_gen == gen; tries++) {
                time_t t = time(NULL) - 80 * 60 - tries * 600;
                struct tm tm;
                int g;
                t -= t % 600;
                gmtime_r(&t, &tm);
                strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:00Z", &tm);
                snprintf(url, sizeof(url), GIBS_FMT, bands[i].layer,
                         i0 - k * 360.0, lat0, i1 - k * 360.0, lat1,
                         irw, irh, when);
                g = pic_fetch_to_file(url, SC_TMP, 60) == 0 ?
                    blit_png(SC_TMP, img, x0, x1 - x0, enhanced, lat0, lat1,
                             i0, i1, bands[i].sub + k * 360.0, have, &expect) : -1;
                unlink(SC_TMP);
                if (g >= 0 && !used_when[0]) {
                    snprintf(used_when, sizeof(used_when), "%s", when);
                    used_t = t;
                }
                if (g > got) got = g;
                if (g >= 0 && got >= expect * 0.98) break;
            }
            free(have);
            /* Zoomed in: daylit areas from the 1 km visible band */
            if (got >= 0 && !enhanced && span <= VIS_MAX_SPAN &&
                !pic_feature_interrupted(f) && pic_config.viewport_gen == gen) {
                int vw = x1 - x0, vh = H, v = -1, vt;
                unsigned char *vhave = calloc((size_t)(x1 - x0) * H, 1);
                if ((i1 - i0) / vw < VIS_DEG_PX) {
                    vw = (int)ceil((i1 - i0) / VIS_DEG_PX);
                    vh = (int)lround((double)H * vw / (x1 - x0));
                }
                /* Patch tiles missing from the newest step from up to
                 * two older ones, like the IR */
                for (vt = 0; vhave && vt < 3 && v != 0 &&
                             !pic_feature_interrupted(f); vt++) {
                    time_t t = used_t - vt * 600;
                    struct tm tm;
                    gmtime_r(&t, &tm);
                    strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:00Z", &tm);
                    snprintf(url, sizeof(url), GIBS_FMT, bands[i].vis,
                             i0 - k * 360.0, lat0, i1 - k * 360.0, lat1,
                             vw, vh, when);
                    if (pic_fetch_to_file(url, SC_TMP, 60) == 0) {
                        int m = blend_visible(SC_TMP, img, x0, x1 - x0, lat0, lat1,
                                              i0, i1, bands[i].sub + k * 360.0,
                                              t, vhave);
                        if (m >= 0) v = m;
                    }
                    unlink(SC_TMP);
                }
                free(vhave);
                if (v > 0) printf("satclouds: visible band missing %d daylit px (IR used)\n", v);
            }
            if (got >= 0) ok++;
            if (got < 0)
                printf("satclouds: no imagery from %s\n", bands[i].layer);
            else if (got < expect * 0.98)
                printf("satclouds: %s %d%% covered\n", bands[i].layer,
                       expect ? got * 100 / expect : 0);
        }
    }
    cairo_surface_mark_dirty(img);

    if (pic_config.viewport_gen != gen) {
        /* The view changed mid-fetch: this image is for the old one */
        cairo_surface_destroy(img);
        return 1;
    }
    if (!ok) {
        cairo_surface_destroy(img);
        printf("satclouds: fetch failed\n");
        return 120;
    }

    pthread_mutex_lock(&sc.mutex);
    {
        cairo_surface_t *old = sc.image;
        sc.image = img;
        sc.lon0 = lon0; sc.lon1 = lon1; sc.lat0 = lat0; sc.lat1 = lat1;
        sc.span = span;
        sc.fetched = time(NULL);
        if (old) cairo_surface_destroy(old);
    }
    pthread_mutex_unlock(&sc.mutex);
    printf("satclouds: %dx%d, %d tiles\n", W, H, ok);

    /* Refetch sooner if the viewport moves (zoom/pan) */
    {
        int s;
        for (s = 0; s < 600; s++) {
            if (pic_config.viewport_gen != gen) return 5;
            if (pic_feature_interrupted(f)) return 1;
            sleep(1);
        }
    }
    return 1;
}

static void satclouds_reload(pic_feature_t *f)
{
    char buf[32] = "";
    int enh;
    pic_conf_str("SATCLOUD_STYLE", buf, sizeof(buf));
    enh = strcmp(buf, "enhanced") == 0;
    pthread_mutex_lock(&sc.mutex);
    if (enh != sc.enhanced) {
        sc.enhanced = enh;
        pic_feature_poke(f);
    }
    pthread_mutex_unlock(&sc.mutex);
}

void pic_layer_render_satclouds(cairo_t *cr, int width, int height,
                                time_t now, void *ud)
{
    double span, ppd, world, x0, y0, y1, w_px;
    int iw, ih, rep;

    (void)now; (void)ud;
    pthread_mutex_lock(&sc.mutex);
    sc.screen_w = width;
    sc.screen_h = height;
    if (!sc.image || pic_view_zoomed_out_since(sc.span)) {
        pthread_mutex_unlock(&sc.mutex);
        return;
    }
    span = pic_config.view_span_lon;
    if (span < 0.0001) span = 360.0;
    ppd = width / span;
    world = 360.0 * ppd;
    w_px = (sc.lon1 - sc.lon0) * ppd;
    x0 = pic_lon_to_x((sc.lon0 + sc.lon1) / 2.0, width) - w_px / 2.0;
    y0 = pic_lat_to_y(sc.lat1, height);
    y1 = pic_lat_to_y(sc.lat0, height);
    iw = cairo_image_surface_get_width(sc.image);
    ih = cairo_image_surface_get_height(sc.image);

    for (rep = -1; rep <= 1; rep++) {
        double xr = x0 + rep * world;
        if (xr > width || xr + w_px < 0) continue;
        cairo_save(cr);
        cairo_rectangle(cr, xr, y0, w_px, y1 - y0);
        cairo_clip(cr);
        cairo_translate(cr, xr, y0);
        cairo_scale(cr, w_px / iw, (y1 - y0) / ih);
        cairo_set_source_surface(cr, sc.image, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    pthread_mutex_unlock(&sc.mutex);
}

void pic_satclouds_free(void);
static void satclouds_stopped(pic_feature_t *f)
{
    (void)f;
    pic_satclouds_free();
}

static pic_feature_t feature_def = {
    .name = "satclouds", .layer = pic_layer_render_satclouds,
    .interval = 60, .extra_mb = 12,
    .reload = satclouds_reload, .work = satclouds_work, .needs_network = 1,
    .stopped = satclouds_stopped,
};

pic_feature_t *pic_feature_satclouds(void)
{
    static int inited;
    if (!inited) {
        pthread_mutex_init(&sc.mutex, NULL);
        inited = 1;
    }
    return &feature_def;
}

/* Release the image when the layer is turned off */
void pic_satclouds_free(void)
{
    pthread_mutex_lock(&sc.mutex);
    if (sc.image) cairo_surface_destroy(sc.image);
    sc.image = NULL;
    pthread_mutex_unlock(&sc.mutex);
}

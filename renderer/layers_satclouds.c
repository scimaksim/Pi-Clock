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
#include "localview.h"
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
    double ccl;                 /* centre latitude it was made for      */
    cairo_surface_t *alt;       /* the other view's image (auto-switch) */
    double alt_box[4];          /* lon0, lon1, lat0, lat1               */
    double alt_span, alt_ccl;
    time_t alt_fetched;
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
 * GIBS's band 13 palette uses grey twice: for anything warmer than
 * -19 °C (ground and low cloud) and again for -70..-79 °C, the coldest
 * storm tops, which sit inside the dark-red (-61..-70) and pink
 * (-80..-91) rings. RGB alone can't tell them apart, so mark grey
 * pixels near those colours as cold. Returns a w*h mask, or NULL.
 */
static int very_cold_rgb(const unsigned char *s)
{
    int r = s[0], g = s[1], b = s[2];
    if (s[3] < 128) return 0;
    if (r >= 20 && g < 50 && b < 50 && r > g + 15) return 1;          /* reds  */
    if (r >= 120 && b >= 120 && g + 50 < r && g + 50 < b) return 1;   /* pinks */
    return 0;
}

static unsigned char *cold_mask(const unsigned char *px, int w, int h)
{
    unsigned char *m = calloc((size_t)w * h, 1), *t = calloc((size_t)w * h, 1);
    int x, y, k, rad = w / 500 + 3, any = 0;

    if (!m || !t) { free(m); free(t); return NULL; }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            if (very_cold_rgb(px + ((size_t)y * w + x) * 4)) {
                m[(size_t)y * w + x] = 1;
                any = 1;
            }
    if (!any) { free(m); free(t); return NULL; }
    /* Dilate: rows, then columns (running counts) */
    for (y = 0; y < h; y++) {
        int c = 0;
        unsigned char *row = m + (size_t)y * w, *out = t + (size_t)y * w;
        for (k = 0; k < rad && k < w; k++) c += row[k];
        for (x = 0; x < w; x++) {
            if (x + rad < w) c += row[x + rad];
            if (x - rad - 1 >= 0) c -= row[x - rad - 1];
            out[x] = c > 0;
        }
    }
    for (x = 0; x < w; x++) {
        int c = 0;
        for (k = 0; k < rad && k < h; k++) c += t[(size_t)k * w + x];
        for (y = 0; y < h; y++) {
            if (y + rad < h) c += t[(size_t)(y + rad) * w + x];
            if (y - rad - 1 >= 0) c -= t[(size_t)(y - rad - 1) * w + x];
            m[(size_t)y * w + x] = c > 0;
        }
    }
    free(t);
    return m;
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
    unsigned char *px = stbi_load(path, &w, &h, &ch, 4), *d, *cold = NULL;

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
    cold = cold_mask(px, w, h);
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
                if (cold && cold[(size_t)y * w + x] &&
                    abs((int)s[0] - s[1]) < 25 && abs((int)s[1] - s[2]) < 25) {
                    /* -70..-79 °C grey inside a storm core */
                    unsigned v = enhanced ? s[0] : 250;
                    p = (229u << 24) | ((v * 229 / 255) << 16) |
                        ((v * 229 / 255) << 8) | (v * 229 / 255);
                } else
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
    free(cold);
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
                         unsigned char *vhave, int enhanced)
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
            /*
             * GIBS's visible band is gamma-stretched raw brightness, so
             * everything is darker under a low sun (morning, winter):
             * overcast reads ~120 at 25° elevation vs ~200+ at noon,
             * clear ground ~55-85. Normalise by sqrt(sin elevation)
             * before the ramp so low cloud isn't missed early and late.
             * Checked against airport METARs (OVC/BKN vs clear).
             */
            {
                double se = sin(el * M_PI / 180);
                double vn = v / sqrt(se < 0.1 ? 0.1 : se);
                al = (vn - 105.0) / 70.0;
            }
            if (al < 0) al = 0;
            if (al > 1) al = 1;
            al *= 0.9 * fade;
            c = 205.0 + 50.0 * al;
            p = ((uint32_t)(al * 255) << 24) | ((uint32_t)(c * al) << 16) |
                ((uint32_t)(c * al) << 8) | (uint32_t)(c * al);
            q = row[x0 + x];
            /* Enhanced style: keep the IR colour on cold (storm) tops,
             * use the visible detail everywhere else */
            if (enhanced) {
                int qr = (q >> 16) & 255, qg = (q >> 8) & 255, qb = q & 255;
                int mx = qr > qg ? (qr > qb ? qr : qb) : (qg > qb ? qg : qb);
                int mn = qr < qg ? (qr < qb ? qr : qb) : (qg < qb ? qg : qb);
                if (mx - mn > 30) { vhave[(size_t)y * w + x] = 1; continue; }
            }
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

/*
 * Fetch and composite the cloud image for viewport v (span lon/lat,
 * centre lon/lat). current: v is the view on screen, so give up if it
 * changes mid-fetch. Returns the image (box = lon0, lon1, lat0, lat1)
 * or NULL.
 */
static cairo_surface_t *build_view(pic_feature_t *f, unsigned gen, int current,
                                   const double v[4], double box[4])
{
    double span = v[0], span_lat = v[1];
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
    lon0 = v[2] - span / 2;
    lon1 = v[2] + span / 2;
    lat0 = v[3] - span_lat / 2;
    lat1 = v[3] + span_lat / 2;
    if (lat0 < -LAT_LIMIT) lat0 = -LAT_LIMIT;
    if (lat1 > LAT_LIMIT) lat1 = LAT_LIMIT;
    if (lat1 <= lat0) return NULL;

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
        return NULL;
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
                            (!current || pic_config.viewport_gen == gen); tries++) {
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
            if (got >= 0 && span <= VIS_MAX_SPAN &&
                !pic_feature_interrupted(f) && (!current || pic_config.viewport_gen == gen)) {
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
                                              t, vhave, enhanced);
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
    if ((current && pic_config.viewport_gen != gen) || !ok) {
        /* View changed mid-fetch, or nothing came back */
        if (!ok) printf("satclouds: fetch failed\n");
        cairo_surface_destroy(img);
        return NULL;
    }
    box[0] = lon0; box[1] = lon1; box[2] = lat0; box[3] = lat1;
    printf("satclouds: %dx%d, %d tiles%s\n", W, H, ok, current ? "" : " (next view)");
    return img;
}

/* Does an image made for span `span` centred (clon, clat) suit view v? */
static int slot_fits(double span, double clon, double clat, const double v[4])
{
    double dl = fabs(fmod(clon - v[2] + 540.0, 360.0) - 180.0);
    return span > 0 && fabs(span - v[0]) < v[0] * 0.01 &&
           dl < v[0] * 0.01 && fabs(clat - v[3]) < v[1] * 0.01;
}

/*
 * Two images: the view on screen (sc.image) and, when Map View
 * auto-switches, the other view (sc.alt), fetched ahead so the clouds
 * are there the moment the map zooms. The renderer draws whichever
 * fits the current view.
 */
#define SC_FRESH 540   /* reuse an image younger than 9 minutes */

static int satclouds_work(pic_feature_t *f)
{
    unsigned gen = pic_config.viewport_gen;
    double v[4], o[4], box[4];
    int have_cur = 0, have_other = 0;
    time_t now = time(NULL);
    cairo_surface_t *img;

    v[0] = pic_config.view_span_lon; v[1] = pic_config.view_span_lat;
    v[2] = pic_config.view_center_lon; v[3] = pic_config.view_center_lat;
    if (v[0] < 0.0001) v[0] = 360.0;
    if (v[1] < 0.0001) v[1] = 180.0;

    /* Already have a fresh image for the view on screen? (e.g. the one
     * prefetched before the switch) Make it the main one. */
    pthread_mutex_lock(&sc.mutex);
    if (sc.alt && now - sc.alt_fetched < SC_FRESH &&
        slot_fits(sc.alt_span, (sc.alt_box[0] + sc.alt_box[1]) / 2,
                  sc.alt_ccl, v)) {
        cairo_surface_t *t = sc.image;
        double tb[4] = { sc.lon0, sc.lon1, sc.lat0, sc.lat1 };
        double ts = sc.span, tcl = sc.ccl;
        time_t tf = sc.fetched;
        sc.image = sc.alt;
        sc.lon0 = sc.alt_box[0]; sc.lon1 = sc.alt_box[1];
        sc.lat0 = sc.alt_box[2]; sc.lat1 = sc.alt_box[3];
        sc.span = sc.alt_span; sc.ccl = sc.alt_ccl; sc.fetched = sc.alt_fetched;
        sc.alt = t;
        memcpy(sc.alt_box, tb, sizeof(tb));
        sc.alt_span = ts; sc.alt_ccl = tcl; sc.alt_fetched = tf;
    }
    have_cur = sc.image && now - sc.fetched < SC_FRESH &&
               slot_fits(sc.span, (sc.lon0 + sc.lon1) / 2, sc.ccl, v);
    pthread_mutex_unlock(&sc.mutex);

    if (!have_cur) {
        img = build_view(f, gen, 1, v, box);
        if (!img)
            return pic_config.viewport_gen != gen ? 1 : 120;
        pthread_mutex_lock(&sc.mutex);
        {
            cairo_surface_t *old = sc.image;
            sc.image = img;
            sc.lon0 = box[0]; sc.lon1 = box[1]; sc.lat0 = box[2]; sc.lat1 = box[3];
            sc.span = v[0];
            sc.ccl = v[3];
            sc.fetched = time(NULL);
            if (old) cairo_surface_destroy(old);
        }
        pthread_mutex_unlock(&sc.mutex);
    }

    /* Auto-switching: get the other view ready too */
    if (pic_view_other(o) && !pic_feature_interrupted(f) &&
        pic_config.viewport_gen == gen) {
        pthread_mutex_lock(&sc.mutex);
        have_other = sc.alt && time(NULL) - sc.alt_fetched < SC_FRESH &&
                     slot_fits(sc.alt_span, (sc.alt_box[0] + sc.alt_box[1]) / 2,
                               sc.alt_ccl, o);
        pthread_mutex_unlock(&sc.mutex);
        if (!have_other && (img = build_view(f, gen, 0, o, box)) != NULL) {
            pthread_mutex_lock(&sc.mutex);
            if (sc.alt) cairo_surface_destroy(sc.alt);
            sc.alt = img;
            memcpy(sc.alt_box, box, sizeof(box));
            sc.alt_span = o[0];
            sc.alt_ccl = o[3];
            sc.alt_fetched = time(NULL);
            pthread_mutex_unlock(&sc.mutex);
        }
    }

    /* Refetch when the images age, or sooner if the viewport moves */
    {
        int s;
        for (s = 0; s < SC_FRESH + 30; s++) {
            if (pic_config.viewport_gen != gen) return 1;
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
    double span, ppd, world, x0, y0, y1, w_px, v[4];
    double l0, l1, a0, a1;
    cairo_surface_t *img = NULL;
    int iw, ih, rep;

    (void)now; (void)ud;
    v[0] = pic_config.view_span_lon; v[1] = pic_config.view_span_lat;
    v[2] = pic_config.view_center_lon; v[3] = pic_config.view_center_lat;
    if (v[0] < 0.0001) v[0] = 360.0;
    if (v[1] < 0.0001) v[1] = 180.0;
    pthread_mutex_lock(&sc.mutex);
    sc.screen_w = width;
    sc.screen_h = height;
    /* Right after a global/local switch the prefetched image (alt)
     * is the one that fits */
    if (sc.alt && slot_fits(sc.alt_span, (sc.alt_box[0] + sc.alt_box[1]) / 2,
                            sc.alt_ccl, v) &&
        !(sc.image && slot_fits(sc.span, (sc.lon0 + sc.lon1) / 2, sc.ccl, v))) {
        img = sc.alt;
        l0 = sc.alt_box[0]; l1 = sc.alt_box[1]; a0 = sc.alt_box[2]; a1 = sc.alt_box[3];
    } else if (sc.image && !pic_view_image_stale(sc.span)) {
        img = sc.image;
        l0 = sc.lon0; l1 = sc.lon1; a0 = sc.lat0; a1 = sc.lat1;
    }
    if (!img) {
        pthread_mutex_unlock(&sc.mutex);
        return;
    }
    span = v[0];
    ppd = width / span;
    world = 360.0 * ppd;
    w_px = (l1 - l0) * ppd;
    x0 = pic_lon_to_x((l0 + l1) / 2.0, width) - w_px / 2.0;
    y0 = pic_lat_to_y(a1, height);
    y1 = pic_lat_to_y(a0, height);
    iw = cairo_image_surface_get_width(img);
    ih = cairo_image_surface_get_height(img);

    for (rep = -1; rep <= 1; rep++) {
        double xr = x0 + rep * world;
        if (xr > width || xr + w_px < 0) continue;
        cairo_save(cr);
        cairo_rectangle(cr, xr, y0, w_px, y1 - y0);
        cairo_clip(cr);
        cairo_translate(cr, xr, y0);
        cairo_scale(cr, w_px / iw, (y1 - y0) / ih);
        cairo_set_source_surface(cr, img, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    pthread_mutex_unlock(&sc.mutex);
}

/*
 * Legend for the enhanced style: GIBS's cloud-top temperature colours
 * (band 13 brightness temperature, °C). Colder = higher cloud tops =
 * stronger storms.
 */
static const struct { double t; unsigned char r, g, b; } sc_stops[] = {
    { -10.0, 215, 215, 220 }, { -19.4, 245, 245, 245 },
    { -19.6,   0, 255, 255 }, { -24.6,   0, 128, 185 }, { -29.6,   0,   0, 125 },
    { -35.0,   0, 128,  58 }, { -40.6,   0, 255,   0 }, { -50.6, 255, 255,   0 },
    { -60.6, 255,   0,   0 }, { -69.6,  26,   0,   0 }, { -70.6,   5,   5,   5 },
    { -78.6, 204, 204, 204 }, { -80.0, 230, 230, 230 }, { -80.6, 255, 127, 203 },
    { -90.6, 127,   0, 127 }, { -92.0, 255, 255, 255 },
};

static void satclouds_legend(cairo_t *cr, int width, int height, time_t now,
                             void *ud)
{
    double fs = height / 90.0, bw = fs * 34, bh = fs * 0.75, pad = fs * 0.6;
    double t0 = -10, t1 = -92, x0, y, tw, boxw;
    int enhanced, have, i, n = sizeof(sc_stops) / sizeof(sc_stops[0]);
    const char *title = "Cloud tops \xc2\xb0""C";
    cairo_text_extents_t e;

    (void)now; (void)ud;
    pthread_mutex_lock(&sc.mutex);
    enhanced = sc.enhanced;
    {
        double v[4] = { pic_config.view_span_lon, pic_config.view_span_lat,
                        pic_config.view_center_lon, pic_config.view_center_lat };
        if (v[0] < 0.0001) v[0] = 360.0;
        if (v[1] < 0.0001) v[1] = 180.0;
        /* Same choice as the layer: an image drawn for this view */
        have = (sc.image != NULL && !pic_view_image_stale(sc.span)) ||
               (sc.alt != NULL && slot_fits(sc.alt_span,
                                            (sc.alt_box[0] + sc.alt_box[1]) / 2,
                                            sc.alt_ccl, v));
    }
    pthread_mutex_unlock(&sc.mutex);
    if (!enhanced || !have) return;

    cairo_save(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    cairo_text_extents(cr, title, &e);
    tw = e.x_advance + fs * 0.8;
    boxw = tw + bw + pad * 2;
    x0 = (width - boxw) / 2.0;
    /* Next free row(s) of the bottom-centre legend stack, just above
     * the map key and any other notes */
    {
        double f72 = height / 72.0;
        double bottom = pic_legend_take(height, 1.5) + f72 * 0.45;
        y = bottom - (bh + fs * 1.75) + fs * 0.2;
    }

    cairo_new_path(cr);
    cairo_rectangle(cr, x0, y - fs * 0.2, boxw, bh + fs * 1.75);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.9, 0.92, 0.96, 0.95);
    cairo_move_to(cr, x0 + pad, y + bh * 0.9);
    cairo_show_text(cr, title);

    {
        cairo_pattern_t *g = cairo_pattern_create_linear(x0 + pad + tw, 0,
                                                         x0 + pad + tw + bw, 0);
        for (i = 0; i < n; i++)
            cairo_pattern_add_color_stop_rgb(g, (sc_stops[i].t - t0) / (t1 - t0),
                                             sc_stops[i].r / 255.0,
                                             sc_stops[i].g / 255.0,
                                             sc_stops[i].b / 255.0);
        cairo_rectangle(cr, x0 + pad + tw, y, bw, bh);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
    }
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, fs * 0.8);
    for (i = -20; i >= -90; i -= 10) {
        char lab[8];
        double x = x0 + pad + tw + (i - t0) / (t1 - t0) * bw;
        snprintf(lab, sizeof(lab), "%d", i);
        cairo_text_extents(cr, lab, &e);
        cairo_set_source_rgba(cr, 0.85, 0.87, 0.9, 0.95);
        cairo_move_to(cr, x - e.x_advance / 2, y + bh + fs * 1.05);
        cairo_show_text(cr, lab);
        cairo_new_path(cr);
        cairo_move_to(cr, x, y + bh);
        cairo_line_to(cr, x, y + bh + fs * 0.2);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

void pic_satclouds_free(void);
static void satclouds_stopped(pic_feature_t *f)
{
    (void)f;
    pic_satclouds_free();
}

static pic_feature_t feature_def = {
    .name = "satclouds", .layer = pic_layer_render_satclouds,
    /* Redraw often enough that a new image (e.g. after a global/local
     * switch) shows within seconds; drawing is a single scaled paint */
    .interval = 15, .extra_mb = 20,
    .legend = satclouds_legend,
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
    if (sc.alt) cairo_surface_destroy(sc.alt);
    sc.alt = NULL;
    pthread_mutex_unlock(&sc.mutex);
}

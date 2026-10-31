/*
 * layers_twilight.c - Sunset / twilight lines
 *
 * Draws the lines where the sun is at the horizon (sunrise/sunset),
 * -6° (civil), -12° (nautical) and -18° (astronomical twilight), as
 * circles around the sub-solar point. Zoomed in, each line is
 * labelled where it crosses home's latitude with the time it reaches
 * home, e.g. "Sunset 6:21 PM" on the evening side and "Sunrise
 * 7:36 AM" on the morning side.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "astro.h"
#include "config.h"
#include "solar.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define D2R (M_PI / 180.0)

static const struct {
    double h;
    const char *eve, *morn;
    double r, g, b, a, w;
} lines[] = {
    { -0.833, "Sunset", "Sunrise",       1.0, 0.75, 0.35, 0.85, 1.6 },
    { -6,     "Civil dusk", "Civil dawn", 0.9, 0.55, 0.6, 0.7, 1.2 },
    { -12,    "Nautical dusk", "Nautical dawn", 0.6, 0.5, 0.9, 0.6, 1.0 },
    { -18,    "Dark", "Astro dawn",       0.4, 0.4, 0.85, 0.55, 1.0 },
};

/* Point at angular distance d (deg) and bearing b from (lat0, lon0) */
static void dest(double lat0, double lon0, double d, double b,
                 double *lat, double *lon)
{
    double la = lat0 * D2R, dd = d * D2R, bb = b * D2R, l2;
    l2 = asin(sin(la) * cos(dd) + cos(la) * sin(dd) * cos(bb));
    *lon = lon0 + atan2(sin(bb) * sin(dd) * cos(la), cos(dd) - sin(la) * sin(l2)) / D2R;
    *lon = fmod(*lon + 540.0, 360.0) - 180.0;
    *lat = l2 / D2R;
}

void pic_layer_render_twilight(cairo_t *cr, int width, int height,
                               time_t now, void *ud)
{
    pic_solar_position_t sp = pic_solar_position(now);
    double sslat = sp.declination / D2R, sslon = -sp.gha / D2R;
    double wrap = pic_wrap_threshold_px(width), fs = height / 85.0;
    double qlat = pic_config.qth_lat, qlon = pic_config.qth_lon;
    int labels = pic_zoom_factor() >= 3, i, k;

    (void)ud;
    sslon = fmod(sslon + 540.0, 360.0) - 180.0;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

    for (i = 0; i < (int)(sizeof(lines) / sizeof(lines[0])); i++) {
        double d = 90.0 - lines[i].h, px = 0;
        double best[2][3] = { { 1e9, 0, 0 }, { 1e9, 0, 0 } };   /* |dlat|, lat, lon */
        double dsh[2] = { 6, 5 };

        cairo_new_path(cr);
        for (k = 0; k <= 720; k++) {
            double lat, lon, x, y, east;
            int side;
            dest(sslat, sslon, d, k * 0.5, &lat, &lon);
            x = pic_lon_to_x(lon, width);
            y = pic_lat_to_y(lat, height);
            if (k == 0 || fabs(x - px) > wrap) cairo_move_to(cr, x, y);
            else cairo_line_to(cr, x, y);
            px = x;
            /* Evening side: east of the sub-solar point */
            east = fmod(lon - sslon + 720.0, 360.0);
            side = east < 180.0 ? 0 : 1;
            if (fabs(lat - qlat) < best[side][0]) {
                best[side][0] = fabs(lat - qlat);
                best[side][1] = lat;
                best[side][2] = lon;
            }
        }
        cairo_set_dash(cr, i ? dsh : NULL, i ? 2 : 0, 0);
        cairo_set_source_rgba(cr, lines[i].r, lines[i].g, lines[i].b, lines[i].a);
        cairo_set_line_width(cr, lines[i].w * height / 1080.0 * 1.5);
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);

        if (!labels) continue;
        for (k = 0; k < 2; k++) {
            double x, y;
            time_t t;
            char hm[16], lab[48];
            if (best[k][0] > 2.0) continue;   /* line never reaches home's latitude */
            x = pic_lon_to_x(best[k][2], width);
            y = pic_lat_to_y(best[k][1], height);
            if (x < 0 || x > width - fs * 8 || y < fs || y > height - fs) continue;
            t = pic_astro_next_crossing(0, now, qlat, qlon, lines[i].h, k == 1, 30);
            if (!t) continue;
            pic_fmt_hm(hm, sizeof(hm), t);
            snprintf(lab, sizeof(lab), "%s %s", k == 0 ? lines[i].eve : lines[i].morn, hm);
            pic_draw_text_pill(cr, x + 4, y - fs * 0.6 - i * fs * 1.5, lab,
                               (lines[i].r + 1) / 2, (lines[i].g + 1) / 2,
                               (lines[i].b + 1) / 2, 0.55);
        }
    }
}

static pic_feature_t def = {
    .name = "twilight", .layer = pic_layer_render_twilight, .interval = 60,
};

pic_feature_t *pic_feature_twilight(void)
{
    return &def;
}

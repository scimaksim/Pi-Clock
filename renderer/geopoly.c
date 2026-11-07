/*
 * geopoly.c - Styled geographic polygon container and renderer
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "geopoly.h"
#include "config.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int pic_geopoly_init(pic_geopoly_t *g, int max_rings, int max_vert)
{
    memset(g, 0, sizeof(*g));
    g->rings = calloc((size_t)max_rings, sizeof(pic_ring_t));
    g->lat = calloc((size_t)max_vert, sizeof(float));
    g->lon = calloc((size_t)max_vert, sizeof(float));
    if (!g->rings || !g->lat || !g->lon) {
        pic_geopoly_free(g);
        return -1;
    }
    g->max_rings = max_rings;
    g->max_vert = max_vert;
    return 0;
}

void pic_geopoly_free(pic_geopoly_t *g)
{
    free(g->rings);
    free(g->lat);
    free(g->lon);
    memset(g, 0, sizeof(*g));
}

void pic_geopoly_clear(pic_geopoly_t *g)
{
    g->nrings = 0;
    g->nvert = 0;
}

void pic_geopoly_copy(pic_geopoly_t *dst, const pic_geopoly_t *src)
{
    int nr = src->nrings < dst->max_rings ? src->nrings : dst->max_rings;
    int nv = src->nvert < dst->max_vert ? src->nvert : dst->max_vert;
    memcpy(dst->rings, src->rings, (size_t)nr * sizeof(pic_ring_t));
    memcpy(dst->lat, src->lat, (size_t)nv * sizeof(float));
    memcpy(dst->lon, src->lon, (size_t)nv * sizeof(float));
    dst->nrings = nr;
    dst->nvert = nv;
}

int pic_geopoly_add_ring(pic_geopoly_t *g, const float *lat, const float *lon,
                         int n, const pic_ring_t *style)
{
    int room = g->max_vert - g->nvert, step = 1, k, w = 0;
    pic_ring_t *r;

    if (n < 3 || g->nrings >= g->max_rings || room < 4) return -1;
    /* Decimate long rings so one huge polygon can't exhaust the pool */
    while (n / step > room || n / step > 4000) step++;

    r = &g->rings[g->nrings];
    *r = *style;
    r->first = g->nvert;
    for (k = 0; k < n; k += step, w++) {
        g->lat[g->nvert + w] = lat[k];
        g->lon[g->nvert + w] = lon[k];
    }
    if (w < 3) return -1;
    r->n = w;
    g->nvert += w;
    g->nrings++;
    return 0;
}

static int add_json_ring(pic_geopoly_t *g, const pic_json_t *j, int ring,
                         const pic_ring_t *style)
{
    int n = pic_json_size(j, ring), k, room, step = 1, w = 0;
    pic_ring_t *r;

    room = g->max_vert - g->nvert;
    if (n < 3 || g->nrings >= g->max_rings || room < 4) return 0;
    while (n / step > room || n / step > 4000) step++;

    r = &g->rings[g->nrings];
    *r = *style;
    r->first = g->nvert;
    for (k = 0; k < n; k += step, w++) {
        int c = pic_json_at(j, ring, k);
        g->lon[g->nvert + w] = (float)pic_json_num(j, pic_json_at(j, c, 0), 0);
        g->lat[g->nvert + w] = (float)pic_json_num(j, pic_json_at(j, c, 1), 0);
    }
    if (w < 3) return 0;
    r->n = w;
    g->nvert += w;
    g->nrings++;
    return 1;
}

int pic_geopoly_add_geojson(pic_geopoly_t *g, const pic_json_t *j, int geom,
                            const pic_ring_t *style)
{
    int coords = pic_json_get(j, geom, "coordinates");
    int added = 0, p, np;

    if (!pic_json_is_array(j, coords)) return 0;
    if (pic_json_eq(j, pic_json_get(j, geom, "type"), "MultiPolygon")) {
        np = pic_json_size(j, coords);
        for (p = 0; p < np; p++)
            added += add_json_ring(g, j, pic_json_at(j, pic_json_at(j, coords, p), 0), style);
    } else if (pic_json_eq(j, pic_json_get(j, geom, "type"), "Polygon")) {
        added += add_json_ring(g, j, pic_json_at(j, coords, 0), style);
    }
    return added;
}

void pic_geopoly_draw(cairo_t *cr, const pic_geopoly_t *g, int width,
                      int height, int min_group, double line_w)
{
    double span = pic_config.view_span_lon;
    double ppd, world;
    int i, k, rep;

    if (span < 0.0001) span = 360.0;
    ppd = width / span;
    world = 360.0 * ppd;

    for (i = 0; i < g->nrings; i++) {
        const pic_ring_t *r = &g->rings[i];
        if (r->group < min_group) continue;

        for (rep = -1; rep <= 1; rep++) {
            double x = pic_lon_to_x(g->lon[r->first], width) + rep * world;
            double prev = g->lon[r->first], minx = x, maxx = x;

            cairo_new_path(cr);
            cairo_move_to(cr, x, pic_lat_to_y(g->lat[r->first], height));
            for (k = 1; k < r->n; k++) {
                double d = g->lon[r->first + k] - prev;
                while (d > 180) d -= 360;
                while (d < -180) d += 360;
                x += d * ppd;
                prev = g->lon[r->first + k];
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                cairo_line_to(cr, x, pic_lat_to_y(g->lat[r->first + k], height));
            }
            cairo_close_path(cr);
            if (maxx < 0 || minx > width) { cairo_new_path(cr); continue; }

            if (r->fill_a > 0) {
                cairo_set_source_rgba(cr, ((r->fill >> 16) & 255) / 255.0,
                                      ((r->fill >> 8) & 255) / 255.0,
                                      (r->fill & 255) / 255.0, r->fill_a);
                cairo_fill_preserve(cr);
            }
            if (r->stroke_a > 0) {
                cairo_set_source_rgba(cr, ((r->stroke >> 16) & 255) / 255.0,
                                      ((r->stroke >> 8) & 255) / 255.0,
                                      (r->stroke & 255) / 255.0, r->stroke_a);
                cairo_set_line_width(cr, line_w);
                cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
                cairo_stroke(cr);
            } else {
                cairo_new_path(cr);
            }
        }
    }
}

int pic_geopoly_max_group_at(const pic_geopoly_t *g, double lat, double lon)
{
    int i, k, best = -1;
    for (i = 0; i < g->nrings; i++) {
        const pic_ring_t *r = &g->rings[i];
        const float *la = g->lat + r->first, *lo = g->lon + r->first;
        int in = 0;
        if (r->group <= best) continue;
        for (k = 0; k < r->n; k++) {
            int j = (k + r->n - 1) % r->n;
            if (((la[k] > lat) != (la[j] > lat)) &&
                lon < (lo[j] - lo[k]) * (lat - la[k]) / (la[j] - la[k]) + lo[k])
                in = !in;
        }
        if (in) best = r->group;
    }
    return best;
}

unsigned pic_parse_hex_rgb(const char *s, unsigned def)
{
    char *end;
    unsigned long v;
    if (!s || s[0] != '#') return def;
    v = strtoul(s + 1, &end, 16);
    return (end - s == 7) ? (unsigned)v : def;
}

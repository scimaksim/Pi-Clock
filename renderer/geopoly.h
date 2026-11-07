/*
 * geopoly.h - Store and draw styled geographic polygons
 *
 * A small container shared by polygon-based overlays (SPC outlooks,
 * smoke plumes, ...): rings of lat/lon vertices in one vertex pool,
 * each with fill/stroke colours, plus helpers to read GeoJSON
 * Polygon/MultiPolygon geometry and to draw with antimeridian-safe
 * unwrapping.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_GEOPOLY_H
#define PIC_GEOPOLY_H

#include <cairo/cairo.h>
#include "json.h"

typedef struct {
    int      first, n;          /* Vertex range in the pool            */
    unsigned fill, stroke;      /* 0xRRGGBB                            */
    float    fill_a, stroke_a;  /* Opacities                           */
    int      group;             /* Caller-defined (e.g. risk level)    */
} pic_ring_t;

typedef struct {
    pic_ring_t *rings;
    int         nrings, max_rings;
    float      *lat, *lon;
    int         nvert, max_vert;
} pic_geopoly_t;

/* Allocate pools; returns 0 on success. */
int  pic_geopoly_init(pic_geopoly_t *g, int max_rings, int max_vert);
void pic_geopoly_free(pic_geopoly_t *g);
void pic_geopoly_clear(pic_geopoly_t *g);

/* Copy contents (pools must be at least as large). */
void pic_geopoly_copy(pic_geopoly_t *dst, const pic_geopoly_t *src);

/* Append one ring from parallel arrays (decimated to fit). */
int pic_geopoly_add_ring(pic_geopoly_t *g, const float *lat, const float *lon,
                         int n, const pic_ring_t *style);

/*
 * Append the outer rings of a GeoJSON geometry object (Polygon or
 * MultiPolygon) with the given style. Returns rings added.
 */
int pic_geopoly_add_geojson(pic_geopoly_t *g, const pic_json_t *j, int geom,
                            const pic_ring_t *style);

/* Draw rings whose group is >= min_group, in pool order. */
void pic_geopoly_draw(cairo_t *cr, const pic_geopoly_t *g, int width,
                      int height, int min_group, double line_w);

/* Highest ring group containing (lat, lon), or -1 if none. */
int pic_geopoly_max_group_at(const pic_geopoly_t *g, double lat, double lon);

/* "#RRGGBB" → 0xRRGGBB (def on failure) */
unsigned pic_parse_hex_rgb(const char *s, unsigned def);

#endif /* PIC_GEOPOLY_H */

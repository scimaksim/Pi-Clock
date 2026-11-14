/*
 * planetview.h - The Moon, Mars and Jupiter as full-screen geochrons
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PIC_PLANETVIEW_H
#define PIC_PLANETVIEW_H

#include <cairo/cairo.h>
#include <stddef.h>
#include <time.h>
#include "feature.h"

enum { PIC_WORLD_NONE, PIC_WORLD_MOON, PIC_WORLD_MARS, PIC_WORLD_JUPITER, PIC_WORLD_N };

int pic_world_by_name(const char *name);      /* "moon" -> PIC_WORLD_MOON, else 0 */
const char *pic_world_name(int world);         /* "Moon", "Mars", "Jupiter" */

/* Draw world w full screen at time now */
void pic_planetview_render(cairo_t *cr, int width, int height, int world, time_t now);

/* Horizons fetch thread (runs while any world is in the view cycle) */
pic_feature_t *pic_feature_worlds(void);

/* ---- pieces exposed for tests ------------------------------------ */

/* One sample of a sub-point series: time, east longitude and
 * latitude in the map's convention (deg), range from the body centre (km) */
typedef struct { double t, lon, lat, r; } pic_subpt_t;

/* Parse a Horizons observer table (CSV, quantities 14[,15][,20]...)
 * into sub-observer points: west-positive longitudes (Mars, Jupiter)
 * are turned east-positive, planetographic latitudes converted when
 * planetocentric is asked (flattening f); with sun != NULL quantity 15
 * (sub-solar point) is read too. Returns the number of rows. */
int pic_horizons_parse(const char *txt, int west, double f, int centric,
                       pic_subpt_t *obs, pic_subpt_t *sun, int max, double au_km);

/* Linear interpolation in a series (longitude wraps); 0 outside it */
int pic_subpt_at(const pic_subpt_t *s, int n, double t, pic_subpt_t *out);

/* Where the shadow of a body at sub-point (lon, lat, range r km) falls
 * on an ellipsoid (a, b km) lit from the sub-solar point (slon, slat);
 * all east-positive planetocentric degrees. 0 if it misses. */
int pic_shadow_point(double lon, double lat, double r, double slon, double slat,
                     double a, double b, double *olon, double *olat);

/* Jupiter: System III west longitude of a feature fixed in System II
 * at west longitude l2, at time t (IAU System II/III rotation) */
double pic_jupiter_sys2_to_sys3(double l2, double t);

/* Mars: Coordinated Mars Time (hours), Mars Sol Date, Ls (deg) */
double pic_mars_msd(double t);
double pic_mars_mtc(double t);
double pic_mars_ls(double t);
double pic_mars_eot(double t);     /* equation of time, degrees */

#endif /* PIC_PLANETVIEW_H */

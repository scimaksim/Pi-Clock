/* Satellite names -> the right silhouette; spent stages get their real
 * proportions; every shape draws without breaking cairo */
#include "check.h"
#include "../sat_icons.h"
#include <math.h>

int main(void)
{
    struct { const char *name; pic_sat_kind_t kind; } T[] = {
        { "ISS (ZARYA)", PIC_SAT_ISS }, { "CSS (TIANHE)", PIC_SAT_TIANGONG },
        { "HST", PIC_SAT_HUBBLE }, { "Hubble", PIC_SAT_HUBBLE },
        { "Hubble 6", PIC_SAT_CUBESAT },            /* Hubble Network, not HST */
        { "STARLINK-31415", PIC_SAT_STARLINK }, { "ONEWEB-0123", PIC_SAT_ONEWEB },
        { "IRIDIUM 180", PIC_SAT_IRIDIUM }, { "NOAA 20", PIC_SAT_EARTHOBS },
        { "Gaofen-3 02", PIC_SAT_EARTHOBS }, { "GLOBAL-21", PIC_SAT_EARTHOBS },
        { "GLOBALSTAR M087", PIC_SAT_GENERIC },     /* whole word only */
        { "Suomi NPP", PIC_SAT_EARTHOBS }, { "Suomi100", PIC_SAT_GENERIC },
        { "Sentinel-2B", PIC_SAT_EARTHOBS }, { "Sentinel-1C", PIC_SAT_SAR },
        { "Iceye-X44", PIC_SAT_SAR }, { "Capella-14", PIC_SAT_SAR },
        { "Umbra-07", PIC_SAT_SAR }, { "Tandem X", PIC_SAT_SAR },
        { "Spacemobile-004", PIC_SAT_BLUEBIRD }, { "Bluewalker 3", PIC_SAT_BLUEBIRD },
        { "TIANQI 21", PIC_SAT_CUBESAT }, { "Kineis-12", PIC_SAT_CUBESAT },
        { "SL-16 R/B", PIC_SAT_ROCKET_BODY }, { "CZ-4B R/B", PIC_SAT_ROCKET_BODY },
        { "OBJECT X", PIC_SAT_GENERIC },
    };
    struct { const char *name; double len, dia; } RB[] = {
        { "SL-16 R/B", 11.5, 3.9 }, { "SL-14 R/B", 2.7, 2.7 }, { "SL-8 R/B", 6.5, 2.4 },
        { "CZ-4B R/B", 4.9, 2.9 }, { "CZ-4 R/B", 0, 0 },     /* no partial match */
        { "Ariane 5 R/B", 4.7, 5.4 }, { "Ariane 40 R/B", 11.05, 2.6 },
        { "Idefix & Ariane 42P R/B", 11.05, 2.6 }, { "THOR Agena D R/B", 7.1, 1.5 },
        { "Breeze-KM R/B", 2.6, 2.5 }, { "QAEM 100 R/B", 0, 0 },
    };
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 120, 120);
    cairo_t *cr = cairo_create(s);
    int i, k;

    for (i = 0; i < (int)(sizeof(T) / sizeof(T[0])); i++)
        CHECK(pic_sat_kind(T[i].name) == T[i].kind, "%s: kind %d, want %d", T[i].name,
              pic_sat_kind(T[i].name), T[i].kind);
    CHECK(!pic_sat_is_landmark("Hubble 7"), "Hubble Network sat is not a landmark");

    for (i = 0; i < (int)(sizeof(RB) / sizeof(RB[0])); i++) {
        double l = 0, d = 0;
        int ok = pic_rocket_body_dims(RB[i].name, &l, &d);
        CHECK(ok == (RB[i].len > 0), "%s: known %d", RB[i].name, ok);
        CHECK(fabs(l - RB[i].len) < 1e-9 && fabs(d - RB[i].dia) < 1e-9,
              "%s: %.2f x %.2f m, want %.2f x %.2f", RB[i].name, l, d, RB[i].len, RB[i].dia);
    }

    /* Every kind, sunlit and in shadow, normal to degenerate sizes */
    for (k = 0; k < (int)(sizeof(T) / sizeof(T[0])); k++) {
        pic_draw_sat_icon(cr, T[k].name, 60, 60, 40, 0.7, 0.95, 0.95, 0.97);
        pic_draw_sat_icon(cr, T[k].name, 60, 60, 12, 0, 0.5, 0.5, 0.6);
        pic_draw_sat_icon(cr, T[k].name, 60, 60, 0, 0, 1, 1, 1);
        CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS, "%s: cairo %s", T[k].name,
              cairo_status_to_string(cairo_status(cr)));
    }
    /* A drawn SAR antenna lies along the direction of travel */
    CHECK(fabs(pic_sat_icon_angle("Iceye-X44", 1, 0)) < 1e-9, "SAR along track");
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    return CHECK_DONE("test_saticons");
}

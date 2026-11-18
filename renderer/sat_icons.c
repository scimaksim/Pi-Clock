/*
 * sat_icons.c - Small vector silhouettes for well-known spacecraft
 *
 * pic_draw_sat_icon() draws a recognisable outline for the ISS, the
 * Tiangong station and the Hubble Space Telescope, shapes for the
 * common families (Starlink, OneWeb, Iridium, weather/EO buses, radar
 * imagers, BlueBird, CubeSats), spent stages to their real proportions,
 * and a generic box-and-panels satellite for anything else. Shapes are simplified
 * so they still read at 15-30 px; a dark edge keeps them visible on
 * both day and night map tiles.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "sat_icons.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int name_is(const char *name, const char *want)
{
    size_t n = strlen(want), i;
    if (!name) return 0;
    for (i = 0; i < n; i++)
        if (tolower((unsigned char)name[i]) != want[i]) return 0;
    /* whole word: "ISS" but not "ISSUE" */
    return !isalnum((unsigned char)name[n]);
}

/* A word in name starting with want. A leading '=' asks for the whole
 * word ("=global" matches "GLOBAL-21" but not "GLOBALSTAR M087") */
static int has_word(const char *name, const char *want)
{
    size_t n, i, j;
    int whole = want[0] == '=';
    if (whole) want++;
    n = strlen(want);
    if (!name) return 0;
    for (i = 0; name[i]; i++) {
        if (i > 0 && isalnum((unsigned char)name[i - 1])) continue;
        for (j = 0; j < n && name[i + j]; j++)
            if (tolower((unsigned char)name[i + j]) != want[j]) break;
        if (j == n && (!whole || !isalnum((unsigned char)name[i + n]))) return 1;
    }
    return 0;
}

pic_sat_kind_t pic_sat_kind(const char *name)
{
    static const char *wx[] = { "noaa", "metop", "meteor", "fengyun", "fy-", "jpss",
                                "=suomi", "npp", "=aqua", "=terra", "landsat", "sentinel",
                                "gosat", "dmsp", "gcom", "ziyuan", "cbers", "resurs",
                                "kanopus", "worldview", "pleiades", "=spot", "jilin",
                                "gaofen", "cartosat", "haiyang", "=hj", "=zy", "zy-1",
                                "yunhai", "zhuhai", "=grus", "=global", "alsat", "=dmc",
                                "deimos", "=proba", "=tiros", "skysat", "kompsat",
                                "superview", "=beijing", "ningxia", "vrss", "rcm-", NULL };
    static const char *cube[] = { "tianqi", "lemur", "flock", "dove", "spire", "cubesat",
                                  "fossasat", "xw-", "cas-", "lilacsat", "funcube",
                                  "jy1sat", "cute", "qmr", "ky", "geoscan", "kineis",
                                  "connecta", "=cuby", "aprizesat", "s-net", "netsat",
                                  "lizziesat", "skykraft", "=lyra", "aac-ais-sat", "=uwe",
                                  "unisat", "aerocube", "tyvak", "=ghost", "=firefly",
                                  "ermis", "sateliot", "tomorrow-s", "wildfire",
                                  "=centauri", "=corvus", "saudisat", "=tiger", "zheda",
                                  "ten-koh", "iride-ms", "=otter", "lemur-2", "=bro",
                                  "skybee-a", "=hermes", "ionosfera-m", NULL };
    static const char *sar[] = { "iceye", "=capella", "=strix", "saocom", "skymed",
                                 "cosmo-skymed", "=umbra", "terrasar", "=tandem", "radarsat",
                                 "novasar", "=paz", "kompsat-5", "qps-sar", "hongtu",
                                 "piesat", "alos", "sentinel-1", "xsar", "neusar",
                                 "risat", "eos-04", "nisar", "biomass", NULL };
    int i;
    if (name_is(name, "iss") || name_is(name, "zarya"))
        return PIC_SAT_ISS;
    if (name_is(name, "tiangong") || name_is(name, "css") ||
        name_is(name, "tianhe"))
        return PIC_SAT_TIANGONG;
    if (name_is(name, "hubble") || name_is(name, "hst")) {
        /* the telescope, not Hubble Network's numbered smallsats */
        const char *p = name + 3;
        while (isalpha((unsigned char)*p)) p++;
        while (*p == ' ') p++;
        if (isdigit((unsigned char)*p)) return PIC_SAT_CUBESAT;
        return PIC_SAT_HUBBLE;
    }
    if (has_word(name, "r/b") || has_word(name, "rocket") || has_word(name, "akm") ||
        has_word(name, "sl-") || has_word(name, "cz-"))
        return PIC_SAT_ROCKET_BODY;
    if (has_word(name, "spacemobile") || has_word(name, "bluebird") ||
        has_word(name, "bluewalker"))
        return PIC_SAT_BLUEBIRD;
    for (i = 0; sar[i]; i++) if (has_word(name, sar[i])) return PIC_SAT_SAR;
    if (has_word(name, "starlink")) return PIC_SAT_STARLINK;
    if (has_word(name, "oneweb")) return PIC_SAT_ONEWEB;
    if (has_word(name, "iridium")) return PIC_SAT_IRIDIUM;
    for (i = 0; wx[i]; i++) if (has_word(name, wx[i])) return PIC_SAT_EARTHOBS;
    for (i = 0; cube[i]; i++) if (has_word(name, cube[i])) return PIC_SAT_CUBESAT;
    return PIC_SAT_GENERIC;
}

int pic_sat_is_landmark(const char *name)
{
    pic_sat_kind_t k = pic_sat_kind(name);
    return k == PIC_SAT_ISS || k == PIC_SAT_TIANGONG || k == PIC_SAT_HUBBLE;
}

double pic_sat_icon_angle(const char *name, double dx, double dy)
{
    pic_sat_kind_t k = pic_sat_kind(name);
    double a;
    if (dx == 0 && dy == 0) return 0;
    a = atan2(dy, dx);
    if (k == PIC_SAT_HUBBLE || k == PIC_SAT_ROCKET_BODY || k == PIC_SAT_SAR) return a;
    return a + M_PI / 2;
}

/* ── Drawing helpers ─────────────────────────────────────────────── */

#define EDGE_R 0.04
#define EDGE_G 0.05
#define EDGE_B 0.09

/* Box with a soft top-left highlight (metal / foil) and a dark edge */
static void box(cairo_t *cr, double x, double y, double w, double h,
                double r, double g, double b, double edge)
{
    cairo_pattern_t *pat = cairo_pattern_create_linear(x, y, x + w, y + h);
    cairo_pattern_add_color_stop_rgba(pat, 0, fmin(1, r * 1.15 + 0.08), fmin(1, g * 1.15 + 0.08),
                                      fmin(1, b * 1.15 + 0.08), 0.97);
    cairo_pattern_add_color_stop_rgba(pat, 1, r * 0.78, g * 0.78, b * 0.78, 0.97);
    cairo_new_path(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_set_source(cr, pat);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(pat);
    cairo_set_source_rgba(cr, EDGE_R, EDGE_G, EDGE_B, 0.8);
    cairo_set_line_width(cr, edge);
    cairo_stroke(cr);
}

/* Cylinder seen side-on, axis along x: shaded across its width */
static void cyl(cairo_t *cr, double x, double y, double w, double h,
                double r, double g, double b, double edge)
{
    cairo_pattern_t *pat = cairo_pattern_create_linear(x, y, x, y + h);
    cairo_pattern_add_color_stop_rgba(pat, 0.0, r * 0.7, g * 0.7, b * 0.7, 0.97);
    cairo_pattern_add_color_stop_rgba(pat, 0.35, fmin(1, r * 1.12 + 0.06), fmin(1, g * 1.12 + 0.06),
                                      fmin(1, b * 1.12 + 0.06), 0.97);
    cairo_pattern_add_color_stop_rgba(pat, 1.0, r * 0.6, g * 0.6, b * 0.6, 0.97);
    cairo_new_path(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_set_source(cr, pat);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(pat);
    cairo_set_source_rgba(cr, EDGE_R, EDGE_G, EDGE_B, 0.8);
    cairo_set_line_width(cr, edge);
    cairo_stroke(cr);
}

/* Solar array: cells (rows x cols) with a sheen, frame, dark edge */
static void array(cairo_t *cr, double x, double y, double w, double h,
                  double r, double g, double b, int rows, int cols, double edge)
{
    cairo_pattern_t *pat = cairo_pattern_create_linear(x, y, x + w * 0.6, y + h);
    int i;
    cairo_pattern_add_color_stop_rgba(pat, 0, fmin(1, r * 1.35 + 0.1), fmin(1, g * 1.35 + 0.1),
                                      fmin(1, b * 1.35 + 0.1), 0.97);
    cairo_pattern_add_color_stop_rgba(pat, 0.45, r, g, b, 0.97);
    cairo_pattern_add_color_stop_rgba(pat, 1, r * 0.7, g * 0.7, b * 0.7, 0.97);
    cairo_new_path(cr);
    cairo_rectangle(cr, x, y, w, h);
    cairo_set_source(cr, pat);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);
    /* cell grid */
    cairo_set_source_rgba(cr, EDGE_R, EDGE_G, EDGE_B, 0.35);
    cairo_set_line_width(cr, edge * 0.45);
    for (i = 1; i < cols; i++) {
        cairo_move_to(cr, x + w * i / cols, y);
        cairo_line_to(cr, x + w * i / cols, y + h);
    }
    for (i = 1; i < rows; i++) {
        cairo_move_to(cr, x, y + h * i / rows);
        cairo_line_to(cr, x + w, y + h * i / rows);
    }
    cairo_stroke(cr);
    cairo_rectangle(cr, x, y, w, h);
    /* Dark cells (Starlink, OneWeb) get a light frame so they show
     * against the night map, as the real panel edges catch the sun */
    if (0.3 * r + 0.59 * g + 0.11 * b < 0.25)
        cairo_set_source_rgba(cr, 0.62, 0.68, 0.82, 0.9);
    else
        cairo_set_source_rgba(cr, EDGE_R, EDGE_G, EDGE_B, 0.85);
    cairo_set_line_width(cr, edge);
    cairo_stroke(cr);
}

static void strut(cairo_t *cr, double x0, double y0, double x1, double y1, double edge)
{
    cairo_new_path(cr);
    cairo_move_to(cr, x0, y0);
    cairo_line_to(cr, x1, y1);
    cairo_set_source_rgba(cr, EDGE_R, EDGE_G, EDGE_B, 0.8);
    cairo_set_line_width(cr, edge * 2.2);
    cairo_stroke_preserve(cr);
    cairo_set_source_rgba(cr, 0.78, 0.8, 0.84, 0.95);
    cairo_set_line_width(cr, edge * 1.1);
    cairo_stroke(cr);
}

/* Array colours */
#define C_GOLD   0.86, 0.62, 0.24     /* ISS SAW blankets (amber-gold)    */
#define C_IROSA  0.16, 0.2, 0.36      /* roll-out arrays, deep blue-grey  */
#define C_BLUE   0.18, 0.3, 0.68      /* typical triple-junction cells    */
#define C_DARK   0.14, 0.18, 0.34     /* Starlink / OneWeb near-black     */
#define C_WHITE  0.93, 0.94, 0.96
#define C_SILVER 0.78, 0.8, 0.84
#define C_FOIL   0.85, 0.66, 0.3      /* gold MLI blanket                 */

/*
 * ISS, top view, truss across (x). Eight solar array wings in four pairs
 * at the truss ends (P6/P4 port, S4/S6 starboard), each ~1/3 of the
 * truss long, with the newer roll-out arrays (iROSA) laid over six of
 * them; the white radiator panels near the middle; the pressurised
 * modules crossing the truss (US lab and nodes forward, Kibo and
 * Columbus to the sides, the Russian segment with Zvezda's small
 * wings aft).
 */
static void draw_iss(cairo_t *cr, double s, double e)
{
    static const double ax[4] = { -3.85, -2.95, 2.05, 2.95 };
    int i;

    /* Pressurised modules (under the truss) */
    cyl(cr, -0.27 * s, -1.75 * s, 0.54 * s, 4.6 * s, C_WHITE, e);         /* along y */
    box(cr, -1.25 * s, -1.25 * s, 2.5 * s, 0.42 * s, C_WHITE, e);         /* Kibo-Columbus */
    array(cr, -1.45 * s, 2.15 * s, 1.05 * s, 0.32 * s, C_BLUE, 1, 4, e);  /* Zvezda wings */
    array(cr, 0.4 * s, 2.15 * s, 1.05 * s, 0.32 * s, C_BLUE, 1, 4, e);
    /* Radiators (P1/S1), white, both sides of the truss */
    for (i = -1; i <= 1; i += 2) {
        box(cr, (i < 0 ? -1.75 : 1.25) * s, -1.15 * s, 0.5 * s, 0.95 * s, C_WHITE, e * 0.8);
    }
    /* Main solar array wings */
    for (i = 0; i < 4; i++) {
        array(cr, ax[i] * s, -2.45 * s, 0.8 * s, 2.2 * s, C_GOLD, 8, 2, e);
        array(cr, ax[i] * s, 0.25 * s, 0.8 * s, 2.2 * s, C_GOLD, 8, 2, e);
        if (i != 1) {           /* iROSA over six wings */
            array(cr, (ax[i] + 0.16) * s, -1.85 * s, 0.48 * s, 1.35 * s, C_IROSA, 5, 1, e * 0.8);
            array(cr, (ax[i] + 0.16) * s, 0.55 * s, 0.48 * s, 1.35 * s, C_IROSA, 5, 1, e * 0.8);
        }
    }
    /* Truss on top */
    box(cr, -4.0 * s, -0.2 * s, 8.0 * s, 0.4 * s, C_SILVER, e);
}

/*
 * Tiangong, top view: core module Tianhe along y, the Wentian and
 * Mengtian labs forming the bar of the T, each with a very long pair of
 * flexible wings; Tianhe's own shorter wings; a docked Shenzhou.
 */
static void draw_tiangong(cairo_t *cr, double s, double e)
{
    array(cr, -4.0 * s, -1.85 * s, 3.3 * s, 0.7 * s, C_BLUE, 2, 10, e);
    array(cr, 0.7 * s, -1.85 * s, 3.3 * s, 0.7 * s, C_BLUE, 2, 10, e);
    cyl(cr, -2.6 * s, -1.0 * s, 5.2 * s, 0.7 * s, C_WHITE, e);           /* labs */
    array(cr, -1.95 * s, 0.75 * s, 1.5 * s, 0.5 * s, C_BLUE, 1, 5, e);   /* Tianhe wings */
    array(cr, 0.45 * s, 0.75 * s, 1.5 * s, 0.5 * s, C_BLUE, 1, 5, e);
    box(cr, -0.42 * s, -0.4 * s, 0.84 * s, 2.9 * s, C_WHITE, e);          /* Tianhe */
    box(cr, -0.3 * s, 2.5 * s, 0.6 * s, 0.9 * s, C_SILVER, e);            /* Shenzhou */
}

/*
 * Hubble, side view: aft shroud and the long tube along x, aperture
 * door open at the front, two narrow blue arrays on masts.
 */
static void draw_hubble(cairo_t *cr, double s, double e)
{
    strut(cr, -0.4 * s, -1.6 * s, -0.4 * s, 1.6 * s, e);
    array(cr, -2.3 * s, -2.2 * s, 3.8 * s, 0.7 * s, C_BLUE, 2, 12, e);
    array(cr, -2.3 * s, 1.5 * s, 3.8 * s, 0.7 * s, C_BLUE, 2, 12, e);
    cyl(cr, -3.2 * s, -1.0 * s, 1.9 * s, 2.0 * s, C_SILVER, e);
    cyl(cr, -1.3 * s, -0.85 * s, 4.1 * s, 1.7 * s, C_WHITE, e);
    box(cr, 2.65 * s, -0.7 * s, 0.25 * s, 1.4 * s, 0.1, 0.12, 0.18, e * 0.6);
    strut(cr, 2.8 * s, -0.85 * s, 3.9 * s, -1.6 * s, e * 1.4);
}

/*
 * Starlink v2 mini: a flat, dark chassis with two long arrays in line
 * (about 30 m tip to tip), seen from above.
 */
static void draw_starlink(cairo_t *cr, double s, double e)
{
    array(cr, -4.0 * s, -0.42 * s, 3.2 * s, 0.84 * s, C_DARK, 2, 8, e);
    array(cr, 0.8 * s, -0.42 * s, 3.2 * s, 0.84 * s, C_DARK, 2, 8, e);
    box(cr, -0.8 * s, -0.55 * s, 1.6 * s, 1.1 * s, 0.6, 0.62, 0.66, e);
    box(cr, -0.55 * s, -0.3 * s, 1.1 * s, 0.6 * s, 0.32, 0.34, 0.38, e * 0.7);  /* antennas */
}

/* OneWeb: compact box, two arrays on short booms */
static void draw_oneweb(cairo_t *cr, double s, double e)
{
    strut(cr, -1.0 * s, 0, 1.0 * s, 0, e);
    array(cr, -3.4 * s, -0.6 * s, 2.3 * s, 1.2 * s, C_DARK, 2, 4, e);
    array(cr, 1.1 * s, -0.6 * s, 2.3 * s, 1.2 * s, C_DARK, 2, 4, e);
    box(cr, -0.7 * s, -0.7 * s, 1.4 * s, 1.4 * s, C_FOIL, e);
}

/* Iridium NEXT: long prism body, two arrays angled off one end */
static void draw_iridium(cairo_t *cr, double s, double e)
{
    array(cr, -3.4 * s, -2.0 * s, 1.5 * s, 1.5 * s, C_BLUE, 3, 3, e);
    array(cr, -3.4 * s, 0.5 * s, 1.5 * s, 1.5 * s, C_BLUE, 3, 3, e);
    strut(cr, -1.9 * s, -1.2 * s, -1.4 * s, -0.3 * s, e);
    strut(cr, -1.9 * s, 1.2 * s, -1.4 * s, 0.3 * s, e);
    cyl(cr, -1.6 * s, -0.5 * s, 4.2 * s, 1.0 * s, C_SILVER, e);
    box(cr, 2.0 * s, -0.4 * s, 0.6 * s, 0.8 * s, 0.35, 0.37, 0.42, e);  /* antenna face */
}

/* Weather / Earth-observation (NOAA-20, MetOp, Meteor, Landsat...):
 * an instrument-laden box with one large array on a boom to one side */
static void draw_earthobs(cairo_t *cr, double s, double e)
{
    strut(cr, 0.9 * s, 0, 1.7 * s, 0, e);
    array(cr, 1.6 * s, -1.6 * s, 2.4 * s, 3.2 * s, C_BLUE, 4, 3, e);
    box(cr, -2.6 * s, -0.85 * s, 3.5 * s, 1.7 * s, C_FOIL, e);
    box(cr, -2.2 * s, -0.5 * s, 0.9 * s, 1.0 * s, C_WHITE, e * 0.7);     /* sensors */
    box(cr, -1.0 * s, -0.4 * s, 0.6 * s, 0.8 * s, 0.25, 0.27, 0.32, e * 0.7);
}

/* CubeSat (Tianqi, Lemur, amateur sats): a small cube with four
 * deployed panels around it */
static void draw_cubesat(cairo_t *cr, double s, double e)
{
    array(cr, -2.2 * s, -0.55 * s, 1.5 * s, 1.1 * s, C_BLUE, 2, 3, e);
    array(cr, 0.7 * s, -0.55 * s, 1.5 * s, 1.1 * s, C_BLUE, 2, 3, e);
    array(cr, -0.55 * s, -2.2 * s, 1.1 * s, 1.5 * s, C_BLUE, 3, 2, e);
    array(cr, -0.55 * s, 0.7 * s, 1.1 * s, 1.5 * s, C_BLUE, 3, 2, e);
    box(cr, -0.7 * s, -0.7 * s, 1.4 * s, 1.4 * s, 0.25, 0.27, 0.32, e);
    box(cr, -0.45 * s, -0.45 * s, 0.9 * s, 0.9 * s, C_FOIL, e * 0.7);
}

/*
 * Spent stages, by type: length and diameter in metres (nozzle
 * included), how much of the length is engine bell, the bell's exit
 * diameter as a share of the stage diameter, and whether the stage is
 * bare metal / foil (Centaur, Agena, Breeze) rather than painted white.
 */
typedef struct {
    const char *name;
    double len, dia, noz, exitr;
    int silver;
} pic_stage_t;

static const pic_stage_t STAGES[] = {
    { "=sl-16", 11.5, 3.9, 0.15, 0.55, 0 },   /* Zenit-2 second stage       */
    { "=sl-14", 2.7, 2.7, 0.30, 0.40, 0 },    /* Tsyklon-3 S5M third stage  */
    { "=sl-8", 6.5, 2.4, 0.20, 0.45, 0 },     /* Kosmos-3M second stage     */
    { "=sl-3", 3.1, 2.6, 0.30, 0.35, 0 },     /* Vostok Blok E              */
    { "=sl-4", 6.7, 2.66, 0.20, 0.45, 0 },    /* Soyuz Blok I               */
    { "=sl-6", 3.3, 2.6, 0.30, 0.40, 0 },     /* Molniya Blok L             */
    { "=sl-12", 6.3, 3.7, 0.15, 0.40, 0 },    /* Proton Blok DM             */
    { "=sl-19", 2.6, 2.5, 0.25, 0.40, 1 },    /* Rokot Breeze-KM            */
    { "breeze-km", 2.6, 2.5, 0.25, 0.40, 1 },
    { "breeze-m", 2.6, 4.0, 0.25, 0.30, 1 },
    { "fregat", 1.5, 3.35, 0.35, 0.25, 1 },
    { "=cz-2c", 7.5, 3.35, 0.20, 0.50, 0 },
    { "=cz-2d", 10.9, 3.35, 0.18, 0.50, 0 },
    { "=cz-3b", 12.4, 3.0, 0.15, 0.45, 0 },
    { "=cz-4b", 4.9, 2.9, 0.25, 0.45, 0 },
    { "=cz-4c", 4.9, 2.9, 0.25, 0.45, 0 },
    { "=cz-6", 3.0, 2.25, 0.30, 0.40, 0 },
    { "=cz-6a", 7.0, 3.35, 0.20, 0.45, 0 },
    { "=cz-8a", 12.0, 3.0, 0.15, 0.45, 0 },
    { "=cz-11", 1.8, 2.0, 0.30, 0.40, 0 },
    { "=h-2a", 9.2, 4.0, 0.25, 0.50, 0 },
    { "=h3", 11.0, 5.2, 0.22, 0.45, 0 },
    { "ariane 5", 4.7, 5.4, 0.40, 0.40, 0 },   /* ESC-A                      */
    { "ariane 6", 11.6, 5.4, 0.25, 0.40, 0 },  /* ULPM                       */
    { "=ariane", 11.05, 2.6, 0.20, 0.45, 0 },  /* Ariane 1-4 H10 third stage */
    { "=delta 2", 6.0, 2.4, 0.30, 0.55, 0 },
    { "=delta 1", 6.0, 1.5, 0.30, 0.55, 0 },
    { "=delta 4", 12.0, 4.0, 0.30, 0.55, 0 },
    { "=atlas", 12.7, 3.05, 0.15, 0.40, 1 },   /* Centaur                    */
    { "=titan", 9.0, 4.3, 0.15, 0.40, 1 },     /* Centaur T                  */
    { "=agena", 7.1, 1.5, 0.20, 0.55, 1 },
    { "=thor", 7.1, 1.5, 0.20, 0.55, 1 },      /* "THOR Agena D R/B"         */
    { "=pegasus", 1.3, 0.97, 0.35, 0.60, 0 },
    { "=gslv", 8.7, 2.8, 0.20, 0.45, 0 },
    { "=pslv", 3.0, 2.8, 0.30, 0.45, 0 },
    { "=falcon", 12.6, 3.66, 0.30, 0.90, 0 },  /* MVac's huge bell           */
    { "=electron", 2.4, 1.2, 0.30, 0.50, 0 },
    { NULL, 0, 0, 0, 0, 0 }
};

static const pic_stage_t *stage_of(const char *name)
{
    int i;
    for (i = 0; STAGES[i].name; i++)
        if (has_word(name, STAGES[i].name)) return &STAGES[i];
    return NULL;
}

int pic_rocket_body_dims(const char *name, double *len_m, double *dia_m)
{
    const pic_stage_t *st = stage_of(name);
    if (!st) return 0;
    if (len_m) *len_m = st->len;
    if (dia_m) *dia_m = st->dia;
    return 1;
}

/* Spent upper stage, axis along x, front (dome) to -x, bell to +x,
 * drawn to the stage's real proportions within about 6.6 x 3.9 units */
static void draw_rocket_body(cairo_t *cr, const char *name, double s, double e)
{
    static const pic_stage_t dflt = { "", 6.0, 2.5, 0.22, 0.45, 0 };
    const pic_stage_t *st = stage_of(name);
    double k, L, D, nl, bl, x0, rr, ex;
    double cr_, cg, cb;
    cairo_pattern_t *pat;

    if (!st) st = &dflt;
    k = fmin(6.6 / st->len, 3.9 / st->dia);
    L = st->len * k * s;
    D = st->dia * k * s;
    nl = L * st->noz;                 /* bell length */
    bl = L - nl;                      /* tank length */
    x0 = -L / 2;
    rr = fmin(D * 0.3, bl * 0.3);     /* forward dome rounding */
    ex = D * st->exitr / 2;           /* bell exit half-width */
    if (st->silver) { cr_ = 0.78; cg = 0.79; cb = 0.82; }
    else            { cr_ = 0.9;  cg = 0.9;  cb = 0.91; }

    /* engine bell */
    cairo_new_path(cr);
    cairo_move_to(cr, x0 + bl - e, -D * 0.12);
    cairo_curve_to(cr, x0 + bl + nl * 0.5, -D * 0.14, x0 + L - nl * 0.2, -ex, x0 + L, -ex);
    cairo_line_to(cr, x0 + L, ex);
    cairo_curve_to(cr, x0 + L - nl * 0.2, ex, x0 + bl + nl * 0.5, D * 0.14, x0 + bl - e, D * 0.12);
    cairo_close_path(cr);
    pat = cairo_pattern_create_linear(0, -ex, 0, ex);
    cairo_pattern_add_color_stop_rgba(pat, 0, 0.22, 0.21, 0.21, 0.97);
    cairo_pattern_add_color_stop_rgba(pat, 0.35, 0.5, 0.47, 0.45, 0.97);
    cairo_pattern_add_color_stop_rgba(pat, 1, 0.2, 0.19, 0.19, 0.97);
    cairo_set_source(cr, pat);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(pat);
    cairo_set_source_rgba(cr, EDGE_R, EDGE_G, EDGE_B, 0.8);
    cairo_set_line_width(cr, e);
    cairo_stroke(cr);

    /* tank: rounded forward dome, flat aft skirt */
    cairo_new_path(cr);
    cairo_move_to(cr, x0 + bl, -D / 2);
    cairo_line_to(cr, x0 + rr, -D / 2);
    cairo_curve_to(cr, x0 + rr * 0.3, -D / 2, x0, -D / 2 + rr * 0.7, x0, -D / 2 + rr);
    cairo_line_to(cr, x0, D / 2 - rr);
    cairo_curve_to(cr, x0, D / 2 - rr * 0.7, x0 + rr * 0.3, D / 2, x0 + rr, D / 2);
    cairo_line_to(cr, x0 + bl, D / 2);
    cairo_close_path(cr);
    pat = cairo_pattern_create_linear(0, -D / 2, 0, D / 2);
    cairo_pattern_add_color_stop_rgba(pat, 0.0, cr_ * 0.68, cg * 0.68, cb * 0.68, 0.97);
    cairo_pattern_add_color_stop_rgba(pat, 0.33, fmin(1, cr_ * 1.1), fmin(1, cg * 1.1), fmin(1, cb * 1.1), 0.97);
    cairo_pattern_add_color_stop_rgba(pat, 1.0, cr_ * 0.58, cg * 0.58, cb * 0.58, 0.97);
    cairo_set_source(cr, pat);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(pat);
    cairo_set_source_rgba(cr, EDGE_R, EDGE_G, EDGE_B, 0.8);
    cairo_set_line_width(cr, e);
    cairo_stroke(cr);

    /* tank seam (intertank / skirt ring) when there is room for it */
    if (bl > 2.5 * s) {
        double sx[2] = { x0 + bl * 0.55, x0 + bl - 0.25 * s };
        int i;
        cairo_set_source_rgba(cr, EDGE_R, EDGE_G, EDGE_B, 0.35);
        cairo_set_line_width(cr, e * 0.7);
        for (i = 0; i < 2; i++) {
            cairo_move_to(cr, sx[i], -D / 2);
            cairo_line_to(cr, sx[i], D / 2);
        }
        cairo_stroke(cr);
    }
}

/*
 * Radar imaging satellite (ICEYE, Umbra, Strix, SAOCOM, COSMO-SkyMed):
 * a long, flat phased-array antenna dominating a small bus, with one
 * small array. Capella is the exception: a big round mesh reflector.
 */
static void draw_sar(cairo_t *cr, const char *name, double s, double e)
{
    if (has_word(name, "=capella") || has_word(name, "=umbra")) {
        cairo_pattern_t *pat;
        array(cr, -3.9 * s, -0.55 * s, 1.6 * s, 1.1 * s, C_BLUE, 2, 3, e);
        strut(cr, -2.3 * s, 0, -1.0 * s, 0, e);
        cairo_new_path(cr);
        cairo_arc(cr, 1.0 * s, 0, 2.7 * s, 0, 2 * M_PI);
        pat = cairo_pattern_create_radial(0.4 * s, -0.6 * s, 0.2 * s, 1.0 * s, 0, 2.7 * s);
        cairo_pattern_add_color_stop_rgba(pat, 0, 0.93, 0.86, 0.62, 0.9);
        cairo_pattern_add_color_stop_rgba(pat, 1, 0.66, 0.56, 0.32, 0.9);
        cairo_set_source(cr, pat);
        cairo_fill_preserve(cr);
        cairo_pattern_destroy(pat);
        cairo_set_source_rgba(cr, EDGE_R, EDGE_G, EDGE_B, 0.8);
        cairo_set_line_width(cr, e);
        cairo_stroke(cr);
        {   /* mesh ribs */
            int i;
            cairo_set_source_rgba(cr, EDGE_R, EDGE_G, EDGE_B, 0.3);
            cairo_set_line_width(cr, e * 0.5);
            for (i = 0; i < 8; i++) {
                double a = i * M_PI / 4;
                cairo_move_to(cr, 1.0 * s, 0);
                cairo_line_to(cr, 1.0 * s + 2.7 * s * cos(a), 2.7 * s * sin(a));
            }
            cairo_stroke(cr);
        }
        box(cr, 0.45 * s, -0.55 * s, 1.1 * s, 1.1 * s, C_FOIL, e);
        return;
    }
    array(cr, -4.0 * s, -0.8 * s, 8.0 * s, 1.6 * s, 0.6, 0.6, 0.6, 2, 12, e);
    box(cr, -0.75 * s, -1.6 * s, 1.5 * s, 0.85 * s, C_FOIL, e);          /* bus  */
    array(cr, -0.7 * s, -2.7 * s, 1.4 * s, 1.1 * s, C_BLUE, 2, 2, e);   /* cells */
}

/*
 * AST SpaceMobile BlueWalker / BlueBird: a flat phased array of
 * hexagonal-ish tiles, 64-220 m^2, larger than anything else in LEO
 * except the ISS, with the small bus at its centre.
 */
static void draw_bluebird(cairo_t *cr, double s, double e)
{
    array(cr, -3.0 * s, -3.0 * s, 6.0 * s, 6.0 * s, C_DARK, 8, 8, e);
    box(cr, -0.55 * s, -0.55 * s, 1.1 * s, 1.1 * s, C_FOIL, e);
}

static void draw_generic(cairo_t *cr, double s, double e)
{
    array(cr, -3.4 * s, -0.75 * s, 2.2 * s, 1.5 * s, C_BLUE, 2, 4, e);
    array(cr, 1.2 * s, -0.75 * s, 2.2 * s, 1.5 * s, C_BLUE, 2, 4, e);
    strut(cr, -1.2 * s, 0, 1.2 * s, 0, e);
    box(cr, -1.0 * s, -0.85 * s, 2.0 * s, 1.7 * s, C_FOIL, e);
    strut(cr, 0, -0.85 * s, 0, -1.6 * s, e);                            /* antenna */
}

void pic_draw_sat_icon(cairo_t *cr, const char *name, double x, double y,
                       double size, double angle, double r, double g, double b)
{
    /* size = full icon width; the shapes span about 8 units */
    double s = size / 8.0, e = size / 48.0;
    /* r/g/b carry the marker state: dimmer when the satellite is in
     * Earth's shadow (grey), full colour when sunlit */
    double dim = (r + g + b) / 3.0 < 0.7 ? 0.55 : 1.0;

    if (e < 0.5) e = 0.5;
    cairo_save(cr);
    cairo_new_path(cr);
    cairo_translate(cr, x, y);
    cairo_rotate(cr, angle);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
    if (dim < 1.0) cairo_push_group(cr);
    switch (pic_sat_kind(name)) {
    case PIC_SAT_ISS:         draw_iss(cr, s, e); break;
    case PIC_SAT_TIANGONG:    draw_tiangong(cr, s, e); break;
    case PIC_SAT_HUBBLE:      draw_hubble(cr, s, e); break;
    case PIC_SAT_STARLINK:    draw_starlink(cr, s, e); break;
    case PIC_SAT_ONEWEB:      draw_oneweb(cr, s, e); break;
    case PIC_SAT_IRIDIUM:     draw_iridium(cr, s, e); break;
    case PIC_SAT_EARTHOBS:    draw_earthobs(cr, s, e); break;
    case PIC_SAT_CUBESAT:     draw_cubesat(cr, s * 1.2, e); break;
    case PIC_SAT_ROCKET_BODY: draw_rocket_body(cr, name, s, e); break;
    case PIC_SAT_SAR:         draw_sar(cr, name, s, e); break;
    case PIC_SAT_BLUEBIRD:    draw_bluebird(cr, s, e); break;
    default:                  draw_generic(cr, s, e); break;
    }
    if (dim < 1.0) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, dim);
    }
    cairo_restore(cr);
    cairo_new_path(cr);
}

/*
 * Colour for a satellite's name and trail from its marker state (the
 * r/g/b the overhead layer picks: gold = visible from home now, white =
 * sunlit, grey = in Earth's shadow). Aqua and blue stand out from the
 * warm city lights of the night map and the tan land of the day map.
 * Returns the alpha.
 */
double pic_sat_label_rgb(double r, double g, double b,
                         double *lr, double *lg, double *lb)
{
    if (r > 0.9 && b < 0.5) {            /* look up: you can see it */
        *lr = 0.30; *lg = 1.00; *lb = 0.92;
        return 1.0;
    }
    if ((r + g + b) / 3.0 >= 0.7) {      /* sunlit */
        *lr = 0.52; *lg = 0.80; *lb = 1.00;
        return 0.95;
    }
    /* In Earth's shadow: muted rose, which stays distinct from both
     * blues (and from city lights) with red-green or blue-yellow
     * colour blindness; trails are dashed as well */
    *lr = 0.86; *lg = 0.52; *lb = 0.66;
    return 0.9;
}

/*
 * Crew badge beside a station icon: a small person pictogram and the
 * number aboard, in a dark pill. (x, y) is the badge's left-centre;
 * h its height in px. Returns the badge width.
 */
double pic_draw_crew_badge(cairo_t *cr, double x, double y, double h, int aboard,
                           double r, double g, double b)
{
    char num[8];
    cairo_text_extents_t e;
    double ph = h * 0.72, hr = ph * 0.17, pad = h * 0.22, w, px, py = y;

    if (aboard < 0) return 0;
    snprintf(num, sizeof(num), "%d", aboard);
    cairo_save(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, h * 0.78);
    cairo_text_extents(cr, num, &e);
    w = pad + ph * 0.55 + pad * 0.6 + e.x_advance + pad;
    /* pill */
    cairo_new_path(cr);
    cairo_arc(cr, x + h / 2, y, h / 2, M_PI / 2, 3 * M_PI / 2);
    cairo_arc(cr, x + w - h / 2, y, h / 2, -M_PI / 2, M_PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0, 0, 0.03, 0.72);
    cairo_fill(cr);
    /* person: head, shoulders/body */
    px = x + pad + ph * 0.27;
    cairo_set_source_rgba(cr, r, g, b, 0.97);
    cairo_new_path(cr);
    cairo_arc(cr, px, py - ph * 0.30, hr, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_new_path(cr);
    cairo_move_to(cr, px - ph * 0.26, py + ph * 0.38);
    cairo_curve_to(cr, px - ph * 0.26, py - ph * 0.02, px - ph * 0.14, py - ph * 0.10, px, py - ph * 0.10);
    cairo_curve_to(cr, px + ph * 0.14, py - ph * 0.10, px + ph * 0.26, py - ph * 0.02, px + ph * 0.26, py + ph * 0.38);
    cairo_close_path(cr);
    cairo_fill(cr);
    /* number */
    cairo_move_to(cr, x + pad + ph * 0.55 + pad * 0.6, py + e.height / 2 - (e.height + e.y_bearing));
    cairo_show_text(cr, num);
    cairo_restore(cr);
    cairo_new_path(cr);
    return w;
}

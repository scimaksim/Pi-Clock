/*
 * craft_icons.h - Landers, rovers and orbiters of the other worlds
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PIC_CRAFT_ICONS_H
#define PIC_CRAFT_ICONS_H

#include <cairo/cairo.h>

typedef enum {
    /* Moon, on the surface */
    PIC_CRAFT_APOLLO_LM,       /* descent stage left behind              */
    PIC_CRAFT_APOLLO_LM_LRV,   /* ... with the Lunar Roving Vehicle      */
    PIC_CRAFT_LUNOKHOD,
    PIC_CRAFT_LUNA_SR,         /* Luna 16/20/24 sample return            */
    PIC_CRAFT_LUNA_ALS,        /* Luna 9/13 petal capsule                */
    PIC_CRAFT_SURVEYOR,
    PIC_CRAFT_CHANGE_LANDER,   /* Chang'e 3/4/5/6                        */
    PIC_CRAFT_YUTU,            /* Yutu / Yutu-2 rover                    */
    PIC_CRAFT_VIKRAM,          /* Chandrayaan-3 lander                   */
    PIC_CRAFT_PRAGYAN,
    PIC_CRAFT_SLIM,            /* landed nose-down                        */
    PIC_CRAFT_NOVA_C,          /* Intuitive Machines (on its side)       */
    PIC_CRAFT_BLUE_GHOST,
    /* Mars, on the surface */
    PIC_CRAFT_VIKING_LANDER,
    PIC_CRAFT_MARS3,           /* Soviet Mars 2/3 lander                  */
    PIC_CRAFT_PATHFINDER,      /* lander with petals                      */
    PIC_CRAFT_SOJOURNER,
    PIC_CRAFT_MER,             /* Spirit, Opportunity                     */
    PIC_CRAFT_MSL,             /* Curiosity, Perseverance                 */
    PIC_CRAFT_INGENUITY,
    PIC_CRAFT_PHOENIX,         /* Phoenix, InSight: round solar arrays    */
    PIC_CRAFT_ZHURONG,
    PIC_CRAFT_BEAGLE2,
    /* anywhere */
    PIC_CRAFT_CRASH,           /* impact / crash site                     */
    /* orbiters */
    PIC_CRAFT_LRO,
    PIC_CRAFT_CHANDRAYAAN2,
    PIC_CRAFT_DANURI,
    PIC_CRAFT_MRO,
    PIC_CRAFT_ODYSSEY,
    PIC_CRAFT_MAVEN,
    PIC_CRAFT_MEX,             /* Mars Express                            */
    PIC_CRAFT_TGO,
    PIC_CRAFT_TIANWEN1,
    PIC_CRAFT_HOPE,
    PIC_CRAFT_JUNO,
    PIC_CRAFT_N
} pic_craft_t;

/* Draw craft kind k centred at (x, y), about `size` px across. dim
 * (0..1) greys it for missions that have ended. */
void pic_draw_craft(cairo_t *cr, pic_craft_t k, double x, double y, double size, double dim);

/* Name of a kind (for the icon sheet / tests) */
const char *pic_craft_name(pic_craft_t k);

/* Moons as small bodies: Phobos/Deimos (irregular), Io, Europa,
 * Ganymede, Callisto (coloured discs) */
enum { PIC_MOON_PHOBOS, PIC_MOON_DEIMOS, PIC_MOON_IO, PIC_MOON_EUROPA,
       PIC_MOON_GANYMEDE, PIC_MOON_CALLISTO };
void pic_draw_small_moon(cairo_t *cr, int which, double x, double y, double r);

#endif /* PIC_CRAFT_ICONS_H */

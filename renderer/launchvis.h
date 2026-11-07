/*
 * launchvis.h - Will a launch be visible from home?
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PIC_LAUNCHVIS_H
#define PIC_LAUNCHVIS_H

#include <time.h>

/* Minimum elevation (degrees) that counts as visible over trees and
 * houses */
#define PIC_LV_MIN_EL 5.0
/* Seconds of powered flight modelled (to second-stage cut-off) */
#define PIC_LV_T_END  540

typedef struct {
    int    known;         /* a trajectory azimuth could be worked out   */
    int    visible;       /* above PIC_LV_MIN_EL while home is dark     */
    int    sunlit;        /* plume lit by sunlight against a dark sky   */
    int    daytime;       /* above the horizon, but home sky too bright */
    double az_launch;     /* trajectory azimuth from the pad (degrees)  */
    int    t_start, t_end;/* seconds after liftoff above PIC_LV_MIN_EL  */
    double max_el;        /* highest elevation from home (degrees)      */
    double az_start, az_end; /* azimuths from home at t_start / t_end   */
} pic_launchvis_t;

/*
 * Trajectory azimuth (degrees from north) for a launch from the pad at
 * (lat, lon), from its target orbit (LL2 orbit abbreviation such as
 * "LEO", "GTO", "SSO"), mission name and rocket. 0 if it can't be
 * worked out.
 */
int pic_launchvis_azimuth(double lat, double lon, const char *orbit,
                          const char *mission, const char *rocket,
                          double *az);

/* Rocket position t seconds after liftoff along azimuth az */
void pic_launchvis_position(double lat, double lon, double az, double t,
                            double *rlat, double *rlon, double *alt_km);

/* Elevation / azimuth (degrees) of a point at alt_km above (rlat, rlon)
 * seen from (hlat, hlon) */
double pic_launchvis_look(double hlat, double hlon, double rlat, double rlon,
                          double alt_km, double *az);

/*
 * Predict visibility from home (hlat, hlon) of a launch at time net from
 * the pad at (lat, lon). Returns 1 when the trajectory is known.
 */
int pic_launchvis_predict(double lat, double lon, const char *orbit,
                          const char *mission, const char *rocket,
                          time_t net, double hlat, double hlon,
                          pic_launchvis_t *out);

/* "SSE" etc. */
const char *pic_launchvis_compass(double az);

/*
 * Learning from past launches: the family a launch belongs to
 * ("Falcon 9|Starlink Group 10", "Falcon 9|Crew", "Vulcan|USSF"), and a
 * table of inclinations actually flown by each family, kept in a file.
 * pic_launchvis_predict() uses a learned inclination in place of the
 * built-in guess for that family.
 */
void   pic_launchvis_family(const char *rocket, const char *mission, char *out, size_t n);
void   pic_launchvis_learn(const char *family, double inclination, int objects);
int    pic_launchvis_learned(const char *family, double *inclination);
int    pic_launchvis_load(const char *path);
int    pic_launchvis_save(const char *path);

#endif /* PIC_LAUNCHVIS_H */

/*
 * features_all.h - Constructors for the pic_feature_t overlays
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PIC_FEATURES_ALL_H
#define PIC_FEATURES_ALL_H

#include "feature.h"

pic_feature_t *pic_feature_spc(void);       /* SPC convective outlook  */
pic_feature_t *pic_feature_launches(void);  /* Rocket launches         */
pic_feature_t *pic_feature_dsn(void);       /* Deep Space Network      */
pic_feature_t *pic_feature_passes(void);    /* Visible ISS/CSS passes  */
pic_feature_t *pic_feature_satclouds(void); /* GOES/Himawari IR clouds */
pic_feature_t *pic_feature_fires(void);     /* Fire hotspots + smoke   */
pic_feature_t *pic_feature_tides(void);     /* NOAA tide predictions   */
pic_feature_t *pic_feature_planets(void);   /* Planets + sky events    */
pic_feature_t *pic_feature_ships(void);     /* AIS vessel positions    */

#endif

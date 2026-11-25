/*
 * spacedata.h - Space-Track data cache for the space-object layers
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PIC_SPACEDATA_H
#define PIC_SPACEDATA_H
#include <stddef.h>
#include "feature.h"

#define SPACEDATA_DIR "/data/cache/pi-clock-spacetrack"

pic_feature_t *pic_feature_spacedata(void);

/* Append each "key":"digits" value in txt to a comma list, no repeats */
void pic_spacedata_ids(const char *txt, const char *key, char *list, size_t n);
#endif

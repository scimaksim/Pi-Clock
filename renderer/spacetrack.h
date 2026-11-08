/*
 * spacetrack.h - Space-Track.org as a back-up source of orbital data
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PIC_SPACETRACK_H
#define PIC_SPACETRACK_H

/* Root-only file written by the dashboard: identity, then password */
#define SPACETRACK_CRED "/data/etc/pi-clock-spacetrack.cred"

/* Query paths (after https://www.space-track.org), TLE replies */
#define ST_Q_LAST30 "/basicspacedata/query/class/gp/LAUNCH_DATE/%3Enow-30/DECAY_DATE/null-val" \
                    "/orderby/NORAD_CAT_ID/format/3le"
#define ST_Q_CATNR  "/basicspacedata/query/class/gp/NORAD_CAT_ID/%d/format/3le"
#define ST_Q_INTDES "/basicspacedata/query/class/gp/OBJECT_ID/~~%s/DECAY_DATE/null-val/format/3le"
#define ST_Q_IDS    "/basicspacedata/query/class/gp/NORAD_CAT_ID/%s/format/3le"

/* 1 when an account is saved */
int pic_spacetrack_configured(void);

/*
 * Run one query and save the TLEs (CelesTrak layout) to path.
 * 0 on success; -1 when there is no account, the hourly cap or a
 * failed-sign-in pause holds it back, or nothing came back.
 */
int pic_spacetrack_fetch(const char *query, const char *path);

#endif

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

/*
 * A PC on the network that fetches orbits for the whole house
 * (ORBIT_SHARE_URL, e.g. http://192.168.1.251:8080, a Pi-Dash server).
 * When set, every Space-Track query goes to that PC instead, and
 * CelesTrak is never asked: one machine talks to the providers, so
 * nothing in the house can get its address blocked for asking too
 * often. Writes the address (no trailing /) to out; 1 when set.
 */
#include <stddef.h>
int pic_orbit_share(char *out, size_t n);

/* When the saved account last changed (0: none). Features compare it
 * to retry straight away when an account is added or changed. */
#include <time.h>
time_t pic_spacetrack_stamp(void);

/* 1 when an account is saved */
int pic_spacetrack_configured(void);

/*
 * Run one query and save the TLEs (CelesTrak layout) to path.
 * 0 on success; -1 when there is no account, the hourly cap or a
 * failed-sign-in pause holds it back, or nothing came back.
 */
int pic_spacetrack_fetch(const char *query, const char *path);

/*
 * Run several queries in one session (one sign-in) and save each
 * reply as is (JSON etc.). Returns how many succeeded, -1 if the
 * session could not start (no account, cap, pause, sign-in).
 */
int pic_spacetrack_fetch_raw(const char *const *query, const char *const *path, int n);

/* For tests: replace the HTTPS request (NULL restores it). Returns the
 * HTTP status; writes the body to `out`, Set-Cookie values to `cookies`. */
#include <stdio.h>
typedef int (*pic_spacetrack_transport)(const char *method, const char *path,
                                        const char *cookie, const char *body,
                                        FILE *out, char *cookies, size_t cn);
void pic_spacetrack_set_transport(pic_spacetrack_transport fn);

#endif

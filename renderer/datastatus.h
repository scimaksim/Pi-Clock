/*
 * datastatus.h - Health of every data source, for the dashboard
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PIC_DATASTATUS_H
#define PIC_DATASTATUS_H

#define PIC_STATUS_PATH "/tmp/pi-clock-status.json"

/* Name of the feature whose thread is running (set by the feature
 * runner); recorded as the user of each source */
extern __thread const char *pic_status_feature;

/* Thread-local, for fetch.c: while set, an HTTP error answer (404,
 * 502...) counts as the source being up (expected missing items) */
extern __thread int pic_status_http_ok;

/*
 * Record one attempt to reach a source. source is a host name or a URL
 * (the host is taken from it); detail describes a failure. The status
 * file is rewritten at most every few seconds.
 */
void pic_status_report(const char *source, int ok, const char *detail);

/* Same, naming the user explicitly (threads outside the feature runner) */
void pic_status_report_as(const char *user, const char *source, int ok,
                          const char *detail);

#endif /* PIC_DATASTATUS_H */

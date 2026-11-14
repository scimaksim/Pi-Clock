/*
 * labels.h - Shared placement of map labels
 *
 * Each layer draws on its own surface at its own pace, so labels from
 * different layers used to print over each other. Layers that label
 * things on the map register the boxes they use here and look for a
 * spot no other layer has taken.
 *
 * Main thread only (layers render there).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef PIC_LABELS_H
#define PIC_LABELS_H

/* A stable id for a layer's labels (by name; -1 if the table is full) */
int pic_labels_owner(const char *name);

/* Start of the owner's render: forget the boxes it claimed last time */
void pic_labels_begin(int owner);

/* 1 if box {x0, y0, x1, y1} overlaps no box claimed by ANOTHER owner
 * that is still on screen (same view, drawn in the last few minutes) */
int pic_labels_free(int owner, const double box[4]);

/* As pic_labels_free(), and clear of this owner's own boxes too */
int pic_labels_clear(int owner, const double box[4]);

/* Record a box this owner has drawn in (a label, or an icon others
 * should keep off) */
void pic_labels_claim(int owner, const double box[4]);

/* Find room for a bw x bh box beside the point (ax, ay), `gap` away:
 * right, left, then above and below on either side. The box must lie
 * inside the w x h screen, clear of other owners' boxes and of this
 * owner's own. On success claims it, returns 1 and its top-left corner.
 * With no room returns 0 and the first (right-hand) position, unclaimed,
 * for callers that would rather overlap than drop the label. */
int pic_labels_place(int owner, double ax, double ay, double gap,
                     double bw, double bh, int w, int h,
                     double *ox, double *oy);

/* For tests */
void pic_labels_reset(void);
#endif

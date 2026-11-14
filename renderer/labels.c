/*
 * labels.c - Shared placement of map labels (see labels.h)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "labels.h"
#include "config.h"
#include <string.h>
#include <time.h>

#define MAX_OWNERS 16
#define MAX_BOXES  48
#define LIVE_S     300     /* an owner not redrawn for this long is off screen */

static struct {
    char name[24];
    double box[MAX_BOXES][4];
    int n;
    time_t at;             /* last pic_labels_begin() */
    unsigned gen;          /* viewport it was drawn for */
} own[MAX_OWNERS];
static int nown;

void pic_labels_reset(void)
{
    memset(own, 0, sizeof(own));
    nown = 0;
}

int pic_labels_owner(const char *name)
{
    int i;
    for (i = 0; i < nown; i++)
        if (strcmp(own[i].name, name) == 0) return i;
    if (nown >= MAX_OWNERS) return -1;
    strncpy(own[nown].name, name, sizeof(own[nown].name) - 1);
    return nown++;
}

void pic_labels_begin(int owner)
{
    if (owner < 0 || owner >= nown) return;
    own[owner].n = 0;
    own[owner].at = time(NULL);
    own[owner].gen = pic_config.viewport_gen;
}

static int hits(const double *a, const double *b)
{
    return !(a[2] < b[0] || a[0] > b[2] || a[3] < b[1] || a[1] > b[3]);
}

static int live(int o, time_t now)
{
    return own[o].n > 0 && own[o].gen == pic_config.viewport_gen && now - own[o].at <= LIVE_S;
}

int pic_labels_free(int owner, const double box[4])
{
    time_t now = time(NULL);
    int o, i;
    for (o = 0; o < nown; o++) {
        if (o == owner || !live(o, now)) continue;
        for (i = 0; i < own[o].n; i++)
            if (hits(box, own[o].box[i])) return 0;
    }
    return 1;
}

static int free_of_self(int owner, const double box[4])
{
    int i;
    if (owner < 0 || owner >= nown) return 1;
    for (i = 0; i < own[owner].n; i++)
        if (hits(box, own[owner].box[i])) return 0;
    return 1;
}

int pic_labels_clear(int owner, const double box[4])
{
    return pic_labels_free(owner, box) && free_of_self(owner, box);
}

void pic_labels_claim(int owner, const double box[4])
{
    if (owner < 0 || owner >= nown || own[owner].n >= MAX_BOXES) return;
    memcpy(own[owner].box[own[owner].n++], box, sizeof(double) * 4);
}

/* Side panels and the stack of notes at the bottom, from the last frame
 * (weak: absent in small test builds) */
extern double pic_panel_rect[2][4] __attribute__((weak));
double pic_legend_top(int height) __attribute__((weak));

/* The clocks, the side panels and the notes/key at the bottom: a label
 * there would be hidden or would cover them */
static int on_furniture(const double *b, int w, int h)
{
    double keep[3][4] = {
        { 0, 0, w * 0.21, h * 0.115 },              /* UTC clock */
        { w * 0.79, 0, w, h * 0.13 },               /* local clock, date, address */
        { 0, h * 0.905, w, h },                     /* notes and key */
    };
    int i;
    if (pic_legend_top && pic_legend_top(h) < keep[2][1]) keep[2][1] = pic_legend_top(h);
    for (i = 0; i < 3; i++)
        if (hits(b, keep[i])) return 1;
    if (&pic_panel_rect)
        for (i = 0; i < 2; i++)
            if (pic_panel_rect[i][2] > pic_panel_rect[i][0] && hits(b, pic_panel_rect[i])) return 1;
    return 0;
}

int pic_labels_place(int owner, double ax, double ay, double gap,
                     double bw, double bh, int w, int h,
                     double *ox, double *oy)
{
    /* dx side (+1 right, -1 left, 0 centred), dy in box heights */
    static const double cand[8][2] = {
        { 1, -0.5 }, { -1, -0.5 }, { 1, -1.15 }, { 1, 0.15 },
        { -1, -1.15 }, { -1, 0.15 }, { 0, -1.6 }, { 0, 0.6 } };
    int k;
    for (k = 0; k < 8; k++) {
        double b[4];
        b[0] = cand[k][0] > 0 ? ax + gap : cand[k][0] < 0 ? ax - gap - bw : ax - bw / 2;
        b[1] = ay + cand[k][1] * bh - (cand[k][0] == 0 ? (cand[k][1] < 0 ? gap * 0.5 : -gap * 0.5) : 0);
        b[2] = b[0] + bw;
        b[3] = b[1] + bh;
        if (b[0] < 2 || b[1] < 2 || b[2] > w - 2 || b[3] > h - 2) continue;
        if (on_furniture(b, w, h) || !pic_labels_clear(owner, b)) continue;
        pic_labels_claim(owner, b);
        *ox = b[0];
        *oy = b[1];
        return 1;
    }
    *ox = ax + gap;
    *oy = ay - bh * 0.5;
    return 0;
}

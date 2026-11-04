/* Cached compositing gives the same picture as plain compositing,
 * through layers re-rendering, switching off/on and opacity changes */
#include "check.h"
#include "layer.h"
#include <stdlib.h>
#include <string.h>

static void rnd_render(cairo_t *cr, int w, int h, time_t now, void *ud)
{
    unsigned seed = (unsigned)(size_t)ud * 7919u + (unsigned)now;
    int k;
    srand(seed);
    for (k = 0; k < 8; k++) {
        cairo_set_source_rgba(cr, rand() % 100 / 100.0, rand() % 100 / 100.0,
                              rand() % 100 / 100.0, 0.2 + rand() % 80 / 100.0);
        cairo_rectangle(cr, rand() % w, rand() % h, rand() % (w / 2), rand() % (h / 2));
        cairo_fill(cr);
    }
}

int main(void)
{
    int W = 480, H = 270, i, frame, maxdiff = 0;
    int iv[] = { 0, 60, 60, 300, 1, 60, 300, 2, 60, 120, 1, 60, 60, 1, 1, 60 };
    const char *const skip[] = { "hud", NULL };
    char names[16][8];
    pic_layer_stack_t st;
    cairo_surface_t *a = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cairo_surface_t *b = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);

    pic_layer_stack_init(&st, W, H);
    for (i = 0; i < 16; i++) {
        snprintf(names[i], sizeof(names[i]), "l%d", i);
        pic_layer_stack_add(&st, names[i], rnd_render, iv[i], 0.4 + 0.3 * (i % 3), (void *)(size_t)(i + 1));
    }
    snprintf(names[15], sizeof(names[15]), "hud");     /* skipped by name */
    st.layers[15].name = names[15];
    for (frame = 0; frame < 16; frame++) {
        time_t now = 1000 + frame;
        unsigned char *pa, *pb;
        long p;
        cairo_t *ca, *cb;
        if (frame == 4) st.layers[3].enabled = 0;
        if (frame == 7) st.layers[5].opacity = 0.2;
        if (frame == 9) st.layers[3].enabled = 1;
        if (frame == 11) st.layers[8].view_hidden = 1;
        pic_layer_stack_update(&st, now);
        ca = cairo_create(a);
        cairo_set_source_rgb(ca, 0, 0, 0);
        cairo_paint(ca);
        for (i = 0; i < st.count; i++) {
            pic_layer_t *l = &st.layers[i];
            if (!l->enabled || l->view_hidden || !l->surface || !strcmp(l->name, "hud")) continue;
            cairo_set_source_surface(ca, l->surface, 0, 0);
            cairo_paint_with_alpha(ca, l->opacity);
        }
        cairo_destroy(ca);
        cb = cairo_create(b);
        cairo_set_source_rgb(cb, 0, 0, 0);
        cairo_paint(cb);
        pic_layer_stack_composite_cached(&st, cb, skip, frame == 0);
        cairo_destroy(cb);
        cairo_surface_flush(a);
        cairo_surface_flush(b);
        pa = cairo_image_surface_get_data(a);
        pb = cairo_image_surface_get_data(b);
        for (p = 0; p < (long)W * H * 4; p++) {
            int d = abs(pa[p] - pb[p]);
            if (d > maxdiff) maxdiff = d;
        }
    }
    CHECK(maxdiff <= 3, "cached composite differs by %d/255", maxdiff);
    printf("composite: max difference %d/255 over 16 frames\n", maxdiff);
    return CHECK_DONE("test_composite");
}

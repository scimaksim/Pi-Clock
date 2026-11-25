/* Radar echo filter and coarse-layer rule, on made-up images */
#include "check.h"
#include "radar.h"
#include "layer.h"
#include "config.h"
#include <cairo/cairo.h>
#include <string.h>

/* IEM N0Q ramp colours at a few indices (index i = -32 + i/2 dBZ) */
static const unsigned RGB_0DBZ  = (154u << 16) | (162u << 8) | 180u;  /* idx 60: -2 dBZ */
static const unsigned RGB_8DBZ  = (75u << 16) | (100u << 8) | 161u;   /* idx 80:  8 dBZ */
static const unsigned RGB_28DBZ = (13u << 16) | (158u << 8) | 17u;    /* idx 120: 28 dBZ */

static cairo_surface_t *img(int w, int h)
{
    return cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
}
static void put(cairo_surface_t *s, int x, int y, unsigned rgb)
{
    unsigned int *row = (unsigned int *)(cairo_image_surface_get_data(s) +
                                         (size_t)y * cairo_image_surface_get_stride(s));
    row[x] = 0xff000000u | rgb;
}
static unsigned get(cairo_surface_t *s, int x, int y)
{
    const unsigned int *row = (const unsigned int *)(cairo_image_surface_get_data(s) +
                                                     (size_t)y * cairo_image_surface_get_stride(s));
    return row[x];
}

int main(void)
{
    cairo_surface_t *s;
    long op, unk, gone;
    int x, y;

    /* Threshold: 8 dBZ goes at 10, stays at 5; 28 dBZ always stays */
    s = img(4, 1);
    cairo_surface_flush(s);
    put(s, 0, 0, RGB_0DBZ); put(s, 1, 0, RGB_8DBZ); put(s, 2, 0, RGB_28DBZ);
    cairo_surface_mark_dirty(s);
    gone = pic_radar_filter(s, 10, 0, &op, &unk);
    CHECK(gone == 2 && op == 3 && unk == 0, "two weak echoes dropped (%ld of %ld)", gone, op);
    CHECK(get(s, 0, 0) == 0 && get(s, 1, 0) == 0 && get(s, 2, 0) != 0, "right pixels dropped");
    CHECK(get(s, 3, 0) == 0, "empty pixel left empty");
    cairo_surface_destroy(s);

    s = img(3, 1);
    cairo_surface_flush(s);
    put(s, 0, 0, RGB_0DBZ); put(s, 1, 0, RGB_8DBZ); put(s, 2, 0, RGB_28DBZ);
    cairo_surface_mark_dirty(s);
    CHECK(pic_radar_filter(s, 5, 0, NULL, NULL) == 1 && get(s, 1, 0) != 0, "5 dBZ keeps 8 dBZ");
    cairo_surface_destroy(s);

    s = img(3, 1);
    cairo_surface_flush(s);
    put(s, 0, 0, RGB_0DBZ);
    cairo_surface_mark_dirty(s);
    CHECK(pic_radar_filter(s, -32, 0, NULL, NULL) == 0, "-32 keeps everything");
    cairo_surface_destroy(s);

    /* Despeckle: a solid 5x5 rain cell survives (its corners have 3
     * kept neighbours and go, the rest stay); a lone strong pixel goes */
    s = img(12, 8);
    cairo_surface_flush(s);
    for (y = 1; y <= 5; y++)
        for (x = 1; x <= 5; x++) put(s, x, y, RGB_28DBZ);
    put(s, 9, 3, RGB_28DBZ);
    cairo_surface_mark_dirty(s);
    gone = pic_radar_filter(s, 10, 1, &op, NULL);
    CHECK(op == 26, "26 echo pixels (%ld)", op);
    CHECK(get(s, 9, 3) == 0, "isolated pixel removed");
    CHECK(get(s, 3, 3) != 0 && get(s, 1, 3) != 0 && get(s, 3, 1) != 0, "cell body and edges kept");
    CHECK(get(s, 1, 1) == 0 && get(s, 5, 5) == 0, "cell corners (3 neighbours) removed");
    CHECK(gone == 5, "five removed (%ld)", gone);
    cairo_surface_destroy(s);

    /* Without despeckle the lone pixel stays */
    s = img(5, 5);
    cairo_surface_flush(s);
    put(s, 2, 2, RGB_28DBZ);
    cairo_surface_mark_dirty(s);
    CHECK(pic_radar_filter(s, 10, 0, NULL, NULL) == 0 && get(s, 2, 2) != 0, "no despeckle: kept");
    cairo_surface_destroy(s);

    /* A different colour ramp: dBZ can't be read, image left alone */
    s = img(10, 1);
    cairo_surface_flush(s);
    for (x = 0; x < 10; x++) put(s, x, 0, x < 5 ? 0x123456u : RGB_0DBZ);
    cairo_surface_mark_dirty(s);
    gone = pic_radar_filter(s, 10, 0, &op, &unk);
    CHECK(gone == -1 && unk == 5 && get(s, 9, 0) != 0, "unknown ramp: untouched (%ld)", gone);
    cairo_surface_destroy(s);

    /* Unknown colours (under 5%) are kept */
    s = img(40, 1);
    cairo_surface_flush(s);
    for (x = 0; x < 40; x++) put(s, x, 0, x == 0 ? 0x123456u : RGB_28DBZ);
    cairo_surface_mark_dirty(s);
    CHECK(pic_radar_filter(s, 10, 0, NULL, &unk) == 0 && unk == 1 && get(s, 0, 0) != 0,
          "stray unknown colour kept");
    cairo_surface_destroy(s);

    CHECK(pic_radar_filter(NULL, 10, 1, NULL, NULL) == 0, "NULL surface");

    /* Coarse world-grid layers hide when the view is under 60° wide */
    CHECK(pic_layer_too_coarse("wind", 18.0), "wind hidden at 18°");
    CHECK(pic_layer_too_coarse("cloud", 45.0) && pic_layer_too_coarse("precip", 45.0) &&
          pic_layer_too_coarse("aurora", 45.0) && pic_layer_too_coarse("bandconditions", 45.0),
          "all five hidden at 45°");
    CHECK(!pic_layer_too_coarse("wind", 360.0) && !pic_layer_too_coarse("wind", 60.0),
          "wind shown on the world map");
    CHECK(!pic_layer_too_coarse("wind", 0.0), "unset span = world");
    CHECK(!pic_layer_too_coarse("radar", 18.0) && !pic_layer_too_coarse("satclouds", 18.0) &&
          !pic_layer_too_coarse("sst", 18.0), "fine layers not hidden");

    /* The stack applies it whatever the view setting */
    {
        pic_layer_stack_t st;
        memset(&st, 0, sizeof(st));
        st.count = 3;
        st.layers[0].name = "wind";   st.layers[0].enabled = 1; st.layers[0].view = 0;
        st.layers[1].name = "cloud";  st.layers[1].enabled = 1; st.layers[1].view = 2;
        st.layers[2].name = "radar";  st.layers[2].enabled = 1; st.layers[2].view = 0;
        pic_config.view_span_lon = 18.0;
        pic_layer_stack_apply_view(&st, 1);
        CHECK(st.layers[0].view_hidden && st.layers[1].view_hidden && !st.layers[2].view_hidden,
              "local view: wind and local-only cloud hidden, radar shown");
        pic_layer_stack_apply_view(&st, -1);
        CHECK(st.layers[0].view_hidden && st.layers[1].view_hidden, "fixed zoomed view: still hidden");
        pic_config.view_span_lon = 360.0;
        pic_layer_stack_apply_view(&st, 0);
        CHECK(!st.layers[0].view_hidden && st.layers[1].view_hidden, "world view: wind back, local-only cloud hidden");
        pic_layer_stack_apply_view(&st, -1);
        CHECK(!st.layers[0].view_hidden && !st.layers[1].view_hidden, "fixed world view: both shown");
    }
    return CHECK_DONE("test_radar");
}

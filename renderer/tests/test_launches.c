/*
 * Launches layer: parsing, nearby/visibility, map label layout (no two
 * labels overlap, all on screen, "+N more" for busy sites) and the
 * applet, for a crowded synthetic week.
 */
#include "check.h"
#include "../layers_launches.c"

static char js[16000];

static void add(time_t net, double la, double lo, const char *rk, const char *ms,
                const char *orbit, const char *loc)
{
    char a[40], b[1200];
    struct tm g;
    gmtime_r(&net, &g);
    strftime(a, sizeof(a), "%Y-%m-%dT%H:%M:%SZ", &g);
    snprintf(b, sizeof(b), "%s{\"net\":\"%s\",\"status\":{\"abbrev\":\"Go\"},"
             "\"name\":\"%s | %s\",\"rocket\":{\"configuration\":{\"name\":\"%s\"}},"
             "\"mission\":{\"name\":\"%s\",\"orbit\":{\"abbrev\":\"%s\"}},"
             "\"net_precision\":{\"abbrev\":\"MIN\"},"
             "\"pad\":{\"latitude\":%f,\"longitude\":%f,\"location\":{\"name\":\"%s\"}}}",
             js[strlen(js) - 1] == '[' ? "" : ",", a, rk, ms, rk, ms, orbit, la, lo, loc);
    strcat(js, b);
}

int main(void)
{
    time_t now = 1794000000;     /* fixed: 2026-11-06 */
    int i, j, z, launches;
    double zooms[][2] = { { 0, 0 }, { 0.6, -85 }, { 0, -150 } };

    pic_config.qth_lat = 35.6928;
    pic_config.qth_lon = -78.727;
    strcpy(js, "{\"results\":[");
    add(now + 1800, 37.84, -75.48, "Electron", "Wallops test", "LEO", "Wallops Island, VA, USA");
    add(now + 3 * 3600, 28.56, -80.58, "Falcon 9", "Crew-14", "LEO", "Cape Canaveral SFS, FL, USA");
    add(now + 9 * 3600, 28.49, -80.54, "Vulcan VC4S", "USSF-106", "GTO", "Cape Canaveral SFS, FL, USA");
    add(now + 20 * 3600, 28.60, -80.60, "Falcon Heavy", "GPS IIIF-1", "MEO", "Kennedy Space Center, FL, USA");
    add(now + 30 * 3600, 28.56, -80.57, "Falcon 9", "Starlink Group 10-3", "LEO", "Cape Canaveral SFS, FL, USA");
    add(now + 5 * 3600, 34.63, -120.61, "Falcon 9", "Starlink Group 17-9", "LEO", "Vandenberg SFB, CA, USA");
    add(now + 7 * 3600, -39.26, 177.86, "Electron", "Mahia test", "SSO", "Rocket Lab LC-1, Mahia, New Zealand");
    add(now + 11 * 3600, 45.92, 63.34, "Soyuz 2.1a", "Soyuz MS-30", "LEO", "Baikonur Cosmodrome, Republic of Kazakhstan");
    add(now + 15 * 3600, 25.99, -97.15, "Starship", "Flight 16", "LEO", "Starbase, TX, USA");
    strcat(js, "]}");

    pic_feature_launches();
    launches = pic_launches_parse(js, strlen(js), ll.items, MAX_LAUNCHES);
    CHECK(launches == 9, "parsed %d of 9", launches);
    ll.count = launches;
    ll.days = 3;
    ll.labels = 1;
    for (i = 0; i < ll.count; i++) {
        launch_t *L = &ll.items[i];
        L->near = pic_dist_km(pic_config.qth_lat, pic_config.qth_lon, L->lat, L->lon) < NEAR_KM;
        if (L->near)
            pic_launchvis_predict(L->lat, L->lon, L->orbit, L->mission, L->rocket, L->net,
                                  pic_config.qth_lat, pic_config.qth_lon, &L->vis);
    }
    CHECK(ll.items[0].near && ll.items[1].near && !ll.items[5].near, "nearby pads");
    CHECK(!strcmp(ll.items[1].orbit, "LEO"), "orbit parsed: %s", ll.items[1].orbit);

    for (z = 0; z < 3; z++) {
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1920, 1080);
        cairo_t *cr = cairo_create(s);
        pic_config.map_zoom = zooms[z][0];
        pic_config.center_lon = zooms[z][1];
        pic_config_recompute_viewport();
        pic_layer_render_launches(cr, 1920, 1080, now, NULL);
        CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS, "cairo ok");
        CHECK(last_nlabel >= 4, "zoom %d: only %d label blocks", z, last_nlabel);
        for (i = 0; i < last_nlabel; i++) {
            double *a = last_label[i];
            CHECK(a[0] >= 0 && a[1] >= 0 && a[2] <= 1920 && a[3] <= 1080,
                  "zoom %d: label %d off screen (%.0f,%.0f-%.0f,%.0f)", z, i, a[0], a[1], a[2], a[3]);
            for (j = i + 1; j < last_nlabel; j++) {
                double *b = last_label[j];
                CHECK(a[2] < b[0] || a[0] > b[2] || a[3] < b[1] || a[1] > b[3],
                      "zoom %d: labels %d and %d overlap", z, i, j);
            }
        }
        cairo_destroy(cr);
        cairo_surface_destroy(s);
    }
    {
        cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 300, 320);
        cairo_t *cr = cairo_create(s);
        double h = pic_applet_render_launches(NULL, 264, now, NULL);
        double h2 = pic_applet_render_launches(cr, 264, now, NULL);
        CHECK(h > 50 && h == h2, "applet heights %.1f / %.1f", h, h2);
        CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS, "applet cairo ok");
        cairo_destroy(cr);
        cairo_surface_destroy(s);
    }
    {
        pic_launch_alert_t A[8];
        int n = pic_launches_visible(A, 8);
        CHECK(n >= 0 && n <= 8, "visible list %d", n);
    }
    return CHECK_DONE("test_launches");
}

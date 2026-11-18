/* Close-up of home: feed parsers, map-data readers, the view cycle and
 * which layers show there. All data made up. */
#include "check.h"
#include "closeup.h"
#include "features_all.h"
#include "localview.h"
#include "layer.h"
#include "config.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int nlines, npts, nrings;
static char lastname[64];
static void cb(const char *name, const double *xy, int n, int ring, void *ud)
{
    (void)ud;
    nlines++; npts += n; nrings += ring;
    snprintf(lastname, sizeof(lastname), "%s", name);
    (void)xy;
}

int main(void)
{
    {   /* Pi-Dash /api/watershed */
        static cu_wsh_t w;
        const char *js = "{\"sites\":["
            "{\"site\":\"01000001\",\"role\":\"in\",\"name\":\"Test Creek above\",\"flow\":3.1,\"stage\":1.8,\"t\":1794769200,"
            "\"normal\":6.2,\"cls\":{\"k\":\"normal\",\"txt\":\"about normal for the date\"}},"
            "{\"site\":\"01000002\",\"role\":\"out\",\"name\":\"Test Creek below\",\"flow\":null,\"t\":null,\"normal\":null,\"cls\":null}],"
            "\"station\":{\"latest\":{\"airtemp\":{\"v\":54.3,\"unit\":\"F\",\"t\":\"2026-11-15 14:10:00\"},"
            "\"soilmoist\":{\"v\":0.25,\"unit\":\"m3/m3\",\"t\":\"2026-11-15 14:10:00\"}},"
            "\"rain_2h\":0,\"rain_24h\":0.1,\"rain_7d\":0.07,\"rain_30d\":2.4},\"station_error\":null}";
        int n = pic_cu_parse_watershed(js, strlen(js), &w);
        CHECK(n == 2, "two sites (%d)", n);
        CHECK(!strcmp(w.s[0].site, "01000001") && !strcmp(w.s[0].role, "in") && fabs(w.s[0].flow - 3.1) < 1e-9 &&
              fabs(w.s[0].normal - 6.2) < 1e-9 && w.s[0].t == 1794769200 && !strcmp(w.s[0].cls, "normal") &&
              !strcmp(w.s[0].cls_txt, "about normal for the date"), "site fields");
        CHECK(isnan(w.s[1].flow) && isnan(w.s[1].normal) && w.s[1].t == 0 && !w.s[1].cls[0], "nulls kept as missing");
        CHECK(w.st.ok && fabs(w.st.air - 54.3) < 1e-9 && fabs(w.st.moist - .25) < 1e-9 && isnan(w.st.wind) &&
              !strcmp(w.st.at, "2026-11-15 14:10:00") && fabs(w.st.rain_30d - 2.4) < 1e-9 && !w.st.has_loc, "station");
        CHECK(pic_cu_parse_watershed("{\"error\": \"unknown source\"}", 27, &w) == -1, "server error reply refused");
        CHECK(pic_cu_parse_watershed("<html>", 6, &w) == -1, "not JSON");
        js = "{\"sites\":[],\"station\":{\"latest\":{},\"lat\":35.7,\"lon\":-78.7}}";
        CHECK(pic_cu_parse_watershed(js, strlen(js), &w) == 0 && w.st.has_loc, "station position when given");
    }
    {   /* Pi-Dash /api/ncorbit */
        cu_fire_t f[4];
        int h;
        const char *js = "{\"fires\":[{\"lat\":35.6,\"lon\":-78.6,\"t\":1794726060,\"frp\":4.9,\"night\":true},"
                         "{\"lat\":99,\"lon\":0,\"t\":1},{\"lat\":36.0,\"lon\":-79.0,\"t\":1794700000,\"frp\":null,\"night\":false}],"
                         "\"fire_hours\":120,\"buoy\":{}}";
        int n = pic_cu_parse_fires(js, strlen(js), f, 4, &h);
        CHECK(n == 2 && h == 120, "two good fires, 5 days (%d, %d)", n, h);
        CHECK(f[0].night && fabsf(f[0].frp - 4.9f) < 1e-4 && f[1].frp == 0 && !f[1].night, "fire fields");
        CHECK(pic_cu_parse_fires("{\"fires\":null,\"buoy\":{}}", 24, f, 4, &h) == 0 && h == 48, "no fire data");
        CHECK(pic_cu_parse_fires("nope", 4, f, 4, &h) == -1, "bad reply");
    }
    {   /* USGS site rdb */
        cu_site_t s[2];
        const char *rdb = "# comment\n"
            "agency_cd\tsite_no\tstation_nm\tdec_lat_va\tdec_long_va\n"
            "5s\t15s\t50s\t16s\t16s\n"
            "USGS\t01000001\tTEST CREEK\t35.718889\t-78.752222\n"
            "USGS\t01000009\tOTHER\t35.1\t-78.1\r\n";
        memset(s, 0, sizeof(s));
        strcpy(s[0].site, "01000001");
        strcpy(s[1].site, "01000002");
        CHECK(pic_cu_parse_usgs_sites(rdb, s, 2) == 1, "one located");
        CHECK(s[0].has_loc && fabs(s[0].lat - 35.718889) < 1e-9 && fabs(s[0].lon + 78.752222) < 1e-9 && !s[1].has_loc, "coordinates");
        CHECK(pic_cu_parse_usgs_sites("#only comments\n", s, 2) == 0, "empty reply");
    }
    {   /* GeoJSON lines and rings */
        const char *js = "{\"features\":["
            "{\"properties\":{\"gnis_name\":\"Test Creek\"},\"geometry\":{\"type\":\"LineString\",\"coordinates\":[[-78.7,35.7],[-78.6,35.6],[-78.5,35.5]]}},"
            "{\"properties\":{\"gnis_name\":null},\"geometry\":{\"type\":\"MultiLineString\",\"coordinates\":[[[0,0],[1,1]],[[2,2],[3,3],[4,4]]]}},"
            "{\"properties\":{\"gnis_name\":\"Test Lake\"},\"geometry\":{\"type\":\"Polygon\",\"coordinates\":[[[0,0],[1,0],[1,1],[0,0]]]}},"
            "{\"properties\":{},\"geometry\":{\"type\":\"MultiPolygon\",\"coordinates\":[[[[0,0],[1,0],[1,1]]],[[[5,5],[6,5],[6,6]],[[5.2,5.2],[5.4,5.2],[5.4,5.4]]]]}},"
            "{\"properties\":{},\"geometry\":null},"
            "{\"properties\":{},\"geometry\":{\"type\":\"LineString\",\"coordinates\":[[1,1]]}}]}";
        int n = pic_cu_geo_lines(js, strlen(js), "gnis_name", cb, NULL);
        CHECK(n == 5, "features with coordinates (%d)", n);
        CHECK(nlines == 7 && nrings == 4 && npts == 3 + 2 + 3 + 4 + 3 + 3 + 3, "lines %d rings %d points %d", nlines, nrings, npts);
        CHECK(pic_cu_geo_lines("{\"error\":{}}", 12, NULL, cb, NULL) == -1, "error reply");
    }
    {   /* TIGER places */
        cu_place_t p[4];
        const char *js = "{\"features\":[{\"properties\":{\"BASENAME\":\"Testville\",\"CENTLAT\":\"+35.7800000\",\"CENTLON\":\"-078.6400000\",\"AREALAND\":380000000}},"
                         "{\"properties\":{\"BASENAME\":\"Nowhere\"}}]}";
        int n = pic_cu_places(js, strlen(js), p, 4);
        CHECK(n == 1 && !strcmp(p[0].name, "Testville") && fabs(p[0].lat - 35.78) < 1e-9 && fabs(p[0].lon + 78.64) < 1e-9 &&
              p[0].area == 380000000, "place (%d)", n);
    }
    {   /* ECONet local times */
        time_t t = pic_cu_local_time("2026-11-15 14:10:00");
        struct tm tm;
        localtime_r(&t, &tm);
        CHECK(t > 0 && tm.tm_hour == 14 && tm.tm_min == 10 && tm.tm_mday == 15, "local time");
        CHECK(pic_cu_local_time("") == 0 && pic_cu_local_time(NULL) == 0, "bad time");
    }
    {   /* Close-up only the layers that work there, and its own two only there */
        pic_layer_stack_t st;
        static const char *const nm[] = { "closeup", "watershed", "radar", "wind", "satellites", "qth", "hud", "basemap", "localmap" };
        int k;
        memset(&st, 0, sizeof(st));
        st.count = 9;
        for (k = 0; k < 9; k++) { st.layers[k].name = nm[k]; st.layers[k].enabled = 1; }
        st.layers[2].view = 2;   /* radar: local only */
        pic_config.view_span_lon = 0.7;
        pic_layer_stack_apply_view(&st, 2);
        CHECK(!st.layers[0].view_hidden && !st.layers[1].view_hidden && !st.layers[2].view_hidden &&
              !st.layers[5].view_hidden && !st.layers[6].view_hidden, "close-up: its layers, radar, home, clock");
        CHECK(st.layers[3].view_hidden && st.layers[4].view_hidden && st.layers[7].view_hidden && st.layers[8].view_hidden,
              "close-up: wind, satellites, world map, regional map hidden");
        pic_config.view_span_lon = 18;
        pic_layer_stack_apply_view(&st, 1);
        CHECK(st.layers[0].view_hidden && st.layers[1].view_hidden && !st.layers[2].view_hidden && !st.layers[8].view_hidden,
              "local view: close-up layers hidden");
        pic_config.view_span_lon = 360;
        pic_layer_stack_apply_view(&st, -1);
        CHECK(st.layers[0].view_hidden && st.layers[1].view_hidden, "fixed view: close-up layers hidden");
    }
    {   /* The cycle: world 5 min, local 10, close-up 3, back to world */
        FILE *f = fopen("/tmp/pic_test_closeup.conf", "w");
        time_t t0 = 1794000000, t;
        int seen_cu = 0, cu_secs = 0, local_secs = 0, prev = 0;
        double span = 0, clat = 0, clon = 0;
        fprintf(f, "VIEW_MODE=cycle\nVIEW_GLOBAL_MIN=5\nVIEW_LOCAL_MIN=10\nVIEW_CLOSEUP_MIN=3\nCLOSEUP_SPAN=0.7\n"
                   "CLOSEUP_CENTER=35.691,-78.700\nVIEW_SMART=0\nLOCAL_ZOOM=20\n");
        fclose(f);
        setenv("PIC_RENDERER_CONF", "/tmp/pic_test_closeup.conf", 1);
        setenv("PIC_VIEW_STATE", "/tmp/pic_test_closeup_worlds", 1);
        pic_config.qth_lat = 35.6928; pic_config.qth_lon = -78.727;
        pic_view_reload(t0);
        for (t = t0; t < t0 + 3 * 18 * 60; t += 5) {
            pic_view_update(t);
            if (pic_view_is_closeup()) {
                cu_secs += 5;
                if (!prev) seen_cu++;
                span = pic_config.view_span_lon; clat = pic_config.view_center_lat; clon = pic_config.view_center_lon;
                CHECK(pic_view_is_local(), "close-up counts as local");
            } else if (pic_view_is_local()) local_secs += 5;
            prev = pic_view_is_closeup();
        }
        CHECK(seen_cu == 3, "close-up once a cycle (%d in 54 min)", seen_cu);
        CHECK(cu_secs >= 3 * 175 && cu_secs <= 3 * 185, "3 min each (%d s)", cu_secs);
        CHECK(local_secs >= 3 * 590 && local_secs <= 3 * 610, "local 10 min each (%d s)", local_secs);
        CHECK(fabs(span - 0.7) < 1e-9 && fabs(clat - 35.691) < 1e-9 && fabs(clon + 78.7) < 1e-9,
              "close-up viewport %.3f at %.3f,%.3f", span, clat, clon);
        /* off: back to world -> local -> world */
        f = fopen("/tmp/pic_test_closeup.conf", "w");
        fprintf(f, "VIEW_MODE=cycle\nVIEW_GLOBAL_MIN=5\nVIEW_LOCAL_MIN=5\nVIEW_CLOSEUP_MIN=0\nVIEW_SMART=0\n");
        fclose(f);
        pic_view_reload(t);
        seen_cu = 0;
        for (t0 = t; t < t0 + 3600; t += 5) { pic_view_update(t); seen_cu |= pic_view_is_closeup(); }
        CHECK(!seen_cu && pic_config.closeup_zoom == 0, "close-up off");
        unlink("/tmp/pic_test_closeup.conf");
        unlink("/tmp/pic_test_closeup_worlds");
    }
    return CHECK_DONE("test_closeup");
}

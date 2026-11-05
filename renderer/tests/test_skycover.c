/* Cloud cover at home: hour overlap, threshold, Astrospheric first,
 * Open-Meteo parsing */
#include "check.h"
#include "../skycover.h"
#include "../astro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    time_t t0 = 1793793600;   /* 2026-11-04 12:00 UTC, on the hour */
    int c[6] = { 10, 30, 90, 100, 40, 0 }, pct = -2;
    static pic_astro_t a;
    FILE *f;
    const char *js =
        "{\"hourly\":{\"time\":[\"2026-11-04T12:00\",\"2026-11-04T13:00\",\"2026-11-04T14:00\"],"
        "\"cloud_cover\":[5,null,75]}}";

    f = fopen("/tmp/pic_test_sky.conf", "w");
    fputs("PASS_CLOUD_MAX=50\n", f);
    fclose(f);
    setenv("PIC_RENDERER_CONF", "/tmp/pic_test_sky.conf", 1);

    CHECK(pic_sky_cloud(t0, t0 + 600) == -1, "no forecast -> unknown");
    CHECK(pic_sky_clear(t0, t0 + 600, &pct) == PIC_SKY_UNKNOWN && pct == -1, "unknown state");

    pic_skycover_set(t0, c, 6);
    /* 12:45 for 5 minutes lies inside the 12:00 hour */
    CHECK(pic_sky_cloud(t0 + 2700, t0 + 3000) == 10, "inside one hour: %d", pic_sky_cloud(t0 + 2700, t0 + 3000));
    /* 12:58 - 13:04 spans two hours: (10 + 30) / 2 */
    CHECK(pic_sky_cloud(t0 + 3480, t0 + 3840) == 20, "two hours: %d", pic_sky_cloud(t0 + 3480, t0 + 3840));
    CHECK(pic_sky_cloud(t0 + 2 * 3600 + 60, t0 + 2 * 3600 + 400) == 90, "14:01");
    CHECK(pic_sky_clear(t0 + 60, t0 + 400, &pct) == PIC_SKY_CLEAR && pct == 10, "clear at 10%%");
    CHECK(pic_sky_clear(t0 + 2 * 3600, t0 + 2 * 3600 + 300, &pct) == PIC_SKY_CLOUDY && pct == 90, "cloudy 90%%");
    CHECK(pic_sky_clear(t0 + 4 * 3600, t0 + 4 * 3600 + 300, NULL) == PIC_SKY_CLEAR, "40%% under 50");
    CHECK(pic_sky_cloud(t0 + 7 * 3600, t0 + 7 * 3600 + 300) == -1, "past the forecast -> unknown");
    CHECK(pic_sky_clear(t0, t0, &pct) == PIC_SKY_CLEAR, "instant at the hour start");

    /* Astrospheric wins where it has hours */
    pthread_mutex_init(&a.mutex, NULL);
    a.nhours = 2;
    a.hours[0].t = t0; a.hours[0].cloud = 80;
    a.hours[1].t = t0 + 3600; a.hours[1].cloud = 0;
    pic_astro_shared = &a;
    CHECK(pic_sky_cloud(t0 + 600, t0 + 900) == 80, "Astrospheric first");
    CHECK(pic_sky_cloud(t0 + 2 * 3600 + 60, t0 + 2 * 3600 + 400) == 90, "Open-Meteo beyond it");
    pic_astro_shared = NULL;

    /* Parsing: null hours are skipped, times are UTC */
    CHECK(pic_skycover_parse(js, strlen(js), t0) == 3, "3 hours parsed");
    CHECK(pic_sky_cloud(t0 + 60, t0 + 120) == 5, "12:00 = 5");
    CHECK(pic_sky_cloud(t0 + 3600 + 60, t0 + 3600 + 120) == -1, "null hour unknown");
    CHECK(pic_sky_cloud(t0 + 3000, t0 + 7300) == 40, "avg over 5 and 75 skipping null: %d",
          pic_sky_cloud(t0 + 3000, t0 + 7300));
    CHECK(pic_skycover_parse("{oops", 5, t0) == 0, "bad JSON");
    CHECK(pic_sky_cloud(t0 + 60, t0 + 120) == 5, "bad reply keeps the old forecast");

    remove("/tmp/pic_test_sky.conf");
    return CHECK_DONE("test_skycover");
}

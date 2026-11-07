/* Eclipse geometry against published circumstances (NASA/Espenak):
 * greatest-eclipse points, central durations, Raleigh's 2024 partial,
 * eclipse types and lunar contacts; and the layer's idle/replay logic */
#include "check.h"
#include "../eclipse.h"
#include "../feature.h"
#include "../features_all.h"
#include "../config.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cairo/cairo.h>

static long long utc(int y, int mo, int d, int h, int mi, int s)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
    return (long long)timegm(&tm);
}

static const pic_ecl_solar_t *find(const char *date)
{
    int i;
    for (i = 0; i < pic_ecl_nsolar; i++)
        if (!strcmp(pic_ecl_solar[i].date, date)) return &pic_ecl_solar[i];
    return NULL;
}

static int duration(const pic_ecl_solar_t *e, double t, double lat, double lon)
{
    pic_ecl_local_t o;
    double a = t, b = t;
    while (pic_ecl_solar_local(e, a - 1, lat, lon, &o), o.central) a -= 1;
    while (pic_ecl_solar_local(e, b + 1, lat, lon, &o), o.central) b += 1;
    return (int)(b - a + 1);
}

int main(void)
{
    /* date, published GE lat, lon, central duration (s), type */
    struct { const char *date; double lat, lon; int dur; char type; } G[] = {
        { "2024-04-08", 25.29, -104.14, 268, 'T' },
        { "2024-10-02", -21.95, -114.50, 445, 'A' },
        { "2026-08-12", 65.20, -25.20, 138, 'T' },
        { "2027-08-02", 25.50, 33.20, 383, 'T' },
    };
    int i;
    double lat, lon;
    pic_ecl_local_t o;

    for (i = 0; i < 4; i++) {
        const pic_ecl_solar_t *e = find(G[i].date);
        int d;
        CHECK(e != NULL, "%s in the table", G[i].date);
        if (!e) continue;
        CHECK(e->type == G[i].type, "%s type %c", G[i].date, e->type);
        CHECK(pic_ecl_solar_centre(e, (double)e->tmax, &lat, &lon), "%s centre", G[i].date);
        CHECK(fabs(lat - G[i].lat) < 0.1 && fabs(lon - G[i].lon) < 0.1,
              "%s GE %.2f %.2f, want %.2f %.2f", G[i].date, lat, lon, G[i].lat, G[i].lon);
        d = duration(e, (double)e->tmax, lat, lon);
        CHECK(abs(d - G[i].dur) <= 3, "%s duration %d s, want %d", G[i].date, d, G[i].dur);
        pic_ecl_solar_local(e, (double)e->tmax, lat, lon, &o);
        CHECK(o.central == (G[i].type == 'T' ? 1 : 2), "%s central kind %d", G[i].date, o.central);
        CHECK(e->p1 < e->tmax && e->tmax < e->p4 && e->p4 - e->p1 < 6 * 3600 + 1800, "%s span", G[i].date);
    }

    /* Raleigh / Lake Wheeler, 8 Apr 2024: ~78% at 3:16 PM EDT */
    {
        const pic_ecl_solar_t *e = find("2024-04-08");
        double best = 0, tb = 0, t;
        for (t = (double)e->p1; t < (double)e->p4; t += 10) {
            pic_ecl_solar_local(e, t, 35.69, -78.71, &o);
            if (o.obscuration > best) { best = o.obscuration; tb = t; }
        }
        CHECK(best > 0.76 && best < 0.80, "Raleigh max %.3f", best);
        CHECK(fabs(tb - (double)utc(2024, 4, 8, 19, 15, 45)) < 120, "Raleigh max at %.0f", tb);
        /* the Moon sits north of the Sun's centre there (path to the NW) */
        pic_ecl_solar_local(e, tb, 35.69, -78.71, &o);
        CHECK(o.dy > 0, "Moon north of centre: dy %.2f", o.dy);
        CHECK(o.moon_r > 1.0, "Moon larger than the Sun in a total eclipse: %.3f", o.moon_r);
        /* night side and outside the penumbra */
        pic_ecl_solar_local(e, tb, 35.0, 120.0, &o);
        CHECK(o.obscuration == 0 && !o.sun_up, "China at night");
        pic_ecl_solar_local(e, tb, -40.0, -60.0, &o);
        CHECK(o.obscuration == 0 && o.sun_up, "Argentina outside");
    }

    /* Overlap helper */
    CHECK(fabs(pic_ecl_overlap(1.0, 0.0) - 1.0) < 1e-9, "full cover");
    CHECK(fabs(pic_ecl_overlap(0.9, 0.0) - 0.81) < 1e-9, "annular 0.81");
    CHECK(pic_ecl_overlap(1.0, 2.0) == 0.0, "touching");
    CHECK(fabs(pic_ecl_overlap(1.0, 1.0) - 0.391) < 0.001, "half offset %.4f", pic_ecl_overlap(1.0, 1.0));

    /* Lookups */
    {
        const pic_ecl_solar_t *e = find("2027-08-02");
        CHECK(pic_ecl_solar_at((time_t)e->tmax) == e, "under way at tmax");
        CHECK(pic_ecl_solar_at((time_t)e->p1 - 60) == NULL, "not before P1");
        CHECK(pic_ecl_solar_next((time_t)utc(2026, 11, 7, 0, 0, 0)) == find("2027-02-06"), "next after today");
        CHECK(pic_ecl_solar_prev((time_t)utc(2026, 11, 7, 0, 0, 0)) == find("2026-08-12"), "last before today");
    }

    /* Lunar: 14 Mar 2025 total, totality 06:26-07:31 UT, umbral mag 1.178;
     * 3 Mar 2026 total, totality 11:04-12:03 UT */
    {
        int k, ok = 0;
        for (k = 0; k < pic_ecl_nlunar; k++) {
            const pic_ecl_lunar_t *e = &pic_ecl_lunar[k];
            pic_ecl_lunar_state_t s;
            if (!strcmp(e->date, "2025-03-14")) {
                ok++;
                CHECK(e->kind == 2, "2025-03-14 total");
                CHECK(llabs(e->c[4] - utc(2025, 3, 14, 6, 26, 0)) < 120, "U2 %lld", e->c[4]);
                CHECK(llabs(e->c[5] - utc(2025, 3, 14, 7, 31, 0)) < 120, "U3 %lld", e->c[5]);
                pic_ecl_lunar_state(e, (double)e->tmax, &s);
                CHECK(fabs(s.umbral - 1.178) < 0.01, "umag %.3f", s.umbral);
                /* the Moon was overhead the eastern Pacific, near 105W */
                CHECK(fabs(s.sublon + 105) < 4 && fabs(s.sublat - 3) < 4, "sub-lunar %.1f %.1f",
                      s.sublat, s.sublon);
                CHECK(pic_ecl_lunar_at((time_t)e->tmax) == e, "lunar under way");
            }
            if (!strcmp(e->date, "2026-03-03")) {
                ok++;
                CHECK(llabs(e->c[4] - utc(2026, 3, 3, 11, 4, 0)) < 120, "2026 U2 %lld", e->c[4]);
                CHECK(llabs(e->c[5] - utc(2026, 3, 3, 12, 3, 0)) < 120, "2026 U3 %lld", e->c[5]);
            }
            if (e->kind == 0) CHECK(e->c[2] == 0 && e->c[4] == 0, "%s penumbral: no umbral contacts", e->date);
        }
        CHECK(ok == 2, "lunar eclipses found");
    }

    /* The layer: idle between eclipses, drawing during one, replays */
    {
        pic_feature_t *f = pic_feature_eclipse();
        const pic_ecl_solar_t *e = find("2024-04-08");
        cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 960, 540);
        cairo_t *cr = cairo_create(sf);
        FILE *c;
        pic_config.qth_lat = 35.69;
        pic_config.qth_lon = -78.71;
        c = fopen("/tmp/pic_ecl_test.conf", "w");
        fputs("ECLIPSE_REPLAY=0\n", c);
        fclose(c);
        setenv("PIC_RENDERER_CONF", "/tmp/pic_ecl_test.conf", 1);
        CHECK(f->idle((time_t)utc(2026, 11, 7, 12, 0, 0), f->data), "idle today");
        CHECK(!f->idle((time_t)e->tmax, f->data), "busy during 2024-04-08");
        CHECK(!f->idle((time_t)e->p1 - 3600, f->data), "preview an hour before");
        f->layer(cr, 960, 540, (time_t)e->tmax, f->data);
        f->legend(cr, 960, 540, (time_t)e->tmax, f->data);
        CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS, "solar render");
        {
            const pic_ecl_lunar_t *l = pic_ecl_lunar_next((time_t)utc(2025, 3, 1, 0, 0, 0));
            f->layer(cr, 960, 540, (time_t)l->tmax, f->data);
            CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS, "lunar render");
        }
        remove("/tmp/pic_ecl_test.conf");
        cairo_destroy(cr);
        cairo_surface_destroy(sf);
    }
    return CHECK_DONE("test_eclipse");
}

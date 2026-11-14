/* Other-world geochrons: Mars time against NASA's Mars24 worked example
 * and against Horizons; Horizons parsing; shadow geometry; Jupiter
 * System II -> III */
#include "check.h"
#include "../planetview.h"
#include <math.h>
#include <cairo/cairo.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string.h>

int main(void)
{
    pic_subpt_t o[8], s[8], q;
    double t2000 = 947116800.0;   /* 2000-01-06 00:00:00 UTC */
    double t = 1794067200.0;      /* 2026-11-07 16:00 UTC */
    double lon, lat, ls, dec;
    int n;

    /* Mars24 worked example: MTC 23.99425 h (23:59:39), Ls ~277.19 */
    CHECK(fabs(pic_mars_mtc(t2000) - 23.99425) < 0.0005, "MTC %.5f", pic_mars_mtc(t2000));
    CHECK(fabs(pic_mars_ls(t2000) - 277.1876) < 0.01, "Ls %.4f", pic_mars_ls(t2000));
    CHECK(fabs(pic_mars_msd(t2000) - 44795.9998) < 0.0005, "MSD %.5f", pic_mars_msd(t2000));

    /* Against Horizons today: sub-solar latitude on Mars 7.89 deg
     * (planetographic) at 2026-11-07 16:00; Mars24 solar declination
     * from Ls: asin(0.42565 sin Ls) + 0.25 sin Ls */
    ls = pic_mars_ls(t);
    dec = asin(0.42565 * sin(ls * M_PI / 180)) * 180 / M_PI + 0.25 * sin(ls * M_PI / 180);
    CHECK(fabs(dec - 7.891) < 0.1, "Ls %.2f gives dec %.3f, Horizons 7.891", ls, dec);

    /* Horizons table: west longitudes -> east, planetographic -> centric */
    {
        const char *txt =
            "header\n$$SOE\n"
            " 2026-Nov-07 16:00, , , 273.240048,  21.126847,  310.126242,   7.891367,  1.37767389345799,-14.4260611,    11.45776439,\n"
            " 2026-Nov-07 16:01, , , 273.483385,  21.126916,  310.369627,   7.891502,  1.37766810781356,-14.4260966,    11.45771627,\n"
            "$$EOE\nfooter\n";
        n = pic_horizons_parse(txt, 1, 0.00589, 1, o, s, 8, 149597870.7);
        CHECK(n == 2, "2 rows, got %d", n);
        CHECK(fabs(o[0].lon - (360 - 273.240048)) < 1e-6, "obs lon east %.6f (273.24 W = 86.76 E)", o[0].lon);
        CHECK(fabs(s[0].lon - (-310.126242 + 360)) < 1e-6, "sun lon east %.6f", s[0].lon);
        CHECK(o[0].lat < 21.126847 && o[0].lat > 20.9, "centric lat %.4f", o[0].lat);
        CHECK(fabs(o[0].r - 1.37767389345799 * 149597870.7) < 1, "range");
        CHECK(fabs(o[0].t - t) < 0.5, "time");
        CHECK(pic_subpt_at(o, n, t + 30, &q) && fabs(q.t - t - 30) < 1e-9, "interp");
        CHECK(!pic_subpt_at(o, n, t + 120, &q), "outside");
    }
    {   /* spacecraft table: lon, lat, range only */
        const char *txt = "$$SOE\n 2026-Nov-07 16:00, , , 359.956996,  87.317835,  0.00002467502292, -0.0003840,\n$$EOE\n";
        n = pic_horizons_parse(txt, 1, 0.00589, 1, o, NULL, 8, 149597870.7);
        CHECK(n == 1 && fabs(o[0].lon - 0.043004) < 1e-6, "MRO lon %.6f", o[0].lon);
        CHECK(fabs(o[0].r - 3691.3) < 2, "MRO range %.1f km", o[0].r);
    }
    /* interpolation across the 180 deg line */
    {
        pic_subpt_t a[2] = { { 0, 179, 0, 1 }, { 10, -179, 0, 1 } };
        CHECK(pic_subpt_at(a, 2, 5, &q) && fabs(fabs(q.lon) - 180) < 1e-9, "wrap %.3f", q.lon);
    }

    /* Shadows: Sun overhead at (0,0); a moon over (0,0) shadows (0,0);
     * a moon over (10E, 0) at 3r shadows asin(3 sin 10) east */
    CHECK(pic_shadow_point(0, 0, 3000, 0, 0, 1000, 1000, &lon, &lat) &&
          fabs(lon) < 1e-9 && fabs(lat) < 1e-9, "straight down");
    CHECK(pic_shadow_point(10, 0, 3000, 0, 0, 1000, 1000, &lon, &lat) &&
          fabs(lon - asin(3 * sin(10 * M_PI / 180)) * 180 / M_PI) < 1e-9, "offset %.4f", lon);
    CHECK(!pic_shadow_point(180, 0, 3000, 0, 0, 1000, 1000, &lon, &lat), "behind the planet");
    CHECK(!pic_shadow_point(40, 0, 3000, 0, 0, 1000, 1000, &lon, &lat), "misses the limb");
    /* flattened planet: shadow latitude follows the ellipsoid */
    CHECK(pic_shadow_point(0, 10, 2000, 0, 0, 1000, 900, &lon, &lat) && lat > 10 && lat < 90, "ellipsoid %.2f", lat);

    /* Jupiter II -> III: rate 0.266 deg/day; and offset 241.65 at J2000 */
    {
        double a = pic_jupiter_sys2_to_sys3(50, 946727935.816);   /* J2000.0 TT as UTC */
        double b = pic_jupiter_sys2_to_sys3(50, 946727935.816 + 100 * 86400.0);
        CHECK(fabs(a - 291.65) < 0.01, "J2000 %.3f", a);
        CHECK(fabs(fmod(b - a + 360, 360) - 26.6) < 0.01, "100 d drift %.3f", b - a);
    }
    /* The whole view, from a cache written here: Moon day/night lands
     * where the Sun is overhead; Mars draws a rover from its feed */
    {
        char dir[] = "/tmp/pvtestXXXXXX", path[300];
        FILE *f;
        cairo_surface_t *sf;
        cairo_t *cr;
        unsigned char *px;
        int st;
        CHECK(pic_world_by_name("MARS") == PIC_WORLD_MARS && pic_world_by_name("pluto") == 0, "names");
        CHECK(strcmp(pic_world_name(PIC_WORLD_JUPITER), "Jupiter") == 0, "name");
        CHECK(mkdtemp(dir) != NULL, "tmp");
        setenv("PIC_WORLDS_CACHE", dir, 1);
        setenv("PIC_WORLDS_DIR", dir, 1);
        {   /* a plain light-grey map (stb reads PNG whatever the name) */
            cairo_surface_t *m = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 64, 32);
            cairo_t *mc = cairo_create(m);
            cairo_set_source_rgb(mc, 0.7, 0.7, 0.7);
            cairo_paint(mc);
            cairo_destroy(mc);
            snprintf(path, sizeof(path), "%s/moon_1920.jpg", dir);
            cairo_surface_write_to_png(m, path);
            cairo_surface_destroy(m);
        }
        snprintf(path, sizeof(path), "%s/hz_moon_earth.txt", dir);
        f = fopen(path, "w");
        fprintf(f, "$$SOE\n"
                " 2026-Nov-07 16:00, , , 5.0, 5.7, 90.0, 0.0, 0.0025, 0.03,\n"
                " 2026-Nov-07 18:00, , , 5.1, 5.7, 90.0, 0.0, 0.0025, 0.03,\n$$EOE\n");
        fclose(f);
        snprintf(path, sizeof(path), "%s/M20_current.json", dir);
        f = fopen(path, "w");
        fprintf(f, "{\"features\":[{\"properties\":{\"RMC\":\"93_0\",\"sol\":2022,"
                   "\"lon\":77.23322287,\"lat\":18.43559434,\"yaw\":94,\"dist_total_m\":45245.71}}]}");
        fclose(f);
        sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 960, 540);
        cr = cairo_create(sf);
        pic_planetview_render(cr, 960, 540, PIC_WORLD_MOON, (time_t)1794074400);
        cairo_surface_flush(sf);
        px = cairo_image_surface_get_data(sf);
        st = cairo_image_surface_get_stride(sf);
        {
            /* map is 960x480 from y=30: 90E 0N = (720, 270); 90W = (240, 270) */
            int day = px[270 * st + 760 * 4 + 1], night = px[270 * st + 200 * 4 + 1];
            CHECK(day > night + 20, "day %d at 90E brighter than night %d at 90W", day, night);
        }
        pic_planetview_render(cr, 960, 540, PIC_WORLD_MARS, (time_t)1794074400);
        pic_planetview_render(cr, 960, 540, PIC_WORLD_JUPITER, (time_t)1794074400);
        CHECK(cairo_status(cr) == CAIRO_STATUS_SUCCESS, "cairo ok");
        cairo_destroy(cr);
        cairo_surface_destroy(sf);
        unlink(path);
        snprintf(path, sizeof(path), "%s/hz_moon_earth.txt", dir);
        unlink(path);
        snprintf(path, sizeof(path), "%s/moon_1920.jpg", dir);
        unlink(path);
        rmdir(dir);
    }
    return CHECK_DONE("test_planet");
}

/*
 * Astronomy checks against published values: equinoxes and solstices
 * (USNO / Meeus tables) and the Galilean moons (Meeus example 44.a).
 */
#include "check.h"
#include "astro.h"
#include "sky_icons.h"
#include "planetview.h"
#include <math.h>
#include <stdlib.h>
#include <time.h>

static time_t utc(int Y, int M, int D, int h, int m)
{
    struct tm t = { 0 };
    t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D; t.tm_hour = h; t.tm_min = m;
    return timegm(&t);
}

int main(void)
{
    /* Seasons, to the minute (USNO): within 90 s */
    static const int want[][6] = {
        { 2026, 0, 3, 20, 14, 46 }, { 2026, 1, 6, 21, 8, 25 },
        { 2026, 2, 9, 23, 0, 6 },   { 2026, 3, 12, 21, 20, 50 },
        { 2024, 0, 3, 20, 3, 6 },   { 2025, 3, 12, 21, 15, 3 },
        { 2027, 1, 6, 21, 14, 11 },
    };
    size_t i;
    for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        time_t got = pic_astro_season(want[i][0], want[i][1]);
        time_t exp = utc(want[i][0], want[i][2], want[i][3], want[i][4], want[i][5]);
        CHECK(labs((long)(got - exp)) <= 90, "season %d/%d off by %ld s", want[i][0], want[i][1],
              (long)(got - exp));
    }

    {   /* Galilean moons: Meeus example 44.a, 1992 Dec 16 0h TD */
        double x[4];
        int b[4];
        static const double ex[4] = { -3.44, 7.44, 1.24, 7.08 };
        pic_galilean_moons((time_t)((2448972.50068 - 2440587.5) * 86400.0) - 69, x, b);
        for (i = 0; i < 4; i++)
            CHECK(fabs(x[i] - ex[i]) < 0.03, "moon %d x %.2f want %.2f", (int)i + 1, x[i], ex[i]);
        /* 2015-03-02 14:00 UT (JPL): Io +3.7, Europa just west of centre and behind */
        pic_galilean_moons(1425304800, x, b);
        CHECK(fabs(x[0] - 3.7) < 0.4 && fabs(x[1]) < 0.4 && b[1] == 1, "2015 Io %.2f Europa %.2f", x[0], x[1]);
    }
    {   /* Mars24 worked example (Allison & McEwen): 2000 Jan 6 00:00 UTC:
         * Ls 277.19, EOT -5.19 deg, sub-solar longitude 174.73 W */
        double t = 947116800.0, eot = pic_mars_eot(t);
        double west = fmod(pic_mars_mtc(t) * 15.0 + eot + 180.0 + 720.0, 360.0);
        CHECK(fabs(pic_mars_ls(t) - 277.18758) < 0.01, "Ls %.4f", pic_mars_ls(t));
        CHECK(fabs(eot + 5.18774) < 0.05, "EOT %.4f", eot);
        CHECK(fabs(west - 174.726) < 0.05, "sub-solar %.3f W", west);
    }
    return CHECK_DONE("test_astro");
}

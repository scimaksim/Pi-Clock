/*
 * SGP4 positions against Skyfield (data/sgp4_ref.csv, made from
 * data/tles.txt): heavy-drag, eccentric and ordinary low orbits at the
 * TLE epoch and up to 3 days on. Also the elevation used for passes.
 */
#include "check.h"
#include "satellite.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

double pic_sat_look_angles(const pic_sat_t *s, time_t t, double qlat,
                           double qlon, double *az, int *sunlit);

static pic_sat_t sats[64];
static int nsat;

static double gc_km(double la1, double lo1, double la2, double lo2)
{
    double r = M_PI / 180, a = sin((la2 - la1) * r / 2), b = sin((lo2 - lo1) * r / 2);
    return 6371.0 * 2 * asin(sqrt(a * a + cos(la1 * r) * cos(la2 * r) * b * b));
}

int main(void)
{
    char l0[200], l1[200], l2[200], line[256];
    FILE *f = fopen("data/tles.txt", "r");
    int rows = 0, i;
    double worst = 0, worst_alt = 0;

    CHECK(f != NULL, "data/tles.txt");
    if (!f) return 1;
    while (fgets(l0, sizeof(l0), f) && fgets(l1, sizeof(l1), f) && fgets(l2, sizeof(l2), f)) {
        l1[strcspn(l1, "\r\n")] = 0;
        l2[strcspn(l2, "\r\n")] = 0;
        CHECK(pic_sat_parse_tle(&sats[nsat], l1, l2) == 0, "parse %s", l1);
        sats[nsat].norad_id = atoi(l1 + 2);
        nsat++;
    }
    fclose(f);
    f = fopen("data/sgp4_ref.csv", "r");
    CHECK(f != NULL, "data/sgp4_ref.csv");
    if (!f) return 1;
    while (fgets(line, sizeof(line), f)) {
        int id;
        long t;
        double la, lo, al, cla, clo, cal, d;
        if (line[0] == '#' || sscanf(line, "%d,%ld,%lf,%lf,%lf", &id, &t, &la, &lo, &al) != 5)
            continue;
        for (i = 0; i < nsat && sats[i].norad_id != id; i++) ;
        CHECK(i < nsat, "no TLE for %d", id);
        if (i == nsat) continue;
        pic_sat_position_at(&sats[i], (time_t)t, &cla, &clo, &cal);
        d = gc_km(la, lo, cla, clo);
        if (d > worst) worst = d;
        if (fabs(cal - al) > worst_alt) worst_alt = fabs(cal - al);
        CHECK(d < 0.5, "%d at %ld: ground position off by %.3f km", id, t, d);
        CHECK(fabs(cal - al) < 0.2, "%d at %ld: altitude off by %.3f km", id, t, cal - al);
        rows++;
    }
    fclose(f);
    CHECK(rows > 100, "only %d reference rows", rows);
    printf("sgp4: %d satellites, %d positions, worst %.3f km ground / %.3f km altitude\n",
           nsat, rows, worst, worst_alt);

    /* Elevation straight up: a satellite over home is at ~90° */
    for (i = 0; i < nsat && sats[i].norad_id != 25544; i++) ;
    if (i < nsat) {
        double la, lo, al, el;
        time_t t = (time_t)((sats[i].epoch_jd - 2440587.5) * 86400.0);
        pic_sat_position_at(&sats[i], t, &la, &lo, &al);
        el = pic_sat_look_angles(&sats[i], t, la, lo, NULL, NULL);
        CHECK(el > 89.5, "ISS seen from its own ground point at %.2f deg", el);
    }
    return CHECK_DONE("test_sgp4");
}

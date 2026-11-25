/* Launch visibility from Raleigh, NC for typical cases */
#include "check.h"
#include "launchvis.h"
#include <stdio.h>
#include <string.h>

static time_t utc(int y, int mo, int d, int h, int mi)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi;
    return timegm(&t);
}

int main(void)
{
    const double HL = 35.6928, HO = -78.727, CL = 28.5618, CO = -80.5772;
    pic_launchvis_t v;
    double az;

    /* Azimuths from orbit */
    pic_launchvis_azimuth(CL, CO, "LEO", "Crew-14", "Falcon 9", &az);
    CHECK(az > 42 && az < 48, "ISS from the Cape: az %.1f", az);
    pic_launchvis_azimuth(CL, CO, "LEO", "Starlink Group 6-88", "Falcon 9", &az);
    CHECK(az > 52 && az < 60, "Starlink 43 deg shell: az %.1f", az);
    pic_launchvis_azimuth(CL, CO, "GTO", "SES", "Falcon 9", &az);
    CHECK(az > 85 && az < 100, "GTO: az %.1f", az);
    pic_launchvis_azimuth(37.83, -75.49, "LEO", "Cygnus NG-30", "Antares", &az);
    CHECK(az > 120 && az < 140, "ISS from Wallops goes south-east: az %.1f", az);

    /* Night Starlink: visible low in the south a few minutes in */
    pic_launchvis_predict(CL, CO, "LEO", "Starlink Group 6-88", "Falcon 9",
                          utc(2026, 11, 10, 6, 0), HL, HO, &v);
    CHECK(v.visible && !v.daytime, "night Starlink visible (%d)", v.visible);
    CHECK(v.t_start > 120 && v.t_start < 300, "comes into view at T+%d s", v.t_start);
    CHECK(v.max_el > 8 && v.max_el < 30, "max elevation %.1f", v.max_el);
    CHECK(v.az_start > 150 && v.az_start < 210, "first seen at az %.0f (south)", v.az_start);
    /* Dusk: plume lit by the sun */
    pic_launchvis_predict(CL, CO, "LEO", "Starlink Group 6-88", "Falcon 9",
                          utc(2026, 11, 10, 22, 50), HL, HO, &v);
    CHECK(v.visible && v.sunlit, "dusk launch: sunlit plume (%d %d)", v.visible, v.sunlit);
    /* Noon: daytime */
    pic_launchvis_predict(CL, CO, "LEO", "Starlink Group 6-88", "Falcon 9",
                          utc(2026, 11, 10, 17, 0), HL, HO, &v);
    CHECK(!v.visible && v.daytime, "noon launch: daytime, not visible");
    /* Polar from the Cape heads south: never above the horizon here */
    pic_launchvis_predict(CL, CO, "SSO", "Transporter", "Falcon 9",
                          utc(2026, 11, 10, 2, 0), HL, HO, &v);
    CHECK(!v.visible, "polar launch not visible");
    /* Learning: families, a learned inclination changes the azimuth,
     * and the table round-trips through a file */
    {
        char fam[64], fam2[64];
        double inc;
        pic_launchvis_family("Falcon 9 Block 5", "Starlink Group 10-30", fam, sizeof(fam));
        CHECK(!strcmp(fam, "Falcon 9|Starlink Group 10"), "family %s", fam);
        pic_launchvis_family("Falcon 9", "Bandwagon 5 (Dedicated Mid-Inclination Rideshare)", fam2, sizeof(fam2));
        CHECK(!strcmp(fam2, "Falcon 9|Bandwagon"), "family %s", fam2);
        CHECK(!pic_launchvis_learned(fam2, &inc), "nothing learned yet");
        pic_launchvis_azimuth(CL, CO, "LEO", "Bandwagon 5", "Falcon 9", &az);
        CHECK(az > 50 && az < 58, "Bandwagon guessed az %.1f", az);
        pic_launchvis_learn(fam2, 53.0, 3);
        pic_launchvis_learn(fam2, 53.2, 1);
        CHECK(pic_launchvis_learned(fam2, &inc) && inc > 52.9 && inc < 53.2, "learned %.2f", inc);
        pic_launchvis_azimuth(CL, CO, "LEO", "Bandwagon 5", "Falcon 9", &az);
        CHECK(az > 41 && az < 46, "Bandwagon learned az %.1f (53 deg orbit)", az);
        CHECK(pic_launchvis_save("/tmp/pic_test_inc.txt") == 0, "save");
        pic_launchvis_learn("X|Y", 97.0, 2);
        CHECK(pic_launchvis_load("/tmp/pic_test_inc.txt") >= 1, "load");
        CHECK(!pic_launchvis_learned("X|Y", &inc), "load replaces the table");
        CHECK(pic_launchvis_learned(fam2, &inc) && inc > 52.9, "loaded value %.2f", inc);
        pic_launchvis_learn("X|Y", 97.0, 2);
        pic_launchvis_azimuth(CL, CO, "LEO", "Y 1", "X", &az);
        CHECK(az > 150, "learned polar family heads south: az %.0f", az);
        remove("/tmp/pic_test_inc.txt");
    }

    /* Elevation of a point overhead is 90 */
    CHECK(pic_launchvis_look(HL, HO, HL, HO, 200, NULL) > 89.9, "overhead elevation");
    return CHECK_DONE("test_launchvis");
}

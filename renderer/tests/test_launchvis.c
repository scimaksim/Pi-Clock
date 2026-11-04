/* Launch visibility from Raleigh, NC for typical cases */
#include "check.h"
#include "launchvis.h"
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
    /* Elevation of a point overhead is 90 */
    CHECK(pic_launchvis_look(HL, HO, HL, HO, 200, NULL) > 89.9, "overhead elevation");
    return CHECK_DONE("test_launchvis");
}

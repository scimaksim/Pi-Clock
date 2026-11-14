/* People aboard the stations: LL2 space-station reply parsing */
#include "check.h"
#include "features_all.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    static char buf[65536];
    FILE *f = fopen("data/stations.json", "r");
    size_t n;
    CHECK(f != NULL, "fixture");
    if (!f) return 1;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    CHECK(pic_crew_aboard("ISS") == -1, "unknown before parsing");
    CHECK(pic_crew_parse(buf, n) == 2, "two stations parsed");
    CHECK(pic_crew_aboard("ISS") == 7, "ISS %d", pic_crew_aboard("ISS"));
    CHECK(pic_crew_aboard("tiangong") == 3, "Tiangong %d (case-insensitive)", pic_crew_aboard("tiangong"));
    CHECK(pic_crew_aboard("Mir") == -1, "unknown station");
    CHECK(pic_crew_parse("not json", 8) < 0, "bad input rejected");
    CHECK(pic_crew_aboard("ISS") == 7, "bad input keeps the old counts");
    return CHECK_DONE("test_crew");
}

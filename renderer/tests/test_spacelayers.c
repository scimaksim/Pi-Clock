/*
 * Space-Track layer parsing: re-entry messages, close approaches,
 * debris groups, catalogue totals, the re-entry path and names.
 * All replies here are made up (no Space-Track data in the repo).
 */
#include "check.h"
#include "spacelayers.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    time_t now = sl_parse_time("2026-11-08 17:40:00", NULL);
    double frac;

    /* times */
    CHECK(now == 1794159600, "now %ld", (long)now);
    CHECK(sl_parse_time("2026-11-08 4:56:00", NULL) == now - 12 * 3600 - 44 * 60, "unpadded hour");
    CHECK(sl_parse_time("2026-11-10T12:23:56.101000", &frac) == now + 2 * 86400 - 5 * 3600 - 16 * 60 - 4 &&
          fabs(frac - 0.101) < 1e-6, "T + fraction");
    CHECK(sl_parse_time("garbage", NULL) == 0, "bad time");

    {   /* TIP: newest message per object, recent past kept, sorted */
        static sl_reentry_t re[SL_MAX_RE];
        const char *js = "["
            "{\"NORAD_CAT_ID\":\"90001\",\"MSG_EPOCH\":\"2026-11-07 10:00:00\",\"DECAY_EPOCH\":\"2026-11-10 01:00:00\","
            "\"WINDOW\":\"900\",\"LAT\":\"10\",\"LON\":\"300\",\"INCL\":\"53\",\"DIRECTION\":\"descending\"},"
            "{\"NORAD_CAT_ID\":\"90001\",\"MSG_EPOCH\":\"2026-11-08 10:00:00\",\"DECAY_EPOCH\":\"2026-11-10 02:00:00\","
            "\"WINDOW\":\"660\",\"LAT\":\"31.8\",\"LON\":\"335.9\",\"INCL\":\"53\",\"DIRECTION\":\"ascending\"},"
            "{\"NORAD_CAT_ID\":\"90002\",\"MSG_EPOCH\":\"2026-11-08 02:00:00\",\"DECAY_EPOCH\":\"2026-11-08 4:56:00\","
            "\"WINDOW\":\"23\",\"LAT\":\"-29.3\",\"LON\":\"285.7\",\"INCL\":\"53.2\",\"DIRECTION\":\"descending\"},"
            "{\"NORAD_CAT_ID\":\"90003\",\"MSG_EPOCH\":\"2026-11-05 02:00:00\",\"DECAY_EPOCH\":\"2026-11-06 01:44:00\","
            "\"WINDOW\":\"16\",\"LAT\":\"-6.5\",\"LON\":\"192.1\",\"INCL\":\"43\",\"DIRECTION\":\"descending\"}]";
        int n = sl_parse_tip(js, now, SL_RE_KEEP, re, SL_MAX_RE);
        CHECK(n == 2, "tip count %d", n);
        CHECK(re[0].id == 90002 && re[0].window_min == 23 && re[0].descending, "past one first");
        CHECK(fabs(re[0].lon - (285.7 - 360)) < 1e-9, "lon wrapped %f", re[0].lon);
        CHECK(re[1].id == 90001 && re[1].window_min == 660 && !re[1].descending &&
              re[1].decay == sl_parse_time("2026-11-10 02:00:00", NULL), "newest message wins");
        CHECK(sl_parse_tip("not json", now, SL_RE_KEEP, re, SL_MAX_RE) == 0, "bad json");
    }

    {   /* CDM: newest message per pair (either order), 6 h window, by TCA */
        static sl_conj_t cj[SL_MAX_CJ];
        const char *js = "["
            "{\"CREATED\":\"2026-11-08 01:00:00\",\"TCA\":\"2026-11-08T21:58:27.712000\",\"MIN_RNG\":\"120\",\"PC\":\"0.0002\","
            "\"SAT_1_ID\":\"1\",\"SAT_1_NAME\":\"ALPHA\",\"SAT_2_ID\":\"2\",\"SAT_2_NAME\":\"BETA DEB\"},"
            "{\"CREATED\":\"2026-11-08 09:00:00\",\"TCA\":\"2026-11-08T21:58:27.712000\",\"MIN_RNG\":\"39\",\"PC\":\"0.0001249477\","
            "\"SAT_1_ID\":\"2\",\"SAT_1_NAME\":\"BETA DEB\",\"SAT_2_ID\":\"1\",\"SAT_2_NAME\":\"ALPHA\"},"
            "{\"CREATED\":\"2026-11-08 09:00:00\",\"TCA\":\"2026-11-08T18:32:44.299000\",\"MIN_RNG\":\"249\",\"PC\":null,"
            "\"SAT_1_ID\":\"3\",\"SAT_1_NAME\":\"GAMMA\",\"SAT_2_ID\":\"4\",\"SAT_2_NAME\":\"DELTA R/B\"},"
            "{\"CREATED\":\"2026-11-08 09:00:00\",\"TCA\":\"2026-11-09T05:28:54\",\"MIN_RNG\":\"88\",\"PC\":\"0.0002\","
            "\"SAT_1_ID\":\"5\",\"SAT_1_NAME\":\"LATER\",\"SAT_2_ID\":\"6\",\"SAT_2_NAME\":\"LATER2\"},"
            "{\"CREATED\":\"2026-11-08 09:00:00\",\"TCA\":\"2026-11-08T17:00:00\",\"MIN_RNG\":\"88\",\"PC\":\"0.0002\","
            "\"SAT_1_ID\":\"7\",\"SAT_1_NAME\":\"PAST\",\"SAT_2_ID\":\"8\",\"SAT_2_NAME\":\"PAST2\"}]";
        int n = sl_parse_cdm(js, now, SL_CJ_HOURS, cj, SL_MAX_CJ);
        CHECK(n == 2, "cdm count %d", n);
        CHECK(cj[0].id1 == 3 && cj[0].pc == 0 && cj[0].rng_m == 249, "soonest first, null Pc");
        CHECK(cj[1].rng_m == 39 && fabs(cj[1].pc - 0.0001249477) < 1e-12 && cj[1].id1 == 2,
              "newest message for the pair");
        CHECK(fabs(cj[1].tca_frac - 0.712) < 1e-6, "fraction");
    }

    {   /* debris groups: newest first, decayed pieces left out, ids */
        sl_group_t g[SL_MAX_GRP];
        char ids[256];
        const char *js = "["
            "{\"NORAD_CAT_ID\":\"101263\",\"OBJECT_ID\":\"1981-043AF\",\"OBJECT_NAME\":\"OLDSAT 2-7 DEB\",\"COUNTRY\":\"CIS\",\"LAUNCH\":\"1981-05-14\",\"DECAY\":null},"
            "{\"NORAD_CAT_ID\":\"101262\",\"OBJECT_ID\":\"1981-043AE\",\"OBJECT_NAME\":\"OLDSAT 2-7 DEB\",\"COUNTRY\":\"CIS\",\"LAUNCH\":\"1981-05-14\",\"DECAY\":null},"
            "{\"NORAD_CAT_ID\":\"101261\",\"OBJECT_ID\":\"1981-043AD\",\"OBJECT_NAME\":\"OLDSAT 2-7 DEB\",\"COUNTRY\":\"CIS\",\"LAUNCH\":\"1981-05-14\",\"DECAY\":null},"
            "{\"NORAD_CAT_ID\":\"101260\",\"OBJECT_ID\":\"1981-043AC\",\"OBJECT_NAME\":\"OLDSAT 2-7 DEB\",\"COUNTRY\":\"CIS\",\"LAUNCH\":\"1981-05-14\",\"DECAY\":null},"
            "{\"NORAD_CAT_ID\":\"101100\",\"OBJECT_ID\":\"2026-051C\",\"OBJECT_NAME\":\"NEWSAT DEB\",\"COUNTRY\":\"PRC\",\"LAUNCH\":\"2026-03-15\",\"DECAY\":null},"
            "{\"NORAD_CAT_ID\":\"101099\",\"OBJECT_ID\":\"2026-051D\",\"OBJECT_NAME\":\"NEWSAT DEB\",\"COUNTRY\":\"PRC\",\"LAUNCH\":\"2026-03-15\",\"DECAY\":\"2026-10-01\"},"
            "{\"NORAD_CAT_ID\":\"101050\",\"OBJECT_ID\":\"2026-051E\",\"OBJECT_NAME\":\"NEWSAT DEB\",\"COUNTRY\":\"PRC\",\"LAUNCH\":\"2026-03-15\",\"DECAY\":null},"
            "{\"NORAD_CAT_ID\":\"100900\",\"OBJECT_ID\":\"2026-051F\",\"OBJECT_NAME\":\"NEWSAT DEB\",\"COUNTRY\":\"PRC\",\"LAUNCH\":\"2026-03-15\",\"DECAY\":null},"
            "{\"NORAD_CAT_ID\":\"100800\",\"OBJECT_ID\":\"2026-051G\",\"OBJECT_NAME\":\"NEWSAT DEB\",\"COUNTRY\":\"PRC\",\"LAUNCH\":\"2026-03-15\",\"DECAY\":null},"
            "{\"NORAD_CAT_ID\":\"99000\",\"OBJECT_ID\":\"2020-001A\",\"OBJECT_NAME\":\"LONE DEB\",\"COUNTRY\":\"US\",\"LAUNCH\":\"2020-01-01\",\"DECAY\":null}]";
        int n = sl_debris_groups(js, 4, g, SL_MAX_GRP);
        CHECK(n == 2, "groups %d", n);
        CHECK(strcmp(g[0].key, "1981-043") == 0 && g[0].count == 4 && g[0].newest == 101263 &&
              strcmp(g[0].name, "OLDSAT 2-7") == 0 && g[0].year == 1981, "newest group");
        CHECK(strcmp(g[1].key, "2026-051") == 0 && g[1].count == 4 && strcmp(g[1].country, "PRC") == 0,
              "decayed piece not counted (%d)", g[1].count);
        sl_debris_ids(js, g, 1, ids, sizeof(ids));
        CHECK(strcmp(ids, "101263,101262,101261,101260") == 0, "ids %s", ids);
        sl_debris_ids(js, g + 1, 1, ids, sizeof(ids));
        CHECK(strstr(ids, "101099") == NULL && strstr(ids, "101100") != NULL, "decayed id left out %s", ids);
    }

    {   /* totals and the week's re-entries (one per object) */
        sl_stats_t s;
        const char *box = "[{\"COUNTRY\":\"US\",\"ORBITAL_TOTAL_COUNT\":\"10\"},"
            "{\"COUNTRY\":\"ALL\",\"ORBITAL_PAYLOAD_COUNT\":\"19870\",\"ORBITAL_ROCKET_BODY_COUNT\":\"2434\","
            "\"ORBITAL_DEBRIS_COUNT\":\"12545\",\"ORBITAL_TOTAL_COUNT\":\"35526\"}]";
        const char *dec = "[{\"NORAD_CAT_ID\":\"1\",\"DECAY_EPOCH\":\"2026-11-09 0:00:00\"},"
            "{\"NORAD_CAT_ID\":\"1\",\"DECAY_EPOCH\":\"2026-11-09 0:00:00\"},"
            "{\"NORAD_CAT_ID\":\"2\",\"DECAY_EPOCH\":\"2026-11-14 0:00:00\"},"
            "{\"NORAD_CAT_ID\":\"3\",\"DECAY_EPOCH\":\"2026-11-20 0:00:00\"},"
            "{\"NORAD_CAT_ID\":\"4\",\"DECAY_EPOCH\":\"2026-11-08 0:00:00\"}]";
        CHECK(sl_parse_stats(box, dec, now, &s) == 1, "stats ok");
        CHECK(s.total == 35526 && s.payload == 19870 && s.rocket == 2434 && s.debris == 12545, "totals");
        CHECK(s.week == 2, "week %d", s.week);
    }

    {   /* re-entry path: through the point at 0, south-going if descending */
        double la, lo, la2, lo2, maxl = 0;
        int k;
        sl_incl_track(-29.3, -74.3, 53.2, 1, 150, 0, &la, &lo);
        CHECK(fabs(la + 29.3) < 1e-6 && fabs(lo + 74.3) < 1e-6, "passes the point (%f %f)", la, lo);
        sl_incl_track(-29.3, -74.3, 53.2, 1, 150, 1, &la2, &lo2);
        CHECK(la2 < la && lo2 > lo, "descending heads south-east (%f %f)", la2, lo2);
        sl_incl_track(-29.3, -74.3, 53.2, 0, 150, 1, &la2, &lo2);
        CHECK(la2 > la, "ascending heads north");
        for (k = -60; k <= 60; k++) {
            sl_incl_track(10, 20, 53.2, 0, 150, k, &la, &lo);
            if (fabs(la) > maxl) maxl = fabs(la);
        }
        CHECK(maxl <= 53.2 + 1e-6 && maxl > 50, "latitude bounded by inclination %f", maxl);
        /* about 7.8 km/s along the ground at 150 km: ~470 km a minute */
        sl_incl_track(0, 0, 53.2, 0, 150, 1, &la, &lo);
        {
            double d = 6371 * acos(cos(la * M_PI / 180) * cos(lo * M_PI / 180));
            CHECK(d > 440 && d < 480, "ground speed %f km/min", d);
        }
    }

    {   /* names */
        char o[48];
        sl_tidy_name("FENGYUN 1C DEB", o, sizeof(o));
        CHECK(strcmp(o, "Fengyun 1C debris") == 0, "%s", o);
        sl_tidy_name("SL-8 R/B", o, sizeof(o));
        CHECK(strcmp(o, "SL-8 rocket body") == 0, "%s", o);
        sl_tidy_name("STARLINK-4607", o, sizeof(o));
        CHECK(strcmp(o, "Starlink-4607") == 0, "%s", o);
        sl_tidy_name("NOAA 16 DEB", o, sizeof(o));
        CHECK(strcmp(o, "NOAA 16 debris") == 0, "%s", o);
        sl_tidy_name("GOES 19", o, sizeof(o));
        CHECK(strcmp(o, "GOES 19") == 0, "%s", o);
    }
    return CHECK_DONE("test_spacelayers");
}

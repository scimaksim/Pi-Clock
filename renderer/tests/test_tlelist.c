/*
 * The overhead layer's TLE list: stale and decaying elements dropped,
 * the newest copy of a satellite kept (with its real name), and
 * unidentified pieces labelled with their COSPAR ID.
 */
#include "check.h"
#include "../layers_overhead.c"

static void tle_line1(char *out, int id, const char *intl, double days_ago)
{
    double jd = time(NULL) / 86400.0 + 2440587.5 - days_ago;
    /* epoch as YYDDD.DDDDDDDD for the current year */
    time_t t = (time_t)((jd - 2440587.5) * 86400.0);
    struct tm g;
    char ep[20];
    gmtime_r(&t, &g);
    snprintf(ep, sizeof(ep), "%02d%012.8f", g.tm_year % 100,
             g.tm_yday + 1 + (g.tm_hour * 3600 + g.tm_min * 60 + g.tm_sec) / 86400.0);
    snprintf(out, 80, "1 %05dU %-8s %s  .00001000  00000-0  10000-3 0  9990", id, intl, ep);
}

static void tle_line2(char *out, int id, double mm)
{
    snprintf(out, 80, "2 %05d  97.5000 100.0000 0010000  90.0000 270.0000 %11.8f123450", id, mm);
}

int main(void)
{
    static ov_sat_t L[64];
    FILE *f = fopen("/tmp/pic_test_a.tle", "w"), *g;
    char a[100], b[100];
    int n = 0, i;
    struct { const char *name, *intl; int id; double age, mm; } A[] = {
        { "OBJECT AB", "98067AB", 40001, 2, 15.2 },
        { "OBJECT E", "22167E", 40002, 1, 15.2 },
        { "REALSAT", "20061F", 40003, 5, 15.2 },
        { "OLDSAT", "21001A", 40004, 30, 15.2 },      /* stale: dropped */
        { "DECAYING", "24001B", 40005, 5, 16.3 },      /* re-entered: dropped */
        { "OBJECT ABC", "23091ABC", 40006, 1, 15.2 },
        { "GEO SAT", "20001A", 40007, 1, 1.0027 },     /* not LEO: dropped */
    };
    for (i = 0; i < (int)(sizeof(A) / sizeof(A[0])); i++) {
        tle_line1(a, A[i].id, A[i].intl, A[i].age);
        tle_line2(b, A[i].id, A[i].mm);
        fprintf(f, "%s\n%s\n%s\n", A[i].name, a, b);
    }
    fclose(f);
    g = fopen("/tmp/pic_test_b.tle", "w");
    tle_line1(a, 40003, "20061F", 1); tle_line2(b, 40003, 15.2);
    fprintf(g, "OBJECT F\n%s\n%s\n", a, b);       /* newer, unnamed copy */
    tle_line1(a, 40002, "22167E", 3); tle_line2(b, 40002, 15.2);
    fprintf(g, "REALSAT2\n%s\n%s\n", a, b);       /* older copy: ignored */
    fclose(g);

    n = load_tles("/tmp/pic_test_a.tle", L, n, 64);
    n = load_tles("/tmp/pic_test_b.tle", L, n, 64);
    CHECK(n == 4, "expected 4 satellites, got %d", n);
    for (i = 0; i < n; i++) {
        double age = time(NULL) / 86400.0 + 2440587.5 - L[i].sat.epoch_jd;
        switch (L[i].sat.norad_id) {
        case 40001: CHECK(!strcmp(L[i].label, "Object 1998-067AB"), "label %s", L[i].label); break;
        case 40002: CHECK(!strcmp(L[i].label, "Object 2022-167E"), "label %s", L[i].label);
                    CHECK(age < 1.5, "40002 kept the older copy (%.1f d)", age); break;
        case 40003: CHECK(!strcmp(L[i].label, "Realsat"), "real name lost: %s", L[i].label);
                    CHECK(age < 1.5, "40003 not updated to the newer copy (%.1f d)", age); break;
        case 40006: CHECK(!strcmp(L[i].label, "Object 2023-091ABC"), "label %s", L[i].label); break;
        default:    CHECK(0, "unexpected satellite %d", L[i].sat.norad_id);
        }
    }
    unlink("/tmp/pic_test_a.tle");
    unlink("/tmp/pic_test_b.tle");
    return CHECK_DONE("test_tlelist");
}

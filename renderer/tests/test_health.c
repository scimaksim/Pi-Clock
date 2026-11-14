/* Vital-signs log: a start line, samples, the summary, and trimming */
#include "check.h"
#define PIC_HEALTH_TEST_PATH "/tmp/pic_test_health.csv"
#include "../health.c"

int main(void)
{
    pic_health_t h;
    FILE *f;
    char line[256];
    int lines = 0, i;
    time_t now = time(NULL);

    unlink(PIC_HEALTH_PATH);
    health_work(NULL);                         /* start line */
    sleep(1);
    health_work(NULL);                         /* first sample */
    f = fopen(PIC_HEALTH_PATH, "r");
    CHECK(f != NULL, "log written");
    while (f && fgets(line, sizeof(line), f)) {
        lines++;
        if (lines == 1) CHECK(!strncmp(line, "start,", 6), "first line is a start: %s", line);
        if (lines == 2) {
            long t, rss, avail;
            double temp, cpu;
            CHECK(sscanf(line, "t,%ld,%ld,%ld,%lf,%lf", &t, &rss, &avail, &temp, &cpu) == 5,
                  "sample parses: %s", line);
            CHECK(rss > 0 && avail > 0, "memory figures %ld / %ld", rss, avail);
            CHECK(cpu >= 0 && cpu <= 400, "cpu %.1f", cpu);
        }
    }
    if (f) fclose(f);
    CHECK(lines == 2, "%d lines", lines);
    CHECK(pic_health_summary(now, &h) == 1, "summary available");
    CHECK(h.restarts_24h == 1, "restarts %d", h.restarts_24h);
    CHECK(h.avail_mb > 0 && h.rss_mb > 0, "summary memory");

    /* Two more starts within a day: a restart warning case */
    f = fopen(PIC_HEALTH_PATH, "a");
    fprintf(f, "start,%ld,x\nstart,%ld,x\nstart,%ld,x\n", (long)(now - 3600), (long)(now - 90000), (long)(now - 200));
    fclose(f);
    pic_health_summary(now, &h);
    CHECK(h.restarts_24h == 3, "restarts in 24 h: %d (old one excluded)", h.restarts_24h);

    /* Trimming keeps the file bounded */
    f = fopen(PIC_HEALTH_PATH, "a");
    for (i = 0; i < HEALTH_MAX_LINES + 500; i++)
        fprintf(f, "t,%ld,400,300,50.0,30.0,0.50,2000,200,%d\n", (long)(now - 86400 * 8 + i * 60), i);
    fclose(f);
    trim();
    lines = 0;
    f = fopen(PIC_HEALTH_PATH, "r");
    while (fgets(line, sizeof(line), f)) lines++;
    fclose(f);
    CHECK(lines <= HEALTH_MAX_LINES, "trimmed to %d lines (max %d)", lines, HEALTH_MAX_LINES);
    unlink(PIC_HEALTH_PATH);
    return CHECK_DONE("test_health");
}

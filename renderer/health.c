/*
 * health.c - A week of the Pi's own vital signs
 *
 * Every HEALTH_STEP seconds a line goes to PIC_HEALTH_PATH (on /data,
 * so it survives reboots): time, renderer memory, system memory
 * available, CPU temperature, renderer CPU share since the last
 * sample, 1-minute load, free space on /data and /tmp, and the
 * renderer's uptime. A "start" line marks each renderer start. The
 * file is trimmed to HEALTH_DAYS days.
 *
 * The dashboard's Health card reads it (/api/health), and the Home
 * Assistant notifier uses it to warn about repeated restarts, high
 * temperature and low memory (pic_health_summary).
 *
 * Format (CSV, one record a line):
 *   t,<unix>,rss_mb,avail_mb,temp_c,cpu_pct,load1,data_free_mb,tmp_free_mb,uptime_s
 *   start,<unix>,<version>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "feature.h"
#include "features_all.h"
#ifdef PIC_HEALTH_TEST_PATH
#undef PIC_HEALTH_PATH
#define PIC_HEALTH_PATH PIC_HEALTH_TEST_PATH
#endif
#include "config.h"
#ifndef VERSION
#define VERSION "dev"
#endif
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define HEALTH_STEP  300
#define HEALTH_DAYS  7
#define HEALTH_MAX_LINES (HEALTH_DAYS * 86400 / HEALTH_STEP + 200)

static time_t started;
static unsigned long last_ticks;
static time_t last_sample;

static long read_rss_mb(void)
{
    long pages = 0, rss = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = -1;
    fclose(f);
    return rss < 0 ? -1 : rss * (long)sysconf(_SC_PAGESIZE) / (1024 * 1024);
}

static long read_avail_mb(void)
{
    char line[128];
    long kb = -1;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb < 0 ? -1 : kb / 1024;
}

static double read_temp(void)
{
    long mc = -1;
    FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return -1;
    if (fscanf(f, "%ld", &mc) != 1) mc = -1;
    fclose(f);
    return mc < 0 ? -1 : mc / 1000.0;
}

static unsigned long read_ticks(void)
{
    char buf[512], *p;
    unsigned long u = 0, s = 0;
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return 0;
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    fclose(f);
    p = strrchr(buf, ')');               /* skip the command name */
    if (!p) return 0;
    if (sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &u, &s) != 2)
        return 0;
    return u + s;
}

static double read_load1(void)
{
    double l = -1;
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return -1;
    if (fscanf(f, "%lf", &l) != 1) l = -1;
    fclose(f);
    return l;
}

static long free_mb(const char *path)
{
    struct statvfs v;
    if (statvfs(path, &v) != 0) return -1;
    return (long)((unsigned long long)v.f_bavail * v.f_frsize / (1024 * 1024));
}

/* Keep the newest HEALTH_MAX_LINES lines */
static void trim(void)
{
    FILE *f = fopen(PIC_HEALTH_PATH, "r");
    char **lines, line[256];
    int n = 0, cap = HEALTH_MAX_LINES + 1000, i, keep;
    if (!f) return;
    lines = calloc((size_t)cap, sizeof(char *));
    if (!lines) { fclose(f); return; }
    while (n < cap && fgets(line, sizeof(line), f)) lines[n++] = strdup(line);
    fclose(f);
    if (n > HEALTH_MAX_LINES) {
        keep = HEALTH_MAX_LINES * 9 / 10;
        f = fopen(PIC_HEALTH_PATH ".tmp", "w");
        if (f) {
            for (i = n - keep; i < n; i++) if (lines[i]) fputs(lines[i], f);
            fclose(f);
            rename(PIC_HEALTH_PATH ".tmp", PIC_HEALTH_PATH);
        }
    }
    for (i = 0; i < n; i++) free(lines[i]);
    free(lines);
}

static void append(const char *rec)
{
    FILE *f;
    mkdir("/data/cache", 0755);
    f = fopen(PIC_HEALTH_PATH, "a");
    if (!f) return;
    fputs(rec, f);
    fclose(f);
}

/* The service log lives in RAM (/var/log is a 64 MB tmpfs) and nothing
 * rotated it: keep its newest part once it passes LOG_MAX. Only when
 * stdout is a regular file opened for appending, as the service does. */
#define LOG_MAX  (4L * 1024 * 1024)
#define LOG_KEEP (512L * 1024)

static void trim_log(void)
{
    struct stat sb;
    char *tail, *nl;
    int fd;
    ssize_t n;

    fflush(stdout);
    if (fstat(STDOUT_FILENO, &sb) != 0 || !S_ISREG(sb.st_mode) || sb.st_size < LOG_MAX) return;
    if (!(fcntl(STDOUT_FILENO, F_GETFL) & O_APPEND)) return;
    if (!(tail = malloc(LOG_KEEP))) return;
    fd = open("/proc/self/fd/1", O_RDONLY);
    n = fd >= 0 ? pread(fd, tail, LOG_KEEP, sb.st_size - LOG_KEEP) : -1;
    if (fd >= 0) close(fd);
    if (n > 0 && ftruncate(STDOUT_FILENO, 0) == 0) {
        nl = memchr(tail, '\n', (size_t)n);          /* start on a whole line */
        if (nl && nl + 1 < tail + n) {
            ssize_t w = write(STDOUT_FILENO, nl + 1, (size_t)(tail + n - nl - 1));
            (void)w;
        }
        printf("health: log trimmed (was %ld KB)\n", (long)(sb.st_size / 1024));
    }
    free(tail);
}

static int health_work(pic_feature_t *f)
{
    time_t now = time(NULL);
    unsigned long ticks = read_ticks();
    double cpu = -1;
    char rec[256];
    static int samples;

    (void)f;
    if (!started) {
        started = now;
        snprintf(rec, sizeof(rec), "start,%ld,%s\n", (long)now, VERSION);
        append(rec);
        last_ticks = ticks;
        last_sample = now;
        return HEALTH_STEP;
    }
    if (last_sample && now > last_sample && ticks >= last_ticks)
        cpu = 100.0 * (double)(ticks - last_ticks) / (double)sysconf(_SC_CLK_TCK) /
              (double)(now - last_sample);
    last_ticks = ticks;
    last_sample = now;
    snprintf(rec, sizeof(rec), "t,%ld,%ld,%ld,%.1f,%.1f,%.2f,%ld,%ld,%ld\n",
             (long)now, read_rss_mb(), read_avail_mb(), read_temp(), cpu, read_load1(),
             free_mb("/data"), free_mb("/tmp"), (long)(now - started));
    append(rec);
    if (++samples % 48 == 0) trim();     /* every 4 h */
    trim_log();
    return HEALTH_STEP;
}

/* For the Home Assistant notifier: restarts in the last 24 h, latest
 * temperature and memory. 0 if there is no history. */
int pic_health_summary(time_t now, pic_health_t *out)
{
    FILE *f = fopen(PIC_HEALTH_PATH, "r");
    char line[256];
    memset(out, 0, sizeof(*out));
    out->temp_c = -1;
    out->avail_mb = -1;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        long t;
        if (sscanf(line, "start,%ld", &t) == 1) {
            if (now - t < 86400) out->restarts_24h++;
        } else if (!strncmp(line, "t,", 2)) {
            long rss, avail, dfree, tfree, up;
            double temp, cpu, load;
            if (sscanf(line, "t,%ld,%ld,%ld,%lf,%lf,%lf,%ld,%ld,%ld", &t, &rss, &avail,
                       &temp, &cpu, &load, &dfree, &tfree, &up) == 9 && now - t < 3600) {
                out->temp_c = temp;
                out->avail_mb = avail;
                out->rss_mb = rss;
                out->data_free_mb = dfree;
                if (temp > out->max_temp_24h && now - t < 86400) out->max_temp_24h = temp;
            }
        }
    }
    fclose(f);
    return 1;
}

static int health_wanted(pic_feature_t *f) { (void)f; return 1; }   /* always */

static pic_feature_t feature_def = {
    .name = "health", .work = health_work, .wanted = health_wanted, .interval = 0,
};

pic_feature_t *pic_feature_health(void)
{
    return &feature_def;
}

/*
 * gcat.c - End-of-life dates from Jonathan McDowell's General Catalog
 * of Artificial Space Objects (GCAT), https://planet4589.org/space/gcat
 *
 * The payload catalogue lists every satellite with the date it stopped
 * transmitting (TDate) and the date regular operations ended (TOp);
 * "*" in either means "still going". It is updated every few days and
 * covers new launches, so nothing here is a fixed list. Two files are
 * read: psatcat (catalogue numbers up to 99999) and psatcat100k (above).
 * A satellite GCAT doesn't list gets no note.
 */
#include "gcat.h"
#include "fetch.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define GCAT_DIR     "/data/cache/pi-clock-gcat"
#define GCAT_URL     "https://planet4589.org/space/gcat/tsv/cat/"
#define GCAT_REFRESH (3 * 86400)
#define GCAT_RETRY   (6 * 3600)
#define GCAT_LINE    2048

static const char *const FILES[2] = { "psatcat.tsv", "psatcat100k.tsv" };
static const long MIN_SIZE[2] = { 1000000, 1000 };

static int refresh_one(int k)
{
    char path[128], tmp[136], url[160], head[64] = "";
    struct stat st;
    time_t now = time(NULL);
    FILE *f;
    int have;

    snprintf(path, sizeof(path), GCAT_DIR "/%s", FILES[k]);
    have = stat(path, &st) == 0 && st.st_size >= MIN_SIZE[k];
    if (have && now - st.st_mtime < GCAT_REFRESH && now >= st.st_mtime) return 1;
    snprintf(tmp, sizeof(tmp), "%s.new", path);
    snprintf(url, sizeof(url), GCAT_URL "%s", FILES[k]);
    if (pic_fetch_to_file(url, tmp, 180) == 0 && stat(tmp, &st) == 0 &&
        st.st_size >= MIN_SIZE[k] && (f = fopen(tmp, "r")) != NULL) {
        /* a real catalogue starts "#JCAT": an error page doesn't */
        if (!fgets(head, sizeof(head), f)) head[0] = '\0';
        fclose(f);
        if (!strncmp(head, "#JCAT", 5) && rename(tmp, path) == 0) {
            printf("gcat: %s updated (%ld bytes)\n", FILES[k], (long)st.st_size);
            return 1;
        }
    }
    unlink(tmp);
    if (have) {   /* keep the old copy; try again in GCAT_RETRY */
        struct utimbuf t;
        t.actime = t.modtime = now - GCAT_REFRESH + GCAT_RETRY;
        utime(path, &t);
    }
    return have;
}

int pic_gcat_refresh(void)
{
    mkdir("/data/cache", 0755);
    mkdir(GCAT_DIR, 0755);
    return refresh_one(0) + refresh_one(1);
}

/* The k-th tab-separated field of line, trimmed, into out. 0 if present. */
static int field(const char *line, int k, char *out, size_t n)
{
    const char *p = line, *e;
    size_t len;
    while (k-- > 0) {
        p = strchr(p, '\t');
        if (!p) return -1;
        p++;
    }
    e = p + strcspn(p, "\t\r\n");
    while (p < e && *p == ' ') p++;
    while (e > p && e[-1] == ' ') e--;
    len = (size_t)(e - p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

int pic_gcat_header(const char *line, pic_gcat_cols_t *c)
{
    char name[32];
    int k;
    c->top = c->tdate = c->tf = -1;
    c->max = 0;
    if (strncmp(line, "#JCAT", 5) != 0) return -1;
    for (k = 1; k < 64 && field(line, k, name, sizeof(name)) == 0; k++) {
        if (!strcmp(name, "TOp")) c->top = k;
        else if (!strcmp(name, "TDate")) c->tdate = k;
        else if (!strcmp(name, "TF")) c->tf = k;
    }
    if (c->top < 0 || c->tdate < 0) return -1;
    c->max = c->top > c->tdate ? c->top : c->tdate;
    if (c->tf > c->max) c->max = c->tf;
    return 0;
}

/*
 * GCAT "vague date" -> "Apr 2012", "2020", "1990s". Forms seen:
 * "2012 Apr  8", "2012 Apr  8 2130", "2020 Jul", "2019 Q2", "2020",
 * "1990s", each with an optional "?" for uncertain. Returns 0 on
 * success with *unsure set, -1 for anything that isn't a date.
 */
static int vague_date(const char *v, char *out, size_t n, int *unsure)
{
    static const char MON[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int year, k;
    const char *p = v;
    time_t now = time(NULL);
    struct tm tm;

    *unsure = strchr(v, '?') != NULL;
    for (k = 0; k < 4; k++)
        if (!isdigit((unsigned char)p[k])) return -1;
    year = atoi(p);
    gmtime_r(&now, &tm);
    if (year < 1957 || year > tm.tm_year + 1900) return -1;
    p += 4;
    if (*p == 's') {                       /* a decade */
        snprintf(out, n, "%ds", year);
        return 0;
    }
    if (*p == ' ' && isalpha((unsigned char)p[1]) && isalpha((unsigned char)p[2]) &&
        isalpha((unsigned char)p[3])) {
        for (k = 0; k < 12; k++)
            if (!strncmp(p + 1, MON + 3 * k, 3)) {
                snprintf(out, n, "%.3s %d", MON + 3 * k, year);
                return 0;
            }
    }
    snprintf(out, n, "%d", year);          /* year only, or a quarter */
    return 0;
}

int pic_gcat_row(const char *line, const pic_gcat_cols_t *c, int *norad,
                 char *note, size_t n)
{
    char top[40], td[40], tf[8] = "", when[24];
    const char *p = line + 1, *word;
    int unsure, id;

    if (line[0] != 'S' || !isdigit((unsigned char)line[1])) return 0;
    for (id = 0; isdigit((unsigned char)*p) && id < 100000000; p++)
        id = id * 10 + (*p - '0');
    if (*p != ' ' && *p != '\t') return 0;   /* "S12345A": not a plain number */
    if (id <= 0) return 0;
    if (field(line, c->top, top, sizeof(top)) != 0 ||
        field(line, c->tdate, td, sizeof(td)) != 0) return 0;
    if (c->tf >= 0) field(line, c->tf, tf, sizeof(tf));

    if (strcmp(td, "*") != 0) {
        if (vague_date(td, when, sizeof(when), &unsure) != 0) return 0;
        word = "defunct";
    } else if (strcmp(top, "*") != 0 && tf[0] == 'Z') {
        /* GCAT's "zombie": no longer controlled, beacon still heard.
         * Without that flag an end-of-operations date alone isn't
         * enough: OSCAR 7 has one (1981) and is in daily use. */
        if (vague_date(top, when, sizeof(when), &unsure) != 0) return 0;
        word = "retired";
    } else {
        return 0;
    }
    if (tf[0] == 'X') unsure = 1;            /* GCAT: the date is a guess */
    snprintf(note, n, "%s %s%s", word, unsure ? "about " : "", when);
    *norad = id;
    return 1;
}

int pic_gcat_each_file(const char *path,
                       void (*fn)(int norad, const char *note, void *ud), void *ud)
{
    FILE *fp = fopen(path, "r");
    char *line;
    pic_gcat_cols_t c = { -1, -1, -1, 0 };
    int rows = 0, have = 0;

    if (!fp) return -1;
    line = malloc(GCAT_LINE);
    if (!line) { fclose(fp); return -1; }
    while (fgets(line, GCAT_LINE, fp)) {
        size_t len = strlen(line);
        int id;
        char note[40];
        if (len && line[len - 1] != '\n' && !feof(fp)) {
            /* over-long line: the columns wanted are near the start;
             * drop the rest of it */
            int ch;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') ;
        }
        if (line[0] == '#') {
            if (!have && pic_gcat_header(line, &c) == 0) have = 1;
            continue;
        }
        if (!have) continue;
        rows++;
        if (pic_gcat_row(line, &c, &id, note, sizeof(note))) fn(id, note, ud);
    }
    free(line);
    fclose(fp);
    return have ? rows : -1;
}

int pic_gcat_each(void (*fn)(int norad, const char *note, void *ud), void *ud)
{
    char path[128];
    int k, rows = 0, got = 0;
    for (k = 0; k < 2; k++) {
        int r;
        snprintf(path, sizeof(path), GCAT_DIR "/%s", FILES[k]);
        r = pic_gcat_each_file(path, fn, ud);
        if (r >= 0) { rows += r; got = 1; }
    }
    return got ? rows : -1;
}

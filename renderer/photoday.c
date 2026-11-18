/*
 * photoday.c - Picture of the day view (see photoday.h)
 *
 * The fetch thread reads three Pi-Dash replies every 30 minutes:
 *   /api/apod       NASA APOD (title, explanation, url, copyright)
 *   /api/astrobin   AstroBin Image of the Day, newest first
 *   /api/spacefeed  agency releases; the picture-of-the-week/month
 *                   series are picked out by their image names
 * and keeps each current picture as a JPEG in /data/cache/pi-clock-photoday
 * (fetched through Pi-Dash's /api/img cache). A picture is only shown
 * while it is current: 2 days for daily series, 9 for weekly, 40 for
 * monthly. Credits are shown only when the feed gives one.
 *
 * Drawing: the photo fits the screen over a blurred, dimmed copy of
 * itself, drifting and zooming very slowly (6% over the showing). Tall
 * and square pictures get a caption panel on the right; wide ones a
 * caption across the bottom. The source badge is an aperture whose
 * blades turn.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "photoday.h"
#include "fetch.h"
#include "image.h"
#include "json.h"
#include "localview.h"

/* the ticker bar (display.c; absent in the tests) */
extern double pic_ticker_bar_top __attribute__((weak));

#define PD_DIR "/data/cache/pi-clock-photoday"

/* ── 1. Parsing ─────────────────────────────────────────────────── */

void pic_pd_tidy(char *s)
{
    char *r = s, *w = s;
    int sp = 0;
    while (*r) {
        unsigned char c = (unsigned char)*r++;
        if (isspace(c)) { sp = 1; continue; }
        if (sp && w > s) {
            /* no space before closing punctuation or a closing quote */
            int close = strchr(",.;:!?)]", c) != NULL ||
                        (c == 0xe2 && (unsigned char)r[0] == 0x80 &&
                         ((unsigned char)r[1] == 0x9d || (unsigned char)r[1] == 0x99));
            /* nor after an opening quote or bracket */
            int open = w[-1] == '(' || w[-1] == '[' ||
                       (w - s >= 3 && (unsigned char)w[-3] == 0xe2 &&
                        (unsigned char)w[-2] == 0x80 && (unsigned char)w[-1] == 0x9c);
            if (!close && !open) *w++ = ' ';
        }
        sp = 0;
        *w++ = (char)c;
    }
    *w = '\0';
}

/* A sentence ends at . ! or ? (and any closing quote or bracket) followed
 * by a space and a capital, digit or opening quote, or by the end of the
 * text; not after an initial ("A. Person") or a common abbreviation. A
 * text cut off mid-sentence has no end at its tail. */
int pic_pd_sentences(const char *s, size_t *ends, int max)
{
    static const char *abbr[] = { "Dr", "Mr", "Mrs", "Ms", "St", "Mt", "No", "Fig",
                                  "vs", "approx", "ca", "e.g", "i.e", "et al", "Jr", "Sr",
                                  "Jan", "Feb", "Mar", "Apr", "Jun", "Jul", "Aug", "Sep",
                                  "Sept", "Oct", "Nov", "Dec", NULL };
    size_t i, len = strlen(s);
    int n = 0, a;
    for (i = 0; i < len && n < max; i++) {
        size_t e = i + 1, j;
        char c = s[i];
        if (c != '.' && c != '!' && c != '?') continue;
        /* closing quotes and brackets belong to the sentence */
        for (;;) {
            if (s[e] == '"' || s[e] == ')' || s[e] == '\'') { e++; continue; }
            if ((unsigned char)s[e] == 0xe2 && (unsigned char)s[e + 1] == 0x80 &&
                ((unsigned char)s[e + 2] == 0x9d || (unsigned char)s[e + 2] == 0x99)) { e += 3; continue; }
            break;
        }
        if (s[e] != '\0') {
            unsigned char nx;
            if (s[e] != ' ') continue;
            nx = (unsigned char)s[e + 1];
            if (!(isupper(nx) || isdigit(nx) || nx == '"' || nx == '(' || nx == 0xe2)) continue;
        }
        if (c == '.') {
            /* the word before the full stop */
            j = i;
            while (j > 0 && s[j - 1] != ' ' && s[j - 1] != '(') j--;
            if (i - j == 1 && isupper((unsigned char)s[j])) continue;      /* an initial */
            for (a = 0; abbr[a]; a++) {
                size_t al = strlen(abbr[a]);
                if (i >= al && !strncmp(s + i - al, abbr[a], al) &&
                    (i == al || s[i - al - 1] == ' ' || s[i - al - 1] == '(')) break;
            }
            if (abbr[a]) continue;
        }
        ends[n++] = e;
        i = e - 1;
    }
    return n;
}

int pic_pd_current(const pd_item_t *it, time_t now)
{
    return it->t > 0 && now - it->t <= (time_t)it->max_days * 86400 + 12 * 3600 &&
           it->t - now < 2 * 86400;
}

static time_t date_utc(const char *d)
{
    struct tm tm;
    int y, m, dd;
    if (sscanf(d, "%d-%d-%d", &y, &m, &dd) != 3 || y < 2000 || m < 1 || m > 12 || dd < 1 || dd > 31)
        return 0;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = dd; tm.tm_hour = 12;
    return timegm(&tm);
}

/* A Pi-Dash image path for `u`: kept as is when it is one already,
 * else wrapped in /api/img (full screen width) */
static void img_path(const char *u, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o;
    if (u[0] == '/') {
        const char *w = strstr(u, "&w=");
        if (w && strlen(u) < n - 8) {
            snprintf(out, n, "%.*s&w=1920%s", (int)(w - u), u, w + 3 + strspn(w + 3, "0123456789"));
            return;
        }
        snprintf(out, n, "%s", u);
        return;
    }
    o = (size_t)snprintf(out, n, "/api/img?u=");
    for (; *u && o + 12 < n; u++) {
        unsigned char c = (unsigned char)*u;
        if (isalnum(c) || strchr("-._~", c)) out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    snprintf(out + o, n - o, "&w=1920");
}

static int is_url(const char *s)
{
    return !strncmp(s, "https://", 8) || !strncmp(s, "http://", 7) || s[0] == '/';
}

int pic_pd_parse_apod(const char *js, size_t len, time_t now, pd_item_t *out)
{
    pic_json_t j;
    char d[24], u[600], c[160];
    if (pic_json_parse(&j, js, len) != 0) return -1;
    memset(out, 0, sizeof(*out));
    snprintf(out->key, sizeof(out->key), "apod");
    snprintf(out->src, sizeof(out->src), "NASA Astronomy Picture of the Day");
    out->max_days = 2;
    pic_json_str(&j, pic_json_get(&j, 0, "date"), d, sizeof(d));
    out->t = date_utc(d);
    pic_json_str(&j, pic_json_get(&j, 0, "title"), out->title, sizeof(out->title));
    pic_json_str(&j, pic_json_get(&j, 0, "explanation"), out->text, sizeof(out->text));
    if (!strncmp(out->text, "Explanation:", 12))
        memmove(out->text, out->text + 12, strlen(out->text + 12) + 1);
    {   /* the page footer that sometimes rides along */
        char *tp = strstr(out->text, "Tomorrow's picture");
        if (!tp) tp = strstr(out->text, "Tomorrowâs picture");
        if (tp) *tp = '\0';
    }
    pic_json_str(&j, pic_json_get(&j, 0, "url"), u, sizeof(u));
    if (pic_json_str(&j, pic_json_get(&j, 0, "copyright"), c, sizeof(c)) > 0) {
        pic_pd_tidy(c);
        if (c[0]) snprintf(out->credit, sizeof(out->credit), "Image: %s", c);
    }
    /* videos have no picture to show */
    if (!pic_json_eq(&j, pic_json_get(&j, 0, "media_type"), "image") || !is_url(u)) {
        pic_json_free(&j);
        return 0;
    }
    pic_json_free(&j);
    img_path(u, out->img, sizeof(out->img));
    pic_pd_tidy(out->title);
    pic_pd_tidy(out->text);
    return out->title[0] && pic_pd_current(out, now) ? 1 : 0;
}

int pic_pd_parse_astrobin(const char *js, size_t len, time_t now, pd_item_t *out)
{
    pic_json_t j;
    int it;
    char d[24], u[600], by[96], split[200], mount[96];
    double hours;
    if (pic_json_parse(&j, js, len) != 0) return -1;
    it = pic_json_at(&j, pic_json_get(&j, 0, "items"), 0);
    memset(out, 0, sizeof(*out));
    if (it < 0) { pic_json_free(&j); return 0; }
    snprintf(out->key, sizeof(out->key), "astrobin");
    snprintf(out->src, sizeof(out->src), "AstroBin Image of the Day");
    out->max_days = 2;
    pic_json_str(&j, pic_json_get(&j, it, "date"), d, sizeof(d));
    out->t = date_utc(d);
    pic_json_str(&j, pic_json_get(&j, it, "title"), out->title, sizeof(out->title));
    pic_json_str(&j, pic_json_get(&j, it, "img"), u, sizeof(u));
    if (pic_json_str(&j, pic_json_get(&j, it, "by"), by, sizeof(by)) > 0) {
        pic_pd_tidy(by);
        snprintf(out->credit, sizeof(out->credit), "Image: %s", by);
    }
    /* AstroBin's text is the photographer's own words (often a quote),
     * so the caption gives how the picture was made instead */
    hours = pic_json_num(&j, pic_json_get(&j, it, "hours"), 0);
    pic_json_str(&j, pic_json_get(&j, it, "split"), split, sizeof(split));
    pic_json_str(&j, pic_json_get(&j, it, "mount"), mount, sizeof(mount));
    if (hours > 0)
        snprintf(out->text, sizeof(out->text), "%.4g hours of exposure%s%s%s%s", hours,
                 split[0] ? ": " : "", split, mount[0] ? " \xc2\xb7 " : "", mount);
    pic_json_free(&j);
    if (!is_url(u)) return 0;
    img_path(u, out->img, sizeof(out->img));
    pic_pd_tidy(out->title);
    return out->title[0] && pic_pd_current(out, now) ? 1 : 0;
}

/* Which agency releases are a picture-of-the-day/week/month series */
static const struct {
    const char *key, *src, *in_img, *about, *label;
    int days;
} SERIES[] = {
    { "nasa",    "NASA",    NULL,    "NASA Image of the Day", "NASA Image of the Day",           5 },
    { "eso",     "ESO",     "/potw", NULL,                    "ESO Picture of the Week",         9 },
    { "noirlab", "NOIRLab", "/iotw", NULL,                    "NOIRLab Image of the Week",       9 },
    { "hubblew", "Hubble",  "/potw", NULL,                    "ESA/Hubble Picture of the Week",  9 },
    { "hubblem", "Hubble",  "/potm", NULL,                    "ESA/Hubble Picture of the Month", 40 },
    { "webb",    "Webb",    "/potm", NULL,                    "ESA/Webb Picture of the Month",   40 },
};
#define NSERIES ((int)(sizeof(SERIES) / sizeof(SERIES[0])))

int pic_pd_parse_spacefeed(const char *js, size_t len, time_t now,
                           pd_item_t *out, int max)
{
    pic_json_t j;
    int rel, n, i, k, cnt = 0, best[NSERIES];
    double best_t[NSERIES];
    if (pic_json_parse(&j, js, len) != 0) return -1;
    rel = pic_json_get(&j, 0, "releases");
    n = pic_json_size(&j, rel);
    for (k = 0; k < NSERIES; k++) { best[k] = -1; best_t[k] = 0; }
    for (i = 0; i < n; i++) {
        int r = pic_json_at(&j, rel, i);
        char src[64], img[600], about[96];
        double t = pic_json_num(&j, pic_json_get(&j, r, "t"), 0);
        pic_json_str(&j, pic_json_get(&j, r, "src"), src, sizeof(src));
        pic_json_str(&j, pic_json_get(&j, r, "img"), img, sizeof(img));
        pic_json_str(&j, pic_json_get(&j, r, "about"), about, sizeof(about));
        for (k = 0; k < NSERIES; k++) {
            if (strcmp(src, SERIES[k].src)) continue;
            if (SERIES[k].in_img && !strstr(img, SERIES[k].in_img)) continue;
            if (SERIES[k].about && strncmp(about, SERIES[k].about, strlen(SERIES[k].about))) continue;
            if (t > best_t[k]) { best_t[k] = t; best[k] = r; }
        }
    }
    for (k = 0; k < NSERIES && cnt < max; k++) {
        pd_item_t *it = &out[cnt];
        char img[600];
        if (best[k] < 0) continue;
        memset(it, 0, sizeof(*it));
        snprintf(it->key, sizeof(it->key), "%s", SERIES[k].key);
        snprintf(it->src, sizeof(it->src), "%s", SERIES[k].label);
        it->max_days = SERIES[k].days;
        it->t = (time_t)best_t[k];
        pic_json_str(&j, pic_json_get(&j, best[k], "title"), it->title, sizeof(it->title));
        pic_json_str(&j, pic_json_get(&j, best[k], "text"), it->text, sizeof(it->text));
        pic_json_str(&j, pic_json_get(&j, best[k], "img"), img, sizeof(img));
        if (!is_url(img) || !it->title[0] || !pic_pd_current(it, now)) continue;
        img_path(img, it->img, sizeof(it->img));
        pic_pd_tidy(it->title);
        pic_pd_tidy(it->text);
        cnt++;
    }
    pic_json_free(&j);
    return cnt;
}

/* ── 2. Fetching ────────────────────────────────────────────────── */

static struct {
    pthread_mutex_t m;
    char url[160];
    pd_item_t it[PD_MAX];
    int n;
    unsigned gen;          /* bumped when the list changes */
    int cur;               /* main thread: index shown      */
} pd = { .m = PTHREAD_MUTEX_INITIALIZER, .cur = -1 };

static const char *pd_dir(void)
{
    const char *d = getenv("PIC_PHOTODAY_DIR");   /* the tests */
    return d && d[0] ? d : PD_DIR;
}

static void file_for(const char *key, const char *ext, char *out, size_t n)
{
    snprintf(out, n, "%s/%s.%s", pd_dir(), key, ext);
}

/* The picture's JPEG is on disk and was fetched from this path */
static int have_image(const pd_item_t *it)
{
    char p[96], u[sizeof(it->img)];
    FILE *f;
    struct stat st;
    int ok = 0;
    file_for(it->key, "jpg", p, sizeof(p));
    if (stat(p, &st) != 0 || st.st_size < 1000) return 0;
    file_for(it->key, "url", p, sizeof(p));
    if ((f = fopen(p, "r")) != NULL) {
        ok = fgets(u, sizeof(u), f) && !strcmp(u, it->img);
        fclose(f);
    }
    return ok;
}

static int fetch_image(const char *base, const pd_item_t *it)
{
    char url[800], tmp[96], p[96];
    FILE *f;
    file_for(it->key, "tmp", tmp, sizeof(tmp));
    snprintf(url, sizeof(url), "%s%s", base, it->img);
    if (pic_fetch_to_file(url, tmp, 90) != 0) { unlink(tmp); return -1; }
    /* a picture the clock can decode, at a usable size */
    {
        cairo_surface_t *s = pic_image_load(tmp);
        int ok = s && cairo_image_surface_get_width(s) >= 300 &&
                 cairo_image_surface_get_height(s) >= 200;
        if (s) cairo_surface_destroy(s);
        if (!ok) { unlink(tmp); return -1; }
    }
    file_for(it->key, "jpg", p, sizeof(p));
    if (rename(tmp, p) != 0) { unlink(tmp); return -1; }
    file_for(it->key, "url", p, sizeof(p));
    if ((f = fopen(p, "w")) != NULL) { fputs(it->img, f); fclose(f); }
    return 0;
}

static int pd_work(pic_feature_t *f)
{
    pd_item_t got[PD_MAX];
    char base[160], url[220], *buf;
    int n = 0, k, r, ok = 0, missed = 0;
    time_t now = time(NULL);
    pthread_mutex_lock(&pd.m);
    snprintf(base, sizeof(base), "%s", pd.url);
    pthread_mutex_unlock(&pd.m);
    if (!base[0]) return 600;
    buf = malloc(2 * 1024 * 1024);
    if (!buf) return 300;
    snprintf(url, sizeof(url), "%s/api/apod", base);
    if ((r = pic_fetch_to_buf(url, buf, 2 * 1024 * 1024 - 1)) > 0) {
        buf[r] = '\0';
        ok |= (k = pic_pd_parse_apod(buf, (size_t)r, now, &got[n])) >= 0;
        if (k > 0) n++;
    }
    snprintf(url, sizeof(url), "%s/api/astrobin", base);
    if (!pic_feature_interrupted(f) && (r = pic_fetch_to_buf(url, buf, 2 * 1024 * 1024 - 1)) > 0) {
        buf[r] = '\0';
        ok |= (k = pic_pd_parse_astrobin(buf, (size_t)r, now, &got[n])) >= 0;
        if (k > 0) n++;
    }
    snprintf(url, sizeof(url), "%s/api/spacefeed", base);
    if (!pic_feature_interrupted(f) && (r = pic_fetch_to_buf(url, buf, 2 * 1024 * 1024 - 1)) > 0) {
        buf[r] = '\0';
        ok |= (k = pic_pd_parse_spacefeed(buf, (size_t)r, now, &got[n], PD_MAX - n)) >= 0;
        if (k > 0) n += k;
    }
    free(buf);
    if (!ok) {
        printf("photoday: Pi-Dash server not answering, keeping %d picture(s)\n", pd.n);
        return 600;
    }
    mkdir("/data/cache", 0755);
    mkdir(pd_dir(), 0755);
    /* keep the ones whose picture is on disk */
    for (k = 0; k < n && !pic_feature_interrupted(f); ) {
        if (have_image(&got[k]) || fetch_image(base, &got[k]) == 0) { k++; continue; }
        printf("photoday: %s picture could not be fetched, trying again in 5 min\n", got[k].src);
        missed = 1;
        memmove(&got[k], &got[k + 1], (size_t)(n - k - 1) * sizeof(got[0]));
        n--;
    }
    if (pic_feature_interrupted(f)) return 1;
    pthread_mutex_lock(&pd.m);
    if (n != pd.n || memcmp(got, pd.it, (size_t)n * sizeof(got[0]))) {
        memcpy(pd.it, got, (size_t)n * sizeof(got[0]));
        pd.n = n;
        pd.gen++;
        printf("photoday: %d picture(s) of the day:", n);
        for (k = 0; k < n; k++) printf(" %s", got[k].key);
        printf("\n");
    }
    pthread_mutex_unlock(&pd.m);
    /* Pi-Dash fetches an image from its source on first request, which
     * can outlast the download timeout: ask again soon */
    return missed ? 300 : 1800;
}

static void pd_reload(pic_feature_t *f)
{
    char u[160] = "";
    pic_conf_str("PIDASH_URL", u, sizeof(u));
    pthread_mutex_lock(&pd.m);
    if (strcmp(u, pd.url)) {
        snprintf(pd.url, sizeof(pd.url), "%s", u);
        pic_feature_poke(f);
    }
    pthread_mutex_unlock(&pd.m);
}

static int pd_wanted(pic_feature_t *f)
{
    (void)f;
    return pic_conf_int("VIEW_PHOTO_MIN", 0) > 0;
}

/* View hooks: is there a current picture, and move to the next one */
static int pd_ready(time_t now)
{
    int k, any = 0;
    pthread_mutex_lock(&pd.m);
    for (k = 0; k < pd.n; k++) any |= pic_pd_current(&pd.it[k], now);
    pthread_mutex_unlock(&pd.m);
    return any;
}

static void pd_next(void)
{
    time_t now = time(NULL);
    int k;
    pthread_mutex_lock(&pd.m);
    for (k = 1; k <= pd.n; k++) {
        int i = (pd.cur + k) % pd.n;
        if (i < 0) i += pd.n;
        if (pic_pd_current(&pd.it[i], now)) { pd.cur = i; break; }
    }
    pthread_mutex_unlock(&pd.m);
}

void pic_photoday_set_items(const pd_item_t *it, int n, int cur)
{
    pthread_mutex_lock(&pd.m);
    if (n > PD_MAX) n = PD_MAX;
    memcpy(pd.it, it, (size_t)n * sizeof(*it));
    pd.n = n;
    pd.cur = cur;
    pd.gen++;
    pthread_mutex_unlock(&pd.m);
}

static pic_feature_t f_pd = {
    .name = "photoday", .work = pd_work, .reload = pd_reload, .wanted = pd_wanted,
    .needs_network = 1,
};

pic_feature_t *pic_feature_photoday(void)
{
    pic_view_set_photo_source(pd_ready, pd_next);
    return &f_pd;
}

/* ── 3. Drawing ─────────────────────────────────────────────────── */

static struct {
    char key[12], img[sizeof(((pd_item_t *)0)->img)];
    int w, h;                       /* screen size these were built for */
    cairo_surface_t *photo;         /* the picture, full size            */
    cairo_surface_t *under, *over;  /* background; caption layer         */
    double fx, fy, fw, fh;          /* where the photo fits              */
    double bx, by;                  /* badge centre                      */
    time_t since;                   /* when this showing began           */
    int idx, n;
} dr;

static void dr_free(void)
{
    if (dr.photo) cairo_surface_destroy(dr.photo);
    if (dr.under) cairo_surface_destroy(dr.under);
    if (dr.over) cairo_surface_destroy(dr.over);
    dr.photo = dr.under = dr.over = NULL;
    dr.key[0] = '\0';
}

static void set_font(cairo_t *cr, double size, int bold)
{
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
}

static double tw(cairo_t *cr, const char *s)
{
    cairo_text_extents_t e;
    cairo_text_extents(cr, s, &e);
    return e.x_advance;
}

/* Word-wrap `s` into at most `maxl` lines of width `w`, ending with an
 * ellipsis when cut; draws from baseline y with line step `lh` and
 * returns the y after the last line. draw = 0 only measures. */
static double wrap_text(cairo_t *cr, const char *s, double x, double y, double w,
                        int maxl, double lh, int draw)
{
    char line[512], word[256];
    const char *p = s;
    int lines = 0;
    line[0] = '\0';
    while (*p && lines < maxl) {
        size_t wl = strcspn(p, " ");
        char trial[768];
        if (wl >= sizeof(word)) wl = sizeof(word) - 1;
        memcpy(word, p, wl); word[wl] = '\0';
        p += wl; p += strspn(p, " ");
        snprintf(trial, sizeof(trial), "%s%s%s", line, line[0] ? " " : "", word);
        if (tw(cr, trial) <= w || !line[0]) {
            snprintf(line, sizeof(line), "%s", trial);
            continue;
        }
        /* line full */
        if (lines == maxl - 1) {
            /* last line: cut and add an ellipsis */
            char t[540];
            size_t ll = strlen(line);
            if (ll && line[ll - 1] == '.') line[ll - 1] = '\0';
            snprintf(t, sizeof(t), "%s\xe2\x80\xa6", line);
            while (tw(cr, t) > w && strrchr(line, ' ')) {
                *strrchr(line, ' ') = '\0';
                snprintf(t, sizeof(t), "%s\xe2\x80\xa6", line);
            }
            if (draw) { cairo_move_to(cr, x, y); cairo_show_text(cr, t); }
            return y + lh;
        }
        if (draw) { cairo_move_to(cr, x, y); cairo_show_text(cr, line); }
        y += lh; lines++;
        snprintf(line, sizeof(line), "%s", word);
    }
    if (line[0] && lines < maxl) {
        if (draw) { cairo_move_to(cr, x, y); cairo_show_text(cr, line); }
        y += lh;
    }
    return y;
}

/* Lines `s` needs at width w in the current font (never cut) */
static int nlines(cairo_t *cr, const char *s, double w)
{
    return (int)(wrap_text(cr, s, 0, 0, w, 1000, 1, 0) + .5);
}

/* The largest size from `size` down to `min` at which `s` fits in maxl
 * lines; 0 when it doesn't fit even at `min` */
static double fit_size(cairo_t *cr, const char *s, double w, int maxl,
                       double size, double min, int bold)
{
    double step = size / 28, f;
    for (f = size; f >= min - 1e-6; f -= step) {
        set_font(cr, f, bold);
        if (nlines(cr, s, w) <= maxl) return f;
    }
    return 0;
}

/* The description to show: the most complete sentences that fit in maxl
 * lines at `size`; else the first sentence, shrunk down to `min` and
 * allowed two more lines; else nothing. Returns the font size, 0 for none. */
static double pick_text(cairo_t *cr, const char *text, char *out, size_t n,
                        double w, int maxl, double size, double min)
{
    size_t ends[64];
    int k = pic_pd_sentences(text, ends, 64), i;
    double f;
    out[0] = '\0';
    set_font(cr, size, 0);
    for (i = k - 1; i >= 0; i--) {
        snprintf(out, n, "%.*s", (int)ends[i], text);
        if (nlines(cr, out, w) <= maxl) return size;
    }
    if (k > 0) {
        snprintf(out, n, "%.*s", (int)ends[0], text);
        if ((f = fit_size(cr, out, w, maxl, size, min, 0)) > 0) return f;
        if ((f = fit_size(cr, out, w, maxl + 2, min, min, 0)) > 0) return f;
    }
    out[0] = '\0';
    return 0;
}

static void fmt_date(char *out, size_t n, time_t t, int date_only)
{
    struct tm tm;
    char d[8];
    /* date-only feeds (APOD, AstroBin) are noon UTC on their date */
    if (date_only) gmtime_r(&t, &tm); else localtime_r(&t, &tm);
    snprintf(d, sizeof(d), "%d", tm.tm_mday);
    {
        char a[8], b[8];
        strftime(a, sizeof(a), "%a", &tm);
        strftime(b, sizeof(b), "%b", &tm);
        snprintf(out, n, "%s %s %s %d", a, d, b, tm.tm_year + 1900);
    }
}

/* The aperture badge: a ring with six blades that turn slowly */
static void badge(cairo_t *cr, double cx, double cy, double r, double ang)
{
    int k;
    cairo_set_source_rgb(cr, 1, .78, .35);
    cairo_set_line_width(cr, r * .18);
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_set_line_width(cr, r * .14);
    for (k = 0; k < 6; k++) {
        double a = ang + k * M_PI / 3;
        cairo_move_to(cr, cx + r * .38 * cos(a), cy + r * .38 * sin(a));
        cairo_line_to(cr, cx + r * .95 * cos(a + 1.1), cy + r * .95 * sin(a + 1.1));
    }
    cairo_stroke(cr);
}

/* Bottom of the free area: above the ticker when it is showing */
static double bottom_y(int h)
{
    double t = &pic_ticker_bar_top ? pic_ticker_bar_top : 0;
    return t > h * .6 && t < h ? t : h;
}

/* Background and caption for item `it` (n pictures today, idx shown) */
static int dr_build(const pd_item_t *it, int w, int h, int idx, int n)
{
    char path[96], date[40];
    cairo_surface_t *ph, *small;
    cairo_t *cr;
    double s = h / 1080.0, iw, ih, sc, pw, x0, y, cw, bot = bottom_y(h);
    int land, sw, sh, k;

    file_for(it->key, "jpg", path, sizeof(path));
    ph = pic_image_load(path);
    if (!ph) return -1;
    dr_free();
    dr.photo = ph;
    iw = cairo_image_surface_get_width(ph);
    ih = cairo_image_surface_get_height(ph);
    land = iw / ih > 1.4;
    pw = land ? 0 : 440 * s;

    /* background: the picture covering the screen, blurred by scaling
     * it down to a tiny copy and back up, and dimmed */
    sw = (int)fmax(8, w / 24.0); sh = (int)fmax(6, h / 24.0);
    small = cairo_image_surface_create(CAIRO_FORMAT_RGB24, sw, sh);
    cr = cairo_create(small);
    sc = fmax(sw / iw, sh / ih);
    cairo_translate(cr, (sw - iw * sc) / 2, (sh - ih * sc) / 2);
    cairo_scale(cr, sc, sc);
    cairo_set_source_surface(cr, ph, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_destroy(cr);
    dr.under = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cr = cairo_create(dr.under);
    cairo_scale(cr, (double)w / sw, (double)h / sh);
    cairo_set_source_surface(cr, small, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_paint(cr);
    cairo_identity_matrix(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, .62);
    cairo_paint(cr);
    if (!land) {
        cairo_rectangle(cr, w - pw, 0, pw, h);
        cairo_set_source_rgb(cr, .03, .045, .08);
        cairo_fill(cr);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(small);

    /* where the photo goes */
    sc = fmin((w - pw) / iw, h / ih);
    dr.fw = iw * sc; dr.fh = ih * sc;
    dr.fx = (w - pw - dr.fw) / 2; dr.fy = (h - dr.fh) / 2;

    /* caption layer */
    dr.over = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cr = cairo_create(dr.over);
    fmt_date(date, sizeof(date), it->t, !strcmp(it->key, "apod") || !strcmp(it->key, "astrobin"));
    {
        /* Lay the caption out so nothing is ever cut: the title, credit
         * and description are shown whole, shrinking the type a little if
         * needed, and the description is as many complete sentences as
         * fit (at least the first, or none at all). */
        double tsz, csz, xsz, tlh, clh, xlh, blk, gh;
        int tl, cl, xl;
        char desc[sizeof(it->text)];
        if (land) {
            cw = w - 48 * s - 300 * s;
            x0 = 48 * s;
        } else {
            x0 = w - pw + 30 * s;
            cw = pw - 60 * s;
        }
        tsz = fit_size(cr, it->title, cw, land ? 2 : 4, (land ? 30 : 28) * s, 20 * s, 1);
        if (tsz <= 0) tsz = 20 * s;
        set_font(cr, tsz, 1);
        tl = nlines(cr, it->title, cw);
        csz = it->credit[0] ? fit_size(cr, it->credit, cw, 2, 16 * s, 13 * s, 0) : 0;
        if (it->credit[0] && csz <= 0) csz = 13 * s;
        cl = 0;
        if (csz > 0) { set_font(cr, csz, 0); cl = nlines(cr, it->credit, cw); }
        xsz = pick_text(cr, it->text, desc, sizeof(desc), cw, land ? 3 : 8, 16 * s, 13 * s);
        xl = 0;
        if (xsz > 0) { set_font(cr, xsz, 0); xl = nlines(cr, desc, cw); }
        tlh = tsz * 1.22; clh = csz * 1.45; xlh = xsz * 1.45;

        if (land) {
            /* measure the block, then draw it up from the bottom */
            blk = 36 * s + tl * tlh + cl * clh + (xl ? 4 * s + xl * xlh : 0);
            gh = blk + 110 * s;
            {
                cairo_pattern_t *g = cairo_pattern_create_linear(0, bot - gh, 0, bot);
                cairo_pattern_add_color_stop_rgba(g, 0, .02, .03, .06, 0);
                cairo_pattern_add_color_stop_rgba(g, .55, .02, .03, .06, .72);
                cairo_pattern_add_color_stop_rgba(g, 1, .02, .03, .06, .9);
                cairo_rectangle(cr, 0, bot - gh, w, gh);
                cairo_set_source(cr, g);
                cairo_fill(cr);
                cairo_pattern_destroy(g);
            }
            y = bot - 22 * s - blk;
            {
                char t[60];
                double after;
                set_font(cr, 18 * s, 1);
                cairo_set_source_rgb(cr, 1, .78, .35);
                cairo_move_to(cr, x0 + 30 * s, y + 6 * s);
                cairo_show_text(cr, it->src);
                after = x0 + 30 * s + tw(cr, it->src);
                set_font(cr, 14 * s, 0);
                cairo_set_source_rgb(cr, .64, .68, .75);
                snprintf(t, sizeof(t), "  \xc2\xb7  %s", date);
                cairo_move_to(cr, after, y + 6 * s);
                cairo_show_text(cr, t);
            }
            dr.bx = x0 + 11 * s; dr.by = y;
            y += 36 * s + tsz * .25;
        } else {
            y = 230 * s;
            {
                double fs = 18 * s;
                set_font(cr, fs, 1);
                while (tw(cr, it->src) > cw - 30 * s && fs > 12 * s) set_font(cr, fs -= s, 1);
            }
            cairo_set_source_rgb(cr, 1, .78, .35);
            cairo_move_to(cr, x0 + 30 * s, y + 6 * s);
            cairo_show_text(cr, it->src);
            set_font(cr, 14 * s, 0);
            cairo_set_source_rgb(cr, .64, .68, .75);
            cairo_move_to(cr, x0 + 30 * s, y + 26 * s);
            cairo_show_text(cr, date);
            dr.bx = x0 + 11 * s; dr.by = y;
            y += 66 * s;
        }
        set_font(cr, tsz, 1);
        cairo_set_source_rgb(cr, 1, 1, 1);
        y = wrap_text(cr, it->title, x0, y, cw, 1000, tlh, 1);
        if (cl) {
            set_font(cr, csz, 0);
            cairo_set_source_rgb(cr, .82, .84, .88);
            y = wrap_text(cr, it->credit, x0, y + (land ? -tsz * .1 : 0), cw, 1000, clh, 1);
        }
        if (xl) {
            set_font(cr, xsz, 0);
            cairo_set_source_rgb(cr, .7, .74, .8);
            wrap_text(cr, desc, x0, y + (land ? 4 : 8) * s, cw, 1000, xlh, 1);
        }
    }
    /* which of today's pictures this is */
    if (n > 1) {
        char t[40];
        double dx, dy = bot - 26 * s, tx;
        set_font(cr, 14 * s, 0);
        snprintf(t, sizeof(t), "%d of %d today", idx + 1, n);
        tx = tw(cr, t);
        dx = land ? w - 40 * s - tx - 10 * s - n * 18 * s : w - pw + 30 * s;
        for (k = 0; k < n; k++) {
            cairo_new_path(cr);
            cairo_arc(cr, dx + k * 18 * s + 5 * s, dy, 5 * s, 0, 2 * M_PI);
            if (k == idx) cairo_set_source_rgb(cr, 1, .78, .35);
            else cairo_set_source_rgb(cr, .36, .38, .44);
            cairo_fill(cr);
        }
        cairo_set_source_rgb(cr, .6, .63, .68);
        cairo_move_to(cr, dx + n * 18 * s + 8 * s, dy + 5 * s);
        cairo_show_text(cr, t);
    }
    cairo_destroy(cr);
    snprintf(dr.key, sizeof(dr.key), "%s", it->key);
    snprintf(dr.img, sizeof(dr.img), "%s", it->img);
    dr.w = w; dr.h = h;
    return 0;
}

void pic_photoday_render(cairo_t *cr, int w, int h, time_t now)
{
    pd_item_t it;
    int have = 0, idx = 0, n = 0, k;
    double s = h / 1080.0, p, z, cx, cy;

    pthread_mutex_lock(&pd.m);
    if (pd.cur >= 0 && pd.cur < pd.n) {
        it = pd.it[pd.cur];
        have = 1;
        for (k = 0; k < pd.n; k++)
            if (pic_pd_current(&pd.it[k], now)) {
                if (k == pd.cur) idx = n;
                n++;
            }
    }
    pthread_mutex_unlock(&pd.m);

    if (have && (strcmp(it.key, dr.key) || strcmp(it.img, dr.img) || w != dr.w || h != dr.h ||
                 idx != dr.idx || n != dr.n)) {
        if (dr_build(&it, w, h, idx, n) == 0) {
            dr.idx = idx; dr.n = n;
            dr.since = now;
        }
    }
    if (!dr.photo) {
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_paint(cr);
        return;
    }
    if (now < dr.since || now - dr.since > 3600) dr.since = now;

    cairo_save(cr);
    cairo_set_source_surface(cr, dr.under, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* slow drift and zoom inside the photo's frame: 6% over 90 s,
     * towards a point that differs from picture to picture */
    p = fmin(1.0, (now - dr.since) / 90.0);
    p = p * p * (3 - 2 * p);
    z = 1.0 + .06 * p;
    k = (int)(dr.key[0] + dr.key[1] * 7 + dr.idx * 13);
    cx = dr.fx + dr.fw * (.5 + .2 * p * sin(k));
    cy = dr.fy + dr.fh * (.5 + .2 * p * cos(k));
    cairo_rectangle(cr, dr.fx, dr.fy, dr.fw, dr.fh);
    cairo_clip(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, z, z);
    cairo_translate(cr, -cx, -cy);
    cairo_translate(cr, dr.fx, dr.fy);
    cairo_scale(cr, dr.fw / cairo_image_surface_get_width(dr.photo),
                dr.fh / cairo_image_surface_get_height(dr.photo));
    cairo_set_source_surface(cr, dr.photo, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    cairo_set_source_surface(cr, dr.over, 0, 0);
    cairo_paint(cr);
    badge(cr, dr.bx, dr.by, 11 * s, fmod(now * .35, 2 * M_PI));
}

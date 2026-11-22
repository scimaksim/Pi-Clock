/* Picture of the day: Pi-Dash feed parsers, which series are picked,
 * freshness, and the view in the cycle. All data made up. */
#include "check.h"
#include "photoday.h"
#include "localview.h"
#include "config.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int ready_n;
static int nexts;
static int fake_ready(time_t now) { (void)now; return ready_n > 0; }
static void fake_next(void) { nexts++; }

int main(void)
{
    /* 2026-11-17 15:00 UTC */
    time_t now = 1794927600;

    {   /* tidy */
        char s[200] = "  Llullaillaco volcano , more\n than  6700 m ( high ) \xe2\x80\x9c quote \xe2\x80\x9d .";
        pic_pd_tidy(s);
        CHECK(!strcmp(s, "Llullaillaco volcano, more than 6700 m (high) \xe2\x80\x9cquote\xe2\x80\x9d."), "tidy: '%s'", s);
    }
    {   /* sentences: never split an initial or abbreviation, never count a cut-off tail */
        size_t e[16];
        const char *t = "Image by A. Person and Dr. Who. It shows M30 (NGC 7099). \"Wow!\" said St. Clair. And then 3 more. Cut off mid";
        int n = pic_pd_sentences(t, e, 16);
        CHECK(n == 4, "4 sentences (%d)", n);
        CHECK(n >= 1 && e[0] == 31 && !strncmp(t + 27, "Who.", 4), "first ends after 'Who.' (%zu)", n ? e[0] : 0);
        CHECK(n >= 3 && !strncmp(t + e[2] - 15, "said St. Clair.", 15), "St. is not an end");
        CHECK(n >= 4 && !strncmp(t + e[3] - 5, "more.", 5), "last whole sentence");
        CHECK(pic_pd_sentences("No end here", e, 16) == 0, "no complete sentence");
        CHECK(pic_pd_sentences("One. Two.", e, 16) == 2 && e[1] == 9, "ends at the end of text");
    }
    {   /* APOD */
        pd_item_t it;
        const char *js = "{\"title\":\"Test Arches\",\"explanation\":\"Explanation: Two arches  meet.\","
            "\"url\":\"https://example.org/a b.jpg\",\"media_type\":\"image\",\"date\":\"2026-11-17\","
            "\"copyright\":\"\\nA. Person\\n\",\"_t\":1}";
        int n = pic_pd_parse_apod(js, strlen(js), now, &it);
        CHECK(n == 1, "apod parsed (%d)", n);
        CHECK(!strcmp(it.title, "Test Arches") && !strcmp(it.text, "Two arches meet."), "apod text '%s'", it.text);
        CHECK(!strcmp(it.credit, "Image: A. Person"), "apod credit '%s'", it.credit);
        CHECK(!strcmp(it.img, "/api/img?u=https%3A%2F%2Fexample.org%2Fa%20b.jpg&w=1920"), "apod via Pi-Dash cache '%s'", it.img);
        CHECK(pic_pd_parse_apod(js, strlen(js), now + 4 * 86400, &it) == 0, "apod 4 days old: not shown");
        js = "{\"title\":\"Clip\",\"explanation\":\"x\",\"url\":\"https://example.org/v\",\"media_type\":\"video\",\"date\":\"2026-11-17\"}";
        CHECK(pic_pd_parse_apod(js, strlen(js), now, &it) == 0, "apod video: not shown");
        js = "{\"title\":\"No credit\",\"explanation\":\"x\",\"url\":\"https://example.org/p.jpg\",\"media_type\":\"image\",\"date\":\"2026-11-16\"}";
        CHECK(pic_pd_parse_apod(js, strlen(js), now, &it) == 1 && !it.credit[0], "no copyright: no credit line");
        js = "{\"title\":\"T\",\"explanation\":\"Explanation: Seen from Norway.Tomorrow's picture: open space\",\"url\":\"https://example.org/p.jpg\",\"media_type\":\"image\",\"date\":\"2026-11-17\"}";
        CHECK(pic_pd_parse_apod(js, strlen(js), now, &it) == 1 && !strcmp(it.text, "Seen from Norway."), "APOD footer dropped '%s'", it.text);
        CHECK(pic_pd_parse_apod("{bad", 4, now, &it) == -1, "bad json");
    }
    {   /* AstroBin */
        pd_item_t it;
        const char *js = "{\"items\":[{\"title\":\"Test Nebula\",\"by\":\"someone\",\"img\":\"/api/img?u=https%3A%2F%2Fcdn.example%2Fx.jpg&w=1280\","
            "\"date\":\"2026-11-17\",\"text\":\"A poem.\",\"hours\":12.5,\"split\":\"Ha 6 h \\u00b7 OIII 6.5 h\",\"mount\":\"Mount X\"},"
            "{\"title\":\"Older\",\"date\":\"2026-11-16\",\"img\":\"/api/img?u=y\"}]}";
        int n = pic_pd_parse_astrobin(js, strlen(js), now, &it);
        CHECK(n == 1 && !strcmp(it.title, "Test Nebula") && !strcmp(it.credit, "Image: someone"), "astrobin parsed");
        CHECK(!strcmp(it.img, "/api/img?u=https%3A%2F%2Fcdn.example%2Fx.jpg&w=1920"), "astrobin full width '%s'", it.img);
        CHECK(!strcmp(it.text, "12.5 hours of exposure: Ha 6 h \xc2\xb7 OIII 6.5 h \xc2\xb7 Mount X"), "astrobin caption '%s'", it.text);
        CHECK(pic_pd_parse_astrobin("{\"items\":[]}", 12, now, &it) == 0, "astrobin empty");
    }
    {   /* spacefeed: newest of each series, agency news skipped, stale dropped */
        pd_item_t it[PD_MAX];
        char js[4000];
        int n, k, eso = -1, nasa = -1, webb = -1, noir = -1;
        snprintf(js, sizeof(js), "{\"releases\":["
            "{\"src\":\"ESA\",\"title\":\"News story\",\"img\":\"https://e.example/a.webp\",\"t\":%ld},"
            "{\"src\":\"ESO\",\"title\":\"Old week\",\"img\":\"https://cdn.eso.example/images/screen/potw2645a.jpg\",\"t\":%ld},"
            "{\"src\":\"ESO\",\"title\":\"This week\",\"text\":\"Two lights , rising.\",\"img\":\"https://cdn.eso.example/images/screen/potw2646a.jpg\",\"t\":%ld},"
            "{\"src\":\"ESO\",\"title\":\"Press release\",\"img\":\"https://cdn.eso.example/images/screen/eso2613d.jpg\",\"t\":%ld},"
            "{\"src\":\"NOIRLab\",\"title\":\"Meeting photo\",\"img\":\"https://n.example/screen/IMG_1.jpg\",\"t\":%ld},"
            "{\"src\":\"NOIRLab\",\"title\":\"Twilight\",\"img\":\"https://n.example/screen/iotw2645a.jpg\",\"t\":%ld},"
            "{\"src\":\"NASA\",\"title\":\"Star burst\",\"about\":\"NASA Image of the Day \\u00b7 more\",\"img\":\"https://w.example/s.jpg\",\"t\":%ld},"
            "{\"src\":\"Webb\",\"title\":\"Cauldron\",\"img\":\"https://w.example/screen/potm2610a.jpg\",\"t\":%ld},"
            "{\"src\":\"Hubble\",\"title\":\"Last year\",\"img\":\"https://h.example/screen/potw2552a.jpg\",\"t\":%ld}"
            "]}",
            (long)now, (long)(now - 8 * 86400), (long)(now - 86400), (long)now, (long)now,
            (long)(now - 6 * 86400), (long)(now - 4 * 86400), (long)(now - 26 * 86400), (long)(now - 330 * 86400));
        n = pic_pd_parse_spacefeed(js, strlen(js), now, it, PD_MAX);
        for (k = 0; k < n; k++) {
            if (!strcmp(it[k].key, "eso")) eso = k;
            if (!strcmp(it[k].key, "nasa")) nasa = k;
            if (!strcmp(it[k].key, "webb")) webb = k;
            if (!strcmp(it[k].key, "noirlab")) noir = k;
        }
        CHECK(n == 4, "four current series (%d)", n);
        CHECK(eso >= 0 && !strcmp(it[eso].title, "This week") && !strcmp(it[eso].text, "Two lights, rising."),
              "ESO: the newest picture of the week, not the press release");
        CHECK(eso >= 0 && !strcmp(it[eso].src, "ESO Picture of the Week") && !it[eso].credit[0], "ESO label, no made-up credit");
        CHECK(noir >= 0 && !strcmp(it[noir].title, "Twilight"), "NOIRLab image of the week, not other photos");
        CHECK(nasa >= 0 && webb >= 0, "NASA image of the day and Webb picture of the month");
        n = pic_pd_parse_spacefeed(js, strlen(js), now + 30 * 86400, it, PD_MAX);
        CHECK(n == 0, "a month later nothing is current (%d)", n);
    }
    {   /* memory: a picture that dropped out of the feed stays while current */
        pd_item_t got[PD_MAX], mem[PD_MAX], back[PD_MAX];
        int n, m;
        memset(got, 0, sizeof(got)); memset(mem, 0, sizeof(mem));
        snprintf(mem[0].key, sizeof(mem[0].key), "webb");
        snprintf(mem[0].src, sizeof(mem[0].src), "ESA/Webb Picture of the Month");
        snprintf(mem[0].title, sizeof(mem[0].title), "Cauldron");
        snprintf(mem[0].text, sizeof(mem[0].text), "Stars\tbrewing.");
        snprintf(mem[0].img, sizeof(mem[0].img), "/api/img?u=x&w=1920");
        mem[0].t = now - 26 * 86400; mem[0].max_days = 40;
        mem[1] = mem[0];
        snprintf(mem[1].key, sizeof(mem[1].key), "eso");
        mem[1].t = now - 20 * 86400; mem[1].max_days = 9;            /* out of date */
        mem[2] = mem[0];
        snprintf(mem[2].key, sizeof(mem[2].key), "apod");             /* fresh one wins */
        snprintf(mem[2].title, sizeof(mem[2].title), "Yesterday");
        mem[2].t = now - 86400; mem[2].max_days = 2;
        got[0] = mem[2];
        snprintf(got[0].title, sizeof(got[0].title), "Today");
        got[0].t = now;
        CHECK(pic_pd_save("/tmp/pic_test_pd_items.tsv", mem, 3) == 0, "save");
        m = pic_pd_load("/tmp/pic_test_pd_items.tsv", back, PD_MAX);
        CHECK(m == 3 && !strcmp(back[0].title, "Cauldron") && !strcmp(back[0].text, "Stars brewing.") &&
              back[0].t == mem[0].t && back[0].max_days == 40 && !strcmp(back[0].img, mem[0].img), "load round trip");
        n = pic_pd_merge(got, 1, PD_MAX, back, m, now);
        CHECK(n == 2 && !strcmp(got[0].title, "Today") && !strcmp(got[1].key, "webb"),
              "kept the dropped Webb picture, not the stale ESO one or yesterday's APOD (%d)", n);
        CHECK(pic_pd_merge(got, 2, PD_MAX, back, m, now + 20 * 86400) == 2, "nothing extra once out of date");
        CHECK(pic_pd_load("/tmp/pic_test_pd_none.tsv", back, PD_MAX) == 0, "no file: nothing");
        unlink("/tmp/pic_test_pd_items.tsv");
    }
    {   /* restart with the Pi-Dash server away: pictures on disk still show */
        pd_item_t mem[1];
        FILE *f;
        pic_feature_t *ft = pic_feature_photoday();
        time_t t = time(NULL);
        system("rm -rf /tmp/pic_test_pd && mkdir -p /tmp/pic_test_pd");
        setenv("PIC_PHOTODAY_DIR", "/tmp/pic_test_pd", 1);
        memset(mem, 0, sizeof(mem));
        snprintf(mem[0].key, sizeof(mem[0].key), "eso");
        snprintf(mem[0].src, sizeof(mem[0].src), "ESO Picture of the Week");
        snprintf(mem[0].title, sizeof(mem[0].title), "Early risers");
        snprintf(mem[0].img, sizeof(mem[0].img), "/api/img?u=x&w=1920");
        mem[0].t = t - 86400; mem[0].max_days = 9;
        pic_pd_save("/tmp/pic_test_pd/items.tsv", mem, 1);
        f = fopen("/tmp/pic_test_pd/eso.url", "w"); fputs(mem[0].img, f); fclose(f);
        f = fopen("/tmp/pic_test_pd/eso.jpg", "w");
        { int z; for (z = 0; z < 2000; z++) fputc(z & 0xff, f); }
        fclose(f);
        f = fopen("/tmp/pic_test_pd.conf", "w");
        fprintf(f, "VIEW_PHOTO_MIN=1\nPIDASH_URL=http://127.0.0.1:9\n");
        fclose(f);
        setenv("PIC_RENDERER_CONF", "/tmp/pic_test_pd.conf", 1);
        ft->reload(ft);
        ft->work(ft);
        pic_view_set_photo_source(NULL, NULL);
        {
            extern int pic_photoday_count_for_test(time_t now);
            CHECK(pic_photoday_count_for_test(t) == 1, "picture on disk kept after a restart without the server");
        }
        system("rm -rf /tmp/pic_test_pd /tmp/pic_test_pd.conf");
        unsetenv("PIC_RENDERER_CONF");
        unsetenv("PIC_PHOTODAY_DIR");
        pic_photoday_set_items(NULL, 0, -1);   /* leave no state for later checks */
    }
    {   /* the view: after the other worlds, only while there is a picture */
        FILE *f = fopen("/tmp/pic_test_photoday.conf", "w");
        time_t t0 = 1794000000, t;
        int shown = 0, secs = 0, prev = 0;
        fprintf(f, "VIEW_MODE=cycle\nVIEW_GLOBAL_MIN=5\nVIEW_LOCAL_MIN=5\nVIEW_PHOTO_MIN=1\nVIEW_SMART=0\n"
                   "VIEW_WORLDS=mars\nVIEW_WORLD_MIN=2\n");
        fclose(f);
        setenv("PIC_RENDERER_CONF", "/tmp/pic_test_photoday.conf", 1);
        setenv("PIC_VIEW_STATE", "/tmp/pic_test_photoday_worlds", 1);
        pic_view_set_photo_source(fake_ready, fake_next);
        ready_n = 1;
        pic_view_reload(t0);
        for (t = t0; t < t0 + 3 * 13 * 60; t += 5) {
            pic_view_update(t);
            if (pic_view_photo()) {
                secs += 5;
                if (!prev) {
                    shown++;
                    CHECK(!pic_view_world() && !pic_view_is_local() && pic_view_offworld() && pic_view_state() == -2,
                          "photo view state");
                }
            }
            if (prev && !pic_view_photo())
                CHECK(!pic_view_world() && !pic_view_is_local(), "photo -> world map");
            prev = pic_view_photo();
        }
        CHECK(shown == 3 && nexts == 3, "once a cycle, next picture each time (%d, %d)", shown, nexts);
        CHECK(secs >= 3 * 55 && secs <= 3 * 65, "1 min each (%d s)", secs);
        ready_n = 0; shown = 0;
        for (t0 = t; t < t0 + 3600; t += 5) { pic_view_update(t); shown |= pic_view_photo(); }
        CHECK(!shown, "no picture: view skipped");
        unlink("/tmp/pic_test_photoday.conf");
        unlink("/tmp/pic_test_photoday_worlds");
    }
    return CHECK_DONE("test_photoday");
}

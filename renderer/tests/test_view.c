/* Smart view switching: holds the local view during a pass, switches to
 * local early for one, never holds longer than VIEW_HOLD_MAX, and
 * cycles normally with nothing going on */
#include "check.h"
#include "features_all.h"
#include "localview.h"
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int (*pic_events_test_source)(time_t now, pic_event_t *out, int max);
static time_t ev_start, ev_end;
static int fake(time_t now, pic_event_t *out, int max)
{
    (void)now; (void)max;
    if (!ev_start) return 0;
    memset(out, 0, sizeof(*out));
    out->kind = PIC_EV_PASS; out->start = ev_start; out->end = ev_end;
    snprintf(out->text, sizeof(out->text), "ISS pass");
    return 1;
}

int main(void)
{
    time_t t0 = 1794000000, t, last_switch = 0;
    int switches = 0, local_at_pass = 0;
    FILE *f = fopen("/tmp/pic_test_view.conf", "w");
    fprintf(f, "VIEW_MODE=cycle\nVIEW_GLOBAL_MIN=5\nVIEW_LOCAL_MIN=5\nVIEW_SMART=1\nLOCAL_ZOOM=8\n");
    fclose(f);
    setenv("PIC_RENDERER_CONF", "/tmp/pic_test_view.conf", 1);
    pic_config.qth_lat = 35.69; pic_config.qth_lon = -78.73;
    pic_events_test_source = fake;
    pic_view_reload(t0);

    /* Nothing happening: switches every 5 min */
    for (t = t0; t < t0 + 1800; t += 5)
        if (pic_view_update(t)) { switches++; last_switch = t; }
    CHECK(switches == 5 || switches == 6, "quiet half hour: %d switches, want 5-6", switches);
    (void)last_switch;

    /* A 6-minute pass starting while global: switch early, then hold */
    pic_view_reload(t0 + 1800);
    while (pic_view_is_local()) pic_view_update(t += 5);          /* get to global */
    t += 90;                                                      /* global for 1.5 min */
    ev_start = t + 60; ev_end = t + 60 + 360;
    {
        time_t sw = 0;
        for (; t < ev_end + 400; t += 5) {
            int chg = pic_view_update(t);
            if (chg && pic_view_is_local() && !sw) sw = t;
            if (t > ev_start && t < ev_end && pic_view_is_local()) local_at_pass++;
            if (t > ev_start + 30 && t < ev_end && !pic_view_is_local())
                CHECK(0, "left the local view during the pass at +%ld s", (long)(t - ev_start));
        }
        CHECK(sw && sw <= ev_start, "switched early for the pass (at %ld, pass %ld)", (long)sw, (long)ev_start);
    }
    CHECK(local_at_pass > 60, "local during the pass (%d samples)", local_at_pass);

    /* A never-ending event: hold capped at VIEW_HOLD_MAX (20 min) */
    ev_start = t; ev_end = t + 7200;
    pic_view_reload(t);
    {
        int left = 0;
        time_t start = t;
        for (; t < start + 40 * 60; t += 5) {
            pic_view_update(t);
            if (!pic_view_is_local() && !left) left = (int)((t - start) / 60);
        }
        CHECK(left > 0 && left <= 26, "endless event: left local after %d min (cap 20 + cycle)", left);
    }
    unlink("/tmp/pic_test_view.conf");
    return CHECK_DONE("test_view");
}

/* fetch.c with a fake wget on PATH: plain replies, a wget whose
 * ssl_client-like child outlives it holding stdout, and no pipe ends
 * leaking into another thread's wget */
#include "check.h"
#include "../fetch.h"
#include "../spacetrack.h"
#include "../satellite.h"
#include "../spacedata.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *dir = "/tmp/pic_fake_wget";

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *slow_fetch(void *arg)
{
    static char buf[64];
    (void)arg;
    pic_fetch_to_buf("http://x/slow", buf, sizeof(buf));
    return NULL;
}

static char st_log[1024], st_body[512];

static int fake_st(const char *method, const char *path, const char *cookie,
                   const char *body, FILE *out, char *cookies, size_t cn)
{
    size_t l = strlen(st_log);
    snprintf(st_log + l, sizeof(st_log) - l, "%s %s%s%s|", method, path,
             cookie ? " cookie=" : "", cookie ? cookie : "");
    if (body) {
        snprintf(st_body, sizeof(st_body), "%s", body);
        if (strstr(body, "wrongpw")) { if (out) fputs("{\"Login\":\"Failed\"}", out); return 200; }
        if (cookies) snprintf(cookies, cn, "chocolatechip=abc");
        return 200;
    }
    if (out && strncmp(path, "/basic", 6) == 0)
        fputs("0 ISS (ZARYA)\n1 25544U line\n2 25544 line\n", out);
    return 200;
}

int main(void)
{
    char path[256], buf[256], cmd[512];
    FILE *f;
    double t;
    int n, fds;
    pthread_t th;

    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/wget", dir);
    f = fopen(path, "w");
    fputs("#!/bin/sh\n"
          "for a; do u=$a; done\n"
          "case $u in\n"
          "  */ok) printf hello ;;\n"
          "  https://celestrak.org/*) echo x >> /tmp/pic_fake_wget/ct.count;"
          " case $u in *fail*) exit 4 ;; *) printf '1 x\\n2 y\\n' ;; esac ;;\n"
          "  */orphan) printf data; sleep 30 & echo $! > /tmp/pic_fake_wget/orphan.pid ;;\n"
          "  */slow) p=$(readlink /proc/$$/fd/1); echo \"$p\" > /tmp/pic_fake_wget/slow.pipe; sleep 2; printf slow ;;\n"
          "  */fds) p=$(cat /tmp/pic_fake_wget/slow.pipe); [ -n \"$p\" ] || { echo none; exit 0; };"
          " if ls -l /proc/$$/fd | grep -qF \"$p\"; then echo 1; else echo 0; fi ;;\n"
          "esac\nexit 0\n", f);
    fclose(f);
    chmod(path, 0755);
    {
        static char pathenv[8192];
        snprintf(pathenv, sizeof(pathenv), "%s:%s", dir,
                 getenv("PATH") ? getenv("PATH") : "/bin:/usr/bin");
        setenv("PATH", pathenv, 1);
    }

    n = pic_fetch_to_buf("http://x/ok", buf, sizeof(buf));
    CHECK(n == 5 && !strcmp(buf, "hello"), "plain reply: %d '%s'", n, buf);

    /* wget exits but its child keeps stdout open: return promptly with
     * the data, and the leftover child is killed */
    t = now_s();
    n = pic_fetch_to_buf("http://x/orphan", buf, sizeof(buf));
    t = now_s() - t;
    CHECK(n == 4 && !strcmp(buf, "data"), "orphan reply: %d '%s'", n, buf);
    CHECK(t < 5, "orphan fetch took %.1f s", t);
    usleep(200000);
    snprintf(path, sizeof(path), "%s/orphan.pid", dir);
    f = fopen(path, "r");
    if (f) {
        int pid = 0;
        if (fscanf(f, "%d", &pid) == 1) {
            /* gone, or a zombie waiting for init to reap it */
            char st[64], line[128];
            FILE *ps;
            int alive = 0;
            snprintf(st, sizeof(st), "/proc/%d/status", pid);
            ps = fopen(st, "r");
            if (ps) {
                while (fgets(line, sizeof(line), ps))
                    if (!strncmp(line, "State:", 6) && !strchr(line, 'Z')) alive = 1;
                fclose(ps);
            }
            CHECK(pid > 0 && !alive, "leftover child %d still running", pid);
        }
        fclose(f);
    } else CHECK(0, "no orphan pid file");

    /* While another thread's fetch is in flight, a new wget must not
     * inherit that fetch's pipe ends */
    pthread_create(&th, NULL, slow_fetch, NULL);
    usleep(500000);
    n = pic_fetch_to_buf("http://x/fds", buf, sizeof(buf));
    fds = atoi(buf);
    CHECK(n > 0 && buf[0] != 'n', "fds reply: %d '%s'", n, buf);
    CHECK(fds == 0, "wget holds the other fetch's pipe");
    pthread_join(th, NULL);

    /* CelesTrak guard: a failure stops all CelesTrak requests (no wget
     * at all) until the back-off ends; a cap of 6 an hour otherwise */
    {
        int calls;
        FILE *c;
        setenv("PIC_CELESTRAK_STATE", "/tmp/pic_fake_wget/ct.state", 1);
        n = pic_fetch_to_buf("https://celestrak.org/a/good", buf, sizeof(buf));
        CHECK(n > 0, "celestrak ok");
        n = pic_fetch_to_buf("https://celestrak.org/b/fail", buf, sizeof(buf));
        CHECK(n < 0, "celestrak fail");
        n = pic_fetch_to_buf("https://celestrak.org/c/good", buf, sizeof(buf));
        CHECK(n < 0, "held back after a failure");
        n = pic_fetch_to_file("https://celestrak.org/d/good", "/tmp/pic_fake_wget/x", 5);
        CHECK(n != 0, "file fetch held back too");
        n = pic_fetch_to_buf("http://x/ok", buf, sizeof(buf));
        CHECK(n > 0, "other hosts unaffected");
        c = fopen("/tmp/pic_fake_wget/ct.count", "r");
        calls = 0;
        if (c) { while (fgets(buf, sizeof(buf), c)) calls++; fclose(c); }
        CHECK(calls == 2, "wget ran %d times for CelesTrak, want 2", calls);
    }

    /* Space-Track: sign in with the form-encoded account, query with
     * the session cookie, sign out; 3LE tidied; a refused sign-in
     * stops further requests */
    {
        FILE *c = fopen("/tmp/pic_fake_wget/st.cred", "w"), *r;
        char line[256];
        int ok;
        fputs("me@example.com\ns3cret pw&x\n", c);
        fclose(c);
        setenv("PIC_SPACETRACK_CRED", "/tmp/pic_fake_wget/st.cred", 1);
        setenv("PIC_SPACETRACK_STATE", "/tmp/pic_fake_wget/st.state", 1);
        pic_spacetrack_set_transport(fake_st);
        CHECK(pic_spacetrack_configured(), "account read");
        ok = pic_spacetrack_fetch("/basicspacedata/query/class/gp/NORAD_CAT_ID/25544/format/3le",
                                  "/tmp/pic_fake_wget/st.out") == 0;
        CHECK(ok, "space-track fetch");
        CHECK(strcmp(st_log, "POST /ajaxauth/login|GET /basicspacedata/query/class/gp/NORAD_CAT_ID/25544/format/3le "
                             "cookie=chocolatechip=abc|GET /ajaxauth/logout cookie=chocolatechip=abc|") == 0,
              "login, query, logout: %s", st_log);
        CHECK(strstr(st_body, "password=s3cret%20pw%26x") && strstr(st_body, "identity=me%40example.com"),
              "form body encoded");
        r = fopen("/tmp/pic_fake_wget/st.out", "r");
        CHECK(r && fgets(line, sizeof(line), r) && strncmp(line, "ISS (ZARYA)", 11) == 0, "name line tidied");
        if (r) fclose(r);
        c = fopen("/tmp/pic_fake_wget/st.cred", "w");
        fputs("me@example.com\nwrongpw\n", c);
        fclose(c);
        st_log[0] = 0;
        CHECK(pic_spacetrack_fetch("/x", "/tmp/pic_fake_wget/st.out") != 0, "refused sign-in fails");
        CHECK(strcmp(st_log, "POST /ajaxauth/login|") == 0, "no query after a refused sign-in");
        st_log[0] = 0;
        CHECK(pic_spacetrack_fetch("/x", "/tmp/pic_fake_wget/st.out") != 0 && !st_log[0],
              "no request while paused");
        /* a changed account lifts the pause; several queries share one sign-in */
        c = fopen("/tmp/pic_fake_wget/st.cred", "w");
        fputs("me@example.com\ns3cret\n", c);
        fclose(c);
        {
            struct timeval tv[2] = { { time(NULL) + 5, 0 }, { time(NULL) + 5, 0 } };
            utimes("/tmp/pic_fake_wget/st.cred", tv);
        }
        st_log[0] = 0;
        {
            const char *q[2] = { "/basicspacedata/query/class/tip/format/json",
                                 "/basicspacedata/query/class/boxscore/format/json" };
            const char *p2[2] = { "/tmp/pic_fake_wget/a.json", "/tmp/pic_fake_wget/b.json" };
            CHECK(pic_spacetrack_fetch_raw(q, p2, 2) == 2, "two raw queries");
            CHECK(strncmp(st_log, "POST /ajaxauth/login|GET /basicspacedata/query/class/tip", 56) == 0 &&
                  strstr(st_log, "boxscore") && strstr(st_log, "logout") &&
                  strstr(st_log + 25, "login") == NULL, "one sign-in for both: %s", st_log);
        }
        pic_spacetrack_set_transport(NULL);
    }

    /* Orbits shared by a PC: CelesTrak is never asked, Space-Track
     * queries go to the PC (no sign-in, no account needed) */
    {
        char share[300], buf2[64];
        setenv("PIC_ORBIT_SHARE", "http://192.168.1.251:8080/", 1);
        CHECK(pic_orbit_share(share, sizeof(share)) && !strcmp(share, "http://192.168.1.251:8080"), "share address: '%s'", share);
        CHECK(pic_spacetrack_configured(), "shared orbits count as configured");
        st_log[0] = 0;
        pic_spacetrack_set_transport(fake_st);
        CHECK(pic_fetch_to_buf("https://celestrak.org/NORAD/elements/gp.php?GROUP=visual&FORMAT=tle", buf2, sizeof(buf2)) < 0,
              "CelesTrak not asked when orbits are shared");
        pic_spacetrack_fetch("/basicspacedata/query/class/gp/NORAD_CAT_ID/25544/format/3le", "/tmp/pic_fake_wget/st2.out");
        CHECK(!st_log[0], "no Space-Track request when orbits are shared");
        pic_spacetrack_set_transport(NULL);
        setenv("PIC_ORBIT_SHARE", "", 1);
        CHECK(!pic_orbit_share(share, sizeof(share)), "empty share address is off");
        unsetenv("PIC_ORBIT_SHARE");
    }

    {   /* ids from Space-Track JSON */
        char ids[256] = "";
        pic_spacedata_ids("[{\"NORAD_CAT_ID\":\"25544\"},{\"NORAD_CAT_ID\":\"100983\"},"
                          "{\"NORAD_CAT_ID\":\"25544\"}]", "NORAD_CAT_ID", ids, sizeof(ids));
        pic_spacedata_ids("{\"SAT_1_ID\":\"44713\",\"SAT_2_ID\":\"25544\"}", "SAT_1_ID", ids, sizeof(ids));
        CHECK(strcmp(ids, "25544,100983,44713") == 0, "ids: %s", ids);
    }

    /* Alpha-5 catalog numbers */
    CHECK(pic_tle_catnr("1 25544U 98067A   26300.5") == 25544, "5-digit catnr");
    CHECK(pic_tle_catnr("1 A0983U 26001A   26300.5") == 100983, "alpha-5 A0983");
    CHECK(pic_tle_catnr("1 Z9999U 26001A   26300.5") == 339999, "alpha-5 Z9999");

    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd)) { /* ignore */ }
    return CHECK_DONE("test_fetch");
}

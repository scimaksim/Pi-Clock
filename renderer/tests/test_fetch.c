/* fetch.c with a fake wget on PATH: plain replies, a wget whose
 * ssl_client-like child outlives it holding stdout, and no pipe ends
 * leaking into another thread's wget */
#include "check.h"
#include "../fetch.h"
#include "../spacetrack.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
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
          "  https://www.space-track.org/ajaxauth/login) echo \"$*\" >> /tmp/pic_fake_wget/st.args;"
          " o=; pf=; prev=; for a; do [ \"$prev\" = -O ] && o=$a; [ \"$prev\" = --post-file ] && pf=$a; prev=$a; done;"
          " cp \"$pf\" /tmp/pic_fake_wget/st.body;"
          " if grep -q wrongpw \"$pf\"; then printf '{\"Login\":\"Failed\"}' > \"$o\";"
          " else printf '0 ISS (ZARYA)\\n1 25544U line\\n2 25544 line\\n' > \"$o\"; fi ;;\n"
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

    /* Space-Track: credentials only in the private POST body (deleted
     * afterwards), never in wget's arguments; 3LE tidied; a refused
     * sign-in stops further requests */
    {
        FILE *c = fopen("/tmp/pic_fake_wget/st.cred", "w"), *r;
        char line[256], args[2048] = "";
        int ok;
        fputs("me@example.com\ns3cret pw&x\n", c);
        fclose(c);
        setenv("PIC_SPACETRACK_CRED", "/tmp/pic_fake_wget/st.cred", 1);
        setenv("PIC_SPACETRACK_STATE", "/tmp/pic_fake_wget/st.state", 1);
        CHECK(pic_spacetrack_configured(), "account read");
        ok = pic_spacetrack_fetch("/basicspacedata/query/class/gp/NORAD_CAT_ID/25544/format/3le",
                                  "/tmp/pic_fake_wget/st.out") == 0;
        CHECK(ok, "space-track fetch");
        r = fopen("/tmp/pic_fake_wget/st.out", "r");
        CHECK(r && fgets(line, sizeof(line), r) && strncmp(line, "ISS (ZARYA)", 11) == 0, "name line tidied");
        if (r) fclose(r);
        r = fopen("/tmp/pic_fake_wget/st.args", "r");
        if (r) { size_t k = fread(args, 1, sizeof(args) - 1, r); args[k] = 0; fclose(r); }
        CHECK(!strstr(args, "s3cret") && !strstr(args, "me@example"), "no credentials on the command line");
        r = fopen("/tmp/pic_fake_wget/st.body", "r");
        CHECK(r && fgets(line, sizeof(line), r) && strstr(line, "password=s3cret%20pw%26x") &&
              strstr(line, "identity=me%40example.com"), "form body encoded");
        if (r) fclose(r);
        {   /* the temp body file is gone */
            char cmdl[128];
            snprintf(cmdl, sizeof(cmdl), "ls /tmp/pi-clock-post-* >/dev/null 2>&1");
            CHECK(system(cmdl) != 0, "post body deleted");
        }
        c = fopen("/tmp/pic_fake_wget/st.cred", "w");
        fputs("me@example.com\nwrongpw\n", c);
        fclose(c);
        CHECK(pic_spacetrack_fetch("/x", "/tmp/pic_fake_wget/st.out") != 0, "refused sign-in fails");
        unlink("/tmp/pic_fake_wget/st.args");
        CHECK(pic_spacetrack_fetch("/x", "/tmp/pic_fake_wget/st.out") != 0 &&
              access("/tmp/pic_fake_wget/st.args", F_OK) != 0, "no request after a refused sign-in");
    }

    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd)) { /* ignore */ }
    return CHECK_DONE("test_fetch");
}

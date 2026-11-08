/*
 * fetch.c - Safe HTTP fetch helpers using fork/execvp
 *
 * All downloads go through wget called via execvp — no shell
 * interpretation of URLs or paths. This prevents command injection
 * even if a URL or filename contains shell metacharacters.
 *
 * A global User-Agent string is sent with all requests:
 *   "Pi-Clock v{version} for {callsign}"
 *
 * Copyright (C) 2026 Andy Taylor (MW0MWZ)
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* pipe2 */
#endif
#include "fetch.h"
#include "spacetrack.h"
#include "datastatus.h"
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>

/* Global User-Agent string — set once at startup */
static char g_useragent[128] = "Pi-Clock";

/*
 * pic_fetch_set_useragent - Set the User-Agent for all HTTP requests.
 */
void pic_fetch_set_useragent(const char *version, const char *callsign)
{
    if (callsign && callsign[0]) {
        snprintf(g_useragent, sizeof(g_useragent),
                 "Pi-Clock v%s for %s", version, callsign);
    } else {
        snprintf(g_useragent, sizeof(g_useragent),
                 "Pi-Clock v%s", version);
    }
    printf("fetch: User-Agent: %s\n", g_useragent);
}

/*
 * pic_fetch_get_useragent - Return the current User-Agent string.
 */
const char *pic_fetch_get_useragent(void)
{
    return g_useragent;
}

/* Thread-local: an HTTP error answer (404, 502...) still means the
 * source is up (callers that expect missing items, like map tiles) */
__thread int pic_status_http_ok;

/* wget's error text -> short description; *http set when the server
 * answered with an HTTP error */
static void classify_error(const char *err, int timed_out, char *out, size_t n, int *http)
{
    const char *h = strstr(err, "HTTP/1."), *g = strstr(err, "ERROR ");
    *http = 0;
    if (timed_out || strstr(err, "timed out")) snprintf(out, n, "timed out");
    else if (h && strlen(h) > 12) {                 /* BusyBox */
        *http = 1;
        snprintf(out, n, "HTTP %.3s", h + 9);
    } else if (g && isdigit((unsigned char)g[6])) { /* GNU wget */
        *http = 1;
        snprintf(out, n, "HTTP %.3s", g + 6);
    } else if (strstr(err, "bad address") || strstr(err, "resolve"))
        snprintf(out, n, "DNS lookup failed");
    else if (strstr(err, "refused")) snprintf(out, n, "connection refused");
    else if (strstr(err, "unreachable") || strstr(err, "No route"))
        snprintf(out, n, "network unreachable");
    else if (strstr(err, "SSL") || strstr(err, "TLS") || strstr(err, "ssl"))
        snprintf(out, n, "TLS error");
    else snprintf(out, n, "download failed");
}

/* Read what the child wrote to its stderr pipe (non-blocking) */
/*
 * Pipes are close-on-exec: wget gets its end through dup2 (which
 * clears the flag on the copy) and no other child inherits it. Without
 * this, a wget started by one thread kept the write end of another
 * thread's pipe open, and a hung child (BusyBox wget's ssl_client
 * stuck on a dead TLS connection) left several fetchers waiting for an
 * end of file forever.
 */
static int cpipe(int fd[2])
{
    return pipe2(fd, O_CLOEXEC);
}

/* In the child, before exec: own process group, so a timeout can kill
 * wget together with the ssl_client it starts for https */
static void child_setup(void)
{
    setpgid(0, 0);
}

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int child_exited(pid_t pid)
{
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    return waitid(P_PID, (id_t)pid, &si, WEXITED | WNOHANG | WNOWAIT) == 0 && si.si_pid == pid;
}

/* Read the child's stdout into buf until EOF, the buffer is full,
 * `limit_s` seconds pass, or wget has exited and nothing more comes
 * (a leftover ssl_client can hold the pipe open). Returns bytes read;
 * *timed_out set on the time limit. */
static int read_until(int fd, char *buf, int max, double limit_s, pid_t pid, int *timed_out)
{
    double end = mono_now() + limit_s;
    int n = 0;
    *timed_out = 0;
    while (n < max) {
        struct pollfd pf = { fd, POLLIN, 0 };
        double left = end - mono_now();
        int r;
        if (left <= 0) { *timed_out = 1; break; }
        r = poll(&pf, 1, left > 0.5 ? 500 : (int)(left * 1000) + 1);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) break;
        if (r == 0) {
            if (child_exited(pid)) break;
            continue;
        }
        r = (int)read(fd, buf + n, (size_t)(max - n));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += r;
    }
    return n;
}

/*
 * Wait up to `ticks` x `tick_us` for the child to exit, then kill its
 * whole process group (wget's ssl_client too, which can outlive wget)
 * and reap it. The group is killed while the child is still a zombie,
 * so its id cannot have been reused. Returns 1 if it had to be killed.
 */
static int finish_child(pid_t pid, int ticks, useconds_t tick_us, int *status)
{
    int t = 0, killed = 0;
    for (;;) {
        if (child_exited(pid))
            break;
        if (t++ >= ticks) { killed = 1; break; }
        usleep(tick_us);
    }
    kill(-pid, SIGKILL);
    if (killed) kill(pid, SIGKILL);
    *status = -1;
    while (waitpid(pid, status, 0) < 0 && errno == EINTR) {}
    return killed;
}

static void read_err(int fd, char *buf, size_t n)
{
    ssize_t r;
    size_t got = 0;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    while (got < n - 1 && (r = read(fd, buf + got, n - 1 - got)) > 0) got += (size_t)r;
    buf[got] = '\0';
    close(fd);
}

/* Record the outcome; returns 1 when the server answered with an HTTP
 * error (404, 502...) rather than not answering at all */
static int report(const char *url, int ok, const char *err, int timed_out, int rc)
{
    char why[48];
    int http;
    if (ok) { pic_status_report(url, 1, NULL); return 0; }
    classify_error(err, timed_out, why, sizeof(why), &http);
    /* GNU wget is silent with -q: use its exit code instead */
    if (!err[0] && !timed_out) {
        if (rc == 8) { http = 1; snprintf(why, sizeof(why), "HTTP error"); }
        else if (rc == 4) snprintf(why, sizeof(why), "network failure");
        else if (rc == 5) snprintf(why, sizeof(why), "TLS error");
    }
    pic_status_report(url, http && pic_status_http_ok, why);
    return http;
}

/*
 * CelesTrak guard. CelesTrak firewalls addresses that ask too often,
 * and a client that keeps retrying while blocked keeps itself blocked.
 * Every request to celestrak.org from any feature goes through here:
 *   - at most CT_PER_HOUR requests in any hour, across all features;
 *   - after a failure, nothing at all for 6 h, doubling to 24 h while
 *     failures continue; a success clears it.
 * The state lives in /data/cache (survives restarts and reboots), so
 * restarting the renderer can't start a burst.
 */
#define CT_STATE    "/data/cache/pi-clock-celestrak.state"
#define CT_PER_HOUR 6
static pthread_mutex_t ct_mu = PTHREAD_MUTEX_INITIALIZER;
static int ct_loaded;
static time_t ct_pause_until, ct_backoff, ct_recent[CT_PER_HOUR];

static int is_celestrak(const char *url)
{
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    return strncmp(h, "celestrak.org", 13) == 0 || strncmp(h, "www.celestrak.org", 17) == 0;
}

static const char *ct_path(void)
{
    const char *e = getenv("PIC_CELESTRAK_STATE");    /* tests */
    return e && *e ? e : CT_STATE;
}

static void ct_load(void)
{
    FILE *f;
    int i;
    if (ct_loaded) return;
    ct_loaded = 1;
    f = fopen(ct_path(), "r");
    if (!f) return;
    if (fscanf(f, "%ld %ld", (long *)&ct_pause_until, (long *)&ct_backoff) != 2)
        ct_pause_until = ct_backoff = 0;
    for (i = 0; i < CT_PER_HOUR; i++)
        if (fscanf(f, "%ld", (long *)&ct_recent[i]) != 1) ct_recent[i] = 0;
    fclose(f);
}

static void ct_save(void)
{
    char tmp[256];
    FILE *f;
    int i;
    mkdir("/data/cache", 0755);
    snprintf(tmp, sizeof(tmp), "%s.tmp", ct_path());
    f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%ld %ld\n", (long)ct_pause_until, (long)ct_backoff);
    for (i = 0; i < CT_PER_HOUR; i++) fprintf(f, "%ld\n", (long)ct_recent[i]);
    fclose(f);
    rename(tmp, ct_path());
}

/* 0: go ahead (the request is counted); otherwise why not, in why */
static int ct_hold(const char *url, char *why, size_t n)
{
    time_t now = time(NULL);
    int i, oldest = 0, held = 0;
    char share[300];
    if (!is_celestrak(url)) return 0;
    if (pic_orbit_share(share, sizeof(share))) {
        /* one machine fetches orbits for the house: never CelesTrak here */
        snprintf(why, n, "not used: orbits come from %s", share);
        return 1;
    }
    pthread_mutex_lock(&ct_mu);
    ct_load();
    if (now < ct_pause_until) {
        snprintf(why, n, "held back after a failure (%ld min left)", (long)(ct_pause_until - now + 59) / 60);
        held = 1;
    } else {
        for (i = 1; i < CT_PER_HOUR; i++) if (ct_recent[i] < ct_recent[oldest]) oldest = i;
        if (now - ct_recent[oldest] < 3600 && ct_recent[oldest] <= now) {
            snprintf(why, n, "held back (%d requests an hour max)", CT_PER_HOUR);
            held = 1;
        } else {
            ct_recent[oldest] = now;
            ct_save();
        }
    }
    pthread_mutex_unlock(&ct_mu);
    return held;
}

static void ct_result(const char *url, int ok)
{
    time_t now = time(NULL);
    if (!is_celestrak(url)) return;
    pthread_mutex_lock(&ct_mu);
    ct_load();
    if (ok) {
        ct_pause_until = 0;
        ct_backoff = 0;
    } else {
        ct_backoff = ct_backoff ? ct_backoff * 2 : 6 * 3600;
        if (ct_backoff > 24 * 3600) ct_backoff = 24 * 3600;
        ct_pause_until = now + ct_backoff;
        printf("fetch: CelesTrak failed, no CelesTrak requests for %ld h\n", (long)ct_backoff / 3600);
    }
    ct_save();
    pthread_mutex_unlock(&ct_mu);
}

/*
 * pic_fetch_to_buf - Fetch a URL into a memory buffer via wget.
 *
 * Creates a pipe, forks a child that execs wget with stdout redirected
 * to the write end. The parent reads from the read end into buf.
 *
 * Returns the number of bytes read, or -1 on failure.
 */
static int fetch_impl(const char *url, const char *header, char *buf,
                      int buf_size)
{
    int pipefd[2], errfd[2];
    pid_t pid;
    int n;
    char err[256];

    if (ct_hold(url, err, sizeof(err))) {
        pic_status_report(url, 0, err);
        return -1;
    }
    if (cpipe(pipefd) < 0) return -1;
    if (cpipe(errfd) < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        close(errfd[0]);
        close(errfd[1]);
        return -1;
    }

    if (pid == 0) {
        child_setup();
        /* Child: stdout to the pipe, stderr (error text) to errfd */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        close(errfd[0]);
        dup2(errfd[1], STDERR_FILENO);
        close(errfd[1]);

        if (header)
            execlp("wget", "wget", "-q", "-O", "-", "--timeout=10",
                   "-U", g_useragent, "--header", header, url, NULL);
        else
            execlp("wget", "wget", "-q", "-O", "-",
                   "--timeout=10", "-U", g_useragent, url, NULL);
        _exit(127);
    }

    /* Parent: read from pipe */
    close(pipefd[1]);
    close(errfd[1]);

    /* A hung wget must not block the calling thread: at most 90 s of
     * reading, then 30 s for it to exit, then the group is killed */
    {
        int status = -1, slow, killed;
        n = read_until(pipefd[0], buf, buf_size - 1, 90.0, pid, &slow);
        close(pipefd[0]);
        buf[n] = '\0';
        killed = finish_child(pid, slow ? 0 : 300, 100000, &status) || slow;
        if (killed) n = 0;   /* cut off part-way: not a whole reply */
        read_err(errfd[0], err, sizeof(err));
        if (n <= 0 && !err[0] && WIFEXITED(status) && WEXITSTATUS(status) == 0)
            snprintf(err, sizeof(err), "empty reply");
        report(url, n > 0, err, killed,
               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        ct_result(url, n > 0);
    }

    return n > 0 ? n : -1;
}

int pic_fetch_to_buf(const char *url, char *buf, int buf_size)
{
    return fetch_impl(url, NULL, buf, buf_size);
}

int pic_fetch_to_buf_header(const char *url, const char *header, char *buf,
                            int buf_size)
{
    return fetch_impl(url, header, buf, buf_size);
}

/*
 * pic_fetch_to_file - Download a URL to a file via wget.
 *
 * Returns 0 on success (wget exit 0), -1 on any failure.
 */
int pic_fetch_to_file(const char *url, const char *path, int timeout)
{
    return pic_fetch_to_file_rc(url, path, timeout) == 0 ? 0 : -1;
}

int pic_fetch_to_file_rc(const char *url, const char *path, int timeout)
{
    pid_t pid;
    int status = -1, errfd[2];
    char timeout_str[16], err[256];

    snprintf(timeout_str, sizeof(timeout_str), "--timeout=%d", timeout);

    if (ct_hold(url, err, sizeof(err))) {
        pic_status_report(url, 0, err);
        return -1;
    }
    if (cpipe(errfd) < 0) return -1;
    pid = fork();
    if (pid < 0) {
        close(errfd[0]);
        close(errfd[1]);
        return -1;
    }

    if (pid == 0) {
        child_setup();
        /* stderr (wget's error text) to the pipe */
        close(errfd[0]);
        dup2(errfd[1], STDERR_FILENO);
        close(errfd[1]);

        execlp("wget", "wget", "-q", "-O", path,
               timeout_str, "-U", g_useragent, url, NULL);
        _exit(127);
    }

    /* Wait with timeout — prevent hung wget from blocking the thread.
     * Polled every 50 ms so small files (map tiles) don't each cost a
     * whole second. */
    {
        int killed;
        close(errfd[1]);
        killed = finish_child(pid, 600, 50000, &status);
        read_err(errfd[0], err, sizeof(err));
        ct_result(url, !killed && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        /* BusyBox wget exits 1 for every failure; report a server's
         * HTTP error as 8, as GNU wget does, so callers can tell "not
         * there" (e.g. a map tile not made yet) from "can't reach" */
        if (report(url, WIFEXITED(status) && WEXITSTATUS(status) == 0, err, killed,
                   WIFEXITED(status) ? WEXITSTATUS(status) : -1) &&
            WIFEXITED(status) && WEXITSTATUS(status) == 1)
            return 8;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/*
 * pic_fetch_post_json - POST a JSON body and read the response.
 *
 * The body is written to a private (0600) temp file and sent with
 * --post-file rather than --post-data, so request contents such as
 * API keys never appear on a process command line (visible in ps).
 * Returns bytes read, or -1 on failure; buf is null-terminated.
 */
int pic_fetch_post_json(const char *url, const char *json,
                        char *buf, int buf_size)
{
    char tmp[] = "/tmp/pi-clock-post-XXXXXX";
    int pipefd[2], fd, n, status_out = -1;
    size_t len = strlen(json);
    pid_t pid;

    fd = mkstemp(tmp);              /* created with mode 0600 */
    if (fd < 0) return -1;
    if (write(fd, json, len) != (ssize_t)len) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);

    if (cpipe(pipefd) < 0) {
        unlink(tmp);
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        unlink(tmp);
        return -1;
    }

    if (pid == 0) {
        child_setup();
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        if (!freopen("/dev/null", "w", stderr)) { /* keep going */ }
        execlp("wget", "wget", "-q", "-O", "-", "--timeout=15",
               "-U", g_useragent,
               "--header", "Content-Type: application/json",
               "--post-file", tmp, url, NULL);
        _exit(127);
    }

    close(pipefd[1]);
    {
        int status = -1, slow;
        n = read_until(pipefd[0], buf, buf_size - 1, 60.0, pid, &slow);
        close(pipefd[0]);
        buf[n] = '\0';
        if (finish_child(pid, slow ? 0 : 300, 100000, &status) || slow) {
            n = 0;
            status = -1;
        }
        status_out = status;
    }
    unlink(tmp);

    /* An empty body is still success if wget exited cleanly (e.g. a
     * Home Assistant webhook answers 200 with no content). */
    pic_status_report(url, n > 0 || (WIFEXITED(status_out) && WEXITSTATUS(status_out) == 0),
                      "no response or HTTP error");
    if (n > 0) return n;
    return (WIFEXITED(status_out) && WEXITSTATUS(status_out) == 0) ? 0 : -1;
}

/*
 * pic_fetch_post_form_to_file - POST an urlencoded form (body in a
 * private 0600 temp file, never on a command line) and save the reply
 * to path. Returns 0 on success, -1 on failure. The body may hold
 * credentials: it is deleted as soon as wget has finished.
 */
int pic_fetch_post_form_to_file(const char *url, const char *body,
                                const char *path, int timeout)
{
    char tmp[] = "/tmp/pi-clock-post-XXXXXX", timeout_str[16], err[256];
    size_t len = strlen(body);
    int fd, status = -1, errfd[2], killed;
    pid_t pid;

    fd = mkstemp(tmp);
    if (fd < 0) return -1;
    if (write(fd, body, len) != (ssize_t)len) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);
    snprintf(timeout_str, sizeof(timeout_str), "--timeout=%d", timeout);
    if (cpipe(errfd) < 0) { unlink(tmp); return -1; }
    pid = fork();
    if (pid < 0) {
        close(errfd[0]);
        close(errfd[1]);
        unlink(tmp);
        return -1;
    }
    if (pid == 0) {
        child_setup();
        close(errfd[0]);
        dup2(errfd[1], STDERR_FILENO);
        close(errfd[1]);
        execlp("wget", "wget", "-q", "-O", path, timeout_str, "-U", g_useragent,
               "--header", "Content-Type: application/x-www-form-urlencoded",
               "--post-file", tmp, url, NULL);
        _exit(127);
    }
    close(errfd[1]);
    killed = finish_child(pid, 1200, 100000, &status);
    read_err(errfd[0], err, sizeof(err));
    unlink(tmp);
    report(url, !killed && WIFEXITED(status) && WEXITSTATUS(status) == 0, err, killed,
           WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return !killed && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

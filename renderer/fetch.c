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

#include "fetch.h"
#include "datastatus.h"
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

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

    if (pipe(pipefd) < 0) return -1;
    if (pipe(errfd) < 0) {
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

    n = 0;
    while (n < buf_size - 1) {
        int r = read(pipefd[0], buf + n, buf_size - 1 - n);
        if (r <= 0) break;
        n += r;
    }
    close(pipefd[0]);
    buf[n] = '\0';

    /* Wait for child with a timeout — a hung wget must not block
     * the calling thread indefinitely (prevents clean shutdown). */
    {
        int status = -1, waited = 0;
        while (waitpid(pid, &status, WNOHANG) == 0 && waited < 30) {
            sleep(1);
            waited++;
        }
        if (waited >= 30) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
        read_err(errfd[0], err, sizeof(err));
        if (n <= 0 && !err[0] && WIFEXITED(status) && WEXITSTATUS(status) == 0)
            snprintf(err, sizeof(err), "empty reply");
        report(url, n > 0, err, waited >= 30,
               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
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

    if (pipe(errfd) < 0) return -1;
    pid = fork();
    if (pid < 0) {
        close(errfd[0]);
        close(errfd[1]);
        return -1;
    }

    if (pid == 0) {
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
        int waited = 0;
        while (waitpid(pid, &status, WNOHANG) == 0 && waited < 600) {
            usleep(50000);
            waited++;
        }
        if (waited >= 600) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
        close(errfd[1]);
        read_err(errfd[0], err, sizeof(err));
        /* BusyBox wget exits 1 for every failure; report a server's
         * HTTP error as 8, as GNU wget does, so callers can tell "not
         * there" (e.g. a map tile not made yet) from "can't reach" */
        if (report(url, WIFEXITED(status) && WEXITSTATUS(status) == 0, err, waited >= 600,
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

    if (pipe(pipefd) < 0) {
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
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        freopen("/dev/null", "w", stderr);
        execlp("wget", "wget", "-q", "-O", "-", "--timeout=15",
               "-U", g_useragent,
               "--header", "Content-Type: application/json",
               "--post-file", tmp, url, NULL);
        _exit(127);
    }

    close(pipefd[1]);
    n = 0;
    while (n < buf_size - 1) {
        int r = read(pipefd[0], buf + n, buf_size - 1 - n);
        if (r <= 0) break;
        n += r;
    }
    close(pipefd[0]);
    buf[n] = '\0';

    {
        int status = -1, waited = 0;
        while (waitpid(pid, &status, WNOHANG) == 0 && waited < 30) {
            sleep(1);
            waited++;
        }
        if (waited >= 30) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
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

/*
 * spacetrack.c - Space-Track.org as a back-up source of orbital data
 *
 * When CelesTrak can't be reached, the satellite features ask
 * Space-Track (US Space Force, free account) for the same data. The
 * account is entered in the dashboard and stored root-only in
 * SPACETRACK_CRED ("identity\npassword\n"); it is never logged, never
 * put on a command line, and only sent to www.space-track.org.
 *
 * Each fetch signs in (POST /ajaxauth/login), runs the query with the
 * session cookie and signs out, over OpenSSL directly so neither the
 * password nor the cookie ever appears on a command line. Space-Track's guidelines: under 30
 * requests a minute and 300 an hour, and don't fetch the same GP data
 * more than about once an hour. This stays far inside that: at most
 * 12 requests an hour, and a failed sign-in stops all requests for a
 * day (repeated bad logins can lock an account). State is kept in
 * /data/cache, so restarts can't add requests.
 *
 * Replies come as 3LE ("0 NAME" lines); they are rewritten to the
 * CelesTrak TLE layout (name line without the "0 ") so the callers'
 * parsers read them unchanged.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "spacetrack.h"
#include "feature.h"
#include "fetch.h"
#include "datastatus.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#define ST_BASE     "https://www.space-track.org"
#define ST_STATE    "/data/cache/pi-clock-spacetrack.state"
#define ST_PER_HOUR 12

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int loaded;
static time_t pause_until, pause_stamp, recent[ST_PER_HOUR];

static const char *cred_path(void)
{
    const char *e = getenv("PIC_SPACETRACK_CRED");      /* tests */
    return e && *e ? e : SPACETRACK_CRED;
}

static const char *state_path(void)
{
    const char *e = getenv("PIC_SPACETRACK_STATE");     /* tests */
    return e && *e ? e : ST_STATE;
}

static void load(void)
{
    FILE *f;
    int i;
    if (loaded) return;
    loaded = 1;
    f = fopen(state_path(), "r");
    if (!f) return;
    if (fscanf(f, "%ld", (long *)&pause_until) != 1) pause_until = 0;
    for (i = 0; i < ST_PER_HOUR; i++)
        if (fscanf(f, "%ld", (long *)&recent[i]) != 1) recent[i] = 0;
    if (fscanf(f, "%ld", (long *)&pause_stamp) != 1) pause_stamp = 0;
    fclose(f);
}

static void save(void)
{
    char tmp[256];
    FILE *f;
    int i;
    mkdir("/data/cache", 0755);
    snprintf(tmp, sizeof(tmp), "%s.tmp", state_path());
    f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "%ld\n", (long)pause_until);
    for (i = 0; i < ST_PER_HOUR; i++) fprintf(f, "%ld\n", (long)recent[i]);
    fprintf(f, "%ld\n", (long)pause_stamp);
    fclose(f);
    rename(tmp, state_path());
}

/* identity and password from the root-only file; 0 if none */
static int read_cred(char *id, size_t idn, char *pw, size_t pwn)
{
    FILE *f = fopen(cred_path(), "r");
    int ok = 0;
    if (!f) return 0;
    if (fgets(id, (int)idn, f) && fgets(pw, (int)pwn, f)) {
        id[strcspn(id, "\r\n")] = 0;
        pw[strcspn(pw, "\r\n")] = 0;
        ok = id[0] && pw[0];
    }
    fclose(f);
    return ok;
}

time_t pic_spacetrack_stamp(void)
{
    struct stat st;
    return stat(cred_path(), &st) == 0 && st.st_size > 0 ? st.st_mtime : 0;
}

int pic_orbit_share(char *out, size_t n)
{
    char v[300] = "";
    const char *e = getenv("PIC_ORBIT_SHARE");          /* tests */
    size_t k;
    if (e) snprintf(v, sizeof(v), "%s", e);
    else pic_conf_str("ORBIT_SHARE_URL", v, sizeof(v));
    k = strlen(v);
    while (k && v[k - 1] == '/') v[--k] = 0;
    if (strncmp(v, "http://", 7) != 0 || k < 8) {
        if (n) out[0] = 0;
        return 0;
    }
    snprintf(out, n, "%s", v);
    return 1;
}

int pic_spacetrack_configured(void)
{
    char id[160], pw[160], share[300];
    int ok;
    if (pic_orbit_share(share, sizeof(share))) return 1;   /* the PC asks for us */
    ok = read_cred(id, sizeof(id), pw, sizeof(pw));
    memset(pw, 0, sizeof(pw));
    return ok;
}

/* application/x-www-form-urlencoded */
static void urlenc(char *out, size_t n, const char *in)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = (char)c;
        else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

/* 3LE -> CelesTrak-style TLE (drop the "0 " before names); checks it
 * really is TLE data. Returns the number of satellites. */
static int tidy_tle(const char *path)
{
    FILE *in = fopen(path, "r"), *out;
    char line[256], tmp[300];
    int n = 0;
    if (!in) return 0;
    snprintf(tmp, sizeof(tmp), "%s.tidy", path);
    out = fopen(tmp, "w");
    if (!out) { fclose(in); return 0; }
    while (fgets(line, sizeof(line), in)) {
        if (line[0] == '0' && line[1] == ' ') fputs(line + 2, out);
        else {
            if (line[0] == '2' && line[1] == ' ') n++;
            fputs(line, out);
        }
    }
    fclose(in);
    fclose(out);
    if (n > 0) rename(tmp, path);
    else unlink(tmp);
    return n;
}

#define ST_HOST "www.space-track.org"

/*
 * One HTTPS request to Space-Track (HTTP/1.0, so no chunking and the
 * server closes the connection). The certificate and host name are
 * verified. Reply body goes to `out` (if given); cookies from the
 * reply are collected into `cookies` as "a=b; c=d". Returns the HTTP
 * status, or -1. Headers and body may hold the password or the
 * session cookie: they live only in this process's memory.
 */
static int st_https(const char *method, const char *path, const char *cookie,
                    const char *body, FILE *out, char *cookies, size_t cn)
{
    struct addrinfo hints, *ai = NULL, *a;
    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    int fd = -1, status = -1, n, inhdr = 1;
    char *req = NULL, buf[8192], hdr[8192];   /* req on the heap: ID lists run to several KB */
    size_t hl = 0;
    struct timeval tv = { 30, 0 };

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(ST_HOST, "443", &hints, &ai) != 0) return -1;
    for (a = ai; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0) return -1;

    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) goto done;
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_set_default_verify_paths(ctx) != 1) goto done;
    ssl = SSL_new(ctx);
    if (!ssl) goto done;
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, ST_HOST);
    SSL_set1_host(ssl, ST_HOST);
    if (SSL_connect(ssl) != 1) goto done;

    {
        char extra[1200] = "";
        if (cookie && *cookie)
            snprintf(extra, sizeof(extra), "Cookie: %s\r\n", cookie);
        if (body) {
            size_t el = strlen(extra);
            snprintf(extra + el, sizeof(extra) - el,
                     "Content-Type: application/x-www-form-urlencoded\r\n"
                     "Content-Length: %zu\r\n", strlen(body));
        }
        n = asprintf(&req,
                     "%s %s HTTP/1.0\r\nHost: " ST_HOST "\r\nUser-Agent: %s\r\n%s\r\n",
                     method, path, pic_fetch_get_useragent(), extra);
        memset(extra, 0, sizeof(extra));
    }
    if (n <= 0 || !req) { req = NULL; goto done; }
    if (SSL_write(ssl, req, n) != n) goto done;
    if (body && SSL_write(ssl, body, (int)strlen(body)) != (int)strlen(body)) goto done;

    if (cookies && cn) cookies[0] = 0;
    while ((n = SSL_read(ssl, buf, sizeof(buf))) > 0) {
        int off = 0;
        if (inhdr) {
            char *end;
            size_t take = (size_t)n < sizeof(hdr) - 1 - hl ? (size_t)n : sizeof(hdr) - 1 - hl;
            memcpy(hdr + hl, buf, take);
            hl += take;
            hdr[hl] = 0;
            end = strstr(hdr, "\r\n\r\n");
            if (!end) continue;
            inhdr = 0;
            off = (int)((end + 4 - hdr) - (hl - take));
            {
                char *line, *sp;
                if (sscanf(hdr, "HTTP/%*s %d", &status) != 1) status = -1;
                for (line = strtok_r(hdr, "\r\n", &sp); line; line = strtok_r(NULL, "\r\n", &sp)) {
                    if (cookies && strncasecmp(line, "Set-Cookie:", 11) == 0) {
                        char *v = line + 11;
                        size_t used = strlen(cookies);
                        while (*v == ' ') v++;
                        v[strcspn(v, ";")] = 0;
                        snprintf(cookies + used, cn - used, "%s%s", used ? "; " : "", v);
                    }
                }
            }
            memset(hdr, 0, sizeof(hdr));
        }
        if (out && n > off) fwrite(buf + off, 1, (size_t)(n - off), out);
    }
done:
    if (req) { OPENSSL_cleanse(req, strlen(req)); free(req); }   /* held the cookie */
    if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
    if (ctx) SSL_CTX_free(ctx);
    if (fd >= 0) close(fd);
    return status;
}

static pic_spacetrack_transport st_req = st_https;

void pic_spacetrack_set_transport(pic_spacetrack_transport fn)
{
    st_req = fn ? fn : st_https;
}

/* One session: sign in, run each query into its path, sign out.
 * tidy: convert 3LE replies to CelesTrak layout (0 sats = failure).
 * Returns how many queries succeeded, or -1 if none could run. */
static int st_session(const char *const *query, const char *const *path, int nq, int tidy)
{
    char id[160], pw[160], body[1100], eid[480], epw[480];
    time_t now = time(NULL);
    int i, oldest = 0, n, good = 0;
    char head[64] = "";
    char share[300];

    /* Orbits shared by a PC on the network: ask it, never Space-Track.
     * The PC caches every answer and keeps its own request cap. */
    if (pic_orbit_share(share, sizeof(share))) {
        for (i = 0; i < nq; i++) {
            size_t ql = strlen(query[i]);
            char *eq = malloc(3 * ql + 1), *url = malloc(3 * ql + 400);
            if (!eq || !url) { free(eq); free(url); continue; }
            if (strcmp(query[i], ST_Q_LAST30) == 0) {
                snprintf(url, 3 * ql + 400, "%s/api/tle_last30", share);
            } else {
                urlenc(eq, 3 * ql + 1, query[i]);
                snprintf(url, 3 * ql + 400, "%s/api/spacetrack?q=%s", share, eq);
            }
            n = pic_fetch_to_file(url, path[i], 60);
            free(eq);
            free(url);
            if (n != 0) {
                unlink(path[i]);
                printf("spacetrack: the shared copy from %s isn't available\n", share);
                continue;
            }
            if (tidy) {
                n = tidy_tle(path[i]);
                if (n <= 0) {
                    unlink(path[i]);
                    printf("spacetrack: no satellites in the shared copy\n");
                    continue;
                }
                printf("spacetrack: %d satellites (shared copy from the PC)\n", n);
            }
            good++;
        }
        return good ? good : -1;
    }

    if (!read_cred(id, sizeof(id), pw, sizeof(pw))) return -1;

    pthread_mutex_lock(&mu);
    load();
    if (now < pause_until && pic_spacetrack_stamp() != pause_stamp) {
        /* the account was changed since it was refused: try it */
        pause_until = 0;
        save();
    }
    if (now < pause_until) {
        pthread_mutex_unlock(&mu);
        memset(pw, 0, sizeof(pw));
        pic_status_report(ST_BASE, 0, "held back after a failed sign-in");
        return -1;
    }
    for (i = 1; i < ST_PER_HOUR; i++) if (recent[i] < recent[oldest]) oldest = i;
    if (now - recent[oldest] < 3600 && recent[oldest] <= now) {
        pthread_mutex_unlock(&mu);
        memset(pw, 0, sizeof(pw));
        pic_status_report(ST_BASE, 0, "held back (12 sessions an hour max)");
        return -1;
    }
    recent[oldest] = now;
    save();
    pthread_mutex_unlock(&mu);

    {
        char cookies[1024] = "";
        FILE *f2;
        int st;
        urlenc(eid, sizeof(eid), id);
        urlenc(epw, sizeof(epw), pw);
        snprintf(body, sizeof(body), "identity=%s&password=%s", eid, epw);
        memset(pw, 0, sizeof(pw));
        memset(epw, 0, sizeof(epw));
        /* sign in: "" means yes, {"Login":"Failed"} means no */
        f2 = tmpfile();
        st = st_req("POST", "/ajaxauth/login", NULL, body, f2, cookies, sizeof(cookies));
        memset(body, 0, sizeof(body));
        head[0] = 0;
        if (f2) {
            rewind(f2);
            if (!fgets(head, sizeof(head), f2)) head[0] = 0;
            fclose(f2);
        }
        if (st == 200 && strstr(head, "Failed")) {
            pthread_mutex_lock(&mu);
            pause_until = now + 86400;
            pause_stamp = pic_spacetrack_stamp();
            save();
            pthread_mutex_unlock(&mu);
            printf("spacetrack: sign-in refused, check the account in the dashboard (no requests for 24 h)\n");
            pic_status_report(ST_BASE, 0, "sign-in refused");
            return -1;
        }
        if (st != 200 || !cookies[0]) {
            printf("spacetrack: sign-in failed (HTTP %d)\n", st);
            pic_status_report(ST_BASE, 0, st < 0 ? "can't connect" : "sign-in failed");
            return -1;
        }
        for (i = 0; i < nq; i++) {
            f2 = fopen(path[i], "w");
            if (!f2) continue;
            st = st_req("GET", query[i], cookies, NULL, f2, NULL, 0);
            fclose(f2);
            if (st != 200) {
                printf("spacetrack: query failed (HTTP %d)\n", st);
                unlink(path[i]);
                continue;
            }
            if (tidy) {
                n = tidy_tle(path[i]);
                if (n <= 0) {
                    printf("spacetrack: no satellites in the reply\n");
                    unlink(path[i]);
                    continue;
                }
                printf("spacetrack: %d satellites\n", n);
            }
            good++;
        }
        /* sign out */
        st_req("GET", "/ajaxauth/logout", cookies, NULL, NULL, NULL, 0);
        memset(cookies, 0, sizeof(cookies));
    }
    if (good) pic_status_report(ST_BASE, 1, NULL);
    else pic_status_report(ST_BASE, 0, "query failed");
    return good;
}

int pic_spacetrack_fetch(const char *query, const char *path)
{
    return st_session(&query, &path, 1, 1) == 1 ? 0 : -1;
}

int pic_spacetrack_fetch_raw(const char *const *query, const char *const *path, int n)
{
    return st_session(query, path, n, 0);
}

/*
 * ws.c - Minimal TLS WebSocket client (RFC 6455)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ws.h"
#include "fetch.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>

static int tcp_connect(const char *host, int port)
{
    struct addrinfo hints, *res, *rp;
    char port_str[8];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;

    for (rp = res; rp; rp = rp->ai_next) {
        int flags, err = 0;
        fd_set wfds;
        struct timeval tv = { 10, 0 };
        socklen_t len = sizeof(err);

        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            fcntl(fd, F_SETFL, flags);
            break;
        }
        if (errno == EINPROGRESS) {
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            if (select(fd + 1, NULL, &wfds, NULL, &tv) > 0 &&
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 &&
                err == 0) {
                fcntl(fd, F_SETFL, flags);
                break;
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int read_fully(SSL *ssl, unsigned char *buf, int n)
{
    int total = 0;
    while (total < n) {
        int r = SSL_read(ssl, buf + total, n - total);
        if (r <= 0) return 0;
        total += r;
    }
    return 1;
}

void pic_ws_close(pic_ws_t *ws)
{
    if (ws->ssl) {
        SSL_shutdown(ws->ssl);
        SSL_free(ws->ssl);
    }
    if (ws->fd >= 0) close(ws->fd);
    if (ws->ctx) SSL_CTX_free(ws->ctx);
    ws->ssl = NULL;
    ws->ctx = NULL;
    ws->fd = -1;
}

int pic_ws_connect(pic_ws_t *ws, const char *host, int port,
                   const char *path, int rcv_timeout_s)
{
    struct timeval tv;
    unsigned char nonce[16];
    char key[32], req[768], resp[4096];
    int n, total = 0;
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    memset(ws, 0, sizeof(*ws));
    ws->fd = -1;

    ws->ctx = SSL_CTX_new(TLS_client_method());
    if (!ws->ctx) goto fail;
    SSL_CTX_set_verify(ws->ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_set_default_verify_paths(ws->ctx) != 1) goto fail;

    ws->fd = tcp_connect(host, port);
    if (ws->fd < 0) goto fail;
    tv.tv_sec = rcv_timeout_s;
    tv.tv_usec = 0;
    setsockopt(ws->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 15;
    setsockopt(ws->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    ws->ssl = SSL_new(ws->ctx);
    if (!ws->ssl) goto fail;
    SSL_set_fd(ws->ssl, ws->fd);
    SSL_set_tlsext_host_name(ws->ssl, host);
    SSL_set1_host(ws->ssl, host);
    if (SSL_connect(ws->ssl) != 1) {
        printf("ws: TLS connect to %s failed (%s)\n", host,
               X509_verify_cert_error_string(SSL_get_verify_result(ws->ssl)));
        goto fail;
    }

    /* Sec-WebSocket-Key: 16 random bytes, base64 */
    if (RAND_bytes(nonce, sizeof(nonce)) != 1) goto fail;
    {
        int i, o = 0;
        for (i = 0; i < 15; i += 3) {
            unsigned v = (nonce[i] << 16) | (nonce[i + 1] << 8) | nonce[i + 2];
            key[o++] = b64[(v >> 18) & 63];
            key[o++] = b64[(v >> 12) & 63];
            key[o++] = b64[(v >> 6) & 63];
            key[o++] = b64[v & 63];
        }
        key[o++] = b64[nonce[15] >> 2];
        key[o++] = b64[(nonce[15] & 3) << 4];
        key[o++] = '=';
        key[o++] = '=';
        key[o] = '\0';
    }

    n = snprintf(req, sizeof(req),
                 "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\n"
                 "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
                 path, host, pic_fetch_get_useragent(), key);
    if (n <= 0 || n >= (int)sizeof(req) || SSL_write(ws->ssl, req, n) != n)
        goto fail;

    /* Read the response header byte-wise so no frame data is eaten */
    while (total < (int)sizeof(resp) - 1) {
        if (SSL_read(ws->ssl, resp + total, 1) != 1) goto fail;
        total++;
        resp[total] = '\0';
        if (total >= 4 && !memcmp(resp + total - 4, "\r\n\r\n", 4)) break;
    }
    if (strncmp(resp, "HTTP/1.1 101", 12) != 0) {
        printf("ws: %s handshake refused: %.40s\n", host, resp);
        goto fail;
    }
    return 0;

fail:
    pic_ws_close(ws);
    return -1;
}

static int send_frame(pic_ws_t *ws, int opcode, const unsigned char *data,
                      size_t len)
{
    unsigned char hdr[14], mask[4], *buf;
    size_t h = 0, i;
    int ok;

    if (RAND_bytes(mask, 4) != 1) return -1;
    hdr[h++] = (unsigned char)(0x80 | opcode);
    if (len < 126) {
        hdr[h++] = (unsigned char)(0x80 | len);
    } else if (len < 65536) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (unsigned char)(len >> 8);
        hdr[h++] = (unsigned char)len;
    } else {
        return -1;
    }
    memcpy(hdr + h, mask, 4);
    h += 4;

    buf = malloc(h + len);
    if (!buf) return -1;
    memcpy(buf, hdr, h);
    for (i = 0; i < len; i++) buf[h + i] = data[i] ^ mask[i % 4];
    ok = SSL_write(ws->ssl, buf, (int)(h + len)) == (int)(h + len);
    free(buf);
    return ok ? 0 : -1;
}

int pic_ws_send_text(pic_ws_t *ws, const char *text)
{
    return send_frame(ws, 0x1, (const unsigned char *)text, strlen(text));
}

int pic_ws_read(pic_ws_t *ws, unsigned char *buf, int size)
{
    unsigned char hdr[2], mk[4];
    uint64_t len;
    int opcode, masked;

    if (!read_fully(ws->ssl, hdr, 2)) return -1;
    opcode = hdr[0] & 0x0F;
    masked = (hdr[1] & 0x80) != 0;
    len = hdr[1] & 0x7F;
    if (len == 126) {
        unsigned char e[2];
        if (!read_fully(ws->ssl, e, 2)) return -1;
        len = ((uint64_t)e[0] << 8) | e[1];
    } else if (len == 127) {
        unsigned char e[8];
        int i;
        if (!read_fully(ws->ssl, e, 8)) return -1;
        for (len = 0, i = 0; i < 8; i++) len = (len << 8) | e[i];
        if (len > INT_MAX) return -1;
    }
    if (masked && !read_fully(ws->ssl, mk, 4)) return -1;

    if (len >= (uint64_t)size) {
        /* Too big for the caller: drain to stay in sync */
        unsigned char drain[512];
        while (len > 0) {
            int c = len > sizeof(drain) ? (int)sizeof(drain) : (int)len;
            if (!read_fully(ws->ssl, drain, c)) return -1;
            len -= (uint64_t)c;
        }
        return opcode == 0x8 ? -1 : 0;
    }
    if (len && !read_fully(ws->ssl, buf, (int)len)) return -1;
    if (masked) {
        uint64_t i;
        for (i = 0; i < len; i++) buf[i] ^= mk[i % 4];
    }
    buf[len] = '\0';

    switch (opcode) {
    case 0x8: return -1;                                   /* close */
    case 0x9: send_frame(ws, 0xA, buf, (size_t)len); return 0;  /* ping */
    case 0xA: return 0;                                    /* pong  */
    default:  return (int)len;
    }
}

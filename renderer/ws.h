/*
 * ws.h - Minimal TLS WebSocket client (RFC 6455)
 *
 * Unlike the Blitzortung client in lightning.c, this one verifies the
 * server certificate and hostname, since it carries API keys.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef PIC_WS_H
#define PIC_WS_H

#include <openssl/ssl.h>

typedef struct {
    int      fd;
    SSL_CTX *ctx;
    SSL     *ssl;
} pic_ws_t;

/* Connect to wss://host:port/path. rcv_timeout_s bounds each read. */
int  pic_ws_connect(pic_ws_t *ws, const char *host, int port,
                    const char *path, int rcv_timeout_s);

/* Send one masked text frame of any length. Returns 0 on success. */
int  pic_ws_send_text(pic_ws_t *ws, const char *text);

/*
 * Read one data frame into buf (NUL-terminated). Pings are answered
 * internally. Returns the payload length, 0 for a skipped frame, or
 * -1 on close/error/timeout.
 */
int  pic_ws_read(pic_ws_t *ws, unsigned char *buf, int size);

void pic_ws_close(pic_ws_t *ws);

#endif /* PIC_WS_H */

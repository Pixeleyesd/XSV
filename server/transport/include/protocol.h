/*
 * protocol.h
 *
 * dead simple text handshake the server sends once, right after the
 * client connects on the control port. tells the client where this
 * window sits on the desktop and which udp port its video stream will
 * arrive on.
 *
 * this is deliberately tiny right now, one window per connection, but
 * the important part for later is already here: position on the desktop
 * is part of the handshake, not an afterthought. the end goal is seeing
 * the whole desktop assembled from independently streamed windows, and
 * that only works if every window says where it belongs. once there's
 * more than one window, this just becomes "send one of these per
 * window" and the client keeps all the resulting positioned windows on
 * screen at once instead of just one. the real wire format will
 * probably end up as something denser than a text line (see the
 * transport section of ARCHITECTURE.md), but the shape of the
 * information doesn't change.
 */

#ifndef XSV_PROTOCOL_H
#define XSV_PROTOCOL_H

#include <stdio.h>

#define XSV_HANDSHAKE_FMT "XSV1 X=%d Y=%d W=%d H=%d PORT=%d\n"

typedef struct {
    int x;
    int y;
    int w;
    int h;
    int port;
} xsv_handshake;

static inline int xsv_parse_handshake(const char *line, xsv_handshake *out) {
    return sscanf(line, XSV_HANDSHAKE_FMT, &out->x, &out->y, &out->w, &out->h, &out->port) == 5;
}

#endif

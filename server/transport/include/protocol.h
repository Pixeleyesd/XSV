/*
 * protocol.h
 *
 * dead simple text handshake. right after the client connects on the
 * control port, the server sends a count line followed by one handshake
 * line per window it's about to stream, each saying where that window
 * sits on the desktop and which udp port its video will arrive on.
 *
 * this is the actual mechanism behind "see the whole desktop assembled
 * from independently streamed windows": every window says where it
 * belongs, the client keeps all of them on screen at their real
 * positions at once, and that's the whole trick, there's no separate
 * "desktop view" mode, just more shards. the real wire format will
 * probably end up denser than a text line eventually (see the transport
 * section of ARCHITECTURE.md), but the shape of the information doesn't
 * change.
 */

#ifndef XSV_PROTOCOL_H
#define XSV_PROTOCOL_H

#include <stdio.h>

/* sent once, before any per-window handshakes, so the client knows how
 * many XSV1 lines to expect right after. */
#define XSV_COUNT_FMT "XSVCOUNT %d\n"

/* one of these per window. adding an ID (the window's own X11 id) is
 * the only real change from the single-window version: with more than
 * one shard in flight the client needs some way to tell them apart,
 * both for the initial handshake and later if a window resizes or
 * closes and the server needs to tell the client which shard that
 * update belongs to. */
#define XSV_HANDSHAKE_FMT "XSV1 ID=%lx X=%d Y=%d W=%d H=%d PORT=%d\n"

typedef struct {
    unsigned long id;
    int x;
    int y;
    int w;
    int h;
    int port;
} xsv_handshake;

static inline int xsv_parse_count(const char *line, int *out) {
    return sscanf(line, XSV_COUNT_FMT, out) == 1;
}

static inline int xsv_parse_handshake(const char *line, xsv_handshake *out) {
    return sscanf(line, XSV_HANDSHAKE_FMT, &out->id, &out->x, &out->y, &out->w, &out->h, &out->port) == 6;
}

#endif

/*
 * xsv_server.c
 *
 * enumerates the real windows on the desktop and streams every one of
 * them independently to a connected client: each window gets its own
 * capture, its own encode pipeline, and its own udp port, with the
 * client told exactly where each one belongs on the desktop. this is
 * the actual shards mechanism, not a stand-in for it, a single window
 * is just what it looks like when there happens to be only one.
 *
 * usage: xsv-server <control-port> <base-udp-port> [window-id-hex]
 *   without the optional window id: streams every real top-level window
 *   found on the display.
 *   with it: streams only that one window, useful for testing, or for
 *   the shell-pipeline proof scripts under this same directory that
 *   already expect one window at a fixed port.
 *
 * known issue, not yet resolved: even with a single window, the actual
 * video from real captured content does not currently decode/display
 * correctly on the client end, despite the handshake, positioning, and
 * network transport all being individually verified working. see the
 * "implementation status" section of ARCHITECTURE.md for what's been
 * ruled in and out so far. this file is otherwise a genuine multi-window
 * implementation, not blocked on that bug structurally, it's just not
 * possible to see the end result correctly yet.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include "../../capture/include/capture.h"
#include "../include/protocol.h"

#define MAX_WINDOWS 32

static volatile sig_atomic_t keep_running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    keep_running = 0;
}

typedef struct {
    Window window;
    Damage damage;
    int x, y, w, h;
    int udp_port;
    int encode_w, encode_h; /* w/h rounded down to even, see start_pipeline */
    GstElement *pipeline; /* NULL until the client's known and this shard's pipeline is up */
    GstElement *appsrc;
    guint64 frame_index;
} window_stream;

static window_stream g_streams[MAX_WINDOWS];
static int g_num_streams = 0;

static int round_down_even(int x) {
    return x & ~1;
}

/* the proper way to get "the real application windows" is to ask the
 * window manager, via the standard _NET_CLIENT_LIST property on the
 * root window. this only exists if a compliant window manager is
 * actually running and maintaining it. */
static int enumerate_via_net_client_list(Display *dpy, Window root, Window *out, int max) {
    Atom net_client_list = XInternAtom(dpy, "_NET_CLIENT_LIST", True);
    if (net_client_list == None) return 0;

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, root, net_client_list, 0, max, False, XA_WINDOW,
                            &actual_type, &actual_format, &nitems, &bytes_after, &data) != Success) {
        return 0;
    }
    if (!data || actual_type != XA_WINDOW) {
        if (data) XFree(data);
        return 0;
    }

    Window *wins = (Window *)data;
    int n = (int)(nitems < (unsigned long)max ? nitems : (unsigned long)max);
    for (int i = 0; i < n; i++) out[i] = wins[i];
    XFree(data);
    return n;
}

/* fallback for setups with no window manager at all (this is what the
 * plain xvfb test environment this was developed against needs, and
 * probably other headless/minimal setups too). walks the root window's
 * direct children and keeps anything that looks like a real, visible
 * top-level window: mapped, not override-redirect (which is usually
 * menus, tooltips, and decorations rather than actual application
 * content), and not some tiny 1x1 utility window. */
static int enumerate_via_query_tree(Display *dpy, Window root, Window *out, int max) {
    Window root_ret, parent_ret, *children = NULL;
    unsigned int nchildren = 0;
    if (!XQueryTree(dpy, root, &root_ret, &parent_ret, &children, &nchildren)) {
        return 0;
    }

    int count = 0;
    for (unsigned int i = 0; i < nchildren && count < max; i++) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, children[i], &wa)) continue;
        if (wa.map_state != IsViewable) continue;
        if (wa.override_redirect) continue;
        if (wa.width < 10 || wa.height < 10) continue;
        out[count++] = children[i];
    }
    if (children) XFree(children);
    return count;
}

static int enumerate_windows(Display *dpy, Window root, Window *out, int max) {
    int n = enumerate_via_net_client_list(dpy, root, out, max);
    if (n > 0) return n;
    return enumerate_via_query_tree(dpy, root, out, max);
}

/* the x11 side of getting one window ready: figure out where it really
 * is on the desktop and start watching it for damage. doesn't touch
 * gstreamer at all yet, since building the actual pipeline needs to know
 * the client's address first, and we don't have that until later. */
static int setup_window_x11(Display *dpy, Window win, window_stream *out) {
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(dpy, win, &attrs)) return -1;

    Window child_return;
    int root_x = 0, root_y = 0;
    XTranslateCoordinates(dpy, win, DefaultRootWindow(dpy), 0, 0, &root_x, &root_y, &child_return);

    XCompositeRedirectWindow(dpy, win, CompositeRedirectAutomatic);
    Damage damage = XDamageCreate(dpy, win, XDamageReportBoundingBox);
    if (!damage) return -1;

    /* XGetWindowAttributes' width/height exclude the window's border by
     * spec, but the composite pixmap xsv_capture_window actually reads
     * from turned out to include it, for windows that draw their own
     * border with no reparenting window manager around them to strip it
     * (exactly xterm's situation in the test setup this was built
     * against). that mismatch would otherwise get silently caught by the
     * frame-size check on every single frame instead of being sorted out
     * once, up front. doing one real capture now and trusting its
     * dimensions as authoritative sidesteps the whole category of
     * mismatch rather than just detecting it downstream. */
    xsv_frame probe_frame;
    if (xsv_capture_window(dpy, win, &probe_frame) != 0) {
        XDamageDestroy(dpy, damage);
        return -1;
    }
    out->w = probe_frame.width;
    out->h = probe_frame.height;
    xsv_free_frame(&probe_frame);

    out->window = win;
    out->damage = damage;
    out->x = root_x;
    out->y = root_y;
    out->frame_index = 0;
    out->pipeline = NULL;
    out->appsrc = NULL;
    return 0;
}

static int wait_for_client(int control_port, char *client_ip_out, size_t client_ip_len) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }

    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)control_port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 1) < 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    fprintf(stderr, "waiting for a client on control port %d...\n", control_port);

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
    close(listen_fd); /* only handling one client for now */
    if (client_fd < 0) {
        perror("accept");
        return -1;
    }

    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip_out, (socklen_t)client_ip_len);
    fprintf(stderr, "client connected from %s\n", client_ip_out);
    return client_fd;
}

static int send_handshakes(int client_fd, window_stream *streams, int n) {
    char line[128];
    int len = snprintf(line, sizeof(line), XSV_COUNT_FMT, n);
    if (write(client_fd, line, (size_t)len) != len) {
        perror("write count");
        return -1;
    }

    for (int i = 0; i < n; i++) {
        len = snprintf(line, sizeof(line), XSV_HANDSHAKE_FMT,
                        (unsigned long)streams[i].window, streams[i].x, streams[i].y,
                        streams[i].w, streams[i].h, streams[i].udp_port);
        if (write(client_fd, line, (size_t)len) != len) {
            perror("write handshake");
            return -1;
        }
    }
    return 0;
}

static int start_pipeline(window_stream *s, const char *client_ip) {
    /* h264's yuv420 encoding halves each dimension for the chroma
     * planes, so it can't handle an odd width or height. windows are
     * under no obligation to be even sized (xterm's own border easily
     * makes one, as it turned out), so encode at the nearest even size
     * and crop the extra row/column off when copying frame data in.
     * the real window size is still what's reported to the client for
     * positioning purposes, this only affects the video content itself,
     * by at most one row and one column. */
    s->encode_w = round_down_even(s->w);
    s->encode_h = round_down_even(s->h);

    char pipeline_desc[512];
    snprintf(pipeline_desc, sizeof(pipeline_desc),
        "appsrc name=xsvsrc is-live=true block=true format=time "
        "max-bytes=20971520 "
        "caps=video/x-raw,format=RGB,width=%d,height=%d,framerate=10/1 "
        "! videoconvert ! x264enc tune=zerolatency speed-preset=ultrafast key-int-max=20 "
        "! rtph264pay config-interval=1 pt=96 ! udpsink host=%s port=%d",
        s->encode_w, s->encode_h, client_ip, s->udp_port);

    fprintf(stderr, "window 0x%lx: gst pipeline: %s\n", (unsigned long)s->window, pipeline_desc);

    GError *gerr = NULL;
    GstElement *pipeline = gst_parse_launch(pipeline_desc, &gerr);
    if (!pipeline) {
        fprintf(stderr, "window 0x%lx: failed to build pipeline: %s\n",
                (unsigned long)s->window, gerr ? gerr->message : "unknown error");
        return -1;
    }

    GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "xsvsrc");
    if (!appsrc) {
        fprintf(stderr, "window 0x%lx: no appsrc in pipeline\n", (unsigned long)s->window);
        return -1;
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "window 0x%lx: pipeline failed to start\n", (unsigned long)s->window);
        return -1;
    }

    s->pipeline = pipeline;
    s->appsrc = appsrc;
    return 0;
}

static void push_frame_for_stream(Display *dpy, window_stream *s) {
    xsv_frame frame;
    if (xsv_capture_window(dpy, s->window, &frame) != 0) {
        fprintf(stderr, "window 0x%lx: capture failed\n", (unsigned long)s->window);
        return;
    }

    /* same defensive check as before: the encoder was set up expecting
     * this window's dimensions to stay put. a live resize would change
     * them out from under us, and pushing a mismatched buffer corrupts
     * the encoder for the rest of the stream rather than failing loudly,
     * so it's worth catching explicitly instead of assuming it can't
     * happen. resizing isn't handled yet, this just stops it from
     * silently breaking things until it is. */
    if (frame.width != s->w || frame.height != s->h) {
        fprintf(stderr, "window 0x%lx: frame size changed (%dx%d vs %dx%d), skipping\n",
                (unsigned long)s->window, frame.width, frame.height, s->w, s->h);
        xsv_free_frame(&frame);
        return;
    }

    /* crop to the even encode size with a per-row copy rather than one
     * flat memcpy, since dropping a column (when width is odd) means
     * each row needs fewer bytes than the source has, not just fewer
     * rows overall. the destination stride is also padded up to a
     * multiple of 4 bytes: gstreamer's default raw-video stride
     * calculation assumes 4-byte-aligned rows, and a tightly packed
     * width*3 only happens to already be a multiple of 4 for some
     * widths, not all of them, by coincidence rather than by design. */
    size_t src_stride = (size_t)frame.width * 3;
    size_t row_bytes = (size_t)s->encode_w * 3;
    size_t dst_stride = (row_bytes + 3) & ~(size_t)3;
    size_t dst_size = dst_stride * (size_t)s->encode_h;

    GstBuffer *buf = gst_buffer_new_allocate(NULL, dst_size, NULL);
    GstMapInfo map;
    gst_buffer_map(buf, &map, GST_MAP_WRITE);
    for (int row = 0; row < s->encode_h; row++) {
        memcpy(map.data + (size_t)row * dst_stride, frame.rgb + (size_t)row * src_stride, row_bytes);
    }
    gst_buffer_unmap(buf, &map);

    /* explicit, evenly spaced timestamps rather than do-timestamp=true.
     * damage-driven capture is bursty, several frames in the same
     * millisecond while something's actively changing, then nothing for
     * a while, and stamping buffers by real arrival time reflects that
     * burstiness straight into the encoder instead of smoothing it out. */
    const GstClockTime frame_duration = GST_SECOND / 10;
    GST_BUFFER_PTS(buf) = s->frame_index * frame_duration;
    GST_BUFFER_DURATION(buf) = frame_duration;
    s->frame_index++;

    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(s->appsrc), buf);
    if (ret != GST_FLOW_OK) {
        fprintf(stderr, "window 0x%lx: push_buffer failed, ret=%d\n", (unsigned long)s->window, ret);
    }
    xsv_free_frame(&frame);
}

static void check_bus(window_stream *s) {
    GstBus *bus = gst_element_get_bus(s->pipeline);
    GstMessage *msg;
    while ((msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR | GST_MESSAGE_WARNING))) {
        GError *err = NULL;
        gchar *debug = NULL;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            gst_message_parse_error(msg, &err, &debug);
            fprintf(stderr, "window 0x%lx GST ERROR from %s: %s\n",
                    (unsigned long)s->window, GST_OBJECT_NAME(msg->src), err->message);
        } else {
            gst_message_parse_warning(msg, &err, &debug);
            fprintf(stderr, "window 0x%lx GST WARNING from %s: %s\n",
                    (unsigned long)s->window, GST_OBJECT_NAME(msg->src), err->message);
        }
        g_error_free(err);
        g_free(debug);
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
}

int main(int argc, char **argv) {
    /* a write to a socket the other end already closed raises SIGPIPE,
     * which by default kills the whole process outright with no error
     * message at all, silently, which is exactly what was happening
     * here. ignoring it means a broken write just fails normally and
     * we get to decide what to do about it, standard practice for
     * anything network-facing. */
    signal(SIGPIPE, SIG_IGN);

    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s <control-port> <base-udp-port> [window-id-hex]\n", argv[0]);
        fprintf(stderr, "  without a window id: streams every real window found on the display\n");
        fprintf(stderr, "  with one: streams only that window\n");
        return 1;
    }
    int control_port = atoi(argv[1]);
    int base_udp_port = atoi(argv[2]);
    Window single_target = 0;
    if (argc == 4) single_target = (Window)strtoul(argv[3], NULL, 16);

    gst_init(&argc, &argv);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "could not open display, is DISPLAY set?\n");
        return 1;
    }

    int comp_event_base, comp_error_base;
    if (!XCompositeQueryExtension(dpy, &comp_event_base, &comp_error_base)) {
        fprintf(stderr, "no xcomposite extension on this display\n");
        return 1;
    }
    int dmg_event_base, dmg_error_base;
    if (!XDamageQueryExtension(dpy, &dmg_event_base, &dmg_error_base)) {
        fprintf(stderr, "no xdamage extension on this display\n");
        return 1;
    }
    if (!XFixesQueryExtension(dpy, &(int){0}, &(int){0})) {
        fprintf(stderr, "no xfixes extension on this display\n");
        return 1;
    }

    Window root = DefaultRootWindow(dpy);
    Window found[MAX_WINDOWS];
    int n;
    if (single_target) {
        found[0] = single_target;
        n = 1;
    } else {
        n = enumerate_windows(dpy, root, found, MAX_WINDOWS);
        if (n == 0) {
            fprintf(stderr, "no windows found to stream\n");
            return 1;
        }
    }
    fprintf(stderr, "found %d window%s to stream\n", n, n == 1 ? "" : "s");

    for (int i = 0; i < n; i++) {
        if (setup_window_x11(dpy, found[i], &g_streams[g_num_streams]) == 0) {
            g_streams[g_num_streams].udp_port = base_udp_port + g_num_streams;
            fprintf(stderr, "  window 0x%lx: %dx%d at (%d,%d), udp port %d\n",
                    (unsigned long)g_streams[g_num_streams].window,
                    g_streams[g_num_streams].w, g_streams[g_num_streams].h,
                    g_streams[g_num_streams].x, g_streams[g_num_streams].y,
                    g_streams[g_num_streams].udp_port);
            g_num_streams++;
        } else {
            fprintf(stderr, "  window 0x%lx: skipped, could not set up capture\n", (unsigned long)found[i]);
        }
    }
    if (g_num_streams == 0) {
        fprintf(stderr, "no windows could be set up for capture\n");
        return 1;
    }

    char client_ip[64];
    int client_fd = wait_for_client(control_port, client_ip, sizeof(client_ip));
    if (client_fd < 0) return 1;

    if (send_handshakes(client_fd, g_streams, g_num_streams) != 0) {
        close(client_fd);
        return 1;
    }
    close(client_fd); /* not needed for anything else yet, see protocol.h */

    for (int i = 0; i < g_num_streams; i++) {
        if (start_pipeline(&g_streams[i], client_ip) != 0) {
            fprintf(stderr, "window 0x%lx: could not start streaming, leaving it out\n",
                    (unsigned long)g_streams[i].window);
            /* pipeline/appsrc stay NULL, the event loop below just skips
             * pushing frames for this one, everything else still runs */
        }
    }

    signal(SIGINT, handle_sigint);
    int fd = ConnectionNumber(dpy);
    fprintf(stderr, "streaming %d window%s, ctrl-c to stop\n", g_num_streams, g_num_streams == 1 ? "" : "s");

    while (keep_running) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            if (ev.type == dmg_event_base + XDamageNotify) {
                XDamageNotifyEvent *de = (XDamageNotifyEvent *)&ev;
                /* drawable is which window this damage actually happened
                 * on, that's the whole routing mechanism: match it
                 * against our list and only touch that one shard. */
                for (int i = 0; i < g_num_streams; i++) {
                    if (g_streams[i].window == de->drawable) {
                        XDamageSubtract(dpy, g_streams[i].damage, None, None);
                        if (g_streams[i].pipeline) {
                            push_frame_for_stream(dpy, &g_streams[i]);
                        }
                        break;
                    }
                }
            }
        }

        for (int i = 0; i < g_num_streams; i++) {
            if (g_streams[i].pipeline) check_bus(&g_streams[i]);
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = {1, 0};
        select(fd + 1, &fds, NULL, NULL, &tv);
    }

    fprintf(stderr, "\nshutting down\n");
    for (int i = 0; i < g_num_streams; i++) {
        if (g_streams[i].pipeline) {
            gst_app_src_end_of_stream(GST_APP_SRC(g_streams[i].appsrc));
            gst_element_set_state(g_streams[i].pipeline, GST_STATE_NULL);
            gst_object_unref(g_streams[i].pipeline);
        }
        XDamageDestroy(dpy, g_streams[i].damage);
        XCompositeUnredirectWindow(dpy, g_streams[i].window, CompositeRedirectAutomatic);
    }
    XCloseDisplay(dpy);
    return 0;
}

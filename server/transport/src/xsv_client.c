/*
 * xsv_client.c
 *
 * connects to xsv_server on its control port, reads the handshake to
 * find out where the window belongs on the desktop and which udp port
 * its stream is on, creates an undecorated window at exactly that
 * position and size, and decodes the incoming video straight into it
 * using gstreamer's video overlay support.
 *
 * this is the first real piece of the "see the whole desktop at once"
 * goal: the window this program creates isn't just a video player, it's
 * placed at the same coordinates the source window occupies on the
 * server's desktop. with one window that just looks like a video
 * player in a slightly odd spot. with several, each connecting the same
 * way and each creating its own correctly positioned window, they'd
 * tile together into the actual desktop layout, no different in kind
 * from what one window does here, just more of them.
 *
 * usage: xsv-client <server-host> <control-port>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <X11/Xlib.h>

#include <gst/gst.h>
#include <gst/video/videooverlay.h>

#include "../include/protocol.h"

static volatile sig_atomic_t keep_running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    keep_running = 0;
}

static int connect_and_read_handshake(const char *host, int port, xsv_handshake *out) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        fprintf(stderr, "could not resolve %s\n", host);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0 || connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        perror("connect");
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    char buf[128] = {0};
    ssize_t total = 0;
    /* handshake is one line, read a byte at a time until the newline.
     * wasteful, but this connection carries five integers and then
     * we're done with it, performance was never going to be the
     * concern here. */
    while (total < (ssize_t)sizeof(buf) - 1) {
        ssize_t n = read(fd, buf + total, 1);
        if (n <= 0) break;
        total += n;
        if (buf[total - 1] == '\n') break;
    }
    close(fd);

    if (!xsv_parse_handshake(buf, out)) {
        fprintf(stderr, "bad handshake: %s\n", buf);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <server-host> <control-port>\n", argv[0]);
        return 1;
    }
    const char *server_host = argv[1];
    int control_port = atoi(argv[2]);

    gst_init(&argc, &argv);

    xsv_handshake hs;
    if (connect_and_read_handshake(server_host, control_port, &hs) != 0) {
        return 1;
    }
    fprintf(stderr, "got handshake: window belongs at (%d,%d) size %dx%d, video on udp port %d\n",
            hs.x, hs.y, hs.w, hs.h, hs.port);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "could not open display, is DISPLAY set?\n");
        return 1;
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    XSetWindowAttributes wattrs;
    wattrs.override_redirect = True; /* unmanaged: no wm decorations, no
        wm repositioning, it goes exactly where the handshake says. this
        is what makes several of these tile into a real desktop layout
        instead of every window landing wherever the client's window
        manager feels like putting it. */
    wattrs.background_pixel = BlackPixel(dpy, screen);

    Window win = XCreateWindow(dpy, root, hs.x, hs.y, (unsigned)hs.w, (unsigned)hs.h, 0,
                                CopyFromParent, InputOutput, CopyFromParent,
                                CWOverrideRedirect | CWBackPixel, &wattrs);
    XMapWindow(dpy, win);
    XSync(dpy, False);

    char pipeline_desc[512];
    snprintf(pipeline_desc, sizeof(pipeline_desc),
        "udpsrc port=%d caps=\"application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000\" "
        "! rtph264depay ! h264parse ! avdec_h264 ! videoconvert "
        "! ximagesink name=xsvsink sync=false",
        hs.port);

    fprintf(stderr, "gst pipeline: %s\n", pipeline_desc);

    GError *gerr = NULL;
    GstElement *pipeline = gst_parse_launch(pipeline_desc, &gerr);
    if (!pipeline) {
        fprintf(stderr, "failed to build pipeline: %s\n", gerr ? gerr->message : "unknown error");
        return 1;
    }

    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "xsvsink");
    if (!sink) {
        fprintf(stderr, "could not find sink in pipeline\n");
        return 1;
    }
    /* this is the actual embedding: instead of ximagesink opening its
     * own top-level window, it draws into the one we already created
     * and positioned. */
    gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink), (guintptr)win);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "pipeline failed to start\n");
        return 1;
    }

    signal(SIGINT, handle_sigint);
    fprintf(stderr, "displaying at (%d,%d), ctrl-c to stop\n", hs.x, hs.y);

    while (keep_running) {
        sleep(1);
    }

    fprintf(stderr, "\nshutting down\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}

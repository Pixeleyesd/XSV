/*
 * xsv_server.c
 *
 * captures one window (same mechanism as server/capture/), figures out
 * where it actually sits on the desktop, waits for a client to connect
 * on a control port, tells it that position plus which udp port to
 * expect video on, then streams the window as h264/rtp using a real
 * gstreamer pipeline built and driven from code, not a shell script
 * wrapping gst-launch-1.0.
 *
 * usage: xsv-server <window-id-hex> <control-port> <udp-port>
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
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include "../../capture/include/capture.h"
#include "../include/protocol.h"

static volatile sig_atomic_t keep_running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    keep_running = 0;
}

/* blocks until one client connects on control_port, sends it the
 * handshake for this window, and hands back the socket so the caller
 * can find out the client's address (needed to know where to point the
 * actual video stream). */
static int wait_for_client_and_handshake(int control_port, const xsv_handshake *hs,
                                         char *client_ip_out, size_t client_ip_len) {
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
    fprintf(stderr, "client connected from %s, sending handshake\n", client_ip_out);

    char line[128];
    int n = snprintf(line, sizeof(line), XSV_HANDSHAKE_FMT, hs->x, hs->y, hs->w, hs->h, hs->port);
    if (write(client_fd, line, (size_t)n) != n) {
        perror("write handshake");
        close(client_fd);
        return -1;
    }

    return client_fd;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <window-id-hex> <control-port> <udp-port>\n", argv[0]);
        return 1;
    }
    Window target = (Window)strtoul(argv[1], NULL, 16);
    int control_port = atoi(argv[2]);
    int udp_port = atoi(argv[3]);

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

    XWindowAttributes attrs;
    if (!XGetWindowAttributes(dpy, target, &attrs)) {
        fprintf(stderr, "XGetWindowAttributes failed, bad window id?\n");
        return 1;
    }

    /* attrs.x/attrs.y are relative to the window's parent, which is
     * usually not the root window once a window manager has reparented
     * it for decorations. translating (0,0) in the window's own space
     * into root space is what actually tells us where it sits on the
     * real desktop, which is the whole point, since that's what the
     * client needs in order to place it correctly. */
    Window child_return;
    int root_x = 0, root_y = 0;
    XTranslateCoordinates(dpy, target, DefaultRootWindow(dpy), 0, 0, &root_x, &root_y, &child_return);

    fprintf(stderr, "window 0x%lx is %dx%d at desktop position (%d,%d)\n",
            target, attrs.width, attrs.height, root_x, root_y);

    xsv_handshake hs = { .x = root_x, .y = root_y, .w = attrs.width, .h = attrs.height, .port = udp_port };

    char client_ip[64];
    int client_fd = wait_for_client_and_handshake(control_port, &hs, client_ip, sizeof(client_ip));
    if (client_fd < 0) {
        return 1;
    }
    /* we don't need the control connection for anything else right now,
     * a real version would keep it open for resize/close notifications
     * and eventually multiple windows, but that's not built yet. */
    close(client_fd);

    XCompositeRedirectWindow(dpy, target, CompositeRedirectAutomatic);
    Damage damage = XDamageCreate(dpy, target, XDamageReportBoundingBox);
    if (!damage) {
        fprintf(stderr, "XDamageCreate failed\n");
        return 1;
    }

    char pipeline_desc[512];
    snprintf(pipeline_desc, sizeof(pipeline_desc),
        "appsrc name=xsvsrc is-live=true block=true format=time do-timestamp=true "
        "max-bytes=20971520 "
        "caps=video/x-raw,format=RGB,width=%d,height=%d,framerate=10/1 "
        "! videoconvert ! x264enc tune=zerolatency speed-preset=ultrafast "
        "! rtph264pay config-interval=1 pt=96 ! udpsink host=%s port=%d",
        attrs.width, attrs.height, client_ip, udp_port);

    fprintf(stderr, "gst pipeline: %s\n", pipeline_desc);

    GError *gerr = NULL;
    GstElement *pipeline = gst_parse_launch(pipeline_desc, &gerr);
    if (!pipeline) {
        fprintf(stderr, "failed to build pipeline: %s\n", gerr ? gerr->message : "unknown error");
        return 1;
    }

    GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "xsvsrc");
    if (!appsrc) {
        fprintf(stderr, "could not find appsrc in pipeline\n");
        return 1;
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "pipeline failed to start\n");
        return 1;
    }

    signal(SIGINT, handle_sigint);
    int fd = ConnectionNumber(dpy);
    long damage_count = 0;

    fprintf(stderr, "streaming, ctrl-c to stop\n");

    while (keep_running) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            if (ev.type == dmg_event_base + XDamageNotify) {
                damage_count++;
                XDamageSubtract(dpy, damage, None, None);

                xsv_frame frame;
                if (xsv_capture_window(dpy, target, &frame) == 0) {
                    /* the pipeline's caps were negotiated once at startup
                     * using attrs.width/attrs.height. xsv_capture_window
                     * gets its own dimensions independently, from the
                     * composite pixmap's own geometry. these should always
                     * agree, but if they ever don't, pushing a buffer sized
                     * for one into a pipeline expecting the other silently
                     * corrupts the encoder for the rest of the stream
                     * rather than failing loudly, so it's worth checking
                     * for explicitly instead of assuming it can't happen. */
                    if (frame.width != attrs.width || frame.height != attrs.height) {
                        fprintf(stderr, "frame size mismatch: captured %dx%d but pipeline expects %dx%d, skipping\n",
                                frame.width, frame.height, attrs.width, attrs.height);
                        xsv_free_frame(&frame);
                        continue;
                    }

                    size_t size = (size_t)frame.width * frame.height * 3;
                    GstBuffer *buf = gst_buffer_new_allocate(NULL, size, NULL);
                    GstMapInfo map;
                    gst_buffer_map(buf, &map, GST_MAP_WRITE);
                    memcpy(map.data, frame.rgb, size);
                    gst_buffer_unmap(buf, &map);

                    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buf);
                    if (ret != GST_FLOW_OK) {
                        fprintf(stderr, "push_buffer failed, ret=%d\n", ret);
                    } else {
                        fprintf(stderr, "pushed frame #%ld (%dx%d)\n", damage_count, frame.width, frame.height);
                    }
                    xsv_free_frame(&frame);
                } else {
                    fprintf(stderr, "capture failed on damage #%ld\n", damage_count);
                }
            }
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = {1, 0};
        select(fd + 1, &fds, NULL, NULL, &tv);

        /* we've been completely ignoring the pipeline's own bus this
         * whole time, which meant an encoder or sink erroring out
         * silently would just look like "nothing happening" from over
         * here. check for anything waiting, don't block on it. */
        GstBus *bus = gst_element_get_bus(pipeline);
        GstMessage *msg;
        while ((msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR | GST_MESSAGE_WARNING))) {
            GError *err = NULL;
            gchar *debug = NULL;
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                gst_message_parse_error(msg, &err, &debug);
                fprintf(stderr, "GST ERROR from %s: %s (%s)\n",
                        GST_OBJECT_NAME(msg->src), err->message, debug ? debug : "no debug info");
            } else {
                gst_message_parse_warning(msg, &err, &debug);
                fprintf(stderr, "GST WARNING from %s: %s (%s)\n",
                        GST_OBJECT_NAME(msg->src), err->message, debug ? debug : "no debug info");
            }
            g_error_free(err);
            g_free(debug);
            gst_message_unref(msg);
        }
        gst_object_unref(bus);
    }

    fprintf(stderr, "\nshutting down, sent %ld frames\n", damage_count);
    gst_app_src_end_of_stream(GST_APP_SRC(appsrc));
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    XDamageDestroy(dpy, damage);
    XCompositeUnredirectWindow(dpy, target, CompositeRedirectAutomatic);
    XCloseDisplay(dpy);
    return 0;
}

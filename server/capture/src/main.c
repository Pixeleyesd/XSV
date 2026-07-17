/*
 * xsv-capture-poc — step 0 of the whole project.
 *
 * does exactly one thing: redirect a single window off-screen with
 * XComposite, watch it with XDamage, and print out damage rects as they
 * come in. no network, no encoding, no classification, nothing clever.
 * everything else in ARCHITECTURE.md assumes this primitive actually
 * works, so it gets proven on its own first.
 *
 * usage: xsv-capture-poc <window-id-in-hex>
 * find a window id with `xwininfo` and click the target window.
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/select.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>

/* not shart, shard. every time. we get it. */
static volatile sig_atomic_t keep_running = 1;

static void handle_sigint(int sig) {
    (void)sig;
    keep_running = 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <window-id-in-hex>\n", argv[0]);
        return 1;
    }

    /* redirected to a file when we background this thing, which means
       fully-buffered stdio by default — line-buffer it so the log is
       actually readable while it's still running, not just after exit. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    Window target = (Window)strtoul(argv[1], NULL, 16);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "could not open display, is DISPLAY set?\n");
        return 1;
    }

    /* check the extensions we need actually exist on this server. skip
       past this and everything downstream fails in confusing ways. */
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

    /* AUTOMATIC: the server keeps rendering the window on screen exactly
       as normal AND maintains an off-screen pixmap copy we can read from
       later via XCompositeNameWindowPixmap. MANUAL hands us the repaint
       duties of a full compositing manager instead, which we are not,
       and which turned out to silently stop damage notifications from
       being delivered at all on this x server. lesson learned the hard
       way, so it's written down here instead of just in the git log. */
    XCompositeRedirectWindow(dpy, target, CompositeRedirectAutomatic);

    /* this is the actual "tell me when this window's pixels changed"
       hook. bounding-box reporting is enough for the poc; per-rect deltas
       come later once we're actually diffing content instead of printing. */
    Damage damage = XDamageCreate(dpy, target, XDamageReportBoundingBox);
    if (!damage) {
        fprintf(stderr, "XDamageCreate failed, bad window id?\n");
        return 1;
    }

    XWindowAttributes attrs;
    if (XGetWindowAttributes(dpy, target, &attrs)) {
        printf("watching window 0x%lx (%dx%d) for damage, ctrl-c to stop\n",
               target, attrs.width, attrs.height);
    } else {
        printf("watching window 0x%lx for damage, ctrl-c to stop\n", target);
    }

    signal(SIGINT, handle_sigint);

    int fd = ConnectionNumber(dpy);
    long damage_count = 0;

    while (keep_running) {
        /* drain whatever xlib already has queued FIRST, unconditionally.
           this is not optional: xlib can silently buffer events internally
           as a side effect of an earlier synchronous round-trip (in our
           case, the XGetWindowAttributes call above), and those events
           will never show up as fresh readability on the socket — select()
           has nothing to report because xlib already read the bytes off
           the wire before we ever got here. gating this drain behind
           select()'s return value (the obvious-looking way to write this
           loop) quietly drops exactly those events. ask me how i know. */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            if (ev.type == dmg_event_base + XDamageNotify) {
                XDamageNotifyEvent *de = (XDamageNotifyEvent *)&ev;
                damage_count++;
                printf("damage #%ld: rect (%d,%d %dx%d) more=%d\n",
                       damage_count,
                       de->area.x, de->area.y,
                       de->area.width, de->area.height,
                       de->more);

                /* ack it or the server stops sending new ones for this
                   damage object. XDamageSubtract with None,None just
                   clears the whole accumulated region — fine for a poc
                   that isn't tracking sub-regions yet. */
                XDamageSubtract(dpy, damage, None, None);
            }
        }

        /* now block efficiently until there's *probably* more to do.
           we don't trust or even check the return value — right or
           wrong, we're going straight back to draining XPending at the
           top of the loop regardless, which is what actually matters. */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = {1, 0}; /* 1s so ctrl-c gets noticed promptly */
        select(fd + 1, &fds, NULL, NULL, &tv);
    }

    printf("\ncaught %ld damage events, shutting down cleanly\n", damage_count);
    XDamageDestroy(dpy, damage);
    XCompositeUnredirectWindow(dpy, target, CompositeRedirectAutomatic);
    XCloseDisplay(dpy);
    return 0;
}

/*
 * capture.c, see capture.h for the overview.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include "../include/capture.h"

/* cached once we know whether this display actually supports shm, so we
 * don't re-probe every single frame. -1 means "haven't checked yet". */
static int g_shm_supported = -1;

/* set by our temporary error handler below if XShmAttach fails. some
 * setups (containers, remote X, restrictive shm limits) advertise the
 * extension but then refuse the actual attach, so checking the extension
 * exists isn't enough on its own, we have to try it and see. */
static volatile int g_shm_attach_failed = 0;

static int shm_error_handler(Display *dpy, XErrorEvent *err) {
    (void)dpy;
    (void)err;
    g_shm_attach_failed = 1;
    return 0;
}

/* a mask like 0x0000ff00 tells us both where a color channel lives in a
 * pixel value and how many bits wide it is. this pulls a channel out and
 * scales it to a plain 0-255 byte, regardless of whether the display is
 * using 8 bits per channel (the common case) or something narrower. */
static unsigned char extract_channel(unsigned long pixel, unsigned long mask) {
    if (mask == 0) return 0;

    int shift = 0;
    unsigned long m = mask;
    while ((m & 1) == 0) {
        m >>= 1;
        shift++;
    }

    int bits = 0;
    while (m & 1) {
        bits++;
        m >>= 1;
    }

    unsigned long value = (pixel & mask) >> shift;
    if (bits >= 8) {
        return (unsigned char)(value >> (bits - 8));
    }
    unsigned long max_value = (1UL << bits) - 1;
    return (unsigned char)((value * 255) / max_value);
}

/* walks the XImage pixel by pixel and writes out a plain RGB buffer.
 * XGetPixel isn't fast (it's doing a format check on every call) but for
 * a first working version that's a later problem, not a today problem. */
static unsigned char *image_to_rgb(XImage *img, Visual *visual) {
    unsigned char *rgb = malloc((size_t)img->width * img->height * 3);
    if (!rgb) return NULL;

    for (int y = 0; y < img->height; y++) {
        for (int x = 0; x < img->width; x++) {
            unsigned long pixel = XGetPixel(img, x, y);
            unsigned char *px = &rgb[(y * img->width + x) * 3];
            px[0] = extract_channel(pixel, visual->red_mask);
            px[1] = extract_channel(pixel, visual->green_mask);
            px[2] = extract_channel(pixel, visual->blue_mask);
        }
    }
    return rgb;
}

/* tries to grab the pixmap via shared memory. returns the rgb buffer on
 * success, or NULL if anything along the way didn't work, caller falls
 * back to plain XGetImage in that case. */
static unsigned char *capture_via_shm(Display *dpy, Pixmap pixmap, Visual *visual,
                                      int width, int height, int depth) {
    XShmSegmentInfo shminfo;
    memset(&shminfo, 0, sizeof(shminfo));

    XImage *img = XShmCreateImage(dpy, visual, depth, ZPixmap, NULL, &shminfo, width, height);
    if (!img) return NULL;

    shminfo.shmid = shmget(IPC_PRIVATE, (size_t)img->bytes_per_line * img->height, IPC_CREAT | 0600);
    if (shminfo.shmid < 0) {
        XDestroyImage(img);
        return NULL;
    }

    shminfo.shmaddr = img->data = shmat(shminfo.shmid, NULL, 0);
    if (shminfo.shmaddr == (char *)-1) {
        shmctl(shminfo.shmid, IPC_RMID, NULL);
        XDestroyImage(img);
        return NULL;
    }
    shminfo.readOnly = False;

    /* the segment is marked for removal immediately, it stays valid
     * until every process detaches (us and the X server), so this just
     * means we don't have to remember to clean it up on every exit path,
     * including the ones where something goes wrong halfway through. */
    shmctl(shminfo.shmid, IPC_RMID, NULL);

    g_shm_attach_failed = 0;
    XErrorHandler previous_handler = XSetErrorHandler(shm_error_handler);
    XShmAttach(dpy, &shminfo);
    XSync(dpy, False);
    XSetErrorHandler(previous_handler);

    if (g_shm_attach_failed) {
        shmdt(shminfo.shmaddr);
        XDestroyImage(img);
        return NULL;
    }

    if (!XShmGetImage(dpy, pixmap, img, 0, 0, AllPlanes)) {
        XShmDetach(dpy, &shminfo);
        shmdt(shminfo.shmaddr);
        XDestroyImage(img);
        return NULL;
    }

    unsigned char *rgb = image_to_rgb(img, visual);

    XShmDetach(dpy, &shminfo);
    /* XDestroyImage here only frees the XImage struct itself, not
     * img->data, XShmCreateImage sets that up on purpose since the
     * actual pixel memory belongs to the shm segment, not malloc. */
    XDestroyImage(img);
    shmdt(shminfo.shmaddr);

    return rgb;
}

static unsigned char *capture_via_getimage(Display *dpy, Pixmap pixmap, Visual *visual,
                                           int width, int height) {
    XImage *img = XGetImage(dpy, pixmap, 0, 0, width, height, AllPlanes, ZPixmap);
    if (!img) return NULL;

    unsigned char *rgb = image_to_rgb(img, visual);
    XDestroyImage(img); /* safe here, this image really is malloc'd */
    return rgb;
}

int xsv_capture_window(Display *dpy, Window target, xsv_frame *out) {
    Pixmap pixmap = XCompositeNameWindowPixmap(dpy, target);
    if (!pixmap) return -1;

    Window root_return;
    int x, y;
    unsigned int width, height, border_width, depth;
    if (!XGetGeometry(dpy, pixmap, &root_return, &x, &y, &width, &height, &border_width, &depth)) {
        XFreePixmap(dpy, pixmap);
        return -1;
    }

    XWindowAttributes wattrs;
    if (!XGetWindowAttributes(dpy, target, &wattrs)) {
        XFreePixmap(dpy, pixmap);
        return -1;
    }

    if (g_shm_supported == -1) {
        g_shm_supported = XShmQueryExtension(dpy) ? 1 : 0;
    }

    unsigned char *rgb = NULL;
    if (g_shm_supported) {
        rgb = capture_via_shm(dpy, pixmap, wattrs.visual, (int)width, (int)height, (int)depth);
        if (!rgb) {
            /* don't keep retrying shm every frame once it's proven not
             * to actually work here, just fall back for good. */
            g_shm_supported = 0;
        }
    }
    if (!rgb) {
        rgb = capture_via_getimage(dpy, pixmap, wattrs.visual, (int)width, (int)height);
    }

    XFreePixmap(dpy, pixmap);

    if (!rgb) return -1;

    out->width = (int)width;
    out->height = (int)height;
    out->rgb = rgb;
    return 0;
}

void xsv_free_frame(xsv_frame *frame) {
    if (frame && frame->rgb) {
        free(frame->rgb);
        frame->rgb = NULL;
    }
}

int xsv_save_ppm(const xsv_frame *frame, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", frame->width, frame->height);
    fwrite(frame->rgb, 1, (size_t)frame->width * frame->height * 3, f);
    fclose(f);
    return 0;
}

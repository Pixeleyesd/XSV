/*
 * capture.h
 *
 * grabs the actual pixel contents of a composited window and hands back
 * a plain RGB buffer. this is the piece that was still missing after the
 * damage-notification proof of concept: damage tells you *that* something
 * changed, this is what actually gets you the pixels.
 */

#ifndef XSV_CAPTURE_H
#define XSV_CAPTURE_H

#include <X11/Xlib.h>

typedef struct {
    int width;
    int height;
    unsigned char *rgb; /* width * height * 3 bytes, row-major, top to bottom */
} xsv_frame;

/*
 * grabs the current contents of `target` (which must already be redirected
 * with XCompositeRedirectWindow) into `out`. uses MIT-SHM when the display
 * supports it, since capture runs on the same machine as the X server and
 * shared memory avoids shipping every pixel back through the X protocol.
 * falls back to plain XGetImage if shm isn't available or fails.
 *
 * returns 0 on success, -1 on failure. on success, caller owns out->rgb
 * and must free it with xsv_free_frame().
 */
int xsv_capture_window(Display *dpy, Window target, xsv_frame *out);

void xsv_free_frame(xsv_frame *frame);

/* writes a frame out as a PPM (P6) file. dead simple format, no
 * dependencies, good enough to sanity-check captures by eye. */
int xsv_save_ppm(const xsv_frame *frame, const char *path);

#endif

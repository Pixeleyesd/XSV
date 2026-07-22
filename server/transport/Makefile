# builds the real server and client programs. these link against
# gstreamer directly (unlike gst-sender.sh/gst-receiver.sh, which just
# shell out to gst-launch-1.0), plus the capture code from
# server/capture/.

CC ?= cc
CFLAGS ?= -Wall -Wextra -O2
GST_FLAGS := $(shell pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
X11_LIBS := -lX11 -lXcomposite -lXdamage -lXfixes -lXext

CAPTURE_SRC := ../capture/src/capture.c

all: xsv-server xsv-client

xsv-server: src/xsv_server.c $(CAPTURE_SRC)
	$(CC) $(CFLAGS) -o $@ src/xsv_server.c $(CAPTURE_SRC) $(X11_LIBS) $(GST_FLAGS)

xsv-client: src/xsv_client.c
	$(CC) $(CFLAGS) -o $@ src/xsv_client.c -lX11 $(GST_FLAGS)

clean:
	rm -f xsv-server xsv-client

.PHONY: all clean

#!/bin/sh
# receiving half of gst-sender.sh. right now this just decodes and drops
# frames into jpeg files so the result can be checked by eye, since there's
# no real client display code yet. usage: gst-receiver.sh [port] [out-dir]

set -e

PORT="${1:-5000}"
OUTDIR="${2:-/tmp/xsv-frames}"
mkdir -p "$OUTDIR"

gst-launch-1.0 -e \
    udpsrc port="$PORT" caps="application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000" \
    ! rtph264depay ! h264parse ! avdec_h264 ! videoconvert ! jpegenc \
    ! multifilesink location="$OUTDIR/frame_%05d.jpg"

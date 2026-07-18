#!/bin/sh
# quick and dirty proof that the capture tool's raw stream can actually
# get encoded and sent over the network. not a real transport layer yet,
# just gluing existing gstreamer elements together to prove the mechanism
# before writing any dedicated code, same approach as the rest of this
# project so far.
#
# usage: gst-sender.sh <window-id-hex> <width> <height> <dest-host> [port]
#
# width/height have to match the actual window right now, nothing
# renegotiates on resize yet. find them with xwininfo.

set -e

WINID="$1"
WIDTH="$2"
HEIGHT="$3"
HOST="${4:-127.0.0.1}"
PORT="${5:-5000}"

if [ -z "$WINID" ] || [ -z "$WIDTH" ] || [ -z "$HEIGHT" ]; then
    echo "usage: $0 <window-id-hex> <width> <height> [dest-host] [port]" >&2
    exit 1
fi

CAPTURE_BIN="$(dirname "$0")/../capture/xsv-capture-poc"

"$CAPTURE_BIN" "$WINID" --stream | gst-launch-1.0 -e \
    fdsrc fd=0 \
    ! rawvideoparse format=rgb width="$WIDTH" height="$HEIGHT" framerate=10/1 \
    ! videoconvert \
    ! x264enc tune=zerolatency speed-preset=ultrafast \
    ! rtph264pay config-interval=1 pt=96 \
    ! udpsink host="$HOST" port="$PORT"

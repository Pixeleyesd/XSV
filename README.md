# XSV, X Shard Viewer

a hybrid remote desktop for linux. each window (or region of a window) gets
forwarded using whichever method suits its content: vector drawing
instructions for normal UI, video encoding for 3D/GL content and actual
video playback. everything gets reassembled into one desktop on the client.

[`ARCHITECTURE.md`](ARCHITECTURE.md). this file is just "how do i build and
run what currently exists."

# PLEASE NOTE

This markdown document was partially written with AI. (to save time btw)

yes, I (the human) wrote this part.

## current status

pre-alpha. what actually runs right now:

1. the capture poc watches a real X11 window and reports when and where
   its contents change, using `XComposite`/`XDamage`, then pulls the
   actual pixel data out on every change using MIT-SHM (falling back to
   plain `XGetImage` if shared memory isn't available)
2. that pixel data can be piped into GStreamer to prove the video path:
   real content, encoded as h264, sent over actual RTP/UDP, and decoded
   on the other end. currently glued together with shell scripts calling
   gstreamer's command line tools, not dedicated server/client code

no real server/client programs yet, no instructions path, no
classification, nothing that renegotiates on window resize. see the
"implementation status" section of `ARCHITECTURE.md` for what's next.

## requirements

- linux with an X11 session (tested on ubuntu 24.04)
- a C compiler and `make` (`gcc`/`cc`)
- dev headers: `libx11-dev libxcomposite-dev libxdamage-dev libxfixes-dev libxext-dev`

on debian/ubuntu:

```
sudo apt-get install build-essential libx11-dev libxcomposite-dev libxdamage-dev libxfixes-dev libxext-dev
```

optional, only needed if you want to test without a real desktop session
(e.g. in a headless container): `xvfb xterm x11-utils`.

optional, only needed if you want to actually look at captured frames:
`imagemagick` (to convert the capture poc's PPM output to something more
viewable, like PNG).

optional, only needed for the network streaming proof below:
`gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-ugly`
(the `ugly` set is where the h264 encoder lives).

## building

```
cd server/capture
make
```

produces `./xsv-capture-poc` in that same directory. it isn't installed
anywhere, just run it from there.

## running the capture poc

1. find the window id of something you want to watch:

   ```
   xwininfo
   ```

   your cursor turns into a crosshair. click the window you want. look for
   the `Window id: 0x...` line in the output.

   note: some apps (xterm included) create a decorated top-level window and
   a separate child window that's actually drawn into. if the id `xwininfo`
   gives you directly shows zero damage while you know the window is
   changing, run `xwininfo -tree` on it and try the child window id
   instead.

2. run the poc against that id:

   ```
   ./xsv-capture-poc 0x<the-id-you-found>
   ```

3. interact with the window: type, scroll, play something. each screen
   update prints a line like:

   ```
   damage #4: rect (2,145 438x26) more=0
   ```

   `rect` is the changed region's `(x,y width x height)`, in the window's
   own coordinate space. each damage event also overwrites `latest.ppm` in
   the current directory with the current contents of the window, so you
   can open it and see what's actually there.

4. to actually look at `latest.ppm`, convert it to something your image
   viewer understands:

   ```
   convert latest.ppm latest.png
   ```

5. `ctrl-c` to stop. it prints a final count of how many damage events it
   caught before shutting down cleanly.

## streaming a window over the network (video path proof)

this is the current step 1 proof: real window content, captured, encoded
as h264, sent over actual RTP/UDP, and decoded on the other end. it's
glued together with GStreamer's command line tools rather than dedicated
code, since that's enough to prove the mechanism works before writing a
real server/client program around it.

1. build the capture poc as above, and make sure the gstreamer packages
   listed under "requirements" are installed.
2. find your target window's id and dimensions with `xwininfo` (dimensions
   are fixed for the whole session right now, nothing renegotiates on
   resize yet).
3. on the receiving machine (or just a second terminal, for a loopback
   test), start:

   ```
   server/transport/gst-receiver.sh <port> <output-dir>
   ```

   it decodes incoming frames and drops them in `<output-dir>` as jpegs.
   since there's no real client display code yet, this is just for
   checking by eye that content actually arrived.

4. on the sending machine, start:

   ```
   server/transport/gst-sender.sh <window-id-hex> <width> <height> <dest-host> <port>
   ```

5. interact with the window you pointed it at. jpegs should start showing
   up in the receiver's output directory as you do.

## troubleshooting

- **`no xcomposite/xdamage/xfixes extension on this display`**: your X
  server doesn't have these extensions available. can't proceed on that
  display. unusual for a normal desktop X11 session but can happen on
  minimal/headless X servers depending on how they were built.
- **zero damage events even though the window is visibly changing**: make
  sure nothing else (including a previous, still-running copy of this same
  poc) is holding a `CompositeRedirectManual` redirect on the window. see
  the capture section of `ARCHITECTURE.md` for why that specifically
  causes this.
- **`capture failed on damage #N`**: the pixmap read failed. if this
  happens on every single frame, check that the extensions listed above
  are actually working, not just present, since some restrictive
  containers advertise MIT-SHM but then refuse the actual shared memory
  attach. the code falls back to plain `XGetImage` automatically in that
  case, so this message showing up occasionally isn't a problem, but
  constantly is worth investigating.
- **built fine but `./xsv-capture-poc: command not found`**: you're
  probably not in `server/capture/`. it's not installed system-wide, run
  it as `./xsv-capture-poc`, not `xsv-capture-poc`.
- **the streaming scripts produce no jpegs at all**: check `DISPLAY` is
  actually set and exported in the shell running `gst-sender.sh`, since it
  calls the capture poc internally and will fail silently into an empty
  stream otherwise.

## license

GPLv3, see [`LICENSE`](LICENSE).

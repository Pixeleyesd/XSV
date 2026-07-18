# XSV Architecture

this explains how XSV is designed to work, the mechanism, not just the
decisions behind it. if you just want to build and run what currently
exists, see [`README.md`](README.md) instead.

# PLEASE NOTE

This markdown document was partially written with AI. (to save time btw)

yes, I (the human) wrote this part.

## the idea, in one example

say you're remoting into a linux desktop and you open Firefox to watch a
YouTube video. most of that screen (the tabs, the toolbar, the page around
the video, the scrollbar) is ordinary 2D UI that barely changes from one
frame to the next. the video itself is the opposite: constantly changing,
usually 3D/GPU-composited by the time it hits the screen.

XSV treats these differently, in the same window, at the same time:

- the page chrome gets forwarded as **drawing instructions**, things like
  "draw this rectangle here" or "draw this text here", which the client
  reconstructs natively. cheap, sharp, and only sent when something
  actually changes.
- the video region gets **encoded and streamed as video**, because trying
  to describe a video frame as a list of shapes is pointless. you'd rather
  just send the frame.

which method applies isn't fixed per app or per window. it's decided
continuously, per region, and can flip live as content changes (scroll a
static page and it's instructions, a video starts playing in one corner
and that corner switches to video while the rest of the page stays put).

## the pieces

### server side

**capture layer**, watches actual windows on the real X11 display and
knows two things: which pixels changed, and what those pixels actually
are. built on two X11 extensions plus MIT-SHM for actually reading the
pixel data:

- `XComposite` redirects a window's rendering into an off-screen buffer we
  can read, without taking over responsibility for painting it to the
  screen ourselves (see "known gotchas" below for why that distinction
  matters).
- `XDamage` tells us, as events, exactly which rectangular regions of a
  window changed and when. this is what lets XSV update only when the
  screen actually changes, instead of polling 60 times a second regardless
  of whether anything moved.
- once we know something changed, `XCompositeNameWindowPixmap` gets us a
  handle to the window's current backing pixmap, and MIT-SHM (`XShmGetImage`)
  reads the actual pixel bytes out of it fast, since capture happens on
  the same machine as the X server and there's no reason to ship pixels
  through the X protocol's normal wire format to get them. falls back to
  plain `XGetImage` if shared memory isn't available.

**status: working.** the capture proof-of-concept in `server/capture/`
proves this end to end. it watches a real window, correctly reports damage
rectangles as things change on screen, and pulls the actual pixel content
out on every change (currently just written out as a PPM image file for
verification, nothing is transmitted anywhere yet).

**classification engine** *(not yet built)*, decides, per window and
eventually per sub-region, whether instructions or video is the better fit
right now. the rough logic: a surface backed by direct GL/EGL/Vulkan
rendering defaults to video, a region with damage events arriving faster
than some threshold (continuous motion, not just occasional redraws)
switches to video, and everything else stays on instructions. needs
hysteresis so it doesn't flip back and forth on borderline cases, a brief
scroll shouldn't trigger a codec switch, ten continuous seconds of motion
should.

**encode/serialize layer** *(not yet built)*, turns the classified content
into bytes to send. the instructions path needs a wire format for drawing
primitives (rectangle, text run, image blit, clip, protobuf is a
reasonable starting point). the video path hands the region off to
hardware video encoding (GStreamer with VAAPI on linux) and packetizes the
result.

**asset cache** *(not yet built)*, fonts, glyph atlases, and background
images get sent once, by content hash. if the client already has that hash
cached, the server only sends a reference to it, not the bytes again.

### client side

**instruction renderer** *(not yet built)*, interprets the instruction
stream and draws it natively. planned to be built on **Skia** (the same 2D
graphics library behind Chrome, Android, and Flutter) rather than
hand-rolling a separate native drawing backend per target platform: one
Skia backend per platform instead of five different rendering stacks.

**video decoder/overlay** *(not yet built)*, decodes the incoming video
stream using whatever hardware decode API the platform offers, and
composites the result into the exact screen rect the server says that
region occupies.

**compositor** *(not yet built)*, the thin layer that stacks the
vector-rendered rects and the video rects together into one coherent
screen.

## why build it this way (prior art)

none of this is starting from zero. closest existing projects, in rough
order of relevance:

- **Xpra** already does per-window capture with automatic encoding choice
  between lossless-ish and video codecs, based on window content and
  change frequency. the closest existing analog to XSV's whole "decide per
  window" idea, worth reading before building the classification engine.
- **NX / X2Go (`nx-libs`)** does true X11 protocol-level proxying with
  caching of pixmaps, fonts, and glyphs. the closest existing analog to
  XSV's instructions method and asset cache.
- **Waypipe** is the Wayland equivalent: proxies the Wayland protocol,
  diffs shared-memory buffer damage, and already has a mode that swaps in
  a per-surface video encode for GPU-backed buffers. relevant reading the
  moment XSV gets to Wayland support, including a documented gotcha about
  visible flicker when the encoder switches per-buffer.
- **Moonlight/Sunshine, Parsec** are reference implementations for the
  video half specifically: hardware encode, low-latency RTP-over-UDP,
  adaptive bitrate.

## transport (not yet decided)

the obvious first cut is TCP for instructions (reliable, ordered) and UDP
for video (fast, tolerates loss). two more deliberate options worth
committing to before writing networking code:

- **QUIC**, multiple independent streams over one UDP socket, each
  individually reliable-ordered or using the unreliable datagram extension
  (RFC 9221), built-in TLS 1.3, no head-of-line blocking between channels
  the way two separate transports would have. Xpra already ships QUIC
  support.
- **WebRTC**, data channels for instructions, media tracks for video. buys
  NAT traversal, congestion control, and encryption for free, and already
  has native SDKs on every platform in the platform roadmap below, ios
  included.

## platform roadmap

- **linux/X11**: primary target, in progress.
- **wayland**: no equivalent of X11's protocol-forwarding trick exists.
  follow Waypipe's model (proxy the protocol, diff SHM buffer damage,
  video-encode DMA-BUF surfaces) rather than inventing this fresh.
- **android client**: same shape as the linux client once the client-core
  renderer exists, since it was never running X11 either way.
- **ios client**: same reasoning as android, needs the Skia instruction
  renderer plus platform video decode (VideoToolbox), nothing X11-specific.
- **windows/macos as a server**: a genuinely different problem from X11.
  there's no equivalent "forward the protocol, get vector instructions"
  trick for GDI/DirectX or Core Graphics/Metal. realistic scope there is
  always-video-per-window (the way Parsec/Moonlight/Xpra's own
  windows-shadow mode work), unless a much harder, separate effort using
  platform accessibility trees (UI Automation / macOS Accessibility API)
  gets built to reconstruct semantic UI instead.

## implementation status

**done:**
- X11 damage detection (`server/capture/`), proves a real window's changes
  get reported correctly and promptly.
- pixel capture (`server/capture/src/capture.c`), proves actual pixel data
  can be pulled out of a redirected window, using MIT-SHM with a plain
  `XGetImage` fallback.
- single-window video streaming, linux to linux, proven locally over real
  RTP/UDP. the capture tool's `--stream` mode writes raw RGB24 frames to
  stdout on every damage event, piped straight into a GStreamer pipeline
  (`x264enc` for encoding, `rtph264pay`/`udpsink` for transport) and back
  out the other side (`udpsrc`/`rtph264depay`, `avdec_h264` for decoding).
  this is currently glued together with a `gst-launch-1.0` shell pipeline
  rather than dedicated code, and only tested over loopback with fixed,
  hardcoded dimensions, no proper client display yet, and no real
  server/client program structure. proves the mechanism, not the product.

**known gotchas already hit and fixed, worth knowing about if you're
reading the capture code:**
- redirecting with `CompositeRedirectManual` instead of `Automatic` will
  silently stop damage notifications from arriving at all. manual mode
  hands you full compositing-manager repaint duties, which this tool
  doesn't perform.
- an event loop that gates draining `XPending()` behind `select()`'s
  return value can silently drop events. Xlib can already have events
  buffered internally as a side effect of an unrelated earlier synchronous
  call (e.g. `XGetWindowAttributes`), and those never show up as fresh
  socket readability. always drain `XPending()` unconditionally first, use
  `select()` only to sleep efficiently in between.
- MIT-SHM can advertise support via `XShmQueryExtension` and then still
  fail the actual attach on restrictive setups (containers especially).
  worth checking for that failure explicitly rather than assuming the
  extension being present means it'll work, and falling back cleanly to
  `XGetImage` when it doesn't.

**not yet built, roughly in the order it makes sense to tackle them:**

1. turn the loopback shell-pipeline proof into an actual server program and
   client program, over a real network connection instead of localhost,
   with the window's actual dimensions instead of hardcoded ones, and a
   real display on the receiving end instead of jpeg files.
2. X11 instructions path for one simple window type (e.g. a terminal),
   over TCP, no caching yet. proves the vector round-trip and Skia
   rendering.
3. per-window classification, whole-window granularity only.
4. font/image caching.
5. sub-region splitting (the firefox/youtube case), the hardest step,
   deliberately last, once everything above is solid.
6. android client.
7. wayland server support, via the Waypipe model.
8. ios client, then scope out windows/macos server support properly.

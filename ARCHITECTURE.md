# XSV — X Shard Viewer

a hybrid remote desktop system. each window (or region of a window) is forwarded
using whichever of two methods suits its content best, then reassembled on the
client into a single desktop.

- **instructions method** — drawing primitives forwarded over a reliable channel,
  rendered natively on the client. cheap, sharp, low bandwidth. good for normal
  2D UI (browsers minus video, terminals, editors, file managers).
- **video method** — the region is captured and encoded as video, streamed over
  an unreliable/low-latency channel. good for 3D/GL content and actual video
  playback, where "instructions" don't apply or aren't worth reconstructing.

the two methods run **side by side, per window, and even per-region within a
window** — e.g. a YouTube tab in Firefox: the video element streams as video,
the rest of the browser chrome and page stays on the instructions path.
switching between them happens dynamically as content changes.

only-update-on-damage (not a fixed 60fps poll) applies to the instructions path.
video path runs its own natural frame cadence.

status: design doc, no code yet beyond the capture proof-of-concept in
`server/capture/`.

---

## 1. prior art (read before building anything)

this idea is not starting from zero. closest existing projects, roughly in
order of relevance:

- **Xpra** — already does per-window capture with automatic encoding choice
  (lossless-ish vs. video codec) based on window content and change frequency.
  closest existing analog to the whole "decide per window" idea. supports
  X11/macOS/Windows server side, has an HTML5 client, and already supports
  QUIC as a transport. worth reading its encoding-selection logic before
  designing ours from scratch.
- **NX / X2Go (`nx-libs`)** — true X11 protocol-level proxying with caching of
  pixmaps, fonts, glyphs, and round-trip reduction. this is the closest thing
  to our "instructions" method and to the "cache constants like fonts/bg
  images" requirement — it's already solved there, at the protocol level.
- **Waypipe** — the wayland equivalent of the above. proxies the wayland
  protocol itself, diffs shared-memory buffer damage, and already has a mode
  that swaps a per-surface video encode in for GPU-backed buffers. also
  documents a real gotcha: switching encoders per-buffer causes visible
  flicker as buffers rotate. relevant the moment we start on wayland support.
- **Moonlight/Sunshine, Parsec** — reference implementations for the video
  half specifically: hardware encode, low-latency RTP-over-UDP, adaptive
  bitrate. good study material independent of the windowing problem.

practical read on scope: XSV = Xpra's per-surface hybrid decision engine +
Waypipe's proxying model (for wayland, later) + a from-scratch cross-platform
client renderer aimed specifically at clean sub-region switching (the
YouTube-in-Firefox case) and genuinely broad client support (linux, android,
and later ios/windows/macos).

---

## 2. server side

### 2.1 capture/hook layer (X11 first)

- `XComposite` — redirect each window to an off-screen pixmap so we can read
  its contents independent of what's on screen.
- `XDamage` — per-window (and ideally per-region) damage events. this is the
  mechanism behind "only update when the screen updates."
- `XShape` — non-rectangular windows, lower priority, cosmetic.
- for the instructions path itself: not reinventing IPC, running something
  like an X11 protocol proxy (nx-libs style) between the app and a virtual X
  server, extracting draw primitives instead of just diffing pixels.

### 2.2 classification/decision engine

the actual novel part of this project. per window, and ideally per sub-region:

- GL/EGL/Vulkan direct-rendering context on a surface → video, by default.
- damage-event frequency above a threshold in a sub-rect (e.g. 20+
  updates/sec confined to one region while the rest of the window is static)
  → that region flips to video, everything else stays on instructions. this
  is the YouTube-tab case and needs sub-window granularity, or the whole
  browser chrome gets dragged into video mode along with the tab content.
- hysteresis required — a window mid-scroll for 300ms isn't worth a codec
  switch, ten seconds of continuous motion is. flapping mode every frame
  would look and perform worse than picking either method and sticking with
  it.

### 2.3 encode/serialize

- instructions path: pick a wire format now even if crude — protobuf is
  fine to start. draw-rect, draw-glyph-run(font-ref, string, position),
  blit-image(cache-key), clip, etc.
- video path: GStreamer — VAAPI hardware encode on linux, RTP payloading,
  and it already has the cross-platform decode elements we'll want on the
  client side later.

### 2.4 asset cache

content-addressed: hash fonts/glyph atlases/background images, server sends
the hash first, only pushes bytes on a cache miss. same trick rsync and NX
already use. server keeps a per-session table of what a given client already
has cached.

---

## 3. client side

two renderers running in parallel, composited together into one screen:

- **instruction interpreter/renderer** — needs a drawing backend. don't
  hand-roll platform-native drawing per OS, that's reimplementing text
  shaping and rasterization five times over. **Skia** is the pragmatic
  choice — it already runs on linux, android, ios, windows, and macos (it's
  what chrome/android/flutter use). the "instructions" wire format is
  basically a thin serialization of Skia canvas calls, and each platform's
  client is a thin Skia backend rather than a bespoke renderer.
- **video decode/overlay** — hardware decode per platform (VA-API/VDPAU on
  linux, MediaCodec on android, VideoToolbox on ios/macos, Media Foundation
  on windows), decoded frame composited into the exact screen rect the
  server says that window/region occupies.
- a lightweight client-side compositor that just knows "these rects are
  vector-rendered, these rects are video frames, stack them in z-order."

---

## 4. transport

TCP-for-instructions / UDP-for-video is the obvious first cut, but before
committing to raw sockets:

- **QUIC** is worth taking seriously — multiple independent streams over one
  UDP socket, per-stream choice of reliable-ordered (instructions channel)
  vs. the unreliable datagram extension (RFC 9221, video channel), built-in
  TLS 1.3, much better behavior on flaky/mobile networks than bolting two
  separate transports together (no cross-channel head-of-line blocking).
  Xpra already ships QUIC support, which is a good sign it's viable here.
- **WebRTC** is the other real option — data channels for instructions,
  media tracks for video. buys NAT traversal (ICE/STUN/TURN), congestion
  control, and encryption for free, and has native SDKs on every target
  platform we care about, ios included. matters a lot for the "maybe
  ios/windows/macos later" goal.

decide between these before writing networking code — they imply different
library dependencies and different degrees of "built for us" vs. "off the
shelf."

---

## 5. platform scope notes (read before promising features)

- **wayland**: no core protocol-forwarding concept the way X11 has. follow
  Waypipe's model when we get here (proxy the protocol, diff SHM buffer
  damage, video-encode DMA-BUF surfaces) — don't invent this fresh.
- **windows/macos as a server**: fundamentally different problem from X11.
  no equivalent "forward the protocol, get vector instructions" trick exists
  — GDI/DirectX and Core Graphics/Metal don't expose anything analogous.
  realistic scope: always-video-per-window there (à la Parsec/Moonlight/
  Xpra's own windows-shadow mode), unless we go down a much harder,
  separate R&D track using platform accessibility trees (UI Automation /
  macOS Accessibility API) to reconstruct semantic UI. decide explicitly
  when we get there whether "windows/macos server" means full parity
  (hard) or video-only there (easy, and a reasonable scope line).
- **ios as a client**: comparatively easy in this architecture. it was never
  going to run X11 anyway — just needs the Skia instruction renderer +
  VideoToolbox decode, same shape as any other client.

---

## 6. build order

1. single-window video-only streaming, linux→linux, over UDP/RTP. proves the
   transport and decode path with nothing else in the way.
2. X11 instruction path for one simple window type (e.g. a terminal), over
   TCP, no caching yet. proves the vector round-trip and Skia rendering.
3. per-window classification, whole-window granularity only (no sub-regions
   yet).
4. font/image caching.
5. sub-region splitting (the firefox/youtube case). hardest step, do it
   last, once everything above is solid.
6. android client.
7. wayland server support, via the waypipe model.
8. ios client, then scope out windows/macos server support properly.

current status: step 0 — proving `XComposite`/`XDamage` capture works at all
on a single window, before any of the above. see `server/capture/`.

---

## 7. repo layout

```
xsv/
  LICENSE              GPLv3, verbatim
  ARCHITECTURE.md       this file
  server/
    capture/            XComposite/XDamage capture layer (X11 first)
    classify/           per-window/per-region instructions-vs-video decision engine
    encode/             instruction serializer + gstreamer video encode pipeline
    transport/          wire protocol, QUIC or WebRTC transport layer
  client-core/          shared skia-based instruction renderer + video overlay compositor
  clients/
    linux/              linux client shell
    android/             android client shell
  docs/                 anything longer-form than fits in this file
```

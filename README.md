# XSV — X Shard Viewer

a hybrid remote desktop: each window (or region of a window) gets forwarded
using whichever method suits its content — vector drawing instructions for
normal UI, video encoding for 3D/GL content and actual video playback — then
reassembled into one desktop on the client. see [`ARCHITECTURE.md`](ARCHITECTURE.md)
for the full design and reasoning.

x11-first on linux, wayland and other platforms later. GPLv3.

## status

pre-alpha, design phase. the only thing that exists right now is a proof of
concept for the single riskiest primitive the whole project depends on:
capturing per-window damage events on X11 via `XComposite`/`XDamage`.

## building the capture poc

```
cd server/capture
make
```

needs `libx11-dev libxcomposite-dev libxdamage-dev libxfixes-dev libxext-dev`
(debian/ubuntu package names).

```
./xsv-capture-poc <window-id-in-hex>
```

find a target window id with `xwininfo`, click the window you want to watch,
then ctrl-c the poc to see a summary of damage events it caught.

## license

GPLv3, see [`LICENSE`](LICENSE).

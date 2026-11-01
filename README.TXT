================
= fbDOOM =
================

Adaptation of the original DOOM
to be easily portable to framebuffer devices with minimal depenencies.
Runs on:
* Plain desktop Linux framebuffer,
* Siglent SSA 3021X Spectrum Analyzer,
* Frosted OS,
* Kinco Green Series GL070HE HMI,
* Kinco MT4434TE HMI

To build (example):
* cd fbdoom
* make CROSS_COMPILE=arm-linux-gnueabihf-

Linux framebuffer backend
=========================

The fbdoom build uses direct Linux framebuffer/evdev or native X11
software video/input, plus ALSA for sound effects. Neither backend
requires SDL or OpenGL. Music and MIDI are not implemented.

Build on a Linux system with ALSA development headers and library installed:

* cd fbdoom
* make

For the X11 desktop test backend, install the Xlib development files and run:

* cd fbdoom
* make BACKEND=x11
* ./fbdoom-x11 -scale 3

Optional build features (fbdev backend). The Makefile probes the selected
compiler, so cross builds work, and the defaults build on any CPU target:

* `PXP=auto|1|0`: i.MX23 PXP video path (`-pxp`). Enabled automatically when
  `<linux/videodev2.h>` is available and the target is little-endian. The
  hardware itself is only looked for at run time.
* `CPU_FLAGS=...`: extra CPU tuning flags, none by default. For i.MX23 use
  `make CPU_FLAGS=-mcpu=arm926ej-s`.
* `make config` prints the detected settings and the resulting `CFLAGS`.

Pass `-flashlight` to add a player-centered dynamic light. Nearby walls,
planes, and sprites receive a stronger existing palette colormap; the
effect falls off with distance. It is opt-in and uses the software
renderer, with no PXP dependency.

`-brightmaps` keeps selected bright pixels in computer and light wall
textures unshaded. `-translucent` draws spectre/invisibility shadows as
50% palette translucency instead of the classic fuzz effect. Both options
are disabled by default; translucency builds its palette blend table once
at startup.

The renderer remains at 320x200; presentation maps it to a corrected 4:3
display image (320x240) using nearest-neighbor sampling. The default fits
that image to the output while preserving aspect ratio and centering bars.
For example, 480x272 uses a centered 363x272 image; 320x240 and its
multiples fill the output. Add `-stretch` to fill the output without
preserving aspect ratio.

Use `-fullscreen` to request fullscreen from the X11 window manager;
`-window` overrides that setting. Fullscreen uses the window manager's
actual output dimensions and recalculates the fit after resize, independent
of windowed scale. Windowed `-scale` accepts integer sizes from 1 to 8,
where 1 is 320x240. The X11 and fbdev targets are separate executables
(`fbdoom-x11` and `fbdoom-fbdev`), so they can be built without reusing
each other's objects or overwriting binaries.

Use `make BACKEND=fbdev` (the default) for direct framebuffer/evdev. This
fills the active framebuffer using the same aspect-preserving fit; pass
`-stretch` to fill non-4:3 displays or `-scaling N` to limit the image to
an N-times 320x240 region.

In fbdev mode, the executable needs read access to a keyboard device under
`/dev/input/event*` and read/write access to `/dev/fb0`; mouse input is
optional. Standard keyboards and limited keypads such as ADC keys are
accepted when they expose at least one key Doom recognizes. Add the user to
the system's input/video groups or configure suitable device permissions.
X11 mode uses the active X server and needs no direct evdev device access.
If ALSA's default playback device is not the desired output, set
`ALSA_PCM_DEVICE` to an ALSA PCM name before starting the game. Pass
`-nosound` to disable sound initialization entirely, including opening the
ALSA playback device; this is useful on systems without audio hardware.

The game renders internally at 320x200; the selected viewport scales that
image without changing the renderer's native resolution.

i.MX23 PXP acceleration (fbdev backend only)
--------------------------------------------

`-pxp` offloads scaling and colour conversion to the i.MX23 PXP through its
V4L2 mem2mem device: the indexed frame is converted to planar YUV 4:2:0 with
a palette lookup table, and the PXP scales it to RGB straight into the
framebuffer. `-pxpdev /dev/videoN` selects the device explicitly; otherwise
the `imx23-pxp` driver is looked up automatically. If anything is missing,
fbDOOM prints a message and falls back to software scaling.

Requirements: a 32bpp (XRGB8888) or 16bpp (RGB565) framebuffer whose width is
a multiple of 8 and not padded. The stock imx23-pxp driver is used unchanged:
the PXP renders into its own V4L2 buffer and one memcpy copies the finished
frame into the framebuffer. For flicker-free page flipping the framebuffer
needs a virtual height of at least twice the visible one (with DRM fbdev
emulation: `CONFIG_DRM_FBDEV_OVERALLOC=200`); otherwise a single page is used
and tearing may occur. Aspect ratio is kept by letterboxing, or by
pillarboxing inside the source frame for screens wider than 4:3.

How the PXP path works
......................

The software scaler (`I_FinishUpdate` in `i_video_fbdev.c`) maps, looks up and
stores every one of the ~130k output pixels on the CPU, clears the whole frame
and then `write()`s it to the framebuffer. On a 454 MHz ARM926 without an FPU
that dominates the frame time. With `-pxp` (`i_video_pxp.c`) the work is split:

1. The CPU converts the 320x200 8-bit frame to planar YUV 4:2:0 (full-range
   BT.601) using a 256-entry palette lookup table that is rebuilt whenever the
   palette changes. Y is written four pixels per 32-bit store; U and V are
   averaged over horizontal pairs on even rows only. That is ~64k lookups per
   frame instead of ~130k scaled pixels.
2. The PXP, through the V4L2 mem2mem device, scales the YUV frame to the
   framebuffer size and converts it to RGB565 or XRGB8888 (chosen from the
   framebuffer layout) into its own capture buffer.
3. The CPU copies that buffer into the off-screen framebuffer page with a
   single `memcpy`, waits for vsync and flips with `FBIOPAN_DISPLAY`.

Frames are pipelined: the PXP converts frame N while the CPU renders frame
N+1, and the CPU only waits for the result when it needs it. Two source
buffers alternate so the CPU never writes to a buffer the PXP is reading.

On screens wider than 4:3 (e.g. 480x272) the source frame is widened with
black margins (e.g. 424x200 for 480x272) so the PXP's scaling keeps the aspect
ratio. PXP limits that shape the code: sizes are multiples of 8, only YUV
sources can be scaled, there is no stride (the output rectangle is the full
framebuffer width), and downscaling is limited to 2x.

Writing the PXP output directly into the framebuffer via V4L2 USERPTR does not
work on Linux 6.6 with DRM fbdev emulation (QBUF fails with EFAULT), which is
why the extra `memcpy` is used. No change to the PXP driver is required.

To measure the gain, run the same `-timedemo demo1` with and without `-pxp`
on an otherwise idle system.

On-screen FPS counter
---------------------

`-showfps` draws the number of presented frames per second (updated once per
second) in the top-left corner of the game frame, for the fbdev and X11
backends and with or without `-pxp`. It is drawn into the 320x200 frame, so it
is scaled with the picture.


Original readme
===============

Here it is, at long last.  The DOOM source code is released for your
non-profit use.  You still need real DOOM data to work with this code.
If you don't actually own a real copy of one of the DOOMs, you should
still be able to find them at software stores.

Many thanks to Bernd Kreimeier for taking the time to clean up the
project and make sure that it actually works.  Projects tends to rot if
you leave it alone for a few years, and it takes effort for someone to
deal with it again.

The bad news:  this code only compiles and runs on linux.  We couldn't
release the dos code because of a copyrighted sound library we used
(wow, was that a mistake -- I write my own sound code now), and I
honestly don't even know what happened to the port that microsoft did
to windows.

Still, the code is quite portable, and it should be straightforward to
bring it up on just about any platform.

I wrote this code a long, long time ago, and there are plenty of things
that seem downright silly in retrospect (using polar coordinates for
clipping comes to mind), but overall it should still be a usefull base
to experiment and build on.

The basic rendering concept -- horizontal and vertical lines of constant
Z with fixed light shading per band was dead-on, but the implementation
could be improved dramatically from the original code if it were
revisited.  The way the rendering proceded from walls to floors to
sprites could be collapsed into a single front-to-back walk of the bsp
tree to collect information, then draw all the contents of a subsector
on the way back up the tree.  It requires treating floors and ceilings
as polygons, rather than just the gaps between walls, and it requires
clipping sprite billboards into subsector fragments, but it would be
The Right Thing.

The movement and line of sight checking against the lines is one of the
bigger misses that I look back on.  It is messy code that had some
failure cases, and there was a vastly simpler (and faster) solution
sitting in front of my face.  I used the BSP tree for rendering things,
but I didn't realize at the time that it could also be used for
environment testing.  Replacing the line of sight test with a bsp line
clip would be pretty easy.  Sweeping volumes for movement gets a bit
tougher, and touches on many of the challenges faced in quake / quake2
with edge bevels on polyhedrons.

Some project ideas:

Port it to your favorite operating system.

Add some rendering features -- transparency, look up / down, slopes,
etc.

Add some game features -- weapons, jumping, ducking, flying, etc.

Create a packet server based internet game.

Create a client / server based internet game.

Do a 3D accelerated version.  On modern hardware (fast pentium + 3DFX)
you probably wouldn't even need to be clever -- you could just draw the
entire level and get reasonable speed.  With a touch of effort, it should
easily lock at 60 fps (well, there are some issues with DOOM's 35 hz
timebase...).  The biggest issues would probably be the non-power of two
texture sizes and the walls composed of multiple textures.


I don't have a real good guess at how many people are going to be
playing with this, but if significant projects are undertaken, it would
be cool to see a level of community cooperation.  I know that most early
projects are going to be rough hacks done in isolation, but I would be
very pleased to see a coordinated 'net release of an improved, backwards
compatable version of DOOM on multiple platforms next year.

Have fun.

John Carmack
12-23-97

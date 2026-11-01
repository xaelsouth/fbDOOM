//
// Optional i.MX23 PXP presentation path for the fbdev backend.
//
// The 8-bit frame is converted to planar YUV 4:2:0 with a palette lookup
// table, then the PXP scales it and converts it to RGB directly into the
// (mmap()ed) framebuffer through the V4L2 mem2mem interface.
//

#ifndef I_VIDEO_PXP_H
#define I_VIDEO_PXP_H

#include <linux/fb.h>

#include "doomtype.h"

#ifndef HAVE_PXP
// Built without PXP support (see the Makefile): no-op stubs so callers need
// no #ifdefs. I_PXP_Init() always fails, which selects software scaling.
static inline void I_PXP_SetPalette(const byte *rgb) { (void) rgb; }
static inline boolean I_PXP_Init(const char *device, int fd_fb,
                                 const struct fb_var_screeninfo *var,
                                 const struct fb_fix_screeninfo *fix,
                                 int viewport_width, int viewport_y,
                                 int viewport_height)
{
    (void) device; (void) fd_fb; (void) var; (void) fix;
    (void) viewport_width; (void) viewport_y; (void) viewport_height;
    return false;
}
static inline void I_PXP_Present(const byte *indexed) { (void) indexed; }
static inline void I_PXP_Shutdown(void) { }
#else
// Updates the palette lookup table. rgb holds 256 gamma-corrected R,G,B
// triplets. Can be called before I_PXP_Init().
void I_PXP_SetPalette(const byte *rgb);

// Converts a SCREENWIDTH x SCREENHEIGHT indexed frame into a planar YUV 4:2:0
// buffer of canvas_width x SCREENHEIGHT; the frame is centered horizontally.
// canvas_width must be a multiple of 8 and >= SCREENWIDTH. The margins are
// not touched.
void I_PXP_ConvertFrame(const byte *indexed, byte *yuv, int canvas_width);

// device may be NULL to look for the imx23-pxp V4L2 device. The viewport
// width/y/height describe the aspect-corrected image inside the framebuffer.
// Returns false (with a message on stderr) if the PXP path cannot be used.
boolean I_PXP_Init(const char *device, int fd_fb,
                   const struct fb_var_screeninfo *var,
                   const struct fb_fix_screeninfo *fix,
                   int viewport_width, int viewport_y, int viewport_height);

// Converts and queues a frame; presents the previously queued one.
void I_PXP_Present(const byte *indexed);

void I_PXP_Shutdown(void);

#endif /* HAVE_PXP */

#endif

// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// $Log:$
//
// DESCRIPTION:
//	DOOM graphics stuff for X11, UNIX.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: i_x.c,v 1.6 1997/02/03 22:45:10 b1 Exp $";

#include "config.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_event.h"
#include "d_main.h"
#include "i_system.h"
#include "i_video.h"
#include "i_video_scaling.h"
#include "z_zone.h"

#include "tables.h"
#include "doomkeys.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <stdarg.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <stdint.h>

extern void I_ShutdownInput(void);

struct fb_var_screeninfo fb = {};
static struct fb_fix_screeninfo fb_fix;
static video_viewport_t viewport;
static boolean stretch_to_fill;
static unsigned int bytes_per_pixel;
static size_t output_buffer_size;
int usemouse = 0;

struct color {
    uint32_t b:8;
    uint32_t g:8;
    uint32_t r:8;
    uint32_t a:8;
};

static struct color colors[256];

// The screen buffer; this is modified to draw things to the screen

byte *I_VideoBuffer = NULL;
byte *I_VideoBuffer_FB = NULL;

/* framebuffer file descriptor */
int fd_fb = 0;

int	X_width;
int X_height;

// If true, game is running as a screensaver

boolean screensaver_mode = false;

// Flag indicating whether the screen is currently visible:
// when the screen isnt visible, don't render the screen

boolean screenvisible;

// Mouse acceleration
//
// This emulates some of the behavior of DOS mouse drivers by increasing
// the speed when the mouse is moved fast.
//
// The mouse input values are input directly to the game, but when
// the values exceed the value of mouse_threshold, they are multiplied
// by mouse_acceleration to increase the speed.

float mouse_acceleration = 2.0;
int mouse_threshold = 10;

// Gamma correction level to use

int usegamma = 0;

static uint32_t FramebufferPixel(struct color color)
{
    uint32_t pixel = 0;

    if (fb.red.length > 0 && fb.red.length <= 8)
    {
        pixel |= (uint32_t) (color.r >> (8 - fb.red.length))
               << fb.red.offset;
    }
    if (fb.green.length > 0 && fb.green.length <= 8)
    {
        pixel |= (uint32_t) (color.g >> (8 - fb.green.length))
               << fb.green.offset;
    }
    if (fb.blue.length > 0 && fb.blue.length <= 8)
    {
        pixel |= (uint32_t) (color.b >> (8 - fb.blue.length))
               << fb.blue.offset;
    }

    return pixel;
}

static void StorePixel(unsigned char *destination, uint32_t pixel)
{
    unsigned int byte;

    for (byte = 0; byte < bytes_per_pixel; ++byte)
    {
        destination[byte] = pixel >> (byte * 8);
    }
}

void I_InitGraphics (void)
{
    int scale_arg;
    int area_width = 0;
    int area_height = 0;
    int area_x = 0;
    int area_y = 0;
    int requested_scale = 0;

    /* Open fbdev file descriptor */
    fd_fb = open("/dev/fb0", O_RDWR);
    if (fd_fb < 0)
    {
        printf("Could not open /dev/fb0");
        exit(-1);
    }

    /* fetch framebuffer info */
    if (ioctl(fd_fb, FBIOGET_VSCREENINFO, &fb) < 0
     || ioctl(fd_fb, FBIOGET_FSCREENINFO, &fb_fix) < 0)
    {
        I_Error("Cannot query framebuffer information: %s", strerror(errno));
        return;
    }
    /* change params if needed */
    //ioctl(fd_fb, FBIOPUT_VSCREENINFO, &fb);
    printf("I_InitGraphics: framebuffer: x_res: %d, y_res: %d, x_virtual: %d, y_virtual: %d, bpp: %d, grayscale: %d\n",
            fb.xres, fb.yres, fb.xres_virtual, fb.yres_virtual, fb.bits_per_pixel, fb.grayscale);

    printf("I_InitGraphics: framebuffer: RGBA: %d%d%d%d, red_off: %d, green_off: %d, blue_off: %d, transp_off: %d\n",
            fb.red.length, fb.green.length, fb.blue.length, fb.transp.length, fb.red.offset, fb.green.offset, fb.blue.offset, fb.transp.offset);

    printf("I_InitGraphics: DOOM screen size: w x h: %d x %d\n", SCREENWIDTH, SCREENHEIGHT);


    if ((fb.bits_per_pixel != 16 && fb.bits_per_pixel != 24
      && fb.bits_per_pixel != 32)
     || fb_fix.line_length < fb.xres * (fb.bits_per_pixel / 8))
    {
        I_Error("Unsupported framebuffer format: %u bpp, line length %u",
                fb.bits_per_pixel, fb_fix.line_length);
        return;
    }

    bytes_per_pixel = fb.bits_per_pixel / 8;
    stretch_to_fill = M_CheckParm("-stretch") > 0;
    scale_arg = M_CheckParmWithArgs("-scaling", 1);
    if (scale_arg > 0)
    {
        requested_scale = atoi(myargv[scale_arg + 1]);
        if (requested_scale <= 0)
        {
            I_Error("-scaling must be a positive integer");
            return;
        }

        area_width = SCREENWIDTH * requested_scale;
        area_height = (SCREENHEIGHT * 6 / 5) * requested_scale;
        if (area_width > (int) fb.xres)
        {
            area_width = fb.xres;
        }
        if (area_height > (int) fb.yres)
        {
            area_height = fb.yres;
        }
        area_x = (fb.xres - area_width) / 2;
        area_y = (fb.yres - area_height) / 2;
    }
    else
    {
        area_width = fb.xres;
        area_height = fb.yres;
    }

    if (!I_CalculateVideoViewport(area_width, area_height, stretch_to_fill,
                                  &viewport))
    {
        I_Error("Cannot calculate framebuffer output scaling");
        return;
    }
    viewport.x += area_x;
    viewport.y += area_y;

    /* Allocate screen to draw to */
	I_VideoBuffer = (byte*)Z_Malloc (SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);
	output_buffer_size = (size_t) fb.xres * fb.yres * bytes_per_pixel;
	I_VideoBuffer_FB = calloc(1, output_buffer_size);
	if (I_VideoBuffer_FB == NULL)
	{
	    I_Error("Cannot allocate framebuffer output buffer");
	    return;
	}

	screenvisible = true;

    extern int I_InitInput(void);
    I_InitInput();
}

void I_ShutdownGraphics (void)
{
	I_ShutdownInput();
	Z_Free (I_VideoBuffer);
	free (I_VideoBuffer_FB);
	if (fd_fb >= 0)
	{
	    close(fd_fb);
	    fd_fb = -1;
	}
}

void I_StartFrame (void)
{

}

__attribute__ ((weak)) void I_GetEvent (void)
{
//	event_t event;
//	bool button_state;
//
//	button_state = button_read ();
//
//	if (last_button_state != button_state)
//	{
//		last_button_state = button_state;
//
//		event.type = last_button_state ? ev_keydown : ev_keyup;
//		event.data1 = KEY_FIRE;
//		event.data2 = -1;
//		event.data3 = -1;
//
//		D_PostEvent (&event);
//	}
//
//	touch_main ();
//
//	if ((touch_state.x != last_touch_state.x) || (touch_state.y != last_touch_state.y) || (touch_state.status != last_touch_state.status))
//	{
//		last_touch_state = touch_state;
//
//		event.type = (touch_state.status == TOUCH_PRESSED) ? ev_keydown : ev_keyup;
//		event.data1 = -1;
//		event.data2 = -1;
//		event.data3 = -1;
//
//		if ((touch_state.x > 49)
//		 && (touch_state.x < 72)
//		 && (touch_state.y > 104)
//		 && (touch_state.y < 143))
//		{
//			// select weapon
//			if (touch_state.x < 60)
//			{
//				// lower row (5-7)
//				if (touch_state.y < 119)
//				{
//					event.data1 = '5';
//				}
//				else if (touch_state.y < 131)
//				{
//					event.data1 = '6';
//				}
//				else
//				{
//					event.data1 = '1';
//				}
//			}
//			else
//			{
//				// upper row (2-4)
//				if (touch_state.y < 119)
//				{
//					event.data1 = '2';
//				}
//				else if (touch_state.y < 131)
//				{
//					event.data1 = '3';
//				}
//				else
//				{
//					event.data1 = '4';
//				}
//			}
//		}
//		else if (touch_state.x < 40)
//		{
//			// button bar at bottom screen
//			if (touch_state.y < 40)
//			{
//				// enter
//				event.data1 = KEY_ENTER;
//			}
//			else if (touch_state.y < 80)
//			{
//				// escape
//				event.data1 = KEY_ESCAPE;
//			}
//			else if (touch_state.y < 120)
//			{
//				// use
//				event.data1 = KEY_USE;
//			}
//			else if (touch_state.y < 160)
//			{
//				// map
//				event.data1 = KEY_TAB;
//			}
//			else if (touch_state.y < 200)
//			{
//				// pause
//				event.data1 = KEY_PAUSE;
//			}
//			else if (touch_state.y < 240)
//			{
//				// toggle run
//				if (touch_state.status == TOUCH_PRESSED)
//				{
//					run = !run;
//
//					event.data1 = KEY_RSHIFT;
//
//					if (run)
//					{
//						event.type = ev_keydown;
//					}
//					else
//					{
//						event.type = ev_keyup;
//					}
//				}
//				else
//				{
//					return;
//				}
//			}
//			else if (touch_state.y < 280)
//			{
//				// save
//				event.data1 = KEY_F2;
//			}
//			else if (touch_state.y < 320)
//			{
//				// load
//				event.data1 = KEY_F3;
//			}
//		}
//		else
//		{
//			// movement/menu navigation
//			if (touch_state.x < 100)
//			{
//				if (touch_state.y < 100)
//				{
//					event.data1 = KEY_STRAFE_L;
//				}
//				else if (touch_state.y < 220)
//				{
//					event.data1 = KEY_DOWNARROW;
//				}
//				else
//				{
//					event.data1 = KEY_STRAFE_R;
//				}
//			}
//			else if (touch_state.x < 180)
//			{
//				if (touch_state.y < 160)
//				{
//					event.data1 = KEY_LEFTARROW;
//				}
//				else
//				{
//					event.data1 = KEY_RIGHTARROW;
//				}
//			}
//			else
//			{
//				event.data1 = KEY_UPARROW;
//			}
//		}
//
//		D_PostEvent (&event);
//	}
}

__attribute__ ((weak)) void I_StartTic (void)
{
	I_GetEvent();
}

void I_UpdateNoBlit (void)
{
}

//
// I_FinishUpdate
//

void I_FinishUpdate (void)
{
    int y;
    int output_bytes = fb.xres * bytes_per_pixel;

    memset(I_VideoBuffer_FB, 0, output_buffer_size);

    for (y = 0; y < viewport.height; ++y)
    {
        int source_y = I_MapOutputToSource(y, viewport.height, SCREENHEIGHT);
        unsigned char *output_line =
            I_VideoBuffer_FB + (viewport.y + y) * output_bytes;
        int x;

        for (x = 0; x < viewport.width; ++x)
        {
            int source_x = I_MapOutputToSource(x, viewport.width, SCREENWIDTH);
            unsigned int destination_x = viewport.x + x;
            size_t source_offset =
                (size_t) source_y * SCREENWIDTH + source_x;

            StorePixel(output_line + destination_x * bytes_per_pixel,
                       FramebufferPixel(colors[I_VideoBuffer[source_offset]]));
        }
    }

    if (fb_fix.line_length == (unsigned int) output_bytes
     && fb.xoffset == 0
     && fb.yoffset == 0)
    {
        size_t bytes_to_write = (size_t) output_bytes * fb.yres;
        size_t written = 0;

        if (lseek(fd_fb, 0, SEEK_SET) < 0)
        {
            I_Error("Cannot seek framebuffer: %s", strerror(errno));
            return;
        }
        while (written < bytes_to_write)
        {
            ssize_t count = write(fd_fb, I_VideoBuffer_FB + written,
                                  bytes_to_write - written);

            if (count < 0 && errno == EINTR)
            {
                continue;
            }
            if (count <= 0)
            {
                I_Error("Cannot write framebuffer: %s",
                        count == 0 ? "short write" : strerror(errno));
                return;
            }
            written += (size_t) count;
        }
    }
    else
    {
        for (y = 0; y < (int) fb.yres; ++y)
        {
            size_t written = 0;
            off_t offset = (off_t) (y + fb.yoffset) * fb_fix.line_length
                         + (off_t) fb.xoffset * bytes_per_pixel;

            if (lseek(fd_fb, offset, SEEK_SET) < 0)
            {
                I_Error("Cannot seek framebuffer: %s", strerror(errno));
                return;
            }
            while (written < (size_t) output_bytes)
            {
                ssize_t count = write(fd_fb,
                                      I_VideoBuffer_FB
                                      + (size_t) y * output_bytes + written,
                                      output_bytes - written);

                if (count < 0 && errno == EINTR)
                {
                    continue;
                }
                if (count <= 0)
                {
                    I_Error("Cannot write framebuffer: %s",
                            count == 0 ? "short write" : strerror(errno));
                    return;
                }
                written += (size_t) count;
            }
        }
    }
}

//
// I_ReadScreen
//
void I_ReadScreen (byte* scr)
{
    memcpy (scr, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
}

//
// I_SetPalette
//
#define GFX_RGB565(r, g, b)			((((r & 0xF8) >> 3) << 11) | (((g & 0xFC) >> 2) << 5) | ((b & 0xF8) >> 3))
#define GFX_RGB565_R(color)			((0xF800 & color) >> 11)
#define GFX_RGB565_G(color)			((0x07E0 & color) >> 5)
#define GFX_RGB565_B(color)			(0x001F & color)

void I_SetPalette (byte* palette)
{
	int i;
	//col_t* c;

	//for (i = 0; i < 256; i++)
	//{
	//	c = (col_t*)palette;

	//	rgb565_palette[i] = GFX_RGB565(gammatable[usegamma][c->r],
	//								   gammatable[usegamma][c->g],
	//								   gammatable[usegamma][c->b]);

	//	palette += 3;
	//}
    

    /* performance boost:
     * map to the right pixel format over here! */

    for (i=0; i<256; ++i ) {
        colors[i].a = 0;
        colors[i].r = gammatable[usegamma][*palette++];
        colors[i].g = gammatable[usegamma][*palette++];
        colors[i].b = gammatable[usegamma][*palette++];
    }

    /* Set new color map in kernel framebuffer driver */
    //XXX FIXME ioctl(fd_fb, IOCTL_FB_PUTCMAP, colors);
}

// Given an RGB value, find the closest matching palette index.

int I_GetPaletteIndex (int r, int g, int b)
{
    int best, best_diff, diff;
    int i;

    best = 0;
    best_diff = INT_MAX;

    for (i = 0; i < 256; ++i)
    {
        diff = (r - colors[i].r) * (r - colors[i].r)
             + (g - colors[i].g) * (g - colors[i].g)
             + (b - colors[i].b) * (b - colors[i].b);

        if (diff < best_diff)
        {
            best = i;
            best_diff = diff;
        }

        if (diff == 0)
        {
            break;
        }
    }

    return best;
}

void I_BeginRead (void)
{
}

void I_EndRead (void)
{
}

void I_SetWindowTitle (char *title)
{
}

void I_GraphicsCheckCommandLine (void)
{
}

void I_SetGrabMouseCallback (grabmouse_callback_t func)
{
}

void I_EnableLoadingDisk(void)
{
}

void I_BindVideoVariables (void)
{
}

void I_DisplayFPSDots (boolean dots_on)
{
}

void I_CheckIsScreensaver (void)
{
}

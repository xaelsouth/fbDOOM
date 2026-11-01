//
// i.MX23 PXP presentation path (see i_video_pxp.h).
//

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <time.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/fb.h>

#include "i_video.h"
#include "i_video_pxp.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "i_video_pxp.c requires a little-endian target"
#endif

#ifndef V4L2_PIX_FMT_XBGR32
#define V4L2_PIX_FMT_XBGR32 v4l2_fourcc('X', 'R', '2', '4')
#endif

#define NUM_SRC 2
#define MAX_PAGES 2

// Packed Y | U << 8 | V << 16, full-range BT.601
static uint32_t palette_yuv[256];

static int fd_v4l = -1;
static int fd_fbdev = -1;
static struct fb_var_screeninfo fbvar;
static uint8_t *fb_base;
static size_t fb_size;
static int pages;
static int shown;
static int pending;
static int cur;
static int canvas_w;
static size_t src_len;
static uint8_t *src_buf[NUM_SRC];
static size_t cap_offset;   // byte offset of the image inside a page
static size_t cap_len;
static uint8_t *cap_buf;    // PXP output, copied into the framebuffer
static size_t page_bytes;
static int vsync_ok = 1;
static double vsync_period;     // measured refresh period in seconds
static double pan_time;         // when the last pan was issued

static double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void I_PXP_SetPalette(const byte *rgb)
{
    int i;

    for (i = 0; i < 256; i++)
    {
        int r = rgb[i * 3], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        int y = (19595 * r + 38470 * g + 7471 * b + 32768) >> 16;
        int u = ((-11059 * r - 21709 * g + 32768 * b + 32768) >> 16) + 128;
        int v = ((32768 * r - 27439 * g - 5329 * b + 32768) >> 16) + 128;

        palette_yuv[i] = (uint32_t) clampi(y, 0, 255)
                       | (uint32_t) clampi(u, 0, 255) << 8
                       | (uint32_t) clampi(v, 0, 255) << 16;
    }
}

void I_PXP_ConvertFrame(const byte *indexed, byte *yuv, int canvas_width)
{
    const int margin = (canvas_width - SCREENWIDTH) / 2;
    uint8_t *yplane = yuv;
    uint16_t *uplane, *vplane;
    int cw = canvas_width / 2;
    int x, y;

    uplane = (uint16_t *) (yuv + canvas_width * SCREENHEIGHT);
    vplane = (uint16_t *) ((uint8_t *) uplane + cw * (SCREENHEIGHT / 2));

    for (y = 0; y < SCREENHEIGHT; y++)
    {
        const uint8_t *s = indexed + y * SCREENWIDTH;
        uint32_t *yd = (uint32_t *) (yplane + y * canvas_width + margin);
        uint16_t *ud = NULL, *vd = NULL;

        if ((y & 1) == 0)
        {
            ud = uplane + ((y / 2) * cw + margin / 2) / 2;
            vd = vplane + ((y / 2) * cw + margin / 2) / 2;
        }

        for (x = 0; x < SCREENWIDTH; x += 4)
        {
            uint32_t p0 = palette_yuv[s[x]];
            uint32_t p1 = palette_yuv[s[x + 1]];
            uint32_t p2 = palette_yuv[s[x + 2]];
            uint32_t p3 = palette_yuv[s[x + 3]];

            *yd++ = (p0 & 0xff) | (p1 & 0xff) << 8
                  | (p2 & 0xff) << 16 | (p3 & 0xff) << 24;

            if (ud)
            {
                uint32_t u0 = ((p0 >> 8 & 0xff) + (p1 >> 8 & 0xff) + 1) >> 1;
                uint32_t u1 = ((p2 >> 8 & 0xff) + (p3 >> 8 & 0xff) + 1) >> 1;
                uint32_t v0 = ((p0 >> 16 & 0xff) + (p1 >> 16 & 0xff) + 1) >> 1;
                uint32_t v1 = ((p2 >> 16 & 0xff) + (p3 >> 16 & 0xff) + 1) >> 1;

                *ud++ = (uint16_t) (u0 | u1 << 8);
                *vd++ = (uint16_t) (v0 | v1 << 8);
            }
        }
    }
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;

    do
        r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

static int find_device(char *path, size_t len)
{
    int i;

    for (i = 0; i < 16; i++)
    {
        struct v4l2_capability cap;
        int fd;

        snprintf(path, len, "/dev/video%d", i);
        fd = open(path, O_RDWR);
        if (fd < 0)
            continue;
        memset(&cap, 0, sizeof(cap));
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0
            && (cap.device_caps & V4L2_CAP_VIDEO_M2M)
            && strcmp((const char *) cap.driver, "imx23-pxp") == 0)
        {
            close(fd);
            return 0;
        }
        close(fd);
    }
    return -1;
}

static int fail(const char *msg)
{
    fprintf(stderr, "PXP: %s", msg);
    if (errno)
        fprintf(stderr, ": %s", strerror(errno));
    fprintf(stderr, "\n");
    I_PXP_Shutdown();
    return 0;
}

boolean I_PXP_Init(const char *device, int fd_fb,
                   const struct fb_var_screeninfo *var,
                   const struct fb_fix_screeninfo *fix,
                   int vp_w, int vp_y, int vp_h)
{
    char path[32];
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    uint32_t pixfmt;
    int bpp = var->bits_per_pixel;
    int xres = var->xres, yres = var->yres;
    int hv, y0, k;
    enum v4l2_buf_type type;

    errno = 0;
    fbvar = *var;
    fd_fbdev = fd_fb;

    if (bpp == 32 && var->red.offset == 16 && var->green.offset == 8
        && var->blue.offset == 0)
        pixfmt = V4L2_PIX_FMT_XBGR32;
    else if (bpp == 16 && var->red.offset == 11 && var->red.length == 5
             && var->green.offset == 5 && var->green.length == 6
             && var->blue.offset == 0)
        pixfmt = V4L2_PIX_FMT_RGB565;
    else
        return fail("unsupported framebuffer pixel format");

    if ((xres & 7) || fix->line_length != (unsigned) (xres * bpp / 8))
        return fail("framebuffer width must be a multiple of 8 and unpadded");

    if (device == NULL)
    {
        if (find_device(path, sizeof(path)) < 0)
            return fail("no imx23-pxp V4L2 device found (try -pxpdev)");
        device = path;
    }

    fd_v4l = open(device, O_RDWR);
    if (fd_v4l < 0)
        return fail(device);

    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd_v4l, VIDIOC_QUERYCAP, &cap) < 0
        || !(cap.device_caps & V4L2_CAP_VIDEO_M2M))
        return fail("not a V4L2 mem2mem device");

    hv = clampi((vp_h + 4) & ~7, 8, yres & ~7);
    y0 = (vp_y + (vp_h - hv) / 2) & ~1;
    y0 = clampi(y0, 0, yres - hv);
    y0 &= ~1;

    canvas_w = SCREENWIDTH;
    if (vp_w < xres)
        canvas_w = clampi((xres * SCREENWIDTH / vp_w + 4) & ~7,
                          SCREENWIDTH, 1024);

    // The OUTPUT format must be set first: it resets the CAPTURE size.
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = canvas_w;
    fmt.fmt.pix.height = SCREENHEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.colorspace = V4L2_COLORSPACE_JPEG;
    fmt.fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_601;
    fmt.fmt.pix.quantization = V4L2_QUANTIZATION_FULL_RANGE;
    if (xioctl(fd_v4l, VIDIOC_S_FMT, &fmt) < 0)
        return fail("S_FMT (output)");
    if ((int) fmt.fmt.pix.width != canvas_w
        || (int) fmt.fmt.pix.height != SCREENHEIGHT
        || fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUV420)
        return fail("driver changed the source format");
    src_len = fmt.fmt.pix.sizeimage;

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = xres;
    fmt.fmt.pix.height = hv;
    fmt.fmt.pix.pixelformat = pixfmt;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd_v4l, VIDIOC_S_FMT, &fmt) < 0)
        return fail("S_FMT (capture)");
    if ((int) fmt.fmt.pix.width != xres || (int) fmt.fmt.pix.height != hv
        || fmt.fmt.pix.pixelformat != pixfmt)
        return fail("driver cannot produce the requested output size");
    cap_len = fmt.fmt.pix.sizeimage;

    fb_size = fix->smem_len;
    fb_base = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd_fb, 0);
    if (fb_base == MAP_FAILED)
    {
        fb_base = NULL;
        return fail("mmap framebuffer");
    }

    pages = (int) (var->yres_virtual / yres);
    pages = clampi(pages, 1, MAX_PAGES);
    if ((size_t) pages * yres * fix->line_length > fb_size)
        pages = 1;
    cap_offset = (size_t) y0 * fix->line_length;
    memset(fb_base, 0, fb_size);

    memset(&req, 0, sizeof(req));
    req.count = NUM_SRC;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_v4l, VIDIOC_REQBUFS, &req) < 0 || req.count < NUM_SRC)
        return fail("REQBUFS (output)");

    for (k = 0; k < NUM_SRC; k++)
    {
        uint8_t *p;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = k;
        if (xioctl(fd_v4l, VIDIOC_QUERYBUF, &buf) < 0)
            return fail("QUERYBUF");
        p = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                 fd_v4l, buf.m.offset);
        if (p == MAP_FAILED)
            return fail("mmap source buffer");
        src_buf[k] = p;

        // Black margins: Y = 0, U = V = 128 (never rewritten afterwards).
        memset(p, 0, canvas_w * SCREENHEIGHT);
        memset(p + canvas_w * SCREENHEIGHT, 128, src_len - canvas_w * SCREENHEIGHT);
    }

    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_v4l, VIDIOC_REQBUFS, &req) < 0 || req.count < 1)
        return fail("REQBUFS (capture)");

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    if (xioctl(fd_v4l, VIDIOC_QUERYBUF, &buf) < 0)
        return fail("QUERYBUF (capture)");
    if (buf.length < cap_len)
        return fail("capture buffer too small");
    cap_buf = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd_v4l, buf.m.offset);
    if (cap_buf == MAP_FAILED)
    {
        cap_buf = NULL;
        return fail("mmap capture buffer");
    }

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (xioctl(fd_v4l, VIDIOC_STREAMON, &type) < 0)
        return fail("STREAMON (output)");
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_v4l, VIDIOC_STREAMON, &type) < 0)
        return fail("STREAMON (capture)");

    page_bytes = (size_t) yres * fix->line_length;
    shown = 0;
    pending = 0;
    cur = 0;

    // Measure the refresh period; without it every flip waits for vsync.
    if (pages > 1)
    {
        unsigned int arg = 0;
        double t0, t1;
        int n;

        if (ioctl(fd_fb, FBIO_WAITFORVSYNC, &arg) < 0)
            vsync_ok = 0;
        else
        {
            t0 = now_sec();
            for (n = 0; n < 8 && vsync_ok; n++)
                if (ioctl(fd_fb, FBIO_WAITFORVSYNC, &arg) < 0)
                    vsync_ok = 0;
            t1 = now_sec();
            vsync_period = (t1 - t0) / 8;
            // Be conservative: only skip the wait if clearly past the period.
            vsync_period *= 1.25;
            if (vsync_period < 0.005 || vsync_period > 0.1)
                vsync_period = 1e9;
        }
    }

    fprintf(stderr, "PXP: %s, %dx%d from %dx%d canvas, %d page(s)\n",
            device, xres, hv, canvas_w, SCREENHEIGHT, pages);
    if (pages > 1 && vsync_ok && vsync_period < 1.0)
        fprintf(stderr, "PXP: refresh %.1f Hz\n", 1.25 / vsync_period);
    return true;
}

static int dequeue_pending(void)
{
    struct v4l2_buffer buf;
    int bad;

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd_v4l, VIDIOC_DQBUF, &buf) < 0)
        return -1;
    bad = buf.flags & V4L2_BUF_FLAG_ERROR;

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf.memory = V4L2_MEMORY_MMAP;
    xioctl(fd_v4l, VIDIOC_DQBUF, &buf);
    return bad ? -1 : 0;
}

void I_PXP_Present(const byte *indexed)
{
    struct v4l2_buffer buf;

    I_PXP_ConvertFrame(indexed, src_buf[cur], canvas_w);

    if (pending)
    {
        pending = 0;
        if (dequeue_pending() == 0)
        {
            int target = pages > 1 ? 1 - shown : 0;

            // Make sure the previous pan took effect before touching the
            // page that was on screen until then.
            // A pan takes effect at the first vsync after it was issued, so
            // once a full refresh period has passed it is already visible
            // and waiting for yet another vsync would only stall the game.
            if (pages > 1 && vsync_ok
                && now_sec() - pan_time < vsync_period)
            {
                unsigned int arg = 0;

                if (ioctl(fd_fbdev, FBIO_WAITFORVSYNC, &arg) < 0)
                    vsync_ok = 0;
            }

            memcpy(fb_base + target * page_bytes + cap_offset, cap_buf,
                   cap_len);

            if (pages > 1)
            {
                fbvar.yoffset = target * fbvar.yres;
                pan_time = now_sec();
                if (xioctl(fd_fbdev, FBIOPAN_DISPLAY, &fbvar) == 0)
                    shown = target;
            }
        }
    }

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    if (xioctl(fd_v4l, VIDIOC_QBUF, &buf) < 0)
        return;

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = cur;
    buf.bytesused = src_len;
    if (xioctl(fd_v4l, VIDIOC_QBUF, &buf) < 0)
    {
        // Reset the queues so the stale capture buffer is dropped.
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        xioctl(fd_v4l, VIDIOC_STREAMOFF, &t);
        xioctl(fd_v4l, VIDIOC_STREAMON, &t);
        return;
    }

    pending = 1;
    cur ^= 1;
}

void I_PXP_Shutdown(void)
{
    enum v4l2_buf_type type;
    int k;

    if (fd_v4l >= 0)
    {
        type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ioctl(fd_v4l, VIDIOC_STREAMOFF, &type);
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(fd_v4l, VIDIOC_STREAMOFF, &type);
    }
    if (fd_fbdev >= 0 && pages > 1)
    {
        fbvar.yoffset = 0;
        ioctl(fd_fbdev, FBIOPAN_DISPLAY, &fbvar);
    }
    for (k = 0; k < NUM_SRC; k++)
    {
        if (src_buf[k])
            munmap(src_buf[k], src_len);
        src_buf[k] = NULL;
    }
    if (cap_buf)
        munmap(cap_buf, cap_len);
    cap_buf = NULL;
    if (fb_base)
        munmap(fb_base, fb_size);
    fb_base = NULL;
    if (fd_v4l >= 0)
        close(fd_v4l);
    fd_v4l = -1;
    pending = 0;
}

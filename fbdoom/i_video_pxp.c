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
#include <poll.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <linux/fb.h>

#ifdef HAVE_DRM
#include <xf86drm.h>
#include <xf86drmMode.h>

// libdrm pulls in <stdbool.h>, which clashes with the enum in doomtype.h.
#undef true
#undef false
#undef bool
#endif

#include "i_video.h"
#include "i_video_pxp.h"
#include "m_argv.h"

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

// DMABUF scanout: the PXP renders straight into dumb buffers that are
// page-flipped through DRM/KMS, so no CPU copy is needed.
#define NUM_DRM 3

static int use_drm;
static uint32_t cap_mem = V4L2_MEMORY_MMAP;
#ifdef HAVE_DRM
static int fd_drm = -1;
static uint32_t drm_crtc, drm_conn;
static drmModeCrtc *old_crtc;
#endif
static struct
{
    uint32_t handle, fb_id;
    int dmafd;
} dbuf[NUM_DRM];
static int drm_screen;          // buffer on screen
static int drm_flip = -1;       // buffer being flipped to, or -1
static int drm_render = -1;     // buffer the PXP is currently writing

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

static inline __attribute__((always_inline))
void convert_frame(const byte *indexed, byte *yuv, int canvas_width,
                   const int width, const int height)
{
    const int margin = (canvas_width - width) / 2;
    uint8_t *yplane = yuv;
    uint16_t *uplane, *vplane;
    int cw = canvas_width / 2;
    int x, y;

    uplane = (uint16_t *) (yuv + canvas_width * height);
    vplane = (uint16_t *) ((uint8_t *) uplane + cw * (height / 2));

    for (y = 0; y < height; y++)
    {
        const uint8_t *s = indexed + y * width;
        uint32_t *yd = (uint32_t *) (yplane + y * canvas_width + margin);
        uint16_t *ud = NULL, *vd = NULL;

        if ((y & 1) == 0)
        {
            ud = uplane + ((y / 2) * cw + margin / 2) / 2;
            vd = vplane + ((y / 2) * cw + margin / 2) / 2;
        }

        for (x = 0; x < width; x += 4)
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

// Instantiated with constant sizes for the default 320x200 mode: with a
// runtime width the inner loop runs out of registers on the ARM926.
void I_PXP_ConvertFrame(const byte *indexed, byte *yuv, int canvas_width)
{
    if (video_scale == 1)
        convert_frame(indexed, yuv, canvas_width, ORIGWIDTH, ORIGHEIGHT);
    else
        convert_frame(indexed, yuv, canvas_width, SCREENWIDTH, SCREENHEIGHT);
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

static int dequeue_pending(void);

#ifdef HAVE_DRM
static void drm_close(void)
{
    int k;

    if (fd_drm < 0)
        return;
    // Hand the screen back to the fbdev console before freeing our buffers.
    if (old_crtc && old_crtc->buffer_id)
        drmModeSetCrtc(fd_drm, drm_crtc, old_crtc->buffer_id, old_crtc->x,
                       old_crtc->y, &drm_conn, 1, &old_crtc->mode);
    for (k = 0; k < NUM_DRM; k++)
    {
        struct drm_mode_destroy_dumb d;

        if (dbuf[k].dmafd >= 0)
            close(dbuf[k].dmafd);
        if (dbuf[k].fb_id)
            drmModeRmFB(fd_drm, dbuf[k].fb_id);
        if (dbuf[k].handle)
        {
            memset(&d, 0, sizeof(d));
            d.handle = dbuf[k].handle;
            drmIoctl(fd_drm, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        }
        dbuf[k].dmafd = -1;
        dbuf[k].fb_id = dbuf[k].handle = 0;
    }
    if (old_crtc)
        drmModeFreeCrtc(old_crtc);
    old_crtc = NULL;
    close(fd_drm);
    fd_drm = -1;
    drm_flip = -1;
    drm_screen = 0;
}

static int drm_find_output(int xres, int yres, drmModeModeInfo *mode)
{
    drmModeRes *res = drmModeGetResources(fd_drm);
    drmModeConnector *conn = NULL;
    int i, ok = 0;

    if (res == NULL)
        return 0;
    for (i = 0; i < res->count_connectors && !conn; i++)
    {
        conn = drmModeGetConnector(fd_drm, res->connectors[i]);
        if (conn && (conn->connection != DRM_MODE_CONNECTED
                     || conn->count_modes == 0))
        {
            drmModeFreeConnector(conn);
            conn = NULL;
        }
    }
    if (conn)
    {
        drmModeEncoder *enc = conn->encoder_id
            ? drmModeGetEncoder(fd_drm, conn->encoder_id) : NULL;

        for (i = 0; i < conn->count_modes; i++)
            if (conn->modes[i].hdisplay == xres
                && conn->modes[i].vdisplay == yres)
            {
                *mode = conn->modes[i];
                ok = 1;
                break;
            }
        drm_conn = conn->connector_id;
        if (enc && enc->crtc_id)
            drm_crtc = enc->crtc_id;
        else if (res->count_crtcs > 0)
            drm_crtc = res->crtcs[0];
        else
            ok = 0;
        if (enc)
            drmModeFreeEncoder(enc);
        drmModeFreeConnector(conn);
    }
    drmModeFreeResources(res);
    return ok;
}

// Sets up the DRM buffers and the DMABUF capture queue, then runs one
// test job. Returns 0 (with everything undone) if anything is unsupported.
static int drm_start(int xres, int yres)
{
    drmModeModeInfo mode;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    struct pollfd pfd;
    enum v4l2_buf_type type;
    const char *why = "DRM unavailable";
    int k;

    for (k = 0; k < NUM_DRM; k++)
        dbuf[k].dmafd = -1;

    fd_drm = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd_drm < 0 || !drm_find_output(xres, yres, &mode))
        goto bad;

    old_crtc = drmModeGetCrtc(fd_drm, drm_crtc);

    for (k = 0; k < NUM_DRM; k++)
    {
        struct drm_mode_create_dumb c;
        struct drm_mode_map_dumb m;
        void *p;

        memset(&c, 0, sizeof(c));
        c.width = xres;
        c.height = yres;
        c.bpp = 32;
        if (drmIoctl(fd_drm, DRM_IOCTL_MODE_CREATE_DUMB, &c) < 0)
            goto bad;
        dbuf[k].handle = c.handle;
        if (c.pitch != (uint32_t) xres * 4 || c.size < cap_len)
        {
            why = "unsuitable dumb buffer layout";
            goto bad;
        }
        if (drmModeAddFB(fd_drm, xres, yres, 24, 32, c.pitch, c.handle,
                         &dbuf[k].fb_id) < 0
            || drmPrimeHandleToFD(fd_drm, c.handle, DRM_CLOEXEC,
                                  &dbuf[k].dmafd) < 0)
            goto bad;

        memset(&m, 0, sizeof(m));
        m.handle = c.handle;
        if (drmIoctl(fd_drm, DRM_IOCTL_MODE_MAP_DUMB, &m) == 0)
        {
            p = mmap(NULL, c.size, PROT_WRITE, MAP_SHARED, fd_drm, m.offset);
            if (p != MAP_FAILED)
            {
                memset(p, 0, c.size);
                munmap(p, c.size);
            }
        }
    }

    cap_mem = V4L2_MEMORY_DMABUF;
    memset(&req, 0, sizeof(req));
    req.count = NUM_DRM;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_DMABUF;
    if (xioctl(fd_v4l, VIDIOC_REQBUFS, &req) < 0 || req.count < NUM_DRM)
    {
        why = "capture queue does not accept DMABUF";
        goto bad;
    }

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    xioctl(fd_v4l, VIDIOC_STREAMON, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd_v4l, VIDIOC_STREAMON, &type) < 0)
        goto bad;

    // Test job into a buffer that is not on screen.
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.index = 1;
    buf.m.fd = dbuf[1].dmafd;
    buf.length = cap_len;
    if (xioctl(fd_v4l, VIDIOC_QBUF, &buf) < 0)
    {
        why = "cannot queue DMABUF";
        goto bad;
    }
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    buf.bytesused = src_len;
    if (xioctl(fd_v4l, VIDIOC_QBUF, &buf) < 0)
        goto bad;
    pfd.fd = fd_v4l;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 1000) <= 0 || dequeue_pending() < 0)
    {
        why = "DMABUF test job failed";
        goto bad;
    }

    if (drmModeSetCrtc(fd_drm, drm_crtc, dbuf[0].fb_id, 0, 0, &drm_conn, 1,
                       &mode) < 0)
    {
        why = "cannot set the DRM mode (not DRM master?)";
        goto bad;
    }
    drm_screen = 0;
    drm_flip = -1;
    drm_render = -1;
    return 1;

bad:
    fprintf(stderr, "PXP: %s (%s), using the fbdev copy path\n", why,
            errno ? strerror(errno) : "no error");
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(fd_v4l, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    xioctl(fd_v4l, VIDIOC_STREAMOFF, &type);
    memset(&req, 0, sizeof(req));
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_DMABUF;
    xioctl(fd_v4l, VIDIOC_REQBUFS, &req);
    cap_mem = V4L2_MEMORY_MMAP;
    drm_close();
    return 0;
}

#else
// Built without libdrm: only the fbdev copy path is available.
static void drm_close(void)
{
}

static int drm_start(int xres, int yres)
{
    (void) xres;
    (void) yres;
    return 0;
}
#endif

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

    if (!M_CheckParm("-pxpfb") && pixfmt == V4L2_PIX_FMT_XBGR32
        && cap_offset == 0)
        use_drm = drm_start(xres, yres);

    if (!use_drm)
    {
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
    fprintf(stderr, "PXP: %s\n", use_drm
            ? "DMABUF scanout through DRM, no framebuffer copy"
            : "copying into the fbdev framebuffer");
    if (!use_drm && pages > 1 && vsync_ok && vsync_period < 1.0)
        fprintf(stderr, "PXP: refresh %.1f Hz\n", 1.25 / vsync_period);
    return true;
}

static int dequeue_pending(void)
{
    struct v4l2_buffer buf;
    int bad;

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = cap_mem;
    if (xioctl(fd_v4l, VIDIOC_DQBUF, &buf) < 0)
        return -1;
    bad = buf.flags & V4L2_BUF_FLAG_ERROR;

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf.memory = V4L2_MEMORY_MMAP;
    xioctl(fd_v4l, VIDIOC_DQBUF, &buf);
    return bad ? -1 : 0;
}

static void present_copy(void)
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

#ifdef HAVE_DRM
static void drm_flip_done(int fd, unsigned seq, unsigned sec, unsigned usec,
                         void *data)
{
    (void) fd; (void) seq; (void) sec; (void) usec; (void) data;
    if (drm_flip >= 0)
        drm_screen = drm_flip;
    drm_flip = -1;
}

static void present_drm(void)
{
    drmEventContext ev;
    struct pollfd pfd;

    memset(&ev, 0, sizeof(ev));
    ev.version = 2;
    ev.page_flip_handler = drm_flip_done;
    pfd.fd = fd_drm;
    pfd.events = POLLIN;

    // The previous flip must have happened before another one is queued;
    // this is what limits the frame rate to the display refresh.
    while (drm_flip >= 0)
    {
        if (poll(&pfd, 1, 1000) <= 0)
        {
            drm_flip_done(fd_drm, 0, 0, 0, NULL);
            break;
        }
        drmHandleEvent(fd_drm, &ev);
    }

    if (drm_render >= 0
        && drmModePageFlip(fd_drm, drm_crtc, dbuf[drm_render].fb_id,
                           DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0)
        drm_flip = drm_render;
}

#else
static void present_drm(void)
{
}
#endif

static int drm_free_buffer(void)
{
    int k;

    for (k = 0; k < NUM_DRM; k++)
        if (k != drm_screen && k != drm_flip)
            return k;
    return 0;
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
            if (use_drm)
                present_drm();
            else
                present_copy();
        }
    }

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = cap_mem;
    buf.index = 0;
    if (use_drm)
    {
        buf.index = drm_free_buffer();
        buf.m.fd = dbuf[buf.index].dmafd;
        buf.length = cap_len;
        drm_render = buf.index;
    }
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
    drm_close();
    if (fd_fbdev >= 0 && pages > 1 && !use_drm)
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
    use_drm = 0;
    cap_mem = V4L2_MEMORY_MMAP;
}

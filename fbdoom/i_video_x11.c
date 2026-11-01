//
// X11 software video and input backend for framebuffer Doom's ALSA build.
//

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "i_fps.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomkeys.h"
#include "doomtype.h"
#include "d_event.h"
#include "i_system.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_config.h"
#include "tables.h"
#include "i_video_scaling.h"
#include "v_video.h"
#include "z_zone.h"

#define X11_MAX_SCALE 8
#define X11_KEY_LIMIT 256

typedef struct
{
    byte r;
    byte g;
    byte b;
} palette_color_t;

static Display *display;
static Window window;
static GC graphics_context;
static XImage *image;
static Atom wm_delete_window;
static Atom wm_state;
static Atom wm_state_fullscreen;
static int scale_factor = 3;
static int image_width;
static int image_height;
static boolean fullscreen_requested;
static boolean stretch_to_fill;
static video_viewport_t viewport;
static int pointer_grabbed;
static int pointer_center_x;
static int pointer_center_y;
static int pointer_ready;
static unsigned int mouse_button_state;
static unsigned int shift_mask;
static boolean key_down[X11_KEY_LIMIT];
static char *window_title = "fbDOOM";
static palette_color_t palette[256];
static unsigned long pixel_palette[256];
static unsigned int red_shift;
static unsigned int green_shift;
static unsigned int blue_shift;
static unsigned long red_max;
static unsigned long green_max;
static unsigned long blue_max;

byte *I_VideoBuffer;
int screen_width = SCREENWIDTH;
int screen_height = SCREENHEIGHT;
int screen_bpp;
int usemouse = 1;
int fullscreen;
int aspect_ratio_correct = 1;
int novert;
int vanilla_keyboard_mapping = 1;
boolean screensaver_mode;
boolean screenvisible;
float mouse_acceleration = 2.0f;
int mouse_threshold = 10;
int usegamma;

static unsigned int MaskShift(unsigned long mask)
{
    unsigned int shift = 0;

    while (mask != 0 && (mask & 1UL) == 0)
    {
        mask >>= 1;
        ++shift;
    }

    return shift;
}

static unsigned long MaskMaximum(unsigned long mask, unsigned int shift)
{
    return mask >> shift;
}

static unsigned long PackChannel(byte value, unsigned int shift,
                                 unsigned long maximum)
{
    return ((((unsigned long) value * maximum) + 127) / 255) << shift;
}

static void RebuildPalette(void)
{
    int i;

    for (i = 0; i < 256; ++i)
    {
        pixel_palette[i] =
            PackChannel(palette[i].r, red_shift, red_max)
          | PackChannel(palette[i].g, green_shift, green_max)
          | PackChannel(palette[i].b, blue_shift, blue_max);
    }
}

static void WritePixel(char *destination, unsigned long pixel)
{
    int byte_index;
    int bytes_per_pixel = image->bits_per_pixel / 8;

    if (image->byte_order == LSBFirst)
    {
        for (byte_index = 0; byte_index < bytes_per_pixel; ++byte_index)
        {
            destination[byte_index] = pixel >> (byte_index * 8);
        }
    }
    else
    {
        for (byte_index = 0; byte_index < bytes_per_pixel; ++byte_index)
        {
            destination[byte_index] =
                pixel >> ((bytes_per_pixel - byte_index - 1) * 8);
        }
    }
}

static void RenderImage(void)
{
    int y;

    memset(image->data, 0,
           (size_t) image->bytes_per_line * image->height);

    for (y = 0; y < viewport.height; ++y)
    {
        int source_y = I_MapOutputToSource(y, viewport.height, SCREENHEIGHT);
        char *destination =
            image->data + (viewport.y + y) * image->bytes_per_line;
        int x;

        for (x = 0; x < viewport.width; ++x)
        {
            int source_x = I_MapOutputToSource(x, viewport.width, SCREENWIDTH);
            unsigned long pixel =
                pixel_palette[I_VideoBuffer[source_y * SCREENWIDTH + source_x]];

            WritePixel(destination + (viewport.x + x)
                       * (image->bits_per_pixel / 8), pixel);
        }
    }
}

static boolean ResizeImage(int width, int height)
{
    if (width <= 0 || height <= 0 || display == NULL || window == 0)
    {
        return false;
    }

    if (image != NULL && image_width == width && image_height == height)
    {
        return true;
    }

    if (image != NULL)
    {
        XDestroyImage(image);
        image = NULL;
    }

    image = XCreateImage(display, DefaultVisual(display, DefaultScreen(display)),
                         DefaultDepth(display, DefaultScreen(display)),
                         ZPixmap, 0, NULL, width, height, 32, 0);
    if (image == NULL || image->bits_per_pixel % 8 != 0)
    {
        if (image != NULL)
        {
            XDestroyImage(image);
            image = NULL;
        }
        I_Error("Unsupported X11 image format");
        return false;
    }

    image->data = calloc((size_t) image->bytes_per_line, image->height);
    if (image->data == NULL)
    {
        XDestroyImage(image);
        image = NULL;
        I_Error("Cannot allocate X11 image buffer");
        return false;
    }

    image_width = width;
    image_height = height;
    if (!I_CalculateVideoViewport(width, height, stretch_to_fill, &viewport))
    {
        I_Error("Cannot calculate X11 output scaling");
        return false;
    }

    screen_width = width;
    screen_height = height;
    pointer_center_x = width / 2;
    pointer_center_y = height / 2;
    if (pointer_grabbed)
    {
        XWarpPointer(display, None, window, 0, 0, 0, 0,
                     pointer_center_x, pointer_center_y);
        pointer_ready = 0;
    }

    return true;
}

static int TranslateKey(KeySym symbol)
{
    if (symbol >= XK_a && symbol <= XK_z)
    {
        return (int) symbol;
    }
    if (symbol >= XK_A && symbol <= XK_Z)
    {
        return (int) (symbol - XK_A + 'a');
    }
    if (symbol >= XK_0 && symbol <= XK_9)
    {
        return (int) symbol;
    }
    if (symbol >= XK_F1 && symbol <= XK_F10)
    {
        return KEY_F1 + (int) (symbol - XK_F1);
    }

    switch (symbol)
    {
        case XK_Escape: return KEY_ESCAPE;
        case XK_Return: return KEY_ENTER;
        case XK_KP_Enter: return KEYP_ENTER;
        case XK_Tab: return KEY_TAB;
        case XK_BackSpace: return KEY_BACKSPACE;
        case XK_space: return ' ';
        case XK_minus: return KEY_MINUS;
        case XK_equal: return KEY_EQUALS;
        case XK_bracketleft: return '[';
        case XK_bracketright: return ']';
        case XK_backslash: return '\\';
        case XK_semicolon: return ';';
        case XK_apostrophe: return '\'';
        case XK_grave: return '`';
        case XK_comma: return ',';
        case XK_period: return '.';
        case XK_slash: return '/';
        case XK_F11: return KEY_F11;
        case XK_F12: return KEY_F12;
        case XK_Left: return KEY_LEFTARROW;
        case XK_Right: return KEY_RIGHTARROW;
        case XK_Up: return KEY_UPARROW;
        case XK_Down: return KEY_DOWNARROW;
        case XK_Shift_L:
        case XK_Shift_R: return KEY_RSHIFT;
        case XK_Control_L:
        case XK_Control_R: return KEY_RCTRL;
        case XK_Alt_L:
        case XK_Alt_R:
        case XK_Meta_L:
        case XK_Meta_R: return KEY_RALT;
        case XK_Caps_Lock: return KEY_CAPSLOCK;
        case XK_Num_Lock: return KEY_NUMLOCK;
        case XK_Scroll_Lock: return KEY_SCRLCK;
        case XK_Pause: return KEY_PAUSE;
        case XK_Insert: return KEY_INS;
        case XK_Delete: return KEY_DEL;
        case XK_Home:
        case XK_KP_Home: return KEY_HOME;
        case XK_End:
        case XK_KP_End: return KEY_END;
        case XK_Page_Up:
        case XK_KP_Page_Up: return KEY_PGUP;
        case XK_Page_Down:
        case XK_KP_Page_Down: return KEY_PGDN;
        case XK_KP_0: return KEYP_0;
        case XK_KP_1: return KEYP_1;
        case XK_KP_2: return KEYP_2;
        case XK_KP_3: return KEYP_3;
        case XK_KP_4: return KEYP_4;
        case XK_KP_5: return KEYP_5;
        case XK_KP_6: return KEYP_6;
        case XK_KP_7: return KEYP_7;
        case XK_KP_8: return KEYP_8;
        case XK_KP_9: return KEYP_9;
        case XK_KP_Decimal: return KEYP_PERIOD;
        case XK_KP_Add: return KEYP_PLUS;
        case XK_KP_Subtract: return KEYP_MINUS;
        case XK_KP_Multiply: return KEYP_MULTIPLY;
        case XK_KP_Divide: return KEYP_DIVIDE;
        default: return 0;
    }
}

static int ShiftCharacter(int key)
{
    if (key >= 'a' && key <= 'z')
    {
        return key - 'a' + 'A';
    }

    switch (key)
    {
        case '1': return '!';
        case '2': return '@';
        case '3': return '#';
        case '4': return '$';
        case '5': return '%';
        case '6': return '^';
        case '7': return '&';
        case '8': return '*';
        case '9': return '(';
        case '0': return ')';
        case '-': return '_';
        case '=': return '+';
        case '[': return '{';
        case ']': return '}';
        case '\\': return '|';
        case ';': return ':';
        case '\'': return '"';
        case '`': return '~';
        case ',': return '<';
        case '.': return '>';
        case '/': return '?';
        default: return key;
    }
}

static int AccelerateMouse(int value)
{
    if (value < 0)
    {
        return -AccelerateMouse(-value);
    }
    if (value > mouse_threshold)
    {
        return (int) ((value - mouse_threshold) * mouse_acceleration)
             + mouse_threshold;
    }
    return value;
}

static void PostMouseEvent(int dx, int dy, boolean buttons_changed)
{
    event_t event;

    if (dx == 0 && dy == 0 && !buttons_changed)
    {
        return;
    }

    event.type = ev_mouse;
    event.data1 = mouse_button_state;
    event.data2 = AccelerateMouse(dx);
    event.data3 = -AccelerateMouse(dy);
    D_PostEvent(&event);
}

static void GrabPointer(void)
{
    if (display == NULL || window == 0 || !usemouse || pointer_grabbed)
    {
        return;
    }

    if (XGrabPointer(display, window, True, PointerMotionMask
                     | ButtonPressMask | ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync, window, None,
                     CurrentTime) == GrabSuccess)
    {
        pointer_grabbed = 1;
        pointer_ready = 0;
        XWarpPointer(display, None, window, 0, 0, 0, 0,
                     pointer_center_x, pointer_center_y);
    }
}

static void UngrabPointer(void)
{
    if (display != NULL && pointer_grabbed)
    {
        XUngrabPointer(display, CurrentTime);
    }
    pointer_grabbed = 0;
    pointer_ready = 0;
}

static void ReleaseInputState(void)
{
    event_t event;
    int key;

    for (key = 0; key < X11_KEY_LIMIT; ++key)
    {
        if (key_down[key])
        {
            event.type = ev_keyup;
            event.data1 = key;
            event.data2 = 0;
            event.data3 = 0;
            D_PostEvent(&event);
            key_down[key] = false;
        }
    }

    shift_mask = 0;
    if (mouse_button_state != 0)
    {
        mouse_button_state = 0;
        PostMouseEvent(0, 0, true);
    }
}

static void HandleKeyEvent(XKeyEvent *key_event, boolean pressed)
{
    event_t event;
    KeySym symbol = XLookupKeysym(key_event, 0);
    int key = TranslateKey(symbol);

    if (symbol == XK_Shift_L || symbol == XK_Shift_R)
    {
        unsigned int mask = symbol == XK_Shift_L ? 1U : 2U;

        if (pressed)
        {
            shift_mask |= mask;
        }
        else
        {
            shift_mask &= ~mask;
        }
    }

    if (key == 0)
    {
        return;
    }

    event.type = pressed ? ev_keydown : ev_keyup;
    event.data1 = key;
    event.data2 = pressed && shift_mask != 0
                ? ShiftCharacter(key) : (pressed ? key : 0);
    event.data3 = 0;
    D_PostEvent(&event);
    if (key >= 0 && key < X11_KEY_LIMIT)
    {
        key_down[key] = pressed;
    }
}

static int TranslateMouseButton(unsigned int button)
{
    switch (button)
    {
        case Button1: return 0;
        case Button3: return 1;
        case Button2: return 2;
        case 8: return 3;
        case 9: return 4;
        default: return -1;
    }
}

static void HandleEvent(XEvent *event)
{
    switch (event->type)
    {
        case Expose:
            if (event->xexpose.count == 0 && image != NULL)
            {
                XPutImage(display, window, graphics_context, image,
                          0, 0, 0, 0, image->width, image->height);
            }
            break;

        case ConfigureNotify:
            if (event->xconfigure.window == window)
            {
                ResizeImage(event->xconfigure.width,
                            event->xconfigure.height);
            }
            break;

        case KeyPress:
            HandleKeyEvent(&event->xkey, true);
            break;

        case KeyRelease:
            if (XPending(display))
            {
                XEvent next;
                XPeekEvent(display, &next);
                if (next.type == KeyPress
                 && next.xkey.keycode == event->xkey.keycode
                 && next.xkey.time == event->xkey.time)
                {
                    break;
                }
            }
            HandleKeyEvent(&event->xkey, false);
            break;

        case MotionNotify:
        {
            int dx, dy;

            if (!pointer_grabbed)
            {
                break;
            }
            dx = event->xmotion.x - pointer_center_x;
            dy = event->xmotion.y - pointer_center_y;
            if (!pointer_ready)
            {
                pointer_ready = 1;
                break;
            }
            if (dx != 0 || dy != 0)
            {
                PostMouseEvent(dx, dy, false);
                XWarpPointer(display, None, window, 0, 0, 0, 0,
                             pointer_center_x, pointer_center_y);
            }
            break;
        }

        case ButtonPress:
        case ButtonRelease:
        {
            unsigned int button = event->xbutton.button;
            int mouse_button = TranslateMouseButton(button);

            if (button == Button4 || button == Button5)
            {
                if (event->type == ButtonPress)
                {
                    event_t key_event;
                    key_event.type = ev_keydown;
                    key_event.data1 = button == Button4 ? KEY_PGUP : KEY_PGDN;
                    key_event.data2 = key_event.data1;
                    key_event.data3 = 0;
                    D_PostEvent(&key_event);
                    key_event.type = ev_keyup;
                    key_event.data2 = 0;
                    D_PostEvent(&key_event);
                }
                break;
            }

            if (mouse_button >= 0)
            {
                unsigned int old_state = mouse_button_state;
                unsigned int mask = 1U << mouse_button;

                if (event->type == ButtonPress)
                {
                    mouse_button_state |= mask;
                }
                else
                {
                    mouse_button_state &= ~mask;
                }
                PostMouseEvent(0, 0, old_state != mouse_button_state);
            }
            break;
        }

        case FocusIn:
            GrabPointer();
            break;

        case FocusOut:
            UngrabPointer();
            ReleaseInputState();
            break;

        case ClientMessage:
            if ((Atom) event->xclient.data.l[0] == wm_delete_window)
            {
                I_Quit();
                exit(0);
            }
            break;

        default:
            break;
    }
}

void I_InitGraphics(void)
{
    XVisualInfo template;
    XVisualInfo *visual_info;
    int screen;
    int scale_arg;
    int num_visuals;
    int window_width, window_height;

    display = XOpenDisplay(NULL);
    if (display == NULL)
    {
        I_Error("Cannot open X display; check DISPLAY and X server access");
        return;
    }

    screen = DefaultScreen(display);
    template.visualid = XVisualIDFromVisual(DefaultVisual(display, screen));
    visual_info = XGetVisualInfo(display, VisualIDMask, &template,
                                 &num_visuals);
    if (visual_info == NULL || num_visuals == 0)
    {
        I_Error("Cannot query X11 default visual");
        return;
    }

    red_shift = MaskShift(visual_info->red_mask);
    green_shift = MaskShift(visual_info->green_mask);
    blue_shift = MaskShift(visual_info->blue_mask);
    red_max = MaskMaximum(visual_info->red_mask, red_shift);
    green_max = MaskMaximum(visual_info->green_mask, green_shift);
    blue_max = MaskMaximum(visual_info->blue_mask, blue_shift);
    XFree(visual_info);

    fullscreen_requested = M_CheckParm("-fullscreen") > 0;
    if (M_CheckParm("-window") > 0)
    {
        fullscreen_requested = false;
    }
    stretch_to_fill = M_CheckParm("-stretch") > 0;

    scale_arg = M_CheckParmWithArgs("-scale", 1);
    if (scale_arg > 0)
    {
        int requested_scale = atoi(myargv[scale_arg + 1]);
        if (requested_scale < 1 || requested_scale > X11_MAX_SCALE)
        {
            I_Error("-scale must be between 1 and %d", X11_MAX_SCALE);
            return;
        }
        scale_factor = requested_scale;
    }

    window_width = SCREENWIDTH * scale_factor;
    window_height = (SCREENHEIGHT * 6 / 5) * scale_factor;
    if (fullscreen_requested)
    {
        window_width = DisplayWidth(display, screen);
        window_height = DisplayHeight(display, screen);
    }

    screen_width = window_width;
    screen_height = window_height;
    screen_bpp = DefaultDepth(display, screen);
    pointer_center_x = window_width / 2;
    pointer_center_y = window_height / 2;

    window = XCreateSimpleWindow(display, RootWindow(display, screen),
                                 0, 0, window_width, window_height, 0,
                                 BlackPixel(display, screen),
                                 BlackPixel(display, screen));
    XSelectInput(display, window, ExposureMask | KeyPressMask | KeyReleaseMask
                 | ButtonPressMask | ButtonReleaseMask | PointerMotionMask
                 | FocusChangeMask | StructureNotifyMask);

    wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete_window, 1);
    wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    wm_state_fullscreen =
        XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    XStoreName(display, window, window_title);
    graphics_context = XCreateGC(display, window, 0, NULL);
    XMapWindow(display, window);
    if (fullscreen_requested)
    {
        XEvent state_event;

        memset(&state_event, 0, sizeof(state_event));
        state_event.xclient.type = ClientMessage;
        state_event.xclient.window = window;
        state_event.xclient.message_type = wm_state;
        state_event.xclient.format = 32;
        state_event.xclient.data.l[0] = 1;
        state_event.xclient.data.l[1] = wm_state_fullscreen;
        state_event.xclient.data.l[2] = 0;
        XSendEvent(display, RootWindow(display, screen), False,
                   SubstructureRedirectMask | SubstructureNotifyMask,
                   &state_event);
        XMoveResizeWindow(display, window, 0, 0, window_width, window_height);
    }
    XSync(display, False);

    if (!ResizeImage(window_width, window_height))
    {
        return;
    }

    I_VideoBuffer = Z_Malloc(SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);
    memset(I_VideoBuffer, 0, SCREENWIDTH * SCREENHEIGHT);
    screenvisible = true;
    RebuildPalette();
    I_AtExit(I_ShutdownGraphics, true);
    GrabPointer();
}

void I_ShutdownGraphics(void)
{
    UngrabPointer();
    if (image != NULL)
    {
        XDestroyImage(image);
        image = NULL;
    }
    if (display != NULL)
    {
        if (graphics_context != 0)
        {
            XFreeGC(display, graphics_context);
            graphics_context = 0;
        }
        if (window != 0)
        {
            XDestroyWindow(display, window);
            window = 0;
        }
        XCloseDisplay(display);
        display = NULL;
    }
    if (I_VideoBuffer != NULL)
    {
        Z_Free(I_VideoBuffer);
        I_VideoBuffer = NULL;
    }
}

void I_StartFrame(void)
{
}

void I_GetEvent(void)
{
    while (display != NULL && XPending(display))
    {
        XEvent event;
        XNextEvent(display, &event);
        HandleEvent(&event);
    }
}

void I_StartTic(void)
{
    I_GetEvent();
}

void I_UpdateNoBlit(void)
{
}

void I_FinishUpdate(void)
{
    if (display == NULL || image == NULL || I_VideoBuffer == NULL)
    {
        return;
    }

    I_FPS_Frame(I_VideoBuffer);
    RenderImage();
    XPutImage(display, window, graphics_context, image,
              0, 0, 0, 0, image->width, image->height);
}

void I_ReadScreen(byte *screen_buffer)
{
    memcpy(screen_buffer, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
}

void I_SetPalette(byte *doom_palette)
{
    int i;

    for (i = 0; i < 256; ++i)
    {
        palette[i].r = gammatable[usegamma][*doom_palette++];
        palette[i].g = gammatable[usegamma][*doom_palette++];
        palette[i].b = gammatable[usegamma][*doom_palette++];
    }
    RebuildPalette();
}

int I_GetPaletteIndex(int red, int green, int blue)
{
    int best_index = 0;
    int best_difference = INT_MAX;
    int i;

    for (i = 0; i < 256; ++i)
    {
        int dr = red - palette[i].r;
        int dg = green - palette[i].g;
        int db = blue - palette[i].b;
        int difference = dr * dr + dg * dg + db * db;

        if (difference < best_difference)
        {
            best_difference = difference;
            best_index = i;
        }
    }
    return best_index;
}

void I_BeginRead(void)
{
}

void I_EndRead(void)
{
}

void I_SetWindowTitle(char *title)
{
    window_title = title != NULL ? title : "fbDOOM";
    if (display != NULL && window != 0)
    {
        XStoreName(display, window, window_title);
    }
}

void I_GraphicsCheckCommandLine(void)
{
}

void I_SetGrabMouseCallback(grabmouse_callback_t callback)
{
    (void) callback;
}

void I_EnableLoadingDisk(void)
{
}

void I_BindVideoVariables(void)
{
}

void I_DisplayFPSDots(boolean dots_on)
{
    (void) dots_on;
}

void I_CheckIsScreensaver(void)
{
}

void I_InitWindowTitle(void)
{
}

void I_InitWindowIcon(void)
{
}

void kbd_shutdown(void)
{
    I_Quit();
    exit(0);
}

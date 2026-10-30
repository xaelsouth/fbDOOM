//
// Linux evdev input backend for framebuffer Doom.
//

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

enum
{
    LINUX_KEY_ENTER = KEY_ENTER,
    LINUX_KEY_TAB = KEY_TAB,
    LINUX_KEY_BACKSPACE = KEY_BACKSPACE,
    LINUX_KEY_MINUS = KEY_MINUS,
    LINUX_KEY_EQUAL = KEY_EQUAL,
    LINUX_KEY_PAUSE = KEY_PAUSE,
    LINUX_KEY_CAPSLOCK = KEY_CAPSLOCK,
    LINUX_KEY_NUMLOCK = KEY_NUMLOCK,
    LINUX_KEY_SCROLLLOCK = KEY_SCROLLLOCK,
    LINUX_KEY_INSERT = KEY_INSERT,
    LINUX_KEY_DELETE = KEY_DELETE,
    LINUX_KEY_HOME = KEY_HOME,
    LINUX_KEY_END = KEY_END,
    LINUX_KEY_PAGEUP = KEY_PAGEUP,
    LINUX_KEY_PAGEDOWN = KEY_PAGEDOWN,
    LINUX_KEY_F1 = KEY_F1,
    LINUX_KEY_F2 = KEY_F2,
    LINUX_KEY_F3 = KEY_F3,
    LINUX_KEY_F4 = KEY_F4,
    LINUX_KEY_F5 = KEY_F5,
    LINUX_KEY_F6 = KEY_F6,
    LINUX_KEY_F7 = KEY_F7,
    LINUX_KEY_F8 = KEY_F8,
    LINUX_KEY_F9 = KEY_F9,
    LINUX_KEY_F10 = KEY_F10,
    LINUX_KEY_F11 = KEY_F11,
    LINUX_KEY_F12 = KEY_F12
};

#undef KEY_ENTER
#undef KEY_TAB
#undef KEY_BACKSPACE
#undef KEY_MINUS
#undef KEY_PAUSE
#undef KEY_CAPSLOCK
#undef KEY_NUMLOCK
#undef KEY_SCRLCK
#undef KEY_INS
#undef KEY_DEL
#undef KEY_HOME
#undef KEY_END
#undef KEY_PGUP
#undef KEY_PGDN
#undef KEY_F1
#undef KEY_F2
#undef KEY_F3
#undef KEY_F4
#undef KEY_F5
#undef KEY_F6
#undef KEY_F7
#undef KEY_F8
#undef KEY_F9
#undef KEY_F10
#undef KEY_F11
#undef KEY_F12

#include "doomkeys.h"
#include "doomtype.h"
#include "d_event.h"
#include "i_system.h"
#include "i_video.h"
#include "m_controls.h"

#define MAX_INPUT_DEVICES 64
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define BIT_WORD(bit) ((bit) / BITS_PER_LONG)
#define TEST_BIT(bit, array) \
    (((array)[BIT_WORD(bit)] >> ((bit) % BITS_PER_LONG)) & 1)

typedef struct
{
    int fd;
    boolean keyboard;
    boolean mouse;
} input_device_t;

static input_device_t devices[MAX_INPUT_DEVICES];
static int num_devices;
static unsigned int mouse_button_state;
static unsigned int shift_mask;

int vanilla_keyboard_mapping = 1;

static int TranslateKey(unsigned int code)
{
    if (code >= KEY_1 && code <= KEY_9)
    {
        return '1' + code - KEY_1;
    }

    switch (code)
    {
        case KEY_A: return 'a';
        case KEY_B: return 'b';
        case KEY_C: return 'c';
        case KEY_D: return 'd';
        case KEY_E: return 'e';
        case KEY_F: return 'f';
        case KEY_G: return 'g';
        case KEY_H: return 'h';
        case KEY_I: return 'i';
        case KEY_J: return 'j';
        case KEY_K: return 'k';
        case KEY_L: return 'l';
        case KEY_M: return 'm';
        case KEY_N: return 'n';
        case KEY_O: return 'o';
        case KEY_P: return 'p';
        case KEY_Q: return 'q';
        case KEY_R: return 'r';
        case KEY_S: return 's';
        case KEY_T: return 't';
        case KEY_U: return 'u';
        case KEY_V: return 'v';
        case KEY_W: return 'w';
        case KEY_X: return 'x';
        case KEY_Y: return 'y';
        case KEY_Z: return 'z';
        case KEY_0: return '0';
        case KEY_ESC: return KEY_ESCAPE;
        case LINUX_KEY_ENTER: return KEY_ENTER;
        case KEY_KPENTER: return KEYP_ENTER;
        case LINUX_KEY_TAB: return KEY_TAB;
        case LINUX_KEY_BACKSPACE: return KEY_BACKSPACE;
        case KEY_SPACE: return ' ';
        case LINUX_KEY_MINUS: return KEY_MINUS;
        case LINUX_KEY_EQUAL: return KEY_EQUALS;
        case KEY_LEFTBRACE: return '[';
        case KEY_RIGHTBRACE: return ']';
        case KEY_BACKSLASH: return '\\';
        case KEY_SEMICOLON: return ';';
        case KEY_APOSTROPHE: return '\'';
        case KEY_GRAVE: return '`';
        case KEY_COMMA: return ',';
        case KEY_DOT: return '.';
        case KEY_SLASH: return '/';
        case KEY_LEFT: return KEY_LEFTARROW;
        case KEY_RIGHT: return KEY_RIGHTARROW;
        case KEY_UP: return KEY_UPARROW;
        case KEY_DOWN: return KEY_DOWNARROW;
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT: return KEY_RSHIFT;
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL: return KEY_RCTRL;
        case KEY_LEFTALT:
        case KEY_RIGHTALT: return KEY_RALT;
        case LINUX_KEY_CAPSLOCK: return KEY_CAPSLOCK;
        case LINUX_KEY_NUMLOCK: return KEY_NUMLOCK;
        case LINUX_KEY_SCROLLLOCK: return KEY_SCRLCK;
        case LINUX_KEY_PAUSE: return KEY_PAUSE;
        case LINUX_KEY_INSERT: return KEY_INS;
        case LINUX_KEY_DELETE: return KEY_DEL;
        case LINUX_KEY_HOME: return KEY_HOME;
        case LINUX_KEY_END: return KEY_END;
        case LINUX_KEY_PAGEUP: return KEY_PGUP;
        case LINUX_KEY_PAGEDOWN: return KEY_PGDN;
        case LINUX_KEY_F1: return KEY_F1;
        case LINUX_KEY_F2: return KEY_F2;
        case LINUX_KEY_F3: return KEY_F3;
        case LINUX_KEY_F4: return KEY_F4;
        case LINUX_KEY_F5: return KEY_F5;
        case LINUX_KEY_F6: return KEY_F6;
        case LINUX_KEY_F7: return KEY_F7;
        case LINUX_KEY_F8: return KEY_F8;
        case LINUX_KEY_F9: return KEY_F9;
        case LINUX_KEY_F10: return KEY_F10;
        case LINUX_KEY_F11: return KEY_F11;
        case LINUX_KEY_F12: return KEY_F12;
        case KEY_KP0: return KEYP_0;
        case KEY_KP1: return KEYP_1;
        case KEY_KP2: return KEYP_2;
        case KEY_KP3: return KEYP_3;
        case KEY_KP4: return KEYP_4;
        case KEY_KP5: return KEYP_5;
        case KEY_KP6: return KEYP_6;
        case KEY_KP7: return KEYP_7;
        case KEY_KP8: return KEYP_8;
        case KEY_KP9: return KEYP_9;
        case KEY_KPDOT: return KEYP_PERIOD;
        case KEY_KPPLUS: return KEYP_PLUS;
        case KEY_KPMINUS: return KEYP_MINUS;
        case KEY_KPASTERISK: return KEYP_MULTIPLY;
        case KEY_KPSLASH: return KEYP_DIVIDE;
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

static int MouseButton(unsigned int code)
{
    switch (code)
    {
        case BTN_LEFT: return 0;
        case BTN_RIGHT: return 1;
        case BTN_MIDDLE: return 2;
        case BTN_SIDE: return 3;
        case BTN_EXTRA: return 4;
        case BTN_FORWARD: return 5;
        case BTN_BACK: return 6;
        case BTN_TASK: return 7;
        default: return -1;
    }
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

static void HandleInputEvent(input_device_t *device,
                             const struct input_event *input,
                             int *mouse_dx, int *mouse_dy,
                             boolean *buttons_changed)
{
    event_t event;

    if (input->type == EV_KEY && device->keyboard)
    {
        int key = TranslateKey(input->code);

        if (input->code == KEY_LEFTSHIFT || input->code == KEY_RIGHTSHIFT)
        {
            unsigned int mask = input->code == KEY_LEFTSHIFT ? 1U : 2U;

            if (input->value)
            {
                shift_mask |= mask;
            }
            else
            {
                shift_mask &= ~mask;
            }
        }

        if (key != 0 && input->value != 0)
        {
            event.type = ev_keydown;
            event.data1 = key;
            event.data2 = (shift_mask != 0 && key >= 32 && key <= 126)
                        ? ShiftCharacter(key) : key;
            event.data3 = 0;
            D_PostEvent(&event);
        }
        else if (key != 0 && input->value == 0)
        {
            event.type = ev_keyup;
            event.data1 = key;
            event.data2 = 0;
            event.data3 = 0;
            D_PostEvent(&event);
        }
    }
    else if (device->mouse && input->type == EV_REL)
    {
        if (input->code == REL_X)
        {
            *mouse_dx += input->value;
        }
        else if (input->code == REL_Y)
        {
            *mouse_dy += input->value;
        }
    }
    else if (device->mouse && input->type == EV_KEY)
    {
        int button = MouseButton(input->code);

        if (button >= 0 && (input->value == 0 || input->value == 1))
        {
            unsigned int mask = 1U << button;
            unsigned int old_state = mouse_button_state;

            if (input->value)
            {
                mouse_button_state |= mask;
            }
            else
            {
                mouse_button_state &= ~mask;
            }
            *buttons_changed |= old_state != mouse_button_state;
        }
    }
    else if (device->mouse && input->type == EV_SYN
          && input->code == SYN_REPORT)
    {
        PostMouseEvent(*mouse_dx, *mouse_dy, *buttons_changed);
        *mouse_dx = *mouse_dy = 0;
        *buttons_changed = false;
    }
}

static boolean DeviceCapabilities(int fd, boolean *keyboard, boolean *mouse)
{
    unsigned long ev_bits[(EV_MAX + BITS_PER_LONG) / BITS_PER_LONG] = {0};
    unsigned long key_bits[(KEY_MAX + BITS_PER_LONG) / BITS_PER_LONG] = {0};
    unsigned long rel_bits[(REL_MAX + BITS_PER_LONG) / BITS_PER_LONG] = {0};

    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0)
    {
        return false;
    }

    if (TEST_BIT(EV_KEY, ev_bits))
    {
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0)
        {
            return false;
        }
    }

    if (TEST_BIT(EV_REL, ev_bits))
    {
        if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits) < 0)
        {
            return false;
        }
    }

    *keyboard = TEST_BIT(EV_KEY, ev_bits)
             && TEST_BIT(KEY_A, key_bits)
             && TEST_BIT(LINUX_KEY_ENTER, key_bits);
    *mouse = TEST_BIT(EV_REL, ev_bits)
          && TEST_BIT(REL_X, rel_bits)
          && TEST_BIT(REL_Y, rel_bits)
          && TEST_BIT(BTN_LEFT, key_bits);

    return *keyboard || *mouse;
}

int I_InitInput(void)
{
    DIR *input_dir = opendir("/dev/input");
    struct dirent *entry;
    boolean found_keyboard = false;
    boolean found_mouse = false;

    if (input_dir == NULL)
    {
        I_Error("Cannot open /dev/input: %s", strerror(errno));
        return 0;
    }

    while ((entry = readdir(input_dir)) != NULL
        && num_devices < MAX_INPUT_DEVICES)
    {
        char path[PATH_MAX];
        boolean keyboard, mouse;
        int fd;

        if (strncmp(entry->d_name, "event", 5) != 0)
        {
            continue;
        }

        if (snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name)
            >= (int) sizeof(path))
        {
            continue;
        }

        fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
        {
            continue;
        }

        if (!DeviceCapabilities(fd, &keyboard, &mouse))
        {
            close(fd);
            continue;
        }

        devices[num_devices].fd = fd;
        devices[num_devices].keyboard = keyboard;
        devices[num_devices].mouse = mouse;
        ++num_devices;
        found_keyboard |= keyboard;
        found_mouse |= mouse;
        fprintf(stderr, "evdev: using %s%s%s\n", path,
                keyboard ? " keyboard" : "", mouse ? " mouse" : "");
    }

    closedir(input_dir);

    if (!found_keyboard)
    {
        I_Error("No readable evdev keyboard found under /dev/input");
        return 0;
    }

    if (!found_mouse)
    {
        fprintf(stderr, "evdev: no readable mouse found under /dev/input\n");
    }

    return 1;
}

void I_GetEvent(void)
{
    int i;

    for (i = 0; i < num_devices; ++i)
    {
        struct input_event input;
        int mouse_dx = 0, mouse_dy = 0;
        boolean buttons_changed = false;

        for (;;)
        {
            ssize_t bytes = read(devices[i].fd, &input, sizeof(input));

            if (bytes == (ssize_t) sizeof(input))
            {
                HandleInputEvent(&devices[i], &input, &mouse_dx, &mouse_dy,
                                 &buttons_changed);
            }
            else if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
                break;
            }
            else if (bytes < 0 && errno == EINTR)
            {
                continue;
            }
            else if (bytes == 0 || (bytes < 0 && errno == ENODEV))
            {
                close(devices[i].fd);
                devices[i].fd = -1;
                break;
            }
            else if (bytes < 0)
            {
                fprintf(stderr, "evdev: read failed: %s\n", strerror(errno));
                close(devices[i].fd);
                devices[i].fd = -1;
                break;
            }
            else
            {
                fprintf(stderr, "evdev: received incomplete input event\n");
                break;
            }
        }

        if (devices[i].fd >= 0 && devices[i].mouse
         && (mouse_dx != 0 || mouse_dy != 0 || buttons_changed))
        {
            PostMouseEvent(mouse_dx, mouse_dy, buttons_changed);
        }
    }
}

void I_ShutdownInput(void)
{
    int i;

    for (i = 0; i < num_devices; ++i)
    {
        if (devices[i].fd >= 0)
        {
            close(devices[i].fd);
            devices[i].fd = -1;
        }
    }

    num_devices = 0;
}

void kbd_shutdown(void)
{
    I_ShutdownInput();
    exit(0);
}

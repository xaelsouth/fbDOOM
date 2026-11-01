//
// On-screen frames-per-second counter (see i_fps.h).
//

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "i_fps.h"
#include "i_video.h"
#include "m_argv.h"

#define GLYPH_W 3
#define GLYPH_H 5
#define SCALE 2
#define WHITE 4
#define BLACK 0

// 3x5 digits, one row per byte, bit 2 is the left column.
static const byte digits[10][GLYPH_H] =
{
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7},
    {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1},
    {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
};

static int enabled = -1;
static int frames;
static double window_start;
static int shown_fps;

static double Now(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void FillRect(byte *frame, int x, int y, int w, int h, byte color)
{
    int row;

    for (row = y; row < y + h; ++row)
    {
        memset(frame + row * SCREENWIDTH + x, color, w);
    }
}

static void DrawNumber(byte *frame, int value)
{
    char text[8];
    int length = snprintf(text, sizeof(text), "%d", value);
    int cell = (GLYPH_W + 1) * SCALE;
    int i, row, col;

    FillRect(frame, 0, 0, length * cell + SCALE, (GLYPH_H + 1) * SCALE,
             BLACK);

    for (i = 0; i < length; ++i)
    {
        const byte *glyph = digits[text[i] - '0'];

        for (row = 0; row < GLYPH_H; ++row)
        {
            for (col = 0; col < GLYPH_W; ++col)
            {
                if (glyph[row] & (1 << (GLYPH_W - 1 - col)))
                {
                    FillRect(frame, SCALE + i * cell + col * SCALE,
                             SCALE / 2 + row * SCALE, SCALE, SCALE, WHITE);
                }
            }
        }
    }
}

void I_FPS_Frame(byte *frame)
{
    double now;

    if (enabled < 0)
    {
        enabled = M_CheckParm("-showfps") > 0;
        window_start = Now();
    }
    if (!enabled || frame == NULL)
    {
        return;
    }

    ++frames;
    now = Now();
    if (now - window_start >= 1.0)
    {
        shown_fps = (int) (frames / (now - window_start) + 0.5);
        frames = 0;
        window_start = now;
    }

    DrawNumber(frame, shown_fps);
}

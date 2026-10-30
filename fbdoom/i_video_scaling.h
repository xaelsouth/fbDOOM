#ifndef I_VIDEO_SCALING_H
#define I_VIDEO_SCALING_H

#include "i_video.h"

typedef struct
{
    int x;
    int y;
    int width;
    int height;
} video_viewport_t;

static int I_CalculateVideoViewport(int output_width, int output_height,
                                    int stretch, video_viewport_t *viewport)
{
    if (output_width <= 0 || output_height <= 0)
    {
        return 0;
    }

    if (stretch)
    {
        viewport->x = 0;
        viewport->y = 0;
        viewport->width = output_width;
        viewport->height = output_height;
    }
    else if ((long long) output_width * 3
          <= (long long) output_height * 4)
    {
        viewport->width = output_width;
        viewport->height = (output_width * 3 + 2) / 4;
        viewport->x = 0;
        viewport->y = (output_height - viewport->height) / 2;
    }
    else
    {
        viewport->height = output_height;
        viewport->width = (output_height * 4 + 1) / 3;
        viewport->x = (output_width - viewport->width) / 2;
        viewport->y = 0;
    }

    return viewport->width > 0 && viewport->height > 0;
}

static int I_MapOutputToSource(int output_coordinate, int output_size,
                               int source_size)
{
    long long coordinate;

    coordinate = ((long long) output_coordinate * 2 + 1) * source_size;
    coordinate /= (long long) output_size * 2;

    if (coordinate >= source_size)
    {
        coordinate = source_size - 1;
    }

    return (int) coordinate;
}

#endif

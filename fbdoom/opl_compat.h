// Small compatibility layer for the Chocolate Doom MIDI/MUS code.
#ifndef OPL_COMPAT_H
#define OPL_COMPAT_H

#include <stdlib.h>

#define PACKED_STRUCT(...) struct __VA_ARGS__ __attribute__((packed))

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define SDL_SwapBE16(x) ((unsigned short) (x))
#define SDL_SwapBE32(x) ((unsigned int) (x))
#else
#define SDL_SwapBE16(x) __builtin_bswap16(x)
#define SDL_SwapBE32(x) __builtin_bswap32(x)
#endif

static inline void *I_Realloc(void *ptr, size_t size)
{
    void *r = realloc(ptr, size);

    if (r == NULL && size != 0)
    {
        abort();
    }
    return r;
}

#endif

//
// On-screen frames-per-second counter, enabled with -showfps.
//

#ifndef I_FPS_H
#define I_FPS_H

#include "doomtype.h"

// Counts one presented frame and, if -showfps was given, draws the current
// rate into the top-left corner of the 320x200 indexed frame.
void I_FPS_Frame(byte *frame);

#endif

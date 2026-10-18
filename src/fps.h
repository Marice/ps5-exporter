/* Frames per second of the running game, from the display's flip counter. */
#ifndef FPS_H
#define FPS_H

#include "buf.h"

/* Starts the sampling thread (once a second). */
void fps_start(void);

/* Appends ps5_fps while a recent sample exists (a game runs). */
void metrics_fps(Buf* b);

#endif

/* Genesis Plus GX core setup shared by our programs (svptrace, vrr). */
#ifndef VRR_CORE_H
#define VRR_CORE_H

#include "shared.h"

/* Player 1 buttons (INPUT_* bits), handed to the core every frame. */
extern uint16 core_pad;

/* Load the ROM and power on. fps = 0 makes audio follow the real 59.92 Hz,
   otherwise samples per frame = samplerate / fps (see audio_set_rate).
   Returns 0 on success, prints the reason and returns -1 otherwise. */
int core_init(const char *rom, int samplerate, double fps);

/* Buttons for an input script such as "SB" (any of S A B C U D L R X Y Z, M for Mode; "" =
   none). Exits on an unknown letter. */
uint16 core_parse_keys(const char *keys);

#endif

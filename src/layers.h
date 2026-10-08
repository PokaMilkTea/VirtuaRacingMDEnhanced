/* The VDP's layers kept apart: GPGX merges plane B, plane A and the sprites into one pixel
   per dot, so we render our own copy of each layer, line by line, at the moment GPGX renders
   that line (same VRAM, scroll, sprite list and palette as the original frame). The frame
   can then be composited with plane A's 3D replaced by the HLE renderer. */
#ifndef VRR_LAYERS_H
#define VRR_LAYERS_H

#include "shared.h"

#define LAYERS_W 320
#define LAYERS_H 240

/* The four bytes kept for each pixel, in this order */
enum {
  LAYER_PLANE_B,     /* plane B */
  LAYER_PLANE_A,     /* plane A, including the window */
  LAYER_SPRITES,     /* the frontmost sprite pixel */
  LAYER_BACKDROP,    /* the backdrop colour (palette index), shown where all three are clear */
  LAYER_COUNT
};

/* A layer byte: bit 7 = priority, bits 4-5 = palette line, bits 0-3 = colour (0 = transparent).
   The priority bit is kept on transparent plane pixels too: pixels replaced by the HLE need it. */
#define LAYER_PRIORITY 0x80
#define LAYER_COLOUR   0x0F

/* Sprite pixels only: the sprite sits at a side of the screen, away from its middle. In a
   race these are the HUD's left and right parts, which widescreen moves out to the edges. */
#define LAYER_SIDE_SPRITE 0x40

typedef struct {
  uint8 px[LAYERS_H][LAYERS_W][LAYER_COUNT];
  uint16 cram[LAYERS_H][64];     /* CRAM (9-bit BBBGGGRRR) as it was on each line */
  int16 hscroll_b[LAYERS_H];     /* plane B's horizontal scroll on each line */
} layers_t;

extern layers_t layers;

/* Start rendering layers alongside GPGX; call after core_init. */
void layers_init(void);

#endif

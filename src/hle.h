/* HLE of the SVP's commands for the races (2, 3, and the replays A, B), the SEGA logo (5) and
   the title (6-8): builds
   the 3D draw list from the model slots in SVP DRAM and the model/track data in ROM, without
   using the SVP's own output.
   The math follows docs/svp-commands.md sections 6.8-6.13. */
#ifndef VRR_HLE_H
#define VRR_HLE_H

#include "shared.h"

#define HLE_MAX_POLYS 2048

/* The SVP commands the HLE draws (hle_frame.command; docs/svp-commands.md 1.4) */
#define HLE_COMMAND_RACE       2
#define HLE_COMMAND_FINISH     3    /* the race goes on past the finish or time up, with slot 57
                                       (the result's 3D text) over it */
#define HLE_COMMAND_SEGA_LOGO  5
#define HLE_COMMAND_TITLE      6    /* the title demo, and the transition that closes it */
#define HLE_COMMAND_VR_PICTURE 7    /* the "V.R." picture, flashed between the title demo's images */
#define HLE_COMMAND_RANKING    8    /* the title demo with a course ranking over it */
#define HLE_COMMAND_REPLAY     0xA  /* the replay after a race, with the chase camera */
#define HLE_COMMAND_REPLAY_FIXED 0xB  /* the replay seen from a camera standing by the track */

typedef struct {
  float vertex[4][3];        /* camera space: x right, y up, z forward (in the in-car view's
                                second pass, in 1/32 world units) */
  int16 svp_screen[4][2];    /* the SVP's integer projection of the vertices (x right, y up,
                                origin at the buffer's 129, 96); only set when has_svp_screen */
  int has_svp_screen;        /* no vertex is behind the near plane and the SVP's projection
                                did not overflow */
  int vertex_count;          /* 3 or 4 */
  int front_unrounded;       /* it faces the camera with the unrounded projection */
  int front_on_svp_screen;   /* ... on the SVP's integer screen, where a polygon with almost no
                                area can come out the other way (the SVP then drops it). Drawn
                                with the SVP's vertices, a polygon shows when this is set,
                                otherwise when front_unrounded is, so it does not come and go
                                as a sliver at high resolution. */
  float near_z;              /* the near plane it is clipped at, camera-space z (it depends on
                                what the polygon belongs to, as in the SVP) */
  uint16 header;             /* colour byte (two palette indices) and flag byte, as in the model data */
  int depth_bucket;          /* larger = farther */
} hle_poly;

#define HLE_W 256                 /* the SVP's tile buffer */
#define HLE_H 192

typedef struct {
  int valid;
  int poly_count;
  hle_poly polys[HLE_MAX_POLYS];  /* in drawing order: far to near */
  uint8 sprites[HLE_H][HLE_W];    /* the SVP's 2D sprites (text, digits), drawn over the 3D:
                                     palette line 0 index, 0 = none */
  double camera_yaw;              /* camera yaw (slot 0 word 5), 4096 to a turn; 0 without a camera */
  int command;                    /* the SVP command it draws (HLE_COMMAND_...) */
  int sprites_are_picture;        /* the sprites are rows of a picture as wide as the buffer (the
                                     title's): a wider frame extends them from their edges */
  unsigned image_number;          /* different for every image built, so a display can count the
                                     images it shows a second */
} hle_frame;

/* Watch the SVP command block; call after core_init. */
void hle_init(void);

/* Call after every emulated frame: when the game flips plane A to the other buffer, the last
   draw list the SVP finished for that buffer becomes the one on screen. The SVP's DRAM
   buffer is reused for the next command while VRAM still shows the copied image, so the
   list has to be latched at the flip, not looked up by buffer. */
void hle_frame_done(void);

/* The draw list for the 3D image on screen, or NULL when it was drawn by a command the HLE
   does not handle (menus, ...) or plane A shows something else than the SVP's image (the
   game's own 2D screens); it handles races (commands 2 and 3, the replays A and B), the SEGA
   logo (5) and the title (6-8). */
const hle_frame *hle_displayed(void);

/* The race's 3D for smooth motion (docs/game-loop.md). The game logic runs once per command,
   a tick of 4 frames, so the model slots of the last two commands are interpolated by the time
   since the newer one was sent. sub_frame says how far the display is past the last emulated
   frame (0 <= sub_frame < 1; 0.5 on the second refresh of a 120 Hz display). frames_per_image
   caps the rate: 0 a new image every refresh, 1 one per emulated frame (60 fps), 2 one every
   two frames (30 fps, two images a 4-frame tick).
   This runs ahead of hle_displayed(), which shows a command about 9 frames after it is sent.
   Returns NULL unless the last two commands the HLE drew were the same one or show the same
   race (2, 3, A and B: the race going on into its finish, the replay's cameras). When the
   game
   moves on to a command the HLE does not draw, the last of them is held until the display
   flips to the new image. */
const hle_frame *hle_smooth(double sub_frame, int frames_per_image);

/* Far view (default off: as the console): besides the 8 track sections the game picks, draw 16
   more ahead of them and 3 behind (for the sides of a wide view), keep polygons beyond the
   SVP's depth limit, and draw the opponents the game leaves out for being too far away.
   Applies to the draw lists built from then on. */
void hle_set_far_view(int enabled);

/* Forget the draw lists and the ticks kept for smooth motion, after the core's state was
   loaded: they belong to the frames before. The 3D shows the SVP's image until the HLE has
   drawn and latched its own again, about two ticks later. */
void hle_reset(void);

/* Which SVP tile buffer is on screen: 0 = 300000, 1 = 306000 (from plane A's vscroll). */
int hle_displayed_buffer(void);

#endif

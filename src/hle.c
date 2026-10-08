/* HLE of the SVP's command 2 (race) and command 5 (the SEGA logo). See hle.h and
   docs/svp-commands.md.
   The slot matrices, camera position and vertex transforms reproduce the SVP's integer
   arithmetic bit for bit (verified with tools/svplog.py); projection is left to the GPU.
   Alongside, every transform is also computed without rounding, for drawing above the SVP's
   resolution; all decisions (culling, depth buckets) still come from the integer results.

   Frame interpolation (hle_smooth) runs the same build on slot words interpolated between
   two commands. Those can have fractions: the integer path rounds them, the unrounded path
   uses them as they are. With whole numbers both give exactly what they gave before.

   Numbers in comments such as 0067 or 19B5 @0096 are the SVP program addresses each function
   reproduces (docs/svp-commands.md section 5). */
#include <math.h>
#include "hle.h"

#define SIN_TABLE    0x2845    /* program-memory word address: sin(i * 2pi / 4096) * 0x4000 */
#define RECIP_TABLE  0x5845    /* program-memory word address: 32768 / n */
#define NEAR_LIMIT   0x400     /* z at or above this divides by RECIP_TABLE[z >> 5] (docs 6.8) */
#define MAX_BUCKET   2047      /* depth buckets 310000-31FFFF; farther polygons are dropped */
#define FAR_MAX_BUCKET 8703    /* far view: the largest bucket a 16-bit z can give, (32767 >> 2) + 512 */
#define SECTIONS_FAR   16      /* far view: track sections drawn beyond the game's 8 on a side the
                                  camera looks towards, about 15000 world units more than its 6600 */
#define SECTIONS_SIDE  3       /* on a side it looks away from: enough for the sides of a wide view */
#define FACING_AWAY   (-0.3)   /* a side counts as looked towards when its section is less than about
                                  107 degrees from the camera's view (so turned sideways, both do) */
#define IN_CAR_SCALE   32      /* the in-car view works out slots 1-5 in 1/32 world units */

#define DRAM_WORD(address)  (*(uint16 *)(svp->dram + ((address) & 0x1fffe)))
#define ROM_WORD(word)      (((uint16 *)cart.rom)[(word) & 0xfffff])   /* ROM word at a word address */
#define DRAM_SLOT(s, i)     ((int16)DRAM_WORD(0x30EC00 + (s) * 0x40 + (i) * 2))

#define SLOT_COUNT     64
#define SLOT_WORDS     32      /* 0x40 bytes per model slot */
#define RAM_WORD(address)   (*(uint16 *)&work_ram[(address) & 0xFFFE])   /* 68k work RAM */

/* Far view: the opponents the game does not draw (too far away) are drawn from its own records
   of them. They are kept as extra rows after the slots, so that they are snapshotted and
   interpolated like the slots. */
#define OPPONENTS             15
#define FAR_CAR_ROW(n)        (SLOT_COUNT + (n))
#define ROWS                  (SLOT_COUNT + OPPONENTS)
#define CAR_RECORDS           0x9100     /* 68k RAM: a 256-byte record per opponent (see read_far_car) */
#define CAR_MODELS            0xBC428    /* ROM byte address: per colour group (record +C6, 0x10 apart)
                                            a long per detail level 1-3 (+4, +8, +C) */
#define MAX_FAR_CAR_DISTANCE  24000      /* farther than this the 16-bit arithmetic could wrap */
#define MAX_TICK_MOVE  1024    /* an object that moves farther than this in one tick is taken
                                  to be a different object (the most seen in a race is 188) */
#define MAX_CAMERA_TURN  384   /* a camera that turns more than this in one tick (4096 to a
                                  turn), or whose offset (slot 0 words 1-3) changes by more */
#define MAX_CAMERA_SHIFT  150  /* than this, has cut to another view, as in the title demo.
                                  Otherwise the most seen is 216 and 34 in a race, 186 and 26
                                  in the title demo; its cuts change them by 645 to 2046, or
                                  the offset by 179 to 280. A race's change of view (X, Y, Z)
                                  is no cut: the camera glides there, its offset moving by up
                                  to 118 a tick. Nor is the SEGA logo's S (slot 0 of command
                                  5), which flies in by 59 to 122 a tick. */

/* The model slot words the build reads. For a command they are copied from DRAM (whole
   numbers); for an interpolated frame they lie between two commands' values. */
static double slot_words[ROWS][SLOT_WORDS];

/* Round to the nearest whole number and wrap it to 16 bits, like the SVP's arithmetic */
static int16 round_to_int16(double value)
{
  return (int16)(long)floor(value + 0.5);
}

/* A slot word as the SVP would see it (rounded), for the integer path and for words that are
   not numbers to interpolate (flags, pointers, indices) */
#define SLOT(s, i)       round_to_int16(slot_words[s][i])

/* The same word unrounded, for the unrounded (float) path */
#define SLOT_EXACT(s, i) (slot_words[s][i])

/* A transform: 3 rotation rows and a translation row. fixed is the SVP's version in fixed
   point; exact is the same transform without rounding (rotation rows of length 1.0,
   translation in world units). */
typedef struct {
  int16 fixed[4][3];
  double exact[4][3];
} transform;

/* The near plane depends on what a model belongs to: the SVP sets ram0:f4 before each group
   (1957-19a6, and 1935-1952 for the in-car view's second pass) to a value plus ram1:10, which
   comes from 30FE12 and is 0 in a race. A polygon with every vertex nearer is dropped and one
   crossing it is clipped at it; the clipped vertices count at that z for the depth key
   (19B5 @0351). */
typedef struct {
  int near_z;      /* camera-space z the HLE clips at */
  int clipped_z;   /* the z a vertex nearer than near_z counts at for the depth key */
} model_group;

/* The SVP clips the track at 16. The HLE clips it at 8 and keeps the SVP's 16 for the depth
   key, which matches the SVP's images better: what lies in between is below the screen, and
   polygons reaching into it keep the SVP's integer vertices at 1x (clipped by the GPU, thin
   road lines land a pixel off). */
static const model_group TRACK_GROUP    = { 8, 16 };
static const model_group PLAYER_GROUP   = { 2, 2 };     /* the player's car and slots 2-10 */
static const model_group OPPONENT_GROUP = { 6, 6 };     /* the opponents, slot 56, far cars */
static const model_group IN_CAR_GROUP   = { 16, 16 };   /* the in-car view's second pass, slots
                                                           1-5, in 1/32 world units */
static const model_group FINISH_TEXT_GROUP = { 2, 2 };  /* command 3's slot 57 (18cd), in 1/32
                                                           world units */
#define FINISH_TEXT_SLOT 57

/* Per SVP tile buffer: the list of the latest command, and the latest one the SVP has
   finished drawing, which is what the game copies to VRAM and flips plane A to (docs 1.3).
   Usually that is the command before the latest one; when the SVP falls behind (race starts
   on the harder courses) the game flips first and sends the next command afterwards. */
static hle_frame building[2], finished[2];
static int svp_busy_buffer = -1;   /* buffer the SVP is drawing into, -1 when idle */
static hle_frame shown;            /* latched when plane A flips to a buffer */
static int shown_buffer = -1;
static int svp_on_screen;          /* plane A shows the SVP's buffer (plane_a_shows_svp_buffer) */

/* For interpolation (hle_smooth): the model slots as they were when each of the last two
   commands was sent. The game logic runs once per command, so each one is a complete tick. */
typedef struct {
  int16 words[ROWS][SLOT_WORDS];      /* the slots, then the far cars */
  int frame;        /* the frame the command was sent in (a value of frames_done) */
  int command;      /* the command it was sent with (HLE_COMMAND_RACE, ...) */
  int in_car_view;  /* 30FE0E set: slots 1-5 drawn in a second pass (see build_draw_list) */
  int picture_rows; /* command 6: 30FE10, the rows of the title picture closing in from above
                       and from below (see draw_title_sprites) */
} slot_snapshot;

static slot_snapshot snapshots[2];

/* Far view (hle_set_far_view): more track sections and the far cars are drawn; the farthest
   depth bucket kept */
static int far_view = 0;
static int max_bucket = MAX_BUCKET;
static int newest_snapshot;          /* index into snapshots of the newer one */
static int newest_buffer = -1;       /* tile buffer whose draw list belongs to newest_snapshot */
static int frames_done;              /* emulated frames finished so far */
static int in_car_view;              /* of the command being built, like slot_words */
static int command_drawn;            /* the command being built, like slot_words */
static int picture_rows;             /* of the command being built, like slot_words */
static int in_car_flag;              /* the SVP's ram1:c8 after the last command (command_in_car_flag) */
static unsigned images_built;        /* numbers the images (hle_frame.image_number) */

static void copy_frame(hle_frame *to, const hle_frame *from)
{
  to->valid = from->valid;
  to->poly_count = from->poly_count;
  to->camera_yaw = from->camera_yaw;
  to->command = from->command;
  to->sprites_are_picture = from->sprites_are_picture;
  to->image_number = from->image_number;
  memcpy(to->polys, from->polys, from->poly_count * sizeof(hle_poly));
  memcpy(to->sprites, from->sprites, sizeof(from->sprites));
}

/* The SVP has finished the command it was drawing: its list is now the finished one for
   that buffer */
static void svp_done(void)
{
  if (svp_busy_buffer < 0) return;
  copy_frame(&finished[svp_busy_buffer], &building[svp_busy_buffer]);
  svp_busy_buffer = -1;
}

/* ---- SVP fixed-point helpers ------------------------------------------------------------- */

/* The SVP's multiply-accumulate chain (mld / mpya, overlays 15B5 and 19B5): the 32-bit sum of
   2 * a[i] * b[i], shifted left, and its high word */
static int16 svp_multiply_add(const int16 *a, const int16 *b, int count, int shift)
{
  uint32 sum = 0;
  int i;
  for (i = 0; i < count; i++) sum += (uint32)(2LL * a[i] * b[i]);
  return (int16)((sum << shift) >> 16);
}

/* a * b + c * d the SVP's way */
static int16 svp_multiply_add_2(int16 a, int16 b, int16 c, int16 d)
{
  int16 left[2] = { a, c }, right[2] = { b, d };
  return svp_multiply_add(left, right, 2, 1);
}

/* Sine and cosine of a 12-bit angle from the SVP's table, 0x4000 = 1.0 (00a8) */
static void svp_sin_cos(int16 angle, int16 *sine, int16 *cosine)
{
  *sine = (int16)ROM_WORD(SIN_TABLE + (angle & 0xFFF));
  *cosine = (int16)ROM_WORD(SIN_TABLE + ((angle + 0x400) & 0xFFF));
}

/* Identity with the SVP's scale for 1.0: 0x4000 for the camera, 0x80 for objects (00b7 / 00bf) */
static void set_identity(transform *m, int16 scale)
{
  memset(m, 0, sizeof(*m));
  m->fixed[0][0] = m->fixed[1][1] = m->fixed[2][2] = scale;
  m->exact[0][0] = m->exact[1][1] = m->exact[2][2] = 1;
}

/* The unrounded rotation of columns i and j: ci' = ci cos + cj sin, cj' = cj cos - ci sin.
   The angle is in the game's units, 4096 to a full turn. */
static void rotate_exact(double m[4][3], double angle, int i, int j)
{
  double radians = angle * (2 * 3.14159265358979323846 / 4096);
  double sine = sin(radians), cosine = cos(radians);
  int row;
  for (row = 0; row < 4; row++)
  {
    double ci = m[row][i], cj = m[row][j];
    m[row][i] = ci * cosine + cj * sine;
    m[row][j] = cj * cosine - ci * sine;
  }
}

/* Rotations about the X, Y and Z axes, both versions (0067, 007c, 0093). The fixed-point one
   uses the angle rounded to a whole number, as the SVP sees it. */
static void rotate_x(transform *m, double angle)
{
  int16 sine, cosine;
  int row;
  svp_sin_cos(round_to_int16(angle), &sine, &cosine);
  for (row = 0; row < 4; row++)
  {
    int16 y = m->fixed[row][1], z = m->fixed[row][2];
    m->fixed[row][1] = svp_multiply_add_2(y, cosine, z, sine);
    m->fixed[row][2] = svp_multiply_add_2(y, -sine, z, cosine);
  }
  rotate_exact(m->exact, angle, 1, 2);
}

static void rotate_y(transform *m, double angle)
{
  int16 sine, cosine;
  int row;
  svp_sin_cos(round_to_int16(angle), &sine, &cosine);
  for (row = 0; row < 4; row++)
  {
    int16 x = m->fixed[row][0], z = m->fixed[row][2];
    m->fixed[row][0] = svp_multiply_add_2(x, cosine, z, -sine);
    m->fixed[row][2] = svp_multiply_add_2(x, sine, z, cosine);
  }
  rotate_exact(m->exact, angle, 2, 0);
}

static void rotate_z(transform *m, double angle)
{
  int16 sine, cosine;
  int row;
  svp_sin_cos(round_to_int16(angle), &sine, &cosine);
  for (row = 0; row < 4; row++)
  {
    int16 x = m->fixed[row][0], y = m->fixed[row][1];
    m->fixed[row][0] = svp_multiply_add_2(x, cosine, y, sine);
    m->fixed[row][1] = svp_multiply_add_2(x, -sine, y, cosine);
  }
  rotate_exact(m->exact, angle, 0, 1);
}

/* The value with its whole part wrapped to 16 bits like round_to_int16, and its fraction kept:
   the unrounded path must wrap where the SVP's 16-bit arithmetic does */
static double wrap_like_int16(double value)
{
  double whole = floor(value + 0.5);
  return round_to_int16(value) + (value - whole);
}

/* Move by (dx, dy, dz) in world units; the fixed-point translation is in quarter units (001c) */
static void translate(transform *m, double dx, double dy, double dz)
{
  m->fixed[3][0] += 4 * round_to_int16(dx);
  m->fixed[3][1] += 4 * round_to_int16(dy);
  m->fixed[3][2] += 4 * round_to_int16(dz);
  m->exact[3][0] += wrap_like_int16(dx);
  m->exact[3][1] += wrap_like_int16(dy);
  m->exact[3][2] += wrap_like_int16(dz);
}

/* An object's identity at a scale other than 0x80, which is 1.0 for objects: the unrounded
   version scales by the same factor (the in-car view's 0x1000, 00bb, is 32) */
static void set_object_identity(transform *m, int16 scale)
{
  set_identity(m, scale);
  m->exact[0][0] = m->exact[1][1] = m->exact[2][2] = scale / 128.0;
}

/* Add (tx, ty, tz) to the translation as it is (routine 0000), where translate adds 4 times a
   world distance: camera coordinates move by a quarter of it */
static void translate_raw(transform *m, double tx, double ty, double tz)
{
  m->fixed[3][0] += round_to_int16(tx);
  m->fixed[3][1] += round_to_int16(ty);
  m->fixed[3][2] += round_to_int16(tz);
  m->exact[3][0] += wrap_like_int16(tx) / 4;
  m->exact[3][1] += wrap_like_int16(ty) / 4;
  m->exact[3][2] += wrap_like_int16(tz) / 4;
}

/* The in-car view's move by (dx, dy, dz) in world units: the SVP adds 2 * 0x40 * d, wrapped to
   16 bits, without translate's factor 4 (routine 0000 after 0194-01a9), which is 32 times
   finer */
static void translate_in_car(transform *m, double dx, double dy, double dz)
{
  m->fixed[3][0] += (int16)(128 * round_to_int16(dx));
  m->fixed[3][1] += (int16)(128 * round_to_int16(dy));
  m->fixed[3][2] += (int16)(128 * round_to_int16(dz));
  m->exact[3][0] += IN_CAR_SCALE * wrap_like_int16(dx);
  m->exact[3][1] += IN_CAR_SCALE * wrap_like_int16(dy);
  m->exact[3][2] += IN_CAR_SCALE * wrap_like_int16(dz);
}

/* ---- camera and slot matrices (phase 1, docs 6.13) ---------------------------------------- */

/* The camera's angles (slot 0 words 4-6: X pitch, Y yaw, Z roll), 4096 to a turn */
static double camera_angle_x, camera_angle_y, camera_angle_z;
static int16 camera_position[3];             /* the SVP's integer camera position in the world */
static double camera_position_fraction[3];   /* the unrounded position minus camera_position */
static transform camera_matrix;              /* world to camera; the SVP writes it to slot 0 (014a) */

static void compute_camera(void)
{
  transform inverse;
  int16 offset[3], column[3];
  double offset_exact[3];
  int k;

  camera_angle_x = SLOT_EXACT(0, 4);
  camera_angle_y = SLOT_EXACT(0, 5);
  camera_angle_z = SLOT_EXACT(0, 6);

  set_identity(&camera_matrix, 0x4000);
  rotate_y(&camera_matrix, camera_angle_y);
  rotate_x(&camera_matrix, camera_angle_x);
  rotate_z(&camera_matrix, camera_angle_z);

  /* 0350, the replay's camera standing by the track: slot 0 words 1-3 are its world position */
  if (command_drawn == HLE_COMMAND_REPLAY_FIXED)
  {
    for (k = 0; k < 3; k++)
    {
      camera_position[k] = SLOT(0, 1 + k);
      camera_position_fraction[k] = SLOT_EXACT(0, 1 + k) - camera_position[k];
    }
    return;
  }

  /* 010c: the camera's world position is the player's car (slot 1 words 1-3) plus the chase
     offset (slot 0 words 1-3, z negated) turned by the negated angles */
  set_identity(&inverse, 0x4000);
  rotate_z(&inverse, -camera_angle_z);
  rotate_x(&inverse, -camera_angle_x);
  rotate_y(&inverse, -camera_angle_y);

  offset[0] = SLOT(0, 1);
  offset[1] = SLOT(0, 2);
  offset[2] = -SLOT(0, 3);
  offset_exact[0] = SLOT_EXACT(0, 1);
  offset_exact[1] = SLOT_EXACT(0, 2);
  offset_exact[2] = -SLOT_EXACT(0, 3);

  for (k = 0; k < 3; k++)
  {
    double turned_offset;
    column[0] = inverse.fixed[0][k];
    column[1] = inverse.fixed[1][k];
    column[2] = inverse.fixed[2][k];
    camera_position[k] = svp_multiply_add(column, offset, 3, 1) + SLOT(1, 1 + k);

    turned_offset = inverse.exact[0][k] * offset_exact[0] + inverse.exact[1][k] * offset_exact[1]
                  + inverse.exact[2][k] * offset_exact[2];
    camera_position_fraction[k] = turned_offset + SLOT_EXACT(1, 1 + k) - camera_position[k];
  }
}

/* The player's car (015a): its own angles plus the camera's, then the chase-camera offset:
   the car is always at the same place in front of the camera */
static void apply_player_frame(transform *m)
{
  rotate_z(m, SLOT_EXACT(1, 6) + camera_angle_z);
  rotate_x(m, SLOT_EXACT(1, 4) + camera_angle_x);
  rotate_y(m, SLOT_EXACT(1, 5) + camera_angle_y);
  translate(m, 0, -SLOT_EXACT(0, 2), SLOT_EXACT(0, 3));
}

/* An object at the world position in slot words 1-3, seen from the camera (0221) */
static void apply_world_position(transform *m, int slot)
{
  translate(m, SLOT_EXACT(slot, 1) - camera_position[0],
               SLOT_EXACT(slot, 2) - camera_position[1],
               SLOT_EXACT(slot, 3) - camera_position[2]);
  m->exact[3][0] -= camera_position_fraction[0];
  m->exact[3][1] -= camera_position_fraction[1];
  m->exact[3][2] -= camera_position_fraction[2];
  rotate_y(m, camera_angle_y);
  rotate_x(m, camera_angle_x);
  rotate_z(m, camera_angle_z);
}

/* An opponent's car: its own rotation (slot words 4-6), then its world position (01e4) */
static void apply_opponent_frame(transform *m, int slot)
{
  rotate_z(m, SLOT_EXACT(slot, 6));
  rotate_x(m, SLOT_EXACT(slot, 4));
  rotate_y(m, SLOT_EXACT(slot, 5));
  apply_world_position(m, slot);
}

/* A child object (a wheel): its X and Y rotation and its offset from the parent (017a / 020b) */
static void apply_child_offset(transform *m, int slot)
{
  rotate_x(m, SLOT_EXACT(slot, 4));
  rotate_y(m, SLOT_EXACT(slot, 5));
  translate(m, SLOT_EXACT(slot, 1), SLOT_EXACT(slot, 2), SLOT_EXACT(slot, 3));
}

/* Whether a command places the player's car in the world (126F @02fc: the title demo and the
   replays) rather than in front of the camera (@0240: the race and its finish) */
static int car_in_world(int command)
{
  return command == HLE_COMMAND_TITLE || command == HLE_COMMAND_RANKING ||
         command == HLE_COMMAND_REPLAY || command == HLE_COMMAND_REPLAY_FIXED;
}

/* The title demo's and the replays' variant of the slots 1-10 (126F @02fc): the player's car
   is placed like the opponents (01e4), at its own world position instead of in front of the
   camera, and slots 2-4 follow the car as an opponent's children do (020b). Slots 6-10 (tyre
   smoke and the like) take the car's place and turn, as in a race: their own words 1-6 are 0,
   and the matrix the SVP writes back into them is the car's. Slot 5 is not drawn. */
static int title_slot_matrix(int slot, transform *m)
{
  if (slot == 5) return 0;
  if (slot >= 2 && slot <= 4)
  {
    apply_child_offset(m, slot);
    apply_opponent_frame(m, 1);
  }
  else
  {
    apply_opponent_frame(m, slot >= 6 ? 1 : slot);
  }
  return 1;
}

/* The matrix for an object slot; returns 0 for slots command 2 does not draw this way.
   Slots: 1 the player's car, 2-4 its children, 6-10 effects in the car's frame, 11-55 the
   opponents in groups of three (the car, then two children), 56 a list of models. */
static int slot_matrix(int slot, transform *m)
{
  set_identity(m, 0x80);
  if (car_in_world(command_drawn) && slot >= 1 && slot <= 10)
  {
    return title_slot_matrix(slot, m);
  }
  else if (slot == 1 || (slot >= 6 && slot <= 10))
  {
    apply_player_frame(m);
  }
  else if (slot >= 2 && slot <= 4)
  {
    apply_child_offset(m, slot);
    apply_player_frame(m);
  }
  else if (slot >= 11 && slot <= 55)
  {
    int parent = 11 + 3 * ((slot - 11) / 3);
    if (!SLOT(parent, 0)) return 0;          /* phase 1 skips the whole group */
    if (slot != parent) apply_child_offset(m, slot);
    apply_opponent_frame(m, parent);
  }
  else if (slot == 56)                       /* no rotation of its own (pit crew at the start) */
  {
    apply_world_position(m, 56);
  }
  else
  {
    return 0;                                /* ponytail: slot 5 (01ca, only with 30FE0E set) not handled */
  }
  return 1;
}

/* The matrix for slots 1-5 in the in-car view (30FE0E set), in 1/32 world units; returns 0 for
   a slot it does not draw. These are the SVP's variants of 015a, 017a and 01ca (0194-01e2):
   the car turns only by its roll (its other angles are the camera's negated, so they cancel
   anyway), the wheels leave that out, and slot 5 turns by its own roll and sits at its offset
   in the car before taking the car's roll. */
static int in_car_slot_matrix(int slot, transform *m)
{
  double car_roll = SLOT_EXACT(1, 6) + camera_angle_z;

  set_object_identity(m, 0x1000);
  if (slot == 1)
  {
    rotate_z(m, car_roll);
  }
  else if (slot == 2 || slot == 3)
  {
    rotate_x(m, SLOT_EXACT(slot, 4));
    rotate_y(m, SLOT_EXACT(slot, 5));
    translate_in_car(m, SLOT_EXACT(slot, 1), SLOT_EXACT(slot, 2), SLOT_EXACT(slot, 3));
  }
  else if (slot == 5)
  {
    rotate_z(m, SLOT_EXACT(5, 6));
    translate_in_car(m, SLOT_EXACT(5, 1), SLOT_EXACT(5, 2), SLOT_EXACT(5, 3));
    rotate_z(m, car_roll);
  }
  else
  {
    return 0;    /* slot 4: phase 1 leaves it out in this view (its loop covers 2-3 only) */
  }
  translate_in_car(m, 0, -SLOT_EXACT(0, 2), SLOT_EXACT(0, 3));   /* the camera's place in the car */
  return 1;
}

/* Command 3's slot 57, the result's 3D text (146a): no camera, in 1/32 world units like the
   in-car view (00bb), turned about X by word 4 and moved by (0, word 2, word 3) as they are
   (routine 0000) */
static void finish_text_matrix(transform *m)
{
  set_object_identity(m, 0x1000);
  rotate_x(m, SLOT_EXACT(FINISH_TEXT_SLOT, 4));
  translate_raw(m, 0, SLOT_EXACT(FINISH_TEXT_SLOT, 2), SLOT_EXACT(FINISH_TEXT_SLOT, 3));
}

/* ---- vertices, depth buckets, draw list ---------------------------------------------------- */

/* An object's vertex into camera space: fixed as the SVP computes it, exact unrounded
   (19B5 @0096) */
static void transform_object_vertex(const transform *m, const int16 *vertex, int16 fixed[3], float exact[3])
{
  int k;
  for (k = 0; k < 3; k++)
  {
    int16 column[4] = { m->fixed[0][k], m->fixed[1][k], m->fixed[2][k], m->fixed[3][k] };
    int16 point[4] = { vertex[0], vertex[1], vertex[2], 0x400 };
    fixed[k] = svp_multiply_add(column, point, 4, 3);
    exact[k] = (m->exact[0][k] * vertex[0] + m->exact[1][k] * vertex[1] + m->exact[2][k] * vertex[2]) / 32
             + m->exact[3][k];
  }
}

/* A track vertex (world coordinates) into camera space: fixed as the SVP computes it, exact
   unrounded (19B5 @0074). The SVP's difference to the camera wraps around at 16 bits; the exact
   one does not, which matters only for distant sections the SVP never drew. */
static void transform_track_vertex(const int16 *vertex, int16 fixed[3], float exact[3])
{
  int k;
  int16 relative[4];      /* relative to the camera, then the SVP's 0x20 for the translation */
  int difference[3];      /* the same without the wrap-around */
  for (k = 0; k < 3; k++)
  {
    difference[k] = vertex[k] - camera_position[k];
    relative[k] = (int16)difference[k];
  }
  relative[3] = 0x20;

  for (k = 0; k < 3; k++)
  {
    int16 column[4] = { camera_matrix.fixed[0][k], camera_matrix.fixed[1][k],
                        camera_matrix.fixed[2][k], camera_matrix.fixed[3][k] };
    fixed[k] = svp_multiply_add(column, relative, 4, 1);
    exact[k] = camera_matrix.exact[0][k] * (difference[0] - camera_position_fraction[0])
             + camera_matrix.exact[1][k] * (difference[1] - camera_position_fraction[1])
             + camera_matrix.exact[2][k] * (difference[2] - camera_position_fraction[2]);
  }
}

/* An exact camera coordinate rounded and held to 16 bits, for decisions on polygons the SVP
   never drew */
static int16 clamp_to_int16(double value)
{
  double rounded = floor(value + 0.5);
  if (rounded > 32767) return 32767;
  if (rounded < -32768) return -32768;
  return (int16)rounded;
}

/* The SVP's perspective divide (19B5 @00b8): about (64x/z, 80y/z), truncated (docs 6.8). It
   multiplies by a reciprocal from a table, indexed by z, or by z / 32 when z is large. The
   bit juggling reproduces the SVP's accumulator exactly. */
static void svp_project(const int16 camera[3], int16 screen[2])
{
  int32 x = camera[0], y = camera[1], z = camera[2];   /* ram1:10 (30FE12) is 0 in a race */
  if (z >= NEAR_LIMIT)
  {
    int32 reciprocal = (int16)ROM_WORD(RECIP_TABLE + (z >> 5));
    uint32 product = (uint32)(2 * reciprocal * y);
    uint32 accumulator = ((uint32)((int32)product >> 1)) + ((uint32)(int16)((product << 1) >> 16) << 16);
    screen[1] = (int16)(accumulator >> 16);
    screen[0] = (int16)(((uint32)(2 * reciprocal * x) << 1) >> 16);
  }
  else
  {
    int32 reciprocal = (int16)ROM_WORD(RECIP_TABLE + z);
    uint32 accumulator = (uint32)(2 * reciprocal * y) << 4;
    screen[1] = (int16)(((accumulator << 2) + ((uint32)(int16)(accumulator >> 16) << 16)) >> 16);
    screen[0] = (int16)(((uint32)(2 * reciprocal * x) << 6) >> 16);
  }
}

/* The depth bucket of a polygon from its vertices' z, by the polygon's sort flag in the low 3
   bits of its header (docs 6.10): nearest, farthest or average z, some with a bias */
static int depth_bucket(uint16 header, const int16 camera[][3], int vertex_count, const model_group *group)
{
  int i, nearest = 0x7FFF, farthest = -0x8000, sum = 0;
  for (i = 0; i < vertex_count; i++)
  {
    int z = camera[i][2];
    if (z < group->near_z) z = group->clipped_z;   /* where the SVP's clipping puts it */
    if (z < nearest) nearest = z;
    if (z > farthest) farthest = z;
    sum += z;
  }
  switch (header & 7)
  {
    case 1:  return nearest >> 2;
    case 2:
    case 5:  return farthest >> 2;
    case 3:  return ((sum / vertex_count) >> 2) + 512;
    case 4:  return (farthest >> 2) + 256;
    default: return (sum / vertex_count) >> 2;     /* 0; 6 and 7 not seen yet */
  }
}

/* Backface test of 19B5 @01a5 on the projected vertices: visible if the cross term is < 0.
   The SVP tests its integer screen coordinates, so a polygon too small to have any area there
   is dropped (the SEGA logo's far pieces are full of them). With use_svp_screen the test uses
   the SVP's projection too, unless it wraps around far off screen; otherwise the unrounded
   one. A polygon crossing the near plane is tested after clipping by the SVP; the same test
   in 3D, v0 . (v1 x v2) > 0 (the triple product), does not need the projection (it agrees
   with the 2D one on every polygon in front of the camera). */
static int is_front_facing(const int16 camera[][3], int near_z, int use_svp_screen)
{
  float x[3], y[3], svp_x[3], svp_y[3];
  int i;
  for (i = 0; i < 3; i++)
  {
    int16 screen[2];
    if (camera[i][2] < near_z)
    {
      double triple = (double)camera[0][0] * ((double)camera[1][1] * camera[2][2] - (double)camera[1][2] * camera[2][1])
                    - (double)camera[0][1] * ((double)camera[1][0] * camera[2][2] - (double)camera[1][2] * camera[2][0])
                    + (double)camera[0][2] * ((double)camera[1][0] * camera[2][1] - (double)camera[1][1] * camera[2][0]);
      return triple > 0;
    }
    x[i] = 64.0f * camera[i][0] / camera[i][2];
    y[i] = 80.0f * camera[i][1] / camera[i][2];
    if (!use_svp_screen) continue;
    svp_project(camera[i], screen);
    svp_x[i] = screen[0];
    svp_y[i] = screen[1];
    if (fabs(svp_x[i] - x[i]) > 64 || fabs(svp_y[i] - y[i]) > 64) use_svp_screen = 0;   /* wrapped */
  }
  if (use_svp_screen)
    return (svp_x[1] - svp_x[2]) * (svp_y[0] - svp_y[1]) - (svp_y[1] - svp_y[2]) * (svp_x[0] - svp_x[1]) < 0;
  return (x[1] - x[2]) * (y[0] - y[1]) - (y[1] - y[2]) * (x[0] - x[1]) < 0;
}

/* Whether a quad's first three vertices are almost in a line in the model: a triangle stored
   as a quad, with two vertices a unit or so apart (four track polygons in the whole game, one
   of them a hillside on Medium). The SVP's backface
   test looks only at those three, so on such a quad it flips with every rounding and the
   polygon flickers. The area of the triangle 0-1-2 is then tiny next to that of 0-2-3. */
static int first_three_in_line(const int16 model[4][3])
{
  double first[3], other[3];
  int a;
  for (a = 0; a < 3; a++)
  {
    int b = (a + 1) % 3, c = (a + 2) % 3;
    double v1b = model[1][b] - model[0][b], v1c = model[1][c] - model[0][c];
    double v2b = model[2][b] - model[0][b], v2c = model[2][c] - model[0][c];
    double v3b = model[3][b] - model[0][b], v3c = model[3][c] - model[0][c];
    first[a] = v1b * v2c - v1c * v2b;
    other[a] = v2b * v3c - v2c * v3b;
  }
  /* squared lengths of the cross products: the first triangle under 1/16 of the other's area */
  return 256 * (first[0] * first[0] + first[1] * first[1] + first[2] * first[2]) <
         other[0] * other[0] + other[1] * other[1] + other[2] * other[2];
}

/* Every polygon of a model, transformed by m (an object) or by the camera (m = NULL, track),
   into the frame's list, culled like the SVP. A model in ROM: the polygon count - 1, then per
   polygon a header word and 3 or 4 vertices of 3 words.
   group: what the model belongs to, for its near plane.
   hle_only: a model the SVP never drew (a track section the game did not pick, a far car).
   Its distances can overflow the SVP's 16-bit arithmetic, so its culling and depth bucket come
   from the exact coordinates, and the GPU projects it. */
static void add_model(hle_frame *f, uint32 word, const transform *m, const model_group *group,
                      int hle_only)
{
  int poly_count = ROM_WORD(word) + 1, p, i;
  int near_z = group->near_z;
  word++;
  for (p = 0; p < poly_count; p++)
  {
    uint16 header = ROM_WORD(word++);
    int vertex_count = (header & 0x10) ? 3 : 4;
    int all_behind = 1;
    int front_unrounded, front_on_svp_screen;
    int16 model[4][3], camera[4][3];
    float camera_exact[4][3];
    hle_poly *out;

    for (i = 0; i < vertex_count; i++, word += 3)
    {
      int16 vertex[3];
      vertex[0] = (int16)ROM_WORD(word);
      vertex[1] = (int16)ROM_WORD(word + 1);
      vertex[2] = (int16)ROM_WORD(word + 2);
      memcpy(model[i], vertex, sizeof(vertex));
      if (m) transform_object_vertex(m, vertex, camera[i], camera_exact[i]);
      else transform_track_vertex(vertex, camera[i], camera_exact[i]);
      if (hle_only)
      {
        camera[i][0] = clamp_to_int16(camera_exact[i][0]);
        camera[i][1] = clamp_to_int16(camera_exact[i][1]);
        camera[i][2] = clamp_to_int16(camera_exact[i][2]);
      }
      if (camera[i][2] >= near_z) all_behind = 0;
    }

    /* culling, all from the integer results: behind the camera, backface (header flag 0x40),
       too far away */
    if (all_behind || f->poly_count == HLE_MAX_POLYS) continue;
    /* backface (flag 0x40), with the unrounded projection and on the SVP's integer screen:
       for a polygon with almost no area on screen the two can disagree. It is kept when either
       shows it, and each way of drawing it uses its own answer. */
    front_unrounded = 1;
    front_on_svp_screen = 1;
    if (header & 0x40)
    {
      /* unrounded, a triangle stored as a quad is tested on its real triangle (0, 2, 3), so
         that it does not flicker; on the SVP's screen it stays as the SVP tests it */
      if (vertex_count == 4 && first_three_in_line((const int16 (*)[3])model))
      {
        int16 triangle[3][3];
        memcpy(triangle[0], camera[0], sizeof(camera[0]));
        memcpy(triangle[1], camera[2], sizeof(camera[2]));
        memcpy(triangle[2], camera[3], sizeof(camera[3]));
        front_unrounded = is_front_facing((const int16 (*)[3])triangle, near_z, 0);
      }
      else front_unrounded = is_front_facing((const int16 (*)[3])camera, near_z, 0);
      front_on_svp_screen = hle_only ? front_unrounded
                                     : is_front_facing((const int16 (*)[3])camera, near_z, 1);
      if (!front_unrounded && !front_on_svp_screen) continue;
    }

    out = &f->polys[f->poly_count];
    out->depth_bucket = depth_bucket(header, (const int16 (*)[3])camera, vertex_count, group);
    if (out->depth_bucket > max_bucket) continue;
    if (out->depth_bucket < 0) out->depth_bucket = 0;

    out->header = header;
    out->vertex_count = vertex_count;
    out->near_z = near_z;
    out->front_unrounded = front_unrounded;
    out->front_on_svp_screen = front_on_svp_screen;
    out->has_svp_screen = !hle_only;
    for (i = 0; i < vertex_count; i++)
    {
      out->vertex[i][0] = camera_exact[i][0];
      out->vertex[i][1] = camera_exact[i][1];
      out->vertex[i][2] = camera_exact[i][2];
      if (camera[i][2] < near_z) out->has_svp_screen = 0;
    }
    if (out->has_svp_screen)
      for (i = 0; i < vertex_count; i++)
      {
        svp_project(camera[i], out->svp_screen[i]);
        /* far off screen the SVP's 16-bit projection wraps around (it culls such polygons
           before projecting); leave those to the GPU's clipping */
        if (fabs(out->svp_screen[i][0] - 64.0 * camera[i][0] / camera[i][2]) > 64 ||
            fabs(out->svp_screen[i][1] - 80.0 * camera[i][1] / camera[i][2]) > 64)
          out->has_svp_screen = 0;
      }
    f->poly_count++;
  }
}

/* The word address of a slot's model (or model table) in ROM: slot words 19 and 20, a
   PMC-style address (+26/+28) */
static uint32 slot_model_address(int slot)
{
  return ((uint32)(SLOT(slot, 19) & 0x1F) << 16) | (uint16)SLOT(slot, 20);
}

/* ---- SVP sprites (phase 6, overlay 229F; docs 3) ------------------------------------------ */

#define SPRITE_WORD(s, i)  DRAM_WORD(0x30FC00 + (s) * 0x10 + (i) * 2)
#define FONT_SMALL   0x780F0   /* 8x16 glyphs, 0x20 words each: digits, then ' (0B) " (0A) ... */
#define FONT_BIG     0x79130   /* 64x80 digits, 0x500 words each */
#define BCD_TABLE    0x26F3    /* program-memory word address: binary 0-99 -> BCD */

/* Rows of 4bpp pixels (4 per word, high nibble first, words_per_row words a row) to buffer
   x, y. Colour 0 stays transparent: the SVP writes them in overwrite mode.
   ponytail: clipped to the buffer; the SVP clips only the right edge, and only for types
   6-8, but the game keeps sprites inside it. */
static void sprite_blit(hle_frame *f, uint32 word, int x, int y, int words_per_row, int rows)
{
  int row, i, k;
  for (row = 0; row < rows; row++)
    for (i = 0; i < words_per_row; i++)
    {
      uint16 pixels = ROM_WORD(word + row * words_per_row + i);
      for (k = 0; k < 4; k++)
      {
        int px = x + i * 4 + k, colour = (pixels >> (12 - 4 * k)) & 15;
        if (colour && px < HLE_W && y + row < HLE_H) f->sprites[y + row][px] = colour;
      }
    }
}

/* A position word is a word offset into the tile buffer (docs 6.4): bits 13-10 the 16-line
   strip, 9-5 the 8-pixel column, 4-1 the line in the strip, 0 the 4-pixel half. */
static void sprite_position(uint16 position, int *x, int *y)
{
  *x = ((position >> 5) & 31) * 8 + (position & 1) * 4;
  *y = ((position >> 10) & 15) * 16 + ((position >> 1) & 15);
}

/* A time as six BCD digits M M ' S S " h h, 8 pixels apart (012d and its users) */
static void sprite_time(hle_frame *f, int slot, int x, int y, int clip_right)
{
  /* the eight glyphs: digit indices, or (negative) a fixed glyph: ' = 0x0B, " = 0x0A */
  static const int order[8] = { 0, 1, -0x0B, 2, 3, -0x0A, 4, 5 };
  int digits[6], i, started = 0;

  for (i = 0; i < 3; i++)
  {
    digits[i * 2] = (SPRITE_WORD(slot, 3 + i) >> 4) & 15;
    digits[i * 2 + 1] = SPRITE_WORD(slot, 3 + i) & 15;
  }
  if (digits[0] >= 0x0A && digits[0] <= 0x0C)
  {
    digits[0] += 2;                                   /* special glyphs */
  }
  else if (digits[0] == 0x0D)
  {
    digits[0] = 0;
    for (i = 1; i < 6; i++) digits[i] = 0x0D;
  }

  for (i = 0; i < 8; i++, x += 8)
  {
    int glyph = order[i] < 0 ? -order[i] : digits[order[i]];
    if (clip_right && x >= HLE_W) break;
    if (i || glyph) started = 1;                      /* no leading zero */
    if (started) sprite_blit(f, FONT_SMALL + glyph * 0x20, x, y, 2, 16);
  }
}

/* The SVP's sprites in slots first_slot to 31 (at 30FC00) into the frame's sprite image. Word 0
   of a slot is its type, word 2 its position, words 3-4 its data. */
static void draw_sprite_slots(hle_frame *f, int first_slot)
{
  int slot, x, y;
  for (slot = first_slot; slot < 32; slot++)
  {
    int type = SPRITE_WORD(slot, 0);
    uint32 data = ((uint32)(SPRITE_WORD(slot, 3) & 0x1F) << 16) | SPRITE_WORD(slot, 4);
    int clip_right = type >= 6;          /* types 6-8: an x in 4-pixel units in word 1 */

    if (clip_right)
    {
      int x_word = SPRITE_WORD(slot, 1);
      if (x_word >= 0x40) continue;      /* off the right edge */
      sprite_position(SPRITE_WORD(slot, 2) + ((x_word & 0x3E) << 4) + (x_word & 1), &x, &y);
    }
    else
    {
      sprite_position(SPRITE_WORD(slot, 2), &x, &y);
    }

    switch (type)
    {
      case 1:                            /* image: width / 4 - 1, height - 1, pixels */
      case 6:
        sprite_blit(f, data + 2, x, y, ROM_WORD(data) + 1, ROM_WORD(data + 1) + 1);
        break;
      case 3:                            /* two big digits */
      case 7:
      {
        uint16 bcd = ROM_WORD(BCD_TABLE + SPRITE_WORD(slot, 3));
        if (bcd >> 4) sprite_blit(f, FONT_BIG + (bcd >> 4) * 0x500, x, y, 16, 80);
        x += 56;
        if (!clip_right || x < HLE_W) sprite_blit(f, FONT_BIG + (bcd & 15) * 0x500, x, y, 16, 80);
        break;
      }
      case 4:                            /* time */
      case 8:
        sprite_time(f, slot, x, y, clip_right);
        break;
      default:                           /* ponytail: 2 (text), 5, 9 not seen in races */
        break;
    }
  }
}

/* The race's sprites (phase 6, 229F @0000): all 32 slots */
static void draw_sprites(hle_frame *f)
{
  memset(f->sprites, 0, sizeof(f->sprites));
  f->sprites_are_picture = 0;
  draw_sprite_slots(f, 0);
}

/* ---- the title (commands 6 and 8) --------------------------------------------------------- */

#define PICTURE_ROWS_ALL  0x60       /* the title picture covers the screen: no 3D (0afb) */
#define PICTURE_MIDDLE    0x7E61E    /* program words: the title picture (256 x 192) has its
                                        rows 0-95 before here and 96-191 from 2 words on */

/* Command 6's sprites (229F @02f7): when the title demo ends, 30FE10 counts up to 0x60 and the
   title picture closes over the 3D, its top half sliding down from above and its bottom half
   up from below, each picture_rows high. The picture has no colour 0, so although the SVP
   writes these rows without overwrite mode, they cover the 3D completely. */
static void draw_title_sprites(hle_frame *f)
{
  memset(f->sprites, 0, sizeof(f->sprites));
  f->sprites_are_picture = picture_rows != 0;
  if (!picture_rows) return;
  sprite_blit(f, PICTURE_MIDDLE - 64 * picture_rows, 0, 0, 64, picture_rows);
  sprite_blit(f, PICTURE_MIDDLE + 2, 0, HLE_H - picture_rows, 64, picture_rows);
}

#define VR_PICTURE  0x7FE22   /* program words: the "V.R." picture, 192 rows of 256 pixels */

/* Command 7 (229F @0337): only the "V.R." picture, over the whole buffer. Its rows are run-length
   coded (01d2): pairs of a count and a word of 4 pixels written that many times, until a count
   of 0. Every row is 256 pixels with no colour 0, so nothing of the buffer's old image shows. */
static void draw_vr_picture(hle_frame *f)
{
  uint32 word = VR_PICTURE;
  int row;

  f->poly_count = 0;
  f->camera_yaw = 0;
  f->command = HLE_COMMAND_VR_PICTURE;
  f->sprites_are_picture = 1;
  memset(f->sprites, 0, sizeof(f->sprites));
  for (row = 0; row < HLE_H; row++)
  {
    int x = 0;
    uint16 count;
    while ((count = ROM_WORD(word)) != 0)
    {
      uint16 pixels = ROM_WORD(word + 1);
      int k;
      word += 2;
      for (; count > 0 && x < HLE_W; count--)
        for (k = 0; k < 4; k++, x++)
          if (x < HLE_W) f->sprites[row][x] = (pixels >> (12 - 4 * k)) & 15;
    }
    word++;
  }
}

/* ---- the title's course ranking (command 8, overlay 25eb) --------------------------------- */

#define RANKING_ROWS        6         /* sprite slots 0-5, one row each */
#define RANKING_CHARACTERS  23
#define RANKING_X           32        /* the rows start at buffer position 0c80 */
#define RANKING_Y           48
#define FONT_ASCII          0x782D0   /* 8x16 glyphs from the space on, 0x20 words each */
#define RANKING_SUFFIXES    0x2730    /* program words: "st", "nd", ... two per rank */
#define RANKING_MODES       0x2764    /* program words: six characters per V.R. mode, 8 apart */

/* A ranking row as the 23 characters 002e builds from the slot's words: rank (word 1) with
   its suffix, the three-letter name (words 2-3), the time M'SS"hh (BCD in words 4-6) and the
   V.R. mode (word 7) */
static void ranking_text(int slot, char text[RANKING_CHARACTERS])
{
  int rank = SPRITE_WORD(slot, 1);
  uint16 rank_bcd = ROM_WORD(BCD_TABLE + rank);
  uint16 name = SPRITE_WORD(slot, 2), name_end = SPRITE_WORD(slot, 3);
  uint16 seconds = SPRITE_WORD(slot, 5), hundredths = SPRITE_WORD(slot, 6);
  int i, n = 0;

  text[n++] = (rank_bcd >> 4) ? '0' + (rank_bcd >> 4) : ' ';
  text[n++] = '0' + (rank_bcd & 15);
  text[n++] = ROM_WORD(RANKING_SUFFIXES + 2 * rank);
  text[n++] = ROM_WORD(RANKING_SUFFIXES + 2 * rank + 1);
  text[n++] = ' ';
  text[n++] = name >> 8;
  text[n++] = name & 0xFF;
  text[n++] = name_end >> 8;
  text[n++] = ' ';
  text[n++] = '0' + (SPRITE_WORD(slot, 4) & 15);
  text[n++] = '\'';
  text[n++] = '0' + (seconds >> 4);
  text[n++] = '0' + (seconds & 15);
  text[n++] = '"';
  text[n++] = '0' + (hundredths >> 4);
  text[n++] = '0' + (hundredths & 15);
  text[n++] = ' ';
  for (i = 0; i < 6; i++) text[n++] = ROM_WORD(RANKING_MODES + 8 * SPRITE_WORD(slot, 7) + i);
}

/* Command 8's sprites: the ranking rows over the 3D (25eb), then the usual sprites in slots
   6-31 (229F @000b). Word 0 of a ranking slot says how many of its characters show, so the
   rows are typed in; a negative count -n leaves out the first n, wiping the row away. */
static void draw_ranking_sprites(hle_frame *f)
{
  int slot;
  memset(f->sprites, 0, sizeof(f->sprites));
  f->sprites_are_picture = 0;
  for (slot = 0; slot < RANKING_ROWS; slot++)
  {
    int shown = (int16)SPRITE_WORD(slot, 0);
    int first = 0, last = shown - 1, i;
    char text[RANKING_CHARACTERS];

    if (!shown) continue;
    if (shown < 0)
    {
      first = -shown;
      last = RANKING_CHARACTERS - 1;
    }
    if (last >= RANKING_CHARACTERS) last = RANKING_CHARACTERS - 1;   /* ponytail: the SVP reads on into RAM */
    ranking_text(slot, text);
    for (i = first; i <= last; i++)
    {
      int glyph = (uint8)text[i] - ' ';
      if (glyph) sprite_blit(f, FONT_ASCII + glyph * 0x20, RANKING_X + 8 * i, RANKING_Y + 16 * slot, 2, 16);
    }
  }
  draw_sprite_slots(f, RANKING_ROWS);
}

/* The word address of model number index in a table of 32-bit byte pointers (table is a word
   address), such as a track's sections */
static uint32 table_model_address(uint32 table, int index)
{
  uint32 entry = table + index * 2;
  uint32 byte_address = ((uint32)ROM_WORD(entry) << 16) | ROM_WORD(entry + 1);
  return byte_address >> 1;
}

/* Models picked from a table of 32-bit byte pointers by the indices in slot words 21 on
   (loops 1d87 for the track on slot 0, 1daa for slot 56, which skips index 0) */
static void add_indexed_models(hle_frame *f, int slot, const transform *m, int count, int skip_zero,
                               const model_group *group)
{
  uint32 table = slot_model_address(slot);
  int i;
  for (i = 0; i < count; i++)
  {
    uint16 index = (uint16)SLOT(slot, 21 + i);
    if (!index && skip_zero) continue;
    add_model(f, table_model_address(table, index), m, group, 0);
  }
}

/* ---- far view ------------------------------------------------------------------------------ */

/* The number of sections of the current track: its table of section pointers (slot 0's model
   address) is followed directly by the first section's data. Beginner and Medium have 128,
   Expert 166. */
static int track_section_count(void)
{
  uint32 table = slot_model_address(0);                                       /* word address */
  uint32 first_section = ((uint32)ROM_WORD(table) << 16) | ROM_WORD(table + 1);  /* byte address */
  return (int)(first_section - table * 2) / 4;
}

/* How squarely the camera looks at a track section, seen from above: the cosine of the angle
   between its view and the direction to the middle of the section's vertices. 1 straight
   ahead, 0 to the side, -1 behind. */
static double camera_facing(uint32 word)
{
  int poly_count = ROM_WORD(word) + 1, p, i;
  double x = 0, z = 0, distance;
  word++;
  for (p = 0; p < poly_count; p++)
  {
    uint16 header = ROM_WORD(word++);
    int vertex_count = (header & 0x10) ? 3 : 4;
    for (i = 0; i < vertex_count; i++, word += 3)
    {
      int16 vertex[3], fixed[3];
      float exact[3];
      vertex[0] = (int16)ROM_WORD(word);
      vertex[1] = (int16)ROM_WORD(word + 1);
      vertex[2] = (int16)ROM_WORD(word + 2);
      transform_track_vertex(vertex, fixed, exact);
      x += exact[0];
      z += exact[2];
    }
  }
  distance = sqrt(x * x + z * z);
  return distance > 0 ? z / distance : 1;
}

/* The far view's sections, beyond the game's 8 on both sides. Slot 0 words 21-28 hold
   consecutive section numbers, starting at or just behind the player's; they increase along
   the direction of the race and wrap around at the track's section count. The game's 8 are
   enough for a 4:3 view at its depth limit, looking along the race.
   On the side the camera looks towards, SECTIONS_FAR more reach farther; on the other,
   SECTIONS_SIDE fill the sides of a wide view, where the scenery beside the player stays in
   view after the game has dropped its section. Racing, that is far ahead and a few behind;
   facing the other way (driving the wrong way, a replay camera looking back down the track)
   it is the opposite, and turned sideways both sides get the far ones. The side is judged by
   the second section beyond the game's 8, clear of the player's own. */
static void add_sections_around(hle_frame *f)
{
  uint32 table = slot_model_address(0);
  int sections = track_section_count();
  int first_drawn = (uint16)SLOT(0, 21);
  int last_drawn = (uint16)SLOT(0, 28);
  int ahead, behind, i;

  if (sections < 8 + 2 * SECTIONS_FAR || sections > 1024) return;   /* not a track table */
  ahead = camera_facing(table_model_address(table, (last_drawn + 2) % sections)) > FACING_AWAY
        ? SECTIONS_FAR : SECTIONS_SIDE;
  behind = camera_facing(table_model_address(table, (first_drawn - 2 + sections) % sections)) > FACING_AWAY
         ? SECTIONS_FAR : SECTIONS_SIDE;
  for (i = 1; i <= ahead; i++)
    add_model(f, table_model_address(table, (last_drawn + i) % sections), NULL, &TRACK_GROUP, 1);
  for (i = 1; i <= behind; i++)
    add_model(f, table_model_address(table, (first_drawn - i + sections) % sections), NULL, &TRACK_GROUP, 1);
}

/* An opponent's slot words from the game's record of it, the way 2381C fills slots 11 + 3n
   (68k code: x = +2E * 2, y = (+30 + 0x32) >> 3, z = +32 * 2, angles from +38, +3A + +70 and
   +3C, the model by detail level +C4 and colour group +C6), or all zero when the game draws
   it itself (+C4 is not 0). The far view draws it with the least detailed model, the one the
   game switches to when it comes near enough, so the hand-over does not show. */
static void read_far_car(int opponent, int16 words[SLOT_WORDS])
{
  uint32 record = CAR_RECORDS + opponent * 0x100;
  uint32 model_entry = CAR_MODELS + 3 * 4 + RAM_WORD(record + 0xC6);       /* detail level 3 */
  uint32 model = ((uint32)ROM_WORD(model_entry / 2) << 16) | ROM_WORD(model_entry / 2 + 1);

  memset(words, 0, SLOT_WORDS * sizeof(int16));
  if (RAM_WORD(record + 0xC4) != 0) return;
  words[0] = 1;
  words[1] = (int16)(RAM_WORD(record + 0x2E) << 1);
  words[2] = (int16)(0x32 + RAM_WORD(record + 0x30)) >> 3;
  words[3] = (int16)(RAM_WORD(record + 0x32) << 1);
  words[4] = (int16)-((int16)RAM_WORD(record + 0x38) >> 4);
  words[5] = (int16)(RAM_WORD(record + 0x3A) + RAM_WORD(record + 0x70)) >> 4;
  words[6] = (int16)-((int16)RAM_WORD(record + 0x3C) >> 4);
  words[19] = (int16)(model >> 16);
  words[20] = (int16)(model & 0xFFFF);
}

/* The far cars within MAX_FAR_CAR_DISTANCE of the camera, like opponents (01e4), without
   their wheels, which would be smaller than a pixel */
static void add_far_cars(hle_frame *f)
{
  int n, k;
  for (n = 0; n < OPPONENTS; n++)
  {
    int row = FAR_CAR_ROW(n), too_far = 0;
    transform m;
    if (!SLOT(row, 0)) continue;
    for (k = 0; k < 3; k++)
      if (fabs(SLOT_EXACT(row, 1 + k) - camera_position[k]) > MAX_FAR_CAR_DISTANCE) too_far = 1;
    if (too_far) continue;
    set_identity(&m, 0x80);
    apply_opponent_frame(&m, row);
    add_model(f, slot_model_address(row), &m, &OPPONENT_GROUP, 1);
  }
}

void hle_set_far_view(int enabled)
{
  far_view = enabled;
  max_bucket = enabled ? FAR_MAX_BUCKET : MAX_BUCKET;
}

/* Sort the polygons from first on far to near, keeping the order within a bucket (a counting
   sort): count each bucket, turn the counts into start positions from the farthest bucket on,
   then place */
static void sort_far_to_near(hle_frame *f, int first)
{
  static hle_poly sorted[HLE_MAX_POLYS];
  static int bucket_start[FAR_MAX_BUCKET + 1];
  hle_poly *polys = &f->polys[first];
  int count = f->poly_count - first;
  int i, bucket, position;

  memset(bucket_start, 0, sizeof(bucket_start));
  for (i = 0; i < count; i++) bucket_start[polys[i].depth_bucket]++;
  position = 0;
  for (bucket = max_bucket; bucket >= 0; bucket--)
  {
    int bucket_size = bucket_start[bucket];
    bucket_start[bucket] = position;
    position += bucket_size;
  }
  for (i = 0; i < count; i++) sorted[bucket_start[polys[i].depth_bucket]++] = polys[i];
  memcpy(polys, sorted, count * sizeof(hle_poly));
}

/* The group of an object slot, for its near plane */
static const model_group *slot_group(int slot)
{
  if (slot <= 10) return &PLAYER_GROUP;
  return &OPPONENT_GROUP;
}

/* The race's 3D draw list (command 2) for the slots in slot_words, sorted far to near like
   the SVP's */
static void build_race_draw_list(hle_frame *f)
{
  int slot;
  transform m;

  f->poly_count = 0;
  compute_camera();

  /* the title picture covers the whole screen: the SVP skips phases 1 to 4 */
  if (command_drawn == HLE_COMMAND_TITLE && picture_rows == PICTURE_ROWS_ALL)
  {
    f->camera_yaw = 0;
    return;
  }

  /* phase 3: the 8 track sections chosen by the 68k; in the far view more around them, and the
     opponents the game leaves out */
  add_indexed_models(f, 0, NULL, 8, 0, &TRACK_GROUP);
  if (far_view)
  {
    add_sections_around(f);
    add_far_cars(f);
  }

  /* phase 4: visible object slots; slot 56 holds a list of models like the track. The in-car
     view leaves slots 1-10 out of this pass (197d). */
  for (slot = 1; slot < SLOT_COUNT; slot++)
  {
    if (in_car_view && slot <= 10) continue;
    if (!SLOT(slot, 0) || !slot_matrix(slot, &m)) continue;
    if (slot == 56) add_indexed_models(f, 56, &m, 7, 1, &OPPONENT_GROUP);
    else add_model(f, slot_model_address(slot), &m, slot_group(slot), 0);
  }
  sort_far_to_near(f, 0);

  /* the in-car view's second pass (1935, drawn over the first, 088a): slots 1-5, the car the
     camera sits in, with a near plane close enough for its cockpit. Command 3 has none, so
     there the car is not drawn at all in this view. */
  if (in_car_view && command_drawn == HLE_COMMAND_RACE)
  {
    int first = f->poly_count;
    for (slot = 1; slot <= 5; slot++)
    {
      if (!SLOT(slot, 0) || !in_car_slot_matrix(slot, &m)) continue;
      add_model(f, slot_model_address(slot), &m, &IN_CAR_GROUP, 0);
    }
    sort_far_to_near(f, first);
  }

  /* command 3's second list (18cd, drawn over the first by 1e90): slot 57 alone */
  if (command_drawn == HLE_COMMAND_FINISH && SLOT(FINISH_TEXT_SLOT, 0))
  {
    int first = f->poly_count;
    finish_text_matrix(&m);
    add_model(f, slot_model_address(FINISH_TEXT_SLOT), &m, &FINISH_TEXT_GROUP, 0);
    sort_far_to_near(f, first);
  }

  f->camera_yaw = camera_angle_y;
}

/* ---- the SEGA logo (command 5, docs 6.16) ------------------------------------------------- */

/* The logo is 8 pieces in slots 0-7 that fly towards the camera along two curves. The 68k
   sends only the time, 30FE10 (0 to 0x1000, 0x35 a tick); the SVP works out the pieces' places
   and turns itself (0f84) and writes them into the slots. */
#define LOGO_PIECES      8
#define LOGO_PATH_A      0x1025   /* program words: the 4 control points (x, y, z) of slots 2-5 */
#define LOGO_PATH_B      0x1034   /* program words: those of slots 0, 1, 6, 7 */
#define LOGO_WEIGHTS     0x0E57   /* program words: the curve's 10 weight factors, the operands
                                     of the ldi at 0e56-0e68 (every other word) */

static const model_group LOGO_GROUP = { 2, 2 };   /* near plane 2 (18b8) */

/* The SSP's 32-bit accumulator A, as far as the logo's arithmetic uses it: a 16-bit load goes
   to its high word and keeps the low one; shifts and negation work on all 32 bits. What is
   left in the low word changes later results by one, so it is followed exactly. */
typedef struct {
  uint32 value;
} accumulator;

static int16 high_word(const accumulator *a)
{
  return (int16)(a->value >> 16);
}

static void load_high_word(accumulator *a, int16 v)
{
  a->value = (a->value & 0xFFFF) | ((uint32)(uint16)v << 16);
}

/* The sum of the products 2 x y (the multiplier, mld / mpya) shifted left 3, in the
   accumulator; its high word is the sum of x y / 4096, products of 4.12 fixed-point numbers */
static int16 fixed_sum_of_products(accumulator *a, const int16 *x, const int16 *y, int count)
{
  uint32 sum = 0;
  int i;
  for (i = 0; i < count; i++) sum += (uint32)(2 * (int32)x[i] * y[i]);
  a->value = sum << 3;
  return high_word(a);
}

/* -v through the accumulator (ld A; neg): one less when its low word is not 0 */
static int16 negated(accumulator *a, int16 v)
{
  load_high_word(a, v);
  a->value = 0u - a->value;
  return high_word(a);
}

/* Where a piece is at time t (0x1000 = 1.0) on the curve whose control points are at path
   (0d69 -> 0e55 -> 0e8e): four points weighted by polynomials in u = 1 - t, each weight a sum
   of factor x power from LOGO_WEIGHTS */
static void logo_curve(accumulator *a, int16 t, uint32 path, int16 position[3])
{
  int16 factor[10], powers_1[4], powers_2[4], basis[4];
  int16 t2, t3, u, u2, u3;
  int i, k;

  for (i = 0; i < 10; i++) factor[i] = (int16)ROM_WORD(LOGO_WEIGHTS + 2 * i);

  /* powers of t and of u (0e6a-0e8a) */
  t2 = fixed_sum_of_products(a, &t, &t, 1);
  t3 = fixed_sum_of_products(a, &t2, &t, 1);
  load_high_word(a, 0x1000);
  a->value -= (uint32)(uint16)t << 16;
  u = high_word(a);
  u2 = fixed_sum_of_products(a, &u, &u, 1);
  u3 = fixed_sum_of_products(a, &u2, &u, 1);

  /* the four weights (0e8e-0ead); the RAM words the SVP multiplies with factors 3, 4 and 8
     are not set on this path, and those factors are 0 */
  powers_1[0] = u3;  powers_1[1] = u2;  powers_1[2] = 0;  powers_1[3] = 0;
  powers_2[0] = u3;  powers_2[1] = u2;  powers_2[2] = u;  powers_2[3] = 0;
  basis[0] = fixed_sum_of_products(a, &u3, &factor[0], 1);
  basis[1] = fixed_sum_of_products(a, powers_1, &factor[1], 4);
  basis[2] = fixed_sum_of_products(a, powers_2, &factor[5], 4);
  basis[3] = fixed_sum_of_products(a, &t3, &factor[9], 1);

  /* x, y and z, in that order (0eae-0ee3) */
  for (k = 0; k < 3; k++)
  {
    int16 coordinate[4];
    for (i = 0; i < 4; i++) coordinate[i] = (int16)ROM_WORD(path + 3 * i + k);
    position[k] = fixed_sum_of_products(a, basis, coordinate, 4);
  }
}

/* A piece's slot words 1-3 and its turn as the SVP writes them, in its order: the position,
   mirrored in x and y as asked; the turn is t * 4 (ld A, [ram1:c0]; shl; shl, so the low word
   shifts in), negated when mirrored in y. Slots 0, 1, 6, 7 turn about Y (word 5, word 4 set
   to 0), slots 2-5 about X (word 4). */
static void place_logo_piece(accumulator *a, int16 *words, const int16 position[3], int16 t,
                             int mirror_x, int mirror_y, int turn_word)
{
  words[1] = mirror_x ? negated(a, position[0]) : position[0];
  words[2] = mirror_y ? negated(a, position[1]) : position[1];
  words[3] = position[2];
  if (turn_word == 5) words[4] = 0;
  load_high_word(a, t);
  a->value <<= 2;
  if (mirror_y) a->value = 0u - a->value;
  words[turn_word] = high_word(a);
}

/* The logo's slot words for time t, as the SVP writes them at the start of command 5 (0f84) */
static void compute_logo_slots(int16 words[][SLOT_WORDS], int16 t)
{
  accumulator a = { 0 };
  int16 position[3];

  logo_curve(&a, t, LOGO_PATH_A, position);
  place_logo_piece(&a, words[2], position, t, 0, 0, 4);
  place_logo_piece(&a, words[3], position, t, 0, 1, 4);
  place_logo_piece(&a, words[4], position, t, 1, 0, 4);
  place_logo_piece(&a, words[5], position, t, 1, 1, 4);

  logo_curve(&a, t, LOGO_PATH_B, position);
  place_logo_piece(&a, words[0], position, t, 0, 0, 5);
  place_logo_piece(&a, words[1], position, t, 0, 1, 5);
  place_logo_piece(&a, words[6], position, t, 1, 0, 5);
  place_logo_piece(&a, words[7], position, t, 1, 1, 5);
}

/* A piece's matrix (overlay 138f, 025e): there is no camera; identity 0x400, the piece's Y, X
   and Z turns, then its position added as it is, so camera coordinates are (R v + position) / 4 */
static void logo_piece_matrix(int slot, transform *m)
{
  set_object_identity(m, 0x400);
  rotate_y(m, SLOT_EXACT(slot, 5));
  rotate_x(m, SLOT_EXACT(slot, 4));
  rotate_z(m, SLOT_EXACT(slot, 6));
  translate_raw(m, SLOT_EXACT(slot, 1), SLOT_EXACT(slot, 2), SLOT_EXACT(slot, 3));
}

/* The logo's draw list: its pieces, sorted far to near in the coarser buckets of this command
   (18a3 masks the depth key with ffe0: 256 buckets of 32 keys, drawn by 1e7d). No sprites. */
static void build_logo_draw_list(hle_frame *f)
{
  int slot, i;
  transform m;

  f->poly_count = 0;
  for (slot = 0; slot < LOGO_PIECES; slot++)
  {
    if (!SLOT(slot, 0)) continue;
    logo_piece_matrix(slot, &m);
    add_model(f, slot_model_address(slot), &m, &LOGO_GROUP, 0);
  }
  for (i = 0; i < f->poly_count; i++) f->polys[i].depth_bucket >>= 3;   /* key >> 5, not key >> 2 */
  sort_far_to_near(f, 0);
  memset(f->sprites, 0, sizeof(f->sprites));
  f->sprites_are_picture = 0;
  f->camera_yaw = 0;
}

/* ---- the draw list of a command ------------------------------------------------------------ */

static int hle_draws(int command)
{
  return command == HLE_COMMAND_RACE || command == HLE_COMMAND_FINISH || command == HLE_COMMAND_REPLAY ||
         command == HLE_COMMAND_REPLAY_FIXED || command == HLE_COMMAND_SEGA_LOGO ||
         command == HLE_COMMAND_TITLE || command == HLE_COMMAND_VR_PICTURE ||
         command == HLE_COMMAND_RANKING;
}

/* The draw list for the slots in slot_words, of the command in command_drawn */
static void build_draw_list(hle_frame *f)
{
  if (command_drawn == HLE_COMMAND_SEGA_LOGO) build_logo_draw_list(f);
  else build_race_draw_list(f);
  f->command = command_drawn;
}

/* ---- interpolation between two commands -------------------------------------------------- */

/* Slot words 1-3 are a position (or an offset from the parent object), 4-6 the X, Y and Z
   rotation angles. Only these are interpolated. */
#define FIRST_POSITION_WORD 1
#define LAST_POSITION_WORD  3
#define FIRST_ANGLE_WORD    4
#define LAST_ANGLE_WORD     6

/* Whether the camera (slot 0) cut to another view between two commands: an angle or an offset
   word changed too much for a tick */
static int camera_cut(const int16 older[SLOT_WORDS], const int16 newer[SLOT_WORDS])
{
  int i;
  for (i = FIRST_POSITION_WORD; i <= LAST_POSITION_WORD; i++)
    if (abs((int16)(newer[i] - older[i])) > MAX_CAMERA_SHIFT) return 1;
  for (i = FIRST_ANGLE_WORD; i <= LAST_ANGLE_WORD; i++)
  {
    int turned = (((int16)(newer[i] - older[i]) + 2048) & 4095) - 2048;
    if (abs(turned) > MAX_CAMERA_TURN) return 1;
  }
  return 0;
}

/* Whether a slot other than the camera holds the same object in both commands, so that its
   motion can be interpolated: in use in both (word 0 is not 0) and not jumped. The model may
   differ: some objects, such as the grass and smoke beside the tyres, change their model every
   tick as an animation. */
static int jumped(const int16 older[SLOT_WORDS], const int16 newer[SLOT_WORDS])
{
  int i;
  for (i = FIRST_POSITION_WORD; i <= LAST_POSITION_WORD; i++)
  {
    int16 moved = (int16)(newer[i] - older[i]);
    if (abs(moved) > MAX_TICK_MOVE) return 1;
  }
  return 0;
}

static int same_object(const int16 older[SLOT_WORDS], const int16 newer[SLOT_WORDS])
{
  if (older[0] == 0 || newer[0] == 0) return 0;
  return !jumped(older, newer);
}

/* Whether the camera (slot 0) carries on from the older command to the newer, so that it can
   be interpolated. Slot 0 holds the chase camera's offset from the car, except in the replay's
   fixed views (command B), where it is the camera's own place: between the two kinds it does
   not carry on. A fixed camera may turn fast after a car going by, so there only a new place
   is another camera. */
static int same_camera(const slot_snapshot *older, const slot_snapshot *newer)
{
  int older_fixed = older->command == HLE_COMMAND_REPLAY_FIXED;
  int newer_fixed = newer->command == HLE_COMMAND_REPLAY_FIXED;
  if (older_fixed != newer_fixed) return 0;
  if (newer_fixed) return !jumped(older->words[0], newer->words[0]);
  return !camera_cut(older->words[0], newer->words[0]);
}

/* Position between two ticks (t = 0 the older, 1 the newer). The difference is taken in 16
   bits, so a coordinate that wraps around from 32767 to -32768 still moves the short way. */
static double interpolate_position(int16 older, int16 newer, double t)
{
  int16 difference = (int16)(newer - older);
  return newer - (1 - t) * difference;
}

/* Angle between two ticks (t = 0 the older, 1 the newer). 4096 is a full turn, and the game
   only uses the low 12 bits, so the angle turns the short way round: by at most half a turn
   either way. (The most a race turns anything in one tick is about 1360, a wheel spinning.) */
static double interpolate_angle(int16 older, int16 newer, double t)
{
  int difference = (int16)(newer - older);
  difference = ((difference + 2048) & 4095) - 2048;     /* now -2048 .. 2047 */
  return newer - (1 - t) * difference;
}

/* Fill slot_words with the slots between the older and the newer command (t from 0 to 1).
   Positions and angles of objects that are in both are interpolated; every other word, such as
   which model to draw or which track sections, is taken from the newer command. */
static void interpolate_slots(const slot_snapshot *older, const slot_snapshot *newer, double t)
{
  int slot, i;
  in_car_view = newer->in_car_view;
  command_drawn = newer->command;
  picture_rows = newer->picture_rows;
  for (slot = 0; slot < ROWS; slot++)
  {
    const int16 *from = older->words[slot];
    const int16 *to = newer->words[slot];

    for (i = 0; i < SLOT_WORDS; i++) slot_words[slot][i] = to[i];
    if (slot == 0 ? !same_camera(older, newer) : !same_object(from, to)) continue;

    for (i = FIRST_POSITION_WORD; i <= LAST_POSITION_WORD; i++)
      slot_words[slot][i] = interpolate_position(from[i], to[i], t);
    for (i = FIRST_ANGLE_WORD; i <= LAST_ANGLE_WORD; i++)
      slot_words[slot][i] = interpolate_angle(from[i], to[i], t);
  }
}

/* ---- hooks -------------------------------------------------------------------------------- */

/* The SVP's ram1:c8, which leaves slots 1-10 out of the first pass (197d): commands 2 and 3 set
   it from 30FE0E, A and the title's clear it, B keeps it as it was.
   ponytail: commands the HLE does not draw (menus) are not followed; between a race and its
   replay only the result screens come, which send none. */
static int command_in_car_flag(int command, int flag)
{
  if (command == HLE_COMMAND_RACE || command == HLE_COMMAND_FINISH) return DRAM_WORD(0x30FE0E) != 0;
  if (command == HLE_COMMAND_REPLAY_FIXED) return flag;
  return 0;
}

/* Keep the model slots of the command being sent (and the far cars from the game's records),
   replacing the older of the two snapshots, and load them into slot_words for the build */
static void take_snapshot(int command)
{
  slot_snapshot *snapshot;
  int slot, i;

  newest_snapshot = 1 - newest_snapshot;
  snapshot = &snapshots[newest_snapshot];
  snapshot->frame = frames_done;
  snapshot->command = command;
  in_car_flag = command_in_car_flag(command, in_car_flag);
  snapshot->in_car_view = in_car_flag;
  snapshot->picture_rows = command == HLE_COMMAND_TITLE ? DRAM_WORD(0x30FE10) : 0;
  for (slot = 0; slot < SLOT_COUNT; slot++)
    for (i = 0; i < SLOT_WORDS; i++) snapshot->words[slot][i] = DRAM_SLOT(slot, i);
  for (i = 0; i < OPPONENTS; i++) read_far_car(i, snapshot->words[FAR_CAR_ROW(i)]);
  /* the SVP itself writes the logo's slots (before using them), from the time in 30FE10 */
  if (command == HLE_COMMAND_SEGA_LOGO) compute_logo_slots(snapshot->words, (int16)DRAM_WORD(0x30FE10));
  for (slot = 0; slot < ROWS; slot++)
    for (i = 0; i < SLOT_WORDS; i++) slot_words[slot][i] = snapshot->words[slot][i];
  in_car_view = snapshot->in_car_view;
  command_drawn = snapshot->command;
  picture_rows = snapshot->picture_rows;
}

/* Called by GPGX (vrr_svp_dram_write_hook) on every 68k write to SVP DRAM; a nonzero write to
   30FE08 sends a command */
static void on_dram_write(uint32 address, uint32 data)
{
  int buffer;
  int command = (int)data;
  if (address != 0x30FE08 || !data) return;

  /* the SVP draws into the buffer that is not being shown: 30FE04 = 3000 -> 300000 (docs 1.3) */
  buffer = DRAM_WORD(0x30FE04) ? 0 : 1;
  svp_done();                  /* the 68k only sends a command once the last one is done */
  building[buffer].valid = hle_draws(command);
  /* Only the commands the HLE draws in 3D are kept for smooth motion. When another one follows
     (command 1 after the SEGA logo, the finish after a race), smooth motion carries on to the
     last tick the HLE drew and holds it until the display flips to the other command's image.
     Stopping at once would step back to the image on screen, which is two ticks older. */
  if (command == HLE_COMMAND_VR_PICTURE)
  {
    draw_vr_picture(&building[buffer]);
  }
  else if (building[buffer].valid)
  {
    take_snapshot(command);
    build_draw_list(&building[buffer]);
    /* phase 6: the SVP's sprites, over the 3D */
    if (command == HLE_COMMAND_RACE || command == HLE_COMMAND_FINISH || command == HLE_COMMAND_REPLAY ||
        command == HLE_COMMAND_REPLAY_FIXED)
      draw_sprites(&building[buffer]);
    else if (command == HLE_COMMAND_RANKING) draw_ranking_sprites(&building[buffer]);
    else if (command == HLE_COMMAND_TITLE) draw_title_sprites(&building[buffer]);
    newest_buffer = buffer;
  }
  building[buffer].image_number = ++images_built;
  svp_busy_buffer = buffer;
}

void hle_init(void)
{
  vrr_svp_dram_write_hook = on_dram_write;
}

void hle_reset(void)
{
  building[0].valid = building[1].valid = 0;
  finished[0].valid = finished[1].valid = 0;
  shown.valid = 0;
  shown_buffer = -1;
  svp_busy_buffer = -1;
  snapshots[0].command = snapshots[1].command = 0;    /* no tick to interpolate from */
}

int hle_displayed_buffer(void)
{
  /* plane A vscroll: 1 shows buffer 1 (300000), 209 shows buffer 2 (306000) */
  return (*(uint16 *)&vsram[0] & 0x3FF) >= 208;
}

/* Plane A's name table as the game sets it up for the SVP's image (imageformat.txt): each
   buffer is 24 rows of 32 cells, two tiles per column in each 16-line strip. Buffer 1 is at
   rows 2-25 from tile 1, buffer 2 at rows 28-51 from tile 1CC; from the cell with index 1CA
   on, both use the same tiles from 397 (copied in just in time). */
#define SVP_CELL_ROWS      24
#define SVP_CELL_COLUMNS   32
#define SHARED_CELLS_FROM  0x1CA
#define SHARED_FIRST_TILE  0x397

/* Whether plane A shows the SVP's buffer: its cells still point at the buffer's tiles. The
   game's own screens (the results after a race, the menus) rewrite them, and then the image
   on screen is not the SVP's, whatever the HLE last latched. */
static int plane_a_shows_svp_buffer(int buffer)
{
  uint32 table = (reg[2] & 0x38) << 10;
  int width = (reg[16] & 3) == 3 ? 128 : (reg[16] & 3) == 1 ? 64 : 32;
  int first_row = buffer ? 28 : 2;
  int first_tile = buffer ? 0x1CC : 1;
  int row, column;

  for (row = 0; row < SVP_CELL_ROWS; row++)
    for (column = 0; column < SVP_CELL_COLUMNS; column++)
    {
      int index = 64 * (row / 2) + 2 * column + (row & 1);
      int tile = index < SHARED_CELLS_FROM ? first_tile + index : SHARED_FIRST_TILE + index - SHARED_CELLS_FROM;
      uint32 address = (table + 2 * ((first_row + row) * width + column)) & 0xFFFE;
      if ((*(uint16 *)&vram[address] & 0x7FF) != tile) return 0;
    }
  return 1;
}

void hle_frame_done(void)
{
  int buffer = hle_displayed_buffer();
  if (buffer != shown_buffer)
  {
    shown_buffer = buffer;
    copy_frame(&shown, &finished[buffer]);
  }
  svp_on_screen = plane_a_shows_svp_buffer(buffer);
  /* after the flip: an image finished this frame cannot have been copied to VRAM yet */
  if (DRAM_WORD(0x30FE02)) svp_done();
  frames_done++;
}

/* A race's scene: the race, its finish and the replays */
static int is_race_scene(int command)
{
  return command == HLE_COMMAND_RACE || command == HLE_COMMAND_FINISH ||
         command == HLE_COMMAND_REPLAY || command == HLE_COMMAND_REPLAY_FIXED;
}

/* Whether two commands draw the same scene, so that smooth motion can go from one to the
   other: the same command, or two of a race's (the race going on into its finish, the replay
   changing cameras; same_camera decides whether the camera carries on) */
static int same_scene(int older, int newer)
{
  return older == newer || (is_race_scene(older) && is_race_scene(newer));
}

const hle_frame *hle_smooth(double sub_frame, int frames_per_image)
{
  static hle_frame smooth;               /* the last frame built, reused while t stays the same */
  static double smooth_t = -1;
  static int smooth_newer_frame = -1, smooth_far_view = -1;

  const slot_snapshot *older = &snapshots[1 - newest_snapshot];
  const slot_snapshot *newer = &snapshots[newest_snapshot];
  int tick_length = newer->frame - older->frame;   /* frames between the two commands */
  double elapsed, t;

  /* only between two images of the same command that the HLE draws, sent a normal tick apart
     (4 frames, 5 or 6 when the SVP is late, 8 around the "V.R." picture); anything else, such
     as the first command of a race, is shown as it is. So is the picture itself. */
  if (!hle_draws(newer->command) || !same_scene(older->command, newer->command)) return NULL;
  if (shown.valid && shown.command == HLE_COMMAND_VR_PICTURE) return NULL;
  if (tick_length < 1 || tick_length > 8) return NULL;

  /* t = 0 right after the frame the newer command was sent in, when the older tick is shown
     in full; it then grows by one tick length per tick. When the next command comes, t
     starts again from 0 at what was the newer one, so consecutive pairs join up. A capped rate
     holds each image for frames_per_image frames, counted from the tick's start. */
  elapsed = frames_done + sub_frame - newer->frame - 1;
  if (frames_per_image) elapsed = floor(elapsed / frames_per_image) * frames_per_image;
  t = elapsed / tick_length;
  if (t < 0) t = 0;
  if (t > 1) t = 1;     /* the next command is late: hold the newer tick */

  if (t != smooth_t || newer->frame != smooth_newer_frame || far_view != smooth_far_view)
  {
    interpolate_slots(older, newer, t);
    smooth.valid = 1;
    build_draw_list(&smooth);
    smooth.image_number = ++images_built;
    /* the SVP's sprites (text, digits) are not interpolated: take the newer command's */
    memcpy(smooth.sprites, building[newest_buffer].sprites, sizeof(smooth.sprites));
    smooth_t = t;
    smooth_newer_frame = newer->frame;
    smooth_far_view = far_view;
  }
  return &smooth;
}

const hle_frame *hle_displayed(void)
{
  return shown.valid && svp_on_screen ? &shown : NULL;
}

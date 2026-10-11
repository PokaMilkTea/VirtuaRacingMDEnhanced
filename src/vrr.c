/*
 * vrr: Virtua Racing frontend on the Genesis Plus GX core (SDL2 + OpenGL 3.3 core).
 *
 * usage: vrr [-v] [-d N [-t from] [-F]] [-r 1|2|4|0] [-s] [-W] [-l state] [-p frame:keys]... rom
 *   -v    print the version and exit
 *   -d N  run frames 0..N as fast as possible (input only from -p), checking every frame that
 *         our VDP layers (layers.c) composite to exactly the core's frame. Frame N is written
 *         as the core drew it (vrr_NNNNNN.ppm, same format as svptrace -w N) and, when its 3D
 *         came from the HLE, with the HLE 3D (hle_NNNNNN.ppm); the HLE 3D is compared with
 *         the SVP's image pixel by pixel (svp3d_NNNNNN.ppm, hle3d_NNNNNN.ppm). All of that in
 *         9-bit colour; the frame is also written in full colour (full_NNNNNN.ppm) and, with
 *         -r 2 or 4, with the 3D at that resolution (hle_NNNNNN_xR.ppm). Then exits.
 *   -t F  with -d: compare the HLE with the SVP's image on every frame from F on and report
 *         the mean, the lowest and the frames below 85% (wrong draw list, stray polygons);
 *         frames where no pixel is drawn by both (the screen faded out) are left out
 *   -r    internal resolution of the HLE 3D: 1, 2 or 4 times the SVP's 256x192, or 0 = as many
 *         pixels as the 3D area has on screen (key 3)
 *   -s    no slowdown: in races the SVP at SVP_FAST times its clock (key 6)
 *   -W    widescreen (key 8); with -d, frame N is also written in 16:9 (wide_NNNNNN.ppm)
 *   -F    with -d: the newest tick of frame N is also written with the original and the far
 *         view distance and the far view with haze (view_NNNNNN_near.ppm, _far.ppm, _haze.ppm),
 *         in 16:9 with -W
 *   -l    start from a saved state (key O writes one to vrr.gp0); with -d, frames and -p count
 *         from there
 *   -p    from that frame on, hold keys (S A B C U D L R X Y Z M, empty = none), as in svptrace
 * -r, -s and -W apply over the settings. Playing, the settings come from vrr.ini (in the
 * current directory, on Windows beside vrr.exe), which keys 1-9 rewrite; without it they are the play defaults below (and the
 * file is written). -d leaves vrr.ini alone and starts from the console's: 1x, the SVP's
 * integer vertices, slowing down as the console, 4:3, so that its checks do not depend on it.
 *
 * Timing: when the display runs near 60 Hz the emulator does one frame per vsync, near
 * 120 Hz one frame every second vsync; otherwise it falls back to the real 59.92 Hz with a
 * time accumulator. In vsync mode the emulated frame rate is the measured vsync rate
 * (a moving average, since the startup measurement alone can be off by ~0.5%). Audio goes
 * into an SDL queue; samples per frame follow that frame rate and are nudged by up to 0.5%
 * (audio_set_rate) to keep the queue near its target, so it neither runs dry (crackles)
 * nor grows (latency). Timing statistics are printed on exit.
 *
 * Keys, as in GPGX's SDL frontend: arrows, A/S/D = A/B/C (B is the accelerator in a race),
 * F = Start, Z/X/C = X/Y/Z, V = Mode. A game controller works alongside (read_controller). O
 * saves the emulator's state to vrr.gp0, P loads it; that file and vrr.ini are in the current
 * directory, on Windows beside vrr.exe (data_path). Esc (Back or
 * Guide on a controller) pauses the game and opens a menu with the settings below, saving,
 * loading and quitting.
 * The number keys change the settings (not the F keys, which the system may take); the
 * defaults are those for playing:
 * 1 cycles the view (default HLE): HLE (the frame rebuilt from the VDP layers with the HLE 3D in
 * place of the SVP's) / original (the core's own frame) / side by side.
 * 2 toggles the SVP's integer vertex positions and float projection (default float; key 3 picks
 * integer at 1x, float above); smooth motion (key 7) always uses the float ones.
 * 3 cycles the HLE 3D's internal resolution (default window): 1x / 2x / 4x / window. "Window"
 * draws as many pixels as the 3D area has on screen, one texel to a screen pixel; the fixed
 * scales are resampled to the screen without filtering. The 2D layers stay at their own
 * resolution; the dither pattern stays on the SVP's pixel grid.
 * 4 toggles MSAA (default on): the 3D drawn with MSAA_SAMPLES samples a pixel, at any
 * resolution. Full colour only: 9-bit colour draws palette indices, which cannot be averaged,
 * so the menu greys it out there.
 * 5 toggles full colour (default on): the 3D's two-colour dither drawn as their average and
 * dither with colour 0 as 50% coverage, plane B's dithered gradients blended (see
 * background_shader), all mixing in linear light; off shows the 9-bit palette and the dither
 * exactly as the console does.
 * 6 toggles no slowdown (default on): in races the SVP runs at SVP_FAST times its clock, so it
 * never makes the 68k wait and the game logic keeps its 15 ticks a second where the console
 * slows down (docs/game-loop.md). That changes the game's pace against its clock; off is as
 * the console.
 * 7 cycles smooth motion (default the display's rate): the 3D interpolated between the game's
 * ticks (hle_smooth), with the sky following the interpolated yaw, at 30 fps, 60 fps or the
 * display's refresh rate; 15 fps shows each tick's image as the console does, 15 times a second
 * and about 9 frames after the tick.
 * 8 toggles widescreen (default off): the frame 16:9 instead of the console's 4:3, with the
 * race's 3D projected wider (the same scale, more to each side, and over the black borders
 * above and below) and the sky extended; the 2D layers stay in the middle, other screens get
 * black sides.
 * 9 cycles the far view (default on with haze): 16 more track sections ahead of the 8 the game
 * picks and 3 behind them, which fill the sides of the widescreen frame, polygons beyond the
 * SVP's depth limit kept, and the opponents the game leaves out drawn (hle_set_far_view). With
 * haze (full colour) the 3D fades into what is behind it, the sky and the background picture,
 * from haze_start to haze_end thousand units of depth (vrr.ini). Off is as the console, in
 * either aspect.
 * The window title shows the settings, and whether the screen has an HLE image at all (races,
 * the SEGA logo and the title do; elsewhere the SVP's 3D stays).
 */
#include <math.h>
#include <SDL.h>
#include <unistd.h>
#include "core.h"
#include "gl.h"
#include "hle.h"
#include "layers.h"
#include "settings.h"
#include "version.h"

#define TITLE "Virtua Racing MD Enhanced " VRR_VERSION   /* the window's, and -v's */

#ifdef _WIN32
#include <windows.h>

/* Laptops with two GPUs (NVIDIA Optimus, AMD switchable graphics) run a program on the
   integrated one unless it asks for the fast one by exporting these */
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

/* On Windows a crash ends the program without a word: say what happened and where, as an
   offset into vrr.exe (llvm-nm or llvm-symbolizer on the same build turn it into a function) */
static LONG WINAPI report_crash(EXCEPTION_POINTERS *exception)
{
  char *base = (char *)GetModuleHandle(NULL);
  char *address = exception->ExceptionRecord->ExceptionAddress;
  fprintf(stderr, "vrr crashed: exception %08lx at vrr.exe+%llx\n", exception->ExceptionRecord->ExceptionCode,
          (unsigned long long)(address - base));
  return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#define SAMPLE_RATE   48000
#define NTSC_FPS      (53693175.0 / (3420.0 * 262.0))  /* 59.9228, exact Mega Drive NTSC rate */
#define AUDIO_TARGET  (SAMPLE_RATE / 15)              /* aim for ~67 ms of queued audio */
#define AUDIO_NUDGE   0.005                           /* max samples-per-frame correction */
#define PIXEL_ASPECT  (8.0 / 7.0)                     /* H32 pixel aspect ratio */
#define CORE_TEXTURE_SIZE 512                         /* the core's frame is uploaded into this */
#define VIEW_W        256          /* SVP 3D buffer */
#define VIEW_H        192
#define VIEW_TOP      15           /* first screen line of the 3D buffer (docs/screen-layers.md) */
#define MAX_PRESS     256
#define SVP_FAST      4            /* no slowdown: the SVP's clock multiplier (2 is already enough
                                      for every race start measured) */
#define SVP_CYCLES    800          /* GPGX's SVP cycles per line */
#define WIDE_SIDE_COLUMNS 46       /* widescreen: game pixels added on each side. 16:9 of 224 lines
                                      at the 8:7 pixel aspect is 224 * 16/9 / (8/7) = 348.4 wide */
#define MSAA_SAMPLES  4            /* key 4's MSAA (fewer if the GL has fewer) */
#define MENU_COLUMNS  30           /* the in-game menu: characters a line (8 pixels each) */
#define MENU_LINES    17           /* lines of 16 pixels */
#define MENU_W        (MENU_COLUMNS * 8 + 16)      /* its panel, with 8 pixels of margin all round */
#define MENU_H        (MENU_LINES * 16 + 16)

/* ---- settings (keys 1-9, vrr.ini and the command line) ------------------------------------ */

enum { VIEW_HLE, VIEW_ORIGINAL, VIEW_SIDE_BY_SIDE, VIEW_COUNT };
static const char *view_names[VIEW_COUNT] = { "HLE", "original", "side by side: original | HLE" };

/* The settings, as -d checks against the console. Playing starts from load_settings
   instead. */
static int view = VIEW_HLE;        /* key 1 */
static int use_svp_vertices = 1;   /* key 2: the SVP's integer projection where a polygon has one */
static int resolution = 1;         /* key 3: HLE 3D at 1, 2 or 4 times the SVP's, or one of: */
#define RESOLUTION_WINDOW      0   /* as many pixels as the 3D area has on screen */
#define RESOLUTION_OLD_MSAA    3   /* vrr.ini from before MSAA had its own key: the window's with MSAA */
static int msaa = 0;               /* key 4: the 3D anti-aliased with MSAA (full colour) */
static int full_colour = 1;        /* key 5: full colour (dither blended, linear light), else the 9-bit palette */
static int no_slowdown = 0;        /* key 6: in races the SVP at SVP_FAST times its clock */
static int smooth_motion = 1;      /* key 7: the 3D interpolated between ticks (hle_smooth), one of: */
#define SMOOTH_OFF     0           /* the console's 15 fps */
#define SMOOTH_DISPLAY 1           /* a new image every display refresh */
#define SMOOTH_30      2           /* 30 fps: two images a tick */
#define SMOOTH_60      3           /* 60 fps: one image an emulated frame */
static int widescreen = 0;         /* key 8: a 16:9 frame */
static int far_view = 1;           /* key 9: draw farther than the console, one of: */
#define FAR_VIEW_OFF   0           /* the console's distance */
#define FAR_VIEW_ON    1           /* the far view (hle_set_far_view) */
#define FAR_VIEW_HAZE  2           /* the far view fading into the background with distance (full
                                      colour): */
static int haze_start = 6;         /* vrr.ini only: no haze nearer than this, in thousands of
                                      units of camera-space depth (the console draws up to about 8) */
static int haze_end = 12;          /* fully faded from here on, also in thousands. The far view's
                                      edge is mostly 10 to 23 thousand away (docs/hle.md) */

/* ---- saved states ---------------------------------------------------------------------------- */

/* vrr's own files (vrr.ini, vrr.gp0). On Windows they go beside vrr.exe: started from
   Explorer, say with a ROM dropped on it, the program gets a current directory the player never
   chose. Elsewhere they go to the current directory. The path is in a buffer that the next call
   reuses. */
static const char *data_path(const char *name)
{
#ifdef _WIN32
  static char path[1024];
  char *base = SDL_GetBasePath();                /* the folder of vrr.exe, ending in a separator */
  snprintf(path, sizeof(path), "%s%s", base ? base : "", name);
  SDL_free(base);
  return path;
#else
  return name;
#endif
}

#define STATE_FILE data_path("vrr.gp0")       /* key O, P: beside the settings */

static uint8 state_buffer[STATE_SIZE];

/* Key O: the core's whole state (68k, Z80, VDP, sound, the SVP with its memory) to vrr.gp0;
   returns 0 when the file cannot be written */
static int save_state(void)
{
  FILE *file = fopen(STATE_FILE, "wb");
  int size, written;
  if (!file) return 0;
  size = state_save(state_buffer);
  written = fwrite(state_buffer, size, 1, file) == 1;
  fclose(file);
  if (written) printf("state saved to %s\n", STATE_FILE);
  return written;
}

/* Key P and -l: load a state; returns 0 when there is none or it is not one. The HLE's draw
   lists belong to the frames before, so they are dropped (hle_reset). */
static int load_state(const char *path)
{
  FILE *file = fopen(path, "rb");
  size_t size;
  if (!file) return 0;
  size = fread(state_buffer, 1, STATE_SIZE, file);
  fclose(file);
  if (!size || !state_load(state_buffer)) return 0;
  hle_reset();
  printf("state loaded from %s\n", path);
  return 1;
}

/* ---- settings file ------------------------------------------------------------------------- */

#define SETTINGS_FILE data_path("vrr.ini")    /* beside the saves */

#define ON_OR_OFF 3u                                          /* allowed values 0 and 1 */
#define VIEWS     ((1u << VIEW_COUNT) - 1)
#define SCALES    (1u << 0 | 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4)

/* The settings in the order of their keys 1-9, then those only in vrr.ini */
static const setting settings[] = {
  { "view",          &view,             VIEWS,     "1: 0 HLE, 1 original, 2 side by side" },
  { "svp_vertices",  &use_svp_vertices, ON_OR_OFF, "2: 1 the SVP's integer vertices (at 1x, without smooth motion), 0 float" },
  { "resolution",    &resolution,       SCALES,    "3: 3D at 1, 2 or 4 times the console's, 0 the window's" },
  { "msaa",          &msaa,             ON_OR_OFF, "4: 1 the 3D anti-aliased with MSAA (full colour), 0 off" },
  { "full_colour",   &full_colour,      ON_OR_OFF, "5: 1 full colour, 0 the console's 9-bit palette and dither" },
  { "no_slowdown",   &no_slowdown,      ON_OR_OFF, "6: 1 the SVP sped up in races, 0 slowing down as the console" },
  { "smooth_motion", &smooth_motion,    15,        "7: 0 15 fps as the console, 2 30 fps, 3 60 fps, 1 the display's rate" },
  { "widescreen",    &widescreen,       ON_OR_OFF, "8: 1 16:9, 0 4:3" },
  { "far_view",      &far_view,         7,         "9: 1 far view, 2 far view with haze (full colour), 0 the console's distance" },
  { "haze_start",    &haze_start,       ~0u,       "the haze (far_view 2) starts at this depth, in thousands (0-31; the console draws up to 8)" },
  { "haze_end",      &haze_end,         ~0u,       "... and hides everything from this depth on, in thousands (0-31; the far view reaches 24)" },
};
#define SETTING_COUNT ((int)(sizeof(settings) / sizeof(settings[0])))
#define KEY_SETTINGS  9            /* the first nine, on keys 1-9 and in the menu */

static void save_settings(void)
{
  settings_save(SETTINGS_FILE, "vrr settings: keys 1-9 change them while playing, and this file is rewritten",
                settings, SETTING_COUNT);
}

/* The settings for playing: the enhancements on (the window's resolution with MSAA, float
   vertices, smooth motion, no slowdown, far view with haze) in the console's 4:3, then what
   vrr.ini says.
   Writes the file when there is none. */
static void load_settings(void)
{
  view = VIEW_HLE;
  use_svp_vertices = 0;
  resolution = RESOLUTION_WINDOW;
  msaa = 1;
  full_colour = 1;
  no_slowdown = 1;
  smooth_motion = SMOOTH_DISPLAY;
  widescreen = 0;
  far_view = FAR_VIEW_HAZE;
  haze_start = 6;
  haze_end = 12;
  if (!settings_load(SETTINGS_FILE, settings, SETTING_COUNT)) save_settings();
  if (resolution == RESOLUTION_OLD_MSAA)
  {
    resolution = RESOLUTION_WINDOW;
    msaa = 1;
    save_settings();
  }
}

/* Smooth motion's cap for hle_smooth: emulated frames an image is held, 0 for every refresh */
static int frames_per_image(void)
{
  if (smooth_motion == SMOOTH_30) return 2;
  if (smooth_motion == SMOOTH_60) return 1;
  return 0;
}

static const char *smooth_motion_name(void)
{
  if (smooth_motion == SMOOTH_30) return "30 fps";
  if (smooth_motion == SMOOTH_60) return "60 fps";
  if (smooth_motion == SMOOTH_DISPLAY) return "Display";
  return "15 fps";
}

/* ---- OpenGL objects ------------------------------------------------------------------------ */

/* Texture units, each holding one texture for the whole run */
enum {
  UNIT_CORE_FRAME,        /* the core's own frame (RGB565) */
  UNIT_LAYERS,            /* our VDP layers: 4 layer bytes per pixel (layers.h) */
  UNIT_PALETTE,           /* CRAM of each line as RGB565 */
  UNIT_HLE,               /* the HLE 3D, rendered by render_hle */
  UNIT_SVP_SPRITES,       /* the HLE's copy of the SVP's sprites (palette indices) */
  UNIT_LINEAR_PALETTE,    /* CRAM of each line in linear light */
  UNIT_BACKGROUND,        /* plane B over the backdrop with its dither blended (full colour) */
  UNIT_MENU,              /* the in-game menu's panel (RGBA, premultiplied) */
  UNIT_HLE_HAZE           /* the HLE 3D's haze (key 9), premultiplied by coverage */
};

static GLuint blit_program, composite_program, hle_program, background_program, menu_program;
static GLuint quad_vao;                      /* no attributes: the vertex shader makes the quad */
static GLuint hle_vao, hle_vertex_buffer, hle_framebuffer, hle_texture, hle_haze_texture;
static GLuint background_framebuffer;
static GLuint msaa_framebuffer, msaa_renderbuffers[2];       /* key 4's MSAA: the 3D drawn here, */
static int msaa_samples, msaa_w, msaa_h;                     /* then resolved into hle_texture */
static int hle_texture_w = VIEW_W, hle_texture_h = VIEW_H;   /* current size of hle_texture */

/* Per HLE vertex: camera x y z, colour 1, colour 2, checkerboard, has the SVP's projection,
   the SVP's screen x y, the polygon's near plane */
#define FLOATS_PER_VERTEX 10
static float hle_vertices[HLE_MAX_POLYS * 6 * FLOATS_PER_VERTEX];

static unsigned images_shown;      /* distinct HLE images drawn so far (hle_frame.image_number) */
static float sky_shift;            /* plane B moved right by this many pixels in the 3D area (smooth motion) */
static int16 samples[4096 * 2];

/* ---- shaders ------------------------------------------------------------------------------ */

/* A quad over the whole viewport from 4 vertices without attributes (a triangle strip);
   uv runs 0..uv_scale with uv.y = 0 on the top line */
static const char *quad_vertex_shader =
  "#version 330 core\n"
  "uniform vec2 uv_scale;\n"
  "out vec2 uv;\n"
  "void main() {\n"
  "  vec2 corner = vec2(gl_VertexID & 1, gl_VertexID >> 1);\n"   /* (0,0) (1,0) (0,1) (1,1) */
  "  uv = vec2(corner.x, 1.0 - corner.y) * uv_scale;\n"
  "  gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
  "}\n";

/* The core's frame as it is */
static const char *blit_shader =
  "#version 330 core\n"
  "uniform sampler2D tex;\n"
  "in vec2 uv;\n"
  "out vec4 color;\n"
  "void main() { color = texture(tex, uv); }\n";

/* The frame rebuilt from our copy of the VDP layers (layers.h), with plane A inside the 3D
   buffer replaced by the HLE image. VDP priority: sprites high > plane A high > plane B high >
   sprites low > plane A low > plane B low > backdrop; the HLE pixels keep the priority of the
   plane A cells they replace. The 2D layers are scaled up as plain pixels.
   Widescreen (key 8): the frame is wider than the console's screen by side_columns on each side.
   With the HLE the 3D fills them; the other layers stay in the middle. In a race plane B also
   wraps around into them (it is as wide as the screen and scrolls as a whole, so this is what
   the VDP would draw on a wider screen). Screens without the HLE get black sides. The HLE 3D
   also fills the black borders above and below its area (hle_area covers the whole frame); in
   a race plane B is empty there, so the lines of the 3D area are mirrored into them (sky above,
   ground below; simply repeating the first line would draw any cloud on it as a vertical
   streak). The title demo's track gets the same. Elsewhere (the SEGA logo) plane B stays as it
   is: beside the screen, its edge. A picture over the whole buffer (the title's) is extended
   from its edges into the sides and the borders. */
static const char *composite_shader =
  "#version 330 core\n"
  "uniform usampler2D layers;\n"           /* layer bytes: r plane B, g plane A, b sprites, a backdrop */
  "uniform sampler2D palette;\n"           /* CRAM of each line as RGB565 */
  "uniform sampler2D linear_palette;\n"    /* the same in linear light (full colour) */
  "uniform sampler2D background;\n"        /* plane B over the backdrop, dither blended, linear (full colour) */
  "uniform sampler2D hle;\n"
  "uniform sampler2D hle_haze;\n"                  /* key 9's haze of the 3D, premultiplied by coverage */               /* the HLE 3D: palette index / 255 in red, 0 = nothing drawn;
                                              full colour: linear RGB premultiplied by coverage */
  "uniform usampler2D svp_sprites;\n"      /* the SVP's sprites over the HLE 3D, 0 = none */
  "uniform bool use_hle;\n"
  "uniform bool full_colour;\n"            /* work in linear light, dither blended */
  "uniform vec2 frame_size;\n"             /* the frame drawn, in game pixels */
  "uniform vec2 layers_size;\n"            /* the console's screen (our layers) in it */
  "uniform float side_columns;\n"          /* widescreen: columns added on each side */
  "uniform vec2 hle_area;\n"               /* first line and height of the HLE 3D */
  "uniform vec2 svp_area;\n"               /* first line and height of the SVP's buffer (its sprites) */
  "uniform float sky_shift;\n"             /* with the HLE: move plane B right by this many pixels
                                              (the plane is as wide as the screen, so it wraps) */
  "uniform bool wrap_background;\n"        /* a track: plane B wraps into the widescreen sides and is
                                              mirrored into the borders (other screens: as they are) */
  "uniform float hud_shift;\n"              /* widescreen race: the sprites at the sides (bit 6) move
                                              this many columns outward */
  "uniform bool extend_sprites;\n"         /* the SVP's sprites are a picture: beside and above or
                                              below the buffer, take its nearest pixel */
  "in vec2 uv;\n"
  "out vec4 color;\n"
  "\n"
  "const int SOURCE_BACKDROP = 0, SOURCE_PLANE_B = 1, SOURCE_PLANE_A = 2, SOURCE_SPRITES = 3;\n"
  "\n"
  "bool is_opaque(uint layer_byte) { return (layer_byte & 15u) != 0u; }\n"
  "bool is_high_priority(uint layer_byte) { return layer_byte >= 128u; }\n"
  "\n"
  /* the colour of a layer byte (palette line and index) on a screen line */
  "vec3 palette_colour(uint layer_byte, int line) {\n"
  "  ivec2 entry = ivec2(int(layer_byte & 63u), line);\n"
  "  if (full_colour) return texelFetch(linear_palette, entry, 0).rgb;\n"
  "  return texelFetch(palette, entry, 0).rgb;\n"
  "}\n"
  "\n"
  /* which layer shows, in VDP priority order */
  "int visible_layer(uint sprite, uint plane_a, uint plane_b) {\n"
  "  if (is_opaque(sprite) && is_high_priority(sprite)) return SOURCE_SPRITES;\n"
  "  if (is_opaque(plane_a) && is_high_priority(plane_a)) return SOURCE_PLANE_A;\n"
  "  if (is_opaque(plane_b) && is_high_priority(plane_b)) return SOURCE_PLANE_B;\n"
  "  if (is_opaque(sprite)) return SOURCE_SPRITES;\n"
  "  if (is_opaque(plane_a)) return SOURCE_PLANE_A;\n"
  "  if (is_opaque(plane_b)) return SOURCE_PLANE_B;\n"
  "  return SOURCE_BACKDROP;\n"
  "}\n"
  "\n"
  /* a sprite at a side of the console's screen (LAYER_SIDE_SPRITE) */
  "bool is_side_sprite(ivec2 pixel) {\n"
  "  if (pixel.x < 0 || pixel.x >= int(layers_size.x)) return false;\n"
  "  uint sprite = texelFetch(layers, pixel, 0).b;\n"
  "  return is_opaque(sprite) && (sprite & 64u) != 0u;\n"
  "}\n"
  "\n"
  /* the sprite pixel to show at a pixel: with hud_shift the sprites at the sides come from
     hud_shift columns inward and the middle's stay; x = -1 for none */
  "ivec2 sprite_source(ivec2 pixel) {\n"
  "  int shift = int(hud_shift), middle = int(layers_size.x) / 2;\n"
  "  ivec2 moved_left = pixel + ivec2(shift, 0), moved_right = pixel - ivec2(shift, 0);\n"
  "  if (shift == 0) return pixel;\n"
  "  if (moved_left.x < middle && is_side_sprite(moved_left)) return moved_left;\n"
  "  if (moved_right.x >= middle && is_side_sprite(moved_right)) return moved_right;\n"
  "  if (is_side_sprite(pixel)) return ivec2(-1);\n"      /* it moved away */
  "  return pixel;\n"
  "}\n"
  "\n"
  "vec3 linear_to_srgb(vec3 c) {\n"
  "  return mix(12.92 * c, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));\n"
  "}\n"
  "\n"
  /* plane B, or the backdrop where it is clear, at a source pixel */
  "vec3 background_colour(ivec2 pixel) {\n"
  "  pixel = clamp(pixel, ivec2(0), ivec2(layers_size) - 1);\n"
  "  if (full_colour) return texelFetch(background, pixel, 0).rgb;\n"
  "  uvec4 here = texelFetch(layers, pixel, 0);\n"
  "  return palette_colour(is_opaque(here.r) ? here.r : here.a, pixel.y);\n"
  "}\n"
  "\n"
  "void main() {\n"
  "  vec2 position = uv * frame_size - vec2(side_columns, 0.0);\n"   /* in our layers' game pixels */
  "  ivec2 pixel = ivec2(floor(position));\n"
  "  ivec2 last_pixel = ivec2(layers_size) - 1;\n"
  "  bool beside = pixel.x < 0 || pixel.x > last_pixel.x;\n"   /* in a widescreen side */
  "  if (beside && !use_hle) {\n"
  "    color = vec4(0.0, 0.0, 0.0, 1.0);\n"
  "    return;\n"
  "  }\n"
  /* the backdrop and plane A's cell priority do not change across a line: beside the screen,
     take them from its edge */
  "  uvec4 here = texelFetch(layers, clamp(pixel, ivec2(0), last_pixel), 0);\n"
  "\n"
  "  float hle_y = (position.y - hle_area.x) / hle_area.y;\n"   /* 0..1 inside the 3D buffer */
  "  bool in_3d = use_hle && hle_y >= 0.0 && hle_y < 1.0;\n"
  "\n"
  /* plane B and the backdrop, moved by sky_shift inside the 3D area (smooth motion); above
     and below the SVP's area, mirrored at its edges */
  "  vec2 background_position = position;\n"
  "  if (in_3d && wrap_background) {\n"
  "    float first_line = svp_area.x, end_line = svp_area.x + svp_area.y;\n"
  "    background_position.x = mod(position.x - sky_shift, layers_size.x);\n"
  "    if (position.y < first_line) background_position.y = 2.0 * first_line - position.y;\n"
  "    if (position.y >= end_line) background_position.y = 2.0 * end_line - position.y;\n"
  "  }\n"
  "  uint plane_b = texelFetch(layers, clamp(ivec2(floor(background_position)), ivec2(0), last_pixel), 0).r;\n"
  "\n"
  "  ivec2 sprite_from = sprite_source(pixel);\n"
  "  bool has_sprite = sprite_from.x >= 0 && sprite_from.x <= last_pixel.x;\n"
  "  uint plane_a = beside ? 0u : here.g;\n"
  "  uint sprite = has_sprite ? texelFetch(layers, sprite_from, 0).b : 0u;\n"
  "\n"
  /* inside the 3D area plane A is the SVP's sprites over the HLE 3D, with the cell's priority */
  "  vec4 hle_colour = vec4(0.0);\n"
  "  bool hle_drawn = false;\n"                     /* full colour: the 3D covers this pixel */                    /* full colour: the 3D, coverage in alpha */
  "  if (in_3d) {\n"
  "    ivec2 buffer_pixel = ivec2(pixel.x, pixel.y - int(svp_area.x));\n"
  "    bool in_svp_area = position.y >= svp_area.x && position.y < svp_area.x + svp_area.y;\n"
  "    uint svp_sprite = 0u;\n"
  "    if (extend_sprites || (!beside && in_svp_area))\n"
  "      svp_sprite = texelFetch(svp_sprites, clamp(buffer_pixel, ivec2(0), ivec2(layers_size.x, svp_area.y) - 1), 0).r;\n"
  "    vec4 hle_texel = texture(hle, vec2(uv.x, 1.0 - hle_y));\n"
  "    uint hle_byte;\n"
  "    if (svp_sprite != 0u) {\n"
  "      hle_byte = svp_sprite;\n"
  "    } else if (full_colour) {\n"
  "      hle_colour = hle_texel;\n"
  "      hle_drawn = hle_texel.a > 0.0;\n"
  "      hle_byte = uint(hle_drawn);\n"               /* any opaque index marks it as drawn */
  /* the haze fades the 3D into what shows behind it, as if it covered less */
  "      if (hle_drawn) hle_colour *= 1.0 - texture(hle_haze, vec2(uv.x, 1.0 - hle_y)).r / hle_texel.a;\n"
  "    } else {\n"
  "      hle_byte = uint(hle_texel.r * 255.0 + 0.5);\n"
  "    }\n"
  "    plane_a = (here.g & 128u) | hle_byte;\n"
  "  }\n"
  "\n"
  "  int source = visible_layer(sprite, plane_a, plane_b);\n"
  "  vec3 colour;\n"
  "  if (source == SOURCE_SPRITES) colour = palette_colour(sprite, pixel.y);\n"
  "  else if (source == SOURCE_PLANE_A) colour = palette_colour(plane_a, pixel.y);\n"
  "  else colour = background_colour(ivec2(background_position));\n"
  "\n"
  /* the 3D covers its pixel only partly (full colour, a dither with colour 0): blend it over
     whatever shows behind plane A */
  "  if (source == SOURCE_PLANE_A && hle_drawn) {\n"
  "    vec3 behind;\n"
  "    if (visible_layer(sprite, 0u, plane_b) == SOURCE_SPRITES) behind = palette_colour(sprite, pixel.y);\n"
  "    else behind = background_colour(ivec2(background_position));\n"
  "    colour = hle_colour.rgb + (1.0 - hle_colour.a) * behind;\n"
  "  }\n"
  "  color = vec4(full_colour ? linear_to_srgb(colour) : colour, 1.0);\n"
  "}\n";

/* Full colour: plane B over the backdrop in linear light with its dither blended. The sky's
   gradients are dithered: two adjacent CRAM levels interleaved, so a dither pixel sits
   between two equal neighbours (A B A, across or down) one step from it. Such a pixel and
   its 3x3 neighbours that are at most one step away share colour with tent weights 1 2 1,
   which turns checkerboard and line patterns into exact halves. Lines and edges (a brick's
   1-pixel highlight between the brick and the mortar) are no A B A and stay as they are.
   Drawn at the layers' own resolution. */
static const char *background_shader =
  "#version 330 core\n"
  "uniform usampler2D layers;\n"
  "uniform sampler2D palette;\n"
  "uniform sampler2D linear_palette;\n"
  "uniform vec2 frame_size;\n"
  "out vec4 color;\n"
  "\n"
  /* the palette entry (index, line) of plane B, or of the backdrop where plane B is clear */
  "ivec2 palette_entry(ivec2 pixel) {\n"
  "  pixel = clamp(pixel, ivec2(0), ivec2(frame_size) - 1);\n"
  "  uvec4 here = texelFetch(layers, pixel, 0);\n"
  "  uint layer_byte = (here.r & 15u) != 0u ? here.r : here.a;\n"
  "  return ivec2(int(layer_byte & 63u), pixel.y);\n"
  "}\n"
  "\n"
  "vec3 shown_colour(ivec2 pixel) { return texelFetch(palette, palette_entry(pixel), 0).rgb; }\n"
  "\n"
  /* at most one CRAM step apart in every channel (a step is 0.13-0.16 in RGB565) */
  "bool one_step_apart(vec3 a, vec3 b) { return all(lessThanEqual(abs(a - b), vec3(0.2))); }\n"
  "\n"
  "bool is_dither_pixel(ivec2 pixel) {\n"
  "  vec3 centre = shown_colour(pixel);\n"
  "  vec3 left = shown_colour(pixel - ivec2(1, 0)), right = shown_colour(pixel + ivec2(1, 0));\n"
  "  vec3 above = shown_colour(pixel - ivec2(0, 1)), below = shown_colour(pixel + ivec2(0, 1));\n"
  "  bool across = left == right && left != centre && one_step_apart(left, centre);\n"
  "  bool down = above == below && above != centre && one_step_apart(above, centre);\n"
  "  return across || down;\n"
  "}\n"
  "\n"
  "void main() {\n"
  "  ivec2 pixel = ivec2(gl_FragCoord.xy);\n"   /* framebuffer row y = line y */
  "  vec3 centre = shown_colour(pixel);\n"
  "  bool centre_is_dither = is_dither_pixel(pixel);\n"
  "  vec3 sum = vec3(0.0);\n"
  "  float total_weight = 0.0;\n"
  "  for (int dy = -1; dy <= 1; dy++)\n"
  "    for (int dx = -1; dx <= 1; dx++) {\n"
  "      ivec2 neighbour = pixel + ivec2(dx, dy);\n"
  "      bool is_centre = dx == 0 && dy == 0;\n"
  "      if (!is_centre) {\n"
  "        if (!one_step_apart(shown_colour(neighbour), centre)) continue;\n"
  "        if (!centre_is_dither && !is_dither_pixel(neighbour)) continue;\n"
  "      }\n"
  "      float weight = float((2 - abs(dx)) * (2 - abs(dy)));\n"
  "      sum += weight * texelFetch(linear_palette, palette_entry(neighbour), 0).rgb;\n"
  "      total_weight += weight;\n"
  "    }\n"
  "  color = vec4(sum / total_weight, 1.0);\n"
  "}\n";

/* HLE 3D: camera-space vertices, projected like the SVP (docs/svp-commands.md 6.8):
   x = 129 + 64x/z, y = 96 - 80y/z in the 256x192 buffer. Widescreen enlarges the buffer with
   the same scale: buffer_side columns on each side (to buffer_width) and buffer_top lines
   above (to buffer_height lines), so x moves right by buffer_side and y down by buffer_top.
   In clip space, with w = z:
     x = (2z + 128x) / buffer_width
     y = ((buffer_height - 2 buffer_top - 192) z + 160y) / buffer_height
   which for the plain 256x192 buffer is (z + 64x) / 128 and 80y / 96; z = z - 2 near puts the
   near plane at -1, so the GPU clips there. Or the SVP's own integer screen position, where the
   polygon has one, moved the same way. */
static const char *hle_vertex_shader =
  "#version 330 core\n"
  "layout(location = 0) in vec3 camera_position;\n"
  "layout(location = 1) in vec4 colours;\n"        /* colour 1, colour 2, checkerboard, has the SVP's projection */
  "layout(location = 2) in vec2 svp_screen;\n"     /* the SVP's integer projection */
  "layout(location = 3) in float near_z;\n"        /* the polygon's near plane, camera-space z */
  "uniform bool use_svp_screen;\n"
  "uniform float buffer_width;\n"                 /* in SVP pixels: 256, wider in widescreen */
  "uniform float buffer_side;\n"                  /* columns added on each side */
  "uniform float buffer_height;\n"                /* in SVP lines: 192, taller in widescreen */
  "uniform float buffer_top;\n"                   /* lines added above */
  "flat out vec3 dither;\n"                        /* colour 1, colour 2, checkerboard */
  "out float depth;\n"                             /* camera-space z, for the haze */
  "void main() {\n"
  "  vec3 p = camera_position;\n"
  "  depth = p.z;\n"
  "  float extra_lines = buffer_height - 2.0 * buffer_top - 192.0;\n"   /* 0 for the plain buffer */
  "  if (use_svp_screen && colours.w > 0.5) {\n"
  "    float x = 2.0 * (buffer_side + 129.0 + svp_screen.x) / buffer_width - 1.0;\n"
  "    float y = (extra_lines + 2.0 * svp_screen.y) / buffer_height;\n"
  "    gl_Position = vec4(x, y, 0.0, 1.0);\n"
  "  } else {\n"
  "    float x = (2.0 * p.z + 128.0 * p.x) / buffer_width;\n"
  "    float y = (extra_lines * p.z + 160.0 * p.y) / buffer_height;\n"
  "    gl_Position = vec4(x, y, p.z - 2.0 * near_z, p.z);\n"
  "  }\n"
  "  dither = colours.xyz;\n"
  "}\n";

/* Dithering as the SVP does it (docs 6.5): colour 1 on even x (lines) or odd x + y
   (checkerboard), colour 2 on the others; the output is the palette index, coloured by the
   composite. Full colour: the two colours in equal parts, linear light, premultiplied; colour
   0 adds nothing, so a dither with colour 0 is 50% coverage over what was drawn before. */
static const char *hle_fragment_shader =
  "#version 330 core\n"
  "flat in vec3 dither;\n"
  "in float depth;\n"
  "uniform bool haze;\n"                           /* key 9's haze (full colour) */
  "uniform float haze_start, haze_end;\n"
  "uniform vec2 scale;\n"                          /* texture pixels per SVP pixel */
  "uniform float buffer_side;\n"                  /* widescreen: columns added on each side */
  "uniform float buffer_height;\n"                /* lines of the buffer */
  "uniform float buffer_top;\n"                   /* widescreen: lines added above */
  "uniform bool full_colour;\n"
  "uniform sampler2D linear_palette;\n"            /* linear CRAM of each screen line */
  "layout(location = 0) out vec4 color;\n"
  "layout(location = 1) out vec4 haze_out;\n"      /* the haze premultiplied by coverage, in red */
  "void main() {\n"
  "  haze_out = vec4(0.0);\n"
  "  ivec2 svp_pixel = ivec2(floor(gl_FragCoord.xy / scale));\n"
  /* coordinates in the SVP's own buffer, y down (negative or past the end in the widescreen margins) */
  "  int x = svp_pixel.x - int(buffer_side);\n"
  "  int y = int(buffer_height) - 1 - svp_pixel.y - int(buffer_top);\n"
  "  int colour_1 = int(dither.x), colour_2 = int(dither.y);\n"
  "  if (full_colour) {\n"
  "    int line = clamp(15 + y, 15, 206);\n"         /* the buffer starts on screen line 15 */
  "    vec3 first = colour_1 != 0 ? texelFetch(linear_palette, ivec2(colour_1, line), 0).rgb : vec3(0.0);\n"
  "    vec3 second = colour_2 != 0 ? texelFetch(linear_palette, ivec2(colour_2, line), 0).rgb : vec3(0.0);\n"
  "    float coverage = float(colour_1 != 0) + float(colour_2 != 0);\n"
  "    color = 0.5 * vec4(first + second, coverage);\n"
  /* the haze grows linearly from haze_start to haze_end */
  "    float amount = clamp((depth - haze_start) / (haze_end - haze_start), 0.0, 1.0);\n"
  "    if (haze) haze_out = vec4(amount * color.a, 0.0, 0.0, color.a);\n"
  "    return;\n"
  "  }\n"
  "  bool checkerboard = dither.z > 0.5;\n"
  "  bool use_second;\n"
  "  if (checkerboard) use_second = ((x + y + 1) & 1) == 1;\n"
  "  else use_second = (x & 1) == 1;\n"
  "  int colour = use_second ? colour_2 : colour_1;\n"
  "  if (colour == 0) discard;\n"                  /* colour 0 is transparent */
  "  color = vec4(float(colour) / 255.0, 0.0, 0.0, 1.0);\n"
  "}\n";

/* ---- OpenGL setup -------------------------------------------------------------------------- */

static const GLenum HLE_OUTPUTS[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };   /* colour, haze */

static GLuint compile_shader(GLenum type, const char *source)
{
  GLint ok;
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok)
  {
    char log[1024];
    glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    fprintf(stderr, "shader: %s\n", log);
    exit(1);
  }
  return shader;
}

static GLuint link_program(const char *vertex_source, const char *fragment_source)
{
  GLuint program = glCreateProgram();
  glAttachShader(program, compile_shader(GL_VERTEX_SHADER, vertex_source));
  glAttachShader(program, compile_shader(GL_FRAGMENT_SHADER, fragment_source));
  glLinkProgram(program);
  return program;
}

/* Uniform setters for the program in use */
static void set_int(GLuint program, const char *name, int value)
{
  glUniform1i(glGetUniformLocation(program, name), value);
}

static void set_float(GLuint program, const char *name, float value)
{
  glUniform1f(glGetUniformLocation(program, name), value);
}

static void set_vec2(GLuint program, const char *name, float x, float y)
{
  glUniform2f(glGetUniformLocation(program, name), x, y);
}

static void use_texture_unit(int unit)
{
  glActiveTexture(GL_TEXTURE0 + unit);
}

/* A texture on the active unit, with nearest filtering (integer textures need it too) */
static GLuint make_texture(GLint internal_format, int w, int h, GLenum format, GLenum type)
{
  GLuint texture;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexImage2D(GL_TEXTURE_2D, 0, internal_format, w, h, 0, format, type, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  return texture;
}

/* Link the programs and tell each sampler its texture unit */
static void create_programs(void)
{
  blit_program = link_program(quad_vertex_shader, blit_shader);
  composite_program = link_program(quad_vertex_shader, composite_shader);
  hle_program = link_program(hle_vertex_shader, hle_fragment_shader);
  background_program = link_program(quad_vertex_shader, background_shader);
  menu_program = link_program(quad_vertex_shader, blit_shader);

  glUseProgram(composite_program);
  set_int(composite_program, "layers", UNIT_LAYERS);
  set_int(composite_program, "palette", UNIT_PALETTE);
  set_int(composite_program, "hle", UNIT_HLE);
  set_int(composite_program, "svp_sprites", UNIT_SVP_SPRITES);
  set_int(composite_program, "linear_palette", UNIT_LINEAR_PALETTE);
  set_int(composite_program, "background", UNIT_BACKGROUND);
  set_int(composite_program, "hle_haze", UNIT_HLE_HAZE);
  set_vec2(composite_program, "uv_scale", 1, 1);
  set_vec2(composite_program, "svp_area", VIEW_TOP, VIEW_H);

  glUseProgram(hle_program);
  set_int(hle_program, "linear_palette", UNIT_LINEAR_PALETTE);

  glUseProgram(background_program);
  set_int(background_program, "layers", UNIT_LAYERS);
  set_int(background_program, "palette", UNIT_PALETTE);
  set_int(background_program, "linear_palette", UNIT_LINEAR_PALETTE);
  set_vec2(background_program, "uv_scale", 1, 1);

  glUseProgram(menu_program);
  set_int(menu_program, "tex", UNIT_MENU);
  set_vec2(menu_program, "uv_scale", 1, 1);

  glUseProgram(blit_program);    /* the default; every other pass switches back to it */
}

/* The HLE's vertex buffer: attribute 0 camera position, 1 colours and flags, 2 the SVP's screen */
static void create_hle_vertex_array(void)
{
  GLsizei stride = FLOATS_PER_VERTEX * sizeof(float);
  glGenVertexArrays(1, &hle_vao);
  glBindVertexArray(hle_vao);
  glGenBuffers(1, &hle_vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, hle_vertex_buffer);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void *)0);
  glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (void *)(3 * sizeof(float)));
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void *)(7 * sizeof(float)));
  glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, (void *)(9 * sizeof(float)));
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glEnableVertexAttribArray(3);

  glGenVertexArrays(1, &quad_vao);   /* core profile needs a bound VAO, even with no attributes */
  glBindVertexArray(quad_vao);
}

static void create_textures(void)
{
  use_texture_unit(UNIT_LAYERS);
  make_texture(GL_RGBA8UI, LAYERS_W, LAYERS_H, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE);

  use_texture_unit(UNIT_PALETTE);
  make_texture(GL_RGB8, 64, LAYERS_H, GL_RGB, GL_UNSIGNED_SHORT_5_6_5);

  use_texture_unit(UNIT_HLE);                 /* rendered to, resized by render_hle */
  glGenFramebuffers(1, &hle_framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, hle_framebuffer);
  hle_texture = make_texture(GL_RGBA16F, VIEW_W, VIEW_H, GL_RGBA, GL_FLOAT);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hle_texture, 0);
  use_texture_unit(UNIT_HLE_HAZE);            /* the second output of the HLE's fragment shader */
  hle_haze_texture = make_texture(GL_R16F, VIEW_W, VIEW_H, GL_RED, GL_FLOAT);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, hle_haze_texture, 0);
  glDrawBuffers(2, HLE_OUTPUTS);

  use_texture_unit(UNIT_SVP_SPRITES);
  make_texture(GL_R8UI, HLE_W, HLE_H, GL_RED_INTEGER, GL_UNSIGNED_BYTE);

  use_texture_unit(UNIT_LINEAR_PALETTE);
  make_texture(GL_RGB32F, 64, LAYERS_H, GL_RGB, GL_FLOAT);

  use_texture_unit(UNIT_BACKGROUND);          /* rendered to by draw_background */
  glGenFramebuffers(1, &background_framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, background_framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         make_texture(GL_RGBA16F, LAYERS_W, LAYERS_H, GL_RGBA, GL_FLOAT), 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  use_texture_unit(UNIT_MENU);
  make_texture(GL_RGBA8, MENU_W, MENU_H, GL_RGBA, GL_UNSIGNED_BYTE);

  /* MSAA: a multisampled renderbuffer, given its size by render_hle */
  glGetIntegerv(GL_MAX_SAMPLES, &msaa_samples);
  if (msaa_samples > MSAA_SAMPLES) msaa_samples = MSAA_SAMPLES;
  glGenFramebuffers(1, &msaa_framebuffer);
  glGenRenderbuffers(2, msaa_renderbuffers);

  use_texture_unit(UNIT_CORE_FRAME);          /* unit 0 stays active */
  make_texture(GL_RGB8, CORE_TEXTURE_SIZE, CORE_TEXTURE_SIZE, GL_RGB, GL_UNSIGNED_SHORT_5_6_5);
}

static void gl_init(void)
{
  create_programs();
  create_hle_vertex_array();
  create_textures();
}

/* ---- colours --------------------------------------------------------------------------------- */

/* CRAM BBBGGGRRR -> RGB565 the way GPGX's 16bpp output does it (each 3-bit channel doubled to
   the 4-bit scale of shadow/highlight, then widened to 5 or 6 bits) */
static uint16 cram_to_rgb565(uint16 cram_colour)
{
  int r = (cram_colour & 7) << 1, g = ((cram_colour >> 3) & 7) << 1, b = ((cram_colour >> 6) & 7) << 1;
  return ((r << 1) | (r >> 3)) << 11 | ((g << 2) | (g >> 2)) << 5 | ((b << 1) | (b >> 3));
}

/* RGB565 -> 0..255 per channel, the way svptrace's PPM writer expands it */
static void rgb565_to_rgb888(uint16 colour, unsigned char *rgb)
{
  rgb[0] = (colour >> 11) << 3;
  rgb[1] = ((colour >> 5) & 63) << 2;
  rgb[2] = (colour & 31) << 3;
}

/* GL's RGBA8 readback of an RGB565 colour, back to RGB565 */
static uint16 rgba8_to_rgb565(const unsigned char *rgba)
{
  return ((rgba[0] * 31 + 127) / 255) << 11 | ((rgba[1] * 63 + 127) / 255) << 5 | ((rgba[2] * 31 + 127) / 255);
}

/* sRGB to linear light */
static float srgb_to_linear(double value)
{
  if (value <= 0.04045) return value / 12.92;
  return pow((value + 0.055) / 1.055, 2.4);
}

/* ---- widescreen ---------------------------------------------------------------------------- */

/* The columns added on each side of the console's screen: WIDE_SIDE_COLUMNS in widescreen,
   for the H32 mode the game uses everywhere */
static int side_columns(void)
{
  if (widescreen && bitmap.viewport.w == VIEW_W) return WIDE_SIDE_COLUMNS;
  return 0;
}

/* The frame drawn, in game pixels: the console's screen plus the widescreen sides */
static int frame_width(void)
{
  return bitmap.viewport.w + 2 * side_columns();
}

/* Widescreen also fills the console's black borders above and below the race's 3D area: the
   HLE buffer then covers every line of the frame. These are the lines added above the SVP's
   buffer, and the buffer's height. */
static int buffer_top(void)
{
  return side_columns() ? VIEW_TOP : 0;
}

static int buffer_height(void)
{
  return side_columns() ? bitmap.viewport.h : VIEW_H;
}

/* Whether a command's 3D is a track under the sky, in a race or in the title demo: the sky
   follows its camera, and plane B extends into the widescreen sides */
static int is_track(int command)
{
  return command == HLE_COMMAND_RACE || command == HLE_COMMAND_FINISH || command == HLE_COMMAND_REPLAY ||
         command == HLE_COMMAND_REPLAY_FIXED || command == HLE_COMMAND_TITLE || command == HLE_COMMAND_RANKING;
}

/* Widescreen in a race (also its finish and replays): the columns the HUD's sides move out by,
   to the frame's edges; 0 elsewhere */
static int hud_shift(void)
{
  const hle_frame *f = hle_displayed();
  int race = f && (f->command == HLE_COMMAND_RACE || f->command == HLE_COMMAND_FINISH ||
                   f->command == HLE_COMMAND_REPLAY || f->command == HLE_COMMAND_REPLAY_FIXED);
  return race ? side_columns() : 0;
}

/* Whether the HLE image on screen is a track (is_track) */
static int showing_track(void)
{
  const hle_frame *f = hle_displayed();
  return f && is_track(f->command);
}

/* ---- the HLE 3D ------------------------------------------------------------------------------ */

/* The frame's polygons as triangles into hle_vertices (a quad is two); returns the vertex count.
   svp_vertices: drawn with the SVP's integer vertices, so the SVP's backface test applies. */
static int fill_hle_vertices(const hle_frame *f, int svp_vertices)
{
  static const int quad_corners[6] = { 0, 1, 2, 0, 2, 3 };
  int i, k, vertex_count = 0;

  for (i = 0; f && i < f->poly_count; i++)
  {
    const hle_poly *poly = &f->polys[i];
    float colour_1 = (poly->header >> 12) & 15;
    float colour_2 = (poly->header >> 8) & 15;
    float checkerboard = (poly->header & 0x20) ? 1 : 0;
    int corners = poly->vertex_count == 4 ? 6 : 3;
    int faces_camera = svp_vertices ? poly->front_on_svp_screen : poly->front_unrounded;
    if (!faces_camera) continue;

    for (k = 0; k < corners; k++)
    {
      int corner = quad_corners[k];
      float *out = &hle_vertices[vertex_count * FLOATS_PER_VERTEX];
      out[0] = poly->vertex[corner][0];
      out[1] = poly->vertex[corner][1];
      out[2] = poly->vertex[corner][2];
      out[3] = colour_1;
      out[4] = colour_2;
      out[5] = checkerboard;
      out[6] = poly->has_svp_screen;
      out[7] = poly->svp_screen[corner][0];
      out[8] = poly->svp_screen[corner][1];
      out[9] = poly->near_z;
      vertex_count++;
    }
  }
  return vertex_count;
}

/* Draw an HLE frame into the HLE texture at w x h and upload its sprites. The texture covers
   the SVP's 256x192 buffer, in widescreen enlarged to the whole frame (side_columns() on each
   side, buffer_top() lines above, buffer_height() lines). 9-bit colour:
   palette indices, 0 where nothing is drawn; full colour: linear colours with coverage. The
   dither pattern stays on the SVP's pixel grid. svp_vertices: use the SVP's integer projection
   where a polygon has one. */
static void render_hle(const hle_frame *f, int w, int h, int svp_vertices)
{
  int vertex_count = fill_hle_vertices(f, svp_vertices), multisample;

  use_texture_unit(UNIT_SVP_SPRITES);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, HLE_W);
  if (f) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, HLE_W, HLE_H, GL_RED_INTEGER, GL_UNSIGNED_BYTE, f->sprites);
  use_texture_unit(UNIT_CORE_FRAME);

  if (w != hle_texture_w || h != hle_texture_h)
  {
    use_texture_unit(UNIT_HLE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, NULL);
    use_texture_unit(UNIT_HLE_HAZE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, w, h, 0, GL_RED, GL_FLOAT, NULL);
    use_texture_unit(UNIT_CORE_FRAME);
    hle_texture_w = w;
    hle_texture_h = h;
  }

  /* key 4's MSAA, in full colour (palette indices cannot be averaged); -d never uses it */
  multisample = msaa && full_colour && msaa_samples > 1;
  if (multisample && (w != msaa_w || h != msaa_h))
  {
    glBindRenderbuffer(GL_RENDERBUFFER, msaa_renderbuffers[0]);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa_samples, GL_RGBA16F, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, msaa_renderbuffers[1]);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa_samples, GL_R16F, w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, msaa_framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msaa_renderbuffers[0]);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, msaa_renderbuffers[1]);
    glDrawBuffers(2, HLE_OUTPUTS);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
      fprintf(stderr, "MSAA: %d samples of RGBA16F and R16F not supported, drawing without\n", msaa_samples);
      msaa_samples = 0;
      multisample = 0;
    }
    msaa_w = w;
    msaa_h = h;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, multisample ? msaa_framebuffer : hle_framebuffer);
  glViewport(0, 0, w, h);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);

  glUseProgram(hle_program);
  set_vec2(hle_program, "scale", (float)w / (VIEW_W + 2 * side_columns()), (float)h / buffer_height());
  set_float(hle_program, "buffer_width", VIEW_W + 2 * side_columns());
  set_float(hle_program, "buffer_side", side_columns());
  set_float(hle_program, "buffer_height", buffer_height());
  set_float(hle_program, "buffer_top", buffer_top());
  set_int(hle_program, "use_svp_screen", svp_vertices);
  set_int(hle_program, "full_colour", full_colour);
  set_int(hle_program, "haze", far_view == FAR_VIEW_HAZE && full_colour);
  set_float(hle_program, "haze_start", haze_start * 1000.0f);
  set_float(hle_program, "haze_end", (haze_end > haze_start ? haze_end : haze_start + 1) * 1000.0f);
  if (full_colour) glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   /* premultiplied */

  glBindVertexArray(hle_vao);
  glBindBuffer(GL_ARRAY_BUFFER, hle_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, vertex_count * FLOATS_PER_VERTEX * sizeof(float), hle_vertices, GL_STREAM_DRAW);
  glDrawArrays(GL_TRIANGLES, 0, vertex_count);   /* in list order: far to near, like the SVP */

  if (multisample)                                /* resolve: each pixel the mean of its samples */
  {
    int i;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msaa_framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, hle_framebuffer);
    for (i = 0; i < 2; i++)                       /* a blit copies one buffer: colour, then haze */
    {
      glReadBuffer(HLE_OUTPUTS[i]);
      glDrawBuffer(HLE_OUTPUTS[i]);
      glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glReadBuffer(GL_COLOR_ATTACHMENT0);           /* as the -d checks read it */
    glDrawBuffers(2, HLE_OUTPUTS);
  }

  glDisable(GL_BLEND);
  glBindVertexArray(quad_vao);
  glUseProgram(blit_program);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* ---- uploading an emulated frame ------------------------------------------------------------ */

/* The CRAM of every line, as RGB565 and in linear light. The linear colours are those the
   9-bit path shows (through RGB565), so solid colours look the same in both. */
static void upload_palettes(void)
{
  static uint16 palette[LAYERS_H][64];
  static float linear_palette[LAYERS_H][64][3];
  static float linear_of_cram[512][3];      /* every 9-bit CRAM colour in linear light */
  static int table_ready = 0;
  int line, i;

  if (!table_ready)
  {
    for (i = 0; i < 512; i++)
    {
      uint16 rgb565 = cram_to_rgb565(i);
      linear_of_cram[i][0] = srgb_to_linear((rgb565 >> 11) / 31.0);
      linear_of_cram[i][1] = srgb_to_linear(((rgb565 >> 5) & 63) / 63.0);
      linear_of_cram[i][2] = srgb_to_linear((rgb565 & 31) / 31.0);
    }
    table_ready = 1;
  }

  for (line = 0; line < bitmap.viewport.h; line++)
    for (i = 0; i < 64; i++)
    {
      palette[line][i] = cram_to_rgb565(layers.cram[line][i]);
      memcpy(linear_palette[line][i], linear_of_cram[layers.cram[line][i] & 511], sizeof(linear_of_cram[0]));
    }

  use_texture_unit(UNIT_LINEAR_PALETTE);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 64);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, bitmap.viewport.h, GL_RGB, GL_FLOAT, linear_palette);
  use_texture_unit(UNIT_PALETTE);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 64);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 64, bitmap.viewport.h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, palette);
}

/* Full colour: plane B with its dither blended (background_shader), at the layers' resolution */
static void draw_background(void)
{
  glBindFramebuffer(GL_FRAMEBUFFER, background_framebuffer);
  glViewport(0, 0, bitmap.viewport.w, bitmap.viewport.h);
  glUseProgram(background_program);
  set_vec2(background_program, "frame_size", bitmap.viewport.w, bitmap.viewport.h);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glUseProgram(blit_program);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* After an emulated frame: our layers and their palettes, the core's own
   frame (its bitmap viewport, RGB565), and the blended background */
static void upload_frame(void)
{
  int w = bitmap.viewport.w, h = bitmap.viewport.h;

  upload_palettes();

  use_texture_unit(UNIT_LAYERS);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, LAYERS_W);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, layers.px);

  use_texture_unit(UNIT_CORE_FRAME);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, bitmap.pitch / 2);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_SHORT_5_6_5,
                  bitmap.data + bitmap.viewport.y * bitmap.pitch + bitmap.viewport.x * 2);

  draw_background();
}

/* ---- drawing to the screen ---------------------------------------------------------------- */

/* The core's frame as a textured quad */
static void blit(int x, int y, int w, int h)
{
  set_vec2(blit_program, "uv_scale", (float)bitmap.viewport.w / CORE_TEXTURE_SIZE,
           (float)bitmap.viewport.h / CORE_TEXTURE_SIZE);
  glViewport(x, y, w, h);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* The frame from our layers, with the HLE 3D (render_hle) when use_hle */
static void composite(int use_hle, int x, int y, int w, int h)
{
  glUseProgram(composite_program);
  set_int(composite_program, "use_hle", use_hle);
  set_int(composite_program, "full_colour", full_colour);
  set_float(composite_program, "sky_shift", sky_shift);
  set_int(composite_program, "wrap_background", use_hle && showing_track());
  set_float(composite_program, "hud_shift", use_hle ? hud_shift() : 0);
  set_int(composite_program, "extend_sprites", use_hle && hle_displayed() && hle_displayed()->sprites_are_picture);
  set_vec2(composite_program, "frame_size", frame_width(), bitmap.viewport.h);
  set_vec2(composite_program, "layers_size", bitmap.viewport.w, bitmap.viewport.h);
  set_float(composite_program, "side_columns", side_columns());
  set_vec2(composite_program, "hle_area", VIEW_TOP - buffer_top(), buffer_height());
  glViewport(x, y, w, h);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glUseProgram(blit_program);
}

/* Where a frame frame_w game pixels wide goes in the area x0..x0+w by h: letterboxed, 8:7
   pixel aspect. rect gets x, y, width, height. */
static void fit(int frame_w, int x0, int w, int h, int rect[4])
{
  double aspect = frame_w * PIXEL_ASPECT / bitmap.viewport.h;
  int fitted_w = w, fitted_h = h;
  if ((double)w / h > aspect) fitted_w = (int)lround(h * aspect);
  else fitted_h = (int)lround(w / aspect);
  rect[0] = x0 + (w - fitted_w) / 2;
  rect[1] = (h - fitted_h) / 2;
  rect[2] = fitted_w;
  rect[3] = fitted_h;
}

/* How far to move plane B (the sky) so that it matches the camera yaw of an interpolated frame.
   The game scrolls plane B to floor(-yaw / 16) (docs/screen-layers.md), but only once per tick
   and a few frames ahead of its 3D. Here the sky follows the yaw the frame is drawn with,
   unrounded, so it turns as smoothly as the 3D. Plane B is 256 pixels wide, as wide as the
   screen, so the shift is wrapped to -128 .. 128. */
static float sky_shift_for(const hle_frame *f)
{
  double wanted = -f->camera_yaw / 16;
  double scrolled = layers.hscroll_b[VIEW_TOP + VIEW_H / 2];   /* what the game set, mid 3D area */
  double shift = wanted - scrolled;
  shift -= 256 * floor(shift / 256 + 0.5);
  return (float)shift;
}

/* One display refresh into a w x h window: the core's frame (views original and side by side)
   and the composite with the HLE 3D (views HLE and side by side). sub_frame is how far this
   refresh is past the last emulated frame (for smooth motion). */
static void draw(int w, int h, double sub_frame)
{
  static unsigned last_image_number;
  int original_rect[4], hle_rect[4];
  const hle_frame *f = hle_displayed();

  if (view == VIEW_SIDE_BY_SIDE)
  {
    fit(bitmap.viewport.w, 0, w / 2, h, original_rect);
    fit(frame_width(), w / 2, w - w / 2, h, hle_rect);
  }
  else
  {
    fit(bitmap.viewport.w, 0, w, h, original_rect);
    fit(frame_width(), 0, w, h, hle_rect);
  }

  sky_shift = 0;
  if (view != VIEW_ORIGINAL && f)
  {
    /* resolution times the SVP's buffer (widened in widescreen), or as many pixels as the 3D
       area covers on screen */
    int buffer_w = VIEW_W + 2 * side_columns();
    int scale = resolution;
    int hle_w = scale ? buffer_w * scale : hle_rect[2] * buffer_w / frame_width();
    int hle_h = scale ? buffer_height() * scale : hle_rect[3] * buffer_height() / bitmap.viewport.h;
    int svp_vertices = use_svp_vertices;
    const hle_frame *interpolated = smooth_motion ? hle_smooth(sub_frame, frames_per_image()) : NULL;

    if (interpolated)
    {
      f = interpolated;
      if (is_track(f->command)) sky_shift = sky_shift_for(f);
      /* The SVP's integer vertices are off by up to a pixel or so. A whole tick's motion hides
         that, a single frame's does not: in a turn the scene would judder. */
      svp_vertices = 0;
    }
    if (hle_w < buffer_w) hle_w = buffer_w;
    if (hle_h < buffer_height()) hle_h = buffer_height();
    render_hle(f, hle_w, hle_h, svp_vertices);
    if (f->image_number != last_image_number) images_shown++;
    last_image_number = f->image_number;
  }

  glViewport(0, 0, w, h);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  if (view != VIEW_HLE) blit(original_rect[0], original_rect[1], original_rect[2], original_rect[3]);
  if (view != VIEW_ORIGINAL) composite(hle_displayed() != NULL, hle_rect[0], hle_rect[1], hle_rect[2], hle_rect[3]);
}

/* ---- input and emulation -------------------------------------------------------------------- */

/* The in-game menu (see "the in-game menu" below): while it is open the game is paused */
static struct {
  int open;
  int selected;              /* the item under the cursor */
  const char *status;        /* shown beside the selected item after saving or loading */
  int wait_for_release;      /* after closing it: no game input until every button is up */
} menu;

/* The first game controller. SDL names its buttons by their places on an Xbox pad (A bottom,
   B right, X left, Y top), so PlayStation and Switch Pro pads map the same way: the face
   buttons sit like the Mega Drive's, A B C along the bottom row and Z on top. (Nintendo pads
   need SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS off, set in main, or SDL names their
   buttons by their labels, which are swapped.) Back and Guide open the menu
   (handle_controller_button). */
static SDL_GameController *controller;

/* How far a stick or trigger must go, of 32767, to count as pressed: the game's steering is
   digital, so the left stick works as the D-pad. */
#define AXIS_PRESSED 12000

static const struct
{
  SDL_GameControllerButton button;
  uint16 input;
} controller_buttons[] =
{
  { SDL_CONTROLLER_BUTTON_DPAD_UP,       INPUT_UP    },
  { SDL_CONTROLLER_BUTTON_DPAD_DOWN,     INPUT_DOWN  },
  { SDL_CONTROLLER_BUTTON_DPAD_LEFT,     INPUT_LEFT  },
  { SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    INPUT_RIGHT },
  { SDL_CONTROLLER_BUTTON_X,             INPUT_A     },
  { SDL_CONTROLLER_BUTTON_A,             INPUT_B     },   /* gas in a race */
  { SDL_CONTROLLER_BUTTON_B,             INPUT_C     },
  { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  INPUT_X     },   /* X, Y, Z pick the camera view */
  { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, INPUT_Y     },
  { SDL_CONTROLLER_BUTTON_Y,             INPUT_Z     },
  { SDL_CONTROLLER_BUTTON_START,         INPUT_START },
};

static int axis_pressed(SDL_GameControllerAxis axis, int direction)
{
  return SDL_GameControllerGetAxis(controller, axis) * direction > AXIS_PRESSED;
}

/* Opens a controller when one is plugged in and drops it when it is pulled out */
static uint16 read_controller(void)
{
  uint16 buttons = 0;
  int i;
  if (controller && !SDL_GameControllerGetAttached(controller))
  {
    SDL_GameControllerClose(controller);
    controller = NULL;
  }
  for (i = 0; !controller && i < SDL_NumJoysticks(); i++)
    if (SDL_IsGameController(i)) controller = SDL_GameControllerOpen(i);
  if (!controller) return 0;

  for (i = 0; i < (int)(sizeof controller_buttons / sizeof controller_buttons[0]); i++)
    if (SDL_GameControllerGetButton(controller, controller_buttons[i].button)) buttons |= controller_buttons[i].input;
  if (axis_pressed(SDL_CONTROLLER_AXIS_LEFTY, -1)) buttons |= INPUT_UP;
  if (axis_pressed(SDL_CONTROLLER_AXIS_LEFTY, 1)) buttons |= INPUT_DOWN;
  if (axis_pressed(SDL_CONTROLLER_AXIS_LEFTX, -1)) buttons |= INPUT_LEFT;
  if (axis_pressed(SDL_CONTROLLER_AXIS_LEFTX, 1)) buttons |= INPUT_RIGHT;
  if (axis_pressed(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 1)) buttons |= INPUT_A;
  if (axis_pressed(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 1)) buttons |= INPUT_B;
  return buttons;
}

/* The keyboard and the controller together */
static uint16 read_pad(void)
{
  const Uint8 *key = SDL_GetKeyboardState(NULL);
  uint16 buttons = read_controller();
  if (key[SDL_SCANCODE_UP]) buttons |= INPUT_UP;
  if (key[SDL_SCANCODE_DOWN]) buttons |= INPUT_DOWN;
  if (key[SDL_SCANCODE_LEFT]) buttons |= INPUT_LEFT;
  if (key[SDL_SCANCODE_RIGHT]) buttons |= INPUT_RIGHT;
  if (key[SDL_SCANCODE_A]) buttons |= INPUT_A;
  if (key[SDL_SCANCODE_S]) buttons |= INPUT_B;
  if (key[SDL_SCANCODE_D]) buttons |= INPUT_C;
  if (key[SDL_SCANCODE_F]) buttons |= INPUT_START;
  if (key[SDL_SCANCODE_Z]) buttons |= INPUT_X;
  if (key[SDL_SCANCODE_X]) buttons |= INPUT_Y;
  if (key[SDL_SCANCODE_C]) buttons |= INPUT_Z;
  if (key[SDL_SCANCODE_V]) buttons |= INPUT_MODE;
  /* a real D-pad cannot press opposite directions at once */
  if (buttons & INPUT_UP) buttons &= ~INPUT_DOWN;
  if (buttons & INPUT_LEFT) buttons &= ~INPUT_RIGHT;
  /* the button that closed the menu does not reach the game */
  if (menu.wait_for_release)
  {
    if (buttons) return 0;
    menu.wait_for_release = 0;
  }
  return buttons;
}

/* Key 6 / -s: the SVP's clock for the next frame. Only in races: elsewhere a faster SVP shortens
   screens such as the SEGA logo, the title and the menus, which moves every input script's
   timing. */
static void set_svp_clock(void)
{
  const hle_frame *f = hle_displayed();
  if (no_slowdown && f && f->command == HLE_COMMAND_RACE) SVP_cycles = SVP_CYCLES * SVP_FAST;
  else SVP_cycles = SVP_CYCLES;
}

/* ---- -d: checks and images ----------------------------------------------------------------- */

static int write_ppm(const char *name, int w, int h, const unsigned char *rgb)
{
  FILE *f = fopen(name, "wb");
  if (!f) return 1;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  fwrite(rgb, 1, w * h * 3, f);
  fclose(f);
  printf("wrote %s (%dx%d)\n", name, w, h);
  return 0;
}

/* A palette-index image (0 = transparent) as RGB, transparent in magenta */
static void index_image_to_rgb(const unsigned char *indices, int count, unsigned char *rgb)
{
  int i;
  for (i = 0; i < count; i++)
  {
    unsigned char *out = rgb + i * 3;
    if (indices[i])
    {
      rgb565_to_rgb888(cram_to_rgb565(*(uint16 *)&cram[indices[i] << 1]), out);
    }
    else
    {
      out[0] = 255;
      out[1] = 0;
      out[2] = 255;
    }
  }
}

/* The w x h image at the bottom left of the bound framebuffer, top-down, as RGB565 */
static void read_rgb565(int w, int h, uint16 *out)
{
  static unsigned char rgba[LAYERS_W * LAYERS_H * 4];
  int x, y;
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  for (y = 0; y < h; y++)               /* GL rows are bottom-up */
    for (x = 0; x < w; x++) out[y * w + x] = rgba8_to_rgb565(rgba + ((h - 1 - y) * w + x) * 4);
}

/* The w x h image at the bottom left of the bound framebuffer as a PPM, 8 bits per channel */
static int write_framebuffer(const char *name, int w, int h)
{
  unsigned char *rgba = malloc(w * h * 4), *rgb = malloc(w * h * 3);
  int x, y, result;
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++) memcpy(rgb + (y * w + x) * 3, rgba + ((h - 1 - y) * w + x) * 4, 3);
  result = write_ppm(name, w, h, rgb);
  free(rgba);
  free(rgb);
  return result;
}

static int write_rgb565(const char *name, int w, int h, const uint16 *pixels)
{
  static unsigned char rgb[LAYERS_W * LAYERS_H * 3];
  int i;
  for (i = 0; i < w * h; i++) rgb565_to_rgb888(pixels[i], rgb + i * 3);
  return write_ppm(name, w, h, rgb);
}

/* A framebuffer with a w x h RGBA8 renderbuffer, left bound */
static GLuint create_offscreen_framebuffer(int w, int h)
{
  GLuint framebuffer, renderbuffer;
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glGenRenderbuffers(1, &renderbuffer);
  glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
  return framebuffer;
}

/* Pixel counts of compare_hle_with_svp */
typedef struct {
  int both;           /* drawn by both */
  int same_colour;    /* of those, the same palette index */
  int only_svp;
  int only_hle;
} comparison;

/* The HLE 3D and sprites at the SVP's resolution and vertices, against plane A as the core
   drew it (the SVP's image). The palette indices go into svp_image and hle_image. */
static comparison compare_hle_with_svp(unsigned char *svp_image, unsigned char *hle_image)
{
  static unsigned char rgba[VIEW_W * VIEW_H * 4];
  const hle_frame *f = hle_displayed();
  comparison result = { 0, 0, 0, 0 };
  int x, y, i;

  render_hle(f, VIEW_W, VIEW_H, 1);
  glBindFramebuffer(GL_FRAMEBUFFER, hle_framebuffer);
  glReadPixels(0, 0, VIEW_W, VIEW_H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  for (y = 0; y < VIEW_H; y++)
    for (x = 0; x < VIEW_W; x++)
    {
      int hle_index = rgba[((VIEW_H - 1 - y) * VIEW_W + x) * 4];   /* GL rows are bottom-up */
      svp_image[y * VIEW_W + x] = layers.px[VIEW_TOP + y][x][LAYER_PLANE_A] & LAYER_COLOUR;
      hle_image[y * VIEW_W + x] = f->sprites[y][x] ? f->sprites[y][x] : hle_index;
    }

  for (i = 0; i < VIEW_W * VIEW_H; i++)
  {
    if (svp_image[i] && hle_image[i])
    {
      result.both++;
      if (svp_image[i] == hle_image[i]) result.same_colour++;
    }
    else if (svp_image[i]) result.only_svp++;
    else if (hle_image[i]) result.only_hle++;
  }
  return result;
}

/* Our layers composited without the HLE against the core's frame; returns the number of lines
   that differ, and reports the first difference of the run */
static int check_layers(int frame, int w, int h, int report_first)
{
  static uint16 ours[LAYERS_W * LAYERS_H];
  int x, y, bad_lines = 0;

  composite(0, 0, 0, w, h);
  read_rgb565(w, h, ours);
  for (y = 0; y < h; y++)
  {
    const uint16 *core_line = (const uint16 *)(bitmap.data + (y + bitmap.viewport.y) * bitmap.pitch) + bitmap.viewport.x;
    for (x = 0; x < w && core_line[x] == ours[y * w + x]; x++) ;
    if (x == w) continue;
    if (bad_lines == 0 && report_first) printf("layers: first difference at frame %d line %d x %d\n", frame, y, x);
    bad_lines++;
  }
  return bad_lines;
}

/* -F: the newest tick (hle_smooth) with the console's view distance, the far view and the far
   view with haze (view_NNNNNN_near, _far, _haze.ppm), at the -r resolution, in widescreen with
   wide set */
static void write_view_distance_images(int n, int h, int wide)
{
  static const char *const names[3] = { "near", "far", "haze" };   /* FAR_VIEW_OFF, ON, HAZE */
  int scale = resolution > 1 ? resolution : 1, setting = far_view;
  char name[40];

  widescreen = wide;
  for (far_view = FAR_VIEW_OFF; far_view <= FAR_VIEW_HAZE; far_view++)
  {
    const hle_frame *f;
    int frame_w, frame_h;
    hle_set_far_view(far_view != FAR_VIEW_OFF);
    f = hle_smooth(0, 0);
    if (!f) break;
    frame_w = frame_width() * scale;
    frame_h = h * scale;
    sky_shift = is_track(f->command) ? sky_shift_for(f) : 0;
    render_hle(f, (VIEW_W + 2 * side_columns()) * scale, buffer_height() * scale, 0);
    create_offscreen_framebuffer(frame_w, frame_h);
    composite(1, 0, 0, frame_w, frame_h);
    sprintf(name, "view_%06d_%s.ppm", n, names[far_view]);
    write_framebuffer(name, frame_w, frame_h);
  }
  far_view = setting;
  hle_set_far_view(0);
  sky_shift = 0;
}

/* The images of the last frame of -d n: as the core drew it, the HLE against the SVP, the
   frame with the HLE in 9-bit and in full colour, at the -r resolution, and with wide set, in
   widescreen */
static void write_final_images(int n, GLuint framebuffer, int w, int h, int wide)
{
  static uint16 core_frame[LAYERS_W * LAYERS_H], ours[LAYERS_W * LAYERS_H];
  static unsigned char svp_image[VIEW_W * VIEW_H], hle_image[VIEW_W * VIEW_H];
  const hle_frame *f = hle_displayed();
  char name[32];

  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  blit(0, 0, w, h);
  read_rgb565(w, h, core_frame);
  sprintf(name, "vrr_%06d.ppm", n);
  write_rgb565(name, w, h, core_frame);

  if (f)
  {
    static unsigned char rgb[VIEW_W * VIEW_H * 3];
    comparison c = compare_hle_with_svp(svp_image, hle_image);
    int changed = 0, i;

    printf("HLE vs SVP (%d polygons): %d pixels drawn by both, %.2f%% same colour; only SVP %d, only HLE %d\n",
           f->poly_count, c.both, 100.0 * c.same_colour / (c.both ? c.both : 1), c.only_svp, c.only_hle);
    index_image_to_rgb(svp_image, VIEW_W * VIEW_H, rgb);
    sprintf(name, "svp3d_%06d.ppm", n);
    write_ppm(name, VIEW_W, VIEW_H, rgb);
    index_image_to_rgb(hle_image, VIEW_W * VIEW_H, rgb);
    sprintf(name, "hle3d_%06d.ppm", n);
    write_ppm(name, VIEW_W, VIEW_H, rgb);

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    composite(1, 0, 0, w, h);
    read_rgb565(w, h, ours);
    for (i = 0; i < w * h; i++) changed += ours[i] != core_frame[i];
    sprintf(name, "hle_%06d.ppm", n);
    write_rgb565(name, w, h, ours);
    printf("with the HLE 3D: %d pixels differ from the core's frame\n", changed);
  }

  full_colour = 1;
  if (f) render_hle(f, VIEW_W, VIEW_H, 1);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  composite(f != NULL, 0, 0, w, h);
  sprintf(name, "full_%06d.ppm", n);
  write_framebuffer(name, w, h);

  if (f && resolution > 1)       /* the frame with the 3D at that resolution, to look at */
  {
    int big_w = w * resolution, big_h = h * resolution;
    render_hle(f, VIEW_W * resolution, VIEW_H * resolution, 0);
    create_offscreen_framebuffer(big_w, big_h);
    composite(1, 0, 0, big_w, big_h);
    sprintf(name, "hle_%06d_x%d.ppm", n, resolution);
    write_framebuffer(name, big_w, big_h);
  }

  if (f && wide)                 /* -W: the frame in widescreen, at the resolution given (1x at least) */
  {
    int scale = resolution > 1 ? resolution : 1, wide_w, wide_h;
    widescreen = 1;
    wide_w = frame_width() * scale;
    wide_h = h * scale;
    render_hle(f, (VIEW_W + 2 * side_columns()) * scale, buffer_height() * scale, scale == 1);
    create_offscreen_framebuffer(wide_w, wide_h);
    composite(1, 0, 0, wide_w, wide_h);
    sprintf(name, "wide_%06d.ppm", n);
    write_framebuffer(name, wide_w, wide_h);
  }
}

/* -d: run frames 0..n. Every frame, our layers composited without the HLE must equal the
   core's frame exactly. Frame n is written as the core drew it (vrr_N.ppm, same format as
   svptrace -w) and with the HLE 3D (hle_N.ppm), and the HLE 3D is compared with plane A's
   SVP image pixel by pixel (svp3d_N.ppm, hle3d_N.ppm). From frame t on (t >= 0), that
   comparison runs on every frame that has an HLE image, which catches the HLE showing the
   wrong draw list (timing) or stray polygons. All of that is in 9-bit colour; frame n is also
   written in full colour (full_N.ppm, and hle_N_xR.ppm at -r R). */
static int dump(int n, int t, const int *press_frame, const uint16 *press_pad, int press_count, int view_distance_images)
{
  static unsigned char svp_image[VIEW_W * VIEW_H], hle_image[VIEW_W * VIEW_H];
  GLuint framebuffer;
  int frame, w = 0, h = 0, i, bad_frames = 0, bad_lines = 0;
  int tracked_frames = 0, low_frames = 0, worst_frame = -1, blank_frames = 0;
  double match_sum = 0, worst_match = 101;
  int wide = widescreen;

  full_colour = 0;
  widescreen = 0;              /* the checks compare with the console's own 4:3 frame */
  framebuffer = create_offscreen_framebuffer(LAYERS_W, LAYERS_H);

  for (frame = 0; frame <= n; frame++)
  {
    int frame_bad_lines;
    for (i = 0; i < press_count; i++)
      if (press_frame[i] == frame) core_pad = press_pad[i];
    set_svp_clock();
    system_frame_gen(0);
    audio_update(samples);
    hle_frame_done();

    w = bitmap.viewport.w;
    h = bitmap.viewport.h;
    upload_frame();
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    frame_bad_lines = check_layers(frame, w, h, bad_frames == 0);
    if (frame_bad_lines)
    {
      bad_frames++;
      bad_lines += frame_bad_lines;
    }

    if (t >= 0 && frame >= t && hle_displayed())
    {
      comparison c = compare_hle_with_svp(svp_image, hle_image);
      double match = 100.0 * c.same_colour / (c.both ? c.both : 1);
      /* no pixel drawn by both, as when the game fades the screen out: nothing to compare */
      if (!c.both)
      {
        blank_frames++;
        continue;
      }
      tracked_frames++;
      match_sum += match;
      if (match < worst_match)
      {
        worst_match = match;
        worst_frame = frame;
      }
      if (match < 85 && low_frames++ < 20) printf("HLE vs SVP: frame %d only %.1f%% same colour\n", frame, match);
    }
  }

  printf("layers composite vs core: %d of %d frames differ (%d lines)\n", bad_frames, n + 1, bad_lines);
  if (t >= 0)
    printf("HLE vs SVP on %d frames from %d: mean %.2f%%, lowest %.2f%% (frame %d), %d below 85%%"
           " (%d frames with nothing to compare left out)\n",
           tracked_frames, t, match_sum / (tracked_frames ? tracked_frames : 1),
           tracked_frames ? worst_match : 0, worst_frame, low_frames, blank_frames);

  write_final_images(n, framebuffer, w, h, wide);
  if (view_distance_images) write_view_distance_images(n, h, wide);
  return 0;
}

/* ---- the window ------------------------------------------------------------------------------ */

/* The real refresh rate, from the median interval of presenting a few blank frames */
static double measure_refresh(SDL_Window *window)
{
  double times[40], intervals[39];
  int i, j;
  for (i = 0; i < 40; i++)
  {
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(window);
    times[i] = (double)SDL_GetPerformanceCounter() / SDL_GetPerformanceFrequency();
  }
  for (i = 0; i < 39; i++) intervals[i] = times[i + 1] - times[i];
  for (i = 0; i < 39; i++)                /* sort, for the median */
    for (j = i + 1; j < 39; j++)
      if (intervals[j] < intervals[i])
      {
        double swap = intervals[i];
        intervals[i] = intervals[j];
        intervals[j] = swap;
      }
  return 1.0 / intervals[19];
}

/* Make the window's width fit the frame's aspect at its current height */
static void fit_window_width(SDL_Window *window)
{
  int window_w, window_h;
  SDL_GetWindowSize(window, &window_w, &window_h);
  SDL_SetWindowSize(window, (int)lround(window_h * frame_width() * PIXEL_ASPECT / bitmap.viewport.h), window_h);
}

/* The window title: the view and the settings that apply to it */
static double now_seconds(void)
{
  return (double)SDL_GetPerformanceCounter() / SDL_GetPerformanceFrequency();
}

/* The HLE images drawn a second, measured over the last second or so */
static int measured_image_rate(void)
{
  static double since;
  static unsigned counted;
  static int rate;
  double now = now_seconds();
  if (now - since >= 1.0)
  {
    rate = (int)((images_shown - counted) / (now - since) + 0.5);
    counted = images_shown;
    since = now;
  }
  return rate;
}

static void update_title(SDL_Window *window)
{
  static char shown[280];
  char title[280] = TITLE;
  int image_rate = measured_image_rate();   /* measured all the time, so it is current when shown */

  if (view == VIEW_ORIGINAL)
  {
    snprintf(title, sizeof(title), TITLE " - %s", view_names[view]);
  }
  else
  {
    /* the HLE only draws command 2 (races); other screens keep the SVP's 3D */
    char size[24];
    if (resolution == RESOLUTION_WINDOW)
      sprintf(size, "%dx%d", hle_texture_w, hle_texture_h);
    else
      sprintf(size, "%dx", resolution);
    if (msaa && full_colour) strcat(size, " [4] MSAA");
    snprintf(title, sizeof(title), TITLE " - %s - [8] %s - [5] %s - %s", view_names[view],
             widescreen ? "16:9" : "4:3", full_colour ? "full colour" : "9-bit colour",
             !hle_displayed() ? "SVP 3D on this screen (no HLE for it yet)" : "");
    if (hle_displayed())
      snprintf(title + strlen(title), sizeof(title) - strlen(title), "[9] %s - [7] %s (showing %d) - [3] 3D %s - [2] %s",
               far_view == FAR_VIEW_HAZE ? "far view, haze" : far_view ? "far view" : "original distance", smooth_motion_name(),
               image_rate, size,
               use_svp_vertices && !smooth_motion ? "SVP integer vertices" : "float vertices");
  }
  if (no_slowdown) snprintf(title + strlen(title), sizeof(title) - strlen(title), " - [6] SVP overclock");
  if (strcmp(title, shown)) SDL_SetWindowTitle(window, strcpy(shown, title));
}

/* ---- audio ---------------------------------------------------------------------------------- */

static struct {
  SDL_AudioDeviceID device;    /* 0 = no audio */
  int started;                 /* playback starts once the queue has reached its target */
  int underruns;               /* times the queue ran dry */
  long lowest_queue;           /* in samples, -1 = not measured yet */
} audio = { 0, 0, 0, -1 };

static void open_audio(void)
{
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = SAMPLE_RATE;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 512;
  audio.device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (!audio.device) fprintf(stderr, "no audio: %s\n", SDL_GetError());
}

/* Queue one frame's samples, and nudge the core's sample rate so that the queue stays near
   AUDIO_TARGET: a low queue makes the next frames a little longer */
static void queue_audio(int sample_count, double fps)
{
  long queued;
  double error;
  if (!audio.device) return;

  queued = SDL_GetQueuedAudioSize(audio.device) / 4;     /* 4 bytes per stereo sample */
  error = (double)(AUDIO_TARGET - queued) / AUDIO_TARGET;
  if (audio.started && queued == 0) audio.underruns++;
  if (audio.started && (audio.lowest_queue < 0 || queued < audio.lowest_queue)) audio.lowest_queue = queued;

  SDL_QueueAudio(audio.device, samples, sample_count * 4);
  if (!audio.started && queued + sample_count >= AUDIO_TARGET)
  {
    SDL_PauseAudioDevice(audio.device, 0);
    audio.started = 1;
  }

  if (error > 1) error = 1;
  if (error < -1) error = -1;
  audio_set_rate(SAMPLE_RATE, fps * (1.0 - AUDIO_NUDGE * error));
}

/* ---- the in-game menu and the keys ---------------------------------------------------------- */

/* Esc (Back or Guide on a controller) opens a panel over the paused game: the settings with
   their keys and values, saving and loading the state, and quitting. It is drawn in the
   game's own 8x16 font, read from the ROM. */
#define MENU_FONT 0x782D0    /* ROM word address: glyphs from the space to 'z', 0x20 words each,
                                4 bits a pixel (1 = outline, any other = fill); the title's
                                course ranking uses it (docs/svp-commands.md 6.17) */
#define ROM_WORD(word) (((uint16 *)cart.rom)[(word) & 0xfffff])

enum { MENU_SAVE = KEY_SETTINGS, MENU_LOAD, MENU_QUIT, MENU_ITEMS };

static const char *const setting_labels[KEY_SETTINGS] = {
  "View", "Vertices", "3D resolution", "MSAA", "Colour", "SVP overclock", "Smooth motion",
  "Widescreen", "Far view"
};

/* Colours, RGBA premultiplied by alpha */
static const uint8 MENU_PANEL[4]     = { 8, 12, 30, 230 };
static const uint8 MENU_CURSOR[4]    = { 30, 56, 150, 255 };
static const uint8 MENU_OUTLINE[4]   = { 0, 0, 0, 255 };
static const uint8 MENU_TITLE[4]     = { 255, 214, 0, 255 };
static const uint8 MENU_LABEL[4]     = { 235, 235, 235, 255 };
static const uint8 MENU_VALUE[4]     = { 110, 200, 255, 255 };
static const uint8 MENU_HINT[4]      = { 140, 140, 150, 255 };

static uint8 menu_image[MENU_H][MENU_W][4];     /* the panel, top line first */

/* The current value of setting index (key index + 1), as the menu shows it */
static const char *setting_value(int index)
{
  static char scale[8];
  switch (index)
  {
    case 0: return view == VIEW_HLE ? "HLE" : view == VIEW_ORIGINAL ? "Original" : "Side by side";
    case 1: return use_svp_vertices && !smooth_motion ? "SVP integer" : "Float";   /* smooth motion draws float */
    case 2:
      if (resolution == RESOLUTION_WINDOW) return "Window";
      sprintf(scale, "%dx", resolution);
      return scale;
    case 3: return msaa ? "On" : "Off";
    case 4: return full_colour ? "Full" : "9-bit";
    case 5: return no_slowdown ? "On" : "Off";
    case 6: return smooth_motion_name();
    case 7: return widescreen ? "16:9" : "4:3";
    default: return far_view == FAR_VIEW_HAZE ? "On + haze" : far_view ? "On" : "Off";
  }
}

/* Whether a setting cannot be changed now because it would have no effect: the original view
   shows the core's own frame, which only the view and the SVP overclock (key 6, it changes how
   the game runs) affect; smooth motion always draws float vertices (see draw), so key 2 waits
   until it is off; and MSAA (key 4) has nothing to average in 9-bit colour. The menu shows such
   a setting greyed out. */
static int setting_locked(int index)
{
  if (view == VIEW_ORIGINAL) return index != 0 && index != 5;
  return (index == 1 && smooth_motion) || (index == 3 && !full_colour);
}

/* Setting index (key index + 1) one step forward (1) or back (-1); vrr.ini is rewritten */
static void change_setting(SDL_Window *window, int index, int step)
{
  static const int resolutions[] = { 1, 2, 4, RESOLUTION_WINDOW };                           /* key 3's order */
  static const int smooth_orders[] = { SMOOTH_OFF, SMOOTH_30, SMOOTH_60, SMOOTH_DISPLAY };    /* key 7's */
  int i = 0;
  if (setting_locked(index)) return;
  switch (index)
  {
    case 0: view = (view + VIEW_COUNT + step) % VIEW_COUNT; break;
    case 1: use_svp_vertices = !use_svp_vertices; break;
    case 2:
      while (i < 3 && resolutions[i] != resolution) i++;
      resolution = resolutions[(i + 4 + step) % 4];
      use_svp_vertices = resolution == 1;               /* the SVP's integer vertices only at 1x */
      break;
    case 3: msaa = !msaa; break;
    case 4: full_colour = !full_colour; break;
    case 5: no_slowdown = !no_slowdown; break;
    case 6:
      while (i < 3 && smooth_orders[i] != smooth_motion) i++;
      smooth_motion = smooth_orders[(i + 4 + step) % 4];
      break;
    case 7:
      widescreen = !widescreen;
      fit_window_width(window);
      break;
    default:
      far_view = (far_view + 3 + step) % 3;              /* off, on, on with haze */
      hle_set_far_view(far_view != FAR_VIEW_OFF);
      break;
  }
  save_settings();
}

/* The game pauses while the menu is open, its sound too */
static void set_menu_open(int open)
{
  menu.open = open;
  menu.status = NULL;
  if (!open) menu.wait_for_release = 1;
  if (audio.device && audio.started) SDL_PauseAudioDevice(audio.device, open);
}

static void move_cursor(int step)
{
  menu.selected = (menu.selected + MENU_ITEMS + step) % MENU_ITEMS;
  menu.status = NULL;
}

/* Enter or A on the selected item; returns 0 to quit */
static int confirm_item(SDL_Window *window)
{
  if (menu.selected < KEY_SETTINGS) change_setting(window, menu.selected, 1);
  else if (menu.selected == MENU_SAVE) menu.status = save_state() ? "Saved" : "Failed";
  else if (menu.selected == MENU_LOAD && load_state(STATE_FILE)) set_menu_open(0);
  else if (menu.selected == MENU_LOAD) menu.status = "No save";
  else return 0;
  return 1;
}

/* A rectangle of the panel in one colour */
static void menu_fill(int x, int y, int w, int h, const uint8 colour[4])
{
  int i, j;
  for (j = y; j < y + h; j++)
    for (i = x; i < x + w; i++) memcpy(menu_image[j][i], colour, 4);
}

/* Text in the game's font at panel pixel x, y; characters it has no glyph for are left blank */
static void menu_text(int x, int y, const char *text, const uint8 fill[4])
{
  for (; *text && x + 8 <= MENU_W; text++, x += 8)
  {
    int glyph = (uint8)*text - ' ', row, k;
    if (glyph <= 0 || glyph > 'z' - ' ') continue;
    for (row = 0; row < 16; row++)
    {
      uint32 word = MENU_FONT + glyph * 0x20 + row * 2;
      uint32 pixels = (uint32)ROM_WORD(word) << 16 | ROM_WORD(word + 1);
      for (k = 0; k < 8; k++)
      {
        int index = (pixels >> (28 - 4 * k)) & 15;
        if (index) memcpy(menu_image[y + row][x + k], index == 1 ? MENU_OUTLINE : fill, 4);
      }
    }
  }
}

/* The panel: a title, the settings, the state and quitting, and how to use it */
static void build_menu_image(void)
{
  static const char *const hint = "Esc/B: close   Enter/A: change";
  int item;
  menu_fill(0, 0, MENU_W, MENU_H, MENU_PANEL);
  menu_text((MENU_W - 8 * 8) / 2, 8, "SETTINGS", MENU_TITLE);

  for (item = 0; item < MENU_ITEMS; item++)
  {
    /* lines: the title, a gap, the settings, a gap, save, load and quit, a gap, the hint */
    int y = 8 + 16 * (item < KEY_SETTINGS ? 2 + item : 3 + item);
    const char *value = NULL;
    char label[MENU_COLUMNS + 1];

    if (item < KEY_SETTINGS)
    {
      snprintf(label, sizeof(label), "[%d] %s", item + 1, setting_labels[item]);
      value = setting_value(item);
    }
    else if (item == MENU_SAVE) strcpy(label, "[O] Save state");
    else if (item == MENU_LOAD) strcpy(label, "[P] Load state");
    else strcpy(label, "    Quit");
    if (item == menu.selected && menu.status) value = menu.status;

    if (item == menu.selected) menu_fill(4, y, MENU_W - 8, 16, MENU_CURSOR);
    menu_text(8, y, label, MENU_LABEL);
    if (value)
      menu_text(8 + 8 * (MENU_COLUMNS - (int)strlen(value)), y, value,
                item < KEY_SETTINGS && setting_locked(item) ? MENU_HINT : MENU_VALUE);
  }
  menu_text((MENU_W - 8 * (int)strlen(hint)) / 2, 8 + 16 * (MENU_LINES - 1), hint, MENU_HINT);
}

/* The open menu over a w x h window, in the middle, at the largest whole scale that fits */
static void draw_menu(int w, int h)
{
  int scale = w / MENU_W < h / MENU_H ? w / MENU_W : h / MENU_H;
  if (scale < 1) scale = 1;
  build_menu_image();
  use_texture_unit(UNIT_MENU);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, MENU_W);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, MENU_W, MENU_H, GL_RGBA, GL_UNSIGNED_BYTE, menu_image);
  use_texture_unit(UNIT_CORE_FRAME);

  glViewport((w - MENU_W * scale) / 2, (h - MENU_H * scale) / 2, MENU_W * scale, MENU_H * scale);
  glUseProgram(menu_program);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisable(GL_BLEND);
  glUseProgram(blit_program);
}

/* Alt+Enter: borderless fullscreen at the desktop's resolution and refresh rate (so the timing
   stays as it is), or back to the window. The mouse pointer is hidden while fullscreen. */
static void toggle_fullscreen(SDL_Window *window)
{
  int fullscreen = !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
  SDL_SetWindowFullscreen(window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
  SDL_ShowCursor(fullscreen ? SDL_DISABLE : SDL_ENABLE);
}

/* A key: Esc opens or closes the menu, which takes the arrows and Enter while open; 1-9 change
   the settings, O and P save and load the state, Alt+Enter toggles fullscreen. Keys are taken by
   their place on the keyboard (scancode), as the game's are (read_pad): the character a key
   types depends on the layout (an AZERTY number row types & é " ...), and under Wine on Linux
   it can come out wrong altogether. Returns 0 to quit. */
static int handle_key(SDL_Window *window, SDL_Scancode key)
{
  if ((key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_KP_ENTER) && (SDL_GetModState() & KMOD_ALT))
  {
    toggle_fullscreen(window);
    return 1;
  }
  if (key == SDL_SCANCODE_ESCAPE)
  {
    set_menu_open(!menu.open);
    return 1;
  }
  if (menu.open)
    switch (key)
    {
      case SDL_SCANCODE_UP: move_cursor(-1); return 1;
      case SDL_SCANCODE_DOWN: move_cursor(1); return 1;
      case SDL_SCANCODE_LEFT:
      case SDL_SCANCODE_RIGHT:
        if (menu.selected < KEY_SETTINGS) change_setting(window, menu.selected, key == SDL_SCANCODE_LEFT ? -1 : 1);
        return 1;
      case SDL_SCANCODE_RETURN:
      case SDL_SCANCODE_KP_ENTER:
      case SDL_SCANCODE_SPACE:
        return confirm_item(window);
      default: break;
    }
  if (key >= SDL_SCANCODE_1 && key <= SDL_SCANCODE_9) change_setting(window, key - SDL_SCANCODE_1, 1);
  else if (key == SDL_SCANCODE_O) save_state();
  else if (key == SDL_SCANCODE_P) load_state(STATE_FILE);
  return 1;
}

/* A controller button: Back or Guide opens or closes the menu; while it is open the D-pad
   moves and changes, A confirms and B closes. Returns 0 to quit. */
static int handle_controller_button(SDL_Window *window, int button)
{
  if (button == SDL_CONTROLLER_BUTTON_BACK || button == SDL_CONTROLLER_BUTTON_GUIDE)
  {
    set_menu_open(!menu.open);
    return 1;
  }
  if (!menu.open) return 1;
  switch (button)
  {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: move_cursor(-1); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: move_cursor(1); break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
      if (menu.selected < KEY_SETTINGS)
        change_setting(window, menu.selected, button == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? -1 : 1);
      break;
    case SDL_CONTROLLER_BUTTON_A: return confirm_item(window);
    case SDL_CONTROLLER_BUTTON_B: set_menu_open(0); break;
  }
  return 1;
}

/* Returns 0 when the program should end */
static int handle_events(SDL_Window *window)
{
  SDL_Event e;
  while (SDL_PollEvent(&e))
  {
    if (e.type == SDL_QUIT) return 0;
    if (e.type == SDL_KEYDOWN && !e.key.repeat && !handle_key(window, e.key.keysym.scancode)) return 0;
    if (e.type == SDL_CONTROLLERBUTTONDOWN && !handle_controller_button(window, e.cbutton.button)) return 0;
  }
  return 1;
}

/* ---- main ------------------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
  SDL_Window *window;
  int dump_frame = -1, track_from = -1, running = 1, frames_per_vsync, hitches = 0, opt;
  int view_distance_images = 0;
  int option_resolution = -1, option_no_slowdown = 0, option_widescreen = 0;
  int press_frame[MAX_PRESS], press_count = 0;
  uint16 press_pad[MAX_PRESS];
  double hz, fps, time_accumulator = 0, last_time, average_interval, sub_frame = 0;
  double last_interval;              /* real time between the last two refreshes */
  double interval_sum = 0, interval_sum_squares = 0, worst_interval = 0;
  long vsyncs = 0, frames_emulated = 0;
  long periods = 0, next_frame_period = 0;   /* refresh periods passed (a missed vsync counts all of
                                                its periods), and the one the next frame is due at */
  const char *rom, *state_path = NULL;

#ifdef _WIN32
  setvbuf(stdout, NULL, _IONBF, 0);          /* print at once, even if a crash follows */
  SetUnhandledExceptionFilter(report_crash);
#endif

  /* ---- command line: -r, -s and -W apply over the settings (vrr.ini when playing) ---- */
  while ((opt = getopt(argc, argv, "d:Fl:p:r:st:vW")) != -1)
  {
    char keys[16] = "";
    int value = optarg ? atoi(optarg) : 0;
    if (opt == 'v')
    {
      puts(TITLE);
      return 0;
    }
    else if (opt == 'd') dump_frame = value;
    else if (opt == 't') track_from = value;
    else if (opt == 's') option_no_slowdown = 1;
    else if (opt == 'W') option_widescreen = 1;
    else if (opt == 'F') view_distance_images = 1;
    else if (opt == 'l') state_path = optarg;
    else if (opt == 'r' && (value == 0 || value == 1 || value == 2 || value == 4)) option_resolution = value;
    else if (opt == 'p' && press_count < MAX_PRESS && sscanf(optarg, "%d:%15s", &press_frame[press_count], keys) >= 1)
      press_pad[press_count++] = core_parse_keys(keys);
    else optind = argc + 1;
  }
  if (optind != argc - 1)
  {
    fprintf(stderr, "usage: %s [-v] [-d N [-t from] [-F]] [-r 1|2|4|0] [-s] [-W] [-l state] [-p frame:keys]... rom\n",
            argv[0]);
    return 1;
  }
  rom = argv[optind];
  if (dump_frame < 0) load_settings();
  if (option_no_slowdown) no_slowdown = 1;
  if (option_widescreen) widescreen = 1;
  if (option_resolution >= 0)
  {
    resolution = option_resolution;
    use_svp_vertices = resolution == 1;
  }

  /* ---- window and OpenGL (a hidden window for -d) ---- */
  SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");   /* face buttons by position */
  /* Windows: sizes in scaled points and the drawable in real pixels, as on macOS, instead of
     the system enlarging (blurring) the window on a high-DPI display */
  SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
  SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
  if (SDL_Init(SDL_INIT_VIDEO | (dump_frame < 0 ? SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER : 0)) < 0)
  {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
  window = SDL_CreateWindow(TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            (int)((VIEW_W + 2 * (widescreen ? WIDE_SIDE_COLUMNS : 0)) * PIXEL_ASPECT * 3), 224 * 3,
                            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                            (dump_frame >= 0 ? SDL_WINDOW_HIDDEN : 0));
  if (!window || !SDL_GL_CreateContext(window))
  {
    fprintf(stderr, "window/GL context: %s\n", SDL_GetError());
    return 1;
  }
  if (!gl_load_functions()) return 1;
  SDL_StopTextInput();  /* otherwise keys open the macOS IME candidate window */
  gl_init();

  if (dump_frame >= 0)
  {
    printf("checking frames 0 to %d\n", dump_frame);
    if (core_init(rom, SAMPLE_RATE, 0) < 0) return 1;
    hle_init();
    layers_init();
    if (state_path && !load_state(state_path))
    {
      fprintf(stderr, "%s: not a saved state\n", state_path);
      return 1;
    }
    return dump(dump_frame, track_from, press_frame, press_pad, press_count, view_distance_images);
  }

  /* ---- pace: one emulated frame per vsync near 60 Hz, every second one near 120 Hz,
          otherwise by the clock ---- */
  SDL_GL_SetSwapInterval(1);
  hz = measure_refresh(window);
  if (fabs(hz - 60.0) < 1.5)
  {
    frames_per_vsync = 1;
    fps = hz;
  }
  else if (fabs(hz - 120.0) < 3.0)
  {
    frames_per_vsync = 2;
    fps = hz / 2;
  }
  else
  {
    frames_per_vsync = 0;      /* time-based */
    fps = NTSC_FPS;
  }
  printf("OpenGL: %s (%s)\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
  printf("display %.2f Hz: %s, emulating at %.3f fps\n", hz,
         frames_per_vsync == 1 ? "1 frame per vsync" : frames_per_vsync == 2 ? "1 frame every 2 vsyncs" : "time-based",
         fps);

  if (core_init(rom, SAMPLE_RATE, fps) < 0) return 1;
  hle_init();
  hle_set_far_view(far_view != FAR_VIEW_OFF);
  layers_init();
  if (state_path && !load_state(state_path))
  {
    fprintf(stderr, "%s: not a saved state\n", state_path);
    return 1;
  }
  open_audio();

  average_interval = 1.0 / hz;
  last_interval = 1.0 / hz;
  last_time = now_seconds();
  while (running)
  {
    int frames_to_run, i, w, h;
    double current_time, interval;

    running = handle_events(window);
    update_title(window);

    /* how many frames to emulate before this refresh: none while the menu is open */
    if (menu.open)
    {
      frames_to_run = 0;
    }
    else if (frames_per_vsync)
    {
      /* a frame every frames_per_vsync refresh periods; after a missed vsync the frames due
         meanwhile are caught up (up to 4), so the game keeps time and the sound stays fed */
      frames_to_run = 0;
      while (periods >= next_frame_period && frames_to_run < 4)
      {
        next_frame_period += frames_per_vsync;
        frames_to_run++;
      }
      if (periods >= next_frame_period) next_frame_period = periods + frames_per_vsync;   /* too far behind: skip */
    }
    else
    {
      /* the real time since the last refresh, not one refresh period: a window covered by
         others gets its refreshes late and irregularly (macOS), and the game must keep time */
      frames_to_run = 0;
      time_accumulator += last_interval;
      while (time_accumulator >= 1.0 / fps && frames_to_run < 4)
      {
        time_accumulator -= 1.0 / fps;
        frames_to_run++;
      }
      if (time_accumulator >= 1.0 / fps) time_accumulator = 0;   /* too far behind: skip */
    }

    for (i = 0; i < frames_to_run; i++)
    {
      int sample_count;
      core_pad = read_pad();
      set_svp_clock();
      system_frame_gen(0);
      hle_frame_done();
      sample_count = audio_update(samples);
      frames_emulated++;
      queue_audio(sample_count, fps);
    }
    if (frames_to_run) upload_frame();

    /* How far this refresh is past the last emulated frame, in frames, for smooth motion:
       at 120 Hz the second refresh of a frame is half a frame later; without vsync the time
       left over in the accumulator says it. While the menu is open it stays where it was. */
    if (!menu.open)
      sub_frame = frames_per_vsync ? (double)(periods - (next_frame_period - frames_per_vsync)) / frames_per_vsync
                                   : time_accumulator * fps;

    SDL_GL_GetDrawableSize(window, &w, &h);
    draw(w, h, sub_frame);
    if (menu.open) draw_menu(w, h);
    SDL_GL_SwapWindow(window);

    /* timing: a running average of the vsync interval (the emulated frame rate follows it),
       missed vsyncs, and statistics for the end, all after the first 60 vsyncs of startup */
    current_time = now_seconds();
    interval = current_time - last_time;
    last_time = current_time;
    last_interval = interval;
    if (!menu.open)
      periods += interval >= 1.5 * average_interval ? lround(interval / average_interval) : 1;
    if (vsyncs > 60)
    {
      if (interval >= 1.5 * average_interval)
      {
        hitches++;                                            /* a missed vsync */
      }
      else
      {
        average_interval += (interval - average_interval) * 0.01;   /* ~100 vsync time constant */
        if (frames_per_vsync) fps = 1.0 / (average_interval * frames_per_vsync);
      }
      interval_sum += interval;
      interval_sum_squares += interval * interval;
      if (interval > worst_interval) worst_interval = interval;
    }
    vsyncs++;
  }

  if (vsyncs > 61)
  {
    long n = vsyncs - 61;
    double mean = interval_sum / n;
    printf("%ld vsyncs, %ld frames emulated; vsync interval %.3f ms (sd %.3f, worst %.3f), %d missed vsyncs\n",
           vsyncs, frames_emulated, mean * 1000, sqrt(fmax(0, interval_sum_squares / n - mean * mean)) * 1000,
           worst_interval * 1000, hitches);
    printf("audio: %d underruns, lowest queue %.1f ms (target %.1f ms); final frame rate %.3f fps\n",
           audio.underruns, audio.lowest_queue * 1000.0 / SAMPLE_RATE, AUDIO_TARGET * 1000.0 / SAMPLE_RATE, fps);
  }
  SDL_Quit();
  return 0;
}

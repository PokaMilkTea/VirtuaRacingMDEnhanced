/* See layers.h. Mirrors GPGX's mode 5 renderer (core/vdp_render.c: render_bg_m5, window_clip,
   render_obj_m5) one pixel at a time; vrr -d checks the composite against GPGX's own output
   on every frame.
   ponytail: no shadow/highlight, interlace mode 2 or the window/plane A column bug; Virtua
   Racing uses none of them (docs/screen-layers.md). */
#include "layers.h"

layers_t layers;

/* VRAM access. GPGX keeps VRAM as 16-bit words in host order (LSB_FIRST), so a byte address
   has its lowest bit flipped. */
#define VRAM_WORD(address) (*(uint16 *)&vram[(address) & 0xFFFE])
#define VRAM_BYTE(address) vram[((address) & 0xFFFF) ^ 1]

/* A name table entry (plane cells) or sprite attribute word */
#define ENTRY_PRIORITY  0x8000
#define ENTRY_PALETTE   0x6000     /* palette line, bits 13-14 */
#define ENTRY_VFLIP     0x1000
#define ENTRY_HFLIP     0x0800
#define ENTRY_TILE      0x07FF     /* tile number; a tile is 32 bytes, 8 x 8 pixels of 4 bits */

/* The horizontal scroll table holds two words per line: plane A's, then plane B's */
#define HSCROLL_PLANE_A 0
#define HSCROLL_PLANE_B 1

/* The layer byte for pixel (x, y), both 0-7, of the cell that a name table entry names */
static uint8 cell_pixel(uint16 entry, int x, int y)
{
  int tile_byte, colour, priority, palette;
  if (entry & ENTRY_HFLIP) x = 7 - x;
  if (entry & ENTRY_VFLIP) y = 7 - y;

  /* 4 bytes per row of the tile, 2 pixels per byte, the left one in the high nibble */
  tile_byte = VRAM_BYTE((entry & ENTRY_TILE) * 32 + y * 4 + x / 2);
  colour = (x & 1) ? (tile_byte & 15) : (tile_byte >> 4);

  priority = (entry & ENTRY_PRIORITY) ? LAYER_PRIORITY : 0;
  palette = ((entry & ENTRY_PALETTE) >> 13) << 4;
  return priority | palette | colour;
}

/* One scrolling plane (plane A or B) into layer byte `layer` of the pixels first_x..end_x-1 of
   a line. which_scroll picks the plane's scroll values: HSCROLL_PLANE_A or HSCROLL_PLANE_B. */
static void draw_plane(uint8 (*line_pixels)[LAYER_COUNT], int layer, int line, int which_scroll,
                       uint16 name_table, int first_x, int end_x)
{
  int plane_width = (playfield_col_mask + 1) * 16;   /* in pixels */
  int hscroll = *(uint16 *)&vram[hscb + ((line & hscroll_mask) << 2) + which_scroll * 2];
  int x;

  for (x = first_x; x < end_x; x++)
  {
    /* vertical scroll: one value for the whole plane, or one per 16-pixel column (reg 11) */
    int vsram_offset = (reg[11] & 4) ? (x >> 4) * 4 : 0;
    int vscroll = *(uint16 *)&vsram[vsram_offset + which_scroll * 2];

    /* the pixel of the plane that lands on screen x of this line */
    int plane_y = (line + vscroll) & playfield_row_mask;
    int plane_x = (x - hscroll) & (plane_width - 1);

    /* the name table entry of that cell: rows are playfield_shift bits apart, 2 bytes a cell */
    int row_offset = ((plane_y >> 3) << playfield_shift) & 0x1FC0;
    uint16 entry = VRAM_WORD(name_table + row_offset + (plane_x >> 3) * 2);

    line_pixels[x][layer] = cell_pixel(entry, plane_x & 7, plane_y & 7);
  }
}

/* Plane A of a line, with the window drawn instead where it covers the line. The window is
   not scrolled. It covers whole lines above or below a row (reg 18), and otherwise the
   16-pixel columns left or right of a position (reg 17). */
static void draw_plane_a_and_window(uint8 (*line_pixels)[LAYER_COUNT], int line, int width)
{
  int window_down = (reg[18] >> 7) & 1;                /* the window is below the row, not above */
  int window_row_line = (reg[18] & 0x1F) * 8;
  int window_whole_line = window_down == (line >= window_row_line);
  int window_right = reg[17] >> 7;                     /* the window is right of the position */
  int window_column = reg[17] & 0x1F;                  /* in 16-pixel columns */
  int row_stride_shift = 6 + (reg[12] & 1);            /* 64 bytes per window row in H32, 128 in H40 */
  int column, x;

  for (column = 0; column < width / 16; column++)
  {
    int in_window;
    if (window_whole_line) in_window = 1;
    else if (window_right) in_window = column >= window_column;
    else in_window = column < window_column;

    if (!in_window)
    {
      draw_plane(line_pixels, LAYER_PLANE_A, line, HSCROLL_PLANE_A, ntab, column * 16, column * 16 + 16);
      continue;
    }
    for (x = column * 16; x < column * 16 + 16; x++)
    {
      uint16 entry = VRAM_WORD(ntwb | ((line >> 3) << row_stride_shift) | (x >> 3) * 2);
      line_pixels[x][LAYER_PLANE_A] = cell_pixel(entry, x & 7, line & 7);
    }
  }
}

/* A sprite whose middle is more than this from the screen's middle is at a side
   (LAYER_SIDE_SPRITE). The race HUD's left part (position, rev counter, speed) and right part
   (lap time, map, difficulty) are all over 48 away, its middle part (TIME) within 16. */
#define SIDE_SPRITE_DISTANCE 28

/* The sprites on a line, from GPGX's list for it (4 words each: the line within the sprite,
   x + 128, attributes, size). Earlier sprites are in front. Like GPGX: a sprite at x = 0
   masks the sprites after it (once a sprite with x > 0 came first, or the previous line
   overflowed: overflow), and at most max_sprite_pixels pixels are drawn per line. */
static void draw_sprites(uint8 (*line_pixels)[LAYER_COUNT], int width, const uint16 *sprite_list,
                         int count, int overflow)
{
  int pixels_used = 0, masked = 0;
  const uint16 *sprite;

  for (sprite = sprite_list; sprite < sprite_list + count * 4; sprite += 4)
  {
    int sprite_line = sprite[0], raw_x = sprite[1], attributes = sprite[2], size = sprite[3];
    int sprite_width = 8 + ((size & 0x0C) << 1);       /* 1-4 cells */
    int sprite_height = 8 + ((size & 3) << 3);         /* 1-4 cells */
    int cells_down = (size & 3) + 1;                   /* tiles are numbered down each column */
    int x = raw_x - 0x80;
    int pixels_to_draw = sprite_width, i;
    int middle_offset = x + sprite_width / 2 - width / 2;
    uint8 side = abs(middle_offset) > SIDE_SPRITE_DISTANCE ? LAYER_SIDE_SPRITE : 0;

    if (raw_x) overflow = 1;                           /* masking needs a sprite with x > 0 first */
    else if (overflow) masked = 1;
    pixels_used += sprite_width;

    if (x + sprite_width > 0 && x < width && !masked)
    {
      if (attributes & ENTRY_VFLIP) sprite_line = sprite_height - 1 - sprite_line;
      if (pixels_used > max_sprite_pixels) pixels_to_draw -= pixels_used - max_sprite_pixels;
      pixels_to_draw &= ~7;                            /* whole cells only */

      for (i = 0; i < pixels_to_draw; i++)
      {
        int screen_x = x + i;
        int sprite_x = (attributes & ENTRY_HFLIP) ? sprite_width - 1 - i : i;
        int tile, cell_entry;
        uint8 pixel;

        if (screen_x < 0 || screen_x >= width) continue;
        if (line_pixels[screen_x][LAYER_SPRITES] & LAYER_COLOUR) continue;   /* the first sprite wins */

        /* the cell's tile: cells_down tiles per column of cells, then the row of the cell.
           The flips are already applied to sprite_x and sprite_line, so they are left out. */
        tile = (attributes + (sprite_x >> 3) * cells_down + (sprite_line >> 3)) & ENTRY_TILE;
        cell_entry = (attributes & (ENTRY_PRIORITY | ENTRY_PALETTE)) | tile;
        pixel = cell_pixel(cell_entry, sprite_x & 7, sprite_line & 7);
        if (pixel & LAYER_COLOUR) line_pixels[screen_x][LAYER_SPRITES] = pixel | side;
      }
    }
    if (pixels_used >= max_sprite_pixels) return;
  }
}

/* Called by GPGX (vrr_line_hook) before it renders a line: draw our copy of its layers with
   the same VDP state */
static void line_hook(int line, const uint16 *sprite_list, int sprite_count, int overflow)
{
  uint8 (*line_pixels)[LAYER_COUNT];
  int width = bitmap.viewport.w, x;

  if (line < 0 || line >= LAYERS_H || width > LAYERS_W) return;
  line_pixels = layers.px[line];
  memcpy(layers.cram[line], cram, sizeof(layers.cram[line]));
  layers.hscroll_b[line] = *(uint16 *)&vram[hscb + ((line & hscroll_mask) << 2) + HSCROLL_PLANE_B * 2];

  memset(line_pixels, 0, sizeof(layers.px[line]));
  for (x = 0; x < width; x++) line_pixels[x][LAYER_BACKDROP] = reg[7] & 0x3F;
  if (!(reg[1] & 0x40)) return;            /* display off: GPGX blanks the line to the backdrop */

  draw_plane(line_pixels, LAYER_PLANE_B, line, HSCROLL_PLANE_B, ntbb, 0, width);
  draw_plane_a_and_window(line_pixels, line, width);
  draw_sprites(line_pixels, width, sprite_list, sprite_count, overflow);

  if (reg[0] & 0x20)                       /* the leftmost 8 pixels blanked (reg 0) */
    for (x = 0; x < 8; x++)
    {
      line_pixels[x][LAYER_PLANE_B] = 0;
      line_pixels[x][LAYER_PLANE_A] = 0;
      line_pixels[x][LAYER_SPRITES] = 0;
    }
}

/* Called by GPGX (vrr_blank_hook) when it blanks pixels offset..offset+width-1 of a line to the
   backdrop (the display turned off in the middle of the line); it then recolours the whole
   line with the current palette, so the line's CRAM is taken again */
static void blank_hook(int line, int offset, int width)
{
  int x, end = offset + width;
  if (line < 0 || line >= bitmap.viewport.h || bitmap.viewport.w > LAYERS_W) return;
  memcpy(layers.cram[line], cram, sizeof(layers.cram[line]));
  for (x = offset < 0 ? 0 : offset; x < end && x < bitmap.viewport.w; x++)
  {
    uint8 *pixel = layers.px[line][x];
    pixel[LAYER_PLANE_B] = 0;
    pixel[LAYER_PLANE_A] = 0;
    pixel[LAYER_SPRITES] = 0;
    pixel[LAYER_BACKDROP] = reg[7] & 0x3F;
  }
}

void layers_init(void)
{
  vrr_line_hook = line_hook;
  vrr_blank_hook = blank_hook;
}

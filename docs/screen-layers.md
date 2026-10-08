# Screen layers: what the SVP draws and what the VDP draws


The HLE compositing, and the handling of the 2D layers in widescreen, are based on this document.
Labels are the same as in `svp-commands.md`: [Verified], [Source], [Speculation].

## Method [Verified]

- `svptrace -v frame`: saves the VDP registers, VRAM, CRAM and VSRAM at the end of that frame to `vdp_NNNNNN.bin`.
- `tools/vdplayers.py`: following the mode 5 specification, draws Plane B, Plane A (with the Window) and the sprites separately, composites them again and compares the result **line by line** with the `svptrace -w` screenshot.
- All 6 frames checked match (224/224 lines). The only exception is that line 223 sometimes differs: on that line the display is actually off for DMA (see "Top and bottom borders"), which the dumped state cannot show. So **no other mid-line effect** (palette or scroll changes) affects the visible picture.
- Later the comparison moved to `src/layers.c`, line by line (see "Compositing in the HLE version"), covering every frame from power-on to frame 12000. It found only one more mid-frame change: **some frames turn the display off part way through line 207** (for example frame 7114, from x = 207), and the rest of that line becomes the backdrop colour. This is in the border below the 3D area and cannot be seen.

## VDP setup shared by all screens [Verified: 6 frames]

| Item | Setting |
|---|---|
| Mode | H32, 256×224, no shadow/highlight |
| Plane size | 32×64 cells (256×512 pixels) |
| Plane A name table | `C000` |
| Plane B name table | `E000` |
| Window | `F000`, **unused** (H=0, V=0) |
| Sprite table | `F200` |
| Horizontal scroll table | `EC00`, full-screen scroll |
| Vertical scroll | Full-screen scroll |
| Backdrop colour | 0 (black) |

## Race screen (command 02) [Verified: frames 3510, 4410]

From bottom to top:

| Layer | Content | Range | Palette | Priority |
|---|---|---|---|---|
| Backdrop | Black | Whole screen, visible only in the top and bottom borders | — | — |
| **Plane B** | Sky, clouds, distant grass: one 2D background picture | Lines 15-206 | Lines 2 and 3 | Low |
| **Plane A** | **The SVP's 3D output**; transparent where there are no polygons, showing Plane B | Lines 15-206 (192 lines) | Line 0 only | Low |
| **Sprites** | **The whole HUD**: position (such as 11th/16), rev counter, TIME, LAPTIME, the map and the cars' dots, SPEED and the speed, difficulty (BEGINNER) | Over the 3D | Lines 0, 1 and 3 | Low (sprites cover planes of the same priority anyway) |

- **The C.P. checkpoint arch is 3D**, drawn in Plane A, not a sprite.
- **The large messages during a race are not VDP sprites** but SVP sprites drawn into the 3D buffer, so they belong to Plane A. Examples: PIT START, the big start countdown digits, EXTEND TIME, BEST LAP and the lap time, PIT WORK (`svp-commands.md` section 3).
- The number of opaque Plane A pixels in this frame (34,077) is exactly the number of non-zero pixels the SVP wrote into the tile buffer.
- **3D double buffering**: Plane A's vertical scroll alternates between 1 and 209. At 1 it shows rows 2-25 of the name table (buffer 1), at 209 rows 28-51 (buffer 2). This matches notaz's `imageformat.txt`.
- **Top and bottom borders**: lines 0-14 and 207-223. The game uses H-INT to DMA tiles during these lines, with the display turned off ([Source: Devon], post of 2019-10-21; the dumps also show display off).
- **Sky scrolling**: Plane B's horizontal scroll = `floor(−camera yaw / 16)` (measured: −19 gives 297, −69 gives 1092). One full turn (`0x1000`) scrolls exactly 256 pixels, the width of Plane B, so the sky wraps seamlessly. The vertical scroll is fixed at 1: **the horizon does not follow the camera pitch**.
  - [Verified: frame by frame, Beginner with turns both ways, all 1798 frames match] The yaw is word 5 of slot 0. The scroll in frame f comes from the last command 02 issued before frame f − 3. The 3D only appears in the 9th frame after it is issued, so **in the original the sky leads the 3D by about 6 frames**.
  - With interpolation (`hle.md`, "Interpolation"), the composite shader shifts Plane B from the scroll the game set to the position of the interpolated yaw (not rounded). Plane B is exactly as wide as the screen (256), so the whole row is on screen, and the pixels that wrap around after the shift are what the VDP would draw with that scroll value.
- [Speculation] The sky moves 1.4° per pixel, while the 3D at the screen centre moves about 0.9° per pixel (focal length 64). The two angular speeds differ; that is how the original looks.

## Title screen (commands 06, 08) [Verified: frame 1100]

Same structure as the race: Plane B is the sky, Plane A the 3D demo, and "PRESS START BUTTON", the AM2 logo and the copyright text are sprites.

## Mode select (command 09) [Verified: frames 1400, 1520, 1700]

- **While idle** (frame 1400): the five option panels are **sprites**, Plane B is the brick wall background, Plane A is empty.
- **The zoom animation after choosing** (frames 1520, 1700): the zooming panel is 3D drawn by the SVP (Plane A), and **its name table entries have high priority**, so it covers the other sprite panels.
- So **Plane A's priority changes from screen to screen** (low in a race, high here). The HLE compositing must follow the priority bit of each tile in the name table, not assume a fixed layer order.

## Course select [Verified: frame 2100]

**No SVP at all**. Plane A is 2D graphics the 68k puts in VRAM directly (course map, text), Plane B a background pattern, and two sprites are small things such as the cursor.

## Screens not checked yet

The 2P split screen (command 4; [Source: Devon] uses line scroll), replay (commands A, B), finish and game over (command 3), the SEGA logo (command 5), Records, Options.

## Compositing in the HLE version [Verified: `vrr -d`, every frame bit-identical from power-on to frame 12000]

When GPGX draws a line it merges Plane B, Plane A and the sprites into single pixels, without keeping the source layer, so Plane A cannot be replaced afterwards. Instead we draw our own copy of the layers:

- **Timing**: a hook at the start of GPGX's `render_line()` (`vrr_line_hook`) draws the three layers of the line from the same VDP state GPGX uses for it. This handles mid-frame changes automatically, such as scroll, palette or sprite table changes. Reading the VDP state at the end of the frame would not work: during vblank the 68k may already have changed it for the next frame.
- **Sprite list**: uses the list GPGX has already parsed, including the limits of 16 sprites and 256 pixels per line and the masking rule at x = 0, so the result is exactly GPGX's.
- **Display turned off mid-line**: GPGX's `blank_line()` paints part of a line with the backdrop colour and then recolours the whole line with the current palette, so a hook there (`vrr_blank_hook`) follows along.
- **4 bytes per pixel**: one byte each for Plane B, Plane A and the sprites (bit 7 priority, bits 4-5 palette, bits 0-3 colour, 0 transparent), plus the backdrop colour. Each line also keeps a copy of the CRAM at that time.
- **Compositing** (GPU):
  - Priority from high to low: high-priority sprites, Plane A, Plane B, then low-priority sprites, Plane A, Plane B, and finally the backdrop.
  - Inside the 3D area, Plane A pixels are replaced with the HLE output, keeping the priority of the original cell, so high-priority 3D such as the mode select zoom works too.
  - Colours are converted to RGB565 through that line's CRAM, the same way GPGX does it, so screens without HLE are bit-identical to the original.
- **2D layers at high resolution**: scaled up as plain pixels. A pixel-art upscaler (our own implementation of the xBR level 1 edge rules for Plane A, the sprites and the SVP text, with Sharp Bilinear for Plane B) was tried and removed (2026-10-07): it rounded letters nicely but broke steep and shallow lines, such as `/` and the rev counter needle, into bead-like segments, and supersampling its result only softened the edges.

## Sky dither in full colour mode [Verified: visual comparison of frames 1100, 1400, 3510]

- **Cloud dither** [Verified: frame 3510 listed pixel by pixel]: the cloud gradients interleave **two adjacent CRAM levels** (for example `0,32,232` and `32,68,232`) in an irregular arrangement, not a fixed checkerboard or lines. Every interleave in the sky is only 1 level apart.
- **Method** (`background_shader` in `src/vrr.c`): a linear colour image of Plane B plus the backdrop is drawn at native resolution and replaces Plane B's colours in compositing.
  - A pixel counts as dither when its left and right neighbours have the same colour and differ from it by 1 level (A B A), or the same holds vertically.
  - A dither pixel and the pixels within 1 level of it in its 3×3 neighbourhood share colour with tent weights (1 2 1). Checkerboard and line dither blend to exactly half and half, and irregular dither keeps its original total amount of each colour.
- **Why "1 level apart" alone is not enough**: blending every neighbour 1 level apart at first blurred the brick wall on the mode select screen. The brick face, its 1-pixel highlight and the mortar happen to be 1 level apart from each other, but the highlight has different colours on its two sides (brick on one, mortar on the other), not A B A. With that condition the brick wall matches the original exactly, and the clouds still blend into gradients.
- Only Plane B is processed. HUD sprites and Plane A's 2D stay as they are. The 3D in the mode select animation is still drawn by the SVP (HLE does not cover command 09), so those polygons keep their dither.

## What this means for the rest of the work

- **Compositing**: everything except the 3D part of Plane A is rendered by the VDP as before (Plane B, sprites, and Plane A on screens without 3D). The 3D part is replaced with the HLE output, placed where Plane A is, and **the priority bit in the name table** decides whether it is in front of or behind the sprites.
- **2D layers at high resolution**: Plane B's sky and the HUD sprites are 256-wide pixel art. There is no higher-resolution source; they are scaled up as plain pixels.
- **Colour**: all 3D colours come from palette line 0 in CRAM, read again every frame (the 68k may update the palette during DMA).
- **Widescreen** (implementation in `hle.md`, "Widescreen"):
  - The 3D area widens with the screen (348 columns at 16:9) and extends up and down to fill the original borders (224 lines in all). Plane B is empty on those lines, so it is filled by mirroring lines 15 to 206 of Plane B up and down.
  - Plane B is only 256 wide but wraps seamlessly. During a race the Plane B on the sides takes its pixels by wrapping the same row (the clouds repeat). The whole of Plane B scrolls as one, so this is what the VDP would draw on a wider screen.
  - Plane A's 2D stays in the middle 256 columns.
  - **HUD moved to the sides** (race, finish, replay): the HUD sprites are laid out for 256 columns; in widescreen the ones on the sides move out by 46 columns to the new screen edges, and the middle ones stay.
    - [Verified: sprite tables of 16 frames in a Beginner race, `svptrace -v`] The HUD falls into three groups far apart: left x 8-88 (position, rev counter, SPEED and the speed, with one more line under the position at the start), middle x 112-144 (TIME), right x 176 and up (LAPTIME and the lap times, map, difficulty).
    - The map sprites sometimes go past the right edge (to x 265), and the original cuts them at x 256. After moving 46 columns right, the cut is exactly at the new screen edge.
    - Method: when `layers.c` draws the sprites, a sprite whose centre is more than 28 from the screen's centre line is marked "on a side" in bit 6 of the layer byte (previously unused). For each pixel the composite shader looks at three sources: a sprite 46 columns to the right moved left, one 46 columns to the left moved right, and middle sprites in place.
    - Whole sprites move together, so the VDP's per-line sprite limit and front-to-back order are the same as the original; only the positions differ.
    - The large text drawn by the SVP (PIT START, BEST LAP and so on) belongs to the 3D buffer and stays in the middle.
  - Screens that are not races (title, menus) get black bars on the sides, so the picture does not change size between the menus and the race.

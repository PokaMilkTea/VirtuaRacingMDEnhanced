# HLE renderer


Redraws the race 3D (command 2) on the GPU instead of using the SVP's output, then composites it with the VDP's 2D layers into the full picture. The code has three parts:
- `src/hle.c`: builds the draw list and the sprites.
- `src/layers.c`: draws our own copy of the VDP layers.
- `src/vrr.c`: GPU drawing, compositing and comparison.

The data formats and arithmetic are based on `svp-commands.md`, the screen layout on `screen-layers.md`. Same labels: [Verified], [Source], [Speculation].

## Pipeline

1. **Catching the command**: when the 68k writes `30FE08` (the command number), the model slots, sprite slots and command block are read from SVP DRAM. `30FE04` tells which buffer this one is drawn into (`svp-commands.md` section 1.3).
2. **Matrices and camera**: the camera matrix, the camera's world position and each object slot's matrix are computed with integer arithmetic bit-identical to the SVP (sections 6.9, 6.12, 6.13). Every step is also done again in floating point, without any truncation (see "Vertex precision at high resolution").
3. **Vertices**: transformed to camera space, with integer results bit-identical to the SVP (sections 6.8, 6.12), plus a floating-point result. The models transformed are:
   - The 8 track sections the 68k picks (section 4.1).
   - The model of each visible object slot.
   - The model list in slot 56 (section 2), such as the pit crew at the start.
4. **Culling and sorting** (always decided on the integer results, so which polygons are drawn and in what order matches the original):
   - Polygons with all vertices behind the near plane are dropped. As in the SVP, the near plane depends on what is drawn (`svp-commands.md` section 6.8): 2 for the player car, 6 for opponents and slot 56, 16 for the second pass of the in-car view (in 1/32 units). The track is the exception: the SVP uses 16, HLE uses 8 (see "Near plane and the in-car view").
   - Polygons with the backface culling flag use the SVP's cross product formula (`01a5` in `19B5`). For polygons crossing the near plane, the SVP clips first and tests afterwards; HLE uses the equivalent 3D test instead: the triple product `v0 · (v1 × v2) > 0` means front-facing. It needs no projection, and on 130,000 polygons in front of the camera it agrees exactly with the 2D formula.
   - [Verified: polygon list of frame 114 of the SEGA logo] The SVP computes the cross product on **integer screen coordinates**, so polygons with almost no area on screen (small distant things) can be dropped by rounding, or get the wrong sign. HLE does both: once with the floating-point projection, once with the SVP's integer projection, and drops a polygon only when both say it faces away. When drawing with original vertices (key 2, 1×) the integer result is used, matching the original; with floating-point vertices the floating-point result is used, so small faces at high resolution do not flicker with rounding.
     - [Verified: a Medium save state, polygons listed tick by tick; all three courses counted with `vrr -d 9000`] The SVP tests only the **first three vertices**. Four track polygons in the whole game (three on Beginner, one hillside on Medium) are triangles stored as quads, with two of those three vertices about one unit apart, so the test is on a sliver with almost no area: its sign changes with every rounding, and on Medium a large slope on the left hill flickered whenever the car moved. With floating-point vertices, a quad whose triangle 0-1-2 has under 1/16 of the area of triangle 0-2-3 in the model is tested on 0-2-3 instead. The decision is made on the model, not the screen, so it does not change as the camera moves. The original flickers the same way [Verified: SVP output and HLE 1× side by side from the same save state: the slope is missing for several ticks, then drawn, identically in both]. The integer test stays the SVP's, so 1× comparisons are unchanged (`-d 4410` bit-identical). Testing every quad by its whole area was tried and rejected: many quads are not flat, and the area changed thousands of decisions per race (leaves, the sides of the player car's nose), away from the original look.
   - The depth key is computed according to the Z-sort flags (section 6.10); keys above 2047 are dropped. For polygons hit by near clipping, the z of vertices behind the near plane is taken as the SVP's near plane (16 for the track), because the SVP sets the z of the intersection to the near plane.
   - Finally sorted back to front. Within a bucket the order of creation is kept: track first, then objects by slot number.
5. **Sprites**: following the rules of the SVP's stage 6 (section 3), text, countdown digits and times are drawn into a 256×192 index image. In compositing the sprites go over the 3D.
6. **Latching** (section 1.3): each buffer keeps two draw lists, one for the latest command and one for what the SVP finished most recently. When Plane A switches to a buffer, the "most recently finished" one is shown.
   - "Finished" means: the 68k issues the next command (the game always waits for the previous one to finish), or the done flag `30FE02` is not 0 at the end of a frame.
   - At the end of each frame the switch is handled before checking the done flag, because a picture finished in the same frame cannot have been DMAed into VRAM yet.
   - **Is Plane A still showing an SVP buffer?** [Verified: name tables from VDP dumps, plus test input with a changed lap count run past the finish line]: at the end of each frame, the 24×32 cells of that buffer in Plane A's name table are checked for still pointing at the SVP's tiles (buffer 1 from tile 1, buffer 2 from tile `1CC`, two tiles per column per 16-line strip; from index `1CA` on, both buffers share tiles from `397`). When the game switches to its own 2D screens (results after the finish, menus) it rewrites these cells, and then HLE draws nothing and the original is shown. Previously only the last latched command was checked; after the finish the SVP gets no more commands, and HLE kept laying the last 3D picture over the menu.
7. **GPU drawing of the 3D**:
   - Projection: screen x = 129 + 64x/z, y = 96 − 80y/z (256×192 buffer, y down). Near clipping is left to the GPU.
   - **Original vertices** (key 2; used at 1× and in `-d` checks, while playing the default is floating-point vertices): polygons with all vertices in front of the near plane use the SVP's integer projection (reciprocal table plus truncation), reproducing the original vertex positions.
     - Exception: for vertices far off screen, the SVP's 16-bit projection overflows and wraps (the SVP itself culls such polygons before projecting). When the integer and float projections differ by more than 64 pixels, the float vertices are used and the GPU clips.
   - **Original colours** (key 5 with full colour off): dither is produced in the fragment shader by the rules of section 6.5. The output is palette indices (colour 0 transparent); colouring happens in compositing.
   - **Full colour** (default, key 5): no dither; colours are output directly according to the two uses in section 6.5:
     - Colours are looked up in that line's CRAM and converted to linear light (RGB565 takes the same path as original colours, so solid polygons match the original colours).
     - Each polygon outputs premultiplied (colour 1 + colour 2) / 2, with coverage (colour 1 not 0 + colour 2 not 0) / 2. The same formula covers three cases:
       - Solid: the original colour, coverage 1.
       - Two-colour dither: the average colour, coverage 1.
       - Dither with colour 0: 50% coverage, alpha blended over the polygons already drawn (`ONE, ONE_MINUS_SRC_ALPHA`), such as car shadows and tyre smoke.
     - Blending is done in linear light, because the eye averages the **light** of the two colours in on-screen dither. Averaging sRGB values directly gives colours that are too dark.
     - The texture is 16-bit float and holds linear colours.
   - No depth buffer; drawn in list order (painter's algorithm).
   - **Internal resolution** (key 3 or `-r`; "window" by default when playing): native 1× (256×192), 2×, 4×, or "window", drawing as many pixels as the 3D area actually covers on screen.
     - Compositing samples with nearest, so a fixed-scale texture of a different size than the screen just drops or repeats pixels; it does not become anti-aliasing. "Window" is one texture pixel per screen pixel.
     - **MSAA** (key 4, on by default when playing, at any resolution): the 3D is drawn into an RGBA16F multisample renderbuffer with 4 samples per pixel, then resolved into the HLE texture with `glBlitFramebuffer`. Only in full colour: full colour outputs linear-light colours and coverage, which average correctly; 9-bit mode outputs palette indices, which cannot be averaged. `-d` never uses it (it does not read `vrr.ini`).
     - [Verified: forced on temporarily and wrote the widescreen picture of `-W -d 3510`] The slanted edges of cars and lane lines have intermediate colours, no stairs.
     - At higher resolutions the dither stays in units of the SVP's native pixels, keeping the original look.
     - Original vertices are on by default only at 1×, because at higher resolutions integer vertices make polygon edges jump only every several pixels.
     - Sprites and 2D layers stay at native resolution and are scaled up.
8. **Compositing** (`screen-layers.md`, "Compositing in the HLE version"):
   - The whole picture is rebuilt from our own VDP layers.
   - Plane A pixels in the 3D area (lines 15-206) are replaced with HLE sprites or 3D, keeping the priority of the original cell.
   - Colours come from each line's CRAM at that time.
   - In full colour, the whole composite is done in linear light and converted back to sRGB at the end. Where the 3D's coverage is below 1, the layer below Plane A in priority shows through (Plane B, the backdrop, or low-priority sprites as on the mode select screen).

## Verification [Verified: `vrr -d N -p …`]

`vrr -d` makes these checks (comparisons always at 1×, with original vertices and original colours; the last frame also writes a full colour picture `full_N.ppm`, and with `-r 2` or `-r 4` one more at that resolution, for visual checks):
- **Every frame**: the layer composite without HLE must be bit-identical to the emulator core's output.
- **Last frame**: HLE 3D plus sprites are compared pixel by pixel with the Plane A the core actually drew (the SVP's output).
- **With `-t F`**: from frame F on, the HLE comparison above runs on every frame, listing the average, the minimum and the frames below 85%. Frames where neither side has any pixel (the game has faded the screen to black) have nothing to compare; they are left out of the statistics and counted separately. When HLE picks the wrong draw list (the picture going backwards) or draws polygons it should not, the match drops to 20% to 80%, plain to see.

**Frame by frame** (`-t 2470`, throttle held from before the start, run to frame 9000):

| Course | Frames compared | Average | Minimum | Below 85% |
|---|---|---|---|---|
| Beginner | 6521 | 94.6% | 89.1% | 0 |
| Medium | 6085 | 94.9% | 86.5% | 0 |
| Expert | 6520 | 94.1% | 87.5% | 0 |

For the in-car view (adding `-p 2800:BX -p 2806:B`, `-t 2900`) see "Near plane and the in-car view".

This check found and fixed three problems:
1. **The picture going backwards**: at the Medium and Expert starts the SVP is overloaded, and the old rule "show the previous command" picked a picture one round too old (section 1.3).
2. **No backface culling on polygons crossing the near plane**: such as the hill above the tunnel and the ground when leaving the track; back faces were drawn and covered most of the screen.
3. **Integer projection overflow**: opponents far to the side of the camera became black polygons across the whole screen with original vertices.

**Layers**: from power-on to frame 12000, all 12001 frames bit-identical. This span includes:
- The title demo, mode select, course select
- The start and the race
- Time up, game over, the SEGA logo, back to the title

**3D and sprites** (original vertices):

| Frame | Screen | Same colour | Only SVP drew | Only HLE drew | Pixels differing from the original (whole screen) |
|---|---|---|---|---|---|
| 2612 | Start: pit crew, PIT START | 94.21% | 108 | 101 | 2262 |
| 2812 | Start: countdown "2" | 91.57% | 107 | 101 | 3027 |
| 3510 | Racing | 95.52% | 405 | 75 | 1826 |
| 4402 | Racing | 92.55% | 130 | 54 | 2934 |
| 4407 | Racing | 93.97% | 144 | 23 | 2440 |
| 4410 | Racing | 93.50% | 142 | 36 | 2477 |
| 4492 | EXTEND TIME | 89.10% | 107 | 117 | 3307 |
| 8212 | BEST LAP, PIT WORK | 94.16% | 119 | 194 | 1966 |

"Same colour" is the share of pixels drawn by both sides that have the same colour (about 30,000 to 39,000 pixels). The whole screen is 57,344 pixels.

The remaining differences:
- **Near clipping**: polygons clipped by the SVP are smaller than the geometrically correct shape (`svp-commands.md` section 7), so large polygons close to the camera, such as kerbs, pit walls and the pit buildings at the start, are a bit short in the original and show what is behind them. HLE leaves clipping to the GPU, so the shapes are correct but differ from the original. This is the main reason the start and frame 4492 score lower.
- **Single-pixel lines at polygon edges**: the SVP rasterizer's edge rules differ from the GPU's. For tiny or nearly degenerate triangles, such as the grass thrown up by the wheels, the GPU draws no pixels while the SVP draws a thin line.
- A few long thin distant polygons, and the drawing order within a bucket.

The floating-point projection (key 2) was measured before layer compositing was added: about 3 to 4 percentage points lower, all from rounding of vertex positions.

Polygons hit by near clipping have no SVP integer projection, so they always go to GPU clipping with floating-point coordinates. After switching to untruncated floating-point coordinates, their shapes moved both closer to and further from the original, changing each frame's number by less than ±1 percentage point.

## Interpolation (smooth motion) [Verified: interpolated camera yaw and sky position logged per frame, plus visual checks of consecutive frames]

The game logic runs only once per tick (4 frames) (`game-loop.md`), so the original 3D is 15 fps. HLE interpolates between ticks and draws the 3D at the display's refresh rate (key 7 in `vrr`, on by default).

- **Snapshots**: when each command is issued, the contents of all 64 model slots and the frame number are saved, keeping the latest two (A older, B newer). The 68k writes the model slots only in the phase that issues the command, so every snapshot is a complete tick.
- **Ratio**: t = (now − frame B was issued − 1) ÷ (frames between B and A), clamped to 0 to 1.
  - "Now" includes fractional frames of the display: at 120 Hz each emulated frame is drawn twice (0 and 0.5).
  - Right after B is issued t = 0, the state of A. The previous pair is at exactly t = 1 at the same moment, so the motion is continuous.
  - The divisor is the length of the previous tick, so when the SVP slows down and a tick takes 5 frames, the picture slows with it.
- **Interpolated fields**: words 1-6 of a slot, the position and angles.
  - Positions use 16-bit differences, so coordinate wrap-around is fine.
  - Angles have 4096 per turn and take the shortest way. During a race the angle changes by at most about 1360 per tick (wheels spinning), less than half a turn.
  - Only "the same object" is interpolated: the camera slot (slot 0) always; other slots must exist in both ticks (word 0 not 0) and move by no more than 1024. During a race positions move by at most 188.
  - The model need not be the same, because one object can change models (the grass and smoke beside the tyres are animations that change pictures every tick).
  - Other fields (model pointers, which track sections to show, model lists) always come from B. Track sections change only every 20 to 60 ticks or so.
- **Rebuilding**: the whole build (matrices, vertices, culling, sorting) runs again with the interpolated slots.
  - The integer path rounds its inputs, the float path uses the fractions directly. Without interpolation the inputs are all integers and the result is bit-identical to before (the regression numbers are unchanged).
  - The SVP sprites come from B.
- **Latency**: in the original it takes 9 frames from issuing a command to displaying it. The interpolated picture starts moving towards B in the frame after B is issued and reaches B in the 4th frame, about 5 to 8 frames earlier than the original. The HUD is VDP sprites the 68k updates directly every tick, which already led the 3D in the original; now the two are more in sync.
- **Switching to commands HLE does not draw** [Verified: logged per frame which command the interpolated and the latched pictures came from, at the end of the SEGA logo and at the end of a Medium race]:
  - Only commands drawn by HLE save snapshots. After the last command of the SEGA logo (frame 350) the 68k sends command 1 (a communication test that draws nothing); at the end of a race it sends command 3.
  - Interpolation used to stop as soon as such a command arrived and fall back to the latched picture. But the latched picture is 2 ticks behind the interpolation, so it jumped back and caught up again: a flash of an earlier state when the SEGA logo came together, and the same at the end of a race.
  - Now interpolation runs on to the last tick drawn by HLE and stops there, waiting for the latched picture to catch up. Only when the picture changes to the new command's image (the 2D logo at frame 363 for the SEGA logo, frame 8564 for the race) is the original output shown.
  - The title's "V.R." picture (command 7) is just a picture and saves no snapshot either; when the latched picture is this one it is shown directly (see "Title screen").
- **Camera cuts** [Verified: change of slot 0 logged command by command, a whole Medium race and one round of the title demo]: the title demo cuts between cameras, with the camera angle jumping 645 to 2046 at once (4096 per turn), or the offset (slot 0 words 1-3) jumping 179 to 280. Interpolation would swing or fly the camera there within one tick, so when any angle changes by more than 384 or any offset by more than 150, slot 0 is not interpolated and takes the new value directly. The usual maxima are: angle 216 and offset 34 during races, angle 186 and offset 26 in the title demo. [Verified: svptrace, slot 0 per tick while pressing X, Y and Z in a race] Changing view in a race is not a cut: the game glides the camera to the new offset over about 70 frames, by up to 118 a tick at first (X to Z and back). An earlier limit of 48 took those ticks for cuts, so the camera stepped at 15 fps while the cars moved smoothly. The same limit also caught the SEGA logo (command 5), where slot 0 is the flying S, not a camera: it moves 59 to 122 a tick, so 78 of its 79 ticks were taken for cuts and the S moved at 15 fps; with 150 only the first tick is (from an empty slot) [Verified: svptrace, slot 0 of every command 5 from power-on].
- **Sky**: see `screen-layers.md`, "Sky scrolling". With interpolation, Plane B shifts with the interpolated yaw and turns smoothly together with the 3D.
- **Always float vertices**: the SVP's integer projection has up to about 1 pixel of truncation error per vertex, and object rotation matrices are only 1.7 fixed point, with larger errors.
  - In the original the picture moves several pixels per tick and these errors cannot be seen. Interpolated, it moves about 1 pixel per frame, and the errors become jitter.
  - [Verified: a fixed world point ahead of the car tracked and compared frame by frame] In a turn the float projection moves smoothly by 0.7, 1.2, 1.8 pixels per frame, while the integer projection moves 0, 1, 0, 1, 1, 1, 2, 1 pixels. Play testing also showed the whole picture and the car shaking sideways in turns.
  - So the interpolated picture does not use original vertices (key 2 only works with key 7 off).
- The original display (key 7 off) and all `-d` checks still use the latched picture (pipeline step 6).

## Vertex precision at high resolution, and artefact checks [Verified]

- **Problem**: the SVP's camera coordinates are integers, truncated at every step. Object rotation matrices are only 1.7 fixed point (`0x80` is 1.0). One camera unit is about 64/z native pixels, so vertices close to the camera (z about 20 to 100) jump 0.6 to 3 native pixels at a time, 2.5 to 12 pixels at 4×. When the picture moves, edges visibly jitter at high resolution.
- **Method**: every matrix in `hle.c` is kept twice, an integer version as the SVP has it and an untruncated floating-point version (exact sin/cos for rotations, world units for translations, the camera position not rounded either). Every rotation and translation is applied to both, so there is only one set of matrix chaining code. Vertices get both results:
  - The integer result decides culling, the depth key and the projection for original vertices, so the polygons drawn and their order match the original.
  - The floating-point result goes to the GPU.
- **Verification**:
  - For on-screen vertices, the floating-point projection differs from the SVP's integer projection by 1.2 native pixels on average. The largest differences are on objects very close to the camera, such as grass at z about 20. Integer truncation makes z nearly 1 too small, enlarging the projection by a few percent; that is an error in the original itself.
  - The floating-point and integer camera coordinates differ by about −0.6 to +0.8 units, the accumulated truncation. Vertices very far away overflow in the SVP, and such polygons are dropped as too distant anyway.
  - At 4× the player car's left and right tyres become the same width; integer truncation made the right one wider.
- **Seam check**: at 4× with floating-point vertices, look for pixels that are not drawn but have polygons on both their left and right, or above and below. At several moments on each of the three courses the count is 0 to 91 pixels, almost all in gaps that should show the sky anyway, such as between treetops. Native 1× has about as many, so high resolution adds no new seams.
- **Sorting**: the drawing order follows only the depth buckets computed from integers, independent of resolution, so high resolution adds no new sorting errors.

## Near plane and the in-car view [Verified: `vrr -d N -t 2470` frame-by-frame comparison on all three courses, chase and in-car views]

- **Problem**: in the in-car view (X on a six-button pad), HLE cut off the driver's seat and the steering wheel, leaving only the car's nose. Two reasons:
  - In the in-car view the SVP draws the player car again in a separate pass with 32× precision (`svp-commands.md` section 6.15), which HLE did not implement.
  - HLE always used a near plane of 8, but the SVP's near plane depends on what is drawn (section 6.8).
- **Method**:
  - Each polygon carries its own near plane (`hle_poly.near_z`), passed to the vertex shader for GPU clipping and also used for dropping and the depth key.
  - In the in-car view (`30FE0E` not 0, stored in the snapshot; interpolation uses the newer one), the first pass skips slots 1-10, and slots 1-5 are drawn at the end in 32× units, sorted on their own and appended to the end of the list, so they cover everything. The different units do not matter, because the projection (x/z, y/z) is unit-free.
- **Near plane of the track**: the SVP uses 16, but with 16 HLE's frame-by-frame match with the SVP got worse (Expert: 518 frames worse, 217 better). The difference came from the long thin white road lines: polygons with vertices between 8 and 16 switch to GPU projection and land one pixel off at 1×. Between 8 and 16 is below the screen in the chase view, so HLE keeps clipping at 8 while the depth key uses the SVP's 16.
  - Changing only the player car and opponents to the SVP's values, with the track depth key at 8: Expert 132 frames better, 0 worse, but Beginner and Medium had 20 and 40 frames slightly worse. With the track depth key back at 16, those frames all improved too.
- **Results**:
  - Chase view (the original regression test): compared with before the change, 288 frames on Beginner, 284 on Medium and 132 on Expert match better, and no frame matches worse. Averages went from 94.61%, 94.84%, 94.09% to 94.64%, 94.87%, 94.10%. All screenshots are bit-identical to before.
  - In-car view (X pressed at frame 2800, compared frame by frame from 2900): averages from about 65% to 94.96% on Beginner, 95.11% on Medium, 94.24% on Expert. The frames below 85% (lowest 78.98%) are while following right behind an opponent whose tail crosses the near plane, where the SVP and the GPU clip different shapes (the first item of "Not handled yet" below).

## SEGA logo [Verified: `vrr -d 360 -t 30`, plus comparison with the SVP's polygon list entry by entry]

The SEGA logo at power-on (command 5) is also drawn by HLE; method in `svp-commands.md` section 6.16.

- **Animation**: the 68k only sends the time (`30FE10`); the SVP computes the positions and angles of the 8 letter pieces itself. HLE computes them with the same fixed-point arithmetic when the command is issued (`compute_logo_slots`), including the low remainders of the accumulator, bit-identical to what the SVP writes back into the slots (all 80 commands, 632 slots). The results go into the snapshot and are interpolated like race slots, so smooth motion (key 7) works here too.
- **Matrices, sorting**: no camera; each piece uses its own Y, X, Z rotations and position directly; near plane 2; depth sorting uses this command's coarser buckets (32 depth keys per bucket).
- **Results**:
  - The polygon list of frame 114 is identical to the SVP's (164 polygons, same order). This also confirms that a bucket is drawn in order of creation (drawn the other way round, the average drops to 84%).
  - 1× frame by frame, 90.0% average over the whole sequence. Once the letters grow (from frame 300) it is 95% to 96.6%; earlier the letters are only a few pixels across, edge rasterization differences weigh more, with the lowest 80.4% (frame 119).
  - When the Medium regression test reaches frame 9000, the race has ended and the game is back at the SEGA logo, so the last frames include the logo too.
- **Widescreen**: the 3D extends to the sides and up and down as well (the middle 256 columns are pixel-identical to 4:3), and the letters fly wider over the black background. Plane B is the centred SEGA silhouette, without the race's wrap-around and mirroring.
- This screen does not overclock the SVP with key 6 (only races do), so the time from power-on to the title is the same as the original.

## Title screen [Verified: `vrr -d 3500 -t 690` frame by frame over a whole attract round, plus widescreen screenshots]

Without any button presses, the title screen runs: command 6 (title demo, frames 695-1183) → command 8 (demo with the ranking, frames 1188-2775) → commands 6 and 7 alternating every two commands (the big "V.R." flashing, frames 3015-3191) → command 6 (demo ends, the title picture closes from top and bottom, frames 3195-3491) → back to the SEGA logo. All three commands are drawn by HLE; method in `svp-commands.md` section 6.17.

- **3D**: the same pipeline as races (same geometry, rasterization), only slots 1-10 are computed differently: the player car (slot 1) is placed at its own world position like an opponent, slots 2-4 hang off slot 1, and slots 6-10 (tyre smoke and the like) take the car's transform as in races. [Verified: `vrr -d 730`, the title demo] Slots 6-10 were first placed at their own world positions, which are 0, so the smoke was missing from the title demo and the replays; with the car's transform frame 730 went from 94.94% to 95.95%, and the attract round (`-d 3500 -t 690`) from 95.91% to 95.93% on average.
- **Ranking** (command 8): each of the first 6 sprite slots is one row; HLE assembles the 23 characters as the SVP does (position, name, time, the V.R. mode icon) and draws them with the ASCII font. Word 0 of a slot is how many characters to show, so the text types out one character at a time.
- **Title picture** (`30FE10` of command 6): two picture bands close in from off screen above and below, covering everything at `0x60`, after which no 3D is drawn.
- **"V.R." picture** (command 7): an RLE-compressed full-screen picture, no 3D.
- **Frame by frame**:

  | Part | Frames | Average | Below 85% |
  |---|---|---|---|
  | Title demo (command 6) | 483 | 94.5% | 0 |
  | Ranking demo (command 8) | 1591 | 96.0% | 0 |
  | "V.R." flashing (6 and 7) | 185 | 97.5% | 0 |
  | Demo end to title picture | 301 | 95.6% | 5 |

  - The title picture and the "V.R." picture themselves are 100%.
  - The 5 frames below 85% are from frame 3496 on: the game has faded the screen to black, the SVP side has no pixels to compare, but the composite is pixel-identical to the original.
- **Widescreen**:
  - The title demo, like races, wraps the sky to the sides and mirrors it up and down.
  - The title picture and the "V.R." picture fill the whole buffer; in widescreen they extend from their edge pixels to the sides and up and down. So no 3D shows on the sides during the transition, and no sky shows behind the full title picture.
- **Smooth motion**: the title demo is interpolated too, except at camera cuts (see "Interpolation"). While "V.R." flashes, the same command repeats 12 frames apart, so there is no interpolation and the original rhythm is kept.

## Finish and time up [Verified: `vrr -d 8900 -t 8500`, Medium time up]

Command 3 after the race is also drawn by HLE; method in `svp-commands.md` section 6.18: the whole scene is drawn as in a race, then slot 57 (the result's 3D text, such as "GAMEOVER") on top.

- **Frame by frame**: during command 3 of the Medium time up, 94.4% on average, the same as a race. (At first the title demo's way of computing slots 1-10 was used for command 3 by mistake, giving only 92.3%; command 3's stage 1 is actually the same as the race's, and the fix raised it.) Afterwards the game fades the screen to black (92 frames from frame 8872), where the SVP side has no pixels to compare; spot checks of a few frames show the composite pixel-identical to the original, and `-t` leaves these frames out of its statistics.
- **Regression test**: Medium (`-t 2470` to frame 9000) now includes the time up screen: 6430 frames, 94.72% average, no frame below 85%. Beginner and Expert have not timed out before frame 9000, so their numbers are unchanged.
- **Smooth motion**: when the race (command 2) passes to command 3 it is the same scene, so one interpolates straight into the other without going backwards. After the finish the camera circles the car at about 32 per tick, far below the camera cut threshold, so it is interpolated smoothly.
- **Widescreen**: the sky extends as in races.
- The finish screen has no automated test input (it takes actually finishing a race); only the camera data was checked with test input that changes the lap count. The picture needs checking in play.

## Replay [Read from the code; the picture needs checking in play]

The replay of the top three after the race (commands A, B) is also drawn by HLE; method in `svp-commands.md` section 6.19.

- **A**: a chase camera; slots 1-10 are placed in world coordinates as in the title demo.
- **B**: a fixed trackside camera; words 1-3 of slot 0 are the camera's world position directly.
- **Smooth motion**:
  - When switching between A and B, slot 0 means different things (offset and world position), so the camera cuts over without interpolation; the cars and other objects are interpolated as usual.
  - A fixed camera can turn very fast when a car passes close by, so camera cuts in B only look at whether the position jumps (by more than 1024), not the angles.
  - Race, finish and replay all count as the same scene, so interpolation carries on between them without going backwards.
- **Widescreen, far view**: as in races (sky extended, extra track sections ahead and behind).
- **The sky turns with the camera** (with smooth motion): [Source: observed in side-by-side mode] the original replay does not scroll the sky; however the camera turns, Plane B stays where it is. HLE treats the replay as a track screen too, and with smooth motion places the sky by the camera's yaw (`screen-layers.md`, "Sky scrolling"), so the sky turns with the camera and lines up with the 3D. This started as a side effect of applying the race rules, and was kept after review (2026-10-06). With smooth motion off, the sky uses the scroll the game sets and stays still as in the original.

## Widescreen [Verified: `vrr -W -d N`, frames 3510, 4492, 8212]

Key 8 in `vrr` (or `-W`) changes the picture from 4:3 to 16:9.

- **Width**: the race screen is H32 (256×224) with a pixel aspect of 8:7. At 16:9 a 224-line picture is 224 × 16/9 ÷ (8/7) = 348.4 game pixels wide, so 46 columns are added on each side, making 348×224.
- **Projection (Hor+)**: the vertical FOV stays, the horizontal FOV widens.
  - The HLE buffer widens from 256 to 348 columns, with the same pixels per unit (focal length 64), and the projection centre moves 46 columns right: screen x = 46 + 129 + 64x/z.
  - In clip space x = (2z + 128x) ÷ buffer width. At width 256 this equals the original (z + 64x) ÷ 128 exactly in floating point.
  - The integer projection of original vertices (key 2) moves 46 columns right as well.
  - Dither positions are still computed in the original buffer's coordinates (minus 46).
  - The middle has exactly the original proportions; only more is seen on the sides.
- **Filling the top and bottom borders**: the game's 3D covers only lines 15 to 206, with a black border above and below (where the game turns the display off for DMA). At 16:9 that looks like letterboxing, so in widescreen the HLE buffer also gains 15 lines above and 17 below, covering all 224 lines.
  - Same proportions again; only more is seen vertically too (sky above, near road below).
  - Clip space: y = ((buffer height − 2 × lines above − 192) z + 160y) ÷ buffer height. With the original 192-line buffer this is the original 80y ÷ 96.
  - Plane B is empty on these lines ([Verified: Plane B separated by `vdplayers.py`, frame 3509]), so lines 15 to 206 of Plane B are mirrored across the top and bottom edges. At first line 15 was simply repeated, but when a cloud happened to be on line 15 it became vertical stripes going up.
  - The SVP sprites (text, countdown digits) stay where they were.
- **Verification**: at 1× with original vertices, the middle 256 columns of the widescreen picture are almost pixel-identical to the 4:3 full colour picture (checked before the borders were filled):
  - Frame 3510 is identical.
  - Frames 4492 and 8212 differ by only 3 and 2 isolated pixels, all on edges of polygons projected by the GPU because they cross the near plane or overflow the integer projection. The clip space divisors differ, and floating-point rounding moves an edge by one pixel.
  - Without widescreen, all regression tests and screenshots are bit-identical to before.
- **HUD**: in races, at the finish and in replays, the left and right HUD groups move out by 46 columns to the screen edges, and TIME stays in the middle (`screen-layers.md`, "Widescreen").

## Widescreen edges (culling range) [Verified: software rasterizer measurements over whole races, plus comparison images from `vrr -W -F -d N`]

What is seen on the widescreen sides never needed drawing in the original. Every source that could leave things missing on the sides was checked:

- **Opponent cars**: the 68k decides whether to put a car into a slot (detail level `+C4`) only by its distance from the player, not by the view (`svp-commands.md` section 2). So a wider view does not lose cars.
- **Object slots and the SVP's culling**: HLE transforms every model in the slots and leaves what is off screen to GPU clipping, without culling by screen x. The SVP's own culling of off-screen polygons does not affect HLE.
- **Track sections**: this is the only source. The 68k picks 8 sections **in order along the track** (from the player's section forwards, section 4.1), enough for the 4:3 view. Widescreen sees two more kinds of sections:
  - Sections **behind** the player: hills and trees by the road are on the sides, and once the player passes, that section is no longer among the 8, so things suddenly vanish from the sides of the screen.
  - Sections seen across a bend: far away in order (many sections ahead or behind) but in view in space, such as Medium's big bridge or the road on the far side. The original does not see these in the middle of 4:3 either; widescreen often shows them on the sides.
- **Measurement**: a tool rasterizes HLE's draw list itself at every command, at 1× widescreen (348×224) with the painter's algorithm, twice: once with the original list, once with some sections added, and compares pixels whose colour differs, split into the 4:3 range and the widescreen-only sides. At first it only compared "which polygon is on top", which also counted ground overlapping itself in the same plane and distorted the numbers; comparing colours is correct.
  - Samples: a whole race on each of the three courses (throttle only, about 1500 to 1900 ticks), in four views (default, X, Y, Z; see `svp-commands.md` section 2).
  - The original 8 sections plus 3 behind: differences on the sides appear in about 1% to 4% of ticks, up to about 1000 pixels at a time. The 1st and 2nd sections behind account for most of it.
  - The old far view (16 more sections ahead) compared with "draw the whole track" still misses 60,000 to 670,000 pixels on the sides per race on the three courses, more in the high view (Z).
  - The far view (16 more ahead) plus 3 behind: in the default view the side differences are about as above (Beginner 74 ticks, up to 1085 pixels at a time). In the high view (Z) the camera is 616 behind and 540 above the player and sees the road and ground behind, so 3 sections behind fill in more (Beginner 333 ticks, up to 5670 pixels at a time), including holes in the lower left of the original's own 4:3 picture.
  - Compared with 20 sections behind, what remains are distant ridges and roads seen across bends.
- **Method**: with the far view (key 9), **3 more sections behind** are drawn besides the 16 ahead (`SECTIONS_SIDE`; facing the other way it is the reverse, see "Far view"), keeping the game's own way of picking "consecutive sections" and only extending the range ahead and behind. Hills and trees by the road stay on the sides after the player passes instead of suddenly vanishing.
  - Sections seen across a bend (far in order, in view in space) are not drawn, as in the original and the old far view.
- **Tried and rejected**: picking sections **by space**, drawing every section within 32767 units of the camera and dropping only polygons beyond the 16-bit range. Compared with "draw the whole track", that differs by 0 pixels on all three courses in all four views, but in play the view reached too far and looked strange:
  - The track is elevated with no ground under it (the ground and sea are Plane B's background picture), so distant track seems to float in the air.
  - Some large hill sections are not far from the camera but belong to distant parts of the track, and are drawn in front of the hills in Plane B's background picture.
  - The verdict in play testing: the old far view's distance (16 sections ahead) is far enough, and what is far away should still be cut.
- **With the far view off (key 9)**: the original 8 sections, in 4:3 and widescreen alike, bit-identical to the original. Widescreen then has gaps on the sides as the original would. Adding only sections that lie "entirely within the original depth limit" was also tried, but it changed the middle of the picture (about 190 pixels per tick on average on Medium), no longer the original look, so it was rejected.

## Far view (longer draw distance) [Verified: `vrr -F -d N`, comparison images of original distance and far view at frames 3180, 3510, 4410, 6100]

Toggled with key 9 in `vrr`, on by default (`hle_set_far_view`).

- **Original distance**: the 68k picks only 8 consecutive track sections at a time (from the player's section, `svp-commands.md` section 4.1), reaching only about 6600 units ahead. Polygons with a depth bucket above 2047 (z above about 8190) are also dropped. So hills and trees ahead pop up suddenly at about 6600 units.
- **More sections ahead**: after the 68k's 8 sections, 16 more are drawn following the numbering forwards (about 15,000 more units). 3 more are drawn behind as well, for the widescreen sides (previous section, "Widescreen edges").
  - **Which side gets the 16** [Verified: a save state facing the wrong way on the Beginner straight, `vrr -l -d -F`; the far views of frames 3180, 4410 and 6100 racing normally bit-identical to before]: the game picks its 8 sections in the race direction whichever way the camera faces, so driving the wrong way (or a replay camera looking back down the track) saw only the 3 behind, about 3,000 to 4,700 units, too near even for the haze to start. Now each side is judged by where the camera looks: the second section beyond the game's 8 on that side, the direction to the middle of its vertices seen from above. A side less than about 107° from the view gets `SECTIONS_FAR` (16), the other `SECTIONS_SIDE` (3); turned sideways, both get 16. Racing normally that is still 16 ahead and 3 behind.
  - The number of sections comes from the track's pointer table: 128 for Beginner and Medium, 166 for Expert. Numbering wraps to 0 at the section count.
  - The distance from distant sections to the camera would overflow the SVP's 16-bit arithmetic, so culling and the depth bucket of these sections use float coordinates (rounded, clamped to the 16-bit range), and they always go to GPU projection. The limit is never hit in practice: over whole races on all three courses (default and high view), the farthest vertex of these sections is about 23,700 units.
  - When the float path computes track vertices, differences from the camera no longer wrap at 16 bits as in the SVP. This only affects distant sections; the original 8 are never far enough to wrap, so their results are unchanged.
  - The depth limit is raised to the largest bucket a 16-bit z can reach (8703). Distant polygons above 2047 in the original 8 sections are kept as well.
- **Distant cars**: the 68k only puts opponents within about 7700 units into slots, but it keeps records for all 15 opponents, with positions and angles (`svp-commands.md` section 2).
  - With the far view, at the moment a command is issued HLE reads from RAM the cars the game does not draw (`+C4` is 0) and draws them with the game's own farthest level (level 3) body model, out to 24,000 units. The wheels are smaller than a pixel and left out.
  - When a car comes close enough for the game to draw it, the game uses exactly the same level 3 model at the same position, so the handover cannot be seen.
  - These cars go into the snapshot with the slots and are interpolated too. This only reads 68k memory, in line with the architecture rules.
- **Effect**: at frame 3180, for example, the original horizon is empty beyond the first row of trees; the far view shows the road winding into the distance, the woods on both sides and opponents on the road far ahead.
- With the far view off (and in all `-d` checks), the result is bit-identical to the original.
- **Haze** (the third choice of key 9, the default when playing, full colour only): distant 3D gradually fades by depth into "the original picture behind it", the sky and Plane B's background picture.
  - Method: HLE's fragment shader also outputs a haze amount (times coverage, `GL_R16F`, resolved together under MSAA). Compositing multiplies the 3D's colour and coverage by 1 − haze, letting the sky or background behind show through. The haze follows the frontmost polygon, so distant hills do not show hills farther away.
  - Amount: rises linearly from `haze_start` to `haze_end` (`vrr.ini`, in thousands, 6 to 12 by default).
  - [Verified: whole races with the far view, the farthest polygon depth on screen logged every tick] The end of the far view varies a lot with the bends: the 25th, 50th and 90th percentiles are 11,561, 19,150, 23,411 on Beginner and 10,298, 15,398, 20,173 on Medium. With the original distance, the farthest polygon never exceeds 8,159 (median 6,231).
  - The first version hazed only beyond the original distance (8,192 to 24,000, smoothstep, later squared). At the median end it was only about 20%; the difference was hard to see and it did not help with the cut-off, so it became linear from 6,000 to 16,000, and later (2026-10-07) 6,000 to 12,000 for a stronger haze. The cost is that the farthest part of the original distance (6,000 to 8,192) gets up to about 35% haze too.
  - Limitation: the haze is at a fixed distance while the end of the far view varies, so the cut-off is not always hidden. The verdict in play testing was "doesn't quite fit, but keep the option" (2026-10-05).

## Not handled yet

- **The SVP's near clipping shapes**: see above. GPU clipping is kept for now (geometrically correct, and side by side the difference cannot be seen). If the original look is wanted later, study the SVP's clipping arithmetic and make it an option.
- **Sprite types 2, 5, 9**: they do not appear in races or on the title screen (type 5 has the same format as the "V.R." picture, see `svp-commands.md` section 3.2).
- **Other commands** (mode select 09, 2P 04, C): the 3D keeps the SVP's output, and the 2D is composited from the layers as usual. Menus (mode select and so on) and the 2P split screen deliberately keep the original picture without HLE redrawing (few people would play 2P in this enhanced version, and the work is not worth it). The SEGA logo (05), title (06, 07, 08), finish (03) and replay (A, B) are drawn by HLE (see "SEGA logo", "Title screen", "Finish and time up", "Replay").
- **The SVP's off-screen culling**: the SVP drops a whole polygon when any vertex's camera `|x|` or z exceeds a limit (`svp-commands.md` section 6.8), even when part of it is on screen, such as a piece far away at the left edge in Beginner frame 3500. HLE does not emulate this and leaves it to GPU clipping, so distant things at the screen edges are more complete than the original, and 1× comparisons count those spots as mismatches.
- The limit of 128 polygons per bucket is not emulated; it is never reached in practice.

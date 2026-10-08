# SVP command interface and data formats


This document records the interface between the 68k and the SVP in Virtua Racing, and the structure of the SVP program. It is also the specification for the HLE renderer (redrawing the 3D on the GPU), and is updated as the research goes on.

**Labels**

- **[Verified]**: confirmed with this project's tools, with the method in brackets.
- **[Source]**: taken from outside research, not confirmed here yet.
- **[Speculation]**: inferred from the data, still needs verifying.

**Address conventions**: 68k addresses are byte addresses, such as `30FE08`. SVP program addresses are word addresses, such as `0425`. Word `W` of the ROM is at byte `2*W`. All addresses refer to the USA version (SHA-1 `ff969ae5…`).

**Sources**

- **notaz**: `third_party/genplus-gx/core/cart_hw/svp/svpdoc.txt`, and the [2014 revision](https://segaretro.org/SVP_documentation_(2014-09-23)) (Sega Retro blocks crawlers; fetch it from web.archive.org instead).
- **Devon** (forum name Ralakimus): the [Sonic Retro thread](https://forums.sonicretro.org/threads/sega-virtua-processor-virtua-racing-research.38296/), the first post of 2018-11-12 (DRAM layout, command list, model format) and the post of 2019-10-21 (game loop and DMA).
- **svp-blender**: [Ralakimus/svp-blender](https://github.com/Ralakimus/svp-blender), import and export code for the model format, MIT licence.
- **svpdev**: [JaviTaiyou's wiki](https://github.com/jdesiloniz/svpdev/wiki), including real-hardware timing and the SVP's internal ROM.

---

## 1. The interface as the 68k sees it

### 1.1 Control registers [Source: notaz]

| Address | Use |
|---|---|
| `A15000` / `A15002` | Command and result register, called XST on the SVP side. At power-on the 68k writes `"OH"` (`4F48`) to initialise the SVP |
| `A15004` | Status. Bit 0 means the SVP wrote XST, bit 1 means the 68k wrote XST |
| `A15006` | The 68k writes `000A` during DMA and `0` afterwards; presumably pauses the SVP so it does not compete for the bus |

### 1.2 DRAM layout (`300000-31FFFF`, 128KB) [Source: Devon]

| Range | Use |
|---|---|
| `300000-305FFF` | Tile buffer 1 |
| `306000-30BFFF` | Tile buffer 2 |
| `30EC00-30FBFF` | 64 model slots, `0x40` bytes each (section 2) |
| `30FC00-30FDFF` | 32 sprite slots, `0x10` bytes each (section 3) |
| `30FE00` | Screen mirror flag |
| `30FE02` | Command done flag (written by the SVP) |
| `30FE04` | Word offset of the tile buffer, see section 1.3 |
| `30FE06` | Command sent flag |
| `30FE08` | Command number |
| `30FE0E`, `30FE10` | Command parameters |

Use of `30FE10` [Source: Devon]: in the transitions at the end of the SEGA logo and the title screen, it is the animation frame number.

Use of `30FE12` [Verified: disassembly of `1e02`]: **the projection depth offset**. The SVP stores it in `ram1:10`, and projection uses `z' = z − offset`. It is 0 during races.

Use of `30FE0E` [Verified: disassembly of `088a`]: command 2 reads `30FE0E` into `ram1:c8` at the start, and if it is not 0 runs an extra pass (section 5.4).

### 1.3 Command sequence [Verified: measured with svptrace]

```
W 30FE04 = 0000 or 3000   tile buffer (alternates every command)
W 30FE06 = 0001           sent flag
W 30FE02 = 0000           clear the done flag
W 30FE08 = command number the SVP's dispatch loop starts as soon as it sees non-zero
```

`30FE04` holds a word offset: `3000` words are `0x6000` bytes, tile buffer 2 at `306000`.

[Verified: svptrace -m, frames 3500 and 4400] With `30FE04 = 3000` the SVP draws into **buffer 1** (`300000`), with `30FE04 = 0000` into **buffer 2** (`306000`). So `30FE04` names the buffer being displayed, and the SVP draws into the other one.

[Verified: `vrr -d -t` frame by frame, each of the three courses from the start to frame 9000, every frame matches] **When Plane A switches to buffer X, the screen shows the result of the most recent command into X that the SVP has finished**:

- When the SVP finishes a command it sets `30FE02` to non-zero (the done flag). The 68k waits for it before issuing the next command, and clears it to 0 when issuing.
- The 68k DMAs the finished DRAM buffer into VRAM in parts, and switches Plane A's scroll only after the DMA (1 for buffer 1, 209 for buffer 2); the switch always happens around line 5.

**Normal case** (racing with the SVP keeping up, one command every 4 frames):

| Frame | What happens |
|---|---|
| N | Command issued; the SVP starts drawing into DRAM buffer X |
| N+4 to N+7 | The 68k DMAs the finished X into VRAM in 3 parts (meanwhile the next command draws into the other buffer) |
| N+8 | The **next** command starts drawing DRAM buffer X again |
| N+9 | Switch to X. The command of frame N+8 has not finished, so the result of frame N's command is shown |

From issuing a command to showing it on screen takes 9 frames, which looks like "show the previous command of the same buffer".

**When the SVP is overloaded** (Medium and Expert starts, with 16 cars plus the pit crew on screen):
- Command intervals become 4, 5 or 6 frames, and the order becomes "**switch to X first, issue X's next command one frame later**".
- Then the **latest** command of X is shown. For example on Medium with the throttle held: frame 2993 issues the command for buffer 1, finished in frame 2997; frame 3002 switches to buffer 1, and frame 3003 issues the next command. Frames 3002 to 3007 show the command of frame 2993.
- Applying the "previous command" rule would pick a picture one round older (about 10 frames ago), which looks like the picture going backwards. That is how HLE went wrong at first.

Other notes:
- By the time the picture switches, DRAM buffer X may already be redrawn by the next command. To get "the 3D on screen", decode Plane A from VRAM, or read it as each scanline is drawn; do not read DRAM.
- An earlier claim that "frame 3500's command appears in frame 3510" was wrong; it was based on comparing whole screenshots, which was not enough evidence.

### 1.4 Command list

Command numbers and uses [Source: Devon]; handler addresses [Verified: disassembly of the jump table at `0438`]; screens where they appear [Verified: measured with svptrace].

| Number | Handler | Use | Seen in |
|---|---|---|---|
| 0 | `0425` | Invalid (keep waiting) | |
| 1 | `0448` | Communication test | Power-on |
| 2 | `088a` | Models and sprites with a camera (race screen of Virtua Racing mode and Free Run) | Races |
| 3 | `0959` | The race continues, plus slot 57 (the result's 3D text), used at the finish or time up (section 6.18) | Time up (Medium frames 8555-8871), camera work after the finish |
| 4 | `0c6f` | Models and sprites with a camera, split screen (2P) | |
| 5 | `0ee5` | SEGA logo animation; the SVP computes the model positions itself from `30FE10` (section 6.16) | SEGA logo (frames 34-350 after power-on, every 4 frames) |
| 6 | `0aee` | Models with a camera (title demo); when `30FE10` is not 0, the title picture closes from top and bottom (section 6.17) | Title demo (frames 695-1183 with no input), demo end (frames 3195-3491) |
| 7 | `0ba2` | Only the "V.R." picture (section 6.17) | Alternates with command 6 every two commands (frames 3015-3191) |
| 8 | `0bc9` | Models and sprites with a camera; the first 6 sprite slots are the ranking (section 6.17) | Title demo with the ranking (frames 1188-2775) |
| 9 | `10e8` | Models with a smaller coordinate range but higher precision (mode select screen) | Mode select |
| A | `0a3a` | Models and sprites, chase camera (replay, section 6.19) | |
| B | `0ac1` | Models and sprites, fixed trackside camera (replay, section 6.19) | |
| C | `1158` | Models with a camera, smaller coordinate range but higher precision | |
| D, E, F | `0425` | Invalid | |

### 1.5 Timing [Verified: measured with svptrace]

- During races command 02 is issued every 4 frames (279 of 280 consecutive intervals are 4 frames, 1 is 5), so the 3D runs at **15 fps**.
- [Source: Devon] The 68k uses the H-INT counter to split the tile transfer into 3 DMA parts over different frames; with setup that is 4 frames, so one 3D picture takes 4 frames. The game logic is in the same loop, also at 15Hz. [Verified] Details in `game-loop.md`.
- [Source: notaz] Overclocking the SVP does not raise the FPS, while underclocking slows it down. This agrees with the above: the bottleneck is the 68k's DMA scheduling, not the SVP.

---

## 2. Model slots (`30EC00 + slot × 0x40`)

Fields [Source: Devon]:

| Offset | Content |
|---|---|
| `+00` | Visible flag |
| `+02` / `+04` / `+06` | X / Y / Z position |
| `+08` / `+0A` / `+0C` | X / Y / Z angle |
| `+26` | Model data pointer (32-bit). An SVP word address plus PMC mode bits (such as auto-increment), so it looks like `0808xxxx` |

Other observations:

- [Source: Devon] In commands 4 and 6, slot 0 is usually the main camera and can also draw the main map. Command 5 treats slot 0 as an ordinary model.
- [Verified: svptrace -m plus `svplog.py track`, during a race] **The 32 words of slot 0 (camera)**:

  | Word | Offset | Content |
  |---|---|---|
  | 0 | `+00` | Visible flag (0 for the camera) |
  | 1-3 | `+02-06` | **Offset from the player car (slot 1)** X, Y, Z, such as (0, 0x22, 0x88) for 34 above and 136 behind (the default chase view; other views below) |
  | 4-6 | `+08-0C` | Angles X, Y, Z |
  | 7-18 | `+0E-24` | Camera matrix written back by the SVP (section 6.9) |
  | 19-20 | `+26-28` | Track data pointer, such as `0009C000` for Beginner, which is ROM `138000` |
  | 21-28 | `+2A-38` | **The numbers of the 8 track sections to draw**, chosen by the 68k, such as 1 to 8 |
  | 29-31 | `+3A-3E` | 0 |

  An earlier guess that "a list of slot numbers follows `+26`" was wrong; those are track section numbers.
- [Verified: words 1-3 of slot 0 logged per tick with an HLE tool] **During races the view is switched with X, Y and Z on a six-button pad** (A, C, up and down on a three-button pad do nothing). After a press the offset moves to the new value over about 70 frames:
  - Default: (0, 34, 136), chase.
  - X: (0, 20, 22), close to the car, roughly an in-car view.
  - Y: (0, 82, 194), a bit higher and further back than default.
  - Z: (0, 540, 616), high view.
- [Verified: svptrace, during a race] Opponent cars use **three consecutive slots**: the first has world coordinates, the other two small positions such as `(0,8,2)` and `(0,8,-0x50)`. All three have different pointers. [Speculation] This is a hierarchy of body, front wheels and rear wheels, with the last two positioned relative to the first.
- [Verified: disassembly of 68k `02381C`, plus the PC at DRAM writes compared with RAM] **Where opponent slots come from**: the 68k keeps a 256-byte record for each of the 15 opponents (RAM `$9100 + n × 0x100`), and `02381C` fills slots 11 + 3n, 12 + 3n, 13 + 3n from them every tick:
  - Position: x = `+2E` × 2, y = (`+30` + 0x32) >> 3, z = `+32` × 2 (16-bit arithmetic).
  - Angles: x = −(`+38` >> 4), y = (`+3A` + `+70`) >> 4, z = −(`+3C` >> 4).
  - Model: `+C4` (detail level) and `+C6` (colour group: 0, 0x10, 0x20, 0x30) look up the table at ROM `0BC428`, at offset `+C4` × 4 + `+C6`; each group has bodies for levels 1 to 3.
  - **Cars with `+C4` = 0 are not drawn** (word 0 of all three slots set to 0). Measured (Beginner): level 1 within about 2200 units, 2 to about 4100, 3 to about 7700, and 0 beyond. So the SVP only gets nearby cars, and distant cars have positions only in the 68k's records.
  - [Verified: disassembly of 68k `028688`, `028708`, `0287CE`] How `+C4` is computed: from the absolute differences from the player record's `+2E` (x) and `+32` (z), level 1 if both axes are within 0x400, 2 within 0x800, 3 within 0xFA0, else 0. In world coordinates (× 2) those are boxes of 2048, 4096 and 8000, agreeing with the measurements above. The `0287CE` variant shrinks level 3 to 0x800 when bit 0 of `$D313` is set. **It looks only at distance, not the view**, so widescreen does not lose cars because of it.
  - The offsets of the sub-objects (wheels) come from the table at ROM `0B5558`, their angles from `+C0` and `+C8`.
- [Verified: access log of svptrace -m, see section 6.1] `+0E` to `+24` are **written by the SVP in stage 1 of command 2**: it reads each slot's position and angles (`+00` to `+0C`) and writes the results back here. [Speculation] They are rotation matrices, because slots without rotation contain values like `0080`, probably 1.0.
- [Verified: svptrace -m] During races the pointer at `+26` of slot 0 (camera) is `0009C000`, ROM byte address `138000`. Stage 3 reads exactly the data from ROM `138004`, so **the track (map) data hangs off the camera slot**, as Devon said ("slot 0 can also draw the main map").
- **[Verified: SVP sine table plus a fit of the camera matrix, section 6.9] Angles have `0x1000` per 360°** (12-bit). An earlier guess of "`0x10000` per 360°" was wrong.
- [Verified: disassembly of overlays `126F` and `15B5`, checked against svptrace -m] **What each slot is for in races (command 2)**:

  | Slot | Use | Stage 1 matrix routine |
  |---|---|---|
  | 0 | Camera, also carrying the track data | `010c`, `014a` |
  | 1 | Player car body | `015a` |
  | 2-4 | Player car sub-objects (wheels), positioned relative to slot 1 | `017a` |
  | 5 | An object in the in-car view, processed only when `30FE0E` is not 0 (in-car view), see below | `01ca` |
  | 6-10 | Effects following the player car, such as tyre smoke in slot 6. **Their own angles and positions are not used**; they reuse slot 1's transform (slot 1's data is still in RAM1 at that point) | `015a` |
  | 11-55 | 15 groups of 3 slots: opponent body plus two sub-objects | Parent `01e4`, children `020b`; the whole group is skipped when the parent is invisible |
  | 56 | A set of models, such as the pit crew at the start (see below) | `0221` |

- [Verified: disassembly of `0221` in overlay `15B5` and `1d97-1dbd` in stage 4, then the start screen compared with `vrr -d`] **Slot 56 is a set of models**, used at the start to draw the pit crew beside the car:
  - The matrix (`0221`) is the opponents' `01e4` without the object's own rotation: identity → translate (position − camera) → the camera's Y, X, Z rotations. At the start the position is the player car's (slot 1).
  - The pointer at `+26` points to a table of 32-bit byte pointers (same format as the track, section 4.1); words 21-27 are 7 indices, each picking a model from the table to draw with the matrix above. **Index 0 is skipped** (the track's 8 indices do not skip 0).
  - For example the indices at frame 2600 are `5, 1D, 1E, 0, 0, 21, 0`.
- [Verified: `svptrace` listing the slots' model pointers command by command over a whole Beginner race, with every model parsed from the ROM] **The effects and the pit crew are flip-book animations that swap models**; the models themselves are still shapes:
  - Slot 6 (tyre smoke, translucent grey and white): cycles `87FB1 → 8801A → 88083`, one per tick, plays backwards at the end, and stops at `87F48`. 8 polygons, 4 per side. The smoke puff with the same number drifts up and back and grows over one cycle, and starts again beside the tyre when the cycle returns to the first picture.
  - Slot 7 (debris in colour 5, 32 polygons): 8 models, one every 2 ticks. A particle with the same number is only the same particle within a pair of pictures.
  - Slot 8: 4 models played backwards (`898C1 → 89738 → 895D3 → 8946E`), each with a different set of polygons.
  - Slots 9 and 10 (grass, triangles in colour C, 16 each): two sets of 4 models each (slot 10 is the other tyre). A triangle with the same number lands at a random place in the next picture; each picture is a freshly scattered cloud of particles.
  - Pit crew in slot 56: the first index counts from 1 to `1C`, changing every 3 ticks (5 fps). Most poses have different polygon counts (65 to 83).
- [Verified] **The format the SVP writes back to `+0E` to `+24`**: 3 rows of 4 words, `m0 m1 m2 t`.
  - Camera (slot 0): `m` is 2.14 fixed point (`0x4000` is 1.0), `t` is 0.
  - Objects: `m` is 1.7 fixed point (`0x80` is 1.0), with the camera rotation already merged in, so it is the "object to camera" rotation; `t` is the object's position in camera space.
- [Verified: `svplog.py matrices`, 40 slots at two moments bit-identical] How each slot's matrix is computed from angles and position, and the parent-child hierarchy, are in section 6.13.

## 3. Sprite slots (`30FC00 + slot × 0x10`)

In stage 6 of command 2 (overlay `229F`) the SVP writes sprites straight into the tile buffer, over the 3D. "PIT START", the big start countdown digits, "EXTEND TIME", "BEST LAP" with the lap time and "PIT WORK" during races are all these sprites, **not** VDP sprites.

### 3.1 Slot format [Verified: disassembly of overlay `229F`, plus the access log of `svptrace -m`]

| Word | Offset | Content |
|---|---|---|
| 0 | `+00` | **Type**; 0 is not drawn. What Devon called the "visible flag" is actually the type number |
| 1 | `+02` | Types 6 to 8: x coordinate in units of 4 pixels; `0x40` or more is not drawn |
| 2 | `+04` | **Position** (see below); types 6 to 8 only give the y part |
| 3-4 | `+06-08` | Types 1, 6: graphics data pointer, same format as model pointers (`0808xxxx`). Types 3, 7: the value to show (0-99). Types 4, 8: a time, 3 BCD bytes in words 3-5 |
| 5 | `+0A` | Types 4, 8: the 3rd BCD byte of the time |

- **The position word is a word offset into the tile buffer**: bits 13-10 are the 16-line strip, bits 9-5 the 8-pixel column, bits 4-1 the line within the strip, bit 0 the left or right half column (4 pixels). In pixels x = column × 8 + half × 4, y = strip × 16 + line. Devon's `00YY YYXX XXXY YYYX` is this address format. For example `0x2180` is (96, 128), and writing starts exactly at buffer + `0x4300` bytes.
- For types 6 to 8 the position is `word 2 + ((x & 0x3E) << 4) + (x & 1)`, converting x to a column and half column and adding it.
- Each frame scans a fixed 32 slots. Dispatch uses a jump table at RAM0 `0xBF + type`, copied at initialisation from program memory word `0x2296` (`2287-228f`).

### 3.2 Types [Verified: disassembly; types 1, 4, 6, 7 and 8, which appear in races, compared with the SVP's output using `vrr -d`]

| Type | Handler | Content |
|---|---|---|
| 1 | `0021` | Picture: the data starts with "width ÷ 4 − 1" and "height − 1", followed by 4bpp pixels row by row (4 pixels per word, high nibble on the left) |
| 2 | `0034` | Text: the pointer points to zero-terminated characters, drawn with the 8×16 font (character − `0x20`), 8 pixels per character. Not seen in races |
| 3 | `0162` | Two big digits (64×80): the value is converted to BCD through the table at program memory `0x26F3`; a tens digit of 0 is not drawn, and the ones digit is 56 pixels to the right |
| 4 | `0095` | Time `M'SS"hh` (8×16 font): 6 digits drawn as "min min ' sec sec " hundredths hundredths", 8 pixels per character; a first digit of 0 is not drawn |
| 5 | `01c4` | RLE-compressed picture: the data starts with "width ÷ 4 − 1" (not read) and "height − 1", then each row is a number of "count, one word" pairs (the word written that many times), with count 0 ending the row (`01d2`, the same as the "V.R." picture of command 7). Not seen in races or on the title screen |
| 6 | `01f7` | Like type 1, but positioned by x coordinate, clipped at the right edge |
| 7 | `0190` | Like type 3, but positioned by x coordinate, not drawn past the right edge |
| 8 | `00d6` | Like type 4, but positioned by x coordinate, stopping at the right edge |
| 9 | `0054` | Like type 4, but with the 8×8 font. Not seen in races |

- **Fonts** (program memory word addresses, no header in the data):
  - 8×8: `0x78000`, `0x10` words per character.
  - 8×16: `0x780F0`, `0x20` words per character. After digits 0-9, `0x0A` is `"`, `0x0B` is `'`, and `0x0C` to `0x0E` are special symbols.
  - 64×80 big digits: `0x79130`, `0x500` words per character.
- If the first digit of a time is `0x0A` to `0x0C`, it becomes the special character `0x0C` to `0x0E`; if it is `0x0D`, the whole string becomes dashes (`012d`).
- Writes use overwrite mode, so colour 0 is transparent; the palette is line 0, as for the 3D.
- Types 1 to 5 and 9 do no clipping. [Speculation] The game makes sure these sprites stay inside the buffer.

### 3.3 When they appear [Verified: `svptrace` over a whole race (to frame 12000)]

| Slots | Type | Frames | Screen |
|---|---|---|---|
| 0, 3, 7 | 1 | 2556-3072 | Start: PIT START, countdown digits |
| 4 | 1 | 4480-9132 | EXTEND TIME and so on |
| 1, 2, 6 | 4, 1 | 8192-8876 | BEST LAP with the lap time, PIT WORK |
| 9-15 | 6, 7, 8 | 9936 (the last command 2) | The screen after time up, then command 3 takes over |

The first 6 slots of command 8 (the ranking on the title screen) are not in this format but rows of the ranking, handled by another overlay; see section 6.17.

## 4. Model data format (in ROM) [Source: Devon, svp-blender]

```
word      polygon count − 1
each polygon:
  byte    colours: high nibble = colour 1, low nibble = colour 2 (palette indices, 0 = transparent)
  byte    flags:
            bit6 (0x40) backface culling
            bit5 (0x20) dither pattern: 1 = checkerboard, 0 = lines
            bit4 (0x10) shape: 1 = triangle, 0 = quad
            bit3-0      related to Z sorting; Devon never cracked them
  vertices signed 16-bit X, Y, Z; 3 for a triangle, 4 for a quad
```

- The two colours are mixed by dither. [Source: notaz 2014] The checkerboard is made by a nibble rotation when reading through the PMC (`abab` becomes `baba`).
- [Source: Devon] No textures and no lighting. Shading is painted on the models with colours and dither.
- svp-blender divides vertices by 256 on import and swaps Y and Z (to fit Blender's coordinate system).

### 4.1 Track data format [Verified: the whole Beginner track parsed, and `svplog.py track`]

- Where the track data is comes from slot 0's pointer (ROM `138000` for Beginner).
- It starts with a **table of 128 32-bit byte pointers** (`address + 4 × section number`), each pointing to a section.
- After the pointer table (from `138200` for Beginner) comes the data of each section in order. **Every section is a standard model in the format of section 4**, with vertices in world coordinates.
- Beginner has 128 sections and 3,264 polygons, 10 to 54 per section. Parsed one by one, every section ends exactly where the next begins.
- Each command draws only the 8 sections the 68k gives in words 21-28 of slot 0 (`001f-0029` in overlay `19B5` reads the addresses from the pointer table, `1d87` reads the section numbers).
- All this is the track's "own" geometry. Trees, signs and so on beside the road are part of the sections' polygons too ([Speculation] judged from the picture).
- [Verified: pointer tables of all three tracks parsed] **The number of sections differs by track**: 128 for Beginner and Medium, 166 for Expert. The first section's data follows right after the pointer table, so the section count = (first section's address − table address) ÷ 4. Every section ends exactly where the next begins, and the pointers increase in order.
- [Verified: player position compared with words 21-28 tick by tick over a Beginner lap] **The 8 sections the 68k picks have consecutive numbers**, starting from the player's section (or 1 to 2 behind) and increasing in the race direction, wrapping to 0 at the section count. Sections are about 943 units apart on average, so only about 6600 units ahead are drawn (a Beginner lap is about 120,000 units). Each section, with the hills and trees beside the road, has a radius of about 3000 to 3600 units.

---

## 5. SVP program structure [Verified: disassembled with `tools/ssp16dis.py`]

### 5.1 Entry point and command dispatch

- **`0400`**: waits for bit 1 of PM0 (the 68k wrote XST), reads XST and compares:
  - `4F48` ("OH") → `0411`: clears RAM0 and RAM1, initialises.
  - `5254` ("RT") → `C004`. [Speculation] A memory self-test mode.
  - Anything else → `0461`.
- **`0425-0437` is the dispatch loop**:
  1. Sets PM4 to read `30FE08`.
  2. Keeps waiting while it reads 0. GPGX detects this wait loop to save time (byte offset `0x854`, word `042A`).
  3. On non-zero, writes `30FE08` back to 0 and jumps with `ld PC, (A)` to `0438 + command number`.
- **`0438-0447`**: the command jump table; contents in section 1.4.

### 5.2 Software stack

The SSP1601's hardware stack has only 6 levels, so the program keeps its own software stack (Devon mentioned this too). The stack pointer is at `ram1:f0`, and a call is written as:

```
ld A, [ram1:f0] ; subi A, 01 ; ld [ram1:f0], A   ; decrement the stack pointer
ld r5, A ; ldi (r5), return address ; store the return address in RAM1
bra always, target ; jump
```

Below, such a call is called a "soft call".

### 5.3 IRAM overlays

The code in IRAM (words `000-3FF`) is loaded from ROM at run time.

- **Loading**: `ldi (r7|00), source ; call 0481`.
- **Overlay format in ROM**: `[PMC address][length (words)][code…]`. PMC address `8000` is IRAM word 0, `8240` is IRAM word `240`.
- **What the loader `0481` does**: gives the destination to the PMC (mode `081c`, the IRAM area of external space), then jumps into an unrolled copy loop according to the length.

Overlays used by command 2:

| ROM source | Loaded to IRAM | Length (words) |
|---|---|---|
| `15B5` | `000` | 571 |
| `126F` | `240` | 285 |
| `19B5` | `000` | 965 |
| `1EB7` | `000` | 945 |
| `229F` | `000` | 841 |

### 5.4 Command 2 flow (`088a`)

Only this top level has been read here; what each stage does is in section 6:

1. Sets ST to `0x60`. Reads `30FE0E` into `ram1:c8`.
2. Soft call `1265`.
3. Loads overlays `15B5` (to IRAM `000`) and `126F` (to IRAM `240`), then soft calls IRAM `0240`.
4. `A = 7F40`, then `call 0d4d`.
5. Soft call `17F3`, load overlay `19B5`, soft call `1957`.
6. `call 2784`.
7. Soft call `1E09`, load overlay `1EB7`, soft call `1EA7`.
8. If `ram1:c8` (that is, `30FE0E`) is not 0: soft call `17F3`, load `19B5`, soft call `1935`, soft call `1E09`, load `1EB7`, soft call `1E97`. This is the **second pass** of the in-car view, see section 6.15.
9. Soft call `2275`, load overlay `229F`, soft call IRAM `0000`.
10. `call 2794`, then back to the dispatch loop.

### 5.5 Other commands' flows [Read from the code: top level of each handler disassembled; commands 5 to 8 verified against the SVP's output (sections 6.16, 6.17)]

Every command is put together from the same components: the stage 1 matrices (overlay `15B5` plus the second overlay in the table), geometry (overlay `19B5`), rasterization (overlay `1EB7`), sprites (overlay `229F`); only the entry points differ. After `@` is the IRAM or program address of the soft call.

| Command | Stage 1 | Geometry | Rasterize | Sprites | Other |
|---|---|---|---|---|---|
| 2 race | `126F` @`0240` | `1957` (plus `1935` in the in-car view) | `1ea7` (plus `1e97`) | @`0000` | |
| 3 finish | `126F` @`0240`, then `146a` @`0240` | `1957`, then `18cd` | `1ea7`, then `1e90` when a condition holds | @`0000` | |
| A replay | `126F` @`02fc` | `1957` | `1ea7` | @`0000` | |
| B replay (fixed camera) | `126F` @`0350` | Otherwise the same as A | | | |
| 6 title | `126F` @`02fc` | `1957` | `1ea7` | @`02f7` | Reads `30FE10`, also calls `0b92`, `0b84`; stage 1 and geometry are skipped when `30FE10` = `0x60` |
| 7 "VR" picture | | | | @`0337` | Sprites only |
| 8 title (ranking) | `126F` @`02fc` | `1957` | `1ea7` | overlay `25eb` @`0000`, `229F` @`000b` | |
| 4 2P split screen | `13c9` @`0245`, `0240` (`0d4d`, `0d5b`) | `18f5`, `18f0` | `1ea7` twice | @`02e0`, @`0000` | |
| 5 SEGA logo | `138f` @`0240` | `18a3` | `1e7d` | | Reads `30FE10`; the SVP computes the model positions itself (`0f84`, `0d69`) |
| 9 mode select | `148d` @`0240` | `1868` | `1e76` | | |
| C | `1532` @`0240` | `1843`, then `18cd` | `1ea7`, then `1e90` | | |

- `126F` @`02fc` is the camera plus slot 1 handled with `01e4` (the opponents' method, world coordinates), so the player car is no longer glued in front of the camera. Replays and the title demo use it.
- The geometry (`1957`) and rasterization (`1ea7`) of replays and the title are exactly the race's, so most of HLE carries over. The SEGA logo, mode select and 2P use their own stage 1 overlays and geometry and rasterization entry points, which need separate study.

---

## 6. Command 2 data flow and output [Verified: svptrace -m plus tools/svplog.py]

Measured on the command 02 issued at frame 3500 during a race: over 36,000 SVP external memory accesses in all. Reproduction in section 8.

### 6.1 Memory read and written by each stage

Split at the IRAM overlay loads (`svplog.py phases`):

| Stage | Program addresses | Reads | Writes | Inferred work |
|---|---|---|---|---|
| 0 | `042a-0895` | `30FE08`, `30FE0E` | Clears `30FE08` | Dispatch the command, read parameters |
| 1 | IRAM and around `0d4d` | `+00` to `+0C` of each slot | `+0E` to `+24` of each slot, `30FE80-30FE8E` | Compute each slot's rotation matrix |
| 2 | `1dde-1e07` | `30FE00-30FE12` | | Read the command block settings |
| 3 | `0027-1d87` | Slot 0's matrix, ROM `138004-139E2E` (track data) | Polygon list `30C000-30CD58`, depth buckets | **Draw the track** |
| 4 | `0007-2792` | Every slot from 1 on, model data in ROM | Polygon list `30CD5A-30D254`, depth buckets | **Draw object models**, after the track |
| 5 | IRAM (overlay `1EB7`) | Polygon list, depth buckets | Tile buffer (about 15,000 writes) | **Rasterization** |
| 6 | IRAM (overlay `229F`) | Sprite slots | `30FE02` (done flag) | Handle sprites, then report done |

### 6.2 Polygon list (from `30C000`)

The geometry stage writes one record per polygon to draw, one after another:

```
word    header: the polygon's "colour byte + flag byte" copied from the model data (format in section 4)
word×2  vertex 1: x, y (signed integers)
...     3 vertices for a triangle (7 words in all), 4 for a quad (9 words in all)
```

- **Vertices are integer-pixel screen coordinates**. The origin is the centre of the 3D picture, x positive to the right, **y positive upwards**.
- **Vertices are not clipped yet**; polygons near the screen may extend thousands of pixels out. Clipping happens at rasterization.
- **No depth information remains** at this level; depth only shows in the depth buckets below.

### 6.3 Depth buckets (`310000-31FFFF`)

- 2048 buckets of `0x20` bytes. The first word is the count, followed by pointers to polygon records.
- Pointers are DRAM word offsets; for example `6000` means `300000 + 0x6000×2 = 30C000`.
- **Higher bucket numbers are farther away**. Rasterization draws from high to low bucket numbers: the painter's algorithm.
- This measurement had 264 polygons in 206 buckets, 29 of them holding more than one polygon.
- [Speculation] Each bucket holds at most 15 pointers.

### 6.4 Tile buffer format and the 3D area

- Each buffer is `0x6000` bytes, a **256×192** 4bpp picture (2 pixels per byte, high nibble on the left).
- The picture is split into 12 horizontal strips of 16 lines, each `0x800` bytes.
- Each strip has 32 columns 8 pixels wide and 64 bytes long, which are two VDP tiles stacked. Each pixel row of a column is 4 bytes.
- Conversion: the pixel at offset `off` is at
  - `y = (off / 0x800) × 16 + (off % 64) / 4`
  - `x = ((off % 0x800) / 64) × 8 + (off % 4) × 2`
- This layout fits the PMC's "special-increment" mode: even addresses add 1, odd addresses add 31, so the rasterizer can write a whole horizontal run of pixels.
- Rasterization uses both ordinary writes and "overwrite mode" writes. Overwrite mode only writes non-zero nibbles, so colour 0 is transparent.
- **The polygon list's origin is at (129, 96) in tile buffer coordinates**. On the final screen (H32, 256×224) it is at (129, 111); the 3D area starts at line 15.

### 6.5 Dither rules

| Pattern | Colour 1 (header high nibble) | Colour 2 (low nibble) |
|---|---|---|
| Lines (flag `0x20` = 0) | Pixels with even x | Pixels with odd x |
| Checkerboard (flag `0x20` = 1) | Pixels with odd x+y | Pixels with even x+y |

x and y are tile buffer coordinates. Colour 0 is transparent.

**The two uses of dither** [Verified: the polygon list of frame 3500 during a race]

| The header's two colours | Meaning | Count in that frame | Suggested enhanced handling |
|---|---|---|---|
| Same | Solid | 164 | As is |
| Different, neither 0 | An in-between colour mixed from two (the palette is not enough) | 90 | Draw the average of the two colours, solid |
| One of them 0 | Screen-door translucency, since colour 0 is not written | 10 | 50% alpha blending |

- All 10 translucent polygons are near the player car: `0162` (black plus transparent) is **the car's shadow**, `0821` and `0221` (grey and white, each plus transparent) are **wheelspin smoke** at the left and right rear ([Source: confirmed against the game picture]).
- `svplog.py preview` makes preview images by the table above. With real translucency, the shadow and smoke both look natural (confirmed in review).
- Conclusion: every "dither with colour 0" seen so far is real translucency, with no exceptions to the rule. If an exception turns up in another scene (dither used for something else), handle that model specifically. The enhanced version keeps a switch back to the original dither.
- HLE's full colour mode implements the table above (`hle.md`, the GPU drawing part).

### 6.6 Verification results

`svplog.py compare` redraws the polygon list by the rules above and compares it pixel by pixel with what the SVP actually wrote into the tile buffer:

| Item | Result |
|---|---|
| Coverage | 77 pixels drawn only by us, 380 only by the SVP, out of about 34,000 |
| Same colour among pixels both drew | **96.70%** (32,586 / 33,697) |

Only two kinds of differences remain: single-pixel lines at polygon edges (details of the edge rasterization rules differ), and small distant objects (drawing order within a bucket differs).

### 6.7 What this means for the HLE design

- The polygon list is in **integer-pixel** screen coordinates, with off-screen objects already culled. Redrawing at this level at high resolution smooths edges and can remove the dither (see `svplog.py preview`), but vertex precision stays at 256×192, so the picture jitters when it moves, and widescreen is impossible.
- So the HLE renderer has to **do the transforms itself from the 3D data**: the model slots plus the models and track data in ROM.
- The polygon list serves as **the reference answer for verification**: the vertices we transform and project must match it at native resolution.

### 6.8 Object transform and projection [Verified: bit-exact comparison, `svplog.py objects`]

The 72 object polygons of stage 4, recomputed with the integer arithmetic below, match the SVP's polygon list **exactly in every vertex (72/72)**.

**1. Model vertices to camera space** (`0096` in overlay `19B5`). For each row:

```
A = 2·(m0·x + m1·y + m2·z + t·0x400)    32-bit accumulation, wraps on overflow
camera coordinate = high 16 bits of (A << 3)   ≈ (m·v + 1024·t) / 4096
```

Here `m` and `t` are the 12 words from the slot's `+0E`, and `x, y, z` are the signed 16-bit coordinates of a model vertex in ROM. In real numbers, `camera coordinate ≈ R·v/32 + t/4` (R with `0x80` = 1.0).

**2. Perspective projection** (`00b8`). The SSP1601 has no divide instruction, so it uses a reciprocal table:

- The **reciprocal table** is at program memory word `0x5845` (ROM byte `0xB08A`): `R(n) = 32768 / n`, 8192 entries (`R(0)` and `R(1)` are both `0x7FFF`). Its base address is stored in `ram1:f3` at initialisation (`041c`).
- `z' = z − ram1:10` (from `30FE12`).
- **Near** (`z' < threshold`):
  ```
  P = 2·R(z')·y;  A = P << 4;  screen y = high part of ((A << 2) + (high part of A << 16))
  screen x = high part of ((2·R(z')·x) << 6)
  ```
- **Far** (`z' ≥ threshold`): looks up `R(z' >> 5)` instead, with shifts adjusted. Same scale, better precision.
  ```
  P = 2·R·y;  screen y = high part of ((P >> 1) + (high part of (P << 1) << 16))
  screen x = high part of ((2·R·x) << 1)
  ```
- In real numbers: **screen x ≈ 64·x/z', screen y ≈ 80·y/z'**. Focal lengths fx = 64 and fy = 80, a pixel aspect of 1.25 (H32 pixels are not square).
- The threshold is in `ram1:1d`, **value `0x400`**. Object data alone only narrows it to `0x100` to `0x800`; the track data pins it to `0x3FC` to `0x404` (section 6.12).
- The origin of screen coordinates is the centre of the 3D picture, y positive upwards (section 6.2).

**3. Near clipping and off-screen tests** (`01c5`): each vertex is checked for `|x|` above `ram1:12`, z above `ram1:11`, and z below `ram0:f4` (the near plane).
- [Verified: disassembly of `1935-19a6`, in-car view compared with `vrr -d`] **The near plane depends on what is drawn**. Before each group, `ram0:f4` is set to the value below plus `ram1:10` (from `30FE12`, 0 in races): 16 for the track, 2 for the player car and slots 2-10, 6 for opponents and slot 56, 16 for the second pass of the in-car view (in 1/32 units, see section 6.15). An earlier note of "`8 + ram1:10`" was wrong.
- [Read from the code, checked against the polygon list of Beginner frame 3500] `0048-004c`: if any vertex's `|x|` or z exceeds its limit (`ram0:ef` not 0), the whole polygon is not drawn, even when part of it is on screen (for example a piece far away at the left edge with `|x|` above the track's limit `0x1000`). The limits are in `ram1:12` (`|x|`) and `ram1:11` (z); `17f3` sets them to `0x1000` and `0x1fff`, and each command's geometry entry point changes them (`0x800` for race objects).
- [Verified: polygon list of frame 114 of the SEGA logo] The backface cross product (`01a5`) uses projected **integer** screen coordinates, so small polygons with zero area are dropped.
- [Read from the code] Polygons cut by the near plane get their intersections computed by `01f0-0336` and `0338`. The intersection's z is set directly to `ram0:f4` (`0351`); a quad may be cut into a quad and a triangle, each getting its own depth key before output. So the depth key is computed **after** clipping.

**4. Self-modifying code**: `1dbe` writes `0x96` (150) into the immediate fields of a few instructions in the current IRAM overlay (IRAM `03d`, `03f`, `041`, `047`). [Speculation] Some distance parameter.

### 6.9 Camera matrix [Verified: fitted with the SVP's sine table, maximum error 0.00006]

```
camera matrix = ( Ry(angle Y) · Rx(angle X) · Rz(angle Z) )ᵀ
```

- The angles come from `+08`/`+0A`/`+0C` of slot 0, with `0x1000` for 360°.
- The code (`010c`) negates the angles and rotates in the order Z → X → Y, with the same result.
- The **sine table** is at program memory word `0x2845` (ROM byte `0x508A`): `sin(i·2π/4096)·0x4000`; `cos` looks up the same table at index plus `0x400`. Its base is in `ram1:f2`.
- [Read from the code, not verified] Slot 0's position `(X, Y, −Z)` and `0x20` are also multiplied by this matrix, with results in `ram1:c0-c2`, as the origin offset for track vertices (`0074` subtracts it first).

### 6.10 Depth key and Z-sort flags [Verified: `svplog.py buckets`, 492/493 over two moments]

This cracks the "Z-sort flags" Devon did not.

**Mechanism** (`00f0-0126` in overlay `19B5`, disassembled):

- The low 3 bits of the polygon flags are an index into the jump table `RAM0[0x38 + index]` (`0160`), choosing how to compute the depth key.
- If the key exceeds `ram1:11`, the polygon is dropped.
- If the bucket's count has reached `ram1:1a` (`0x80` in races), it is dropped too.
- Otherwise the record's pointer is appended to the bucket, and the header and vertices are written.

**Methods**: z is the camera-space depth of each vertex of the polygon, and the result is the bucket number.

| Flag low 3 bits | Bucket | Use |
|---|---|---|
| 0 | `average(z) >> 2` | Ordinary polygons |
| 1 | `min(z) >> 2` | By the nearest vertex, tends to sort to the front |
| 2 | `max(z) >> 2` | By the farthest vertex, tends to sort to the back |
| 3 | `(average(z) >> 2) + 512` | Pushed back, drawn further behind than it really is |
| 4 | `(max(z) >> 2) + 256` | Pushed back |
| 5 | `max(z) >> 2` | Same as 2 in all data seen so far |
| 6, 7 | Not seen yet | |

- For triangles, the average matches with integer division by 3 (the SSP1601 has no divide instruction; how it actually does it has not been read).
- [Verified: data from `svplog.py`, 21 near-clipped track polygons at frame 4400: 18 match, the other 3 are mismatched pairings] **For near-clipped polygons, the z of vertices behind the near plane counts as 16 in the depth key**, not their original negative values. HLE first used the raw z, got negative values clamped to 0, and those polygons were drawn last, covering things in front of them (such as grass thrown up by the player car). [Speculation] The SVP places the new vertices from clipping around z = 16; working back from the projected positions gives the same divisor. [Verified: disassembly] 16 is the track's near plane (section 6.8), and the intersection's z is set to the near plane.
- The single mismatch is, judging by its depth, a mismatched pairing.
- "Pushed back" makes polygons such as the road surface always lie under the cars and roadside objects. If HLE ever used the GPU's depth buffer, these flags would be the basis for reproducing the original's sorting intent.

### 6.11 Parameter blocks

- `ram1:10-1f` and `ram0:f0-ff` are per-stage parameters, set again before each stage (such as `17F3`, `1843-18b9`). For example:
  - `ram1:12`: upper limit of x (`4000`, `1000`, `7fff`, `0800`, by stage).
  - `ram1:11 = 1fff`, `ram1:16 = ffe0`, `ram1:1a = 0080`.
- `1250-125e` is a generic routine copying a data table from program memory to RAM1.

### 6.12 Track transform [Verified: `svplog.py track`, 172 bit-exact]

- **The camera's world position** (`0125-0148` in overlay `15B5`):
  ```
  c = player car position (+02-06 of slot 1) + floor( Mᵀ · (X₀, Y₀, −Z₀) / 16384 )
  ```
  where M is the camera matrix (2.14) and `(X₀, Y₀, Z₀)` is slot 0's offset; taking the high bits is a floor.
- **Track vertices to camera space** (`0074` in overlay `19B5`):
  ```
  A = 2·Σ m_k·(v_k − c_k) + 2·m3·0x20      m3 is each row's 4th word, 0 for the camera
  camera coordinate = high 16 bits of (A << 1)   ≈ M·(v − c) / 16384
  ```
  The track has no extra scale; world and camera coordinates have the same units. Objects are `R·v/32 + t/4` (section 6.8).
- Projection is exactly as for objects (section 6.8). **The near/far threshold is `0x400`**: the track matches best only between `0x3FC` and `0x404`.
- Results (frame 3500 during a race):
  - **172 polygons bit-exact**.
  - 20 have vertices near or behind the near plane and were clipped by the SVP, so they are left out. Clipping turns quads into triangles (flag plus `0x10`) and computes intersections on the near plane.
  - 1 mismatch: a triangle from clipping that happens to share its header with another model polygon, a mismatched pairing.
- The near clipping threshold looks a bit larger than `ram0:f4` (= 8), because vertices at z = 9 were clipped too. The exact value is not confirmed yet.

### 6.13 How model slot matrices are computed [Verified: `svplog.py matrices`, 40 slots at two moments bit-identical]

Stage 1 (driven by overlay `126F`, with routines from overlay `15B5`) computes a 4×3 matrix for each slot: 3 rotation rows plus 1 translation row, written back to the slot's `+0E-24`.

**Routines**: each acts on the result of the previous step, so the call order is the order in which they apply to a vertex.

| Routine | Action |
|---|---|
| `00b7` / `00bb` / `00bf` | Build an identity matrix with diagonal `0x4000`, `0x1000` or `0x80` (2.14 for the camera, 1.7 for objects) |
| `00a8` | Look up sin and cos (2.14) in the sine table, mask `ram1:20 = 0x0FFF`, cos offset `ram1:21 = 0x0400` |
| `0067` / `007c` / `0093` | Rotate each of the 4 rows about X / Y / Z, each time taking the high 16 bits of `((2ab + 2cd) << 1)`, so **every step truncates** |
| `001c` | Add `4 × offset` to the translation row |
| `00e0` | Write back to DRAM: group k of 4 words is `(row0[k], row1[k], row2[k], translation[k])` |

Rotation of each row (r is a row):

```
X: (r0, r1·cos + r2·sin, −r1·sin + r2·cos)
Y: (r0·cos − r2·sin, r1, r0·sin + r2·cos)
Z: (r0·cos + r1·sin, −r0·sin + r1·cos, r2)
```

**Order for each kind of slot** (`a`* are angles, `P` position, `c` the camera's world position, section 6.12):

| Slot | Order (from the vertex) |
|---|---|
| 1 player car, 6-10 effects following the car (`015a`) | Z(a_car_Z + a_cam_Z) → X(a_car_X + a_cam_X) → Y(a_car_Y + a_cam_Y) → translate (0, −camera offset Y, camera offset Z) |
| 2-4 player car sub-objects (`017a`) | X(a_sub_X) → Y(a_sub_Y) → translate P_sub → then slot 1's steps |
| Opponent body (`01e4`) | Z(aZ) → X(aX) → Y(aY) → translate (P − c) → Y(a_cam_Y) → X(a_cam_X) → Z(a_cam_Z) |
| Opponent sub-objects (`020b`) | X(a_sub_X) → Y(a_sub_Y) → translate P_sub → then the parent's steps |

- Sub-objects only have X and Y angles; wheel rotation uses X.
- The player car uses an "**added angles**" shortcut: its angles are exactly the negatives of the camera's, so the sum is 0, the matrix is identity, and the car always appears in the same pose at the same place on screen (chase view).
- Opponents use standard 3D: `camera coordinate = M · (R_object·v + (P − c))`.
- Objects use 1.7 precision, and 6 consecutive rotations accumulate rounding errors (up to about 0.03 from the ideal floating-point result). Computing in floating point, HLE would be more accurate than the original but could differ by ±1 pixel at native resolution. For bit-exact results, use the emulation here (`MatrixOps` in `tools/svplog.py`).

### 6.14 Second verification point [Verified]

To confirm the formulas are general, the same checks were run at frame 4400 during a race (passing the C.P. checkpoint, in a bend, 222 km/h, drawing sections 25 to 32):

| Check | Frame 3500 | Frame 4400 |
|---|---|---|
| Slot matrices (`matrices`) | 23/23 | 17/17 |
| Object polygons (`objects`) | 72/72 | 91/91 |
| Track polygons (`track`) | 172 match, 20 clipped, 1 mismatched pairing | 165 match, 22 clipped, 0 mismatches |
| Depth buckets (`buckets`) | 243/244 | 249/249 |
| Redraw versus SVP output pixels (`compare`) | 96.70% | 96.28% |


### 6.15 In-car view (`30FE0E`) [Verified: disassembly of overlays `126F`, `15B5` and `1935-19a6`, then compared with the SVP's output using `vrr -d`]

When X on a six-button pad switches to the in-car view during a race and the camera offset becomes (0, 20, 22), the 68k sets `30FE0E` in the command block to 1 and swaps slot 1 for the in-car model (on Beginner from `08084CE5` to `08084179`). The driver's seat is only a few units from the camera, so the SVP draws this car with **higher precision in a separate pass**:

- **Stage 1** (`ram1:c8` is `30FE0E`):
  - Slot 1 (the `0194` branch of `015a`): identity with `0x1000` (`00bb`), 32 times the usual `0x80`. Rotated about Z only (car Z angle plus camera Z angle; the X and Y angles cancel out anyway), then translated by (0, −camera offset Y, camera offset Z). The translation is `2 × 0x40 × value` (low 16 bits), added directly (routine `0000`), not multiplied by 4 as in `001c`. So camera coordinates are in 1/32 world units.
  - Slots 2 and 3 (the `01ac` branch of `017a`): also `0x1000`; after the X and Y rotations, translated by 128 × position, then by the camera offset, without the car's Z rotation. Slot 4 is not processed (the loop covers only slots 2 and 3).
  - Slot 5 (`01ca`, processed only now): `0x1000`, rotated by its own Z angle (word 6), translated by 128 × position (words 1-3), then the car's Z rotation and the camera offset.
- **First pass** (`1957`): skips slots 1-10 (`197d`), drawing only the track, opponents and slot 56.
- **Second pass** (`1935`): the polygon list restarts at `30C000`, drawing only slots 1-5. `ram1:10` is multiplied by 32, the near plane is 16 (in 1/32 units, 0.5 world units), and the `|x|` limit is `0x1000`. Then `1E97` rasterizes into the same tile buffer, so the whole car covers the first pass.
- Slots 6-10 (tyre smoke and so on) are drawn in neither pass in the in-car view.
- HLE originally treated slot 1 in the usual `0x80` units with a near plane of 8, cutting away almost the whole driver's seat (only about 65% match between HLE and the SVP in the in-car view). Drawn as above, the in-car view averages 94.2% to 95.1% over whole races on all three courses (the same as the chase view).


### 6.16 SEGA logo (command 5) [Verified: disassembly of `0ee5-10e7`, `0d69-0ee4`, overlays `138f`, `18a3`, `1e7d`, then compared entry by entry with the slots and polygon lists the SVP writes back]

- **Timing**: from frame 34 to frame 350 after power-on, every 4 frames. `30FE10` is the time: 0 for the first two, then `0x35` more each time, `0x1000` (= 1.0) for the last. It appears once more after a race ends, before returning to the title.
- **Slots**: slots 0-7 are the 8 letter pieces (S, E, G, A, and mirrored ones); the 68k only sets word 0 (visible) and the model pointers. The SVP computes positions and angles at the start of the command (`0f84`) and writes them back into the slots, so at the moment a command is issued the slots hold the previous values.
- **Position** (`0d69` → `0e55` → `0e8e`): a cubic curve. t = `30FE10`, u = `0x1000` − t (4.12 fixed point).
  - Weights: w0 = k0·u³, w1 = k1·u³ + k2·u², w2 = k5·u³ + k6·u² + k7·u, w3 = k9·t³. The k are the operands of the `ldi` run at `0e56-0e68` (k0 = 1/6, k1 ≈ −0.9165, k2 = 1.5, k5 = 1.75, k6 = −4.5, k7 = 3, k9 = 1; at t = 0 the weights sum to 1, at t = 1 only w3 remains).
  - Position = Σ wᵢ · Pᵢ. The control points P0-P3 are 12 words from program memory `0x1025` (slots 2-5) and `0x1034` (slots 0, 1, 6, 7).
  - Each product is the SSP's `2·x·y`; the sum is shifted left by 3 and the high 16 bits taken.
- **Write-back** (`0f84-101f`): each curve's (x, y, z) goes to 4 slots: as is, mirrored in y, mirrored in x, mirrored in both. Word 4 (X angle) of slots 2-5 is t × 4; word 5 (Y angle) of slots 0, 1, 6, 7 is t × 4 with word 4 written as 0; the two pieces mirrored in y also negate their angles.
  - **The accumulator's low part**: mirroring and angles load the value into the high part of the 32-bit accumulator (the low part keeps the remainder of earlier computations) and then `neg` or `shl`, so a mirrored value is 1 less when the low part is not 0 (for example y = 692 mirrors to −693), and the lowest 2 bits of angles are shifted in from the low part. For bit-identical results the accumulator must be emulated in order.
  - With this model, the 8 slots of all 80 commands (632 entries) are identical to what the SVP actually writes back.
- **Matrix** (`025e` in overlay `138f`, stage 1): no camera. Identity with `0x400`, then Y (word 5), X (word 4), Z (word 6) rotations, then the position added directly (routine `0000`). Camera coordinate = (R·v + position) / 4.
- **Geometry** (`18a3`): near plane (`ram0:f4`) 2, `|x|` limit `0x7fff`, z limit `0x1fff`, `ram1:10` 0. The depth key mask (`ram1:16`) is `0xffe0`, so buckets are `key >> 5`, 256 of them, at most 128 entries each (`ram1:1a`), rasterized by `1e7d` (from `31FF00` downwards, `0x80` words at a time).
- **Order within a bucket**: the order of creation. At frame 114 the SVP's list is identical to the one computed by these rules (164 polygons, same order). Rasterization zeroes each bucket's count as it processes it, so when reading lists from a `-m` log, use the largest count written to each bucket.

### 6.17 Title screen (commands 6, 7, 8) [Verified: disassembly of `0aee-0bc8`, overlay `126F` @`02fc`, `02f7`, `0337`, `01d2`, `0215` of `229F`, overlay `25eb`, then compared frame by frame with the SVP's output using `vrr -d 3500 -t 690`]

- **Attract order** (no input, `svptrace -n 12000`): command 6 116 times → command 8 381 times → command 6 → commands 6 and 7 alternating every two → command 6 → back to the SEGA logo (commands 5, 1), then repeat.
- **Stage 1** (`126F` @`02fc`, commands 6 and 8, also replay A): the camera (slot 0, `00cf`, `010c`, `014a`) is as in races; slots 1-10 differ:
  - Slot 1 uses `01e4` (the opponents' method: own rotation, then placed at its own world position), not `015a` (glued in front of the camera).
  - Slots 2-4 use `020b` (the opponents' sub-object method), hanging off slot 1. Races use `017a`.
  - Slots 6-10 go through `01e4` when word 0 is not 0 (races use `015a`), but **with the car's transform**, as in races. [Verified: svptrace of the title demo: their words 1-6 are all 0, and the matrix the SVP writes back into words 7-18 equals slot 1's; drawn this way by HLE, the tyre smoke appears where the SVP draws it] An earlier reading, "at their own world positions", put the smoke at the world origin, and HLE drew no smoke in the title demo and the replays.
  - Slot 5 is not processed. Slot 11 on (from `02a8`) and slot 56 are as in races.
  - Command B (the fixed-camera replay) is @`0350`: words 1-6 of slot 0 are taken directly as the camera's position and angles (`ram1:c0-c5`), skipping `00cf` and `010c`, then joining the @`02fc` flow at `0308`.
- **`30FE10` of command 6** (stored in `ram1:e0`): normally 0. At the end of the title demo it goes 5, C, 15, 20, 2D, 3C, 4D, 60.
  - `0b92`: `ram1:e2` (lines to rasterize) = `0xc0` − 2 × ((e0 − 1) & `0xf0`). `0b84`: `ram0:f6` (the rasterization start) moves down (e0 − 1) & `0xf0` lines. So the 3D only draws the middle 16-line strips, without moving (pixel comparison confirms no vertical shift).
  - Sprites (`229F` @`02f7`): a 256-wide, e0-line picture band is drawn at the top and at the bottom, in non-overwrite mode (PMC mode `4018`). The top band's data starts at program memory word `0x7E61E − 64 × e0` and is drawn at line 0; the bottom band starts at `0x7E620` and is drawn at line `0xc0 − e0`. The whole picture is the 256×192 title picture ("V.R. Virtua Racing"; the first 96 lines before `0x7E61E`, the last 96 after `0x7E620`, with the two words between being the second half's width and height). The picture has no colour 0, so non-overwrite mode makes no difference.
  - At e0 = `0x60` stage 1 and geometry are skipped, and only the whole picture is drawn.
- **Command 7** (`229F` @`0337`): does not call the rasterization setup (`1e09`); only `01d2` decompresses the "V.R." picture from program memory word `0x7FE22` to position 0, 192 lines, in non-overwrite mode.
  - The format is RLE: each row is a number of "count, one word" pairs, the word written that many times; count 0 ends the row. `0x7FE20-21` are width and height (`003f`, `00bf`). Every row is exactly 64 words (256 pixels), with no colour 0, so it covers the whole buffer.
- **The ranking of command 8** (overlay `25eb` @`0000`, followed by `229F` @`000b` drawing slots 6 to 31):
  - Slots 0-5 are one row each, drawn at x = 32, y = 48 + 16 × slot (position from `0x0c80`, plus `0x400` per row).
  - `002e` first assembles 23 ASCII characters from RAM0 `0x10`:
    - Position (word 1): two digits via the BCD table at `0x26F3`, a tens digit of 0 becoming a space; then the two characters at program memory `0x2730 + 2 × position` ("st", "nd" and so on), then a space.
    - Name: the high and low bytes of word 2, plus the high byte of word 3, three characters; then a space.
    - Time: the low 4 bits of word 4 are minutes, followed by `'`; word 5 is seconds (BCD), followed by `"`; word 6 is hundredths (BCD); then a space.
    - V.R. mode: the 6 characters at program memory `0x2764 + 8 × word 7` (the small icon in the ranking).
  - `008a` then draws them character by character, skipping spaces: the font is at program memory `0x782D0 + 0x20 × (character − 0x20)`, 8×16, PMC mode `0c18` (increment by 1, overwrite).
  - Word 0 is how many characters to show, so a row types out one character at a time; when word 0 is negative −n, the first n characters are skipped and only the rest drawn (erasing from the left); 0 draws nothing.

### 6.18 Finish and time up (command 3) [Verified: disassembly of `0959-0a39`, overlays `146a`, `18cd`, then compared frame by frame with the SVP's output using `vrr -d 8900 -t 8500` (Medium time up)]

- **Flow**: the same stage 1 (`126F` @`0240`, also reading `30FE0E`) and geometry (`1957`) as command 2, so the track, cars and camera are drawn as in a race; then an extra pass draws only slot 57, on top. There is no second pass for the in-car view: in the in-car view slots 1-10 are not drawn at all. Sprites are as in races (`229F` @`0000`).
- **Slot 57's matrix** (overlay `146a`, loaded to IRAM `0240`): no camera. Identity with `0x1000` (`00bb`; as in the in-car view, camera coordinates are in 1/32 units), X rotation by word 4 (`0067`), then (0, word 2, word 3) added directly to the translation (`0000`).
- **Geometry** (`18cd`): the polygon list restarts at `30C000`, near plane (`ram0:f4`) 2, `|x|` limit `0x1000`, other parameters the race defaults set by `17F3`. Only slot 57 is processed (when word 0 is not 0). `ram1:e3` is cleared first; when it is not 0, `1e90` draws this list over the race picture.
- **The time up screen**: "GAMEOVER" is slot 57's 3D text; "RESULT 16th", TOTAL TIME and so on are sprites. The camera does not move then (words 1-6 of slot 0 do not change from tick to tick).
- **The finish**: the camera slowly circles the car, about 32 per tick (4096 per turn), with the offset changing by 2. [Verified: test input sets the lap count (`FF902B`, `FF902D`, completed laps counting from 0) to the last lap at frame 3100, so the first crossing of the line is the finish. Command 3 starts at frame 8200 and is still going at frame 14000.]

### 6.19 Replay (commands A, B) [Read from the code: disassembly of `0a3a-0aed`, `010c`, `014a` of overlay `15B5`, `02fc`, `0350` of `126F`; not yet compared with the SVP's output]

- **A** (`0a3a`): clears `ram1:c8` to 0; stage 1 uses `126F` @`02fc` (as the title demo: the player car at its own world position, slots 2-4 hanging off slot 1, slots 6-10 with the car's transform, see section 6.17), followed by the race's geometry (`1957`), rasterization (`1ea7`) and sprites (`229F` @`0000`).
- **B** (`0ac1`): leaves `ram1:c8` alone; stage 1 enters `126F` at @`0350`: words 1-6 of slot 0 are read directly into `ram1:c0-c5`, skipping `00cf` and `010c`, joining the @`02fc` flow at `0308` (`call 014a`); the rest is as A.
- **`ram1:c0-c5` is the camera**: `010c` computes `c0-c2` = the camera's world position (slot 1's position plus the rotated offset), and `c3-c5` = slot 0's X, Y, Z angles (words 4-6); `014a` builds the camera rotation matrix from `c3-c5`. So in B, words 1-3 of slot 0 are the camera's own world position, not an offset from the car: a camera standing beside the track.
- The replay only follows a top-three finish; no input reaches it automatically yet, so the picture is checked in play.

---

## 7. Open questions

None of these affect the basic HLE renderer, but they need answering when the related features are built:

- ~~How the 68k picks the 8 track sections to draw~~: solved, see section 4.1.
- The difference between depth flags 5 and 2, and the use of 6 and 7.
- ~~The special handling of slot 5 (`01ca`), and the second pass turned on by `30FE0E`~~: solved, see section 6.15. Slot 56 is in section 2.
- **The arithmetic of the new vertices from near clipping** (`01f0-02d4`, `0338`). Polygons clipped by the SVP are smaller than the geometrically correct shape, so large polygons close to the camera (kerbs, pit walls) are a bit short in the original. HLE leaves clipping to the GPU, with different shapes; this is now the most visible difference from the original (see `hle.md`). The depth key part is solved (section 6.10).
- Details of the edge rasterization rules, and the drawing order within a bucket (these only affect pixel comparisons with the original).
- Details of sprite types 2 (text) and 5 (compressed format) (not used in races, see section 3).
- What the screen mirror flag `30FE00` actually does.
- The meaning of `30FE04` (section 1.3; verified only once so far).
- How the other commands (3, 4, 9 to C) differ from command 2. 5 to 8 are solved (sections 6.16, 6.17).

---

## 8. Tools and reproduction

- **`build/svptrace`** (source `src/svptrace.c`): replays input from power-on and records the command block, model slots and sprite slots at every command. Usage in the comment at the top of the file.
  - `-w frame`: writes a screenshot and prints the current palette (`PAL` lines).
  - `-m frame`: records every SVP external memory access during the first command after that frame (`P` lines, `O` for overwrite-mode writes). This uses `vrr_ssp_pm_hook` in GPGX.
- **`tools/ssp16dis.py`**: SSP1601 disassembler that decodes PMC settings into 68k addresses.
  ```sh
  python3 tools/ssp16dis.py "rom/Virtua Racing (USA).md" 0425 0448         # code in a ROM range
  python3 tools/ssp16dis.py "rom/Virtua Racing (USA).md" --overlay 126f    # an overlay, shown at IRAM addresses
  python3 tools/ssp16dis.py --test                                         # self-test
  ```
- **`tools/svplog.py`**: analyses `svptrace -m` logs. Subcommands `phases`, `polys`, `compare`, `preview`, and four bit-exact checks: `matrices` (section 6.13), `objects` (section 6.8), `track` (section 6.12), `buckets` (section 6.10).
- **Reproducing the measurements of section 6** (the command at frame 3500 of a race):
  ```sh
  RACE="-p 1100:S -p 1106: -p 1400:S -p 1406: -p 1700:S -p 1706: -p 2100:C -p 2106: -p 2700:B"
  build/svptrace -n 3512 -f 3500 -m 3500 -w 3510 $=RACE "rom/Virtua Racing (USA).md" > out/race3500.txt   # zsh; in bash use $RACE
  python3 tools/svplog.py phases  out/race3500.txt
  python3 tools/svplog.py compare out/race3500.txt out/diff.ppm     # expect 96.70%
  python3 tools/svplog.py preview out/race3500.txt 3510 out         # 4× preview
  python3 tools/svplog.py objects out/race3500.txt "rom/Virtua Racing (USA).md"   # expect 72 bit-exact
  python3 tools/svplog.py track   out/race3500.txt "rom/Virtua Racing (USA).md"   # expect 172 bit-exact, 20 clipped, 1 differ
  python3 tools/svplog.py matrices out/race3500.txt "rom/Virtua Racing (USA).md"  # expect 23/23
  python3 tools/svplog.py buckets out/race3500.txt "rom/Virtua Racing (USA).md"   # expect a single mismatch, flag 0
  # Second verification point (section 6.14): replace 3500 with 4400, -n 4412, -w 4410
  ```
- Disassemblies, traces and previews are all generated from the ROM and stay local (`out/` is in `.gitignore`); they must not be committed. This document records only addresses and our own descriptions.

# Game loop: logic ticks and frames


Raising the FPS is based on this document. Labels are the same as in `svp-commands.md`: [Verified], [Source], [Speculation].
Only the race screen of the Virtua Racing mode (command 2) has been analysed so far.

## Summary

- **The game logic runs in fixed ticks of at least 4 frames each** (15 Hz). Car physics, AI and the camera are computed once per tick.
- **When the SVP cannot finish in time, a tick stretches to 5 frames or more**, and the game logic stands still meanwhile; the extra time is not made up. So in the original, when the SVP is overloaded, the game itself really slows down.
- **The race timers (the TIME countdown, lap times) count every frame**, independently of ticks. So time keeps passing while the game slows down.
- What this means for raising the FPS:
  - Ticks cannot simply run faster. The logic would speed up while the timers still count 1 second per 60 frames, which changes the game's balance.
  - So the logic keeps its original ticks, and the picture reaches 60 FPS and more by **interpolation**. HLE has the full camera and object matrices with every command 2 and can interpolate between two neighbouring ticks.

## Interrupts [Verified: disassembly, capstone 5]

The ROM's vector table: reset `020000`, H-INT (level 4) `020D7E`, V-INT (level 6) `020D3A`.

- **V-INT `020D3A`**:
  - Only does something when `$D8DA` is not 0: calls a subroutine from the table at `020D6E` by its value, clears `$D8DA` to 0 and adds 1 to `$DAC4`.
  - Always resets the H-INT counter `$D864` to 27.
  - During a race `$D8DA` is always 0 and `$DAC4` does not change.
- **H-INT `020D7E`**:
  - Subtracts 1 from `$D864` each time; at 0 it resets it to 27. If `$D865` is then not 0, it turns the display off, runs the DMA routine in RAM at `$FF0018`, turns the display back on and clears `$D865` to 0.
  - This is how the 3D tiles are DMAed into VRAM in parts (`svp-commands.md` section 1.3).
  - [Verified: PC sampling] During a race this happens once per frame in the bottom border (after line 207). In the frames of phases 0 and 2, lines 5 and 6 (top border) are also inside the interrupt handler. [Speculation] This is a second part scheduled by the DMA routine itself; the mechanism has not been analysed.

## Main loop [Verified: disassembly]

The main loop is a small routine copied to RAM at `$FF0000` and run there. There are two kinds:

| Source | Content | Copied entry | Used for |
|---|---|---|---|
| `0203B0` | `jsr target` → `$D8DA = 4` → wait for V-INT to clear it → repeat | `0203DC` | Screens such as menus ([Speculation] not checked one by one) |
| `0203C8` | `jsr target` → set `$D865` to 1 → wait for H-INT to finish the DMA and clear it → repeat | `0203EC` | **Races** |

- The game rewrites `$FF0002` (the `jsr` target) to choose what to do in this round. The V-INT wait subroutine `02457C` is also called directly from a dozen or so places.
- [Verified: 68k PC sampled on every scanline] A race runs the second kind with target `0248AC`, and **exactly one round per frame**:
  - Each round starts at line 208 (after the DMA in the bottom border).
  - `0248AC` first idles until `$D864` becomes 26 (the first H-INT of the next frame), then does its work.
  - When done it returns to the loop in RAM and waits for this frame's bottom DMA.

## The race state machine `0248AC` [Verified: disassembly, runtime logging]

`$D8DE` is the phase number times 4; the table at `0248BE` jumps to each phase. One phase runs per frame:

| Phase | Code | What it does | Next phase |
|---|---|---|---|
| 0 | `0248D6` | Writes `30FE06 = 1` (the send flag) and so on | 1 |
| 1 | `024908` | Part of the game logic | 2 |
| 2 | `02495E` | Part of the game logic | 3 |
| 3 | `0249A4` | Part of the game logic, writes the model slots and sprite slots, records this tick's pad input (for the replay, 1 byte), adds 1 to `$D90C` (tick counter; [Speculation] used to time events, compared against constants around `0239C6`). Finally, if the SVP has finished (`30FE02` not 0), clears the done flag and issues the next command | Back to 0 if the SVP has finished, else 4 |
| 4 | `024A62` | **Waits for the SVP**: only does the per-frame work, then checks the done flag and issues the command once it is done | Back to 0 when done, else stays in 4 |

- **Per-frame work** (called in every phase):
  - `0211BC`: talks to the Z80 sound driver.
  - `02B2BE`: the race clock. Counts frames and seconds in `$D866-$D868` and subtracts 1 from TIME (`$D054`, word) every 60 frames.
- **`$D8E6`**: the frames each tick took (set to 1 in phase 0, then incremented each frame depending on whether the SVP has finished). Only `02182C` reads it, and only writes it to VRAM for display when the debug flag `$D86D` is 0. So it is a development debug display; **the physics do not use it**.
- [Verified: runtime logging] Normally command 02 is issued around line 153 of the phase 3 frame. How long each phase works on screen (Beginner, from frame 3000):
  - Phase 3: until line 154.
  - Phase 0: until line 120.
  - Phase 1: until line 33.
  - Phase 2: until line 186.
  - The rest of the time is spent waiting for the DMA.
- Other screens have similar state machines (other routines that set `$D8E6` to 1 and increment it are `024D6C`, `0251A0`, `025538`, `0256C4`, `030448`, `033756`); [Speculation] these are modes such as 2P and replay, not analysed yet.

## Measured: what is computed once per tick [Verified: 68k RAM saved and compared every frame]

Method: save the whole 68k RAM (64 KB) at the end of every frame during a race, and see in which phases each word changes.

**Beginner**, frames 3000 to 3400, no dropped frames:

| Changes in | Words | Main RAM ranges |
|---|---|---|
| Phase 1 only | 485 | `8100-89FF` |
| Phase 3 only | 326 | `9000-90FF`, `9C00-A0FF`, `DB00-DBFF` |
| Phase 2 only | 213 | `9100-9BFF` |
| Every phase | 11 | Counters and the like |

So the game logic is spread over phases 1, 2 and 3, each computed once per tick.

**Medium start**, frames 2470 to 3400, with the SVP overloaded:

- 90 of the 931 frames are phase 4 (waiting for the SVP). For a while after the start, every tick takes 1 more frame (5 frames per tick).
- In Beginner, 274 words change only in phases 1 to 3 and more than 20 times. Of the 266 of them that also change in Medium, only 2 (`$80AC`, `$80B4`) change in phase 4. So **the game logic stands still during dropped frames**.
- What changes in both phases 3 and 4 is mostly the block `$8484-$84FF` ([Speculation] HUD or sound data updated every frame).

**Timers** (Medium start, same span):

| Variable | Content | Interval between changes |
|---|---|---|
| `$D055` | TIME (seconds left) | Always exactly 60 frames, whether those 60 frames hold 12, 13 or 15 ticks |
| `$D867` | Clock seconds | Same |
| `$80A5` | Another count in seconds, purpose unconfirmed (counts 15 to 23, then becomes 14) | 57 to 63 frames, 60 on average. [Speculation] The value is computed per frame but only updated per tick, so the interval varies by a few frames |

## Pad input, steering and the replay record [Verified: disassembly, capstone 5; the steering ramp compared against front wheel angles with `svptrace`]

- **Reading the pad**: `020F28` reads a 3- or 6-button pad, and `020EB8` stores the raw bits in `$D8CC` (held) and `$D8CD` (newly pressed this frame). They are rearranged into the game's action bits through the 8-byte button setup table at `$FDEE` ([Speculation] the control setup in Options) and stored in `$DAE4` (held) and `$DAE6` (newly pressed). Left and right (bits 2, 3) are passed through as they are, not through the table.
- **Replay record**:
  - Phase 3 (`0249E0`) stores 1 byte per tick into the buffer from `$DB00` (pointer `$D922`, up to `$FA00`, about 7900 ticks). The value is `($DAE5 & 0x5C) | ($DAE7 & 0x03)`: held left/right (bits 2, 3), bit 4 (throttle, see below), bit 6, plus newly pressed bits 0 and 1.
  - On replay (`02577A`) these bytes are written back into `$DAE5` and `$DAE7` and **the whole race is simulated again**, so the replay is decided entirely by this 1 byte of digital input.
- **Steering** (`029622`, called once per tick by every race state, 10 call sites in all; a0 is the player car's structure):
  - `$D300` is this tick's `$DAE5`, `$D301` the previous tick's. Direction: left +1, right −1, 0 for neither or both. Bit 7 of `$FDC8` swaps left and right ([Speculation] a flag for some mode).
  - **The target `$D000`** (−127 to 127); `$D006` is the previous direction; the step table at `02961C` is −32, 0, +32.
    - When the direction changes (including release): `$D000` is set straight to ±32 or 0, and `$90(a0)` straight to `$D000 × 256`. So steering returns to zero in the tick the button is released.
    - While the same direction is held: `$D000` moves ±32 per tick, up to ±127, reaching the limit on the 4th tick.
    - Absolute values below 32 become zero.
  - **The actual steering `$90(a0)`** (±32512): every tick it becomes the average of "target × 256" and its old value, so it covers half the remaining distance per tick; absolute values below 32 become zero. The car physics read the steering from here (`022726`, `022E1A`, `027AAE`, `029796` and others).
  - `$AE(a0)` accumulates the change in steering (|change| ÷ 256, up to 200); `0298C2` subtracts 8 per tick, and above `0x50` a piece of code that adjusts `$78(a0)` is skipped ([Speculation] related to speed or sliding in sharp turns).
  - [Verified: `svptrace`, holding left for 60 frames in a Beginner race] The front wheels (word 5 of slot 2) ramp up to about `0xCC` over 6 to 8 ticks and return to zero in the tick of release, as described above.
- **Throttle** (`029742`): with bit 4 held the target speed rises by `0x40` per tick, without it falls by `0x40`; also digital.
- **Conclusion**: inside the game the steering is already a continuous value (`$90(a0)`); it is digital only at the input end. The target has just five levels, 0, ±32, ±64, ±96 and ±127, climbing one level at a time while the button is held.

## When SVP DRAM is written [Verified: DRAM write hook, 400 Beginner frames]

| Phase | Model slots | Sprite slots | Command block |
|---|---|---|---|
| 0 | 0 | 0 | 200 |
| 1, 2, 4 | 0 | 0 | 0 |
| 3 | 20655 | 22 | 202 |

- The model slots and sprite slots are **written only in phase 3**, and the command is issued in the same frame. HLE reads them at the moment the command is issued, so every command 02 is a complete snapshot of a tick.
- With dropped frames, phase 3 writes the next tick's model slots while the SVP is still drawing the previous command. [Speculation] Slots the SVP reads only after that are drawn with a mix of old and new data. This may be one reason HLE matches less well at the Medium start.

## Design for raising the FPS

- **Interpolation**: between two neighbouring commands 02 received by HLE, the camera and every object's position and angles are interpolated, with the ratio taken from the current time (which frame of the tick, plus the time within the frame). Implementation in `hle.md`, "Interpolation".
  - The cost is that the picture catches up with the latest tick one tick later. But in the original it takes 9 frames from issuing a command to displaying it (`svp-commands.md` section 1.3), while HLE can draw as soon as the command arrives, so even one tick later it is still faster than the original.
  - The sky shifts with the interpolated yaw. The HUD still follows the original frames.
- **Dropped frames**: as long as the SVP draws fast enough there is no phase 4, and the game keeps 15 ticks per second even in scenes that overload the SVP, like an original without slowdown. Implementation and measurements in the next section.
- **Pad input**: the replay records 1 byte per tick. [Verified: pressing left in each of the 4 frames of a tick during a race, comparing slots 0 and 1 of every command with a run without the press] The game logic reads the pad once per tick, in phase 3 (the frame the command is issued): a press in that frame shows in the command issued in the same frame; a press 1 to 3 frames earlier waits for that frame. Interpolation needs one more tick (4 frames) to show it fully, which is still shorter than the original SVP's 9 frames from issuing a command to displaying it.

## Measured: unlocking the FPS and speeding up the SVP

**Emulation speed** [Verified: windowless probe, core plus SVP]: frames 2470 to 9000 (6531 frames) take about 4.5 seconds, about 1450 frames per second. Performance is not a limit.

**Running the emulation faster than 60 Hz**: the game logic and the timers both count in frames, so the whole game (including the TIME countdown) speeds up together. That is fast-forward, not smoother motion. So **it is not unlocked directly; interpolation is used instead**.

**Speeding up the SVP** [Verified: probe counting race frames (main loop target `0248AC`), speed-up from frame 2470, run to frame 9000]:

GPGX runs the SVP for `SVP_cycles` cycles per scanline (800 by default).

| SVP clock | Beginner dropped frames | Medium dropped frames | Expert dropped frames |
|---|---|---|---|
| 1× (original) | 0 | 101 | 127 |
| 2× | 0 | 0 | 0 |
| 4× | 0 | 0 | 0 |

- From 2× up, every tick on all three courses is exactly 4 frames (for example Expert: 6530 frames, 1632 ticks).
- The speed-up costs almost nothing: GPGX detects the SVP's wait loop (`svp-commands.md` section 5), so idling uses no emulation time, and the work per command is unchanged.
- This agrees with notaz's observation: overclocking the SVP does not take the FPS beyond 15; it only removes the slowdown.

**The front end's "no slowdown" option** (key 6 in `vrr` or `-s`; on by default when playing, off for `-d` checks):
- Speeds the SVP up 4× during races.
- **Only during races** (when HLE 3D is on screen). Speeding up from power-on would also speed up the SVP animations on the title and menus and shorten them; every input script's timing would be off, and the menus would behave differently from the original.
- [Verified: `vrr -s -d 9000 -t 2470`] The per-frame HLE comparison has no frame below 85% on any of the three courses (averages 94.1% to 94.9%). Medium's lowest rises from 86.5% to 87.4% because the dropped frames at the start are gone.
- This changes the game itself: in the original the timer keeps running during slowdown while the cars slow down; with the speed-up there is no such loss (at the start the cars are faster relative to the timer than in the original). So `-d` checks turn it off; when playing it is on by default, called SVP overclock in the menu.

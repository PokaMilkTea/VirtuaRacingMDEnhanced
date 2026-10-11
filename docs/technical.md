# Technical notes


This document is for anyone who wants to build the program themselves, understand how it works, or is simply curious about the technical side. Most of it needs no programming knowledge; where you do have to type something, the exact command is given.

To dig into how the game and the SVP chip work, see the other documents in `docs/`.

## A few terms first

- **Mega Drive**: SEGA's 16-bit console, called the Genesis in North America.
- **SVP**: an extra chip inside the Virtua Racing cartridge that draws the 3D. It made 3D racing possible on the Mega Drive back then, but it is slow, and its picture is only 256×192.
- **Emulator**: a program that pretends to be a Mega Drive on a computer. This project uses Genesis Plus GX.
- **HLE**: the core idea of this project. When the game asks the SVP to draw the 3D, we catch that request and draw it again with the computer's graphics card: sharper, smoother and further into the distance.
- **Frame**: one picture on the screen. The console shows 60 frames a second, so one frame is about 0.017 seconds.

## Building it yourself

### Mac

Install [Homebrew](https://brew.sh) first, then type this in Terminal:

```sh
brew install sdl2
make
```

You get three programs:

| Program | What it is for |
|---|---|
| `build/vrr` | The game itself; this is the one you play |
| `build/svptrace` | A research tool; players don't need it |
| `third_party/genplus-gx/sdl/gen_sdl2` | The plain emulator, for comparing with the original |

Some "warnings" while building are normal and can be ignored.

### Windows

The Windows version is also built on a Mac:

```sh
make win
```

`build/win/` then holds `vrr.exe` and `SDL2.dll`. Keep the two in the same folder and copy them to a Windows PC to play (64-bit Windows 10 or later).

- The first `make win` downloads the tools it needs into the `deps/` folder (about 140 MB, about 860 MB unpacked). Nothing is installed on the system; delete `deps/` when you no longer need it.
- On laptops with two graphics cards, the program asks for the faster one.
- **Windows Defender may call it a trojan.** That is a false alarm: new programs without a digital signature are often flagged like this. The `!ml` in the message means "a machine-learning guess", not an actual virus found. You can allow it under Protection history in Windows Security.

### Linux

Build it on the Linux computer itself. On Ubuntu or Debian:

```sh
sudo apt install build-essential libsdl2-dev zlib1g-dev
make
build/vrr "Virtua Racing (USA).md"
```

- On other distributions, install a C compiler, `make`, and the development packages of SDL2 and zlib.
- It needs a graphics driver with OpenGL 3.3.
- The program is built for that computer's processor and may not run on another one.

## Running and settings

The simplest way is to type this in Terminal:

```sh
build/vrr "rom/Virtua Racing (USA).md"
```

On Windows you can drag the ROM file onto `vrr.exe`.

Every setting can be changed in the game: press Esc to open the menu. Your choices are remembered.

### Where settings and saves are kept

- `vrr.ini`: your settings. Normally you change them in the menu; it is a plain text file, so you can also open it in a text editor, and every line explains itself. A few advanced settings (such as how far away the haze starts) can only be changed here.
- `vrr.gp0`: the save state (press O in the game to save, P to load; one slot, a new save replaces the old one). It contains part of the game cartridge's data, so please don't share it.
- On a Mac and on Linux they go in the folder you started the program from; on Windows, beside `vrr.exe`.

### Advanced start-up options

Players don't need these; they are for testing. Add them to the command:

| Option | Effect |
|---|---|
| `-v` | Print the version and exit (the title bar shows it too) |
| `-W` | Start in widescreen (16:9) |
| `-s` | Turn on the SVP overclock (no slowdown with many cars) |
| `-r 1`, `-r 2`, `-r 4`, `-r 0` | 3D resolution: 1, 2 or 4 times the original, or 0 to follow the window size |
| `-l file` | Start from a given save state |
| `-p frame:keys` | Press buttons automatically, for testing |
| `-d N` | Check mode, see "How we know the picture is right" below |

## Why is the picture smoother and the response faster?

This is the question people ask most. You would expect that making the picture smoother by "filling in" frames makes the controls slower. In fact they get faster, because the original was slow to begin with.

### The original: each picture takes 9 frames to appear

Every 4 frames the game decides what the track looks like right now (where the cars are, which way they turn) and hands it to the SVP chip to draw. That picture goes through three steps before it reaches the screen:

1. **The SVP draws it.** The chip is slow: one picture takes about 4 frames.
2. **It is moved into the console's video memory.** The finished picture is copied over in 3 parts, which takes several more frames.
3. **The screen switches to it.** Only when everything is copied does the new picture appear.

All together, **from the moment the game decides on a picture to the moment you see it takes 9 frames (about 0.15 seconds)**.

Think of a restaurant: after you order, the kitchen (the SVP) takes a long time to cook, then the waiter (the console) brings the dish over in three trips. The kitchen and waiter work on several tables at once, so a dish arrives every so often (15 pictures a second), but every dish was ordered a while ago.

### This project: drawn as soon as the game decides

At the moment the game hands the picture to the SVP, we take the same information and draw it straight away with the computer's graphics card. The graphics card draws a picture in an instant, with no waiting for the SVP and no copying around. So **we can show the same picture almost immediately**.

### The price of smoothing: 4 frames, still faster than the original

The original has only 15 pictures a second, so it looks jumpy. To make it smooth, we fill in the in-between pictures from "the previous picture" to "the newest picture", the way animators draw the in-betweens between two key drawings.

The price is that the picture moves gradually to the newest state, which takes about 4 frames. But 4 frames is still much shorter than the original's 9, so **overall it responds faster than the original**.

We don't "predict the next picture" to save those 4 frames: with a sudden turn or a crash the guess would be wrong and the picture would snap back, which feels worse.

### How big the difference is

From pressing a button to seeing the result on screen:

| Setting | First visible reaction | Full reaction |
|---|---|---|
| Original (also smooth motion at 15 fps) | About 0.175 s | About 0.175 s |
| Smooth motion at Display or 60 fps | About 0.04 s | About 0.09 s |
| Smooth motion at 30 fps | About 0.06 s | About 0.09 s |

(Averages, not counting the monitor's own delay.)

Also, the speed, position and other information on screen never went through the SVP, so in the original they ran several frames ahead of the 3D. With smooth motion the two line up much better.

## How we know the picture is right

Our rule is "first draw it exactly like the original, then improve it". To check that, the program has a check mode:

```sh
build/vrr -d 4410 -p 1100:S -p 1106: -p 1400:S -p 1406: -p 1700:S -p 1706: \
          -p 2100:C -p 2106: -p 2700:B "rom/Virtua Racing (USA).md"
```

It plays automatically from power-on into a race (the `-p` options are the button presses, scripted in advance), and then:

- **Checks on every frame** that everything except the 3D (sky, HUD, menus) is exactly the same as the plain emulator's, down to the last pixel.
- **Compares the 3D with the original SVP picture, point by point.** About 94% of the pixels are identical; the differences are mostly along polygon edges, and side by side they are almost impossible to see.

Check mode always uses the original settings (it ignores your `vrr.ini`), so its results never depend on personal settings.

A longer check compares every frame from the start of a race to frame 9000, to make sure no frame is clearly wrong:

```sh
build/vrr -d 9000 -t 2470 -p 1100:S -p 1106: -p 1400:S -p 1406: -p 1700:S -p 1706: \
          -p 2000:R -p 2006: -p 2100:C -p 2106: -p 2400:B "rom/Virtua Racing (USA).md"
```

## Research tools

These were used during development to take the game apart; players don't need them:

- `build/svptrace`: records everything the game asks the SVP to draw.
- `tools/ssp16dis.py`: turns the SVP chip's program into readable instructions.
- `tools/svplog.py`: analyses which data the SVP reads and writes at each step.
- `tools/vdplayers.py`: splits the console's picture into its layers (background, foreground, sprites).

What these tools produce contains the game's own data. Keep it in the `out/` folder and don't upload or share it.

## What's in the folders

| Folder | Contents |
|---|---|
| `rom/` | Not in the repository: create it and put your own game ROM there (git ignores it) |
| `src/` | This project's code |
| `tools/` | Research tools |
| `docs/` | Documentation |
| `third_party/genplus-gx/` | The Genesis Plus GX emulator |

## What we changed in the emulator

The emulator used here is Genesis Plus GX, and we **barely touched it**: 8 files and about 50 lines in all, each change marked `VRR:` so it is easy to compare with the original.

Think of these changes as a few "sensors" installed in the emulator that tell us when something happens; **they don't change how the emulator behaves**:

| Change | In plain words |
|---|---|
| Notice when a line is drawn | Every time the console draws a line of the picture, we are told, so we can draw our own version in step |
| Notice of SVP memory writes | We are told when the game hands the 3D to the SVP; this is how HLE gets the picture's data |
| Log of SVP reads and writes | Only for the research tools: records everything the SVP does |
| A memory alignment fix | Fixes a crash in the Windows version |
| Two small fixes to the plain emulator | Stops the input method window popping up on key presses on a Mac, and fixes a few key codes |

The console's processors, sound, the SVP emulation and the game itself are not changed at all. Check mode confirms on every frame that the emulator's output is the same as before.

## For developers

The details below only matter when changing or maintaining the code; general readers can stop here.

### Building

- The root Makefile first builds Genesis Plus GX with its own SDL2 Makefile (`third_party/genplus-gx/sdl/Makefile.sdl2`), then links the core's object files into our programs. Its `DEFINES` must match those of `Makefile.sdl2` exactly, or the two sides disagree on struct layouts.
- `make test` runs self-checks that need no ROM (the settings file parser).
- Warnings such as `-O6` and `-Wstrict-prototypes` come from the upstream code.
- Windows:
  - The toolchain is [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), linked against SDL2's official MinGW development package. The Genesis Plus GX core is compiled by its own Makefile in Windows mode; zlib comes from the sources bundled with it.
  - Windows' `opengl32.dll` only has OpenGL 1.1, so the newer functions `vrr` calls are loaded from the driver at run time (`src/gl.h`).
  - The program exports `NvOptimusEnablement` and `AmdPowerXpressRequestHighPerformance` to ask dual-GPU laptops for the discrete GPU; the console prints which GPU is in use at start-up.
  - A crash prints the exception and its offset in `vrr.exe`.
  - `vrr -d 4410` gives bit-identical results to the macOS build.
- Linux:
  - OpenGL is linked as `-lGL` (a framework on macOS); the functions after OpenGL 1.1 are loaded at run time as on Windows (`src/gl.h`).
  - Under `-std=c99`, glibc hides POSIX functions such as `getopt` and libchdr's `fseeko`, so the Makefile adds `-D_DEFAULT_SOURCE`. GPGX's Makefile leaves its `DEFINES` out of some rules, so the GPGX build gets it through `CC`.

### Timing

- Near 60 Hz the emulator runs one frame per vsync, near 120 Hz one frame every second vsync; the emulated frame rate follows the measured refresh rate. At other refresh rates it runs at the console's 59.92 Hz from a time accumulator.
- After a missed vsync it catches up on the frames that were due (up to 4).
- Audio goes into an SDL queue; the samples per frame are nudged by up to 0.5% to keep the queue near its target.
- Timing statistics (missed vsyncs, audio underruns) are printed on exit.
- Smooth motion interpolates between ticks (`hle_smooth`). It draws on every vsync; 30 and 60 fps round the elapsed frames down to a multiple of 2 or 1, so each image stays for a fixed number of frames.

### Check mode (`vrr -d`) in detail

- `-d N` runs frames 0 to N as fast as possible, with input only from `-p`, always from the original settings (1×, the SVP's integer vertices, slowdown as the console, 4:3).
- Files written at frame N: `vrr_N.ppm` (the core's frame), `hle_N.ppm` (with the HLE 3D), `svp3d_N.ppm` and `hle3d_N.ppm` (the two 3D images), `full_N.ppm` (full colour); with `-r 2` or `-r 4` also the 3D at that resolution, with `-W` also the 16:9 frame.
- `-t F`: compare the HLE with the SVP on every frame from F on; prints the mean, the lowest and the frames below 85%. Frames where neither side drew anything (the screen faded out) are left out.
- `-F`: also writes the newest tick of frame N with the original distance, the far view and the far view with haze.
- With `-l`, frames and `-p` count from the saved state, which allows checking screens that need real play to reach (the finish, replays).
- `-p` buttons are `S A B C U D L R X Y Z`, `M` for Mode; empty for none.
- For the long regression run on Expert, add `-p 2030:R -p 2036:`.

### Where the code is

| File | Contents |
|---|---|
| `src/vrr.c` | The front end: timing, sound, input, menu, GPU drawing and compositing, verification |
| `src/hle.c` | The HLE: draw lists and SVP sprites built from the model slots, sprite slots and ROM |
| `src/layers.c` | Our own copy of the VDP layers, drawn as GPGX draws each line |
| `src/settings.c` | `vrr.ini` reading and writing |
| `src/core.c` | Core setup and input scripts shared by `vrr` and `svptrace` |
| `src/svptrace.c` | The research tracer; usage in the comment at the top of the file |

### Changes to Genesis Plus GX

`third_party/genplus-gx/` is upstream commit `939ce4f045f981f89965f24780cef045cc5e52d7`. `grep -rn "VRR:" third_party` lists every change.

| Files | Change | Used by |
|---|---|---|
| `core/vdp_render.c`, `vdp_render.h` | A hook at the start of every `render_line()` (`vrr_line_hook`), and one when part of a line is blanked to the backdrop (`vrr_blank_hook`) | `vrr` |
| `core/cart_hw/svp/svp.c`, `svp.h` | An observer for 68k word writes to SVP DRAM (`0x300000-0x30FFFF`) | `vrr`, `svptrace` |
| `core/cart_hw/svp/ssp16.c`, `ssp16.h` | An observer for every SVP external memory access through the PMx registers, with the address before auto-increment | `svptrace -m` only |
| `core/vdp_ctrl.h` | The declarations of `sat`, `vram`, `cram` and `vsram` gain the 4-byte alignment their definitions in `vdp_ctrl.c` have. Without it clang may assume 16-byte alignment and use aligned SSE loads | All builds |
| `sdl/sdl2/main.c` | SDL2's text input turned off, and key codes still using SDL1 names fixed | `gen_sdl2` |

Most upstream source files use CRLF line endings; changes keep them, so a diff against upstream shows only the changed lines.

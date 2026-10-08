/*
 * svptrace: run Virtua Racing headless on the Genesis Plus GX core and log
 * every SVP command the 68k issues, i.e. every nonzero write to DRAM 0x30FE08.
 * For each command it prints the command block and every non-empty model and
 * sprite slot as they are at that moment.
 *
 * usage: svptrace [-n frames] [-f first_logged_frame] [-m frame] [-p frame:keys]...
 *                 [-w frame]... [-v frame]... rom
 *   -n  frames to emulate (default 600)
 *   -f  only log from this frame on (default 0)
 *   -p  from that frame on, hold keys (any of S A B C U D L R X Y Z, M for Mode; empty = none)
 *   -w  write frame_NNNNNN.ppm (game viewport) after that frame, and print its palette
 *   -v  write vdp_NNNNNN.bin after that frame: "VDP1", reg[32], vram[64K], cram[128],
 *       vsram[128], each array as GPGX keeps it (16-bit words in host byte order)
 *   -m  log every SVP external memory access (PMx) made while executing the
 *       first command issued at or after that frame, until the next command
 *
 * Runs always start at power-on, so the same arguments give the same output.
 *
 * Output lines:
 *   F <frame> L <line> W <addr> = <data>   68k write to the command block
 *   F <frame> L <line> CMD <id>  FE00: 16 words of the command block
 *     M <slot> 32 words                    non-empty model slot
 *     S <slot> 8 words                     non-empty sprite slot
 *   PAL <frame> 64 words                   CRAM after a -w frame, 9-bit BBBGGGRRR
 *   P <svp pc> <R|W|O> <addr> <data>        SVP memory access (-m), O = overwrite-mode
 *                                           write (only nonzero nibbles land); addr is the
 *                                           68k-style byte address (ROM, 30xxxx
 *                                           DRAM, 39xxxx IRAM)
 *
 * DRAM layout (Devon, Sonic Retro SVP research thread, 2018):
 *   30EC00-30FBFF  64 model slots, 0x40 bytes each (slot 0 is usually the camera)
 *   30FC00-30FDFF  32 sprite slots, 0x10 bytes each
 *   30FE00-30FE1F  command block: 02 done, 06 sent, 08 command id, 10 parameter
 */
#include <unistd.h>
#include "core.h"

#define DRAM_WORD(a)   (*(uint16 *)(svp->dram + ((a) & 0x1fffe)))
#define MODEL_SLOTS    0x30EC00    /* 64 slots of 0x40 bytes */
#define SPRITE_SLOTS   0x30FC00    /* 32 slots of 0x10 bytes */
#define COMMAND_BLOCK  0x30FE00    /* 0x20 bytes */
#define COMMAND_ID     0x30FE08    /* the 68k writes the command number here last */
#define MAX_ARGS       256

static int frame;
static int first_logged;

/* -m: log the SVP's memory accesses during one command */
static int mem_log_from = -1;      /* the frame from which the next command is logged, -1 = off */
enum { MEM_LOG_WAITING, MEM_LOG_ON, MEM_LOG_DONE };
static int mem_log = MEM_LOG_WAITING;

static int slot_is_empty(uint32 base, int size)
{
  int offset;
  for (offset = 0; offset < size; offset += 2)
    if (DRAM_WORD(base + offset)) return 0;
  return 1;
}

/* Every slot in use: "  <tag> <number>" and its words */
static void print_slots(uint32 base, int count, int size, char tag)
{
  int slot, offset;
  for (slot = 0; slot < count; slot++, base += size)
  {
    if (slot_is_empty(base, size)) continue;
    printf("  %c %02d", tag, slot);
    for (offset = 0; offset < size; offset += 2) printf(" %04X", DRAM_WORD(base + offset));
    printf("\n");
  }
}

/* Called by GPGX's SVP (vrr_ssp_pm_hook) on every access through a PMx register. pmac is the
   PMC address register: the word address in bits 0-15 and 16-20, overwrite mode in bit 26. */
static void on_pm_access(unsigned int pc, unsigned int pmac, int write, unsigned int data)
{
  unsigned int word_address = ((pmac >> 16) & 0x1f) << 16 | (pmac & 0xffff);
  char kind;
  if (mem_log != MEM_LOG_ON) return;
  if (!write) kind = 'R';
  else if (pmac & 0x04000000) kind = 'O';    /* overwrite mode: only nonzero nibbles land */
  else kind = 'W';
  printf("P %04X %c %06X %04X\n", pc, kind, word_address << 1, data & 0xffff);
}

/* Called by GPGX (vrr_svp_dram_write_hook) on every 68k write to SVP DRAM */
static void on_dram_write(uint32 address, uint32 data)
{
  int offset;

  /* -m: start logging at the first command from mem_log_from on, stop at the next one */
  if (address == COMMAND_ID && data && mem_log_from >= 0)
  {
    if (mem_log == MEM_LOG_ON) mem_log = MEM_LOG_DONE;
    else if (mem_log == MEM_LOG_WAITING && frame >= mem_log_from) mem_log = MEM_LOG_ON;
  }

  if (frame < first_logged) return;
  if (address < COMMAND_BLOCK || address >= COMMAND_BLOCK + 0x20) return;

  printf("F %06d L %03d W %06X = %04X\n", frame, v_counter, address, data);
  if (address != COMMAND_ID || !data) return;

  /* a command: the whole command block and every slot in use */
  printf("F %06d L %03d CMD %02X  FE00:", frame, v_counter, data);
  for (offset = 0; offset < 0x20; offset += 2) printf(" %04X", DRAM_WORD(COMMAND_BLOCK + offset));
  printf("\n");
  print_slots(MODEL_SLOTS, 64, 0x40, 'M');
  print_slots(SPRITE_SLOTS, 32, 0x10, 'S');
}

/* -v: the VDP state as vdp_NNNNNN.bin */
static void write_vdp(void)
{
  char name[32];
  FILE *f;

  sprintf(name, "vdp_%06d.bin", frame);
  if (!(f = fopen(name, "wb"))) return;
  fwrite("VDP1", 1, 4, f);
  fwrite(reg, 1, 0x20, f);
  fwrite(vram, 1, 0x10000, f);
  fwrite(cram, 1, 0x80, f);
  fwrite(vsram, 1, 0x80, f);
  fclose(f);
}

/* -w: the frame as frame_NNNNNN.ppm, each RGB565 pixel widened to 8 bits a channel */
static void write_ppm(void)
{
  char name[32];
  FILE *f;
  int x, y;

  sprintf(name, "frame_%06d.ppm", frame);
  if (!(f = fopen(name, "wb"))) return;
  fprintf(f, "P6\n%d %d\n255\n", bitmap.viewport.w, bitmap.viewport.h);
  for (y = 0; y < bitmap.viewport.h; y++)
  {
    uint16 *pixel = (uint16 *)(bitmap.data + (y + bitmap.viewport.y) * bitmap.pitch) + bitmap.viewport.x;
    for (x = 0; x < bitmap.viewport.w; x++, pixel++)
    {
      uint8 rgb[3];
      rgb[0] = (*pixel >> 11) << 3;
      rgb[1] = ((*pixel >> 5) & 0x3f) << 2;
      rgb[2] = (*pixel & 0x1f) << 3;
      fwrite(rgb, 1, 3, f);
    }
  }
  fclose(f);
}

/* -w: "PAL <frame>" and the 64 CRAM colours (9-bit BBBGGGRRR) */
static void print_palette(void)
{
  int colour;
  printf("PAL %06d", frame);
  for (colour = 0; colour < 64; colour++) printf(" %03X", *(uint16 *)&cram[colour << 1]);
  printf("\n");
}

int main(int argc, char **argv)
{
  static int16 samples[8192];
  int press_frame[MAX_ARGS], ppm_frames[MAX_ARGS], vdp_frames[MAX_ARGS];
  int press_count = 0, ppm_count = 0, vdp_count = 0;
  uint16 press_pad[MAX_ARGS];
  int frames = 600, opt, i;

  while ((opt = getopt(argc, argv, "n:f:m:p:v:w:")) != -1)
  {
    char keys[16] = "";
    switch (opt)
    {
      case 'n': frames = atoi(optarg); break;
      case 'f': first_logged = atoi(optarg); break;
      case 'm': mem_log_from = atoi(optarg); break;
      case 'p':
        if (press_count == MAX_ARGS || sscanf(optarg, "%d:%15s", &press_frame[press_count], keys) < 1) return 1;
        press_pad[press_count++] = core_parse_keys(keys);
        break;
      case 'w':
        if (ppm_count == MAX_ARGS) return 1;
        ppm_frames[ppm_count++] = atoi(optarg);
        break;
      case 'v':
        if (vdp_count == MAX_ARGS) return 1;
        vdp_frames[vdp_count++] = atoi(optarg);
        break;
      default: return 1;
    }
  }
  if (optind >= argc)
  {
    fprintf(stderr, "usage: %s [-n frames] [-f first_logged_frame] [-m frame] [-p frame:keys]... [-w frame]... [-v frame]... rom\n", argv[0]);
    return 1;
  }

  if (core_init(argv[optind], 48000, 0) < 0) return 1;

  vrr_svp_dram_write_hook = on_dram_write;
  vrr_ssp_pm_hook = on_pm_access;

  for (frame = 0; frame < frames; frame++)
  {
    for (i = 0; i < press_count; i++)
      if (press_frame[i] == frame) core_pad = press_pad[i];

    system_frame_gen(0);
    audio_update(samples);     /* drain the sound buffers; the output is discarded */

    for (i = 0; i < vdp_count; i++)
      if (vdp_frames[i] == frame) write_vdp();
    for (i = 0; i < ppm_count; i++)
      if (ppm_frames[i] == frame)
      {
        write_ppm();
        print_palette();
      }
  }
  return 0;
}

/* Genesis Plus GX core setup shared by our programs (svptrace, vrr).
   Same init sequence as third_party/genplus-gx/sdl/sdl2/main.c, minus video/audio output. */
#include "core.h"
#include "sms_ntsc.h"
#include "md_ntsc.h"

/* symbols the core expects from the frontend (see sdl/sdl2/main.c) */
md_ntsc_t *md_ntsc;
sms_ntsc_t *sms_ntsc;

uint16 core_pad;

int sdl_input_update(void)
{
  input.pad[0] = core_pad;
  return 1;
}

int core_init(const char *rom, int samplerate, double fps)
{
  set_config_defaults();
  system_bios = 0;
  memset(boot_rom, 0xFF, sizeof(boot_rom));

  memset(&bitmap, 0, sizeof(bitmap));
  bitmap.width  = 720;
  bitmap.height = 576;
  bitmap.pitch  = bitmap.width * 2; /* USE_16BPP_RENDERING: RGB565 */
  bitmap.data   = malloc(bitmap.pitch * bitmap.height);
  bitmap.viewport.changed = 3;

  if (!load_rom((char *)rom))
  {
    fprintf(stderr, "cannot load %s\n", rom);
    return -1;
  }

  audio_init(samplerate, fps);
  system_init(); /* cartridge hardware, including the SVP, is set up here */
  system_reset();

  if (!svp)
  {
    fprintf(stderr, "%s is not an SVP cartridge\n", rom);
    return -1;
  }
  return 0;
}

uint16 core_parse_keys(const char *keys)
{
  uint16 buttons = 0;
  const char *key;
  for (key = keys; *key; key++)
  {
    switch (*key)
    {
      case 'S': buttons |= INPUT_START; break;
      case 'A': buttons |= INPUT_A;     break;
      case 'B': buttons |= INPUT_B;     break;
      case 'C': buttons |= INPUT_C;     break;
      case 'U': buttons |= INPUT_UP;    break;
      case 'D': buttons |= INPUT_DOWN;  break;
      case 'L': buttons |= INPUT_LEFT;  break;
      case 'R': buttons |= INPUT_RIGHT; break;
      case 'X': buttons |= INPUT_X;     break;   /* 6-button pad; in a race X, Y, Z pick the camera view */
      case 'Y': buttons |= INPUT_Y;     break;
      case 'Z': buttons |= INPUT_Z;     break;
      case 'M': buttons |= INPUT_MODE;  break;
      default:
        fprintf(stderr, "unknown key '%c'\n", *key);
        exit(1);
    }
  }
  return buttons;
}

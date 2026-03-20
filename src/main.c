#include "CPU.h"
#include "Memory.h"
#include "PPU.h"
#include "VirtuaAPUBridge.h"

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    bool enabled;
    bool initialized;
    int scale;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
} DisplayContext;

typedef struct
{
    bool display_enabled;
    bool show_help;
    bool fps_diag;
    bool gbc_mode;
    bool debug_print;
    uint32_t run_for_ms;
    const char *frame_dump_path;
    double speed_mult;
} AppOptions;

static bool rom_requests_cgb(const MemoryState *memory)
{
    if (!memory || !memory->cartridge.rom_data || memory->cartridge.rom_size <= 0x143u)
        return false;

    uint8_t cgb_flag = memory->cartridge.rom_data[0x143];
    return cgb_flag == 0xC0u;
}

/* -- Display --------------------------------------------------------------- */

static void display_shutdown(DisplayContext *display)
{
    if (!display->enabled)
        return;

    if (display->texture)
    {
        SDL_DestroyTexture(display->texture);
        display->texture = NULL;
    }
    if (display->renderer)
    {
        SDL_DestroyRenderer(display->renderer);
        display->renderer = NULL;
    }
    if (display->window)
    {
        SDL_DestroyWindow(display->window);
        display->window = NULL;
    }
    if (display->initialized)
    {
        SDL_Quit();
        display->initialized = false;
    }
    display->enabled = false;
}

static bool display_init(DisplayContext *display)
{
    if (!display->enabled)
        return true;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0)
    {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        display->enabled = false;
        return false;
    }
    display->initialized = true;

    const char *win_title = "GB Emulator";
    display->window = SDL_CreateWindow(win_title,
                                       GB_SCREEN_WIDTH * display->scale,
                                       GB_SCREEN_HEIGHT * display->scale,
                                       0);
    if (!display->window)
    {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        display_shutdown(display);
        return false;
    }

    display->renderer = SDL_CreateRenderer(display->window, NULL);

    // TODO : Vsync arg
    SDL_SetRenderVSync(display->renderer, SDL_RENDERER_VSYNC_DISABLED );
    //SDL_SetRenderVSync(display->renderer, true);
    if (!display->renderer)
    {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        display_shutdown(display);
        return false;
    }

    
    SDL_SetRenderDrawColor(display->renderer, 0.0f, 0.0f, 0.0f, 1.0f);

    display->texture = SDL_CreateTexture(display->renderer,
                                         SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         GB_SCREEN_WIDTH,
                                         GB_SCREEN_HEIGHT);
    if (!display->texture)
    {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        display_shutdown(display);
        return false;
    }
    SDL_SetTextureScaleMode(display->texture, SDL_SCALEMODE_NEAREST);

    return true;
}

static void handle_key_event(MemoryState *memory, SDL_Keycode key, bool pressed)
{
    switch (key)
    {
    case SDLK_RIGHT:
        memory_set_button_state(memory, JOYPAD_RIGHT, pressed);
        break;
    case SDLK_LEFT:
        memory_set_button_state(memory, JOYPAD_LEFT, pressed);
        break;
    case SDLK_UP:
        memory_set_button_state(memory, JOYPAD_UP, pressed);
        break;
    case SDLK_DOWN:
        memory_set_button_state(memory, JOYPAD_DOWN, pressed);
        break;
    case SDLK_Z:
        memory_set_button_state(memory, JOYPAD_B, pressed);
        break;
    case SDLK_A:
        memory_set_button_state(memory, JOYPAD_A, pressed);
        break;
    case SDLK_S:
        memory_set_button_state(memory, JOYPAD_SELECT, pressed);
        break;
    case SDLK_Q:
        memory_set_button_state(memory, JOYPAD_START, pressed);
        break;
    default:
        break;
    }
}

static bool display_poll_events(DisplayContext *display, MemoryState *memory)
{
    if (!display->enabled)
        return true;

    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_EVENT_QUIT:
            return false;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.repeat)
                break;
            if (event.key.key == SDLK_ESCAPE)
                return false;
            handle_key_event(memory, event.key.key, true);
            break;
        case SDL_EVENT_KEY_UP:
            handle_key_event(memory, event.key.key, false);
            break;
        default:
            break;
        }
    }
    return true;
}

static void display_present(DisplayContext *display, const PPUState *ppu)
{
    if (!display->enabled || !display->texture || !display->renderer)
        return;

    if (SDL_UpdateTexture(display->texture, NULL, ppu->fb,
                          GB_SCREEN_WIDTH * (int)sizeof(uint32_t)) < 0)
    {
        fprintf(stderr, "SDL_UpdateTexture failed: %s\n", SDL_GetError());
        return;
    }

    SDL_RenderClear(display->renderer);
    SDL_FRect dst = {
        0.0f, 0.0f,
        (float)(GB_SCREEN_WIDTH  * display->scale),
        (float)(GB_SCREEN_HEIGHT * display->scale)};
    SDL_RenderTexture(display->renderer, display->texture, NULL, &dst);
    SDL_RenderPresent(display->renderer);
}

static void apu_snapshot_from_memory(const MemoryState *memory, GBAPURegisters *snap)
{
    snap->NR10 = memory->memory.NR10;
    snap->NR11 = memory->memory.NR11;
    snap->NR12 = memory->memory.NR12;
    snap->NR13 = memory->memory.NR13;
    snap->NR14 = memory->memory.NR14;
    snap->_pad15 = 0;
    snap->NR21 = memory->memory.NR21;
    snap->NR22 = memory->memory.NR22;
    snap->NR23 = memory->memory.NR23;
    snap->NR24 = memory->memory.NR24;
    snap->NR30 = memory->memory.NR30;
    snap->NR31 = memory->memory.NR31;
    snap->NR32 = memory->memory.NR32;
    snap->NR33 = memory->memory.NR33;
    snap->NR34 = memory->memory.NR34;
    snap->_pad1f = 0;
    snap->NR41 = memory->memory.NR41;
    snap->NR42 = memory->memory.NR42;
    snap->NR43 = memory->memory.NR43;
    snap->NR44 = memory->memory.NR44;
    snap->NR50 = memory->memory.NR50;
    snap->NR51 = memory->memory.NR51;
    snap->NR52 = memory->memory.NR52;
    memset(snap->_pad27_2f, 0, sizeof(snap->_pad27_2f));
    memcpy(snap->WAVE_RAM, memory->memory.WAVE_RAM, sizeof(snap->WAVE_RAM));
}

static void apu_apply_runtime_state(MemoryState *memory, const GBAPURegisters *snap)
{
    /* Keep CPU-visible register state authoritative and only apply the
       runtime bits updated by VirtuaAPU during rendering. */
    memory->memory.NR14 &= 0x7Fu;
    memory->memory.NR24 &= 0x7Fu;
    memory->memory.NR34 &= 0x7Fu;
    memory->memory.NR44 &= 0x7Fu;
    memory->memory.NR52 = (uint8_t)((memory->memory.NR52 & 0xF0u) | (snap->NR52 & 0x0Fu));
}

static void apu_advance(MemoryState *memory,
                        SDL_AudioStream *audio_stream,
                        uint32_t audio_sample_rate,
                        uint32_t cycles,
                        double *sample_accum)
{
    if (!memory || !sample_accum || cycles == 0)
        return;

    double mcycles_per_second = memory->double_speed ? 2097152.0 : 1048576.0;
    *sample_accum += ((double)cycles * (double)audio_sample_rate) / mcycles_per_second;

    while (*sample_accum >= 1.0)
    {
        uint32_t samples = (uint32_t)(*sample_accum);
        if (samples > 512u)
            samples = 512u;
        *sample_accum -= (double)samples;

        int16_t apu_buf[512 * 2];
        GBAPURegisters apu_snap;
        apu_snapshot_from_memory(memory, &apu_snap);
        viruaapu_render(&apu_snap, memory->double_speed ? 1 : 0, samples, apu_buf);
        apu_apply_runtime_state(memory, &apu_snap);

        if (audio_stream)
        {
            int queued = SDL_GetAudioStreamQueued(audio_stream);
            int target_queued = (int)((audio_sample_rate * 2u * sizeof(int16_t) * 40u) / 1000u);
            if (queued < target_queued)
            {
                SDL_PutAudioStreamData(audio_stream, apu_buf,
                                       (int)(samples * 2 * (int)sizeof(int16_t)));
            }
        }
    }
}

static void write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static bool save_frame_bmp(const char *path, const PPUState *ppu)
{
    if (!path || path[0] == '\0' || !ppu)
        return false;

    FILE *f = fopen(path, "wb");
    if (!f)
        return false;

    const uint32_t width = GB_SCREEN_WIDTH;
    const uint32_t height = GB_SCREEN_HEIGHT;
    const uint32_t row_bytes = width * 3u;
    const uint32_t padded_row_bytes = (row_bytes + 3u) & ~3u;
    const uint32_t pixel_bytes = padded_row_bytes * height;
    const uint32_t file_size = 14u + 40u + pixel_bytes;

    uint8_t file_header[14] = {0};
    uint8_t info_header[40] = {0};
    file_header[0] = 'B';
    file_header[1] = 'M';
    write_le32(&file_header[2], file_size);
    write_le32(&file_header[10], 14u + 40u);

    write_le32(&info_header[0], 40u);
    write_le32(&info_header[4], width);
    write_le32(&info_header[8], height);
    write_le16(&info_header[12], 1u);
    write_le16(&info_header[14], 24u);
    write_le32(&info_header[20], pixel_bytes);

    if (fwrite(file_header, sizeof(file_header), 1, f) != 1 ||
        fwrite(info_header, sizeof(info_header), 1, f) != 1)
    {
        fclose(f);
        return false;
    }

    uint8_t row[padded_row_bytes];
    for (int y = (int)height - 1; y >= 0; --y)
    {
        size_t row_index = 0;
        for (uint32_t x = 0; x < width; ++x)
        {
            uint32_t color = ppu->fb[(size_t)y * width + x];
            row[row_index++] = (uint8_t)(color & 0xFFu);
            row[row_index++] = (uint8_t)((color >> 8) & 0xFFu);
            row[row_index++] = (uint8_t)((color >> 16) & 0xFFu);
        }
        while (row_index < padded_row_bytes)
            row[row_index++] = 0;

        if (fwrite(row, padded_row_bytes, 1, f) != 1)
        {
            fclose(f);
            return false;
        }
    }

    fclose(f);
    return true;
}

/* -- Argument parsing ------------------------------------------------------ */

static void print_usage(const char *argv0)
{
    const char *prog = (argv0 && argv0[0] != '\0') ? argv0 : "gb_emu";
    printf("Usage: %s [options] [rom_path]\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help       Show this help and exit\n");
    printf("  --display        Enable SDL display window (default)\n");
    printf("  --no-display     Disable SDL display window\n");
    printf("  --fps-diag       Print FPS diagnostics to stdout\n");
    printf("  --gbc            Force Game Boy Color (CGB) mode\n");
    printf("  --debug-print    Print serial debug output from FF01/FF02\n");
    printf("  --run-for-ms <n> Exit after n milliseconds of host time\n");
    printf("  --frame-dump <p> Save the last frame to BMP on exit\n");
    printf("  --speed <x>      Emulation speed multiplier (default 1.0)\n");
    printf("\n");
    printf("Controls (keyboard):\n");
    printf("  Arrow keys       D-Pad\n");
    printf("  A                Button A\n");
    printf("  Z                Button B\n");
    printf("  Q                Start\n");
    printf("  S                Select\n");
    printf("  Escape           Quit\n");
}

static const char *parse_arguments(int argc, char **argv, AppOptions *options)
{
    const char *rom_path = NULL;
    options->display_enabled = true;
    options->show_help       = false;
    options->fps_diag        = false;
    options->gbc_mode        = false;
    options->debug_print     = false;
    options->run_for_ms      = 0;
    options->frame_dump_path = NULL;
    options->speed_mult      = 1.0;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            options->show_help = true;
        }
        else if (strcmp(argv[i], "--no-display") == 0)
        {
            options->display_enabled = false;
        }
        else if (strcmp(argv[i], "--display") == 0)
        {
            options->display_enabled = true;
        }
        else if (strcmp(argv[i], "--fps-diag") == 0)
        {
            options->fps_diag = true;
        }
        else if (strcmp(argv[i], "--gbc") == 0)
        {
            options->gbc_mode = true;
        }
        else if (strcmp(argv[i], "--debug-print") == 0)
        {
            options->debug_print = true;
        }
        else if (strcmp(argv[i], "--run-for-ms") == 0 && (i + 1) < argc)
        {
            unsigned long v = strtoul(argv[++i], NULL, 10);
            options->run_for_ms = (uint32_t)v;
        }
        else if (strcmp(argv[i], "--frame-dump") == 0 && (i + 1) < argc)
        {
            options->frame_dump_path = argv[++i];
        }
        else if (strcmp(argv[i], "--speed") == 0 && (i + 1) < argc)
        {
            double v = strtod(argv[++i], NULL);
            if (v > 0.0)
                options->speed_mult = v;
        }
        else if (argv[i][0] == '-')
        {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
        }
        else if (!rom_path)
        {
            rom_path = argv[i];
        }
    }

    return rom_path;
}

/* -- CGB boot ROM DMG compatibility palettes ------------------------------- */

/*
 * When the real CGB boot ROM runs a DMG cart, it assigns one of ~50
 * pre-defined colour palettes based on a checksum of the cartridge
 * title.  The table below replicates that behaviour.
 *
 * Each palette set has 3 four-colour palettes (BG, OBJ0, OBJ1) in
 * RGB555 (R in bits 0-4, G in 5-9, B in 10-14) – the native GBC format.
 *
 * Sources:
 *   Pan Docs – "Compatibility palettes"
 *   CGB boot ROM disassembly
 */

typedef struct { uint16_t c[4]; } Pal4;
typedef struct { Pal4 bg, obj0, obj1; } PalScheme;

/* ---------- base palettes (referenced by the combo table) ---------- */
static const Pal4 bp[] = {
    /* 0  Up            */ {{ 0x7FFF, 0x7E10, 0x48E7, 0x0000 }},
    /* 1  Up+A          */ {{ 0x7FFF, 0x42B5, 0x3DC8, 0x0000 }},
    /* 2  Up+B          */ {{ 0x7FFF, 0x7E60, 0x3D10, 0x0000 }},
    /* 3  Down          */ {{ 0x7FFF, 0x7EAC, 0x40C0, 0x0000 }},
    /* 4  Down+A        */ {{ 0x7FFF, 0x5B94, 0x284E, 0x0000 }},
    /* 5  Down+B        */ {{ 0x7FFF, 0x5AD6, 0x318C, 0x0000 }},
    /* 6  Left          */ {{ 0x7FFF, 0x65BD, 0x1B36, 0x0000 }},
    /* 7  Left+A        */ {{ 0x7FFF, 0x329F, 0x001F, 0x0000 }},
    /* 8  Left+B / Gray */ {{ 0x7FFF, 0x5294, 0x294A, 0x0000 }},
    /* 9  Right         */ {{ 0x7FFF, 0x63D0, 0x2180, 0x0000 }},
    /* 10 Right+A       */ {{ 0x7FFF, 0x7F80, 0x5200, 0x0000 }},
    /* 11 Right+B       */ {{ 0x7FFF, 0x3400, 0x0A00, 0x0000 }},
    /* 12 extra brown   */ {{ 0x7FFF, 0x67BF, 0x3D70, 0x0000 }},
};

/* ---------- palette combinations (bg, obj0, obj1 indices) ---------- */
/* IDs referenced by the checksum lookup below.                        */
static const struct { uint8_t bg, obj0, obj1; } combos[] = {
    /*  0 */ {  8,  8,  8 },   /* grayscale (default) */
    /*  1 */ {  0,  0,  0 },   /* red/pink            */
    /*  2 */ {  7,  7,  7 },   /* red (bright)        */
    /*  3 */ {  0,  7,  7 },   /* pink + red obj      */
    /*  4 */ {  7,  0,  0 },   /* red obj, red BG inv */
    /*  5 */ { 11, 11, 11 },   /* dark green          */
    /*  6 */ {  3,  3,  3 },   /* green/yellow        */
    /*  7 */ {  9,  9,  9 },   /* brown               */
    /*  8 */ {  2,  2,  2 },   /* orange/yellow       */
    /*  9 */ {  1,  1,  1 },   /* dark blue/pastel    */
    /* 10 */ {  4,  4,  4 },   /* dark teal           */
    /* 11 */ {  5,  5,  5 },   /* pastel mix          */
    /* 12 */ {  6,  6,  6 },   /* yellow/orange       */
    /* 13 */ { 10, 10, 10 },   /* orange/brown bold   */
    /* 14 */ { 12, 12, 12 },   /* olive               */
    /* 15 */ {  8,  0,  0 },   /* gray BG, red OBJ    */
    /* 16 */ {  0,  8,  8 },   /* red BG, gray OBJ    */
    /* 17 */ {  8,  7,  7 },   /* gray BG, red OBJ2   */
    /* 18 */ {  7,  8,  8 },   /* red BG, gray OBJ2   */
    /* 19 */ {  3,  8,  8 },   /* green BG, gray OBJ  */
    /* 20 */ {  8,  3,  3 },   /* gray BG, green OBJ  */
    /* 21 */ {  0, 11,  7 },   /* pink BG, mixed OBJ  */
    /* 22 */ {  3,  7,  7 },   /* green BG, red OBJ   */
    /* 23 */ {  9,  0,  0 },   /* brown BG, red OBJ   */
};

/*
 * Checksum → combo lookup.
 * The CGB boot ROM computes:  sum of ROM[0x0134..0x0143] (mod 256)
 * and looks into a table of known values.
 * Some checksums collide; the 4th title char (ROM[0x0137]) disambiguates.
 */
typedef struct { uint8_t csum; uint8_t title4; uint8_t combo; } CsumEntry;

static const CsumEntry csum_table[] = {
    /* checksum, title4 (0=any), combo */
    { 0x00, 0x00,  0 },  /* default for 0x00 */
    { 0x01, 0x00,  7 },  /* brown games */
    { 0x08, 0x00,  1 },
    { 0x0D, 0x00,  9 },
    { 0x10, 0x00,  6 },
    { 0x14, 0x00,  3 },  /* POKEMON RED */
    { 0x15, 0x00, 10 },
    { 0x16, 0x00,  5 },
    { 0x17, 0x00, 13 },
    { 0x19, 0x00, 12 },
    { 0x1D, 0x00,  2 },
    { 0x27, 0x00, 17 },
    { 0x28, 0x00,  8 },
    { 0x29, 0x00, 11 },
    { 0x34, 0x00,  5 },
    { 0x35, 0x00, 14 },
    { 0x36, 0x00,  6 },
    { 0x39, 0x00,  4 },
    { 0x3C, 0x00, 10 },
    { 0x3D, 0x00, 12 },
    { 0x3E, 0x00, 13 },
    { 0x3F, 0x00, 12 },
    { 0x43, 0x00, 11 },
    { 0x46, 0x00, 18 },  /* common blue palette (POKEMON BLUE JP etc.) */
    { 0x49, 0x00,  1 },
    { 0x4B, 0x00,  7 },
    { 0x4E, 0x00,  6 },
    { 0x52, 0x00, 10 },
    { 0x58, 0x00,  4 },
    { 0x59, 0x00,  2 },
    { 0x5C, 0x00, 19 },
    { 0x5D, 0x00, 14 },
    { 0x61, 0x00,  9 },  /* common: TETRIS, POKEMON BLUE (US)… */
    { 0x67, 0x00, 11 },
    { 0x68, 0x00,  1 },
    { 0x69, 0x00,  7 },
    { 0x6A, 0x00,  8 },
    { 0x6B, 0x00,  5 },
    { 0x6D, 0x00,  2 },
    { 0x70, 0x00,  1 },
    { 0x71, 0x00, 20 },
    { 0x75, 0x00, 12 },
    { 0x86, 0x00,  7 },
    { 0x88, 0x00,  4 },  /* ZELDA */
    { 0x8B, 0x00, 21 },
    { 0x8C, 0x00,  1 },
    { 0x90, 0x00,  6 },
    { 0x92, 0x00,  2 },
    { 0x95, 0x00, 11 },
    { 0x97, 0x00,  7 },
    { 0x99, 0x00,  5 },
    { 0x9A, 0x00, 14 },
    { 0x9C, 0x00, 10 },
    { 0x9D, 0x00,  4 },
    { 0xA2, 0x00,  2 },
    { 0xA5, 0x00,  7 },  /* common */
    { 0xA8, 0x00,  1 },
    { 0xAA, 0x00,  6 },
    { 0xB3, 0x00, 22 },
    { 0xB7, 0x00, 23 },
    { 0xBD, 0x00, 12 },
    { 0xC9, 0x00, 11 },
    { 0xCE, 0x00,  4 },
    { 0xD1, 0x00,  9 },
    { 0xDB, 0x00,  7 },
    { 0xE0, 0x00, 10 },
    { 0xE8, 0x00, 14 },
    { 0xF0, 0x00,  5 },
    { 0xF2, 0x00,  2 },
    { 0xF6, 0x00,  3 },
    { 0xF7, 0x00,  1 },
    { 0xFF, 0x00, 13 },
};

static void cram_write_pal(uint8_t *cram, uint8_t pal_idx, const Pal4 *p)
{
    for (int c = 0; c < 4; ++c)
    {
        size_t off = (size_t)pal_idx * 8 + (size_t)c * 2;
        cram[off]     = (uint8_t)(p->c[c] & 0xFF);
        cram[off + 1] = (uint8_t)(p->c[c] >> 8);
    }
}

static void gbc_init_palettes(MemoryState *mem)
{
    uint8_t cgb_flag = mem->memory.data[0x0143];

    /* ---------- Pure CGB or CGB-enhanced games: just init white default,
       the game will write its own palettes via BCPS/BCPD. ---------- */
    if (cgb_flag == 0x80 || cgb_flag == 0xC0)
    {
        mem->dmg_compat = false;

        /* Grayscale default so screen isn't black before game inits */
        static const Pal4 gs = {{ 0x7FFF, 0x5294, 0x294A, 0x0000 }};
        for (int p = 0; p < 8; ++p)
        {
            cram_write_pal(mem->bg_cram,  (uint8_t)p, &gs);
            cram_write_pal(mem->obj_cram, (uint8_t)p, &gs);
        }
        return;
    }

    /* ---------- DMG game on CGB: apply boot-ROM compatibility palette --- */
    mem->dmg_compat = true;

    /* Compute title checksum (CGB boot ROM method) */
    uint8_t checksum = 0;
    for (int i = 0x0134; i <= 0x0143; i++)
        checksum += mem->memory.data[i];

    /* Print title + checksum for debugging */
    char title[17] = {0};
    for (int i = 0; i < 16; i++)
    {
        uint8_t ch = mem->memory.data[0x0134 + i];
        title[i] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '.';
    }
    printf("DMG compat: title=\"%s\"  checksum=0x%02X\n", title, checksum);

    /* 4th title char for disambiguation */
    uint8_t title4 = mem->memory.data[0x0137];

    /* Lookup palette combo */
    int combo_id = 0; /* default = grayscale */
    for (size_t i = 0; i < sizeof(csum_table) / sizeof(csum_table[0]); ++i)
    {
        if (csum_table[i].csum == checksum)
        {
            if (csum_table[i].title4 == 0x00 || csum_table[i].title4 == title4)
            {
                combo_id = csum_table[i].combo;
                break;
            }
        }
    }

    const Pal4 *bg_p   = &bp[combos[combo_id].bg];
    const Pal4 *obj0_p = &bp[combos[combo_id].obj0];
    const Pal4 *obj1_p = &bp[combos[combo_id].obj1];

    printf("DMG compat: combo=%d  BG=pal%d  OBJ0=pal%d  OBJ1=pal%d\n",
           combo_id, combos[combo_id].bg, combos[combo_id].obj0, combos[combo_id].obj1);

    /* Store base palettes for BGP/OBP → CRAM remapping */
    memcpy(mem->dmg_bg_pal,   bg_p->c,   sizeof(mem->dmg_bg_pal));
    memcpy(mem->dmg_obj0_pal, obj0_p->c, sizeof(mem->dmg_obj0_pal));
    memcpy(mem->dmg_obj1_pal, obj1_p->c, sizeof(mem->dmg_obj1_pal));

    /* Write initial CRAM (identity mapping = BGP 0xE4, OBP 0xFF) */
    cram_write_pal(mem->bg_cram,  0, bg_p);
    cram_write_pal(mem->obj_cram, 0, obj0_p);
    cram_write_pal(mem->obj_cram, 1, obj1_p);

    /* Fill remaining palettes with same BG palette (some renderers need it) */
    for (int p = 1; p < 8; ++p)
        cram_write_pal(mem->bg_cram, (uint8_t)p, bg_p);
    for (int p = 2; p < 8; ++p)
        cram_write_pal(mem->obj_cram, (uint8_t)p, obj0_p);
}

/* -- Entry point ----------------------------------------------------------- */

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);  /* unbuffered so logs are visible */

    AppOptions options = {0};
    const char *rom_path = parse_arguments(argc, argv, &options);

    if (options.show_help)
    {
        print_usage(argc > 0 ? argv[0] : "gb_emu");
        return 0;
    }

    /* Init memory & load ROM */
    MemoryState memory;
    memory_init(&memory);
    memory_set_debug_print(&memory, options.debug_print);

    const char *primary_rom = rom_path ? rom_path : "Pokemon.gb";
    if (load_rom(primary_rom, &memory) != 0)
    {
        fprintf(stderr, "Failed to load ROM: %s\n", primary_rom);
        memory_shutdown(&memory);
        return 1;
    }
    printf("Loaded ROM: %s\n", primary_rom);

    /* GBC mode: explicit flag or CGB-capable cartridge header */
    if (options.gbc_mode || rom_requests_cgb(&memory))
    {
        memory.gbc_mode = true;
        if (options.gbc_mode)
            printf("Running in GBC (Color) mode.\n");
        else
            printf("Running in GBC (Color) mode (auto-detected from ROM header).\n");
    }

    /* Load BIOS */
    if (memory.gbc_mode)
    {

        if (load_bios("cgb_rom.bin", memory.bios, &memory.bios_size) == 0)
        {
            memory.bios_enabled = true;
            printf("BIOS loaded (CGB, %u bytes).\n", (unsigned)memory.bios_size);
        }
        else
        {
            memory.bios_enabled = false;
            printf("CGB BIOS (cgb_rom.bin) not found, using post-boot register state.\n");
        }
    }
    else
    {
        if (load_bios("dmg_rom.bin", memory.bios, &memory.bios_size) == 0)
        {
            memory.bios_enabled = true;
            printf("BIOS loaded (DMG, %u bytes).\n", (unsigned)memory.bios_size);
        }
        else
        {
            memory.bios_enabled = false;
            printf("BIOS not found, starting without BIOS.\n");
        }
    }

    /* Init CPU / PPU */
    CPUState cpu = {0};
    PPUState ppu = {0};

    cpu.memory = &memory;
    memory.cpu = &cpu;
    memory.ppu = &ppu;
    memory.ppu_synced_cycles = cpu.cycle_count;
    ppu.mem       = &memory.memory;
    ppu.mem_state = &memory;

    cpu_reset(&cpu);
    ppu_reset(&ppu, memory.bios_enabled);

    /* ── CGB palette initialization ───────────────────────────────── */
    if (memory.gbc_mode)
    {
        gbc_init_palettes(&memory);
    }

    /* Init display */
    DisplayContext display = {
        .enabled     = options.display_enabled,
        .initialized = false,
        .scale       = 3,
        .window      = NULL,
        .renderer    = NULL,
        .texture     = NULL};

    if (!display_init(&display))
    {
        memory_shutdown(&memory);
        return 1;
    }

    if (memory.gbc_mode && display.window)
        SDL_SetWindowTitle(display.window, "GB Emulator (GBC)");

    /* ── Audio (VirtuaAPU) ─────────────────────────────────────── */
    const uint32_t audio_sample_rate = 48000;
    SDL_AudioSpec audio_spec = {0};
    audio_spec.format   = SDL_AUDIO_S16;
    audio_spec.channels = 2;
    audio_spec.freq     = (int)audio_sample_rate;

    SDL_AudioStream *audio_stream = NULL;
    if (display.enabled)
    {
        audio_stream = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec, NULL, NULL);
        if (audio_stream)
        {
            SDL_ResumeAudioStreamDevice(audio_stream);
            printf("Audio initialized: %u Hz stereo\n", audio_sample_rate);
        }
        else
        {
            fprintf(stderr, "SDL audio init failed: %s\n", SDL_GetError());
        }
    }
    viruaapu_init(memory.gbc_mode ? 2 : 1, audio_sample_rate);

    /* Timing */
    const double base_mcycles_per_sec = 1048576.0 * options.speed_mult;

    uint64_t perf_freq = SDL_GetPerformanceFrequency();
    if (perf_freq == 0)
        perf_freq = 1;

    uint64_t throttle_epoch   = SDL_GetPerformanceCounter();
    uint64_t run_start_tick   = throttle_epoch;
    double   emulated_mcycles = 0.0;

    /* FPS diagnostics */
    uint64_t fps_interval_ticks   = perf_freq; /* 1 second */
    uint64_t fps_last_tick        = throttle_epoch;
    uint64_t fps_emu_frames       = 0;
    uint64_t fps_presented_frames = 0;
    uint64_t fps_mcycles          = 0;
    double   audio_sample_accum   = 0.0;

    /* Main loop */
    bool running = true;
    while (running)
    {
        if (!display_poll_events(&display, &memory))
            break;

        uint32_t cycles = cpu_execute_instruction(&cpu);
        ppu_step(&ppu, cycles);
        apu_advance(&memory, audio_stream, audio_sample_rate, cycles, &audio_sample_accum);

        emulated_mcycles += (double)cycles;

        /* Frame-rate throttle – double-speed mode doubles CPU clock rate */
        double target_mcycles_per_sec = memory.double_speed
            ? base_mcycles_per_sec * 2.0
            : base_mcycles_per_sec;

        uint64_t now          = SDL_GetPerformanceCounter();
        double   real_elapsed = (double)(now - throttle_epoch) / (double)perf_freq;
        double   emu_elapsed  = emulated_mcycles / target_mcycles_per_sec;
        bool run_deadline_reached = false;

        if (options.run_for_ms > 0)
        {
            uint64_t elapsed_ms = (uint64_t)(((now - run_start_tick) * 1000u) / perf_freq);
            run_deadline_reached = elapsed_ms >= options.run_for_ms;
        }

        if (emu_elapsed > real_elapsed)
        {
            double   ahead_s  = emu_elapsed - real_elapsed;
            uint32_t sleep_ms = (uint32_t)(ahead_s * 1000.0);
            if (sleep_ms > 1)
                SDL_Delay(sleep_ms - 1);

            while (((double)(SDL_GetPerformanceCounter() - throttle_epoch) /
                    (double)perf_freq) < emu_elapsed)
            { /* spin */ }
        }
        else if ((real_elapsed - emu_elapsed) > 0.25)
        {
            /* Resync if host stalled */
            throttle_epoch   = now;
            emulated_mcycles = 0.0;
        }

        if (run_deadline_reached && !ppu.frame_ready)
            running = false;

        if (ppu.frame_ready)
        {
            if (display.enabled)
            {
                display_present(&display, &ppu);
                if (options.fps_diag)
                    fps_presented_frames++;
            }

            if (options.fps_diag)
            {
                fps_emu_frames++;
                fps_mcycles += cycles;

                uint64_t elapsed_ticks = SDL_GetPerformanceCounter() - fps_last_tick;
                if (elapsed_ticks >= fps_interval_ticks)
                {
                    double elapsed_s     = (double)elapsed_ticks / (double)perf_freq;
                    double emu_fps       = (double)fps_emu_frames       / elapsed_s;
                    double present_fps   = (double)fps_presented_frames  / elapsed_s;
                    double mcycles_per_s = (double)fps_mcycles           / elapsed_s;
                    double speed_pct     = (mcycles_per_s / 1048576.0)  * 100.0;

                    printf("[FPS] emu=%.1f present=%.1f mcycles/s=%.0f speed=%.1f%%\n",
                           emu_fps, present_fps, mcycles_per_s, speed_pct);

                    fps_last_tick        = SDL_GetPerformanceCounter();
                    fps_emu_frames       = 0;
                    fps_presented_frames = 0;
                    fps_mcycles          = 0;
                }
            }

            if (run_deadline_reached)
                running = false;

            ppu.frame_ready = false;
        }
    }

    if (options.frame_dump_path)
    {
        if (!save_frame_bmp(options.frame_dump_path, &ppu))
            fprintf(stderr, "Failed to write frame dump: %s\n", options.frame_dump_path);
    }

    /* Cleanup */
    if (audio_stream)
    {
        SDL_DestroyAudioStream(audio_stream);
        audio_stream = NULL;
    }
    display_shutdown(&display);
    memory_shutdown(&memory);

    return 0;
}

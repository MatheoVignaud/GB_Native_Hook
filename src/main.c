#include "CPU.h"
#include "Memory.h"
#include "PPU.h"

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
    double speed_mult;
} AppOptions;

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

    if (SDL_Init(SDL_INIT_VIDEO) < 0)
    {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        display->enabled = false;
        return false;
    }
    display->initialized = true;

    display->window = SDL_CreateWindow("GB Emulator",
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
    if (!display->renderer)
    {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        display_shutdown(display);
        return false;
    }

    SDL_SetRenderVSync(display->renderer, true);
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

/* -- Entry point ----------------------------------------------------------- */

int main(int argc, char **argv)
{
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

    if (load_bios("dmg_rom.bin", memory.bios) == 0)
    {
        memory.bios_enabled = true;
        printf("BIOS loaded.\n");
    }
    else
    {
        memory.bios_enabled = false;
        printf("BIOS not found, starting without BIOS.\n");
    }

    const char *primary_rom = rom_path ? rom_path : "Pokemon.gb";
    if (load_rom(primary_rom, &memory) != 0)
    {
        fprintf(stderr, "Failed to load ROM: %s\n", primary_rom);
        memory_shutdown(&memory);
        return 1;
    }
    printf("Loaded ROM: %s\n", primary_rom);

    /* Init CPU / PPU */
    CPUState cpu = {0};
    PPUState ppu = {0};

    cpu.memory = &memory;
    memory.cpu = &cpu;
    ppu.mem    = &memory.memory;

    cpu_reset(&cpu);
    ppu_reset(&ppu, memory.bios_enabled);

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

    /* Timing */
    const double target_mcycles_per_sec = 1048576.0 * options.speed_mult;

    uint64_t perf_freq = SDL_GetPerformanceFrequency();
    if (perf_freq == 0)
        perf_freq = 1;

    uint64_t throttle_epoch   = SDL_GetPerformanceCounter();
    double   emulated_mcycles = 0.0;

    /* FPS diagnostics */
    uint64_t fps_interval_ticks   = perf_freq; /* 1 second */
    uint64_t fps_last_tick        = throttle_epoch;
    uint64_t fps_emu_frames       = 0;
    uint64_t fps_presented_frames = 0;
    uint64_t fps_mcycles          = 0;

    /* Main loop */
    bool running = true;
    while (running)
    {
        uint32_t cycles = cpu_execute_instruction(&cpu);
        ppu_step(&ppu, cycles);

        emulated_mcycles += (double)cycles;

        /* Frame-rate throttle */
        uint64_t now          = SDL_GetPerformanceCounter();
        double   real_elapsed = (double)(now - throttle_epoch) / (double)perf_freq;
        double   emu_elapsed  = emulated_mcycles / target_mcycles_per_sec;

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

        if (ppu.frame_ready)
        {
            if (!display_poll_events(&display, &memory))
                running = false;

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

            ppu.frame_ready = false;
        }
    }

    /* Cleanup */
    display_shutdown(&display);
    memory_shutdown(&memory);

    return 0;
}
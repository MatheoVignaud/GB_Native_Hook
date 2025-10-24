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

    display->scale = 3;
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
    {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        display->enabled = false;
        return false;
    }

    display->initialized = true;

    display->window = SDL_CreateWindow("GB Native Hook",
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
    case SDLK_LCTRL:
        memory_set_button_state(memory, JOYPAD_A, pressed);
        break;
    case SDLK_X:
    case SDLK_LALT:
        memory_set_button_state(memory, JOYPAD_B, pressed);
        break;
    case SDLK_BACKSPACE:
    case SDLK_RSHIFT:
        memory_set_button_state(memory, JOYPAD_SELECT, pressed);
        break;
    case SDLK_RETURN:
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
            {
                return false;
            }
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

    if (SDL_UpdateTexture(display->texture, NULL, ppu->fb, GB_SCREEN_WIDTH * (int)sizeof(uint32_t)) < 0)
    {
        fprintf(stderr, "SDL_UpdateTexture failed: %s\n", SDL_GetError());
        return;
    }

    SDL_RenderClear(display->renderer);
    SDL_FRect dst = {
        0.0f,
        0.0f,
        (float)(GB_SCREEN_WIDTH * display->scale),
        (float)(GB_SCREEN_HEIGHT * display->scale)};
    SDL_RenderTexture(display->renderer, display->texture, NULL, &dst);
    SDL_RenderPresent(display->renderer);
}

static const char *parse_arguments(int argc, char **argv, bool *display_enabled)
{
    const char *rom_path = NULL;
    *display_enabled = true;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--no-display") == 0)
        {
            *display_enabled = false;
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

int main(int argc, char **argv)
{
    MemoryState memory;
    memory_init(&memory);

    bool display_enabled = true;
    const char *rom_path = parse_arguments(argc, argv, &display_enabled);

    if (load_bios("dmg_rom.bin", memory.bios) == 0)
    {
        memory.bios_enabled = true;
        printf("BIOS loaded and enabled.\n");
    }
    else
    {
        memory.bios_enabled = false;
        printf("BIOS not found, starting without BIOS.\n");
    }

    const char *primary_rom = rom_path ? rom_path : "tetris.gb";
    if (load_rom(primary_rom, memory.memory.data) != 0)
    {
        fprintf(stderr, "Failed to load ROM: %s\n", primary_rom);
        if (strcmp(primary_rom, "tetris.gb") != 0 && load_rom("tetris.gb", memory.memory.data) == 0)
        {
            printf("Loaded default ROM: tetris.gb\n");
        }
        else
        {
            fprintf(stderr, "Failed to load default ROM: tetris.gb\n");
            return 1;
        }
    }
    else
    {
        printf("Loaded ROM: %s\n", primary_rom);
    }

    CPUState cpu = {0};
    PPUState ppu = {0};
    cpu.memory = &memory;
    memory.cpu = &cpu;
    ppu.mem = &memory.memory;
    cpu_reset(&cpu);
    ppu_reset(&ppu);

    DisplayContext display = {
        .enabled = display_enabled,
        .initialized = false,
        .scale = 3,
        .window = NULL,
        .renderer = NULL,
        .texture = NULL};

    display_init(&display);

    bool running = true;
    while (running)
    {
        uint32_t cycles = cpu_execute_instruction(&cpu);
        ppu_step(&ppu, cycles);

        if (!display_poll_events(&display, &memory))
        {
            running = false;
        }

        if (ppu.frame_ready)
        {
            if (display.enabled)
            {
                display_present(&display, &ppu);
            }
            ppu.frame_ready = false;
        }
    }

    display_shutdown(&display);

    return 0;
}

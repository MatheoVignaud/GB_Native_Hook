#ifndef PPU
#define PPU

#include "CPU.h"
#include "Memory.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

// Timings DMG (voir PanDocs) : 456 dots/ligne, 154 lignes
#define GB_SCREEN_WIDTH 160
#define GB_SCREEN_HEIGHT 144

#define DOTS_PER_SCANLINE 456
#define SCANLINES_PER_FRAME 154
#define VBLANK_SCANLINE_START 144
#define VBLANK_SCANLINE_END 153

#define DOTS_PER_CPU_CYCLE 4

#define STAT_LYC_FLAG 0x04
#define STAT_MODE_MASK 0x03
#define STAT_MODE_0 0x00
#define STAT_MODE_1 0x01
#define STAT_MODE_2 0x02
#define STAT_MODE_3 0x03

typedef struct
{
    Memory *mem;
    uint32_t dots;
    bool frame_ready;
    uint32_t fb[GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT];

    uint8_t last_mode;
    bool lcd_enabled;
    bool lyc_match;
    uint8_t sprite_count;
    struct
    {
        uint8_t x;
        uint8_t tile;
        uint8_t attributes;
        uint8_t line;
    } scanline_sprites[10];
} PPUState;

void ppu_reset(PPUState *ppu);
void ppu_step(PPUState *ppu, uint32_t cpu_cycles);

#endif

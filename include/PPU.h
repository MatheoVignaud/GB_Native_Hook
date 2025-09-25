#ifndef PPU
#define PPU

#include <assert.h>
#include <cpu.h>
#include <memory.h>
#include <stdbool.h>
#include <stdio.h>

// https://hacktix.github.io/GBEDG/ppu/

#define CYCLE_PER_DOT 4

#define LY 0xFF44   // LCDC Y Coordinate Register
#define LCDC 0xFF40 // LCD Control Register

typedef struct
{
    Memory *memory;       // Pointer to the memory structure
    uint8_t mode;         // Current PPU mode (0, 1, 2, or 3)
    uint64_t cycle_count; // Cycle count for performance tracking
    uint16_t mode_3_dots; // Dots in mode 3
} PPUState;

typedef struct PPU
{
    union
    {
        struct
        {
            uint8_t color : 2;               // Color value (0-3)
            uint8_t pal : 2;                 // Palette value (0-3)
            uint8_t unused : 2;              // Unused bits
            uint8_t background_priority : 2; // Background priority (0-3)
        };
        uint8_t data;
    };
} FIFO_data;

uint8_t lcd[(160 * 144 * 2) / 8]; // LCD buffer for rendering

inline uint8_t lcd_get(uint8_t x, uint8_t y)
{
    assert(x >= 0 && x < 160);
    assert(y >= 0 && y < 144);

    int index = y * 160 + x;
    int bit_pos = index * 2;
    int byte_idx = bit_pos >> 3;
    int bit_offset = bit_pos & 0x07;

    uint8_t byte = lcd[byte_idx];
    uint8_t value = (byte >> bit_offset) & 0x3;
    return value;
};

inline void lcd_set(uint8_t x, uint8_t y, uint8_t value)
{
    assert(x >= 0 && x < 160);
    assert(y >= 0 && y < 144);
    assert(value < 4);

    int index = y * 160 + x;
    int bit_pos = index * 2;
    int byte_idx = bit_pos >> 3;
    int bit_offset = bit_pos & 0x07;
    lcd[byte_idx] &= (uint8_t)~(0x3 << bit_offset);
    lcd[byte_idx] |= (uint8_t)((value & 0x3) << bit_offset);
};

static inline bool lcdc_get_flag(Memory *memory, uint8_t flag)
{
    uint8_t mask = 0x01 << flag;
    if (flag > 7)
    {
        fprintf(stderr, "Error: Invalid flag %d for LCDC register.\n", flag);
        return false; // Invalid flag
    }
    return (memory->data[LCDC] & mask) != 0;
}

static inline void lcdc_set_flag(Memory *memory, uint8_t flag, bool value)
{
    uint8_t mask = 0x01 << flag;
    if (flag > 7)
    {
        fprintf(stderr, "Error: Invalid flag %d for LCDC register.\n", flag);
        return; // Invalid flag
    }
    if (value)
    {
        memory->data[LCDC] |= mask;
    }
    else
    {
        memory->data[LCDC] &= ~mask;
    }
}

void execute_ppu(PPUState *ppu);

void ppu_reset(PPUState *ppu);

void ppu_mode_0(PPUState *ppu);
void ppu_mode_1(PPUState *ppu);
void ppu_mode_2(PPUState *ppu);
void ppu_mode_3(PPUState *ppu);

#endif
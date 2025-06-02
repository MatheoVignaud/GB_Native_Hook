#include "PPU.h"

void execute_ppu(PPUState *ppu)
{
    switch (ppu->mode)
    {
    case 0:
        if (ppu->memory->data[LY] < 144) // If in VBlank
        {
            ppu_mode_2(ppu);
        }
        else
        {
            ppu_mode_1(ppu);
        }
        break;
    case 2:
        ppu_mode_3(ppu);
        break;
    case 3:
        ppu_mode_0(ppu);
        break;
    default:
        printf("Unknown PPU mode: %d\n", ppu->mode);
        break;
    }
}

void ppu_mode_0(PPUState *ppu)
{
    ppu->mode = 0;
    uint16_t dots_to_add = 376 - ppu->mode_3_dots;
    ppu->cycle_count += dots_to_add * CYCLE_PER_DOT;
    ppu->memory->data[LY] += 1;
}

void ppu_mode_1(PPUState *ppu)
{
    ppu->mode = 0; // vBlank mode is like hblank mode
    ppu->cycle_count += 456 * CYCLE_PER_DOT;
    ppu->memory->data[LY] += 1;
    if (ppu->memory->data[LY] > 153) // If we reach the end of the frame
    {
        ppu->memory->data[LY] = 0; // Reset LY to 0
    }
}

void ppu_mode_2(PPUState *ppu)
{
    ppu->cycle_count += 80 * CYCLE_PER_DOT;
    ppu->mode = 2;
}

void ppu_mode_3(PPUState *ppu)
{
    ppu->mode = 3;

    uint8_t ly = ppu->memory->data[LY];

    uint8_t oa_to_draw[10] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    uint8_t oa_count = 0;
    for (uint8_t x = 0; x < 0xA0; x += 4)
    {
        uint8_t y = ppu->memory->oam[x];
        bool lcdc2 = lcdc_get_flag(ppu->memory, 2);
        uint8_t offset = lcdc2 ? 16 : 8;

        if (y >= ly + 16 && y <= ly + 16 + offset)
        {
            oa_to_draw[oa_count] = x / 4;
            oa_count++;
        }
        else
        {
            continue; // Skip this OAM entry
        }
        if (oa_count >= 10)
        {
            break; // Limit to 10 OAM entries
        }
    }

    uint8_t oam_fifo_count = 0;
    FIFO_data oam_fifo[10];
    uint8_t bg_fifo_count = 0;
    FIFO_data bg_fifo[10];
}

#include "PPU.h"

void ppu_reset(PPUState *ppu)
{
    assert(ppu);
    // Valeurs proches du boot DMG
    ppu->mem->LCDC = 0x91; // LCD ON, BG ON, BG Map 9800, tiles @8000, Sprites ON (8x8)
    ppu->mem->STAT = 0x85;
    ppu->mem->SCY = 0x00;
    ppu->mem->SCX = 0x00;
    ppu->mem->LY = 0x00;
    ppu->mem->LYC = 0x00;
    ppu->mem->DMA = 0xFF;
    ppu->mem->BGP = 0xFC; // 11 11 00 => palette par défaut
    ppu->mem->OBP0 = 0xFF;
    ppu->mem->OBP1 = 0xFF;
    ppu->mem->WY = 0x00;
    ppu->mem->WX = 0x00;

    ppu->dots = 0;
    ppu->frame_ready = false;
    ppu->last_mode = STAT_MODE_0;
}

#define LCDC_ENABLE (1 << 7)
#define LCDC_WINDOW_TILE_MAP (1 << 6)
#define LCDC_WINDOW_ENABLE (1 << 5)
#define LCDC_BG_WINDOW_TILE_DATA (1 << 4)
#define LCDC_BG_TILE_MAP (1 << 3)
#define LCDC_OBJ_SIZE (1 << 2)
#define LCDC_OBJ_ENABLE (1 << 1)
#define LCDC_BG_ENABLE (1 << 0)

void ppu_step(PPUState *ppu, uint32_t cpu_cycles)
{
    if ((ppu->mem->LCDC & LCDC_ENABLE) == 0) // check bit 7
    {
        ppu->mem->LY = 0;
        return;
    }

    uint32_t dots_to_advance = cpu_cycles * DOTS_PER_CPU_CYCLE;

    for (uint32_t i = 0; i < dots_to_advance; ++i)
    {
        ppu->dots++;
        if (ppu->dots >= DOTS_PER_SCANLINE)
        {
            ppu->dots = 0;
            ppu->mem->LY++;
            if (ppu->mem->LY > VBLANK_SCANLINE_END)
            {
                ppu->mem->LY = 0;
                ppu->frame_ready = true;
            }
        }

        // STAT
        uint8_t mode;
        if (ppu->mem->LY >= VBLANK_SCANLINE_START)
        {
            mode = STAT_MODE_1;
        }
        else if (ppu->dots < 80)
        {
            mode = STAT_MODE_2;
        }
        else if (ppu->dots < 252)
        {
            mode = STAT_MODE_3;
        }
        else
        {
            mode = STAT_MODE_0;
        }

        ppu->mem->STAT = (ppu->mem->STAT & ~STAT_MODE_MASK) | mode;

        // LYC=LY
        if (ppu->mem->LY == ppu->mem->LYC)
        {
            ppu->mem->STAT |= STAT_LYC_FLAG;
            if (ppu->mem->STAT & (1 << 6))
            {
                ppu->mem->IF |= IF_LCDSTAT;
            }
        }
        else
        {
            ppu->mem->STAT &= ~STAT_LYC_FLAG;
        }

        // VBLANK
        if (mode == STAT_MODE_1 && ppu->last_mode != STAT_MODE_1)
        {
            ppu->mem->IF |= IF_VBLANK;
        }

        ppu->last_mode = mode;
    }
}

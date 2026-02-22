#include "PPU.h"
#include "ViruaPPUBridge.h"

#include <stdlib.h>
#include <string.h>

#define LCDC_ENABLE (1 << 7)
#define LCDC_WINDOW_TILE_MAP (1 << 6)
#define LCDC_WINDOW_ENABLE (1 << 5)
#define LCDC_BG_WINDOW_TILE_DATA (1 << 4)
#define LCDC_BG_TILE_MAP (1 << 3)
#define LCDC_OBJ_SIZE (1 << 2)
#define LCDC_OBJ_ENABLE (1 << 1)
#define LCDC_BG_ENABLE (1 << 0)

static const uint32_t dmg_palette[4] = {
    0xFF9BBC0F, // Lightest
    0xFF8BAC0F,
    0xFF306230,
    0xFF0F380F // Darkest
};

typedef struct
{
    uint8_t x;
    uint8_t tile;
    uint8_t attributes;
    uint8_t line;
    uint8_t index;
} SpriteCandidate;

static void ppu_update_lyc(PPUState *ppu);
static void ppu_set_mode(PPUState *ppu, uint8_t mode);
static void ppu_lcd_off(PPUState *ppu);
static void ppu_clear_framebuffer(PPUState *ppu);
static void ppu_eval_sprites(PPUState *ppu);
static void ppu_render_pixel(PPUState *ppu, uint8_t x, uint8_t y);
static void ppu_render_frame_with_viruappu(PPUState *ppu);

static FILE *ppu_trace_file = NULL;
static bool ppu_trace_enabled = false;
static bool ppu_trace_inited = false;
static bool ppu_vblank_log_enabled = false;
static bool ppu_legacy_scanline_renderer = false;
static uint32_t ppu_last_hash = 0;
static uint32_t ppu_same_hash_count = 0;
static uint64_t ppu_frame_counter = 0;

static void ppu_trace_init(void)
{
    if (ppu_trace_inited)
        return;
    ppu_trace_inited = true;

    const char *vblank_log_env = getenv("GB_LOG_PPU_VBLANK");
    if (vblank_log_env && vblank_log_env[0] != '\0' && vblank_log_env[0] != '0')
    {
        ppu_vblank_log_enabled = true;
    }

    const char *legacy_scanline_env = getenv("GB_PPU_LEGACY_SCANLINE");
    if (legacy_scanline_env && legacy_scanline_env[0] != '\0' && legacy_scanline_env[0] != '0')
    {
        ppu_legacy_scanline_renderer = true;
    }

    const char *trace_env = getenv("GB_TRACE_PPU");
    if (!trace_env || trace_env[0] == '\0' || trace_env[0] == '0')
        return;

    ppu_trace_file = fopen("ppu_trace.log", "w");
    if (!ppu_trace_file)
        return;

    ppu_trace_enabled = true;
    fprintf(ppu_trace_file, "PPU trace enabled\n");
    fflush(ppu_trace_file);
}

static uint32_t ppu_frame_hash(const uint32_t *pixels, size_t count)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < count; ++i)
    {
        h ^= pixels[i];
        h *= 16777619u;
    }
    return h;
}

static inline uint8_t ppu_vram_read(const PPUState *ppu, uint16_t addr)
{
    assert(addr >= 0x8000 && addr < 0xA000);
    return ppu->mem->vram[addr - 0x8000];
}

static inline uint32_t ppu_palette_color(uint8_t palette, uint8_t color_id)
{
    uint8_t shade = (palette >> (color_id * 2)) & 0x03;
    return dmg_palette[shade];
}

static void ppu_clear_framebuffer(PPUState *ppu)
{
    uint32_t color = ppu_palette_color(ppu->mem->BGP, 0);
    for (size_t i = 0; i < GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT; ++i)
    {
        ppu->fb[i] = color;
    }
}

static void ppu_render_frame_with_viruappu(PPUState *ppu)
{
    GBPPURegisters regs = {
        .LCDC = ppu->mem->LCDC,
        .SCY = ppu->mem->SCY,
        .SCX = ppu->mem->SCX,
        .BGP = ppu->mem->BGP,
        .OBP0 = ppu->mem->OBP0,
        .OBP1 = ppu->mem->OBP1,
        .WY = ppu->mem->WY,
        .WX = ppu->mem->WX,
    };

    viruappu_render_gb_frame(ppu->mem->vram,
                             ppu->mem->oam,
                             &regs,
                             ppu->fb,
                             GB_SCREEN_WIDTH);

    if (ppu_trace_enabled && ppu_trace_file)
    {
        ppu_frame_counter++;
        uint32_t h = ppu_frame_hash(ppu->fb, GB_SCREEN_WIDTH * GB_SCREEN_HEIGHT);
        if (h == ppu_last_hash)
            ppu_same_hash_count++;
        else
            ppu_same_hash_count = 0;
        ppu_last_hash = h;

        if (ppu_frame_counter <= 600 || ppu_same_hash_count == 600 || (ppu_frame_counter % 300) == 0)
        {
            fprintf(ppu_trace_file,
                    "FRAME idx=%llu hash=%08X same=%u LCDC=%02X LY=%02X IF=%02X IE=%02X\n",
                    (unsigned long long)ppu_frame_counter,
                    h,
                    ppu_same_hash_count,
                    ppu->mem->LCDC,
                    ppu->mem->LY,
                    ppu->mem->IF,
                    ppu->mem->IE);
            fflush(ppu_trace_file);
        }
    }
}

static void ppu_eval_sprites(PPUState *ppu)
{
    ppu->sprite_count = 0;

    if ((ppu->mem->LCDC & LCDC_OBJ_ENABLE) == 0)
        return;
    if (ppu->mem->LY >= GB_SCREEN_HEIGHT)
        return;

    uint8_t sprite_height = (ppu->mem->LCDC & LCDC_OBJ_SIZE) ? 16 : 8;
    const uint8_t *oam = ppu->mem->oam;
    SpriteCandidate candidates[40];
    uint8_t candidate_count = 0;

    for (uint8_t i = 0; i < 40; ++i)
    {
        uint8_t y = oam[i * 4];
        uint8_t x = oam[i * 4 + 1];
        uint8_t tile = oam[i * 4 + 2];
        uint8_t attr = oam[i * 4 + 3];

        int sprite_y = (int)y - 16;
        if ((int)ppu->mem->LY < sprite_y || (int)ppu->mem->LY >= sprite_y + sprite_height)
            continue;
        if (x == 0 || x >= 168)
            continue;

        SpriteCandidate sc;
        sc.x = x;
        sc.tile = tile;
        sc.attributes = attr;
        uint8_t line = (uint8_t)(ppu->mem->LY - sprite_y);
        if (attr & 0x40)
        {
            line = (uint8_t)((sprite_height - 1) - line);
        }
        sc.line = line;
        sc.index = i;

        candidates[candidate_count++] = sc;
    }

    if (candidate_count == 0)
        return;

    for (uint8_t i = 1; i < candidate_count; ++i)
    {
        SpriteCandidate key = candidates[i];
        int j = i - 1;
        while (j >= 0 &&
               (candidates[j].x > key.x ||
                (candidates[j].x == key.x && candidates[j].index > key.index)))
        {
            candidates[j + 1] = candidates[j];
            --j;
        }
        candidates[j + 1] = key;
    }

    uint8_t limit = candidate_count > 10 ? 10 : candidate_count;
    for (uint8_t i = 0; i < limit; ++i)
    {
        ppu->scanline_sprites[i].x = candidates[i].x;
        ppu->scanline_sprites[i].tile = candidates[i].tile;
        ppu->scanline_sprites[i].attributes = candidates[i].attributes;
        ppu->scanline_sprites[i].line = candidates[i].line;
    }
    ppu->sprite_count = limit;
}

void ppu_reset(PPUState *ppu, bool bios_enabled)
{
    assert(ppu);
    ppu_trace_init();

    if (bios_enabled)
    {
        // Power-on style state while boot ROM runs.
        ppu->mem->LCDC = 0x00;
        ppu->mem->STAT = 0x00;
        ppu->mem->SCY = 0x00;
        ppu->mem->SCX = 0x00;
        ppu->mem->LY = 0x00;
        ppu->mem->LYC = 0x00;
        ppu->mem->DMA = 0xFF;
        ppu->mem->BGP = 0x00;
        ppu->mem->OBP0 = 0x00;
        ppu->mem->OBP1 = 0x00;
        ppu->mem->WY = 0x00;
        ppu->mem->WX = 0x00;
    }
    else
    {
        // Post-boot defaults when starting without BIOS.
        ppu->mem->LCDC = 0x91; // LCD ON, BG ON, BG Map 9800, tiles @8000, Sprites ON (8x8)
        ppu->mem->STAT = 0x85;
        ppu->mem->SCY = 0x00;
        ppu->mem->SCX = 0x00;
        ppu->mem->LY = 0x00;
        ppu->mem->LYC = 0x00;
        ppu->mem->DMA = 0xFF;
        ppu->mem->BGP = 0xFC;
        ppu->mem->OBP0 = 0xFF;
        ppu->mem->OBP1 = 0xFF;
        ppu->mem->WY = 0x00;
        ppu->mem->WX = 0x00;
    }

    ppu->dots = 0;
    ppu->frame_ready = false;
    ppu->last_mode = STAT_MODE_0;
    ppu->lcd_enabled = (ppu->mem->LCDC & LCDC_ENABLE) != 0;
    ppu->lyc_match = false;
    ppu->sprite_count = 0;
    ppu_clear_framebuffer(ppu);
    ppu_update_lyc(ppu);
}

static void ppu_update_lyc(PPUState *ppu)
{
    bool match = (ppu->mem->LY == ppu->mem->LYC);
    if (match)
    {
        ppu->mem->STAT |= STAT_LYC_FLAG;
        if (!ppu->lyc_match && ppu->lcd_enabled && (ppu->mem->STAT & (1 << 6)))
        {
            ppu->mem->IF |= IF_LCDSTAT;
        }
        ppu->lyc_match = true;
    }
    else
    {
        ppu->mem->STAT &= (uint8_t)~STAT_LYC_FLAG;
        ppu->lyc_match = false;
    }
}

static void ppu_set_mode(PPUState *ppu, uint8_t mode)
{
    uint8_t stat = ppu->mem->STAT;
    uint8_t prev_mode = ppu->last_mode;

    stat = (uint8_t)((stat & ~STAT_MODE_MASK) | mode);
    ppu->mem->STAT = stat;

    if (mode != prev_mode)
    {
        switch (mode)
        {
        case STAT_MODE_0:
            if (stat & (1 << 3))
            {
                ppu->mem->IF |= IF_LCDSTAT;
            }
            break;
        case STAT_MODE_1:
        {
            static uint64_t vblank_counter = 0;
            vblank_counter++;
            if (ppu_vblank_log_enabled && (vblank_counter <= 3 || (vblank_counter % 100 == 0)))
            {
                printf("[PPU] Enter VBlank #%llu: LY=%02X IF(before)=%02X LCDC=%02X\n",
                       (unsigned long long)vblank_counter,
                       ppu->mem->LY,
                       ppu->mem->IF,
                       ppu->mem->LCDC);
            }
            if (prev_mode != STAT_MODE_1)
            {
                ppu->mem->IF |= IF_VBLANK;
            }
            if (stat & (1 << 4))
            {
                ppu->mem->IF |= IF_LCDSTAT;
            }
            break;
        }
        case STAT_MODE_2:
            if (stat & (1 << 5))
            {
                ppu->mem->IF |= IF_LCDSTAT;
            }
            break;
        default:
            break;
        }
    }

    ppu->last_mode = mode;
}

static uint8_t ppu_fetch_tile_color(PPUState *ppu, uint16_t tile_map_base, uint16_t tile_data_base, bool signed_indexing, uint8_t x, uint8_t y)
{
    uint8_t tile_x = x / 8;
    uint8_t tile_y = y / 8;
    uint16_t map_index = (uint16_t)(tile_y * 32 + tile_x);
    uint16_t map_addr = (uint16_t)(tile_map_base + map_index);
    uint8_t tile_index = ppu_vram_read(ppu, map_addr);

    int32_t tile_id;
    if (signed_indexing)
    {
        tile_id = (int8_t)tile_index;
    }
    else
    {
        tile_id = tile_index;
    }

    uint16_t tile_addr = (uint16_t)(tile_data_base + (tile_id * 16));
    uint8_t row = y % 8;
    uint16_t tile_row_addr = (uint16_t)(tile_addr + row * 2);
    uint8_t low = ppu_vram_read(ppu, tile_row_addr);
    uint8_t high = ppu_vram_read(ppu, tile_row_addr + 1);
    uint8_t col = x % 8;
    uint8_t bit = (uint8_t)(7 - col);

    uint8_t color_id = (uint8_t)(((high >> bit) & 0x01) << 1 | ((low >> bit) & 0x01));
    return color_id;
}

static void ppu_render_pixel(PPUState *ppu, uint8_t x, uint8_t y)
{
    if (y >= GB_SCREEN_HEIGHT || x >= GB_SCREEN_WIDTH)
    {
        return;
    }

    uint8_t lcdc = ppu->mem->LCDC;
    uint8_t bg_color_id = 0;
    uint32_t bg_color = ppu_palette_color(ppu->mem->BGP, 0);

    if (lcdc & LCDC_BG_ENABLE)
    {
        uint16_t tile_map_base = (uint16_t)((lcdc & LCDC_BG_TILE_MAP) ? 0x9C00 : 0x9800);
        uint16_t tile_data_base = (uint16_t)((lcdc & LCDC_BG_WINDOW_TILE_DATA) ? 0x8000 : 0x9000);
        bool signed_indexing = (lcdc & LCDC_BG_WINDOW_TILE_DATA) == 0;

        uint8_t scx = ppu->mem->SCX;
        uint8_t scy = ppu->mem->SCY;
        uint8_t bg_x = (uint8_t)(x + scx);
        uint8_t bg_y = (uint8_t)(y + scy);

        bg_color_id = ppu_fetch_tile_color(ppu, tile_map_base, tile_data_base, signed_indexing, bg_x, bg_y);

        bool window_enabled = (lcdc & LCDC_WINDOW_ENABLE) != 0;
        if (window_enabled && ppu->mem->WY <= y)
        {
            uint8_t wx = (ppu->mem->WX > 7) ? (uint8_t)(ppu->mem->WX - 7) : 0;
            if (x >= wx && ppu->mem->WX <= 166)
            {
                uint8_t window_x = (uint8_t)(x - wx);
                uint8_t window_y = (uint8_t)(y - ppu->mem->WY);
                uint16_t window_map_base = (uint16_t)((lcdc & LCDC_WINDOW_TILE_MAP) ? 0x9C00 : 0x9800);
                bg_color_id = ppu_fetch_tile_color(ppu, window_map_base, tile_data_base, signed_indexing, window_x, window_y);
            }
        }

        bg_color = ppu_palette_color(ppu->mem->BGP, bg_color_id);
    }

    uint32_t final_color = bg_color;

    if ((lcdc & LCDC_OBJ_ENABLE) && ppu->sprite_count > 0)
    {
        uint8_t sprite_height = (lcdc & LCDC_OBJ_SIZE) ? 16 : 8;

        for (uint8_t i = 0; i < ppu->sprite_count; ++i)
        {
            const uint8_t sprite_x = ppu->scanline_sprites[i].x;
            int screen_x = (int)sprite_x - 8;
            if (x < screen_x || x >= screen_x + 8)
                continue;

            uint8_t pixel_x = (uint8_t)(x - screen_x);
            uint8_t attributes = ppu->scanline_sprites[i].attributes;
            if (attributes & 0x20)
            {
                pixel_x = (uint8_t)(7 - pixel_x);
            }

            uint8_t tile_index = ppu->scanline_sprites[i].tile;
            uint8_t line = ppu->scanline_sprites[i].line;
            if (sprite_height == 16)
            {
                tile_index = (uint8_t)((tile_index & 0xFE) | (line >= 8));
                line &= 0x07;
            }

            uint16_t tile_addr = (uint16_t)(0x8000 + tile_index * 16);
            uint16_t row_addr = (uint16_t)(tile_addr + line * 2);
            uint8_t low = ppu_vram_read(ppu, row_addr);
            uint8_t high = ppu_vram_read(ppu, row_addr + 1);
            uint8_t bit = (uint8_t)(7 - pixel_x);
            uint8_t color_id = (uint8_t)(((high >> bit) & 0x01) << 1 | ((low >> bit) & 0x01));
            if (color_id == 0)
            {
                continue;
            }

            uint8_t palette = (attributes & 0x10) ? ppu->mem->OBP1 : ppu->mem->OBP0;
            uint32_t sprite_color = ppu_palette_color(palette, color_id);
            if ((attributes & 0x80) && bg_color_id != 0)
            {
                final_color = bg_color;
            }
            else
            {
                final_color = sprite_color;
            }
            break;
        }
    }

    ppu->fb[y * GB_SCREEN_WIDTH + x] = final_color;
}

static void ppu_lcd_off(PPUState *ppu)
{
    if (!ppu->lcd_enabled)
    {
        ppu->mem->LY = 0;
        ppu->mem->STAT = (uint8_t)((ppu->mem->STAT & ~STAT_MODE_MASK) | STAT_MODE_0);
        ppu_update_lyc(ppu);
        return;
    }

    ppu->lcd_enabled = false;
    ppu->dots = 0;
    ppu->mem->LY = 0;
    ppu->frame_ready = false;
    ppu->last_mode = STAT_MODE_0;
    ppu->lyc_match = false;
    ppu->sprite_count = 0;
    ppu_clear_framebuffer(ppu);
    ppu->mem->STAT = (uint8_t)((ppu->mem->STAT & ~STAT_MODE_MASK) | STAT_MODE_0);
    ppu_update_lyc(ppu);
}

static inline uint8_t ppu_current_mode(const PPUState *ppu)
{
    if (ppu->mem->LY >= VBLANK_SCANLINE_START)
        return STAT_MODE_1;
    if (ppu->dots < 80)
        return STAT_MODE_2;
    if (ppu->dots < (80 + 172))
        return STAT_MODE_3;
    return STAT_MODE_0;
}

static inline uint32_t ppu_dots_until_boundary(const PPUState *ppu, uint8_t mode)
{
    switch (mode)
    {
    case STAT_MODE_2:
        return 80u - ppu->dots;
    case STAT_MODE_3:
        return (80u + 172u) - ppu->dots;
    case STAT_MODE_0:
    case STAT_MODE_1:
    default:
        return DOTS_PER_SCANLINE - ppu->dots;
    }
}

void ppu_step(PPUState *ppu, uint32_t cpu_cycles)
{
    if ((ppu->mem->LCDC & LCDC_ENABLE) == 0)
    {
        ppu_lcd_off(ppu);
        return;
    }

    if (!ppu->lcd_enabled)
    {
        ppu->lcd_enabled = true;
        ppu->dots = 0;
        ppu->mem->LY = 0;
        ppu->frame_ready = false;
        ppu->last_mode = STAT_MODE_0;
        ppu->lyc_match = false;
        ppu_clear_framebuffer(ppu);
        ppu_update_lyc(ppu);
    }

    uint32_t dots_to_advance = cpu_cycles * DOTS_PER_CPU_CYCLE;
    ppu_update_lyc(ppu);

    while (dots_to_advance > 0)
    {
        if (ppu->dots == 0 && ppu_legacy_scanline_renderer)
        {
            if (ppu->mem->LY < GB_SCREEN_HEIGHT)
            {
                ppu_eval_sprites(ppu);
            }
            else
            {
                ppu->sprite_count = 0;
            }
        }

        uint8_t mode = ppu_current_mode(ppu);

        ppu_set_mode(ppu, mode);
        uint32_t advance = ppu_dots_until_boundary(ppu, mode);
        if (advance > dots_to_advance)
        {
            advance = dots_to_advance;
        }

        if (ppu_legacy_scanline_renderer && mode == STAT_MODE_3)
        {
            uint32_t start_dot = ppu->dots;
            uint32_t end_dot = ppu->dots + advance;
            if (end_dot > 80u && ppu->mem->LY < GB_SCREEN_HEIGHT)
            {
                uint32_t start_px = (start_dot > 80u) ? (start_dot - 80u) : 0u;
                uint32_t end_px = end_dot - 80u;
                if (end_px > GB_SCREEN_WIDTH)
                {
                    end_px = GB_SCREEN_WIDTH;
                }
                for (uint32_t px = start_px; px < end_px; ++px)
                {
                    ppu_render_pixel(ppu, (uint8_t)px, ppu->mem->LY);
                }
            }
        }

        ppu->dots += advance;
        dots_to_advance -= advance;

        if (ppu->dots >= DOTS_PER_SCANLINE)
        {
            ppu->dots = 0;
            ppu->mem->LY++;
            if (ppu->mem->LY > VBLANK_SCANLINE_END)
            {
                ppu->mem->LY = 0;
                if (!ppu_legacy_scanline_renderer)
                {
                    ppu_render_frame_with_viruappu(ppu);
                }
                ppu->frame_ready = true;
            }
            ppu_update_lyc(ppu);
        }
    }
}


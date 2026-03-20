#include "ViruaPPUBridge.h"

#include <cpu/mode7.h>
#include <virtuappu.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace compat_mode8
{
constexpr std::size_t GB_SCREEN_WIDTH = 160;
constexpr std::size_t GB_SCREEN_HEIGHT = 144;
constexpr std::size_t VRAM_BANK_SIZE = 0x2000;
constexpr std::size_t OAM_SIZE_BYTES = 0x00A0;
constexpr std::size_t CRAM_SIZE = 64;

constexpr uint8_t LCDC_ENABLE = 1u << 7;
constexpr uint8_t LCDC_WINDOW_TILE_MAP = 1u << 6;
constexpr uint8_t LCDC_WINDOW_ENABLE = 1u << 5;
constexpr uint8_t LCDC_BG_WINDOW_TILE_DATA = 1u << 4;
constexpr uint8_t LCDC_BG_TILE_MAP = 1u << 3;
constexpr uint8_t LCDC_OBJ_SIZE = 1u << 2;
constexpr uint8_t LCDC_OBJ_ENABLE = 1u << 1;
constexpr uint8_t LCDC_BG_ENABLE = 1u << 0;

constexpr uint8_t ATTR_PALETTE_MASK = 0x07;
constexpr uint8_t ATTR_VRAM_BANK = 1u << 3;
constexpr uint8_t ATTR_X_FLIP = 1u << 5;
constexpr uint8_t ATTR_Y_FLIP = 1u << 6;
constexpr uint8_t ATTR_PRIORITY = 1u << 7;

struct SpriteCandidate
{
    uint8_t x;
    uint8_t tile;
    uint8_t attributes;
    uint8_t line;
    uint8_t index;
};

static inline uint8_t vram_read(const uint8_t *vram_bank0, const uint8_t *vram_bank1, uint8_t bank, uint16_t addr)
{
    if (addr < 0x8000u || addr >= 0xA000u)
        return 0u;
    uint16_t off = (uint16_t)(addr - 0x8000u);
    return bank ? vram_bank1[off] : vram_bank0[off];
}

static inline uint32_t rgb555_to_argb(uint16_t rgb555)
{
    uint8_t r5 = (uint8_t)((rgb555 >> 0) & 0x1F);
    uint8_t g5 = (uint8_t)((rgb555 >> 5) & 0x1F);
    uint8_t b5 = (uint8_t)((rgb555 >> 10) & 0x1F);
    uint8_t r8 = (uint8_t)((r5 << 3) | (r5 >> 2));
    uint8_t g8 = (uint8_t)((g5 << 3) | (g5 >> 2));
    uint8_t b8 = (uint8_t)((b5 << 3) | (b5 >> 2));
    return 0xFF000000u | ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | (uint32_t)b8;
}

static inline uint32_t palette_color(const uint8_t *cram, uint8_t palette, uint8_t color_id)
{
    std::size_t idx = (std::size_t)palette * 8u + (std::size_t)color_id * 2u;
    uint16_t rgb555 = (uint16_t)(cram[idx] | (cram[idx + 1] << 8));
    return rgb555_to_argb(rgb555);
}

static inline uint8_t fetch_tile_color(const uint8_t *vram_bank0,
                                       const uint8_t *vram_bank1,
                                       uint16_t tile_map_base,
                                       uint16_t tile_data_base,
                                       bool signed_indexing,
                                       uint8_t x,
                                       uint8_t y,
                                       uint8_t *out_palette,
                                       bool *out_priority)
{
    const uint8_t tile_x = (uint8_t)(x / 8u);
    const uint8_t tile_y = (uint8_t)(y / 8u);
    const uint16_t map_offset = (uint16_t)(tile_y * 32u + tile_x);
    const uint16_t map_addr = (uint16_t)(tile_map_base + map_offset);

    const uint8_t tile_index = vram_read(vram_bank0, vram_bank1, 0, map_addr);
    const uint8_t attr = vram_read(vram_bank0, vram_bank1, 1, map_addr);

    if (out_palette)
        *out_palette = (uint8_t)(attr & ATTR_PALETTE_MASK);
    if (out_priority)
        *out_priority = (attr & ATTR_PRIORITY) != 0;

    const uint8_t tile_bank = (attr & ATTR_VRAM_BANK) ? 1u : 0u;
    const int32_t tile_id = signed_indexing ? (int8_t)tile_index : tile_index;
    const uint16_t tile_addr = (uint16_t)(tile_data_base + tile_id * 16);

    uint8_t row = (uint8_t)(y % 8u);
    if (attr & ATTR_Y_FLIP)
        row = (uint8_t)(7u - row);

    const uint16_t row_addr = (uint16_t)(tile_addr + row * 2u);
    const uint8_t low = vram_read(vram_bank0, vram_bank1, tile_bank, row_addr);
    const uint8_t high = vram_read(vram_bank0, vram_bank1, tile_bank, (uint16_t)(row_addr + 1u));

    uint8_t col = (uint8_t)(x % 8u);
    if (attr & ATTR_X_FLIP)
        col = (uint8_t)(7u - col);
    const uint8_t bit = (uint8_t)(7u - col);

    return (uint8_t)((((high >> bit) & 1u) << 1u) | ((low >> bit) & 1u));
}

static inline uint8_t eval_sprites(const uint8_t *oam, uint8_t ly, uint8_t sprite_height, SpriteCandidate *out_sprites)
{
    uint8_t count = 0;
    SpriteCandidate candidates[40];

    for (uint8_t i = 0; i < 40; ++i)
    {
        const uint8_t yy = oam[i * 4u];
        const uint8_t xx = oam[i * 4u + 1u];
        const uint8_t tile = oam[i * 4u + 2u];
        const uint8_t attr = oam[i * 4u + 3u];

        const int sprite_y = (int)yy - 16;
        if ((int)ly < sprite_y || (int)ly >= sprite_y + sprite_height)
            continue;
        if (xx == 0 || xx >= 168)
            continue;

        SpriteCandidate sc{};
        sc.x = xx;
        sc.tile = tile;
        sc.attributes = attr;
        uint8_t line = (uint8_t)(ly - sprite_y);
        if (attr & 0x40u)
            line = (uint8_t)((sprite_height - 1u) - line);
        sc.line = line;
        sc.index = i;
        candidates[count++] = sc;
    }

    for (uint8_t i = 1; i < count; ++i)
    {
        const SpriteCandidate key = candidates[i];
        int j = (int)i - 1;
        while (j >= 0 && candidates[j].index > key.index)
        {
            candidates[j + 1] = candidates[j];
            --j;
        }
        candidates[j + 1] = key;
    }

    const uint8_t limit = count > 10u ? 10u : count;
    for (uint8_t i = 0; i < limit; ++i)
        out_sprites[i] = candidates[i];
    return limit;
}

static void render_frame(const uint8_t *vram_bank0,
                         const uint8_t *vram_bank1,
                         const uint8_t *oam,
                         const uint8_t *bg_cram,
                         const uint8_t *obj_cram,
                         const GBCPPURegisters *regs,
                         const uint8_t *scanline_lcdc,
                         uint32_t *dst_framebuffer,
                         uint32_t dst_stride_pixels)
{
    if ((regs->LCDC & LCDC_ENABLE) == 0)
    {
        for (std::size_t y = 0; y < GB_SCREEN_HEIGHT; ++y)
        {
            for (std::size_t x = 0; x < GB_SCREEN_WIDTH; ++x)
                dst_framebuffer[y * dst_stride_pixels + x] = 0xFFFFFFFFu;
        }
        return;
    }

    for (uint8_t y = 0; y < (uint8_t)GB_SCREEN_HEIGHT; ++y)
    {
        const uint8_t lcdc = scanline_lcdc ? scanline_lcdc[y] : regs->LCDC;
        SpriteCandidate sprites[10];
        uint8_t sprite_count = 0;
        if (lcdc & LCDC_OBJ_ENABLE)
        {
            const uint8_t sprite_height = (lcdc & LCDC_OBJ_SIZE) ? 16u : 8u;
            sprite_count = eval_sprites(oam, y, sprite_height, sprites);
        }

        for (uint8_t x = 0; x < (uint8_t)GB_SCREEN_WIDTH; ++x)
        {
            uint8_t bg_color_id = 0;
            uint8_t bg_palette = 0;
            bool bg_priority = false;
            uint32_t bg_pixel = palette_color(bg_cram, 0, 0);
            const bool bg_master_priority = (lcdc & LCDC_BG_ENABLE) != 0;

            {
                const uint16_t tile_map_base = (lcdc & LCDC_BG_TILE_MAP) ? 0x9C00u : 0x9800u;
                const uint16_t tile_data_base = (lcdc & LCDC_BG_WINDOW_TILE_DATA) ? 0x8000u : 0x9000u;
                const bool signed_idx = (lcdc & LCDC_BG_WINDOW_TILE_DATA) == 0;

                const uint8_t bg_x = (uint8_t)(x + regs->SCX);
                const uint8_t bg_y = (uint8_t)(y + regs->SCY);
                bg_color_id = fetch_tile_color(vram_bank0,
                                               vram_bank1,
                                               tile_map_base,
                                               tile_data_base,
                                               signed_idx,
                                               bg_x,
                                               bg_y,
                                               &bg_palette,
                                               &bg_priority);

                const bool window_on = (lcdc & LCDC_WINDOW_ENABLE) != 0;
                if (window_on && regs->WY <= y)
                {
                    const uint8_t wx = (regs->WX > 7u) ? (uint8_t)(regs->WX - 7u) : 0u;
                    if (x >= wx && regs->WX <= 166u)
                    {
                        const uint16_t win_map = (lcdc & LCDC_WINDOW_TILE_MAP) ? 0x9C00u : 0x9800u;
                        bg_color_id = fetch_tile_color(vram_bank0,
                                                       vram_bank1,
                                                       win_map,
                                                       tile_data_base,
                                                       signed_idx,
                                                       (uint8_t)(x - wx),
                                                       (uint8_t)(y - regs->WY),
                                                       &bg_palette,
                                                       &bg_priority);
                    }
                }

                bg_pixel = palette_color(bg_cram, bg_palette, bg_color_id);
            }

            uint32_t final_color = bg_pixel;

            if ((lcdc & LCDC_OBJ_ENABLE) && sprite_count > 0)
            {
                const uint8_t sprite_height = (lcdc & LCDC_OBJ_SIZE) ? 16u : 8u;

                for (uint8_t i = 0; i < sprite_count; ++i)
                {
                    const int screen_x = (int)sprites[i].x - 8;
                    if (x < screen_x || x >= screen_x + 8)
                        continue;

                    uint8_t pixel_x = (uint8_t)(x - screen_x);
                    const uint8_t attr = sprites[i].attributes;
                    if (attr & 0x20u)
                        pixel_x = (uint8_t)(7u - pixel_x);

                    uint8_t tile_idx = sprites[i].tile;
                    uint8_t line = sprites[i].line;
                    if (sprite_height == 16u)
                    {
                        tile_idx = (uint8_t)((tile_idx & 0xFEu) | (line >= 8u));
                        line &= 0x07u;
                    }

                    const uint8_t tile_bank = (attr & 0x08u) ? 1u : 0u;
                    const uint16_t tile_addr = (uint16_t)(0x8000u + tile_idx * 16u);
                    const uint16_t row_addr = (uint16_t)(tile_addr + line * 2u);
                    const uint8_t low = vram_read(vram_bank0, vram_bank1, tile_bank, row_addr);
                    const uint8_t high = vram_read(vram_bank0, vram_bank1, tile_bank, (uint16_t)(row_addr + 1u));
                    const uint8_t bit = (uint8_t)(7u - pixel_x);
                    const uint8_t color_id = (uint8_t)((((high >> bit) & 1u) << 1u) | ((low >> bit) & 1u));
                    if (color_id == 0)
                        continue;

                    const uint8_t obj_pal = (uint8_t)(attr & 0x07u);
                    const uint32_t sprite_color = palette_color(obj_cram, obj_pal, color_id);

                    if (bg_master_priority)
                    {
                        bool bg_wins = (bg_priority || (attr & 0x80u)) && bg_color_id != 0;
                        final_color = bg_wins ? bg_pixel : sprite_color;
                    }
                    else
                    {
                        final_color = sprite_color;
                    }
                    break;
                }
            }

            dst_framebuffer[(std::size_t)y * dst_stride_pixels + x] = final_color;
        }
    }
}
} // namespace compat_mode8

extern "C" void viruappu_render_gb_frame(const uint8_t *vram,
                                         const uint8_t *oam,
                                         const GBPPURegisters *regs,
                                         uint32_t *dst_framebuffer,
                                         uint32_t dst_stride_pixels)
{
    if (!vram || !oam || !regs || !dst_framebuffer)
        return;
    if (dst_stride_pixels < MODE7_GB_SCREEN_WIDTH)
        return;

    virtuappu_reset();

    Mode7Layout *layout = (Mode7Layout *)virtuappu_get_vram();
    std::memcpy(layout->vram, vram, MODE7_VRAM_SIZE_BYTES);
    std::memcpy(layout->oam, oam, MODE7_OAM_SIZE_BYTES);

    layout->regs.lcdc = regs->LCDC;
    layout->regs.scy = regs->SCY;
    layout->regs.scx = regs->SCX;
    layout->regs.bgp = regs->BGP;
    layout->regs.obp0 = regs->OBP0;
    layout->regs.obp1 = regs->OBP1;
    layout->regs.wy = regs->WY;
    layout->regs.wx = regs->WX;

    PPUMemory *ppu = virtuappu_get_registers();
    ppu->mode = 7;
    ppu->frame_width = MODE7_GB_SCREEN_WIDTH;
    virtuappu_render_frame();

    const uint32_t *src = virtuappu_get_frame_buffer();
    if (dst_stride_pixels == MODE7_GB_SCREEN_WIDTH)
    {
        std::memcpy(dst_framebuffer,
                    src,
                    MODE7_GB_SCREEN_WIDTH * MODE7_GB_SCREEN_HEIGHT * sizeof(uint32_t));
        return;
    }

    for (std::size_t y = 0; y < MODE7_GB_SCREEN_HEIGHT; ++y)
    {
        std::memcpy(dst_framebuffer + y * dst_stride_pixels,
                    src + y * MODE7_GB_SCREEN_WIDTH,
                    MODE7_GB_SCREEN_WIDTH * sizeof(uint32_t));
    }
}

extern "C" void viruappu_render_gbc_frame(const uint8_t *vram_bank0,
                                          const uint8_t *vram_bank1,
                                          const uint8_t *oam,
                                          const uint8_t *bg_cram,
                                          const uint8_t *obj_cram,
                                          const GBCPPURegisters *regs,
                                          const uint8_t *scanline_lcdc,
                                          uint32_t *dst_framebuffer,
                                          uint32_t dst_stride_pixels)
{
    if (!vram_bank0 || !vram_bank1 || !oam || !bg_cram || !obj_cram || !regs || !dst_framebuffer)
        return;
    if (dst_stride_pixels < compat_mode8::GB_SCREEN_WIDTH)
        return;

    compat_mode8::render_frame(vram_bank0,
                               vram_bank1,
                               oam,
                               bg_cram,
                               obj_cram,
                               regs,
                               scanline_lcdc,
                               dst_framebuffer,
                               dst_stride_pixels);
}

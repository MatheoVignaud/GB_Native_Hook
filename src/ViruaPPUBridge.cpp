#include "ViruaPPUBridge.h"

#include <VirtuaPPU.hpp>

#include <cstring>

extern "C"
{
// Mode1/Mode2 in ViruaPPU reference these GBA memory symbols.
// This emulator only uses mode 7, so neutral storage is sufficient.
uint8_t gIoMem[0x400] = {};
uint8_t gVram[0x18000] = {};
uint16_t gBgPltt[256] = {};
uint16_t gObjPltt[256] = {};
uint16_t gOamMem[512] = {};
}

extern "C" void viruappu_render_gb_frame(const uint8_t *vram,
                                         const uint8_t *oam,
                                         const GBPPURegisters *regs,
                                         uint32_t *dst_framebuffer,
                                         uint32_t dst_stride_pixels)
{
    if (!vram || !oam || !regs || !dst_framebuffer)
        return;
    if (dst_stride_pixels < Mode7::GB_SCREEN_WIDTH)
        return;

    Mode7::Mode7Layout *layout = Mode7::GetLayout();
    std::memcpy(layout->vram, vram, Mode7::VRAM_SIZE_BYTES);
    std::memcpy(layout->oam, oam, Mode7::OAM_SIZE_BYTES);

    layout->regs.lcdc = regs->LCDC;
    layout->regs.scy = regs->SCY;
    layout->regs.scx = regs->SCX;
    layout->regs.bgp = regs->BGP;
    layout->regs.obp0 = regs->OBP0;
    layout->regs.obp1 = regs->OBP1;
    layout->regs.wy = regs->WY;
    layout->regs.wx = regs->WX;

    global_Registers.mode = 7;
    global_Registers.frame_width = static_cast<uint16_t>(Mode7::GB_SCREEN_WIDTH);
    RenderFrame();

    const uint32_t *src = GetFrameBuffer();
    if (dst_stride_pixels == Mode7::GB_SCREEN_WIDTH)
    {
        std::memcpy(dst_framebuffer,
                    src,
                    Mode7::GB_SCREEN_WIDTH * Mode7::GB_SCREEN_HEIGHT * sizeof(uint32_t));
        return;
    }

    for (std::size_t y = 0; y < Mode7::GB_SCREEN_HEIGHT; ++y)
    {
        std::memcpy(dst_framebuffer + y * dst_stride_pixels,
                    src + y * Mode7::GB_SCREEN_WIDTH,
                    Mode7::GB_SCREEN_WIDTH * sizeof(uint32_t));
    }
}

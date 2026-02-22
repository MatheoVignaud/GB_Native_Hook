#ifndef VIRUAPPU_BRIDGE_H
#define VIRUAPPU_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    uint8_t LCDC;
    uint8_t SCY;
    uint8_t SCX;
    uint8_t BGP;
    uint8_t OBP0;
    uint8_t OBP1;
    uint8_t WY;
    uint8_t WX;
} GBPPURegisters;

void viruappu_render_gb_frame(const uint8_t *vram,
                              const uint8_t *oam,
                              const GBPPURegisters *regs,
                              uint32_t *dst_framebuffer,
                              uint32_t dst_stride_pixels);

#ifdef __cplusplus
}
#endif

#endif

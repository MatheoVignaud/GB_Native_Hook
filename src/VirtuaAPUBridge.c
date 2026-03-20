#include "VirtuaAPUBridge.h"

#include <stdbool.h>
#include <string.h>

#include <virtuapu.h>

static bool g_viruaapu_initialized = false;

void viruaapu_init(uint8_t mode, uint32_t sample_rate)
{
    VirtuaAPUMemory *regs = virtuapu_get_registers();

    regs->mode = mode;
    regs->sample_rate = sample_rate;
    regs->double_speed = false;
    virtuapu_reset();
    g_viruaapu_initialized = true;
}

void viruaapu_render(GBAPURegisters *regs,
                     int double_speed,
                     uint32_t sample_count,
                     int16_t *dst)
{
    uint32_t offset;
    VirtuaAPUMemory *apu_regs;

    if (regs == NULL || dst == NULL || sample_count == 0) {
        return;
    }

    memcpy(virtuapu_get_amem(), regs, sizeof(*regs));
    apu_regs = virtuapu_get_registers();
    apu_regs->double_speed = double_speed != 0;

    offset = 0;
    while (offset < sample_count) {
        uint32_t chunk = sample_count - offset;

        if (chunk > VIRTUAAPU_MAX_RENDER_SAMPLES) {
            chunk = VIRTUAAPU_MAX_RENDER_SAMPLES;
        }

        virtuapu_render_audio(chunk);
        memcpy(dst + offset * 2u,
               virtuapu_get_audio_buffer(),
               chunk * 2u * sizeof(int16_t));
        offset += chunk;
    }

    memcpy(regs, virtuapu_get_amem(), sizeof(*regs));
}

void viruaapu_sync(GBAPURegisters *regs, int double_speed)
{
    VirtuaAPUMemory *apu_regs;

    if (!g_viruaapu_initialized || regs == NULL) {
        return;
    }

    memcpy(virtuapu_get_amem(), regs, sizeof(*regs));
    apu_regs = virtuapu_get_registers();
    apu_regs->double_speed = double_speed != 0;
    virtuapu_sync_audio();
    memcpy(regs, virtuapu_get_amem(), sizeof(*regs));
}

void viruaapu_step_frame_sequencer(GBAPURegisters *regs, int double_speed)
{
    VirtuaAPUMemory *apu_regs;

    if (!g_viruaapu_initialized || regs == NULL) {
        return;
    }

    memcpy(virtuapu_get_amem(), regs, sizeof(*regs));
    apu_regs = virtuapu_get_registers();
    apu_regs->double_speed = double_speed != 0;
    virtuapu_sync_audio();
    virtuapu_step_frame_sequencer();
    memcpy(regs, virtuapu_get_amem(), sizeof(*regs));
}

void viruaapu_reset(void)
{
    if (!g_viruaapu_initialized) {
        return;
    }
    virtuapu_reset();
}

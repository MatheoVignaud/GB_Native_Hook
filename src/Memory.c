#include "Memory.h"
#include "CPU.h"
#include "PPU.h"
#include "VirtuaAPUBridge.h"

#include <SDL3/SDL.h>

#include <string.h>
#include <time.h>

static inline bool ppu_lcd_enabled(const MemoryState *mem)
{
    return (mem->memory.LCDC & 0x80u) != 0;
}

static void ppu_sync_to_cpu(MemoryState *mem)
{
    if (!mem || !mem->ppu || !mem->cpu)
        return;
    if (mem->cpu->cycle_count <= mem->ppu_synced_cycles)
        return;
    ppu_step(mem->ppu, (uint32_t)(mem->cpu->cycle_count - mem->ppu_synced_cycles));
}

static inline uint8_t ppu_mode_bits(const MemoryState *mem)
{
    if (!mem || !mem->ppu || !mem->cpu || !ppu_lcd_enabled(mem))
        return (uint8_t)(mem->memory.STAT & 0x03u);

    uint8_t ly = mem->memory.LY;
    uint32_t dots = mem->ppu->dots;
    uint16_t startup_delay = mem->ppu->lcd_startup_delay_dots;
    uint64_t unsynced_cycles = 0;
    if (mem->cpu->cycle_count > mem->ppu_synced_cycles)
        unsynced_cycles = mem->cpu->cycle_count - mem->ppu_synced_cycles;

    uint32_t extra_dots = (uint32_t)unsynced_cycles * DOTS_PER_CPU_CYCLE;
    if (mem->double_speed)
        extra_dots = (uint32_t)unsynced_cycles * (DOTS_PER_CPU_CYCLE / 2);

    if (startup_delay != 0)
    {
        if (extra_dots < startup_delay)
            return STAT_MODE_0;
        extra_dots -= startup_delay;
        startup_delay = 0;
    }

    dots += extra_dots;
    while (dots >= DOTS_PER_SCANLINE)
    {
        dots -= DOTS_PER_SCANLINE;
        ly++;
        if (ly > VBLANK_SCANLINE_END)
            ly = 0;
    }

    if (ly >= VBLANK_SCANLINE_START)
        return STAT_MODE_1;
    if (dots < 80u)
        return STAT_MODE_2;
    if (dots < (80u + 172u))
        return STAT_MODE_3;
    return STAT_MODE_0;
}

static inline bool ppu_vram_locked(const MemoryState *mem)
{
    return ppu_lcd_enabled(mem) && (ppu_mode_bits(mem) == 0x03u);
}

static inline bool ppu_oam_locked(const MemoryState *mem)
{
    if (!ppu_lcd_enabled(mem))
        return false;
    uint8_t mode = ppu_mode_bits(mem);
    return mode == 0x02u || mode == 0x03u;
}

static bool ppu_project_position(const MemoryState *mem, uint8_t *out_ly, uint32_t *out_dots)
{
    if (!mem || !mem->ppu || !mem->cpu || !ppu_lcd_enabled(mem))
        return false;

    uint8_t ly = mem->memory.LY;
    uint32_t dots = mem->ppu->dots;
    uint16_t startup_delay = mem->ppu->lcd_startup_delay_dots;
    uint64_t unsynced_cycles = 0;
    if (mem->cpu->cycle_count > mem->ppu_synced_cycles)
        unsynced_cycles = mem->cpu->cycle_count - mem->ppu_synced_cycles;

    uint32_t extra_dots = (uint32_t)unsynced_cycles * DOTS_PER_CPU_CYCLE;
    if (mem->double_speed)
        extra_dots = (uint32_t)unsynced_cycles * (DOTS_PER_CPU_CYCLE / 2);

    if (startup_delay != 0)
    {
        if (extra_dots < startup_delay)
        {
            *out_ly = ly;
            *out_dots = 0;
            return true;
        }
        extra_dots -= startup_delay;
        startup_delay = 0;
    }

    dots += extra_dots;
    while (dots >= DOTS_PER_SCANLINE)
    {
        dots -= DOTS_PER_SCANLINE;
        ly++;
        if (ly > VBLANK_SCANLINE_END)
            ly = 0;
    }

    *out_ly = ly;
    *out_dots = dots;
    return true;
}

static inline uint16_t oam_word_read(const MemoryState *mem, uint16_t byte_offset)
{
    return (uint16_t)(mem->memory.oam[byte_offset] | (mem->memory.oam[byte_offset + 1] << 8));
}

static inline void oam_word_write(MemoryState *mem, uint16_t byte_offset, uint16_t value)
{
    mem->memory.oam[byte_offset] = (uint8_t)(value & 0xFFu);
    mem->memory.oam[byte_offset + 1] = (uint8_t)(value >> 8);
}

static inline int ppu_mode2_oam_row(const MemoryState *mem)
{
    uint8_t ly = 0;
    uint32_t dots = 0;

    if (!ppu_project_position(mem, &ly, &dots))
        return -1;
    if (ly >= VBLANK_SCANLINE_START || dots >= 80u)
        return -1;

    return (int)(dots / 4u);
}

static inline uint16_t oam_bug_bitwise_write(uint16_t a, uint16_t b, uint16_t c)
{
    return (uint16_t)(((a ^ c) & (b ^ c)) ^ c);
}

static inline uint16_t oam_bug_bitwise_read(uint16_t a, uint16_t b, uint16_t c)
{
    return (uint16_t)(b | (a & c));
}

static inline uint16_t oam_bug_bitwise_read_secondary(uint16_t a, uint16_t b, uint16_t c, uint16_t d)
{
    return (uint16_t)((b & (a | c | d)) | (a & c & d));
}

static inline uint16_t oam_bug_bitwise_read_tertiary_1(uint16_t a, uint16_t b, uint16_t c, uint16_t d, uint16_t e)
{
    return (uint16_t)(c | (a & b & d & e));
}

static inline uint16_t oam_bug_bitwise_read_tertiary_2(uint16_t a, uint16_t b, uint16_t c, uint16_t d, uint16_t e)
{
    return (uint16_t)((c & (a | b | d | e)) | (a & b & d & e));
}

static inline uint16_t oam_bug_bitwise_read_tertiary_3(uint16_t a, uint16_t b, uint16_t c, uint16_t d, uint16_t e)
{
    return (uint16_t)((c & (a | b | d | e)) | (b & d & e));
}

static inline uint16_t oam_bug_bitwise_read_quaternary_dmg(uint16_t a,
                                                           uint16_t b,
                                                           uint16_t c,
                                                           uint16_t d,
                                                           uint16_t e,
                                                           uint16_t f,
                                                           uint16_t g,
                                                           uint16_t h)
{
    (void)a;
    return (uint16_t)((e & (h | g | ((uint16_t)(~d) & f) | c | b)) | (c & g & h));
}

static void oam_bug_apply_corruption(MemoryState *mem, bool is_read)
{
    if (!mem || mem->gbc_mode)
        return;

    int row = ppu_mode2_oam_row(mem);
    if (row <= 0 || row >= 20)
        return;

    uint16_t row_off = (uint16_t)(row * 8);
    uint16_t prev_off = (uint16_t)(row_off - 8);

    uint16_t a = oam_word_read(mem, row_off + 0);
    uint16_t b = oam_word_read(mem, prev_off + 0);
    uint16_t c = oam_word_read(mem, prev_off + 4);
    uint16_t first = is_read ? oam_bug_bitwise_read(a, b, c)
                             : oam_bug_bitwise_write(a, b, c);

    oam_word_write(mem, row_off + 0, first);
    oam_word_write(mem, row_off + 2, oam_word_read(mem, prev_off + 2));
    oam_word_write(mem, row_off + 4, oam_word_read(mem, prev_off + 4));
    oam_word_write(mem, row_off + 6, oam_word_read(mem, prev_off + 6));
}

void memory_oam_bug_blocked_read(MemoryState *mem, uint16_t addr)
{
    if (!mem || mem->gbc_mode || addr < 0xFE00u || addr > 0xFEFFu)
        return;

    int row = ppu_mode2_oam_row(mem);
    if (row <= 0 || row >= 20)
        return;

    uint16_t row_off = (uint16_t)(row * 8u);
    uint16_t prev_off = (uint16_t)(row_off - 8u);

    if ((row_off & 0x18u) == 0x10u)
    {
        if (row_off < 0x98u)
        {
            uint16_t prev2_off = (uint16_t)(row_off - 16u);
            uint16_t first = oam_bug_bitwise_read_secondary(
                oam_word_read(mem, prev2_off + 0u),
                oam_word_read(mem, prev_off + 0u),
                oam_word_read(mem, row_off + 0u),
                oam_word_read(mem, prev_off + 4u));
            oam_word_write(mem, prev_off + 0u, first);

            for (uint16_t b = 0; b < 8u; ++b)
                mem->memory.oam[prev2_off + b] = mem->memory.oam[prev_off + b];
        }
    }
    else if ((row_off & 0x18u) == 0x00u)
    {
        if (row_off < 0x98u)
        {
            uint16_t prev2_off = (uint16_t)(row_off - 16u);
            uint16_t prev4_off = (uint16_t)(row_off - 32u);
            uint16_t first;

            if (row_off == 0x40u)
            {
                first = oam_bug_bitwise_read_quaternary_dmg(
                    oam_word_read(mem, 0x0000u),
                    oam_word_read(mem, row_off + 0u),
                    oam_word_read(mem, prev_off + 4u),
                    oam_word_read(mem, prev_off + 2u),
                    oam_word_read(mem, prev_off + 0u),
                    oam_word_read(mem, prev2_off + 2u),
                    oam_word_read(mem, prev2_off + 0u),
                    oam_word_read(mem, prev4_off + 0u));
            }
            else if (row_off == 0x20u)
            {
                first = oam_bug_bitwise_read_tertiary_2(
                    oam_word_read(mem, row_off + 0u),
                    oam_word_read(mem, prev_off + 4u),
                    oam_word_read(mem, prev_off + 0u),
                    oam_word_read(mem, prev2_off + 0u),
                    oam_word_read(mem, prev4_off + 0u));
            }
            else if (row_off == 0x60u)
            {
                first = oam_bug_bitwise_read_tertiary_3(
                    oam_word_read(mem, row_off + 0u),
                    oam_word_read(mem, prev_off + 4u),
                    oam_word_read(mem, prev_off + 0u),
                    oam_word_read(mem, prev2_off + 0u),
                    oam_word_read(mem, prev4_off + 0u));
            }
            else
            {
                first = oam_bug_bitwise_read_tertiary_1(
                    oam_word_read(mem, row_off + 0u),
                    oam_word_read(mem, prev_off + 4u),
                    oam_word_read(mem, prev_off + 0u),
                    oam_word_read(mem, prev2_off + 0u),
                    oam_word_read(mem, prev4_off + 0u));
            }

            oam_word_write(mem, prev_off + 0u, first);
            for (uint16_t b = 0; b < 8u; ++b)
            {
                mem->memory.oam[prev2_off + b] = mem->memory.oam[prev_off + b];
                mem->memory.oam[prev4_off + b] = mem->memory.oam[prev_off + b];
            }
        }
    }
    else
    {
        uint16_t first = oam_bug_bitwise_read(
            oam_word_read(mem, row_off + 0u),
            oam_word_read(mem, prev_off + 0u),
            oam_word_read(mem, prev_off + 4u));
        oam_word_write(mem, prev_off + 0u, first);
    }

    for (uint16_t b = 0; b < 8u; ++b)
        mem->memory.oam[row_off + b] = mem->memory.oam[prev_off + b];

    if (row_off == 0x80u)
    {
        for (uint16_t b = 0; b < 8u; ++b)
            mem->memory.oam[b] = mem->memory.oam[row_off + b];
    }
}

void memory_oam_bug_idu_incdec(MemoryState *mem, uint16_t addr)
{
    if (!mem || addr < 0xFE00u || addr > 0xFEFFu)
        return;
    oam_bug_apply_corruption(mem, false);
}

void memory_oam_bug_read(MemoryState *mem, uint16_t addr)
{
    if (!mem || addr < 0xFE00u || addr > 0xFEFFu || mem->gbc_mode)
        return;
    oam_bug_apply_corruption(mem, true);
}

void memory_oam_bug_read_incdec(MemoryState *mem, uint16_t addr)
{
    if (!mem || addr < 0xFE00u || addr > 0xFEFFu || mem->gbc_mode)
        return;
    int row = ppu_mode2_oam_row(mem);
    if (row <= 0 || row >= 20)
    {
        oam_bug_apply_corruption(mem, true);
        return;
    }

    if (row < 4 || row >= 19)
    {
        oam_bug_apply_corruption(mem, true);
        return;
    }

    uint16_t row_off = (uint16_t)(row * 8);
    uint16_t prev_off = (uint16_t)(row_off - 8);
    uint16_t prev2_off = (uint16_t)(row_off - 16);

    uint16_t a = oam_word_read(mem, prev2_off + 0);
    uint16_t b = oam_word_read(mem, prev_off + 0);
    uint16_t c = oam_word_read(mem, row_off + 0);
    uint16_t d = oam_word_read(mem, prev_off + 4);
    uint16_t merged = (uint16_t)((b & (a | c | d)) | (a & c & d));

    oam_word_write(mem, prev_off + 0, merged);
    for (uint16_t w = 0; w < 8; w += 2)
    {
        uint16_t prev_word = oam_word_read(mem, prev_off + w);
        oam_word_write(mem, row_off + w, prev_word);
        oam_word_write(mem, prev2_off + w, prev_word);
    }

    oam_bug_apply_corruption(mem, true);
}

static inline bool apu_master_enabled(const MemoryState *mem)
{
    return (mem->memory.NR52 & 0x80u) != 0;
}

static inline bool apu_ch3_runtime_active(const MemoryState *mem)
{
    return apu_master_enabled(mem)
        && (mem->memory.NR30 & 0x80u) != 0
        && (mem->memory.NR52 & 0x04u) != 0;
}

static inline uint16_t apu_ch3_period_tcycles(const MemoryState *mem)
{
    uint16_t freq = (uint16_t)(mem->memory.NR33 | ((mem->memory.NR34 & 0x07u) << 8));
    uint16_t period = (uint16_t)((2048u - freq) * 2u);
    return period ? period : 2u;
}

static inline void apu_ch3_reset_runtime(MemoryState *mem)
{
    mem->apu_ch3_period_current = apu_ch3_period_tcycles(mem);
    mem->apu_ch3_period_pending = mem->apu_ch3_period_current;
    mem->apu_ch3_period_pending_valid = false;
    mem->apu_ch3_timer_tcycles = mem->apu_ch3_period_current;
    mem->apu_ch3_sample_buffer = 0;
    mem->apu_ch3_current_byte = mem->memory.WAVE_RAM[0];
    mem->apu_ch3_current_index = 0;
    mem->apu_ch3_pattern_offset = 0;
    mem->apu_ch3_fetch_age_tcycles = 0xFFu;
    mem->apu_ch3_has_fetched = false;
    mem->apu_ch3_restart_pending = false;
    mem->apu_ch3_fetch_valid[0] = false;
    mem->apu_ch3_fetch_valid[1] = false;
}

static inline void apu_ch3_trigger_runtime(MemoryState *mem, bool was_active)
{
    mem->apu_ch3_period_current = apu_ch3_period_tcycles(mem);
    mem->apu_ch3_period_pending = mem->apu_ch3_period_current;
    mem->apu_ch3_period_pending_valid = false;
    mem->apu_ch3_restart_pending = false;
    mem->apu_ch3_fetch_age_tcycles = 0xFFu;

    if (!mem->gbc_mode)
    {
        if (was_active && mem->apu_ch3_timer_tcycles <= 1u)
        {
            mem->apu_ch3_current_byte = mem->memory.WAVE_RAM[0];
            mem->apu_ch3_sample_buffer = mem->apu_ch3_current_byte;
        }
        mem->apu_ch3_current_index = 0;
        mem->apu_ch3_pattern_offset = 0;
        mem->apu_ch3_timer_tcycles = (uint16_t)(mem->apu_ch3_period_current + 6u);
        return;
    }

    mem->apu_ch3_timer_tcycles = (uint16_t)(mem->apu_ch3_period_current + 6u);
    mem->apu_ch3_current_index = 0;
    mem->apu_ch3_current_byte = mem->memory.WAVE_RAM[0];
    mem->apu_ch3_pattern_offset = 0;
}

static inline bool apu_ch3_dmg_accessible_now(const MemoryState *mem)
{
    if (!apu_ch3_runtime_active(mem))
        return false;
    return mem->apu_ch3_has_fetched && mem->apu_ch3_fetch_age_tcycles == 0u;
}

static inline uint8_t apu_ch3_dmg_fetch_index_now(const MemoryState *mem)
{
    if (!apu_ch3_runtime_active(mem))
        return mem->apu_ch3_current_index;
    if (mem->apu_ch3_has_fetched && mem->apu_ch3_fetch_valid[1] && mem->apu_ch3_fetch_age_tcycles == 0u)
        return mem->apu_ch3_fetch_index[1];
    return mem->apu_ch3_current_index;
}

static inline uint8_t apu_ch3_dmg_fetch_byte_now(const MemoryState *mem)
{
    if (!apu_ch3_runtime_active(mem))
        return mem->apu_ch3_current_byte;
    if (mem->apu_ch3_has_fetched && mem->apu_ch3_fetch_valid[1] && mem->apu_ch3_fetch_age_tcycles == 0u)
        return mem->apu_ch3_fetch_byte[1];
    return mem->apu_ch3_current_byte;
}

static inline uint16_t apu_ch3_effective_wave_addr(const MemoryState *mem, uint16_t address)
{
    if (!apu_ch3_runtime_active(mem))
        return address;
    return (uint16_t)(0xFF30u + (mem->apu_ch3_current_index & 0x0Fu));
}

static inline uint8_t apu_ch3_pcm_nibble(const MemoryState *mem)
{
    if (!apu_ch3_runtime_active(mem))
        return 0u;

    uint8_t nibble = (mem->apu_ch3_pattern_offset & 1u)
        ? (uint8_t)(mem->apu_ch3_sample_buffer & 0x0Fu)
        : (uint8_t)(mem->apu_ch3_sample_buffer >> 4);
    uint8_t volume_code = (mem->memory.NR32 >> 5) & 0x03u;

    switch (volume_code)
    {
    case 0: return 0u;
    case 1: return nibble;
    case 2: return (uint8_t)(nibble >> 1);
    case 3: return (uint8_t)(nibble >> 2);
    default: return 0u;
    }
}

static void apu_ch3_corrupt_on_trigger(MemoryState *mem)
{
    uint8_t index = (uint8_t)(((mem->apu_ch3_pattern_offset + 1u) >> 1) & 0x0Fu);
    if (index < 4u)
    {
        mem->memory.WAVE_RAM[0] = mem->memory.WAVE_RAM[index];
        return;
    }

    uint8_t base = (uint8_t)(index & 0xFCu);
    for (uint8_t i = 0; i < 4u; ++i)
        mem->memory.WAVE_RAM[i] = mem->memory.WAVE_RAM[(uint8_t)(base + i)];
}

static uint8_t apu_read_mask(uint16_t address)
{
    switch (address)
    {
    case 0xFF10: return 0x80u;
    case 0xFF11: return 0x3Fu;
    case 0xFF12: return 0x00u;
    case 0xFF13: return 0xFFu;
    case 0xFF14: return 0xBFu;
    case 0xFF15: return 0xFFu;
    case 0xFF16: return 0x3Fu;
    case 0xFF17: return 0x00u;
    case 0xFF18: return 0xFFu;
    case 0xFF19: return 0xBFu;
    case 0xFF1A: return 0x7Fu;
    case 0xFF1B: return 0xFFu;
    case 0xFF1C: return 0x9Fu;
    case 0xFF1D: return 0xFFu;
    case 0xFF1E: return 0xBFu;
    case 0xFF1F: return 0xFFu;
    case 0xFF20: return 0xFFu;
    case 0xFF21: return 0x00u;
    case 0xFF22: return 0x00u;
    case 0xFF23: return 0xBFu;
    case 0xFF24: return 0x00u;
    case 0xFF25: return 0x00u;
    default:     return 0x00u;
    }
}

static uint8_t apu_channel_status_mask(uint16_t address)
{
    switch (address)
    {
    case 0xFF12:
    case 0xFF14:
        return 0x01u;
    case 0xFF17:
    case 0xFF19:
        return 0x02u;
    case 0xFF1A:
    case 0xFF1E:
        return 0x04u;
    case 0xFF21:
    case 0xFF23:
        return 0x08u;
    default:
        return 0x00u;
    }
}

static bool apu_channel_dac_enabled(const MemoryState *mem, uint16_t address)
{
    switch (address)
    {
    case 0xFF12:
    case 0xFF14:
        return (mem->memory.NR12 & 0xF8u) != 0;
    case 0xFF17:
    case 0xFF19:
        return (mem->memory.NR22 & 0xF8u) != 0;
    case 0xFF1A:
    case 0xFF1E:
        return (mem->memory.NR30 & 0x80u) != 0;
    case 0xFF21:
    case 0xFF23:
        return (mem->memory.NR42 & 0xF8u) != 0;
    default:
        return false;
    }
}

static void apu_set_channel_status(MemoryState *mem, uint8_t mask, bool enabled)
{
    if (enabled)
        mem->memory.NR52 |= mask;
    else
        mem->memory.NR52 &= (uint8_t)~mask;
}

static void apu_power_off(MemoryState *mem)
{
    uint8_t nr41 = mem->memory.NR41;

    mem->memory.NR10 = 0x00;
    mem->memory.NR11 = 0x00;
    mem->memory.NR12 = 0x00;
    mem->memory.NR13 = 0x00;
    mem->memory.NR14 = 0x00;
    mem->memory._io_ff15 = 0x00;
    mem->memory.NR21 = 0x00;
    mem->memory.NR22 = 0x00;
    mem->memory.NR23 = 0x00;
    mem->memory.NR24 = 0x00;
    mem->memory.NR30 = 0x00;
    mem->memory.NR31 = 0x00;
    mem->memory.NR32 = 0x00;
    mem->memory.NR33 = 0x00;
    mem->memory.NR34 = 0x00;
    mem->memory._io_ff1f = 0x00;
    mem->memory.NR41 = 0x00;
    mem->memory.NR42 = 0x00;
    mem->memory.NR43 = 0x00;
    mem->memory.NR44 = 0x00;
    mem->memory.NR50 = 0x00;
    mem->memory.NR51 = 0x00;
    mem->memory.NR52 = 0x00;

    if (!mem->gbc_mode)
    {
        mem->memory.NR41 = nr41;
    }

    apu_ch3_reset_runtime(mem);
}

static void apu_snapshot_from_memory(const MemoryState *mem, GBAPURegisters *snap)
{
    snap->NR10 = mem->memory.NR10;
    snap->NR11 = mem->memory.NR11;
    snap->NR12 = mem->memory.NR12;
    snap->NR13 = mem->memory.NR13;
    snap->NR14 = mem->memory.NR14;
    snap->_pad15 = 0;
    snap->NR21 = mem->memory.NR21;
    snap->NR22 = mem->memory.NR22;
    snap->NR23 = mem->memory.NR23;
    snap->NR24 = mem->memory.NR24;
    snap->NR30 = mem->memory.NR30;
    snap->NR31 = mem->memory.NR31;
    snap->NR32 = mem->memory.NR32;
    snap->NR33 = mem->memory.NR33;
    snap->NR34 = mem->memory.NR34;
    snap->_pad1f = 0;
    snap->NR41 = mem->memory.NR41;
    snap->NR42 = mem->memory.NR42;
    snap->NR43 = mem->memory.NR43;
    snap->NR44 = mem->memory.NR44;
    snap->NR50 = mem->memory.NR50;
    snap->NR51 = mem->memory.NR51;
    snap->NR52 = mem->memory.NR52;
    memset(snap->_pad27_2f, 0, sizeof(snap->_pad27_2f));
    snap->GBAPU_META_LAST_WRITE = GBAPU_WRITE_NONE;
    memcpy(snap->WAVE_RAM, mem->memory.WAVE_RAM, sizeof(snap->WAVE_RAM));
}

static void apu_apply_runtime_state(MemoryState *mem, const GBAPURegisters *snap)
{
    mem->memory.NR13 = snap->NR13;
    mem->memory.NR14 &= 0x7Fu;
    mem->memory.NR14 = (uint8_t)((mem->memory.NR14 & 0x78u) | (snap->NR14 & 0x07u));
    mem->memory.NR24 &= 0x7Fu;
    mem->memory.NR34 &= 0x7Fu;
    mem->memory.NR44 &= 0x7Fu;
    mem->memory.NR52 = (uint8_t)((mem->memory.NR52 & 0xF0u) | (snap->NR52 & 0x0Fu));
}

static void apu_sync_bridge(MemoryState *mem, uint16_t write_address)
{
    GBAPURegisters snap;
    apu_snapshot_from_memory(mem, &snap);
    if (write_address >= 0xFF10 && write_address <= 0xFF3F)
        snap.GBAPU_META_LAST_WRITE = (uint8_t)(write_address - 0xFF10u);
    viruaapu_sync(&snap, mem->double_speed ? 1 : 0);
    apu_apply_runtime_state(mem, &snap);
}

void memory_apu_frame_sequencer_tick(MemoryState *mem)
{
    if (!mem)
        return;

    GBAPURegisters snap;
    apu_snapshot_from_memory(mem, &snap);
    viruaapu_step_frame_sequencer(&snap, mem->double_speed ? 1 : 0);
    apu_apply_runtime_state(mem, &snap);
}

void memory_apu_runtime_step(MemoryState *mem, uint32_t cycles)
{
    if (!mem || cycles == 0)
        return;

    for (uint32_t i = 0; i < cycles; ++i)
    {
        if (!apu_ch3_runtime_active(mem))
        {
            mem->apu_ch3_tcycle_counter += 4u;
            mem->apu_ch3_fetch_age_tcycles = 0xFFu;
            continue;
        }

        for (uint8_t t = 0; t < 4u; ++t)
        {
            mem->apu_ch3_tcycle_counter++;
            if (mem->apu_ch3_fetch_age_tcycles != 0xFFu)
                mem->apu_ch3_fetch_age_tcycles++;

            if (mem->apu_ch3_timer_tcycles > 1u)
            {
                mem->apu_ch3_timer_tcycles--;
                continue;
            }

            mem->apu_ch3_pattern_offset = (uint8_t)((mem->apu_ch3_pattern_offset + 1u) & 31u);
            mem->apu_ch3_current_index = (uint8_t)((mem->apu_ch3_pattern_offset >> 1) & 0x0Fu);
            mem->apu_ch3_current_byte = mem->memory.WAVE_RAM[mem->apu_ch3_current_index];
            mem->apu_ch3_sample_buffer = mem->apu_ch3_current_byte;
            mem->apu_ch3_has_fetched = true;
            mem->apu_ch3_fetch_age_tcycles = 0u;
            mem->apu_ch3_fetch_valid[0] = mem->apu_ch3_fetch_valid[1];
            mem->apu_ch3_fetch_tcycle[0] = mem->apu_ch3_fetch_tcycle[1];
            mem->apu_ch3_fetch_byte[0] = mem->apu_ch3_fetch_byte[1];
            mem->apu_ch3_fetch_index[0] = mem->apu_ch3_fetch_index[1];
            mem->apu_ch3_fetch_valid[1] = true;
            mem->apu_ch3_fetch_tcycle[1] = mem->apu_ch3_tcycle_counter;
            mem->apu_ch3_fetch_byte[1] = mem->apu_ch3_current_byte;
            mem->apu_ch3_fetch_index[1] = mem->apu_ch3_current_index;
            if (mem->apu_ch3_period_pending_valid)
            {
                mem->apu_ch3_period_current = mem->apu_ch3_period_pending;
                mem->apu_ch3_period_pending_valid = false;
            }
            mem->apu_ch3_timer_tcycles = mem->apu_ch3_period_current ? mem->apu_ch3_period_current : 2u;
        }
    }
}

static void apu_write(MemoryState *mem, uint16_t address, uint8_t value)
{
    if (address >= 0xFF30 && address <= 0xFF3F)
    {
        if (apu_ch3_runtime_active(mem))
        {
            if (mem->gbc_mode || apu_ch3_dmg_accessible_now(mem))
            {
                uint16_t effective = apu_ch3_effective_wave_addr(mem, address);
                if (!mem->gbc_mode)
                    effective = (uint16_t)(0xFF30u + apu_ch3_dmg_fetch_index_now(mem));
                mem->memory.WAVE_RAM[effective - 0xFF30u] = value;
            }
        }
        else
        {
            mem->memory.WAVE_RAM[address - 0xFF30u] = value;
        }
        apu_sync_bridge(mem, address);
        return;
    }

    if (address == 0xFF26)
    {
        bool was_enabled = apu_master_enabled(mem);
        bool enable = (value & 0x80u) != 0;

        if (was_enabled && !enable)
        {
            apu_power_off(mem);
        }
        else if (!was_enabled && enable)
        {
            mem->memory.NR52 = 0x80u;
            apu_ch3_reset_runtime(mem);
        }
        apu_sync_bridge(mem, address);
        return;
    }

    if (address < 0xFF10 || address > 0xFF25)
    {
        mem->memory.data[address] = value;
        return;
    }

    if (!apu_master_enabled(mem))
    {
        if (mem->gbc_mode)
        {
            return;
        }
        else
        {
            switch (address)
            {
            case 0xFF11:
                mem->memory.NR11 = (uint8_t)((mem->memory.NR11 & 0xC0u) | (value & 0x3Fu));
                apu_sync_bridge(mem, address);
                return;
            case 0xFF16:
                mem->memory.NR21 = (uint8_t)((mem->memory.NR21 & 0xC0u) | (value & 0x3Fu));
                apu_sync_bridge(mem, address);
                return;
            case 0xFF1B:
                mem->memory.NR31 = value;
                apu_sync_bridge(mem, address);
                return;
            case 0xFF20:
                mem->memory.NR41 = value & 0x3Fu;
                apu_sync_bridge(mem, address);
                return;
            default:
                return;
            }
        }
    }

    switch (address)
    {
    case 0xFF10: mem->memory.NR10 = value & 0x7Fu; break;
    case 0xFF11: mem->memory.NR11 = value; break;
    case 0xFF12: mem->memory.NR12 = value; break;
    case 0xFF13: mem->memory.NR13 = value; break;
    case 0xFF14: mem->memory.NR14 = value; break;
    case 0xFF15: mem->memory._io_ff15 = value; break;
    case 0xFF16: mem->memory.NR21 = value; break;
    case 0xFF17: mem->memory.NR22 = value; break;
    case 0xFF18: mem->memory.NR23 = value; break;
    case 0xFF19: mem->memory.NR24 = value; break;
    case 0xFF1A: mem->memory.NR30 = value & 0x80u; break;
    case 0xFF1B: mem->memory.NR31 = value; break;
    case 0xFF1C: mem->memory.NR32 = value & 0x60u; break;
    case 0xFF1D: mem->memory.NR33 = value; break;
    case 0xFF1E: mem->memory.NR34 = value; break;
    case 0xFF1F: mem->memory._io_ff1f = value; break;
    case 0xFF20: mem->memory.NR41 = value & 0x3Fu; break;
    case 0xFF21: mem->memory.NR42 = value; break;
    case 0xFF22: mem->memory.NR43 = value; break;
    case 0xFF23: mem->memory.NR44 = value; break;
    case 0xFF24: mem->memory.NR50 = value; break;
    case 0xFF25: mem->memory.NR51 = value; break;
    default:
        mem->memory.data[address] = value;
        break;
    }

    if (address == 0xFF1A && (value & 0x80u) == 0)
    {
        mem->apu_ch3_fetch_age_tcycles = 0xFFu;
        mem->apu_ch3_has_fetched = false;
    }
    else if ((address == 0xFF1D || address == 0xFF1E) && apu_ch3_runtime_active(mem))
    {
        mem->apu_ch3_period_pending = apu_ch3_period_tcycles(mem);
        mem->apu_ch3_period_pending_valid = true;
    }

    if (address >= 0xFF10 && address <= 0xFF25)
    {
        uint8_t mask = apu_channel_status_mask(address);
        if (mask != 0 && !apu_channel_dac_enabled(mem, address))
            apu_set_channel_status(mem, mask, false);
    }

    switch (address)
    {
    case 0xFF14:
    case 0xFF19:
    case 0xFF1E:
    case 0xFF23:
    {
        bool was_ch3_active = (address == 0xFF1E) ? apu_ch3_runtime_active(mem) : false;
        bool trigger = (value & 0x80u) != 0;
        uint8_t mask = apu_channel_status_mask(address);
        if (trigger)
            apu_set_channel_status(mem, mask, apu_channel_dac_enabled(mem, address));
        if (address == 0xFF1E && trigger)
        {
            if (!mem->gbc_mode && was_ch3_active && apu_ch3_runtime_active(mem) && mem->apu_ch3_timer_tcycles <= 2u)
                apu_ch3_corrupt_on_trigger(mem);
            apu_ch3_trigger_runtime(mem, was_ch3_active);
        }
        break;
    }
    default:
        break;
    }

    apu_sync_bridge(mem, address);
}

static FILE *mbc_trace_file = NULL;
static bool mbc_trace_enabled = false;
static bool mbc_trace_inited = false;
static char mbc_trace_path[512] = {0};
static bool memory_logging_enabled = true;
static bool io_trace_enabled = false;
static bool io_trace_inited = false;
static bool rom_read_trace_inited = false;
static bool rom_read_trace_enabled = false;
static char rom_read_trace_path[512] = {0};
static uint8_t *rom_read_bitmap = NULL;
static size_t rom_read_bitmap_size = 0;
static size_t rom_read_track_size = 0;
static size_t rom_read_unique_count = 0;
static uint8_t *rom_data_read_bitmap = NULL;
static size_t rom_data_read_bitmap_size = 0;
static size_t rom_data_read_unique_count = 0;
static SDL_Mutex *rom_trace_mutex = NULL;

static uint64_t mbc3_rtc_now_seconds(void);
static void mbc3_rtc_update(CartridgeState *cart);
static void cartridge_persist_save(CartridgeState *cart, bool force);

typedef struct
{
    char magic[8];
    uint8_t regs[5];
    uint8_t latched[5];
    uint8_t latch_state;
    uint8_t latched_valid;
    uint8_t _pad[6];
    uint64_t last_unix;
} CartridgeRtcPersist;

typedef struct
{
    bool active;
    uint16_t start;
    uint8_t len;
    uint8_t bytes[4];
} FetchPatchState;

static FetchPatchState fetch_patch = {0};

static void rom_trace_lock(void)
{
    if (!rom_trace_mutex)
    {
        rom_trace_mutex = SDL_CreateMutex();
    }
    if (rom_trace_mutex)
    {
        SDL_LockMutex(rom_trace_mutex);
    }
}

static void rom_trace_unlock(void)
{
    if (rom_trace_mutex)
    {
        SDL_UnlockMutex(rom_trace_mutex);
    }
}

void memory_set_logging(bool enabled)
{
    memory_logging_enabled = enabled;
}

static void io_trace_init(void)
{
    if (io_trace_inited)
        return;
    io_trace_inited = true;

    const char *trace_env = getenv("GB_TRACE_IO");
    if (trace_env && trace_env[0] != '\0' && trace_env[0] != '0')
        io_trace_enabled = true;

}

static void io_trace_write(const MemoryState *mem, uint16_t address, uint8_t value)
{
    if (!io_trace_enabled || !mem || !mem->cpu)
        return;

    if (address == 0xFF0F || address == 0xFFFF || address == 0xFF4D)
    {
        printf("[IO] cyc=%llu pc=%04X write %04X=%02X IE=%02X IF=%02X KEY1=%02X ds=%d\n",
               (unsigned long long)mem->cpu->cycle_count,
               mem->cpu->PC,
               address,
               value,
               mem->memory.IE,
               mem->memory.IF,
               mem->key1,
               mem->double_speed ? 1 : 0);
    }
}

void memory_set_debug_print(MemoryState *mem, bool enabled)
{
    if (!mem)
        return;
    mem->debug_print_enabled = enabled;
}

void memory_set_fetch_patch(uint16_t start, const uint8_t *bytes, uint8_t len)
{
    fetch_patch.active = false;
    fetch_patch.start = start;
    fetch_patch.len = 0;
    memset(fetch_patch.bytes, 0, sizeof(fetch_patch.bytes));
    if (!bytes || len == 0)
        return;

    if (len > (uint8_t)sizeof(fetch_patch.bytes))
        len = (uint8_t)sizeof(fetch_patch.bytes);
    memcpy(fetch_patch.bytes, bytes, len);
    fetch_patch.len = len;
    fetch_patch.active = true;
}

void memory_clear_fetch_patch(void)
{
    fetch_patch.active = false;
    fetch_patch.start = 0;
    fetch_patch.len = 0;
    memset(fetch_patch.bytes, 0, sizeof(fetch_patch.bytes));
}

static void rom_read_trace_reset(size_t rom_size)
{
    rom_trace_lock();
    free(rom_read_bitmap);
    rom_read_bitmap = NULL;
    rom_read_bitmap_size = 0;
    free(rom_data_read_bitmap);
    rom_data_read_bitmap = NULL;
    rom_data_read_bitmap_size = 0;
    rom_read_track_size = rom_size;
    rom_read_unique_count = 0;
    rom_data_read_unique_count = 0;

    if (rom_size == 0)
    {
        rom_trace_unlock();
        return;
    }

    if (rom_read_trace_enabled)
    {
        rom_read_bitmap_size = (rom_size + 7u) / 8u;
        rom_read_bitmap = (uint8_t *)calloc(rom_read_bitmap_size, 1);
        if (!rom_read_bitmap)
        {
            rom_read_bitmap_size = 0;
            rom_read_trace_enabled = false;
            if (memory_logging_enabled)
            {
                fprintf(stderr, "[ROM-TRACE] failed to allocate bitmap, disabling trace.\n");
            }
        }
    }

    rom_data_read_bitmap_size = (rom_size + 7u) / 8u;
    rom_data_read_bitmap = (uint8_t *)calloc(rom_data_read_bitmap_size, 1);
    if (!rom_data_read_bitmap)
    {
        rom_data_read_bitmap_size = 0;
    }
    rom_trace_unlock();
}

static void rom_read_trace_init(void)
{
    if (rom_read_trace_inited)
        return;
    rom_read_trace_inited = true;

    const char *path = getenv("GB_ROM_READ_TRACE_PATH");
    if (!path || path[0] == '\0')
        return;

    size_t n = strlen(path);
    if (n >= sizeof(rom_read_trace_path))
        n = sizeof(rom_read_trace_path) - 1;
    memcpy(rom_read_trace_path, path, n);
    rom_read_trace_path[n] = '\0';
    rom_read_trace_enabled = true;
}

static void rom_read_trace_mark(size_t absolute)
{
    rom_trace_lock();
    if (!rom_read_trace_enabled || !rom_read_bitmap || absolute >= rom_read_track_size)
    {
        rom_trace_unlock();
        return;
    }

    size_t byte_index = absolute >> 3u;
    uint8_t mask = (uint8_t)(1u << (absolute & 7u));
    if ((rom_read_bitmap[byte_index] & mask) == 0)
    {
        rom_read_bitmap[byte_index] |= mask;
        rom_read_unique_count++;
    }
    rom_trace_unlock();
}

static void rom_data_read_mark(size_t absolute)
{
    rom_trace_lock();
    if (!rom_data_read_bitmap || absolute >= rom_read_track_size)
    {
        rom_trace_unlock();
        return;
    }

    size_t byte_index = absolute >> 3u;
    uint8_t mask = (uint8_t)(1u << (absolute & 7u));
    if ((rom_data_read_bitmap[byte_index] & mask) == 0)
    {
        rom_data_read_bitmap[byte_index] |= mask;
        rom_data_read_unique_count++;
    }
    rom_trace_unlock();
}

static void rom_read_trace_flush(void)
{
    rom_trace_lock();
    if (!rom_read_trace_enabled || !rom_read_bitmap || rom_read_trace_path[0] == '\0')
    {
        rom_trace_unlock();
        return;
    }

    FILE *f = fopen(rom_read_trace_path, "w");
    if (!f)
    {
        rom_trace_unlock();
        return;
    }

    fprintf(f, "# GB ROM read trace\n");
    fprintf(f, "# rom_size=%zu unique_reads=%zu\n", rom_read_track_size, rom_read_unique_count);

    size_t i = 0;
    while (i < rom_read_track_size)
    {
        size_t byte_index = i >> 3u;
        uint8_t mask = (uint8_t)(1u << (i & 7u));
        if ((rom_read_bitmap[byte_index] & mask) == 0)
        {
            i++;
            continue;
        }

        size_t start = i;
        size_t end = i;
        while ((end + 1u) < rom_read_track_size)
        {
            size_t next = end + 1u;
            size_t next_byte_index = next >> 3u;
            uint8_t next_mask = (uint8_t)(1u << (next & 7u));
            if ((rom_read_bitmap[next_byte_index] & next_mask) == 0)
                break;
            end = next;
        }

        if (start == end)
            fprintf(f, "%06zX\n", start);
        else
            fprintf(f, "%06zX-%06zX\n", start, end);

        i = end + 1u;
    }

    fclose(f);
    if (memory_logging_enabled)
    {
        printf("[ROM-TRACE] wrote %s (unique=%zu)\n", rom_read_trace_path, rom_read_unique_count);
    }
    rom_trace_unlock();
}

static void mbc_trace_init(void)
{
    if (mbc_trace_inited)
        return;
    mbc_trace_inited = true;

    const char *trace_env = getenv("GB_TRACE_MBC");
    if (!trace_env || trace_env[0] == '\0' || trace_env[0] == '0')
        return;

    const char *path = getenv("GB_TRACE_MBC_PATH");
    if (path && path[0] != '\0')
    {
        size_t n = strlen(path);
        if (n >= sizeof(mbc_trace_path))
            n = sizeof(mbc_trace_path) - 1;
        memcpy(mbc_trace_path, path, n);
        mbc_trace_path[n] = '\0';
    }
    else
    {
        snprintf(mbc_trace_path, sizeof(mbc_trace_path), "%s", "mbc_trace.log");
    }

    mbc_trace_file = fopen(mbc_trace_path, "w");
    if (!mbc_trace_file)
        return;

    mbc_trace_enabled = true;
    fprintf(mbc_trace_file, "MBC trace enabled\n");
    fflush(mbc_trace_file);
}

static void mbc_trace_log(const MemoryState *mem, const char *event, uint16_t address, uint8_t value)
{
    if (!mbc_trace_enabled || !mbc_trace_file || !mem)
        return;

    const CartridgeState *cart = &mem->cartridge;
    uint16_t pc = mem->cpu ? mem->cpu->PC : 0xFFFFu;
    fprintf(mbc_trace_file,
            "%s PC=%04X A=%04X V=%02X TYPE=%02X MBC=%d ROMB=%zu RAMB=%zu EN=%d M1(low=%02X hi=%02X mode=%u) M2=%02X M3(rom=%02X sel=%02X latch=%u) M5(rom=%03X ram=%02X)\n",
            event,
            pc,
            address,
            value,
            cart->cartridge_type,
            (int)cart->mbc_type,
            cart->rom_banks,
            cart->ram_banks,
            cart->ram_enabled ? 1 : 0,
            cart->mbc1_low5,
            cart->mbc1_high2,
            cart->mbc1_mode,
            cart->mbc2_rom_bank,
            cart->mbc3_rom_bank,
            cart->mbc3_ram_rtc_select,
            cart->mbc3_latch_state,
            cart->mbc5_rom_bank,
            cart->mbc5_ram_bank);
    fflush(mbc_trace_file);
}

static const char *cartridge_type_name(uint8_t type)
{
    switch (type)
    {
    case 0x00:
        return "ROM ONLY";
    case 0x01:
        return "MBC1";
    case 0x02:
        return "MBC1+RAM";
    case 0x03:
        return "MBC1+RAM+BATTERY";
    case 0x05:
        return "MBC2";
    case 0x06:
        return "MBC2+BATTERY";
    case 0x08:
        return "ROM+RAM";
    case 0x09:
        return "ROM+RAM+BATTERY";
    case 0x0F:
        return "MBC3+TIMER+BATTERY";
    case 0x10:
        return "MBC3+TIMER+RAM+BATTERY";
    case 0x11:
        return "MBC3";
    case 0x12:
        return "MBC3+RAM";
    case 0x13:
        return "MBC3+RAM+BATTERY";
    case 0x19:
        return "MBC5";
    case 0x1A:
        return "MBC5+RAM";
    case 0x1B:
        return "MBC5+RAM+BATTERY";
    case 0x1C:
        return "MBC5+RUMBLE";
    case 0x1D:
        return "MBC5+RUMBLE+RAM";
    case 0x1E:
        return "MBC5+RUMBLE+RAM+BATTERY";
    default:
        return "UNKNOWN";
    }
}

static size_t cartridge_rom_banks_from_header(uint8_t rom_size_code)
{
    switch (rom_size_code)
    {
    case 0x00:
        return 2;
    case 0x01:
        return 4;
    case 0x02:
        return 8;
    case 0x03:
        return 16;
    case 0x04:
        return 32;
    case 0x05:
        return 64;
    case 0x06:
        return 128;
    case 0x07:
        return 256;
    case 0x08:
        return 512;
    case 0x52:
        return 72;
    case 0x53:
        return 80;
    case 0x54:
        return 96;
    default:
        return 0;
    }
}

static size_t cartridge_ram_size_from_header(uint8_t ram_size_code)
{
    switch (ram_size_code)
    {
    case 0x00:
        return 0;
    case 0x01:
        return 2 * 1024;
    case 0x02:
        return 8 * 1024;
    case 0x03:
        return 32 * 1024;
    case 0x04:
        return 128 * 1024;
    case 0x05:
        return 64 * 1024;
    default:
        return 0;
    }
}

static size_t cartridge_size_to_rom_banks(size_t rom_size)
{
    if (rom_size == 0)
        return 0;
    return (rom_size + 0x3FFFu) / 0x4000u;
}

static size_t cartridge_size_to_ram_banks(size_t ram_size)
{
    if (ram_size == 0)
        return 0;
    return (ram_size + 0x1FFFu) / 0x2000u;
}

static void persist_path_from_rom(const char *rom_path, const char *ext, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    out[0] = '\0';
    if (!rom_path || rom_path[0] == '\0')
        return;

    snprintf(out, out_size, "%s", rom_path);
    char *slash1 = strrchr(out, '/');
    char *slash2 = strrchr(out, '\\');
    char *slash = slash1;
    if (!slash || (slash2 && slash2 > slash))
        slash = slash2;
    char *dot = strrchr(out, '.');
    if (dot && (!slash || dot > slash))
    {
        *dot = '\0';
    }
    if (ext && ext[0] != '\0')
    {
        size_t len = strlen(out);
        if (len + strlen(ext) + 1 < out_size)
            strcat(out, ext);
    }
}

static void cartridge_set_persist_paths(CartridgeState *cart, const char *rom_path)
{
    if (!cart)
        return;
    if (!rom_path || rom_path[0] == '\0')
        return;
    persist_path_from_rom(rom_path, ".sav", cart->save_path, sizeof(cart->save_path));
    persist_path_from_rom(rom_path, ".rtc", cart->rtc_path, sizeof(cart->rtc_path));
}

static void cartridge_persist_load(CartridgeState *cart)
{
    if (!cart || cart->persist_loaded || !cart->has_battery)
    {
        if (cart)
            cart->persist_loaded = true;
        return;
    }

    if (cart->save_path[0] == '\0' && cart->rtc_path[0] == '\0')
    {
        return;
    }

    if (cart->has_ram && cart->ram_data && cart->ram_size > 0 && cart->save_path[0] != '\0')
    {
        FILE *f = fopen(cart->save_path, "rb");
        if (f)
        {
            size_t got = fread(cart->ram_data, 1, cart->ram_size, f);
            if (got < cart->ram_size)
            {
                memset(cart->ram_data + got, 0, cart->ram_size - got);
            }
            fclose(f);
        }
    }

    if (cart->has_rtc && cart->rtc_path[0] != '\0')
    {
        FILE *f = fopen(cart->rtc_path, "rb");
        if (f)
        {
            CartridgeRtcPersist rtc = {0};
            size_t got = fread(&rtc, 1, sizeof(rtc), f);
            fclose(f);
            if (got == sizeof(rtc) && memcmp(rtc.magic, "GBRTC1", 6) == 0)
            {
                memcpy(cart->mbc3_rtc_regs, rtc.regs, sizeof(cart->mbc3_rtc_regs));
                memcpy(cart->mbc3_rtc_latched, rtc.latched, sizeof(cart->mbc3_rtc_latched));
                cart->mbc3_latch_state = (uint8_t)(rtc.latch_state & 0x01u);
                cart->mbc3_rtc_latched_valid = (rtc.latched_valid != 0);
                cart->mbc3_rtc_last_unix = rtc.last_unix;
            }
        }
    }

    cart->ram_dirty = false;
    cart->rtc_dirty = false;
    cart->persist_loaded = true;
}

static void cartridge_persist_save(CartridgeState *cart, bool force)
{
    if (!cart || !cart->has_battery)
        return;

    if (cart->has_ram && cart->ram_data && cart->ram_size > 0 &&
        cart->save_path[0] != '\0' && (force || cart->ram_dirty))
    {
        FILE *f = fopen(cart->save_path, "wb");
        if (f)
        {
            size_t wrote = fwrite(cart->ram_data, 1, cart->ram_size, f);
            fclose(f);
            if (wrote == cart->ram_size)
                cart->ram_dirty = false;
        }
    }

    if (cart->has_rtc && cart->rtc_path[0] != '\0' && (force || cart->rtc_dirty))
    {
        mbc3_rtc_update(cart);
        CartridgeRtcPersist rtc = {0};
        memcpy(rtc.magic, "GBRTC1", 6);
        memcpy(rtc.regs, cart->mbc3_rtc_regs, sizeof(rtc.regs));
        memcpy(rtc.latched, cart->mbc3_rtc_latched, sizeof(rtc.latched));
        rtc.latch_state = (uint8_t)(cart->mbc3_latch_state & 0x01u);
        rtc.latched_valid = cart->mbc3_rtc_latched_valid ? 1u : 0u;
        rtc.last_unix = cart->mbc3_rtc_last_unix;

        FILE *f = fopen(cart->rtc_path, "wb");
        if (f)
        {
            size_t wrote = fwrite(&rtc, 1, sizeof(rtc), f);
            fclose(f);
            if (wrote == sizeof(rtc))
                cart->rtc_dirty = false;
        }
    }
}

static void cartridge_reset(CartridgeState *cart)
{
    memset(cart, 0, sizeof(*cart));
    cart->mbc_type = MBC_NONE;
    cart->mbc1_low5 = 1;
    cart->mbc2_rom_bank = 1;
    cart->mbc3_rom_bank = 1;
    cart->mbc5_rom_bank = 1;
}

static void cartridge_release(CartridgeState *cart)
{
    free(cart->rom_data);
    free(cart->ram_data);
    cartridge_reset(cart);
}

static void cartridge_configure(CartridgeState *cart, const uint8_t *rom_data, size_t rom_size)
{
    if (rom_size < 0x150)
    {
        return;
    }

    cart->cartridge_type = rom_data[0x0147];
    uint8_t rom_size_code = rom_data[0x0148];
    uint8_t ram_size_code = rom_data[0x0149];

    switch (cart->cartridge_type)
    {
    case 0x00:
        cart->mbc_type = MBC_NONE;
        break;
    case 0x01:
        cart->mbc_type = MBC1;
        break;
    case 0x02:
        cart->mbc_type = MBC1;
        cart->has_ram = true;
        break;
    case 0x03:
        cart->mbc_type = MBC1;
        cart->has_ram = true;
        cart->has_battery = true;
        break;
    case 0x05:
        cart->mbc_type = MBC2;
        cart->has_ram = true;
        break;
    case 0x06:
        cart->mbc_type = MBC2;
        cart->has_ram = true;
        cart->has_battery = true;
        break;
    case 0x08:
        cart->mbc_type = MBC_NONE;
        cart->has_ram = true;
        break;
    case 0x09:
        cart->mbc_type = MBC_NONE;
        cart->has_ram = true;
        cart->has_battery = true;
        break;
    case 0x0F:
        cart->mbc_type = MBC3;
        cart->has_battery = true;
        cart->has_rtc = true;
        break;
    case 0x10:
        cart->mbc_type = MBC3;
        cart->has_ram = true;
        cart->has_battery = true;
        cart->has_rtc = true;
        break;
    case 0x11:
        cart->mbc_type = MBC3;
        break;
    case 0x12:
        cart->mbc_type = MBC3;
        cart->has_ram = true;
        break;
    case 0x13:
        cart->mbc_type = MBC3;
        cart->has_ram = true;
        cart->has_battery = true;
        break;
    case 0x19:
        cart->mbc_type = MBC5;
        break;
    case 0x1A:
        cart->mbc_type = MBC5;
        cart->has_ram = true;
        break;
    case 0x1B:
        cart->mbc_type = MBC5;
        cart->has_ram = true;
        cart->has_battery = true;
        break;
    case 0x1C:
        cart->mbc_type = MBC5;
        break;
    case 0x1D:
        cart->mbc_type = MBC5;
        cart->has_ram = true;
        break;
    case 0x1E:
        cart->mbc_type = MBC5;
        cart->has_ram = true;
        cart->has_battery = true;
        break;
    default:
        // Fallback to ROM-only behavior for unknown types.
        cart->mbc_type = MBC_NONE;
        break;
    }

    size_t header_rom_banks = cartridge_rom_banks_from_header(rom_size_code);
    size_t file_rom_banks = cartridge_size_to_rom_banks(rom_size);
    if (header_rom_banks == 0 || header_rom_banks < file_rom_banks)
    {
        cart->rom_banks = file_rom_banks;
    }
    else
    {
        cart->rom_banks = header_rom_banks;
    }

    if (cart->mbc_type == MBC2)
    {
        cart->ram_size = 512;
    }
    else if (cart->has_ram)
    {
        cart->ram_size = cartridge_ram_size_from_header(ram_size_code);
    }

    cart->ram_banks = cartridge_size_to_ram_banks(cart->ram_size);

    if (cart->mbc_type == MBC_NONE)
    {
        cart->ram_enabled = true;
    }

    if (cart->has_rtc)
    {
        cart->mbc3_rtc_last_unix = mbc3_rtc_now_seconds();
        cart->mbc3_rtc_latched_valid = false;
    }
}

static size_t cartridge_wrap_bank(size_t bank, size_t bank_count)
{
    if (bank_count == 0)
        return 0;
    return bank % bank_count;
}

static uint64_t mbc3_rtc_now_seconds(void)
{
    time_t now = time(NULL);
    if (now < 0)
        return 0;
    return (uint64_t)now;
}

static void mbc3_rtc_update(CartridgeState *cart)
{
    if (!cart || !cart->has_rtc)
        return;

    uint64_t now = mbc3_rtc_now_seconds();
    if (now == 0)
        return;

    if (cart->mbc3_rtc_last_unix == 0)
    {
        cart->mbc3_rtc_last_unix = now;
        return;
    }

    if (cart->mbc3_rtc_regs[4] & 0x40u)
    {
        // Halt bit set: RTC stops advancing.
        cart->mbc3_rtc_last_unix = now;
        return;
    }

    uint64_t elapsed = now - cart->mbc3_rtc_last_unix;
    if (elapsed == 0)
        return;
    cart->mbc3_rtc_last_unix = now;

    uint64_t seconds = (uint64_t)(cart->mbc3_rtc_regs[0] % 60u);
    uint64_t minutes = (uint64_t)(cart->mbc3_rtc_regs[1] % 60u);
    uint64_t hours = (uint64_t)(cart->mbc3_rtc_regs[2] % 24u);
    uint64_t days = (uint64_t)cart->mbc3_rtc_regs[3] | ((uint64_t)(cart->mbc3_rtc_regs[4] & 0x01u) << 8u);

    uint64_t total = seconds + (minutes * 60u) + (hours * 3600u) + (days * 86400u);
    total += elapsed;

    uint64_t new_days = total / 86400u;
    uint8_t carry = (cart->mbc3_rtc_regs[4] & 0x80u);
    if (new_days > 511u)
    {
        carry = 0x80u;
        new_days %= 512u;
    }

    uint64_t rem = total % 86400u;
    cart->mbc3_rtc_regs[2] = (uint8_t)(rem / 3600u);
    rem %= 3600u;
    cart->mbc3_rtc_regs[1] = (uint8_t)(rem / 60u);
    cart->mbc3_rtc_regs[0] = (uint8_t)(rem % 60u);
    cart->mbc3_rtc_regs[3] = (uint8_t)(new_days & 0xFFu);
    cart->mbc3_rtc_regs[4] = (uint8_t)((cart->mbc3_rtc_regs[4] & 0x40u) | (uint8_t)((new_days >> 8u) & 0x01u) | carry);
}

static void mbc3_rtc_latch(CartridgeState *cart)
{
    if (!cart || !cart->has_rtc)
        return;
    mbc3_rtc_update(cart);
    for (int i = 0; i < 5; ++i)
        cart->mbc3_rtc_latched[i] = cart->mbc3_rtc_regs[i];
    cart->mbc3_rtc_latched_valid = true;
}

static uint8_t cartridge_rom_read(const MemoryState *mem, uint16_t address)
{
    const CartridgeState *cart = &mem->cartridge;
    if (!cart->rom_data || cart->rom_size == 0 || address >= 0x8000)
    {
        return 0xFF;
    }

    size_t bank = 0;
    size_t bank_offset = (size_t)(address & 0x3FFF);

    switch (cart->mbc_type)
    {
    case MBC1:
        if (address < 0x4000)
        {
            if (cart->mbc1_mode)
            {
                bank = (size_t)(cart->mbc1_high2 & 0x03) << 5;
            }
            else
            {
                bank = 0;
            }
        }
        else
        {
            uint8_t low = (uint8_t)(cart->mbc1_low5 & 0x1F);
            if (low == 0)
                low = 1;
            bank = low | ((size_t)(cart->mbc1_high2 & 0x03) << 5);
        }
        break;
    case MBC2:
        bank = (address < 0x4000) ? 0 : (size_t)(cart->mbc2_rom_bank & 0x0F);
        if (address >= 0x4000 && bank == 0)
            bank = 1;
        break;
    case MBC3:
        bank = (address < 0x4000) ? 0 : (size_t)(cart->mbc3_rom_bank & 0x7F);
        if (address >= 0x4000 && bank == 0)
            bank = 1;
        break;
    case MBC5:
        bank = (address < 0x4000) ? 0 : (size_t)(cart->mbc5_rom_bank & 0x01FF);
        break;
    case MBC_NONE:
    default:
        bank = (address < 0x4000) ? 0 : 1;
        break;
    }

    bank = cartridge_wrap_bank(bank, cart->rom_banks);
    size_t absolute = bank * 0x4000u + bank_offset;
    if (absolute >= cart->rom_size)
    {
        return 0xFF;
    }
    rom_read_trace_mark(absolute);
    if (mem->cpu && mem->cpu->profile_data_reads_active)
    {
        rom_data_read_mark(absolute);
    }
    return cart->rom_data[absolute];
}

static uint8_t cartridge_ram_read(MemoryState *mem, uint16_t address)
{
    CartridgeState *cart = &mem->cartridge;
    if (address < 0xA000 || address > 0xBFFF || !cart->has_ram || !cart->ram_data || cart->ram_size == 0)
    {
        return 0xFF;
    }

    if (!cart->ram_enabled && cart->mbc_type != MBC_NONE)
    {
        return 0xFF;
    }

    size_t offset = (size_t)(address - 0xA000);
    size_t bank = 0;

    switch (cart->mbc_type)
    {
    case MBC1:
        bank = cart->mbc1_mode ? (size_t)(cart->mbc1_high2 & 0x03) : 0;
        break;
    case MBC2:
    {
        size_t idx = offset & 0x01FFu;
        if (idx >= cart->ram_size)
            return 0xFF;
        return (uint8_t)(0xF0u | (cart->ram_data[idx] & 0x0Fu));
    }
    case MBC3:
        if (cart->mbc3_ram_rtc_select <= 0x03)
        {
            bank = cart->mbc3_ram_rtc_select;
        }
        else if (cart->has_rtc && cart->mbc3_ram_rtc_select >= 0x08 && cart->mbc3_ram_rtc_select <= 0x0C)
        {
            mbc3_rtc_update(cart);
            uint8_t idx = (uint8_t)(cart->mbc3_ram_rtc_select - 0x08u);
            if (idx < 5)
            {
                if (cart->mbc3_rtc_latched_valid)
                    return cart->mbc3_rtc_latched[idx];
                return cart->mbc3_rtc_regs[idx];
            }
            return 0xFF;
        }
        else
        {
            return 0xFF;
        }
        break;
    case MBC5:
        bank = (size_t)(cart->mbc5_ram_bank & 0x0F);
        break;
    case MBC_NONE:
    default:
        bank = 0;
        break;
    }

    bank = cartridge_wrap_bank(bank, cart->ram_banks == 0 ? 1 : cart->ram_banks);
    size_t absolute = bank * 0x2000u + offset;
    if (absolute >= cart->ram_size)
    {
        return 0xFF;
    }
    return cart->ram_data[absolute];
}

static void cartridge_ram_write(MemoryState *mem, uint16_t address, uint8_t value)
{
    CartridgeState *cart = &mem->cartridge;
    if (address < 0xA000 || address > 0xBFFF || !cart->has_ram || !cart->ram_data || cart->ram_size == 0)
    {
        return;
    }

    if (!cart->ram_enabled && cart->mbc_type != MBC_NONE)
    {
        return;
    }

    size_t offset = (size_t)(address - 0xA000);
    size_t bank = 0;

    switch (cart->mbc_type)
    {
    case MBC1:
        bank = cart->mbc1_mode ? (size_t)(cart->mbc1_high2 & 0x03) : 0;
        break;
    case MBC2:
    {
        size_t idx = offset & 0x01FFu;
        if (idx < cart->ram_size)
        {
            cart->ram_data[idx] = (uint8_t)(value & 0x0Fu);
            if (cart->has_battery)
                cart->ram_dirty = true;
        }
        return;
    }
    case MBC3:
        if (cart->mbc3_ram_rtc_select <= 0x03)
        {
            bank = cart->mbc3_ram_rtc_select;
        }
        else if (cart->has_rtc && cart->mbc3_ram_rtc_select >= 0x08 && cart->mbc3_ram_rtc_select <= 0x0C)
        {
            mbc3_rtc_update(cart);
            uint8_t idx = (uint8_t)(cart->mbc3_ram_rtc_select - 0x08u);
            if (idx < 5)
            {
                switch (idx)
                {
                case 0:
                    cart->mbc3_rtc_regs[0] = (uint8_t)(value % 60u);
                    break;
                case 1:
                    cart->mbc3_rtc_regs[1] = (uint8_t)(value % 60u);
                    break;
                case 2:
                    cart->mbc3_rtc_regs[2] = (uint8_t)(value % 24u);
                    break;
                case 3:
                    cart->mbc3_rtc_regs[3] = value;
                    break;
                case 4:
                    cart->mbc3_rtc_regs[4] = (uint8_t)(value & 0xC1u); // day high + halt + carry
                    break;
                default:
                    break;
                }
                cart->mbc3_rtc_last_unix = mbc3_rtc_now_seconds();
                if (cart->has_battery)
                    cart->rtc_dirty = true;
            }
            return;
        }
        else
        {
            return;
        }
        break;
    case MBC5:
        bank = (size_t)(cart->mbc5_ram_bank & 0x0F);
        break;
    case MBC_NONE:
    default:
        bank = 0;
        break;
    }

    bank = cartridge_wrap_bank(bank, cart->ram_banks == 0 ? 1 : cart->ram_banks);
    size_t absolute = bank * 0x2000u + offset;
    if (absolute < cart->ram_size)
    {
        cart->ram_data[absolute] = value;
        if (cart->has_battery)
            cart->ram_dirty = true;
    }
}

static void cartridge_write_control(MemoryState *mem, uint16_t address, uint8_t value)
{
    CartridgeState *cart = &mem->cartridge;
    if (!cart->rom_data || address >= 0x8000)
    {
        return;
    }

    bool changed = false;
    bool flush_battery = false;

    switch (cart->mbc_type)
    {
    case MBC1:
        if (address <= 0x1FFF)
        {
            bool ram_enable = ((value & 0x0F) == 0x0A);
            if (cart->ram_enabled != ram_enable)
            {
                if (cart->ram_enabled && !ram_enable)
                    flush_battery = true;
                cart->ram_enabled = ram_enable;
                changed = true;
            }
        }
        else if (address <= 0x3FFF)
        {
            uint8_t low5 = (uint8_t)(value & 0x1F);
            if (low5 == 0)
                low5 = 1;
            if (cart->mbc1_low5 != low5)
            {
                cart->mbc1_low5 = low5;
                changed = true;
            }
        }
        else if (address <= 0x5FFF)
        {
            uint8_t high2 = (uint8_t)(value & 0x03);
            if (cart->mbc1_high2 != high2)
            {
                cart->mbc1_high2 = high2;
                changed = true;
            }
        }
        else
        {
            uint8_t mode = (uint8_t)(value & 0x01);
            if (cart->mbc1_mode != mode)
            {
                cart->mbc1_mode = mode;
                changed = true;
            }
        }
        break;
    case MBC2:
        if (address <= 0x3FFF)
        {
            if ((address & 0x0100) == 0)
            {
                bool ram_enable = ((value & 0x0F) == 0x0A);
                if (cart->ram_enabled != ram_enable)
                {
                    if (cart->ram_enabled && !ram_enable)
                        flush_battery = true;
                    cart->ram_enabled = ram_enable;
                    changed = true;
                }
            }
            else
            {
                uint8_t bank = (uint8_t)(value & 0x0F);
                if (bank == 0)
                    bank = 1;
                if (cart->mbc2_rom_bank != bank)
                {
                    cart->mbc2_rom_bank = bank;
                    changed = true;
                }
            }
        }
        break;
    case MBC3:
        if (address <= 0x1FFF)
        {
            bool ram_enable = ((value & 0x0F) == 0x0A);
            if (cart->ram_enabled != ram_enable)
            {
                if (cart->ram_enabled && !ram_enable)
                    flush_battery = true;
                cart->ram_enabled = ram_enable;
                changed = true;
            }
        }
        else if (address <= 0x3FFF)
        {
            mbc3_rtc_update(cart);
            uint8_t bank = (uint8_t)(value & 0x7F);
            if (bank == 0)
                bank = 1;
            if (cart->mbc3_rom_bank != bank)
            {
                cart->mbc3_rom_bank = bank;
                changed = true;
            }
        }
        else if (address <= 0x5FFF)
        {
            mbc3_rtc_update(cart);
            uint8_t select = (uint8_t)(value & 0x0F);
            if (cart->mbc3_ram_rtc_select != select)
            {
                cart->mbc3_ram_rtc_select = select;
                changed = true;
            }
        }
        else
        {
            uint8_t latch = (uint8_t)(value & 0x01);
            if (cart->mbc3_latch_state == 0 && latch == 1)
            {
                mbc3_rtc_latch(cart);
                changed = true;
            }
            if (cart->mbc3_latch_state != latch)
            {
                cart->mbc3_latch_state = latch;
                changed = true;
            }
        }
        break;
    case MBC5:
        if (address <= 0x1FFF)
        {
            bool ram_enable = ((value & 0x0F) == 0x0A);
            if (cart->ram_enabled != ram_enable)
            {
                if (cart->ram_enabled && !ram_enable)
                    flush_battery = true;
                cart->ram_enabled = ram_enable;
                changed = true;
            }
        }
        else if (address <= 0x2FFF)
        {
            uint16_t bank = (uint16_t)((cart->mbc5_rom_bank & 0x0100u) | value);
            if (cart->mbc5_rom_bank != bank)
            {
                cart->mbc5_rom_bank = bank;
                changed = true;
            }
        }
        else if (address <= 0x3FFF)
        {
            uint16_t bank = (uint16_t)((cart->mbc5_rom_bank & 0x00FFu) | ((value & 0x01u) << 8u));
            if (cart->mbc5_rom_bank != bank)
            {
                cart->mbc5_rom_bank = bank;
                changed = true;
            }
        }
        else if (address <= 0x5FFF)
        {
            uint8_t bank = (uint8_t)(value & 0x0F);
            if (cart->mbc5_ram_bank != bank)
            {
                cart->mbc5_ram_bank = bank;
                changed = true;
            }
        }
        break;
    case MBC_NONE:
    default:
        break;
    }

    if (changed)
    {
        mbc_trace_log(mem, "CTRL", address, value);
    }
    if (flush_battery && cart->has_battery)
    {
        cartridge_persist_save(cart, false);
    }
}

static uint8_t joypad_read(const MemoryState *mem)
{
    uint8_t select = mem->joypad_select & 0x30;
    uint8_t value = (uint8_t)(0xC0 | select);
    uint8_t low = 0x0F;

    // P15 (bit 5) low: select action buttons (A, B, Select, Start)
    if ((select & 0x20) == 0)
    {
        low &= mem->joypad_buttons;
    }
    // P14 (bit 4) low: select d-pad (Right, Left, Up, Down)
    if ((select & 0x10) == 0)
    {
        low &= mem->joypad_dpad;
    }

    value |= low;
    return value;
}

static bool joypad_is_dpad_input(JoypadInput input)
{
    switch (input)
    {
    case JOYPAD_RIGHT:
    case JOYPAD_LEFT:
    case JOYPAD_UP:
    case JOYPAD_DOWN:
        return true;
    case JOYPAD_A:
    case JOYPAD_B:
    case JOYPAD_SELECT:
    case JOYPAD_START:
        return false;
    default:
        return true;
    }
}

static uint8_t joypad_input_bit(JoypadInput input)
{
    switch (input)
    {
    case JOYPAD_RIGHT:
    case JOYPAD_A:
        return 0x01; // bit 0
    case JOYPAD_LEFT:
    case JOYPAD_B:
        return 0x02; // bit 1
    case JOYPAD_UP:
    case JOYPAD_SELECT:
        return 0x04; // bit 2
    case JOYPAD_DOWN:
    case JOYPAD_START:
        return 0x08; // bit 3
    default:
        return 0x00;
    }
}

uint8_t memory_raw_read(MemoryState *mem, uint16_t address)
{
    if (!mem->memory.BOOT && mem->bios_enabled)
    {
        /* DMG BIOS: 0x0000-0x00FF */
        if (address < 0x0100)
            return mem->bios[address];
        /* CGB BIOS: 0x0200-0x08FF (stored at bios[0x200..0x8FF]) */
        if (mem->bios_size == 0x900 && address >= 0x0200 && address < 0x0900)
            return mem->bios[address];
    }

    if (address < 0x8000)
    {
        if (mem->cartridge.rom_data)
            return cartridge_rom_read(mem, address);
        return mem->memory.data[address];
    }

    /* GBC VRAM banking: bank 1 in vram_extra */
    if (mem->gbc_mode && address >= 0x8000 && address <= 0x9FFF && mem->vram_bank == 1)
    {
        return mem->vram_extra[address - 0x8000u];
    }

    if (address >= 0xA000 && address <= 0xBFFF)
    {
        if (mem->cartridge.rom_data)
            return cartridge_ram_read(mem, address);
        return mem->memory.data[address];
    }

    /* GBC WRAM banking: D000-DFFF maps to banks 1-7 */
    if (mem->gbc_mode && address >= 0xD000 && address <= 0xDFFF)
    {
        uint8_t bank = mem->wram_bank;
        if (bank == 0) bank = 1;
        return mem->wram_extra[(bank - 1) * 0x1000 + (address - 0xD000u)];
    }

    if (address >= 0xE000 && address <= 0xFDFF)
    {
        address = (uint16_t)(address - 0x2000);
        /* Also handle D000-DFFF after echo remap */
        if (mem->gbc_mode && address >= 0xD000 && address <= 0xDFFF)
        {
            uint8_t bank = mem->wram_bank;
            if (bank == 0) bank = 1;
            return mem->wram_extra[(bank - 1) * 0x1000 + (address - 0xD000u)];
        }
    }
    if (address >= 0xFEA0 && address <= 0xFEFF)
    {
        return 0x00;
    }

    /* GBC I/O register reads */
    if (mem->gbc_mode)
    {
        switch (address)
        {
        case 0xFF4D: return mem->key1 | 0x7E; /* bit 7 = current speed, bit 0 = prepare, bits 1-6 = 1 */
        case 0xFF4F: return mem->vram_bank | 0xFE;
        case 0xFF51: return mem->hdma1;
        case 0xFF52: return mem->hdma2;
        case 0xFF53: return mem->hdma3;
        case 0xFF54: return mem->hdma4;
        case 0xFF55: return mem->hdma_active ? (uint8_t)(((mem->hdma_remain / 16) - 1) & 0x7F)
                                              : 0xFF;
        case 0xFF56: return mem->rp | 0x3C; /* infrared stub: bits 2-5 unused = 1 */
        case 0xFF68: return mem->bgpi;
        case 0xFF69: return mem->bg_cram[mem->bgpi & 0x3F];
        case 0xFF6A: return mem->obpi;
        case 0xFF6B: return mem->obj_cram[mem->obpi & 0x3F];
        case 0xFF6C: return mem->opri | 0xFE;
        case 0xFF70: return mem->wram_bank | 0xF8;
        case 0xFF72: return mem->undoc_ff72;
        case 0xFF73: return mem->undoc_ff73;
        case 0xFF74: return mem->undoc_ff74;
        case 0xFF75: return mem->undoc_ff75 | 0x8F;
        case 0xFF76: return 0x00; /* PCM12 – read only, not used yet */
        case 0xFF77: return apu_ch3_pcm_nibble(mem); /* PCM34 low nibble = CH3 */
        default: break;
        }
    }

    switch (address)
    {
    case 0xFF02:
        return (uint8_t)((mem->memory.SC & 0x81u) | 0x7Eu);
    case 0xFF0F:
        return (uint8_t)((mem->memory.IF & 0x1Fu) | 0xE0u);
    case 0xFF10:
    case 0xFF11:
    case 0xFF12:
    case 0xFF13:
    case 0xFF14:
    case 0xFF15:
    case 0xFF16:
    case 0xFF17:
    case 0xFF18:
    case 0xFF19:
    case 0xFF1A:
    case 0xFF1B:
    case 0xFF1C:
    case 0xFF1D:
    case 0xFF1E:
    case 0xFF1F:
    case 0xFF20:
    case 0xFF21:
    case 0xFF22:
    case 0xFF23:
    case 0xFF24:
    case 0xFF25:
        return (uint8_t)(mem->memory.data[address] | apu_read_mask(address));
    case 0xFF26:
        return (uint8_t)((mem->memory.NR52 & 0x8Fu) | 0x70u);
    case 0xFF41:
        ppu_sync_to_cpu(mem);
        return mem->memory.STAT;
    case 0xFF44:
        ppu_sync_to_cpu(mem);
        return mem->memory.LY;
    case 0xFF30:
    case 0xFF31:
    case 0xFF32:
    case 0xFF33:
    case 0xFF34:
    case 0xFF35:
    case 0xFF36:
    case 0xFF37:
    case 0xFF38:
    case 0xFF39:
    case 0xFF3A:
    case 0xFF3B:
    case 0xFF3C:
    case 0xFF3D:
    case 0xFF3E:
    case 0xFF3F:
        if (!apu_ch3_runtime_active(mem))
            return mem->memory.WAVE_RAM[address - 0xFF30u];
        if (mem->gbc_mode)
            return mem->memory.WAVE_RAM[mem->apu_ch3_current_index & 0x0Fu];
        if (apu_ch3_dmg_accessible_now(mem))
            return apu_ch3_dmg_fetch_byte_now(mem);
        return 0xFFu;
    case 0xFF27:
    case 0xFF28:
    case 0xFF29:
    case 0xFF2A:
    case 0xFF2B:
    case 0xFF2C:
    case 0xFF2D:
    case 0xFF2E:
    case 0xFF2F:
        return 0xFFu;
    default:
        break;
    }

    return mem->memory.data[address];
}

static void memory_raw_write(MemoryState *mem, uint16_t address, uint8_t value)
{
    if (address < 0x8000)
    {
        cartridge_write_control(mem, address, value);
        return;
    }

    /* GBC VRAM banking: bank 1 goes to vram_extra */
    if (mem->gbc_mode && address >= 0x8000 && address <= 0x9FFF && mem->vram_bank == 1)
    {
        mem->vram_extra[address - 0x8000u] = value;
        return;
    }

    if (address >= 0xA000 && address <= 0xBFFF)
    {
        if (mem->cartridge.rom_data)
        {
            cartridge_ram_write(mem, address, value);
            return;
        }
        mem->memory.data[address] = value;
        return;
    }

    /* GBC WRAM banking: D000-DFFF maps to banks 1-7 */
    if (mem->gbc_mode && address >= 0xD000 && address <= 0xDFFF)
    {
        uint8_t bank = mem->wram_bank;
        if (bank == 0) bank = 1;
        mem->wram_extra[(bank - 1) * 0x1000 + (address - 0xD000u)] = value;
        mem->memory.data[address] = value; /* also mirror into flat map */
        return;
    }

    if (address >= 0xE000 && address <= 0xFDFF)
    {
        uint16_t mirror = (uint16_t)(address - 0x2000);
        mem->memory.data[address] = value;
        if (mem->gbc_mode && mirror >= 0xD000 && mirror <= 0xDFFF)
        {
            uint8_t bank = mem->wram_bank;
            if (bank == 0) bank = 1;
            mem->wram_extra[(bank - 1) * 0x1000 + (mirror - 0xD000u)] = value;
        }
        mem->memory.data[mirror] = value;
        return;
    }

    if (address >= 0xFEA0 && address <= 0xFEFF)
    {
        return;
    }

    if ((address >= 0xFF10 && address <= 0xFF3F))
    {
        apu_write(mem, address, value);
        return;
    }

    /* GBC I/O register writes */
    if (mem->gbc_mode)
    {
        switch (address)
        {
        case 0xFF4D:
            /* Only bit 0 (prepare flag) is writable */
            mem->key1 = (uint8_t)((mem->key1 & 0x80) | (value & 0x01));
            io_trace_write(mem, address, value);
            return;
        case 0xFF4F:
            mem->vram_bank = value & 0x01;
            return;
        case 0xFF51:
            mem->hdma1 = value;
            return;
        case 0xFF52:
            mem->hdma2 = value & 0xF0; /* lower 4 bits ignored */
            return;
        case 0xFF53:
            mem->hdma3 = value & 0x1F; /* only bits 0-4, dest in 0x8000-0x9FF0 */
            return;
        case 0xFF54:
            mem->hdma4 = value & 0xF0; /* lower 4 bits ignored */
            return;
        case 0xFF55:
        {
            if (mem->hdma_active && !(value & 0x80))
            {
                /* Writing bit 7 = 0 while HDMA active → cancel */
                mem->hdma_active = false;
                mem->hdma5 = (uint8_t)(((mem->hdma_remain / 16) - 1) | 0x80);
                return;
            }
            /* Set up new transfer */
            mem->hdma_src = (uint16_t)((mem->hdma1 << 8) | mem->hdma2);
            mem->hdma_dst = (uint16_t)(0x8000 | ((mem->hdma3 << 8) | mem->hdma4));
            uint16_t length = (uint16_t)(((value & 0x7F) + 1) * 16);
            mem->hdma_remain = length;

            if (value & 0x80)
            {
                /* HBlank HDMA – transfer 16 bytes each HBlank */
                mem->hdma_active = true;
                mem->hdma5 = value & 0x7F;
            }
            else
            {
                /* General-purpose DMA – transfer all bytes immediately */
                mem->hdma_active = false;
                for (uint16_t i = 0; i < length; ++i)
                {
                    uint8_t b = memory_raw_read(mem, (uint16_t)(mem->hdma_src + i));
                    uint16_t dst = (uint16_t)(mem->hdma_dst + i);
                    /* Destination wraps within VRAM (0x8000-0x9FFF) */
                    uint16_t voff = (uint16_t)((dst - 0x8000u) & 0x1FFFu);
                    if (mem->vram_bank == 1)
                        mem->vram_extra[voff] = b;
                    else
                        mem->memory.data[0x8000u + voff] = b;
                }
                mem->hdma_src += length;
                mem->hdma_dst += length;
                mem->hdma_remain = 0;
                mem->hdma5 = 0xFF; /* transfer complete */
                /* GDMA costs (length/16) * 8 double-speed cycles or *16 normal */
                if (mem->cpu)
                {
                    uint32_t gdma_cycles = (uint32_t)(length / 16) * (mem->double_speed ? 16u : 8u);
                    mem->cpu->cycle_count += gdma_cycles;
                }
            }
            return;
        }
        case 0xFF56:
            mem->rp = value;
            return;
        case 0xFF68:
            mem->bgpi = value;
            return;
        case 0xFF69:
        {
            uint8_t idx = mem->bgpi & 0x3F;
            mem->bg_cram[idx] = value;
            if (mem->bgpi & 0x80)
                mem->bgpi = (uint8_t)(0x80 | ((idx + 1) & 0x3F));
            return;
        }
        case 0xFF6A:
            mem->obpi = value;
            return;
        case 0xFF6B:
        {
            uint8_t idx = mem->obpi & 0x3F;
            mem->obj_cram[idx] = value;
            if (mem->obpi & 0x80)
                mem->obpi = (uint8_t)(0x80 | ((idx + 1) & 0x3F));
            return;
        }
        case 0xFF6C:
            mem->opri = value & 0x01;
            return;
        case 0xFF70:
            mem->wram_bank = value & 0x07;
            return;
        case 0xFF72:
            mem->undoc_ff72 = value;
            return;
        case 0xFF73:
            mem->undoc_ff73 = value;
            return;
        case 0xFF74:
            mem->undoc_ff74 = value;
            return;
        case 0xFF75:
            mem->undoc_ff75 = value & 0x70;
            return;
        default:
            break;
        }
    }

    /*
     * DMG compatibility on CGB: when a DMG game writes BGP / OBP0 / OBP1,
     * remap the boot-ROM base palette colours through the shade mapping
     * and write the result into CRAM so Mode8 picks them up.
     */
    if (mem->dmg_compat)
    {
        if (address == 0xFF47 || address == 0xFF48 || address == 0xFF49)
        {
            const uint16_t *base = NULL;
            uint8_t *cram = NULL;
            uint8_t  pal  = 0;
            if (address == 0xFF47)       { base = mem->dmg_bg_pal;   cram = mem->bg_cram;  pal = 0; }
            else if (address == 0xFF48)  { base = mem->dmg_obj0_pal; cram = mem->obj_cram; pal = 0; }
            else                         { base = mem->dmg_obj1_pal; cram = mem->obj_cram; pal = 1; }

            for (uint8_t c = 0; c < 4; ++c)
            {
                uint8_t shade = (value >> (c * 2)) & 0x03;
                uint16_t rgb  = base[shade];
                size_t off    = (size_t)pal * 8 + (size_t)c * 2;
                cram[off]     = (uint8_t)(rgb & 0xFF);
                cram[off + 1] = (uint8_t)(rgb >> 8);
            }
        }
    }

    mem->memory.data[address] = value;
}

void memory_init(MemoryState *mem)
{
    memset(mem, 0, sizeof(*mem));
    mbc_trace_init();
    rom_read_trace_init();
    io_trace_init();
    memory_clear_fetch_patch();
    cartridge_reset(&mem->cartridge);
    rom_read_trace_reset(0);
    mem->joypad_read_count = 0;
    mem->bios_enabled = false;
    mem->debug_print_enabled = false;
    mem->joypad_buttons = 0x0F;
    mem->joypad_dpad = 0x0F;
    mem->joypad_select = 0x30;
    mem->memory.P1_JOYP = (uint8_t)(0xC0 | mem->joypad_select | 0x0F);
}

void memory_shutdown(MemoryState *mem)
{
    if (!mem)
        return;
    cartridge_persist_save(&mem->cartridge, true);
    memory_clear_fetch_patch();
    rom_read_trace_flush();
    rom_read_trace_reset(0);
    cartridge_release(&mem->cartridge);
}

static int load_rom_image(MemoryState *mem, uint8_t *rom_data, size_t rom_size, const char *rom_path_hint)
{
    cartridge_release(&mem->cartridge);
    mem->cartridge.rom_data = rom_data;
    mem->cartridge.rom_size = rom_size;
    cartridge_configure(&mem->cartridge, rom_data, rom_size);
    cartridge_set_persist_paths(&mem->cartridge, rom_path_hint);

    if (mem->cartridge.has_ram && mem->cartridge.ram_size > 0)
    {
        mem->cartridge.ram_data = (uint8_t *)calloc(1, mem->cartridge.ram_size);
        if (!mem->cartridge.ram_data)
        {
            cartridge_release(&mem->cartridge);
            return -1;
        }
    }
    cartridge_persist_load(&mem->cartridge);

    memset(mem->memory.rom, 0xFF, sizeof(mem->memory.rom));
    size_t copy_size = mem->cartridge.rom_size < sizeof(mem->memory.rom) ? mem->cartridge.rom_size : sizeof(mem->memory.rom);
    memcpy(mem->memory.rom, mem->cartridge.rom_data, copy_size);
    rom_read_trace_reset(mem->cartridge.rom_size);

    if (memory_logging_enabled)
    {
        static const char *mbc_names[] = {"None", "MBC1", "MBC2", "MBC3", "MBC5"};
        const char *mbc_str = (mem->cartridge.mbc_type <= MBC5)
                                  ? mbc_names[mem->cartridge.mbc_type]
                                  : "Unknown";
        printf("Cartridge: type=%02X (%s), MBC=%s, ROM=%zu banks, RAM=%zu bytes\n",
               mem->cartridge.cartridge_type,
               cartridge_type_name(mem->cartridge.cartridge_type),
               mbc_str,
               mem->cartridge.rom_banks,
               mem->cartridge.ram_size);
    }

    mbc_trace_log(mem, "LOAD", 0, 0);

    return 0;
}

int load_rom(const char *path, MemoryState *mem)
{
    mbc_trace_init();
    rom_read_trace_init();

    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    if (size <= 0)
    {
        fclose(f);
        return -1;
    }

    uint8_t *rom_data = (uint8_t *)malloc((size_t)size);
    if (!rom_data)
    {
        fclose(f);
        return -1;
    }

    if (fread(rom_data, 1, (size_t)size, f) != (size_t)size)
    {
        free(rom_data);
        fclose(f);
        return -1;
    }
    fclose(f);

    return load_rom_image(mem, rom_data, (size_t)size, path);
}

int load_rom_from_buffer(const uint8_t *data, size_t size, MemoryState *mem)
{
    mbc_trace_init();
    rom_read_trace_init();

    if (!data || size == 0)
        return -1;

    uint8_t *rom_data = (uint8_t *)malloc(size);
    if (!rom_data)
        return -1;

    memcpy(rom_data, data, size);
    return load_rom_image(mem, rom_data, size, NULL);
}

void memory_set_save_path_hint(MemoryState *mem, const char *rom_path)
{
    if (!mem)
        return;
    cartridge_set_persist_paths(&mem->cartridge, rom_path);
    if (!mem->cartridge.persist_loaded)
        cartridge_persist_load(&mem->cartridge);
}

int load_bios(const char *path, uint8_t *bios, uint16_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size != 0x100 && size != 0x900)
    {
        fclose(f);
        return -1;
    }
    if (fread(bios, 1, (size_t)size, f) != (size_t)size)
    {
        fclose(f);
        return -1;
    }
    fclose(f);
    if (out_size) *out_size = (uint16_t)size;
    return 0;
}

static inline void memory_probe_data_write(MemoryState *mem, uint16_t address, uint8_t value)
{
    (void)mem; (void)address; (void)value;
}

uint8_t memory_read(MemoryState *mem, uint16_t address)
{
    uint8_t value = 0xFF;
    if (fetch_patch.active)
    {
        uint16_t delta = (uint16_t)(address - fetch_patch.start);
        if (delta < fetch_patch.len)
        {
            value = fetch_patch.bytes[delta];
            return value;
        }
    }

    if (address == 0xFF00)
    {
        mem->joypad_read_count++;
        value = joypad_read(mem);
        mem->memory.P1_JOYP = value;
        return value;
    }

    if (address >= 0x8000 && address <= 0x9FFF && ppu_vram_locked(mem))
        return 0xFF;

    if (address >= 0xFE00 && address <= 0xFEFF && ppu_oam_locked(mem))
    {
        return 0xFF;
    }

    return memory_raw_read(mem, address);
}

void memory_write(MemoryState *mem, uint16_t address, uint8_t value)
{
    if (mem->bios_enabled && address == 0xFF50 && value != 0)
    {
        if (memory_logging_enabled)
        {
            printf("Disabling BIOS mapping at address  \n");
        }
        mem->bios_enabled = false;
    }

    if (address >= 0x8000 && address <= 0x9FFF && ppu_vram_locked(mem))
    {
        return;
    }

    if (address >= 0xFE00 && address <= 0xFEFF && ppu_oam_locked(mem))
    {
        memory_oam_bug_idu_incdec(mem, address);
        return;
    }

    switch (address)
    {
    case 0xFF00:
    {
        mem->joypad_select = value & 0x30;
        uint8_t joy = joypad_read(mem);
        mem->memory.P1_JOYP = joy;
        memory_probe_data_write(mem, address, joy);
        return;
    }
    case 0xFF04: // DIV
        if (mem->cpu)
        {
            cpu_timer_div_write(mem->cpu);
            memory_probe_data_write(mem, address, mem->memory.DIV);
        }
        else
        {
            mem->memory.DIV = 0;
            memory_probe_data_write(mem, address, mem->memory.DIV);
        }
        return;
    case 0xFF05: // TIMA
        if (mem->cpu)
        {
            cpu_timer_tima_write(mem->cpu, value);
            memory_probe_data_write(mem, address, mem->memory.TIMA);
        }
        else
        {
            mem->memory.TIMA = value;
            memory_probe_data_write(mem, address, mem->memory.TIMA);
        }
        return;
    case 0xFF06: // TMA
        if (mem->cpu)
        {
            cpu_timer_tma_write(mem->cpu, value);
            memory_probe_data_write(mem, address, mem->memory.TMA);
        }
        else
        {
            mem->memory.TMA = value;
            memory_probe_data_write(mem, address, mem->memory.TMA);
        }
        return;
    case 0xFF07: // TAC
        if (mem->cpu)
        {
            cpu_timer_tac_write(mem->cpu, value);
            memory_probe_data_write(mem, address, mem->memory.TAC);
        }
        else
        {
            mem->memory.TAC = (uint8_t)(value | 0xF8);
            memory_probe_data_write(mem, address, mem->memory.TAC);
        }
        return;
    case 0xFF0F:
        mem->memory.IF = value & 0x1F;
        io_trace_write(mem, address, value);
        memory_probe_data_write(mem, address, mem->memory.IF);
        return;
    case 0xFF01:
        mem->memory.SB = value;
        memory_probe_data_write(mem, address, mem->memory.SB);
        return;
    case 0xFF02:
        mem->memory.SC = value;
        if (mem->debug_print_enabled && (value & 0x80u) != 0 && (value & 0x01u) != 0)
        {
            putchar((int)mem->memory.SB);
            fflush(stdout);
            mem->memory.SC &= (uint8_t)~0x80u;
            mem->memory.IF |= IF_SERIAL;
        }
        memory_probe_data_write(mem, address, mem->memory.SC);
        return;
    case 0xFF46: // DMA
    {
        mem->memory.DMA = value;
        memory_probe_data_write(mem, address, mem->memory.DMA);
        uint16_t source = (uint16_t)(value << 8);
        for (int i = 0; i < 0xA0; ++i)
        {
            /* OAM DMA uses the bus directly – not subject to PPU mode locks */
            uint8_t data = memory_raw_read(mem, (uint16_t)(source + i));
            mem->memory.oam[i] = data;
            memory_probe_data_write(mem, (uint16_t)(0xFE00u + i), data);
        }
        if (mem->cpu)
        {
            cpu_dma_transfer(mem->cpu, source);
        }
        return;
    }
    case 0xFF41:
        uint8_t keep_ro = mem->memory.STAT & 0x07;
        uint8_t new_rw = (value & 0x78);
        mem->memory.STAT = keep_ro | new_rw;
        memory_probe_data_write(mem, address, mem->memory.STAT);
        return;
    case 0xFF40:
    {
        uint8_t old_lcdc = mem->memory.LCDC;
        bool old_enabled = (old_lcdc & 0x80u) != 0;
        bool new_enabled;

        ppu_sync_to_cpu(mem);
        mem->memory.LCDC = value;
        new_enabled = (value & 0x80u) != 0;

        if (mem->ppu)
        {
            if (!old_enabled && new_enabled)
            {
                ppu_prepare_lcd_enable(mem->ppu);
            }
            else if (old_enabled && !new_enabled)
            {
                mem->ppu->lcd_startup_delay_dots = 0;
            }
        }

        memory_probe_data_write(mem, address, mem->memory.LCDC);
        return;
    }
    case 0xFF44:
        mem->memory.LY = 0;
        memory_probe_data_write(mem, address, mem->memory.LY);
        return;
    case 0xFFFF:
        mem->memory.IE = value & 0x1F;
        io_trace_write(mem, address, value);
        memory_probe_data_write(mem, address, mem->memory.IE);
        return;

    default:
        break;
    }

    memory_raw_write(mem, address, value);
    memory_probe_data_write(mem, address, value);
}

void memory_set_button_state(MemoryState *mem, JoypadInput input, bool pressed)
{
    uint8_t bit = joypad_input_bit(input);
    if (bit == 0)
        return;

    bool is_dpad = joypad_is_dpad_input(input);
    uint8_t old_joyp = joypad_read(mem);

    if (is_dpad)
    {
        if (pressed)
        {
            mem->joypad_dpad &= (uint8_t)~bit;
        }
        else
        {
            mem->joypad_dpad |= bit;
        }
    }
    else
    {
        if (pressed)
        {
            mem->joypad_buttons &= (uint8_t)~bit;
        }
        else
        {
            mem->joypad_buttons |= bit;
        }
    }

    uint8_t new_joyp = joypad_read(mem);
    mem->memory.P1_JOYP = new_joyp;

    // Joypad interrupt is raised on 1->0 transition of an active input line.
    if (((old_joyp & (uint8_t)~new_joyp) & 0x0F) != 0)
    {
        mem->memory.IF |= IF_JOYPAD;
        mem->memory.IF &= 0x1F;
    }
}

void memory_flush_rom_read_trace(void)
{
    rom_read_trace_flush();
}

size_t memory_get_rom_read_unique_count(void)
{
    rom_trace_lock();
    size_t v = rom_read_unique_count;
    rom_trace_unlock();
    return v;
}

size_t memory_get_rom_read_track_size(void)
{
    rom_trace_lock();
    size_t v = rom_read_track_size;
    rom_trace_unlock();
    return v;
}

size_t memory_get_rom_data_read_unique_count(void)
{
    rom_trace_lock();
    size_t v = rom_data_read_unique_count;
    rom_trace_unlock();
    return v;
}

uint64_t memory_get_joypad_read_count(const MemoryState *mem)
{
    if (!mem)
        return 0u;
    return mem->joypad_read_count;
}

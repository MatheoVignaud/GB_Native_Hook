#ifndef VIRUAAPU_BRIDGE_H
#define VIRUAAPU_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// APU register snapshot passed from the emulator to VirtuaAPU.
/// Mirrors FF10-FF3F (48 bytes).
typedef struct
{
    /* NR1x – Channel 1 (square + sweep) */
    uint8_t NR10;   /* FF10 */
    uint8_t NR11;   /* FF11 */
    uint8_t NR12;   /* FF12 */
    uint8_t NR13;   /* FF13 */
    uint8_t NR14;   /* FF14 */
    uint8_t _pad15;
    /* NR2x – Channel 2 (square) */
    uint8_t NR21;   /* FF16 */
    uint8_t NR22;   /* FF17 */
    uint8_t NR23;   /* FF18 */
    uint8_t NR24;   /* FF19 */
    /* NR3x – Channel 3 (wave) */
    uint8_t NR30;   /* FF1A */
    uint8_t NR31;   /* FF1B */
    uint8_t NR32;   /* FF1C */
    uint8_t NR33;   /* FF1D */
    uint8_t NR34;   /* FF1E */
    uint8_t _pad1f;
    /* NR4x – Channel 4 (noise) */
    uint8_t NR41;   /* FF20 */
    uint8_t NR42;   /* FF21 */
    uint8_t NR43;   /* FF22 */
    uint8_t NR44;   /* FF23 */
    /* NR5x – Control */
    uint8_t NR50;   /* FF24 */
    uint8_t NR51;   /* FF25 */
    uint8_t NR52;   /* FF26 */
    uint8_t _pad27_2f[9];
    /* Wave RAM */
    uint8_t WAVE_RAM[16]; /* FF30-FF3F */
} GBAPURegisters;

#define GBAPU_WRITE_NONE 0xFFu
#define GBAPU_META_LAST_WRITE _pad27_2f[0]

/// Initialise VirtuaAPU for DMG (mode=1) or CGB (mode=2).
void viruaapu_init(uint8_t mode, uint32_t sample_rate);

/// Render `sample_count` stereo samples into the caller-supplied buffer.
/// `regs` is the current APU register snapshot.
/// `double_speed` should be true when the CGB is in double-speed mode.
/// Writes `sample_count * 2` int16 values (L, R, L, R, …) into `dst`.
void viruaapu_render(GBAPURegisters *regs,
                     int      double_speed,
                     uint32_t sample_count,
                     int16_t *dst);

/// Apply register writes immediately without advancing audio time.
void viruaapu_sync(GBAPURegisters *regs, int double_speed);

/// Advance the APU frame sequencer by one hardware tick.
void viruaapu_step_frame_sequencer(GBAPURegisters *regs, int double_speed);

/// Reset the APU state (e.g. on power-on or master sound disable).
void viruaapu_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* VIRUAAPU_BRIDGE_H */

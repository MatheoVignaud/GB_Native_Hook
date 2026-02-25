#ifndef MEMORY
#define MEMORY
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <stddef.h>
#include <stdint.h>

#define IF_VBLANK 0x01
#define IF_LCDSTAT 0x02
#define IF_TIMER 0x04
#define IF_SERIAL 0x08
#define IF_JOYPAD 0x10

typedef enum
{
    JOYPAD_RIGHT = 0,
    JOYPAD_LEFT = 1,
    JOYPAD_UP = 2,
    JOYPAD_DOWN = 3,
    JOYPAD_A = 4,
    JOYPAD_B = 5,
    JOYPAD_SELECT = 6,
    JOYPAD_START = 7
} JoypadInput;

typedef enum
{
    MBC_NONE = 0,
    MBC1,
    MBC2,
    MBC3,
    MBC5
} MBCType;

typedef struct
{
    MBCType mbc_type;
    uint8_t cartridge_type;
    bool has_battery;
    bool has_rtc;
    bool has_ram;

    uint8_t *rom_data;
    size_t rom_size;
    size_t rom_banks;

    uint8_t *ram_data;
    size_t ram_size;
    size_t ram_banks;
    bool ram_dirty;

    bool ram_enabled;

    // MBC1
    uint8_t mbc1_low5;
    uint8_t mbc1_high2;
    uint8_t mbc1_mode;

    // MBC2
    uint8_t mbc2_rom_bank;

    // MBC3
    uint8_t mbc3_rom_bank;
    uint8_t mbc3_ram_rtc_select;
    uint8_t mbc3_latch_state;
    uint8_t mbc3_rtc_regs[5];      // S, M, H, DL, DH
    uint8_t mbc3_rtc_latched[5];   // Latched snapshot after 0->1 transition
    bool mbc3_rtc_latched_valid;
    uint64_t mbc3_rtc_last_unix;   // Host timestamp used for coarse ticking
    bool rtc_dirty;

    // MBC5
    uint16_t mbc5_rom_bank;
    uint8_t mbc5_ram_bank;

    bool persist_loaded;
    char save_path[512];
    char rtc_path[512];
} CartridgeState;

struct CPUState;

#if defined(_MSC_VER)
#pragma pack(push, 1)
#else
#pragma pack(push, 1)
#pragma pack(1)
#endif

typedef union
{
    struct
    {
        union
        {
            struct
            {
                uint8_t rom_bank_0[0x4000]; // 0x0000-0x3FFF
                uint8_t rom_bank_n[0x4000]; // 0x4000-0x7FFF
            };
            uint8_t rom[0x8000];
        };

        uint8_t vram[0x2000];

        uint8_t eram[0x2000];

        uint8_t wram[0x2000];

        uint8_t echo[0x1E00];

        uint8_t oam[0x00A0];

        uint8_t unusable_1[0x0060];

        union
        {
            struct
            {
                uint8_t P1_JOYP;
                uint8_t SB;
                uint8_t SC;
                uint8_t _io_ff03;
                uint8_t DIV;
                uint8_t TIMA;
                uint8_t TMA;
                uint8_t TAC;
                uint8_t _io_ff08[7];
                uint8_t IF;

                uint8_t NR10;
                uint8_t NR11;
                uint8_t NR12;
                uint8_t NR13;
                uint8_t NR14;

                uint8_t _io_ff15;
                uint8_t NR21;
                uint8_t NR22;
                uint8_t NR23;
                uint8_t NR24;

                uint8_t NR30;
                uint8_t NR31;
                uint8_t NR32;
                uint8_t NR33;
                uint8_t NR34;

                uint8_t _io_ff1f;
                uint8_t NR41;
                uint8_t NR42;
                uint8_t NR43;
                uint8_t NR44;

                uint8_t NR50;
                uint8_t NR51;
                uint8_t NR52;
                uint8_t _io_ff27_2f[9];

                uint8_t WAVE_RAM[0x10];

                // PPU
                uint8_t LCDC;
                uint8_t STAT;
                uint8_t SCY;
                uint8_t SCX;
                uint8_t LY;
                uint8_t LYC;
                uint8_t DMA;
                uint8_t BGP;
                uint8_t OBP0;
                uint8_t OBP1;
                uint8_t WY;
                uint8_t WX;

                uint8_t _io_ff4c;
                uint8_t _io_ff4d;
                uint8_t _io_ff4e;
                uint8_t _io_ff4f;
                uint8_t BOOT;

                uint8_t _io_ff51_7f[0x2F];
            };
            uint8_t io[0x80];
        };

        uint8_t hram[0x7F];

        uint8_t IE;
    };

    uint8_t data[0x10000];

} Memory;

#pragma pack(pop)

#define OFFS(t, m) ((size_t)offsetof(t, m))

_Static_assert(sizeof(Memory) == 0x10000, "Memory must be 64 KiB");

_Static_assert(OFFS(Memory, rom_bank_0) == 0x0000, "ROM0 offset");
_Static_assert(OFFS(Memory, rom_bank_n) == 0x4000, "ROMn offset");
_Static_assert(OFFS(Memory, vram) == 0x8000, "VRAM offset");
_Static_assert(OFFS(Memory, eram) == 0xA000, "ERAM offset");
_Static_assert(OFFS(Memory, wram) == 0xC000, "WRAM offset");
_Static_assert(OFFS(Memory, echo) == 0xE000, "Echo offset");
_Static_assert(OFFS(Memory, oam) == 0xFE00, "OAM offset");
_Static_assert(OFFS(Memory, unusable_1) == 0xFEA0, "Unusable A offset");
_Static_assert(OFFS(Memory, io) == 0xFF00, "IO offset");
_Static_assert(OFFS(Memory, hram) == 0xFF80, "HRAM offset");
_Static_assert(OFFS(Memory, IE) == 0xFFFF, "IE offset");

_Static_assert(OFFS(Memory, P1_JOYP) == 0xFF00, "JOYP");
_Static_assert(OFFS(Memory, IF) == 0xFF0F, "IF");
_Static_assert(OFFS(Memory, NR10) == 0xFF10, "NR10");
_Static_assert(OFFS(Memory, NR52) == 0xFF26, "NR52");
_Static_assert(OFFS(Memory, WAVE_RAM) == 0xFF30, "Wave RAM");
_Static_assert(OFFS(Memory, LCDC) == 0xFF40, "LCDC");
_Static_assert(OFFS(Memory, WX) == 0xFF4B, "WX");
_Static_assert(OFFS(Memory, BOOT) == 0xFF50, "BOOT");

typedef struct
{
    Memory memory;
    CartridgeState cartridge;
    uint8_t bios[0x100]; // BIOS ROM (256 bytes)
    bool bios_enabled;
    struct CPUState *cpu; // Back-reference for side effects (timers, etc.)
    uint8_t joypad_buttons; // Lower nibble, 1 = released
    uint8_t joypad_dpad;    // Lower nibble, 1 = released
    uint8_t joypad_select;  // Bits 4/5 selection latch
} MemoryState;

int load_rom(const char *path, MemoryState *mem);
int load_rom_from_buffer(const uint8_t *data, size_t size, MemoryState *mem);
int load_bios(const char *path, uint8_t *bios);

void memory_init(MemoryState *mem);
void memory_shutdown(MemoryState *mem);
void memory_set_logging(bool enabled);

void memory_set_button_state(MemoryState *mem, JoypadInput input, bool pressed);
void memory_set_save_path_hint(MemoryState *mem, const char *rom_path);
void memory_flush_rom_read_trace(void);
size_t memory_get_rom_read_unique_count(void);
size_t memory_get_rom_read_track_size(void);
void memory_set_fetch_patch(uint16_t start, const uint8_t *bytes, uint8_t len);
void memory_clear_fetch_patch(void);

uint8_t memory_read(MemoryState *mem, uint16_t address);
void memory_write(MemoryState *mem, uint16_t address, uint8_t value);

#endif

#ifndef MEMORY
#define MEMORY
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef union
{
    struct
    {
        union
        {
            struct
            {
                uint8_t rom_bank_0[0x4000]; // 16KB for ROM Bank 0
                uint8_t rom_bank_n[0x4000]; // 16KB for ROM Bank N
            };
            uint8_t rom[0x8000]; // 16KB for ROM
        }; // 0x0000 - 0x7FFF
        uint8_t vram[0x2000];     // 8KB for VRAM 0x8000 - 0x9FFFs
        uint8_t eram[0x2000];     // 8KB for External RAM 0xA000 - 0xBFFF
        uint8_t wram[0x2000];     // 8KB for Work RAM 0xC000 - 0xDFFF
        uint8_t not_used[0x1E00]; // Unused memory 0xE000 - 0xFDFF
        uint8_t oam[0xA0];        // Object Attribute Memory (OAM) 0xFE00 - 0xFE9F
        uint8_t not_used2[0x60];  // Unused memory 0xFEA0 - 0xFF9F
        uint8_t io[0x80];         // I/O registers 0xFF00 - 0xFF7F
        uint8_t hram[0x7F];       // High RAM 0xFF80 - 0xFFFF
        uint8_t interrupt_enable; // Interrupt Enable Register 0xFFFF
    };
    uint8_t data[0x10000]; // Total memory space (64KB)
} Memory;

int load_rom(const char *path, uint8_t *mem);

#endif
#include "CPU.h"
#include "Memory.h"
#include "PPU.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>

int main(int argc, char **argv)
{
    /*if (argc < 2)
    {
        printf("Usage: %s <rom.gb>\n", argv[0]);
        return 1;
    }*/

    MemoryState memory;
    memset(&memory, 0, sizeof(memory));

    if (load_bios("dmg_rom.bin", memory.bios) == 0)
    {
        memory.bios_enabled = true;
        printf("BIOS loaded and enabled.\n");
    }
    else
    {
        memory.bios_enabled = false;
        printf("BIOS not found, starting without BIOS.\n");
    }
    if (load_rom(argv[1], memory.memory.data) != 0)
    {
        fprintf(stderr, "Failed to load ROM: %s\n", argv[1]);
        if (load_rom("tetris.gb", memory.memory.data) != 0)
        {
            fprintf(stderr, "Failed to load default ROM: tetris.gb\n");
            return 1;
        }
        else
        {
            printf("Loaded default ROM: tetris.gb\n");
        }
    }

    CPUState cpu = {0};
    PPUState ppu = {0};
    cpu.memory = &memory;
    ppu.mem = &memory.memory;
    cpu_reset(&cpu);
    ppu_reset(&ppu);

    bool running = true;
    while (running)
    {

        ppu_step(&ppu, cpu_execute_instruction(&cpu));
    }

    return 0;
}

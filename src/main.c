#include "CPU.h"
#include "Hooks.h"
#include "Memory.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("Usage: %s <rom.gb>\n", argv[0]);
        return 1;
    }

    Memory memory;
    if (load_rom(argv[1], memory.data) != 0)
    {
        fprintf(stderr, "Failed to load ROM: %s\n", argv[1]);
        return 1;
    }
    CPUState cpu;
    cpu.memory = &memory;
    cpu_reset(&cpu);
    while (1)
    {
        cpu_execute_instruction(&cpu);
    }

    return 0;
}
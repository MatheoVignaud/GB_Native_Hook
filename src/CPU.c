#include "CPU.h"

void cpu_reset(CPUState *cpu)
{
    cpu->A = 0;
    cpu->F = 0;
    cpu->B = 0;
    cpu->C = 0;
    cpu->D = 0;
    cpu->E = 0;
    cpu->H = 0;
    cpu->L = 0;

    cpu->SP = 0xFFFE;
    if (cpu->memory->bios_enabled)
    {
        cpu->PC = 0x0000;
    }
    else
    {
        cpu->PC = 0x100;
    }

    cpu->IME = false;
    cpu->EI_pending = false;
    cpu->halt = false;
    cpu->stop = false;

    cpu->cycle_count = 2;
}

uint64_t instruction_count = 0;

uint16_t breakpoints[] = {};

uint32_t cpu_execute_instruction(CPUState *cpu)
{

    for (size_t i = 0; i < sizeof(breakpoints) / sizeof(breakpoints[0]); ++i)
    {
        if (cpu->PC == breakpoints[i])
        {
            printf("Breakpoint hit at PC=%04X\n", cpu->PC);
            printf("PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X IME=%d IE=%02X IF=%02X LY=%02X OP=%02X\n",
                   cpu->PC, cpu->SP, cpu->AF, cpu->BC, cpu->DE, cpu->HL, cpu->IME,
                   cpu->memory->memory.IE, cpu->memory->memory.IF, cpu->memory->memory.LY,
                   memory_read(cpu->memory, cpu->PC));
            getchar();
            break;
        }
    }

    if (instruction_count % 10000 == 0)
        printf("PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X IME=%d IE=%02X IF=%02X LY=%02X OP=%02X\n",
               cpu->PC, cpu->SP, cpu->AF, cpu->BC, cpu->DE, cpu->HL, cpu->IME,
               cpu->memory->memory.IE, cpu->memory->memory.IF, cpu->memory->memory.LY,
               memory_read(cpu->memory, cpu->PC));

    uint64_t prev_cycles = cpu->cycle_count;

    uint8_t opcode = memory_read(cpu->memory, cpu->PC++);
    opcodes[opcode](cpu);
    instruction_count++;

    return (uint32_t)(cpu->cycle_count - prev_cycles);
}
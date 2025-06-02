#include "CPU.h"

void cpu_reset(CPUState *cpu)
{
    // Reset all registers
    cpu->A = 0;
    cpu->F = 0;
    cpu->B = 0;
    cpu->C = 0;
    cpu->D = 0;
    cpu->E = 0;
    cpu->H = 0;
    cpu->L = 0;

    // Reset Stack Pointer and Program Counter
    cpu->SP = 0xFFFE;
    cpu->PC = cpu->memory->data[0x100] << 8 | cpu->memory->data[0x101];

    cpu->cycle_count = 0;
}

void cpu_execute_instruction(CPUState *cpu)
{
    uint8_t opcode = cpu->memory->data[cpu->PC++];
    opcodes[opcode](cpu);
}

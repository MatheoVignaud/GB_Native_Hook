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
    cpu->PC = 0x100;

    cpu->cycle_count = 0;
}

uint64_t instruction_count = 0;

void cpu_execute_instruction(CPUState *cpu)
{
    /*printf("Cycle Count: %llu\n", cpu->cycle_count);
    printf("PC: 0x%04X, SP: 0x%04X, AF: 0x%04X, BC: 0x%04X, DE: 0x%04X, HL: 0x%04X\n",
           cpu->PC, cpu->SP, cpu->AF, cpu->BC, cpu->DE, cpu->HL);*/
    uint16_t prev_pc = cpu->PC;
    uint8_t opcode = cpu->memory->data[cpu->PC++];

    opcodes[opcode](cpu);
    if (cpu->PC >= 0x230)
    {
        // press enter to continue
        printf("Press Enter to continue...");
        getchar();
        printf("Executing opcode: 0x%02X at PC: 0x%04X , Registers: AF: 0x%04X, BC: 0x%04X, DE: 0x%04X, HL: 0x%04X, SP: 0x%04X \n", opcode, prev_pc, cpu->AF, cpu->BC, cpu->DE, cpu->HL, cpu->SP);
    }

    instruction_count++;
}

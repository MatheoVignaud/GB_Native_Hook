#ifndef CPU
#define CPU
#include "CPU_Defs.h"
#include "CPU_OP_Codes.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>

/*
Thanks to :
   https://gbdev.io/pandocs/CPU_Registers_and_Flags.html
   https://meganesu.github.io/generate-gb-opcodes/

*/

void cpu_reset(CPUState *cpu);

typedef struct
{
    uint64_t prev_cycles;
} CPUDecodedStep;

uint32_t cpu_execute_instruction(CPUState *cpu);
uint32_t cpu_execute_decoded_instruction(CPUState *cpu, uint8_t opcode);
uint32_t cpu_decoded_step_begin(CPUState *cpu, uint8_t opcode, CPUDecodedStep *step);
uint32_t cpu_decoded_step_end(CPUState *cpu, CPUDecodedStep *step);
void cpu_set_illegal_opcode_softfail(int enabled);

void cpu_timer_div_write(CPUState *cpu);
void cpu_timer_tima_write(CPUState *cpu, uint8_t value);
void cpu_timer_tma_write(CPUState *cpu, uint8_t value);
void cpu_timer_tac_write(CPUState *cpu, uint8_t value);
void cpu_dma_transfer(CPUState *cpu, uint16_t source);

#endif

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

uint32_t cpu_execute_instruction(CPUState *cpu);

#endif
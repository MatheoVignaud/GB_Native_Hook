#ifndef DECOMPILER_H
#define DECOMPILER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
    
#include "CPU.h"
#include "Memory.h"
#include "PPU.h"
#include <stdio.h>
#include <Decompiler_patterns.h>
typedef enum DecompilerAccessType
{
    DECOMP_ACCESS_NONE = 0,
    DECOMP_ACCESS_REG_A = 1u << 0,
    DECOMP_ACCESS_REG_B = 1u << 1,
    DECOMP_ACCESS_REG_C = 1u << 2,
    DECOMP_ACCESS_REG_D = 1u << 3,
    DECOMP_ACCESS_REG_E = 1u << 4,
    DECOMP_ACCESS_REG_H = 1u << 5,
    DECOMP_ACCESS_REG_L = 1u << 6,
    DECOMP_ACCESS_REG_F = 1u << 7,
    DECOMP_ACCESS_REG_AF = 1u << 8,
    DECOMP_ACCESS_REG_BC = 1u << 9,
    DECOMP_ACCESS_REG_DE = 1u << 10,
    DECOMP_ACCESS_REG_HL = 1u << 11,
    DECOMP_ACCESS_REG_SP = 1u << 12,
    DECOMP_ACCESS_REG_PC = 1u << 13,
    DECOMP_ACCESS_IMM8 = 1u << 14,
    DECOMP_ACCESS_IMM16 = 1u << 15,
    DECOMP_ACCESS_MEM_HL = 1u << 16,
    DECOMP_ACCESS_MEM_C = 1u << 17,
    DECOMP_ACCESS_MEM_IMM16 = 1u << 18,
    DECOMP_ACCESS_MEM_STACK = 1u << 19,
    DECOMP_ACCESS_FLAG_Z = 1u << 20,
    DECOMP_ACCESS_FLAG_N = 1u << 21,
    DECOMP_ACCESS_FLAG_H = 1u << 22,
    DECOMP_ACCESS_FLAG_C = 1u << 23,
} DecompilerAccessType;

typedef enum DecompilerInstructionFlags
{
    DECOMP_INST_NONE = 0,
    DECOMP_INST_BRANCH = 1u << 0,
    DECOMP_INST_CONDITIONAL = 1u << 1,
    DECOMP_INST_CALL = 1u << 2,
    DECOMP_INST_RETURN = 1u << 3,
    DECOMP_INST_TERMINATOR = 1u << 4,
    DECOMP_INST_TOUCHES_IO = 1u << 5,
    DECOMP_INST_TOUCHES_MBC = 1u << 6,
} DecompilerInstructionFlags;

typedef enum DecompilerFunctionFlags
{
    DECOMP_FUNCTION_NONE = 0,
    DECOMP_FUNCTION_INTERRUPT_HANDLER = 1u << 0,
    DECOMP_FUNCTION_ENTRYPOINT = 1u << 1,
    DECOMP_FUNCTION_BANKED = 1u << 2,
    DECOMP_FUNCTION_HAS_UNRESOLVED_FLOW = 1u << 3,
} DecompilerFunctionFlags;

typedef enum DecompilerStage
{
    DECOMP_STAGE_UNINITIALIZED = 0,
    DECOMP_STAGE_DISCOVERY,
    DECOMP_STAGE_CFG,
    DECOMP_STAGE_DATAFLOW,
    DECOMP_STAGE_IR,
    DECOMP_STAGE_READY,
} DecompilerStage;

typedef struct DecompiledInstruction
{
    uint16_t address;
    uint16_t bank;
    uint32_t rom_offset;

    uint8_t opcode;
    bool cb_prefixed;
    uint8_t encoded_size;
    uint8_t cycles_min;
    uint8_t cycles_max;

    char mnemonic[24];
    char operands[40];

    uint32_t dependency_mask;
    uint32_t read_type;
    uint32_t write_type;

    uint16_t read_address;
    uint16_t write_address;

    uint16_t branch_target;
    int8_t branch_disp;
    uint32_t instruction_flags;
} DecompiledInstruction;

typedef struct DecompiledBasicBlock
{
    uint16_t start_address;
    uint16_t end_address;
    uint16_t bank;

    size_t first_instruction_index;
    size_t instruction_count;

    uint32_t successors[2];
    uint8_t successor_count;
    bool has_fallthrough;
} DecompiledBasicBlock;

typedef struct DecompiledFunction
{
    uint16_t entry_address;
    uint16_t bank;

    char name[64];
    uint32_t flags;
    uint8_t confidence;

    size_t first_block_index;
    size_t block_count;

    size_t first_instruction_index;
    size_t instruction_count;
} DecompiledFunction;

typedef struct DecompilerProgram
{
    DecompilerStage stage;
    bool decompile_enabled;

    CPUState *cpu;
    MemoryState *memory;

    DecompiledInstruction *instructions;
    size_t instruction_count;
    size_t instruction_capacity;

    DecompiledBasicBlock *blocks;
    size_t block_count;
    size_t block_capacity;

    DecompiledFunction *functions;
    size_t function_count;
    size_t function_capacity;
} DecompilerProgram;

extern bool decompile;
extern DecompilerProgram decompiler_program;

void decompiler_main(char *rom, char *output_dir, char *sym , uint16_t entry_point);
void decompiler_discover_function(uint16_t addr);
DecompiledInstruction decompiler_analyze_instruction(CPUState *cpu, uint16_t address, uint16_t bank);


static uint8_t visited_rom[8 * 1024 * 1024] = {0};

static inline bool decompiler_is_rom_visited(uint32_t rom_offset)
{
    return (visited_rom[rom_offset >> 3] & (1u << (rom_offset & 7))) != 0;
}

static inline void decompiler_set_rom_visited(uint32_t rom_offset)
{
    visited_rom[rom_offset >> 3] |= (1u << (rom_offset & 7));
}


#endif
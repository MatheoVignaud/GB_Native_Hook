#ifndef DECOMPILER_PATTERNS_H
#define DECOMPILER_PATTERNS_H

#include <stdint.h>

typedef enum Operand_Type
{
    OPERAND_NONE,
    OPERAND_N8,
    OPERAND_N16,
    OPERAND_A8,
    OPERAND_A16,
    OPERAND_E8,
} Operand_Type;

typedef struct Decompiler_Disasemble_Opcode
{
    uint8_t op;
    const char *mnemonic;
    uint8_t length;
    Operand_Type operand_type;
} Decompiler_Disasemble_Opcode;

extern const Decompiler_Disasemble_Opcode disassemble_opcodes[256];
extern const Decompiler_Disasemble_Opcode disassemble_cb_opcodes[256];

#endif
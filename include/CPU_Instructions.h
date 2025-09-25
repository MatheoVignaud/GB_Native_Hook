
#ifndef INSTRUCTIONS
#define INSTRUCTIONS

#include "CPU.h"
#include <stdbool.h>
#include <stdint.h>

// LOAD INSTRUCTIONS

static inline void ld_r8_r8(uint8_t *dest, uint8_t *src, CPUState *cpu)
{
    *dest = *src;
    cpu->cycle_count += 1;
}

static inline void ld_r8_n8(uint8_t *dest, uint8_t value, CPUState *cpu)
{
    *dest = value;
    cpu->cycle_count += 2;
}

static inline void ld_r16_n16(uint16_t *dest, uint16_t value, CPUState *cpu)
{
    *dest = value;
    cpu->cycle_count += 3;
}

static inline void ld_hl_r8(uint8_t *hl_addr, uint8_t *src, CPUState *cpu)
{
    *hl_addr = *src;
    cpu->cycle_count += 2;
}

static inline void ld_hl_n8(uint8_t *hl_addr, uint8_t value, CPUState *cpu)
{
    *hl_addr = value;
    cpu->cycle_count += 3;
}

static inline void ld_r8_hl(uint8_t *dest, uint8_t *hl_addr, CPUState *cpu)
{
    *dest = *hl_addr;
    cpu->cycle_count += 2;
}

static inline void ld_r16_a(uint16_t *dest_addr, CPUState *cpu)
{
    cpu->memory->data[*dest_addr] = cpu->A;
    cpu->cycle_count += 2;
}

static inline void ld_n16_a(uint16_t addr, CPUState *cpu)
{
    cpu->memory->data[addr] = cpu->A;
    cpu->cycle_count += 4;
}

static inline void ldh_n16_a(uint8_t offset, CPUState *cpu)
{
    cpu->memory->data[0xFF00 + offset] = cpu->A;
    cpu->cycle_count += 3;
}

static inline void ldh_c_a(CPUState *cpu)
{
    cpu->A = cpu->memory->data[0xFF00 + cpu->C];
    cpu->cycle_count += 2;
}

static inline void ld_a_r16(uint16_t addr, CPUState *cpu)
{
    cpu->A = cpu->memory->data[addr];
    cpu->cycle_count += 3;
}

static inline void ld_a_n16(uint16_t addr, CPUState *cpu)
{
    cpu->A = cpu->memory->data[addr];
    cpu->cycle_count += 4;
}

static inline void ldh_a_n16(uint16_t offset, CPUState *cpu)
{
    cpu->A = cpu->memory->data[0xFF00 + offset];
    cpu->cycle_count += 3;
}

static inline void ldh_a_c(CPUState *cpu)
{
    cpu->A = cpu->memory->data[0xFF00 + cpu->C];
    cpu->cycle_count += 2;
}

static inline void ld_hl_inc_a(CPUState *cpu)
{
    cpu->memory->data[cpu->HL] = cpu->A;
    cpu->HL++;
    cpu->cycle_count += 2;
}

static inline void ld_hl_dec_a(CPUState *cpu)
{
    cpu->memory->data[cpu->HL] = cpu->A;
    cpu->HL--;
    cpu->cycle_count += 2;
}

static inline void ld_a_hl_inc(CPUState *cpu)
{
    cpu->A = cpu->memory->data[cpu->HL];
    cpu->HL++;
    cpu->cycle_count += 2;
}

static inline void ld_a_hl_dec(CPUState *cpu)
{
    cpu->A = cpu->memory->data[cpu->HL];
    cpu->HL--;
    cpu->cycle_count += 2;
}

static inline void ld_sp_n16(uint16_t value, CPUState *cpu)
{
    cpu->SP = value;
    cpu->cycle_count += 3;
}

static inline void ld_n16_sp(uint16_t addr, CPUState *cpu)
{
    cpu->memory->data[addr] = (uint8_t)(cpu->SP & 0x00FF);
    cpu->memory->data[addr + 1] = (uint8_t)((cpu->SP >> 8) & 0x00FF);
    cpu->cycle_count += 5;
}

static inline void ldh_a_n8(uint8_t offset, CPUState *cpu)
{
    cpu->A = cpu->memory->data[0xFF00 + offset];
    cpu->cycle_count += 3;
}

static inline void ldh_n8_a(uint8_t offset, CPUState *cpu)
{
    cpu->memory->data[0xFF00 + offset] = cpu->A;
    cpu->cycle_count += 3;
}

// ARITHMETIC INSTRUCTIONS

static inline void adc_a_r8(uint8_t value, CPUState *cpu)
{
    uint16_t carry = (cpu->F & 0x10) ? 1 : 0; // Check if Carry flag is set
    uint16_t result = cpu->A + value + carry;

    // Set flags
    cpu->F = 0; // Clear all flags
    if ((result & 0xFF) == 0)
        cpu->F |= FLAG_Z; // Set Zero flag if result is zero
    if (result > 0xFF)
        cpu->F |= (1 << 4); // Set Carry flag if there's a carry out
    if (((cpu->A & 0x0F) + (value & 0x0F) + carry) > 0x0F)
        cpu->F |= FLAG_H; // Set Half Carry flag

    cpu->A = (uint8_t)(result & 0xFF);
    cpu->cycle_count += 1;
}

static inline void adc_a_hl(CPUState *cpu)
{
    adc_a_r8(cpu->memory->data[cpu->HL], cpu);
    cpu->cycle_count += 1;
}

static inline void adc_a_n8(uint8_t value, CPUState *cpu)
{
    adc_a_r8(value, cpu);
    cpu->cycle_count += 1;
}

static inline void add_a_r8(uint8_t value, CPUState *cpu)
{
    uint16_t result = cpu->A + value;

    // Set flags
    cpu->F = 0; // Clear all flags
    if ((result & 0xFF) == 0)
        cpu->F |= FLAG_Z; // Set Zero flag if result is zero
    if (result > 0xFF)
        cpu->F |= (1 << 4); // Set Carry flag if there's a carry out
    if (((cpu->A & 0x0F) + (value & 0x0F)) > 0x0F)
        cpu->F |= FLAG_H; // Set Half Carry flag

    cpu->A = (uint8_t)(result & 0xFF);
    cpu->cycle_count += 1;
}

static inline void add_a_hl(CPUState *cpu)
{
    add_a_r8(cpu->memory->data[cpu->HL], cpu);
    cpu->cycle_count += 1;
}

static inline void add_a_n8(uint8_t value, CPUState *cpu)
{
    add_a_r8(value, cpu);
    cpu->cycle_count += 1;
}

static inline void add_hl_r16(uint16_t value, CPUState *cpu)
{
    uint32_t result = cpu->HL + value;

    // Set flags
    cpu->F &= ~FLAG_Z; // Clear Zero flag
    cpu->F &= ~FLAG_N; // Clear Subtract flag
    if ((cpu->HL & 0x0FFF) + (value & 0x0FFF) > 0x0FFF)
        cpu->F |= FLAG_H; // Set Half Carry flag
    if (result > 0xFFFF)
        cpu->F |= (1 << 4); // Set Carry flag

    cpu->HL = (uint16_t)(result & 0xFFFF);
    cpu->cycle_count += 2;
}

static inline void add_hl_sp(CPUState *cpu)
{
    add_hl_r16(cpu->SP, cpu);
}

static inline void add_sp_e8(int8_t value, CPUState *cpu)
{
    uint16_t old_sp = cpu->SP;
    cpu->SP += value;

    // Set flags
    cpu->F = 0; // Clear all flags
    if (((old_sp & 0x0F) + (value & 0x0F)) > 0x0F)
        cpu->F |= FLAG_H; // Set Half Carry flag
    if (((old_sp & 0xFF) + (value & 0xFF)) > 0xFF)
        cpu->F |= (1 << 4); // Set Carry flag

    cpu->cycle_count += 4;
}

static inline void cp_a_r8(uint8_t value, CPUState *cpu)
{
    uint8_t result = cpu->A - value;

    // Set flags
    cpu->F = 0; // Clear all flags
    if (result == 0)
        cpu->F |= FLAG_Z; // Set Zero flag if result is zero
    cpu->F |= FLAG_N;     // Set Subtract flag
    if (cpu->A < value)
        cpu->F |= (1 << 4); // Set Carry flag if there's a borrow
    if ((cpu->A & 0x0F) < (value & 0x0F))
        cpu->F |= FLAG_H; // Set Half Carry flag

    cpu->cycle_count += 1;
}

static inline void cp_a_hl(CPUState *cpu)
{
    cp_a_r8(cpu->memory->data[cpu->HL], cpu);
    cpu->cycle_count += 1;
}

static inline void cp_a_n8(uint8_t value, CPUState *cpu)
{
    cp_a_r8(value, cpu);
    cpu->cycle_count += 1;
}

static inline void dec_r8(uint8_t *reg, CPUState *cpu)
{
    uint8_t result = *reg - 1;

    // Set flags
    if (result == 0)
        cpu->F |= FLAG_Z; // Set Zero flag if result is zero
    else
        cpu->F &= ~FLAG_Z; // Clear Zero flag otherwise
    cpu->F |= FLAG_N;      // Set Subtract flag
    if ((*reg & 0x0F) == 0x00)
        cpu->F |= FLAG_H; // Set Half Carry flag if there was a borrow from bit 4
    else
        cpu->F &= ~FLAG_H; // Clear Half Carry flag otherwise

    *reg = result;
    cpu->cycle_count += 1;
}

static inline void dec_hl(uint8_t *hl_addr, CPUState *cpu)
{
    uint8_t result = *hl_addr - 1;

    // Set flags
    if (result == 0)
        cpu->F |= FLAG_Z; // Set Zero flag if result is zero
    else
        cpu->F &= ~FLAG_Z; // Clear Zero flag otherwise
    cpu->F |= FLAG_N;      // Set Subtract flag
    if ((*hl_addr & 0x0F) == 0x00)
        cpu->F |= FLAG_H; // Set Half Carry flag if there was a borrow from bit 4
    else
        cpu->F &= ~FLAG_H; // Clear Half Carry flag otherwise

    *hl_addr = result;
    cpu->cycle_count += 1;
}

// BITWISE INSTRUCTIONS

static inline void xor_a_r8(uint8_t value, CPUState *cpu)
{
    cpu->A ^= value;

    // Set flags
    cpu->F = 0; // Clear all flags
    if (cpu->A == 0)
        cpu->F |= FLAG_Z; // Set Zero flag if result is zero

    cpu->cycle_count += 1;
}

// JUMP INSTRUCTIONS

static inline void call_n16(uint16_t addr, CPUState *cpu)
{
    cpu->memory->data[--cpu->SP] = (uint8_t)((cpu->PC >> 8) & 0x00FF);
    cpu->memory->data[--cpu->SP] = (uint8_t)(cpu->PC & 0x00FF);
    cpu->PC = addr;
    cpu->cycle_count += 6;
}

static inline void call_cc_n16(bool condition, uint16_t addr, CPUState *cpu)
{
    if (condition)
    {
        call_n16(addr, cpu);
    }
    else
    {
        cpu->cycle_count += 3;
    }
}

static inline void jp_hl(CPUState *cpu)
{
    cpu->PC = cpu->HL;
}

static inline void jp_n16(uint16_t addr, CPUState *cpu)
{
    cpu->PC = addr;
    cpu->cycle_count += 4;
}

static inline void jp_cc_n16(bool condition, uint16_t addr, CPUState *cpu)
{
    if (condition)
    {
        jp_n16(addr, cpu);
    }
    else
    {
        cpu->cycle_count += 3;
    }
}

static inline void jr_e8(int8_t offset, CPUState *cpu)
{
    cpu->PC += offset;
    cpu->cycle_count += 3;
}

static inline void jr_cc_e8(bool condition, int8_t offset, CPUState *cpu)
{
    if (condition)
    {
        jr_e8(offset, cpu);
    }
    else
    {
        cpu->cycle_count += 2;
    }
}

// INTERRUPT INSTRUCTIONS
static inline void di(CPUState *cpu)
{
    // Disable interrupts
    cpu->memory->data[0xFFFF] &= ~(1 << 0); // Clear the IME flag
    cpu->cycle_count += 1;
}

static inline void ei(CPUState *cpu)
{
    // Enable interrupts
    cpu->memory->data[0xFFFF] |= (1 << 0); // Set the IME flag
    cpu->cycle_count += 1;
}

#endif // INSTRUCTIONS
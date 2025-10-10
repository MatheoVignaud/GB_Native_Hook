
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

static inline void ld_r16_n16(uint16_t *dest, CPUState *cpu)
{
    uint16_t value = (uint16_t)(memory_read(cpu->memory, cpu->PC) | (memory_read(cpu->memory, cpu->PC + 1) << 8));
    cpu->PC += 2;
    *dest = value;
    cpu->cycle_count += 3;
}

static inline void ld_hl_r8(uint8_t value, CPUState *cpu)
{
    memory_write(cpu->memory, cpu->HL, value);
    cpu->cycle_count += 2;
}

static inline void ld_hl_n8(uint8_t value, CPUState *cpu)
{
    memory_write(cpu->memory, cpu->HL, value);
    cpu->cycle_count += 3;
}

static inline void ld_r8_hl(uint8_t *dest, CPUState *cpu)
{
    *dest = memory_read(cpu->memory, cpu->HL);
    cpu->cycle_count += 2;
}

static inline void ld_r16_a(uint16_t dest_addr, CPUState *cpu)
{
    memory_write(cpu->memory, dest_addr, cpu->A);
    cpu->cycle_count += 2;
}

static inline void ld_n16_a(uint16_t addr, CPUState *cpu)
{
    memory_write(cpu->memory, addr, cpu->A);
    cpu->cycle_count += 4;
}

static inline void ldh_n16_a(uint8_t offset, CPUState *cpu)
{
    memory_write(cpu->memory, 0xFF00 + offset, cpu->A);
    cpu->cycle_count += 3;
}

static inline void ldh_c_a(CPUState *cpu)
{
    memory_write(cpu->memory, 0xFF00 + cpu->C, cpu->A);
    cpu->cycle_count += 2;
}

static inline void ld_a_r16(uint16_t addr, CPUState *cpu)
{
    cpu->A = memory_read(cpu->memory, addr);
    cpu->cycle_count += 2;
}

static inline void ld_a_n16(CPUState *cpu)
{
    uint16_t addr = (uint16_t)(memory_read(cpu->memory, cpu->PC) | (memory_read(cpu->memory, cpu->PC + 1) << 8));
    cpu->PC += 2;
    cpu->A = memory_read(cpu->memory, addr);
    cpu->cycle_count += 4;
}

static inline void ldh_a_c(CPUState *cpu)
{
    cpu->A = memory_read(cpu->memory, 0xFF00 + cpu->C);
    cpu->cycle_count += 2;
}

static inline void ld_hl_inc_a(CPUState *cpu)
{
    memory_write(cpu->memory, cpu->HL, cpu->A);
    cpu->HL++;
    cpu->cycle_count += 2;
}

static inline void ld_hl_dec_a(CPUState *cpu)
{
    memory_write(cpu->memory, cpu->HL, cpu->A);
    cpu->HL--;
    cpu->cycle_count += 2;
}

static inline void ld_a_hl_inc(CPUState *cpu)
{
    cpu->A = memory_read(cpu->memory, cpu->HL);
    cpu->HL++;
    cpu->cycle_count += 2;
}

static inline void ld_a_hl_dec(CPUState *cpu)
{
    cpu->A = memory_read(cpu->memory, cpu->HL);
    cpu->HL--;
    cpu->cycle_count += 2;
}

static inline void ld_sp_n16(uint16_t value, CPUState *cpu)
{
    cpu->SP = value;
    cpu->cycle_count += 3;
}

static inline void ld_n16_sp(CPUState *cpu)
{
    uint16_t addr = (uint16_t)(memory_read(cpu->memory, cpu->PC) | (memory_read(cpu->memory, cpu->PC + 1) << 8));
    cpu->PC += 2;
    memory_write(cpu->memory, addr, (uint8_t)(cpu->SP & 0x00FF));
    memory_write(cpu->memory, addr + 1, (uint8_t)((cpu->SP >> 8) & 0x00FF));
    cpu->cycle_count += 5;
}

static inline void ldh_n8_a(uint8_t offset, CPUState *cpu)
{
    memory_write(cpu->memory, 0xFF00 + offset, cpu->A);
    cpu->cycle_count += 3;
}

static inline void ld_hl_sp_e8(CPUState *cpu)
{
    int8_t offset = (int8_t)memory_read(cpu->memory, cpu->PC++);
    uint16_t sp = cpu->SP;
    uint16_t result = sp + offset;

    cpu->HL = result;

    cpu->F = 0;
    if (((sp & 0x0F) + (offset & 0x0F)) > 0x0F)
        cpu->F |= FLAG_H;
    if (((sp & 0xFF) + (offset & 0xFF)) > 0xFF)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 3;
}

static inline void ld_sp_hl(CPUState *cpu)
{
    cpu->SP = cpu->HL;
    cpu->cycle_count += 2;
}

static inline void ldh_a_a8(CPUState *cpu)
{
    uint8_t imm = memory_read(cpu->memory, cpu->PC++);
    uint16_t addr = 0xFF00u + imm;
    cpu->A = memory_read(cpu->memory, addr);
    cpu->cycle_count += 3;
}

// ARITHMETIC INSTRUCTIONS

static inline void adc_a_r8(uint8_t value, CPUState *cpu)
{
    uint16_t carry = (cpu->F & 0x10) ? 1 : 0;
    uint16_t result = cpu->A + value + carry;

    cpu->F = 0;
    if ((result & 0xFF) == 0)
        cpu->F |= FLAG_Z;
    if (result > 0xFF)
        cpu->F |= (1 << 4);
    if (((cpu->A & 0x0F) + (value & 0x0F) + carry) > 0x0F)
        cpu->F |= FLAG_H;

    cpu->A = (uint8_t)(result & 0xFF);
    cpu->cycle_count += 1;
}

static inline void adc_a_hl(CPUState *cpu)
{
    adc_a_r8(memory_read(cpu->memory, cpu->HL), cpu);
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

    cpu->F = 0;
    if ((result & 0xFF) == 0)
        cpu->F |= FLAG_Z;
    if (result > 0xFF)
        cpu->F |= (1 << 4);
    if (((cpu->A & 0x0F) + (value & 0x0F)) > 0x0F)
        cpu->F |= FLAG_H;

    cpu->A = (uint8_t)(result & 0xFF);
    cpu->cycle_count += 1;
}

static inline void add_a_hl(CPUState *cpu)
{
    add_a_r8(memory_read(cpu->memory, cpu->HL), cpu);
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

    cpu->F &= ~FLAG_N;
    if ((cpu->HL & 0x0FFF) + (value & 0x0FFF) > 0x0FFF)
        cpu->F |= FLAG_H;
    else
        cpu->F &= ~FLAG_H;
    if (result > 0xFFFF)
        cpu->F |= FLAG_C;
    else
        cpu->F &= ~FLAG_C;

    cpu->HL = (uint16_t)(result & 0xFFFF);
    cpu->cycle_count += 2;
}

static inline void add_hl_sp(CPUState *cpu)
{
    add_hl_r16(cpu->SP, cpu);
}

static inline void add_sp_e8(CPUState *cpu)
{
    int8_t value = (int8_t)memory_read(cpu->memory, cpu->PC++);
    uint16_t old_sp = cpu->SP;
    cpu->SP += value;

    cpu->F = 0;
    if (((old_sp & 0x0F) + (value & 0x0F)) > 0x0F)
        cpu->F |= FLAG_H;
    if (((old_sp & 0xFF) + (value & 0xFF)) > 0xFF)
        cpu->F |= (1 << 4);

    cpu->cycle_count += 4;
}

static inline void sub_r8(uint8_t value, CPUState *cpu)
{
    uint8_t result = cpu->A - value;

    cpu->F = 0;
    if (result == 0)
        cpu->F |= FLAG_Z;
    cpu->F |= FLAG_N;
    if (cpu->A < value)
        cpu->F |= (1 << 4);
    if ((cpu->A & 0x0F) < (value & 0x0F))
        cpu->F |= FLAG_H;

    cpu->A = result;
    cpu->cycle_count += 1;
}

static inline void sub_hl(CPUState *cpu)
{
    sub_r8(memory_read(cpu->memory, cpu->HL), cpu);
    cpu->cycle_count += 1;
}

static inline void sub_n8(uint8_t value, CPUState *cpu)
{
    sub_r8(value, cpu);
    cpu->cycle_count += 1;
}

static inline void cp_a_r8(uint8_t value, CPUState *cpu)
{
    uint8_t result = cpu->A - value;

    cpu->F = 0;
    if (result == 0)
        cpu->F |= FLAG_Z;
    cpu->F |= FLAG_N;
    if (cpu->A < value)
        cpu->F |= (1 << 4);
    if ((cpu->A & 0x0F) < (value & 0x0F))
        cpu->F |= FLAG_H;

    cpu->cycle_count += 1;
}

static inline void cp_a_hl(CPUState *cpu)
{
    cp_a_r8(memory_read(cpu->memory, cpu->HL), cpu);
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

    if (result == 0)
        cpu->F |= FLAG_Z;
    else
        cpu->F &= ~FLAG_Z;
    cpu->F |= FLAG_N;
    if ((*reg & 0x0F) == 0x00)
        cpu->F |= FLAG_H;
    else
        cpu->F &= ~FLAG_H;

    *reg = result;
    cpu->cycle_count += 1;
}

static inline void dec_hl(CPUState *cpu)
{
    uint8_t result = memory_read(cpu->memory, cpu->HL) - 1;

    if (result == 0)
        cpu->F |= FLAG_Z;
    else
        cpu->F &= ~FLAG_Z;
    cpu->F |= FLAG_N;
    if ((memory_read(cpu->memory, cpu->HL) & 0x0F) == 0x00)
        cpu->F |= FLAG_H;
    else
        cpu->F &= ~FLAG_H;

    memory_write(cpu->memory, cpu->HL, result);
    cpu->cycle_count += 3;
}

static inline void dec_r16(uint16_t *reg, CPUState *cpu)
{
    (*reg)--;
    cpu->cycle_count += 2;
}

static inline void inc_r8(uint8_t *reg, CPUState *cpu)
{
    uint8_t result = *reg + 1;

    if (result == 0)
        cpu->F |= FLAG_Z;
    else
        cpu->F &= ~FLAG_Z;
    cpu->F &= ~FLAG_N;
    if ((result & 0x0F) == 0x00)
        cpu->F |= FLAG_H;
    else
        cpu->F &= ~FLAG_H;

    *reg = result;
    cpu->cycle_count += 1;
}

static inline void inc_r16(uint16_t *reg, CPUState *cpu)
{
    (*reg)++;
    cpu->cycle_count += 2;
}

static inline void inc_hl(CPUState *cpu)
{
    uint8_t hl_value = memory_read(cpu->memory, cpu->HL);
    uint8_t result = hl_value + 1;

    if (result == 0)
        cpu->F |= FLAG_Z;
    else
        cpu->F &= ~FLAG_Z;
    cpu->F &= ~FLAG_N;
    if ((result & 0x0F) == 0x00)
        cpu->F |= FLAG_H;
    else
        cpu->F &= ~FLAG_H;

    memory_write(cpu->memory, cpu->HL, result);
    cpu->cycle_count += 3;
}

static inline void sbc_a_r8(uint8_t value, CPUState *cpu)
{
    uint16_t carry = (cpu->F & 0x10) ? 1 : 0;
    uint16_t result = cpu->A - value - carry;

    cpu->F = 0;
    if ((result & 0xFF) == 0)
        cpu->F |= FLAG_Z;
    cpu->F |= FLAG_N;
    if (cpu->A < (value + carry))
        cpu->F |= (1 << 4);
    if ((cpu->A & 0x0F) < ((value & 0x0F) + carry))
        cpu->F |= FLAG_H;

    cpu->A = (uint8_t)(result & 0xFF);
    cpu->cycle_count += 1;
}

static inline void sbc_a_hl(CPUState *cpu)
{
    sbc_a_r8(memory_read(cpu->memory, cpu->HL), cpu);
    cpu->cycle_count += 1;
}

static inline void sbc_a_n8(CPUState *cpu)
{
    uint8_t value = memory_read(cpu->memory, cpu->PC++);
    sbc_a_r8(value, cpu);
    cpu->cycle_count += 1;
}

// BITWISE INSTRUCTIONS

static inline void and_r8(uint8_t value, CPUState *cpu)
{
    cpu->A &= value;

    cpu->F = FLAG_H;
    if (cpu->A == 0)
        cpu->F |= FLAG_Z;
    cpu->F &= ~FLAG_N;
    cpu->F &= ~(1 << 4);

    cpu->cycle_count += 1;
}

static inline void and_hl(CPUState *cpu)
{
    and_r8(memory_read(cpu->memory, cpu->HL), cpu);
    cpu->cycle_count += 1;
}

static inline void and_n8(uint8_t value, CPUState *cpu)
{
    and_r8(value, cpu);
    cpu->cycle_count += 1;
}

static inline void xor_a_r8(uint8_t value, CPUState *cpu)
{
    cpu->A ^= value;

    cpu->F = 0;
    if (cpu->A == 0)
        cpu->F |= FLAG_Z;

    cpu->cycle_count += 1;
}

static inline void xor_a_hl(CPUState *cpu)
{
    xor_a_r8(memory_read(cpu->memory, cpu->HL), cpu);
    cpu->cycle_count += 1;
}

static inline void xor_a_n8(uint8_t value, CPUState *cpu)
{
    xor_a_r8(value, cpu);
    cpu->cycle_count += 1;
}

static inline void cpl(CPUState *cpu)
{
    cpu->A = ~cpu->A;

    cpu->F |= FLAG_N;
    cpu->F |= FLAG_H;

    cpu->cycle_count += 1;
}

static inline void or_a_r8(uint8_t value, CPUState *cpu)
{
    cpu->A |= value;

    cpu->F = 0;
    if (cpu->A == 0)
        cpu->F |= FLAG_Z;

    cpu->cycle_count += 1;
}

static inline void or_a_hl(CPUState *cpu)
{
    or_a_r8(memory_read(cpu->memory, cpu->HL), cpu);
    cpu->cycle_count += 1;
}

static inline void or_a_n8(uint8_t value, CPUState *cpu)
{
    or_a_r8(value, cpu);
    cpu->cycle_count += 1;
}

// BIT FLAG INSTRUCTIONS

static inline void bit_u3_r8(uint8_t bit, uint8_t reg, CPUState *cpu)
{
    bool bit_set = (reg & (1 << bit)) != 0;

    if (!bit_set)
        cpu->F |= FLAG_Z;
    else
        cpu->F &= ~FLAG_Z;
    cpu->F &= ~FLAG_N;
    cpu->F |= FLAG_H;

    cpu->cycle_count += 2;
}

static inline void bit_u3_hl(uint8_t bit, CPUState *cpu)
{
    uint8_t hl_value = memory_read(cpu->memory, cpu->HL);
    bit_u3_r8(bit, hl_value, cpu);
    cpu->cycle_count += 2;
}

static inline void res_u3_r8(uint8_t bit, uint8_t *reg, CPUState *cpu)
{
    *reg &= ~(1 << bit);
    cpu->cycle_count += 2;
}

static inline void res_u3_hl(uint8_t bit, CPUState *cpu)
{
    uint8_t hl_value = memory_read(cpu->memory, cpu->HL);
    hl_value &= ~(1 << bit);
    memory_write(cpu->memory, cpu->HL, hl_value);
    cpu->cycle_count += 4;
}

static inline void set_u3_r8(uint8_t bit, uint8_t *reg, CPUState *cpu)
{
    *reg |= (1 << bit);
    cpu->cycle_count += 2;
}

static inline void set_u3_hl(uint8_t bit, CPUState *cpu)
{
    uint8_t hl_value = memory_read(cpu->memory, cpu->HL);
    hl_value |= (1 << bit);
    memory_write(cpu->memory, cpu->HL, hl_value);
    cpu->cycle_count += 4;
}

// BITSHIFTING INSTRUCTIONS

static inline void rla(CPUState *cpu)
{
    uint8_t old_carry = (cpu->F & FLAG_C) ? 1 : 0;
    uint8_t new_carry = (cpu->A & 0x80) ? 1 : 0;

    cpu->A = (cpu->A << 1) | old_carry;

    cpu->F = 0;
    if (new_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 1;
}

static inline void rlc_r8(uint8_t *reg, CPUState *cpu)
{
    uint8_t new_carry = (*reg & 0x80) ? 1 : 0;

    *reg = (*reg << 1) | new_carry;

    cpu->F = 0;
    if (*reg == 0)
        cpu->F |= FLAG_Z;
    if (new_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 1;
}

static inline void rlc_hl(CPUState *cpu)
{
    uint8_t hl_value = memory_read(cpu->memory, cpu->HL);
    uint8_t new_carry = (hl_value & 0x80) ? 1 : 0;

    hl_value = (hl_value << 1) | new_carry;
    memory_write(cpu->memory, cpu->HL, hl_value);

    cpu->F = 0;
    if (hl_value == 0)
        cpu->F |= FLAG_Z;
    if (new_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 3;
}

static inline void rlca(CPUState *cpu)
{
    uint8_t new_carry = (cpu->A & 0x80) ? 1 : 0;

    cpu->A = (cpu->A << 1) | new_carry;

    cpu->F = 0;
    if (new_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 1;
}

static inline void rrca(CPUState *cpu)
{
    uint8_t new_carry = (cpu->A & 0x01) ? 1 : 0;

    cpu->A = (cpu->A >> 1) | (new_carry << 7);

    cpu->F = 0;
    if (new_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 1;
}

static inline void rrc_r8(uint8_t *reg, CPUState *cpu)
{
    uint8_t new_carry = *reg & 0x01;

    *reg = (*reg >> 1) | (new_carry << 7);

    cpu->F = 0;
    if (*reg == 0)
        cpu->F |= FLAG_Z;
    if (new_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 1;
}

static inline void rrc_hl(CPUState *cpu)
{
    uint8_t hl_value = memory_read(cpu->memory, cpu->HL);
    uint8_t new_carry = (hl_value & 0x01) ? 1 : 0;

    hl_value = (hl_value >> 1) | (new_carry << 7);
    memory_write(cpu->memory, cpu->HL, hl_value);

    cpu->F = 0;
    if (hl_value == 0)
        cpu->F |= FLAG_Z;
    if (new_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 3;
}

static inline void rl_r8(uint8_t *reg, CPUState *cpu)
{
    uint8_t old_carry = (cpu->F & FLAG_C) ? 1 : 0;
    uint8_t new_carry = (*reg & 0x80) ? 1 : 0;

    *reg = (*reg << 1) | old_carry;

    cpu->F = 0;
    if (*reg == 0)
        cpu->F |= FLAG_Z;
    if (new_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 1;
}

static inline void rl_hl(CPUState *cpu)
{
    uint8_t hl_value = memory_read(cpu->memory, cpu->HL);
    uint8_t old_carry = (cpu->F & FLAG_C) ? 1 : 0;
    uint8_t new_carry = (hl_value & 0x80) ? 1 : 0;

    hl_value = (hl_value << 1) | old_carry;
    memory_write(cpu->memory, cpu->HL, hl_value);

    cpu->F = 0;
    if (hl_value == 0)
        cpu->F |= FLAG_Z;
    if (new_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 3;
}

static inline void swap_r8(uint8_t *reg, CPUState *cpu)
{
    *reg = ((*reg & 0x0F) << 4) | ((*reg & 0xF0) >> 4);

    cpu->F = 0;
    if (*reg == 0)
        cpu->F |= FLAG_Z;

    cpu->cycle_count += 2;
}

static inline void swap_hl(CPUState *cpu)
{
    uint8_t hl_value = memory_read(cpu->memory, cpu->HL);
    hl_value = ((hl_value & 0x0F) << 4) | ((hl_value & 0xF0) >> 4);
    memory_write(cpu->memory, cpu->HL, hl_value);

    cpu->F = 0;
    if (hl_value == 0)
        cpu->F |= FLAG_Z;

    cpu->cycle_count += 4;
}

static inline void sla_r8(uint8_t *reg, CPUState *cpu)
{
    uint8_t new_carry = (*reg & 0x80) ? 1 : 0;

    *reg <<= 1;

    cpu->F = 0;
    if (*reg == 0)
        cpu->F |= FLAG_Z;
    if (new_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 2;
}

static inline void sla_hl(CPUState *cpu)
{
    uint8_t hl_value = memory_read(cpu->memory, cpu->HL);
    uint8_t new_carry = (hl_value & 0x80) ? 1 : 0;

    hl_value <<= 1;
    memory_write(cpu->memory, cpu->HL, hl_value);

    cpu->F = 0;
    if (hl_value == 0)
        cpu->F |= FLAG_Z;
    if (new_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 4;
}

static inline void srl_r8(uint8_t *reg, CPUState *cpu)
{
    uint8_t new_carry = *reg & 0x01;

    *reg >>= 1;

    cpu->F = 0;
    if (*reg == 0)
        cpu->F |= FLAG_Z;
    if (new_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 2;
}

static inline void srl_hl(CPUState *cpu)
{
    uint8_t v = memory_read(cpu->memory, cpu->HL);
    uint8_t new_carry = v & 0x01;
    v >>= 1;
    memory_write(cpu->memory, cpu->HL, v);

    cpu->F = 0;
    if (v == 0)
        cpu->F |= FLAG_Z;
    if (new_carry)
        cpu->F |= FLAG_C;
    cpu->cycle_count += 3;
}

static inline void sra_r8(uint8_t *reg, CPUState *cpu)
{
    uint8_t new_carry = (*reg & 0x01) ? 1 : 0;
    uint8_t msb = *reg & 0x80;

    *reg = (msb) | (*reg >> 1);

    cpu->F = 0;
    if (*reg == 0)
        cpu->F |= FLAG_Z;
    if (new_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 1;
}

static inline void sra_hl(CPUState *cpu)
{
    uint8_t hl_value = memory_read(cpu->memory, cpu->HL);
    uint8_t new_carry = (hl_value & 0x01) ? 1 : 0;
    uint8_t msb = hl_value & 0x80;

    hl_value = msb | (hl_value >> 1);
    memory_write(cpu->memory, cpu->HL, hl_value);

    cpu->F = 0;
    if (hl_value == 0)
        cpu->F |= FLAG_Z;
    if (new_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 3;
}

static inline void rr_r8(uint8_t *reg, CPUState *cpu)
{
    uint8_t old_carry = (cpu->F & FLAG_C) ? 1 : 0;
    uint8_t new_carry = (*reg & 0x01) ? 1 : 0;

    *reg = (*reg >> 1) | (old_carry << 7);

    cpu->F = 0;
    if (*reg == 0)
        cpu->F |= FLAG_Z;
    if (new_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 2;
}

static inline void rr_hl(CPUState *cpu)
{
    uint8_t hl_value = memory_read(cpu->memory, cpu->HL);
    uint8_t old_carry = (cpu->F & FLAG_C) ? 1 : 0;
    uint8_t new_carry = (hl_value & 0x01) ? 1 : 0;

    hl_value = (hl_value >> 1) | (old_carry << 7);
    memory_write(cpu->memory, cpu->HL, hl_value);

    cpu->F = 0;
    if (hl_value == 0)
        cpu->F |= FLAG_Z;
    if (new_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 4;
}

static inline void rra(CPUState *cpu)
{
    uint8_t old_carry = (cpu->F & FLAG_C) ? 1 : 0;
    uint8_t new_carry = (cpu->A & 0x01) ? 1 : 0;

    cpu->A = (cpu->A >> 1) | (old_carry << 7);

    cpu->F = 0;
    if (new_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 1;
}

// JUMP INSTRUCTIONS

static inline void call_n16(CPUState *cpu)
{
    uint16_t addr = memory_read(cpu->memory, cpu->PC) | (memory_read(cpu->memory, cpu->PC + 1) << 8);
    cpu->PC += 2;
    memory_write(cpu->memory, --cpu->SP, (uint8_t)((cpu->PC >> 8) & 0x00FF));
    memory_write(cpu->memory, --cpu->SP, (uint8_t)(cpu->PC & 0x00FF));
    cpu->PC = addr;
    cpu->cycle_count += 6;
}

static inline void call_cc_n16(bool condition, CPUState *cpu)
{
    if (condition)
    {
        call_n16(cpu);
    }
    else
    {
        cpu->PC += 2;
        cpu->cycle_count += 3;
    }
}

static inline void jp_hl(CPUState *cpu)
{
    cpu->PC = cpu->HL;
    cpu->cycle_count += 1;
}

static inline void jp_n16(CPUState *cpu)
{
    uint16_t addr = memory_read(cpu->memory, cpu->PC) | (memory_read(cpu->memory, cpu->PC + 1) << 8);
    cpu->PC += 2;
    cpu->PC = addr;
    cpu->cycle_count += 4;
}

static inline void jp_cc_n16(bool condition, CPUState *cpu)
{
    if (condition)
    {
        jp_n16(cpu);
        return;
    }

    cpu->PC += 2;
    cpu->cycle_count += 3;
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

static inline void ret(CPUState *cpu)
{
    uint8_t low = memory_read(cpu->memory, cpu->SP++);
    uint8_t high = memory_read(cpu->memory, cpu->SP++);
    cpu->PC = (high << 8) | low;
    cpu->cycle_count += 4;
}

static inline void ret_cc(bool condition, CPUState *cpu)
{
    if (condition)
    {
        ret(cpu);
        cpu->cycle_count += 1;
    }
    else
    {
        cpu->cycle_count += 2;
    }
}

static inline void reti(CPUState *cpu)
{
    ret(cpu);
    cpu->IME = true;
}

static inline void rst(uint8_t index, CPUState *cpu)
{
    const uint16_t addresses[] = {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38};
    if (index < 8)
    {
        memory_write(cpu->memory, --cpu->SP, (uint8_t)((cpu->PC >> 8) & 0x00FF));
        memory_write(cpu->memory, --cpu->SP, (uint8_t)(cpu->PC & 0x00FF));
        cpu->PC = addresses[index];
        cpu->cycle_count += 4;
    }
}

// INTERRUPT INSTRUCTIONS
static inline void di(CPUState *cpu)
{
    cpu->IME = false;
    cpu->cycle_count += 1;
}

static inline void ei(CPUState *cpu)
{
    cpu->EI_pending = true;
    cpu->cycle_count += 1;
}

static inline void halt(CPUState *cpu)
{
    cpu->halt = true;
    cpu->cycle_count += 1;
}

// STACK MANIPULATION INSTRUCTIONS

static inline void pop_af(CPUState *cpu)
{
    uint8_t low = memory_read(cpu->memory, cpu->SP++);
    uint8_t high = memory_read(cpu->memory, cpu->SP++);
    cpu->AF = (high << 8) | low;
    cpu->F &= 0xF0;
    cpu->cycle_count += 3;
}

static inline void pop_r16(uint16_t *reg, CPUState *cpu)
{
    uint8_t low = memory_read(cpu->memory, cpu->SP++);
    uint8_t high = memory_read(cpu->memory, cpu->SP++);
    *reg = (high << 8) | low;
    cpu->cycle_count += 3;
}

static inline void push_af(CPUState *cpu)
{
    memory_write(cpu->memory, --cpu->SP, (uint8_t)((cpu->AF >> 8) & 0x00FF));
    memory_write(cpu->memory, --cpu->SP, (uint8_t)(cpu->AF & 0x00FF));
    cpu->cycle_count += 4;
}

static inline void push_r16(uint16_t reg, CPUState *cpu)
{
    memory_write(cpu->memory, --cpu->SP, (uint8_t)((reg >> 8) & 0x00FF));
    memory_write(cpu->memory, --cpu->SP, (uint8_t)(reg & 0x00FF));
    cpu->cycle_count += 4;
}

// MISCELLANEOUS INSTRUCTIONS

static inline void daa(CPUState *cpu)
{
    uint8_t a = cpu->A;
    uint8_t correction = 0;
    bool set_carry = false;

    if (!(cpu->F & FLAG_N))
    {
        if ((cpu->F & FLAG_H) || (a & 0x0F) > 0x09)
            correction |= 0x06;
        if ((cpu->F & FLAG_C) || a > 0x99)
        {
            correction |= 0x60;
            set_carry = true;
        }
        a += correction;
    }
    else
    {
        if (cpu->F & FLAG_H)
            correction |= 0x06;
        if (cpu->F & FLAG_C)
        {
            correction |= 0x60;
            set_carry = true;
        }
        a -= correction;
    }

    cpu->A = a;

    cpu->F &= ~(FLAG_Z | FLAG_H | FLAG_C);
    if (cpu->A == 0)
        cpu->F |= FLAG_Z;
    if (set_carry)
        cpu->F |= FLAG_C;

    cpu->cycle_count += 1;
}

static inline void nop(CPUState *cpu)
{
    cpu->cycle_count += 1;
}

static inline void stop(CPUState *cpu)
{
    cpu->stop = true;
    cpu->cycle_count += 1;
}

// CARRY FLAG INSTRUCTIONS

static inline void ccf(CPUState *cpu)
{
    cpu->F ^= FLAG_C;
    cpu->F &= ~FLAG_N;
    cpu->F &= ~FLAG_H;
    cpu->cycle_count += 1;
}

static inline void scf(CPUState *cpu)
{
    cpu->F |= FLAG_C;
    cpu->F &= ~FLAG_N;
    cpu->F &= ~FLAG_H;
    cpu->cycle_count += 1;
}

#endif // INSTRUCTIONS

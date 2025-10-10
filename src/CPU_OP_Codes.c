#include "CPU_OP_Codes.h"

void cpu_op_0x00(CPUState *cpu)
{
    nop(cpu);
}
void cpu_op_0x01(CPUState *cpu)
{
    ld_r16_n16(&cpu->BC, cpu);
}
void cpu_op_0x02(CPUState *cpu)
{
    ld_r16_a(cpu->BC, cpu);
}
void cpu_op_0x03(CPUState *cpu)
{
    inc_r16(&cpu->BC, cpu);
}
void cpu_op_0x04(CPUState *cpu)
{
    inc_r8(&cpu->B, cpu);
}
void cpu_op_0x05(CPUState *cpu)
{
    dec_r8(&cpu->B, cpu);
}
void cpu_op_0x06(CPUState *cpu)
{
    ld_r8_n8(&cpu->B, memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0x07(CPUState *cpu)
{
    rlca(cpu);
}
void cpu_op_0x08(CPUState *cpu)
{
    ld_n16_sp(cpu);
}
void cpu_op_0x09(CPUState *cpu)
{
    add_hl_r16(cpu->BC, cpu);
}
void cpu_op_0x0A(CPUState *cpu)
{
    ld_a_r16(cpu->BC, cpu);
}
void cpu_op_0x0B(CPUState *cpu)
{
    dec_r16(&cpu->BC, cpu);
}
void cpu_op_0x0C(CPUState *cpu)
{
    inc_r8(&cpu->C, cpu);
}
void cpu_op_0x0D(CPUState *cpu)
{
    dec_r8(&cpu->C, cpu);
}
void cpu_op_0x0E(CPUState *cpu)
{
    ld_r8_n8(&cpu->C, memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0x0F(CPUState *cpu)
{
    rrca(cpu);
}
void cpu_op_0x10(CPUState *cpu)
{
    stop(cpu);
}
void cpu_op_0x11(CPUState *cpu)
{
    ld_r16_n16(&cpu->DE, cpu);
}
void cpu_op_0x12(CPUState *cpu)
{
    ld_r16_a(cpu->DE, cpu);
}
void cpu_op_0x13(CPUState *cpu)
{
    inc_r16(&cpu->DE, cpu);
}
void cpu_op_0x14(CPUState *cpu)
{
    inc_r8(&cpu->D, cpu);
}
void cpu_op_0x15(CPUState *cpu)
{
    dec_r8(&cpu->D, cpu);
}
void cpu_op_0x16(CPUState *cpu)
{
    ld_r8_n8(&cpu->D, memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0x17(CPUState *cpu)
{
    rla(cpu);
}
void cpu_op_0x18(CPUState *cpu)
{
    jr_e8((int8_t)memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0x19(CPUState *cpu)
{
    add_hl_r16(cpu->DE, cpu);
}
void cpu_op_0x1A(CPUState *cpu)
{
    ld_a_r16(cpu->DE, cpu);
}
void cpu_op_0x1B(CPUState *cpu)
{
    dec_r16(&cpu->DE, cpu);
}
void cpu_op_0x1C(CPUState *cpu)
{
    inc_r8(&cpu->E, cpu);
}
void cpu_op_0x1D(CPUState *cpu)
{
    dec_r8(&cpu->E, cpu);
}
void cpu_op_0x1E(CPUState *cpu)
{
    ld_r8_n8(&cpu->E, memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0x1F(CPUState *cpu)
{
    rra(cpu);
}
void cpu_op_0x20(CPUState *cpu)
{
    jr_cc_e8((cpu->F & (1 << 7)) == 0, (int8_t)memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0x21(CPUState *cpu)
{
    ld_r16_n16(&cpu->HL, cpu);
}
void cpu_op_0x22(CPUState *cpu)
{
    ld_hl_inc_a(cpu);
}
void cpu_op_0x23(CPUState *cpu)
{
    inc_r16(&cpu->HL, cpu);
}
void cpu_op_0x24(CPUState *cpu)
{
    inc_r8(&cpu->H, cpu);
}
void cpu_op_0x25(CPUState *cpu)
{
    dec_r8(&cpu->H, cpu);
}
void cpu_op_0x26(CPUState *cpu)
{
    ld_r8_n8(&cpu->H, memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0x27(CPUState *cpu)
{
    daa(cpu);
}
void cpu_op_0x28(CPUState *cpu)
{
    jr_cc_e8((cpu->F & (1 << 7)) != 0, (int8_t)memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0x29(CPUState *cpu)
{
    add_hl_r16(cpu->HL, cpu);
}
void cpu_op_0x2A(CPUState *cpu)
{
    ld_a_hl_inc(cpu);
}
void cpu_op_0x2B(CPUState *cpu)
{
    dec_r16(&cpu->HL, cpu);
}
void cpu_op_0x2C(CPUState *cpu)
{
    inc_r8(&cpu->L, cpu);
}
void cpu_op_0x2D(CPUState *cpu)
{
    dec_r8(&cpu->L, cpu);
}
void cpu_op_0x2E(CPUState *cpu)
{
    ld_r8_n8(&cpu->L, memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0x2F(CPUState *cpu)
{
    cpl(cpu);
}
void cpu_op_0x30(CPUState *cpu)
{
    jr_cc_e8((cpu->F & (1 << 4)) == 0, (int8_t)memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0x31(CPUState *cpu)
{
    ld_sp_n16(memory_read(cpu->memory, cpu->PC) | (memory_read(cpu->memory, cpu->PC + 1) << 8), cpu);
    cpu->PC += 2;
}
void cpu_op_0x32(CPUState *cpu)
{
    ld_hl_dec_a(cpu);
}
void cpu_op_0x33(CPUState *cpu)
{
    inc_r16(&cpu->SP, cpu);
}
void cpu_op_0x34(CPUState *cpu)
{
    inc_hl(cpu);
}
void cpu_op_0x35(CPUState *cpu)
{
    dec_hl(cpu);
}
void cpu_op_0x36(CPUState *cpu)
{
    ld_hl_n8(memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0x37(CPUState *cpu)
{
    scf(cpu);
}
void cpu_op_0x38(CPUState *cpu)
{
    jr_cc_e8((cpu->F & FLAG_C) != 0, (int8_t)memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0x39(CPUState *cpu)
{
    add_hl_sp(cpu);
}
void cpu_op_0x3A(CPUState *cpu)
{
    ld_a_hl_dec(cpu);
}
void cpu_op_0x3B(CPUState *cpu)
{
    dec_r16(&cpu->SP, cpu);
}
void cpu_op_0x3C(CPUState *cpu)
{
    inc_r8(&cpu->A, cpu);
}
void cpu_op_0x3D(CPUState *cpu)
{
    dec_r8(&cpu->A, cpu);
}
void cpu_op_0x3E(CPUState *cpu)
{
    ld_r8_n8(&cpu->A, memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0x3F(CPUState *cpu)
{
    ccf(cpu);
}
void cpu_op_0x40(CPUState *cpu)
{
    ld_r8_r8(&cpu->B, &cpu->B, cpu);
}
void cpu_op_0x41(CPUState *cpu)
{
    ld_r8_r8(&cpu->B, &cpu->C, cpu);
}
void cpu_op_0x42(CPUState *cpu)
{
    ld_r8_r8(&cpu->B, &cpu->D, cpu);
}
void cpu_op_0x43(CPUState *cpu)
{
    ld_r8_r8(&cpu->B, &cpu->E, cpu);
}
void cpu_op_0x44(CPUState *cpu)
{
    ld_r8_r8(&cpu->B, &cpu->H, cpu);
}
void cpu_op_0x45(CPUState *cpu)
{
    ld_r8_r8(&cpu->B, &cpu->L, cpu);
}
void cpu_op_0x46(CPUState *cpu)
{
    ld_r8_hl(&cpu->B, cpu);
}
void cpu_op_0x47(CPUState *cpu)
{
    ld_r8_r8(&cpu->B, &cpu->A, cpu);
}
void cpu_op_0x48(CPUState *cpu)
{
    ld_r8_r8(&cpu->C, &cpu->B, cpu);
}
void cpu_op_0x49(CPUState *cpu)
{
    ld_r8_r8(&cpu->C, &cpu->C, cpu);
}
void cpu_op_0x4A(CPUState *cpu)
{
    ld_r8_r8(&cpu->C, &cpu->D, cpu);
}
void cpu_op_0x4B(CPUState *cpu)
{
    ld_r8_r8(&cpu->C, &cpu->E, cpu);
}
void cpu_op_0x4C(CPUState *cpu)
{
    ld_r8_r8(&cpu->C, &cpu->H, cpu);
}
void cpu_op_0x4D(CPUState *cpu)
{
    ld_r8_r8(&cpu->C, &cpu->L, cpu);
}
void cpu_op_0x4E(CPUState *cpu)
{
    ld_r8_hl(&cpu->C, cpu);
}
void cpu_op_0x4F(CPUState *cpu)
{
    ld_r8_r8(&cpu->C, &cpu->A, cpu);
}
void cpu_op_0x50(CPUState *cpu)
{
    ld_r8_r8(&cpu->D, &cpu->B, cpu);
}
void cpu_op_0x51(CPUState *cpu)
{
    ld_r8_r8(&cpu->D, &cpu->C, cpu);
}
void cpu_op_0x52(CPUState *cpu)
{
    ld_r8_r8(&cpu->D, &cpu->D, cpu);
}
void cpu_op_0x53(CPUState *cpu)
{
    ld_r8_r8(&cpu->D, &cpu->E, cpu);
}
void cpu_op_0x54(CPUState *cpu)
{
    ld_r8_r8(&cpu->D, &cpu->H, cpu);
}
void cpu_op_0x55(CPUState *cpu)
{
    ld_r8_r8(&cpu->D, &cpu->L, cpu);
}
void cpu_op_0x56(CPUState *cpu)
{
    ld_r8_hl(&cpu->D, cpu);
}
void cpu_op_0x57(CPUState *cpu)
{
    ld_r8_r8(&cpu->D, &cpu->A, cpu);
}
void cpu_op_0x58(CPUState *cpu)
{
    ld_r8_r8(&cpu->E, &cpu->B, cpu);
}
void cpu_op_0x59(CPUState *cpu)
{
    ld_r8_r8(&cpu->E, &cpu->C, cpu);
}
void cpu_op_0x5A(CPUState *cpu)
{
    ld_r8_r8(&cpu->E, &cpu->D, cpu);
}
void cpu_op_0x5B(CPUState *cpu)
{
    ld_r8_r8(&cpu->E, &cpu->E, cpu);
}
void cpu_op_0x5C(CPUState *cpu)
{
    ld_r8_r8(&cpu->E, &cpu->H, cpu);
}
void cpu_op_0x5D(CPUState *cpu)
{
    ld_r8_r8(&cpu->E, &cpu->L, cpu);
}
void cpu_op_0x5E(CPUState *cpu)
{
    ld_r8_hl(&cpu->E, cpu);
}
void cpu_op_0x5F(CPUState *cpu)
{
    ld_r8_r8(&cpu->E, &cpu->A, cpu);
}
void cpu_op_0x60(CPUState *cpu)
{
    ld_r8_r8(&cpu->H, &cpu->B, cpu);
}
void cpu_op_0x61(CPUState *cpu)
{
    ld_r8_r8(&cpu->H, &cpu->C, cpu);
}
void cpu_op_0x62(CPUState *cpu)
{
    ld_r8_r8(&cpu->H, &cpu->D, cpu);
}
void cpu_op_0x63(CPUState *cpu)
{
    ld_r8_r8(&cpu->H, &cpu->E, cpu);
}
void cpu_op_0x64(CPUState *cpu)
{
    ld_r8_r8(&cpu->H, &cpu->H, cpu);
}
void cpu_op_0x65(CPUState *cpu)
{
    ld_r8_r8(&cpu->H, &cpu->L, cpu);
}
void cpu_op_0x66(CPUState *cpu)
{
    ld_r8_hl(&cpu->H, cpu);
}
void cpu_op_0x67(CPUState *cpu)
{
    ld_r8_r8(&cpu->H, &cpu->A, cpu);
}
void cpu_op_0x68(CPUState *cpu)
{
    ld_r8_r8(&cpu->L, &cpu->B, cpu);
}
void cpu_op_0x69(CPUState *cpu)
{
    ld_r8_r8(&cpu->L, &cpu->C, cpu);
}
void cpu_op_0x6A(CPUState *cpu)
{
    ld_r8_r8(&cpu->L, &cpu->D, cpu);
}
void cpu_op_0x6B(CPUState *cpu)
{
    ld_r8_r8(&cpu->L, &cpu->E, cpu);
}
void cpu_op_0x6C(CPUState *cpu)
{
    ld_r8_r8(&cpu->L, &cpu->H, cpu);
}
void cpu_op_0x6D(CPUState *cpu)
{
    ld_r8_r8(&cpu->L, &cpu->L, cpu);
}
void cpu_op_0x6E(CPUState *cpu)
{
    ld_r8_hl(&cpu->L, cpu);
}
void cpu_op_0x6F(CPUState *cpu)
{
    ld_r8_r8(&cpu->L, &cpu->A, cpu);
}
void cpu_op_0x70(CPUState *cpu)
{
    ld_hl_r8(cpu->B, cpu);
}
void cpu_op_0x71(CPUState *cpu)
{
    ld_hl_r8(cpu->C, cpu);
}
void cpu_op_0x72(CPUState *cpu)
{
    ld_hl_r8(cpu->D, cpu);
}
void cpu_op_0x73(CPUState *cpu)
{
    ld_hl_r8(cpu->E, cpu);
}
void cpu_op_0x74(CPUState *cpu)
{
    ld_hl_r8(cpu->H, cpu);
}
void cpu_op_0x75(CPUState *cpu)
{
    ld_hl_r8(cpu->L, cpu);
}
void cpu_op_0x76(CPUState *cpu)
{
    halt(cpu);
}
void cpu_op_0x77(CPUState *cpu)
{
    ld_hl_r8(cpu->A, cpu);
}
void cpu_op_0x78(CPUState *cpu)
{
    ld_r8_r8(&cpu->A, &cpu->B, cpu);
}
void cpu_op_0x79(CPUState *cpu)
{
    ld_r8_r8(&cpu->A, &cpu->C, cpu);
}
void cpu_op_0x7A(CPUState *cpu)
{
    ld_r8_r8(&cpu->A, &cpu->D, cpu);
}
void cpu_op_0x7B(CPUState *cpu)
{
    ld_r8_r8(&cpu->A, &cpu->E, cpu);
}
void cpu_op_0x7C(CPUState *cpu)
{
    ld_r8_r8(&cpu->A, &cpu->H, cpu);
}
void cpu_op_0x7D(CPUState *cpu)
{
    ld_r8_r8(&cpu->A, &cpu->L, cpu);
}
void cpu_op_0x7E(CPUState *cpu)
{
    ld_r8_hl(&cpu->A, cpu);
}
void cpu_op_0x7F(CPUState *cpu)
{
    ld_r8_r8(&cpu->A, &cpu->A, cpu);
}
void cpu_op_0x80(CPUState *cpu)
{
    add_a_r8(cpu->B, cpu);
}
void cpu_op_0x81(CPUState *cpu)
{
    add_a_r8(cpu->C, cpu);
}
void cpu_op_0x82(CPUState *cpu)
{
    add_a_r8(cpu->D, cpu);
}
void cpu_op_0x83(CPUState *cpu)
{
    add_a_r8(cpu->E, cpu);
}
void cpu_op_0x84(CPUState *cpu)
{
    add_a_r8(cpu->H, cpu);
}
void cpu_op_0x85(CPUState *cpu)
{
    add_a_r8(cpu->L, cpu);
}
void cpu_op_0x86(CPUState *cpu)
{
    add_a_hl(cpu);
}
void cpu_op_0x87(CPUState *cpu)
{
    add_a_r8(cpu->A, cpu);
}
void cpu_op_0x88(CPUState *cpu)
{
    adc_a_r8(cpu->B, cpu);
}
void cpu_op_0x89(CPUState *cpu)
{
    adc_a_r8(cpu->C, cpu);
}
void cpu_op_0x8A(CPUState *cpu)
{
    adc_a_r8(cpu->D, cpu);
}
void cpu_op_0x8B(CPUState *cpu)
{
    adc_a_r8(cpu->E, cpu);
}
void cpu_op_0x8C(CPUState *cpu)
{
    adc_a_r8(cpu->H, cpu);
}
void cpu_op_0x8D(CPUState *cpu)
{
    adc_a_r8(cpu->L, cpu);
}
void cpu_op_0x8E(CPUState *cpu)
{
    adc_a_hl(cpu);
}
void cpu_op_0x8F(CPUState *cpu)
{
    adc_a_r8(cpu->A, cpu);
}
void cpu_op_0x90(CPUState *cpu)
{
    sub_r8(cpu->B, cpu);
}
void cpu_op_0x91(CPUState *cpu)
{
    sub_r8(cpu->C, cpu);
}
void cpu_op_0x92(CPUState *cpu)
{
    sub_r8(cpu->D, cpu);
}
void cpu_op_0x93(CPUState *cpu)
{
    sub_r8(cpu->E, cpu);
}
void cpu_op_0x94(CPUState *cpu)
{
    sub_r8(cpu->H, cpu);
}
void cpu_op_0x95(CPUState *cpu)
{
    sub_r8(cpu->L, cpu);
}
void cpu_op_0x96(CPUState *cpu)
{
    sub_hl(cpu);
}
void cpu_op_0x97(CPUState *cpu)
{
    sub_r8(cpu->A, cpu);
}
void cpu_op_0x98(CPUState *cpu)
{
    sbc_a_r8(cpu->B, cpu);
}
void cpu_op_0x99(CPUState *cpu)
{
    sbc_a_r8(cpu->C, cpu);
}
void cpu_op_0x9A(CPUState *cpu)
{
    sbc_a_r8(cpu->D, cpu);
}
void cpu_op_0x9B(CPUState *cpu)
{
    sbc_a_r8(cpu->E, cpu);
}
void cpu_op_0x9C(CPUState *cpu)
{
    sbc_a_r8(cpu->H, cpu);
}
void cpu_op_0x9D(CPUState *cpu)
{
    sbc_a_r8(cpu->L, cpu);
}
void cpu_op_0x9E(CPUState *cpu)
{
    sbc_a_hl(cpu);
}
void cpu_op_0x9F(CPUState *cpu)
{
    sbc_a_r8(cpu->A, cpu);
}
void cpu_op_0xA0(CPUState *cpu)
{
    and_r8(cpu->B, cpu);
}
void cpu_op_0xA1(CPUState *cpu)
{
    and_r8(cpu->C, cpu);
}
void cpu_op_0xA2(CPUState *cpu)
{
    and_r8(cpu->D, cpu);
}
void cpu_op_0xA3(CPUState *cpu)
{
    and_r8(cpu->E, cpu);
}
void cpu_op_0xA4(CPUState *cpu)
{
    and_r8(cpu->H, cpu);
}
void cpu_op_0xA5(CPUState *cpu)
{
    and_r8(cpu->L, cpu);
}
void cpu_op_0xA6(CPUState *cpu)
{
    and_hl(cpu);
}
void cpu_op_0xA7(CPUState *cpu)
{
    and_r8(cpu->A, cpu);
}
void cpu_op_0xA8(CPUState *cpu)
{
    xor_a_r8(cpu->B, cpu);
}
void cpu_op_0xA9(CPUState *cpu)
{
    xor_a_r8(cpu->C, cpu);
}
void cpu_op_0xAA(CPUState *cpu)
{
    xor_a_r8(cpu->D, cpu);
}
void cpu_op_0xAB(CPUState *cpu)
{
    xor_a_r8(cpu->E, cpu);
}
void cpu_op_0xAC(CPUState *cpu)
{
    xor_a_r8(cpu->H, cpu);
}
void cpu_op_0xAD(CPUState *cpu)
{
    xor_a_r8(cpu->L, cpu);
}
void cpu_op_0xAE(CPUState *cpu)
{
    xor_a_hl(cpu);
}
void cpu_op_0xAF(CPUState *cpu)
{
    xor_a_r8(cpu->A, cpu);
}
void cpu_op_0xB0(CPUState *cpu)
{
    or_a_r8(cpu->B, cpu);
}
void cpu_op_0xB1(CPUState *cpu)
{
    or_a_r8(cpu->C, cpu);
}
void cpu_op_0xB2(CPUState *cpu)
{
    or_a_r8(cpu->D, cpu);
}
void cpu_op_0xB3(CPUState *cpu)
{
    or_a_r8(cpu->E, cpu);
}
void cpu_op_0xB4(CPUState *cpu)
{
    or_a_r8(cpu->H, cpu);
}
void cpu_op_0xB5(CPUState *cpu)
{
    or_a_r8(cpu->L, cpu);
}
void cpu_op_0xB6(CPUState *cpu)
{
    or_a_hl(cpu);
}
void cpu_op_0xB7(CPUState *cpu)
{
    or_a_r8(cpu->A, cpu);
}
void cpu_op_0xB8(CPUState *cpu)
{
    cp_a_r8(cpu->B, cpu);
}
void cpu_op_0xB9(CPUState *cpu)
{
    cp_a_r8(cpu->C, cpu);
}
void cpu_op_0xBA(CPUState *cpu)
{
    cp_a_r8(cpu->D, cpu);
}
void cpu_op_0xBB(CPUState *cpu)
{
    cp_a_r8(cpu->E, cpu);
}
void cpu_op_0xBC(CPUState *cpu)
{
    cp_a_r8(cpu->H, cpu);
}
void cpu_op_0xBD(CPUState *cpu)
{
    cp_a_r8(cpu->L, cpu);
}
void cpu_op_0xBE(CPUState *cpu)
{
    cp_a_hl(cpu);
}
void cpu_op_0xBF(CPUState *cpu)
{
    cp_a_r8(cpu->A, cpu);
}
void cpu_op_0xC0(CPUState *cpu)
{
    ret_cc((cpu->F & (1 << 7)) == 0, cpu);
}
void cpu_op_0xC1(CPUState *cpu)
{
    pop_r16(&cpu->BC, cpu);
}
void cpu_op_0xC2(CPUState *cpu)
{
    jp_cc_n16((cpu->F & (1 << 7)) == 0, cpu);
}
void cpu_op_0xC3(CPUState *cpu)
{
    jp_n16(cpu);
}
void cpu_op_0xC4(CPUState *cpu)
{
    call_cc_n16((cpu->F & FLAG_Z) == 0, cpu);
}
void cpu_op_0xC5(CPUState *cpu)
{
    push_r16(cpu->BC, cpu);
}
void cpu_op_0xC6(CPUState *cpu)
{
    add_a_n8(memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0xC7(CPUState *cpu)
{
    rst(0, cpu);
}
void cpu_op_0xC8(CPUState *cpu)
{
    ret_cc((cpu->F & FLAG_Z) != 0, cpu);
}
void cpu_op_0xC9(CPUState *cpu)
{
    ret(cpu);
}
void cpu_op_0xCA(CPUState *cpu)
{
    jp_cc_n16((cpu->F & FLAG_Z) != 0, cpu);
}
void cpu_op_0xCB(CPUState *cpu)
{
    uint8_t cb_opcode = memory_read(cpu->memory, cpu->PC++);
    extended_opcodes[cb_opcode](cpu);
}
void cpu_op_0xCC(CPUState *cpu)
{
    call_cc_n16((cpu->F & FLAG_Z) != 0, cpu);
}
void cpu_op_0xCD(CPUState *cpu)
{
    call_n16(cpu);
}
void cpu_op_0xCE(CPUState *cpu)
{
    adc_a_n8(memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0xCF(CPUState *cpu)
{
    rst(1, cpu);
}
void cpu_op_0xD0(CPUState *cpu)
{
    ret_cc((cpu->F & FLAG_C) == 0, cpu);
}
void cpu_op_0xD1(CPUState *cpu)
{
    pop_r16(&cpu->DE, cpu);
}
void cpu_op_0xD2(CPUState *cpu)
{
    jp_cc_n16((cpu->F & FLAG_C) == 0, cpu);
}
void cpu_op_0xD3(CPUState *cpu)
{
    printf("Opcode 0xD3 not implemented yet.\n"); // TODO: implementation de l'opcode 0xD3
    printf("PC: 0x%04X", cpu->PC);
    abort();
}
void cpu_op_0xD4(CPUState *cpu)
{
    call_cc_n16((cpu->F & FLAG_C) == 0, cpu);
}
void cpu_op_0xD5(CPUState *cpu)
{
    push_r16(cpu->DE, cpu);
}
void cpu_op_0xD6(CPUState *cpu)
{
    sub_n8(memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0xD7(CPUState *cpu)
{
    rst(2, cpu);
}
void cpu_op_0xD8(CPUState *cpu)
{
    ret_cc((cpu->F & FLAG_C) != 0, cpu);
}
void cpu_op_0xD9(CPUState *cpu)
{
    reti(cpu);
}
void cpu_op_0xDA(CPUState *cpu)
{
    jp_cc_n16((cpu->F & FLAG_C) != 0, cpu);
}
void cpu_op_0xDB(CPUState *cpu)
{
    printf("Opcode 0xDB not implemented yet.\n"); // TODO: implementation de l'opcode 0xDB
    printf("PC: 0x%04X", cpu->PC);
    abort();
}
void cpu_op_0xDC(CPUState *cpu)
{
    call_cc_n16((cpu->F & FLAG_C) != 0, cpu);
}
void cpu_op_0xDD(CPUState *cpu)
{
    printf("Opcode 0xDD not implemented yet.\n"); // TODO: implementation de l'opcode 0xDD
    printf("PC: 0x%04X", cpu->PC);
    abort();
}
void cpu_op_0xDE(CPUState *cpu)
{
    sbc_a_n8(cpu);
}
void cpu_op_0xDF(CPUState *cpu)
{
    rst(3, cpu);
}
void cpu_op_0xE0(CPUState *cpu)
{
    ldh_n8_a(memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0xE1(CPUState *cpu)
{
    pop_r16(&cpu->HL, cpu);
}
void cpu_op_0xE2(CPUState *cpu)
{
    ldh_c_a(cpu);
}
void cpu_op_0xE3(CPUState *cpu)
{
    printf("Opcode 0xE3 not implemented yet.\n"); // TODO: implementation de l'opcode 0xE3
    printf("PC: 0x%04X", cpu->PC);
    abort();
}
void cpu_op_0xE4(CPUState *cpu)
{
    printf("Opcode 0xE4 not implemented yet.\n"); // TODO: implementation de l'opcode 0xE4
    printf("PC: 0x%04X", cpu->PC);
    abort();
}
void cpu_op_0xE5(CPUState *cpu)
{
    push_r16(cpu->HL, cpu);
}
void cpu_op_0xE6(CPUState *cpu)
{
    and_n8(memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0xE7(CPUState *cpu)
{
    rst(4, cpu);
}
void cpu_op_0xE8(CPUState *cpu)
{
    add_sp_e8(cpu);
}
void cpu_op_0xE9(CPUState *cpu)
{
    jp_hl(cpu);
}
void cpu_op_0xEA(CPUState *cpu)
{
    ld_n16_a(memory_read(cpu->memory, cpu->PC) | (memory_read(cpu->memory, cpu->PC + 1) << 8), cpu);
    cpu->PC += 2;
}
void cpu_op_0xEB(CPUState *cpu)
{
    printf("Opcode 0xEB not implemented yet.\n"); // TODO: implementation de l'opcode 0xEB
    printf("PC: 0x%04X", cpu->PC);
    abort();
}
void cpu_op_0xEC(CPUState *cpu)
{
    printf("Opcode 0xEC not implemented yet.\n"); // TODO: implementation de l'opcode 0xEC
    printf("PC: 0x%04X", cpu->PC);
    abort();
}
void cpu_op_0xED(CPUState *cpu)
{
    printf("Opcode 0xED not implemented yet.\n"); // TODO: implementation de l'opcode 0xED
    printf("PC: 0x%04X", cpu->PC);
    abort();
}
void cpu_op_0xEE(CPUState *cpu)
{
    xor_a_n8(memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0xEF(CPUState *cpu)
{
    rst(5, cpu);
}
void cpu_op_0xF0(CPUState *cpu)
{
    ldh_a_a8(cpu);
}
void cpu_op_0xF1(CPUState *cpu)
{
    pop_af(cpu);
}
void cpu_op_0xF2(CPUState *cpu)
{
    ldh_a_c(cpu);
}
void cpu_op_0xF3(CPUState *cpu)
{
    di(cpu);
}
void cpu_op_0xF4(CPUState *cpu)
{
    printf("Opcode 0xF4 not implemented yet.\n"); // TODO: implementation de l'opcode 0xF4
    printf("PC: 0x%04X", cpu->PC);
    abort();
}
void cpu_op_0xF5(CPUState *cpu)
{
    push_af(cpu);
}
void cpu_op_0xF6(CPUState *cpu)
{
    or_a_n8(memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0xF7(CPUState *cpu)
{
    rst(6, cpu);
}
void cpu_op_0xF8(CPUState *cpu)
{
    ld_hl_sp_e8(cpu);
}
void cpu_op_0xF9(CPUState *cpu)
{
    ld_sp_hl(cpu);
}
void cpu_op_0xFA(CPUState *cpu)
{
    ld_a_n16(cpu);
}
void cpu_op_0xFB(CPUState *cpu)
{
    ei(cpu);
}
void cpu_op_0xFC(CPUState *cpu)
{
    printf("Opcode 0xFC not implemented yet.\n"); // TODO: implementation de l'opcode 0xFC
    printf("PC: 0x%04X", cpu->PC);
    abort();
}
void cpu_op_0xFD(CPUState *cpu)
{
    printf("Opcode 0xFD not implemented yet.\n"); // TODO: implementation de l'opcode 0xFD
    printf("PC: 0x%04X", cpu->PC);
    abort();
}
void cpu_op_0xFE(CPUState *cpu)
{
    cp_a_n8(memory_read(cpu->memory, cpu->PC++), cpu);
}
void cpu_op_0xFF(CPUState *cpu)
{
    rst(7, cpu);
}

void cpu_op_0xCB00(CPUState *cpu)
{
    rlc_r8(&cpu->B, cpu);
}
void cpu_op_0xCB01(CPUState *cpu)
{
    rlc_r8(&cpu->C, cpu);
}
void cpu_op_0xCB02(CPUState *cpu)
{
    rlc_r8(&cpu->D, cpu);
}
void cpu_op_0xCB03(CPUState *cpu)
{
    rlc_r8(&cpu->E, cpu);
}
void cpu_op_0xCB04(CPUState *cpu)
{
    rlc_r8(&cpu->H, cpu);
}
void cpu_op_0xCB05(CPUState *cpu)
{
    rlc_r8(&cpu->L, cpu);
}
void cpu_op_0xCB06(CPUState *cpu)
{
    rlc_hl(cpu);
}
void cpu_op_0xCB07(CPUState *cpu)
{
    rlc_r8(&cpu->A, cpu);
}
void cpu_op_0xCB08(CPUState *cpu)
{
    rrc_r8(&cpu->B, cpu);
}
void cpu_op_0xCB09(CPUState *cpu)
{
    rrc_r8(&cpu->C, cpu);
}
void cpu_op_0xCB0A(CPUState *cpu)
{
    rrc_r8(&cpu->D, cpu);
}
void cpu_op_0xCB0B(CPUState *cpu)
{
    rrc_r8(&cpu->E, cpu);
}
void cpu_op_0xCB0C(CPUState *cpu)
{
    rrc_r8(&cpu->H, cpu);
}
void cpu_op_0xCB0D(CPUState *cpu)
{
    rrc_r8(&cpu->L, cpu);
}
void cpu_op_0xCB0E(CPUState *cpu)
{
    rrc_hl(cpu);
}
void cpu_op_0xCB0F(CPUState *cpu)
{
    rrc_r8(&cpu->A, cpu);
}
void cpu_op_0xCB10(CPUState *cpu)
{
    rl_r8(&cpu->B, cpu);
}
void cpu_op_0xCB11(CPUState *cpu)
{
    rl_r8(&cpu->C, cpu);
}
void cpu_op_0xCB12(CPUState *cpu)
{
    rl_r8(&cpu->D, cpu);
}
void cpu_op_0xCB13(CPUState *cpu)
{
    rl_r8(&cpu->E, cpu);
}
void cpu_op_0xCB14(CPUState *cpu)
{
    rl_r8(&cpu->H, cpu);
}
void cpu_op_0xCB15(CPUState *cpu)
{
    rl_r8(&cpu->L, cpu);
}
void cpu_op_0xCB16(CPUState *cpu)
{
    rl_hl(cpu);
}
void cpu_op_0xCB17(CPUState *cpu)
{
    rl_r8(&cpu->A, cpu);
}
void cpu_op_0xCB18(CPUState *cpu)
{
    rr_r8(&cpu->B, cpu);
}
void cpu_op_0xCB19(CPUState *cpu)
{
    rr_r8(&cpu->C, cpu);
}
void cpu_op_0xCB1A(CPUState *cpu)
{
    rr_r8(&cpu->D, cpu);
}
void cpu_op_0xCB1B(CPUState *cpu)
{
    rr_r8(&cpu->E, cpu);
}
void cpu_op_0xCB1C(CPUState *cpu)
{
    rr_r8(&cpu->H, cpu);
}
void cpu_op_0xCB1D(CPUState *cpu)
{
    rr_r8(&cpu->L, cpu);
}
void cpu_op_0xCB1E(CPUState *cpu)
{
    rr_hl(cpu);
}
void cpu_op_0xCB1F(CPUState *cpu)
{
    rr_r8(&cpu->A, cpu);
}
void cpu_op_0xCB20(CPUState *cpu)
{
    sla_r8(&cpu->B, cpu);
}
void cpu_op_0xCB21(CPUState *cpu)
{
    sla_r8(&cpu->C, cpu);
}
void cpu_op_0xCB22(CPUState *cpu)
{
    sla_r8(&cpu->D, cpu);
}
void cpu_op_0xCB23(CPUState *cpu)
{
    sla_r8(&cpu->E, cpu);
}
void cpu_op_0xCB24(CPUState *cpu)
{
    sla_r8(&cpu->H, cpu);
}
void cpu_op_0xCB25(CPUState *cpu)
{
    sla_r8(&cpu->L, cpu);
}
void cpu_op_0xCB26(CPUState *cpu)
{
    sla_hl(cpu);
}
void cpu_op_0xCB27(CPUState *cpu)
{
    sla_r8(&cpu->A, cpu);
}
void cpu_op_0xCB28(CPUState *cpu)
{
    sra_r8(&cpu->B, cpu);
}
void cpu_op_0xCB29(CPUState *cpu)
{
    sra_r8(&cpu->C, cpu);
}
void cpu_op_0xCB2A(CPUState *cpu)
{
    sra_r8(&cpu->D, cpu);
}
void cpu_op_0xCB2B(CPUState *cpu)
{
    sra_r8(&cpu->E, cpu);
}
void cpu_op_0xCB2C(CPUState *cpu)
{
    sra_r8(&cpu->H, cpu);
}
void cpu_op_0xCB2D(CPUState *cpu)
{
    sra_r8(&cpu->L, cpu);
}
void cpu_op_0xCB2E(CPUState *cpu)
{
    sra_hl(cpu);
}
void cpu_op_0xCB2F(CPUState *cpu)
{
    sra_r8(&cpu->A, cpu);
}
void cpu_op_0xCB30(CPUState *cpu)
{
    swap_r8(&cpu->B, cpu);
}
void cpu_op_0xCB31(CPUState *cpu)
{
    swap_r8(&cpu->C, cpu);
}
void cpu_op_0xCB32(CPUState *cpu)
{
    swap_r8(&cpu->D, cpu);
}
void cpu_op_0xCB33(CPUState *cpu)
{
    swap_r8(&cpu->E, cpu);
}
void cpu_op_0xCB34(CPUState *cpu)
{
    swap_r8(&cpu->H, cpu);
}
void cpu_op_0xCB35(CPUState *cpu)
{
    swap_r8(&cpu->L, cpu);
}
void cpu_op_0xCB36(CPUState *cpu)
{
    swap_hl(cpu);
}
void cpu_op_0xCB37(CPUState *cpu)
{
    swap_r8(&cpu->A, cpu);
}
void cpu_op_0xCB38(CPUState *cpu)
{
    srl_r8(&cpu->B, cpu);
}
void cpu_op_0xCB39(CPUState *cpu)
{
    srl_r8(&cpu->C, cpu);
}
void cpu_op_0xCB3A(CPUState *cpu)
{
    srl_r8(&cpu->D, cpu);
}
void cpu_op_0xCB3B(CPUState *cpu)
{
    srl_r8(&cpu->E, cpu);
}
void cpu_op_0xCB3C(CPUState *cpu)
{
    srl_r8(&cpu->H, cpu);
}
void cpu_op_0xCB3D(CPUState *cpu)
{
    srl_r8(&cpu->L, cpu);
}
void cpu_op_0xCB3E(CPUState *cpu)
{
    srl_hl(cpu);
}
void cpu_op_0xCB3F(CPUState *cpu)
{
    srl_r8(&cpu->A, cpu);
}
void cpu_op_0xCB40(CPUState *cpu)
{
    bit_u3_r8(0, cpu->B, cpu);
}
void cpu_op_0xCB41(CPUState *cpu)
{
    bit_u3_r8(0, cpu->C, cpu);
}
void cpu_op_0xCB42(CPUState *cpu)
{
    bit_u3_r8(0, cpu->D, cpu);
}
void cpu_op_0xCB43(CPUState *cpu)
{
    bit_u3_r8(0, cpu->E, cpu);
}
void cpu_op_0xCB44(CPUState *cpu)
{
    bit_u3_r8(0, cpu->H, cpu);
}
void cpu_op_0xCB45(CPUState *cpu)
{
    bit_u3_r8(0, cpu->L, cpu);
}
void cpu_op_0xCB46(CPUState *cpu)
{
    bit_u3_hl(0, cpu);
}
void cpu_op_0xCB47(CPUState *cpu)
{
    bit_u3_r8(0, cpu->A, cpu);
}
void cpu_op_0xCB48(CPUState *cpu)
{
    bit_u3_r8(1, cpu->B, cpu);
}
void cpu_op_0xCB49(CPUState *cpu)
{
    bit_u3_r8(1, cpu->C, cpu);
}
void cpu_op_0xCB4A(CPUState *cpu)
{
    bit_u3_r8(1, cpu->D, cpu);
}
void cpu_op_0xCB4B(CPUState *cpu)
{
    bit_u3_r8(1, cpu->E, cpu);
}
void cpu_op_0xCB4C(CPUState *cpu)
{
    bit_u3_r8(1, cpu->H, cpu);
}
void cpu_op_0xCB4D(CPUState *cpu)
{
    bit_u3_r8(1, cpu->L, cpu);
}
void cpu_op_0xCB4E(CPUState *cpu)
{
    bit_u3_hl(1, cpu);
}
void cpu_op_0xCB4F(CPUState *cpu)
{
    bit_u3_r8(1, cpu->A, cpu);
}
void cpu_op_0xCB50(CPUState *cpu)
{
    bit_u3_r8(2, cpu->B, cpu);
}
void cpu_op_0xCB51(CPUState *cpu)
{
    bit_u3_r8(2, cpu->C, cpu);
}
void cpu_op_0xCB52(CPUState *cpu)
{
    bit_u3_r8(2, cpu->D, cpu);
}
void cpu_op_0xCB53(CPUState *cpu)
{
    bit_u3_r8(2, cpu->E, cpu);
}
void cpu_op_0xCB54(CPUState *cpu)
{
    bit_u3_r8(2, cpu->H, cpu);
}
void cpu_op_0xCB55(CPUState *cpu)
{
    bit_u3_r8(2, cpu->L, cpu);
}
void cpu_op_0xCB56(CPUState *cpu)
{
    bit_u3_hl(2, cpu);
}
void cpu_op_0xCB57(CPUState *cpu)
{
    bit_u3_r8(2, cpu->A, cpu);
}
void cpu_op_0xCB58(CPUState *cpu)
{
    bit_u3_r8(3, cpu->B, cpu);
}
void cpu_op_0xCB59(CPUState *cpu)
{
    bit_u3_r8(3, cpu->C, cpu);
}
void cpu_op_0xCB5A(CPUState *cpu)
{
    bit_u3_r8(3, cpu->D, cpu);
}
void cpu_op_0xCB5B(CPUState *cpu)
{
    bit_u3_r8(3, cpu->E, cpu);
}
void cpu_op_0xCB5C(CPUState *cpu)
{
    bit_u3_r8(3, cpu->H, cpu);
}
void cpu_op_0xCB5D(CPUState *cpu)
{
    bit_u3_r8(3, cpu->L, cpu);
}
void cpu_op_0xCB5E(CPUState *cpu)
{
    bit_u3_hl(3, cpu);
}
void cpu_op_0xCB5F(CPUState *cpu)
{
    bit_u3_r8(3, cpu->A, cpu);
}
void cpu_op_0xCB60(CPUState *cpu)
{
    bit_u3_r8(4, cpu->B, cpu);
}
void cpu_op_0xCB61(CPUState *cpu)
{
    bit_u3_r8(4, cpu->C, cpu);
}
void cpu_op_0xCB62(CPUState *cpu)
{
    bit_u3_r8(4, cpu->D, cpu);
}
void cpu_op_0xCB63(CPUState *cpu)
{
    bit_u3_r8(4, cpu->E, cpu);
}
void cpu_op_0xCB64(CPUState *cpu)
{
    bit_u3_r8(4, cpu->H, cpu);
}
void cpu_op_0xCB65(CPUState *cpu)
{
    bit_u3_r8(4, cpu->L, cpu);
}
void cpu_op_0xCB66(CPUState *cpu)
{
    bit_u3_hl(4, cpu);
}
void cpu_op_0xCB67(CPUState *cpu)
{
    bit_u3_r8(4, cpu->A, cpu);
}
void cpu_op_0xCB68(CPUState *cpu)
{
    bit_u3_r8(5, cpu->B, cpu);
}
void cpu_op_0xCB69(CPUState *cpu)
{
    bit_u3_r8(5, cpu->C, cpu);
}
void cpu_op_0xCB6A(CPUState *cpu)
{
    bit_u3_r8(5, cpu->D, cpu);
}
void cpu_op_0xCB6B(CPUState *cpu)
{
    bit_u3_r8(5, cpu->E, cpu);
}
void cpu_op_0xCB6C(CPUState *cpu)
{
    bit_u3_r8(5, cpu->H, cpu);
}
void cpu_op_0xCB6D(CPUState *cpu)
{
    bit_u3_r8(5, cpu->L, cpu);
}
void cpu_op_0xCB6E(CPUState *cpu)
{
    bit_u3_hl(5, cpu);
}
void cpu_op_0xCB6F(CPUState *cpu)
{
    bit_u3_r8(5, cpu->A, cpu);
}
void cpu_op_0xCB70(CPUState *cpu)
{
    bit_u3_r8(6, cpu->B, cpu);
}
void cpu_op_0xCB71(CPUState *cpu)
{
    bit_u3_r8(6, cpu->C, cpu);
}
void cpu_op_0xCB72(CPUState *cpu)
{
    bit_u3_r8(6, cpu->D, cpu);
}
void cpu_op_0xCB73(CPUState *cpu)
{
    bit_u3_r8(6, cpu->E, cpu);
}
void cpu_op_0xCB74(CPUState *cpu)
{
    bit_u3_r8(6, cpu->H, cpu);
}
void cpu_op_0xCB75(CPUState *cpu)
{
    bit_u3_r8(6, cpu->L, cpu);
}
void cpu_op_0xCB76(CPUState *cpu)
{
    bit_u3_hl(6, cpu);
}
void cpu_op_0xCB77(CPUState *cpu)
{
    bit_u3_r8(6, cpu->A, cpu);
}
void cpu_op_0xCB78(CPUState *cpu)
{
    bit_u3_r8(7, cpu->B, cpu);
}
void cpu_op_0xCB79(CPUState *cpu)
{
    bit_u3_r8(7, cpu->C, cpu);
}
void cpu_op_0xCB7A(CPUState *cpu)
{
    bit_u3_r8(7, cpu->D, cpu);
}
void cpu_op_0xCB7B(CPUState *cpu)
{
    bit_u3_r8(7, cpu->E, cpu);
}
void cpu_op_0xCB7C(CPUState *cpu)
{
    bit_u3_r8(7, cpu->H, cpu);
}
void cpu_op_0xCB7D(CPUState *cpu)
{
    bit_u3_r8(7, cpu->L, cpu);
}
void cpu_op_0xCB7E(CPUState *cpu)
{
    bit_u3_hl(7, cpu);
}
void cpu_op_0xCB7F(CPUState *cpu)
{
    bit_u3_r8(7, cpu->A, cpu);
}
void cpu_op_0xCB80(CPUState *cpu)
{
    res_u3_r8(0, &cpu->B, cpu);
}
void cpu_op_0xCB81(CPUState *cpu)
{
    res_u3_r8(0, &cpu->C, cpu);
}
void cpu_op_0xCB82(CPUState *cpu)
{
    res_u3_r8(0, &cpu->D, cpu);
}
void cpu_op_0xCB83(CPUState *cpu)
{
    res_u3_r8(0, &cpu->E, cpu);
}
void cpu_op_0xCB84(CPUState *cpu)
{
    res_u3_r8(0, &cpu->H, cpu);
}
void cpu_op_0xCB85(CPUState *cpu)
{
    res_u3_r8(0, &cpu->L, cpu);
}
void cpu_op_0xCB86(CPUState *cpu)
{
    res_u3_hl(0, cpu);
}
void cpu_op_0xCB87(CPUState *cpu)
{
    res_u3_r8(0, &cpu->A, cpu);
}
void cpu_op_0xCB88(CPUState *cpu)
{
    res_u3_r8(1, &cpu->B, cpu);
}
void cpu_op_0xCB89(CPUState *cpu)
{
    res_u3_r8(1, &cpu->C, cpu);
}
void cpu_op_0xCB8A(CPUState *cpu)
{
    res_u3_r8(1, &cpu->D, cpu);
}
void cpu_op_0xCB8B(CPUState *cpu)
{
    res_u3_r8(1, &cpu->E, cpu);
}
void cpu_op_0xCB8C(CPUState *cpu)
{
    res_u3_r8(1, &cpu->H, cpu);
}
void cpu_op_0xCB8D(CPUState *cpu)
{
    res_u3_r8(1, &cpu->L, cpu);
}
void cpu_op_0xCB8E(CPUState *cpu)
{
    res_u3_hl(1, cpu);
}
void cpu_op_0xCB8F(CPUState *cpu)
{
    res_u3_r8(1, &cpu->A, cpu);
}
void cpu_op_0xCB90(CPUState *cpu)
{
    res_u3_r8(2, &cpu->B, cpu);
}
void cpu_op_0xCB91(CPUState *cpu)
{
    res_u3_r8(2, &cpu->C, cpu);
}
void cpu_op_0xCB92(CPUState *cpu)
{
    res_u3_r8(2, &cpu->D, cpu);
}
void cpu_op_0xCB93(CPUState *cpu)
{
    res_u3_r8(2, &cpu->E, cpu);
}
void cpu_op_0xCB94(CPUState *cpu)
{
    res_u3_r8(2, &cpu->H, cpu);
}
void cpu_op_0xCB95(CPUState *cpu)
{
    res_u3_r8(2, &cpu->L, cpu);
}
void cpu_op_0xCB96(CPUState *cpu)
{
    res_u3_hl(2, cpu);
}
void cpu_op_0xCB97(CPUState *cpu)
{
    res_u3_r8(2, &cpu->A, cpu);
}
void cpu_op_0xCB98(CPUState *cpu)
{
    res_u3_r8(3, &cpu->B, cpu);
}
void cpu_op_0xCB99(CPUState *cpu)
{
    res_u3_r8(3, &cpu->C, cpu);
}
void cpu_op_0xCB9A(CPUState *cpu)
{
    res_u3_r8(3, &cpu->D, cpu);
}
void cpu_op_0xCB9B(CPUState *cpu)
{
    res_u3_r8(3, &cpu->E, cpu);
}
void cpu_op_0xCB9C(CPUState *cpu)
{
    res_u3_r8(3, &cpu->H, cpu);
}
void cpu_op_0xCB9D(CPUState *cpu)
{
    res_u3_r8(3, &cpu->L, cpu);
}
void cpu_op_0xCB9E(CPUState *cpu)
{
    res_u3_hl(3, cpu);
}
void cpu_op_0xCB9F(CPUState *cpu)
{
    res_u3_r8(3, &cpu->A, cpu);
}
void cpu_op_0xCBA0(CPUState *cpu)
{
    res_u3_r8(4, &cpu->B, cpu);
}
void cpu_op_0xCBA1(CPUState *cpu)
{
    res_u3_r8(4, &cpu->C, cpu);
}
void cpu_op_0xCBA2(CPUState *cpu)
{
    res_u3_r8(4, &cpu->D, cpu);
}
void cpu_op_0xCBA3(CPUState *cpu)
{
    res_u3_r8(4, &cpu->E, cpu);
}
void cpu_op_0xCBA4(CPUState *cpu)
{
    res_u3_r8(4, &cpu->H, cpu);
}
void cpu_op_0xCBA5(CPUState *cpu)
{
    res_u3_r8(4, &cpu->L, cpu);
}
void cpu_op_0xCBA6(CPUState *cpu)
{
    res_u3_hl(4, cpu);
}
void cpu_op_0xCBA7(CPUState *cpu)
{
    res_u3_r8(4, &cpu->A, cpu);
}
void cpu_op_0xCBA8(CPUState *cpu)
{
    res_u3_r8(5, &cpu->B, cpu);
}
void cpu_op_0xCBA9(CPUState *cpu)
{
    res_u3_r8(5, &cpu->C, cpu);
}
void cpu_op_0xCBAA(CPUState *cpu)
{
    res_u3_r8(5, &cpu->D, cpu);
}
void cpu_op_0xCBAB(CPUState *cpu)
{
    res_u3_r8(5, &cpu->E, cpu);
}
void cpu_op_0xCBAC(CPUState *cpu)
{
    res_u3_r8(5, &cpu->H, cpu);
}
void cpu_op_0xCBAD(CPUState *cpu)
{
    res_u3_r8(5, &cpu->L, cpu);
}
void cpu_op_0xCBAE(CPUState *cpu)
{
    res_u3_hl(5, cpu);
}
void cpu_op_0xCBAF(CPUState *cpu)
{
    res_u3_r8(5, &cpu->A, cpu);
}
void cpu_op_0xCBB0(CPUState *cpu)
{
    res_u3_r8(6, &cpu->B, cpu);
}
void cpu_op_0xCBB1(CPUState *cpu)
{
    res_u3_r8(6, &cpu->C, cpu);
}
void cpu_op_0xCBB2(CPUState *cpu)
{
    res_u3_r8(6, &cpu->D, cpu);
}
void cpu_op_0xCBB3(CPUState *cpu)
{
    res_u3_r8(6, &cpu->E, cpu);
}
void cpu_op_0xCBB4(CPUState *cpu)
{
    res_u3_r8(6, &cpu->H, cpu);
}
void cpu_op_0xCBB5(CPUState *cpu)
{
    res_u3_r8(6, &cpu->L, cpu);
}
void cpu_op_0xCBB6(CPUState *cpu)
{
    res_u3_hl(6, cpu);
}
void cpu_op_0xCBB7(CPUState *cpu)
{
    res_u3_r8(6, &cpu->A, cpu);
}
void cpu_op_0xCBB8(CPUState *cpu)
{
    res_u3_r8(7, &cpu->B, cpu);
}
void cpu_op_0xCBB9(CPUState *cpu)
{
    res_u3_r8(7, &cpu->C, cpu);
}
void cpu_op_0xCBBA(CPUState *cpu)
{
    res_u3_r8(7, &cpu->D, cpu);
}
void cpu_op_0xCBBB(CPUState *cpu)
{
    res_u3_r8(7, &cpu->E, cpu);
}
void cpu_op_0xCBBC(CPUState *cpu)
{
    res_u3_r8(7, &cpu->H, cpu);
}
void cpu_op_0xCBBD(CPUState *cpu)
{
    res_u3_r8(7, &cpu->L, cpu);
}
void cpu_op_0xCBBE(CPUState *cpu)
{
    res_u3_hl(7, cpu);
}
void cpu_op_0xCBBF(CPUState *cpu)
{
    res_u3_r8(7, &cpu->A, cpu);
}
void cpu_op_0xCBC0(CPUState *cpu)
{
    set_u3_r8(0, &cpu->B, cpu);
}
void cpu_op_0xCBC1(CPUState *cpu)
{
    set_u3_r8(0, &cpu->C, cpu);
}
void cpu_op_0xCBC2(CPUState *cpu)
{
    set_u3_r8(0, &cpu->D, cpu);
}
void cpu_op_0xCBC3(CPUState *cpu)
{
    set_u3_r8(0, &cpu->E, cpu);
}
void cpu_op_0xCBC4(CPUState *cpu)
{
    set_u3_r8(0, &cpu->H, cpu);
}
void cpu_op_0xCBC5(CPUState *cpu)
{
    set_u3_r8(0, &cpu->L, cpu);
}
void cpu_op_0xCBC6(CPUState *cpu)
{
    set_u3_hl(0, cpu);
}
void cpu_op_0xCBC7(CPUState *cpu)
{
    set_u3_r8(0, &cpu->A, cpu);
}
void cpu_op_0xCBC8(CPUState *cpu)
{
    set_u3_r8(1, &cpu->B, cpu);
}
void cpu_op_0xCBC9(CPUState *cpu)
{
    set_u3_r8(1, &cpu->C, cpu);
}
void cpu_op_0xCBCA(CPUState *cpu)
{
    set_u3_r8(1, &cpu->D, cpu);
}
void cpu_op_0xCBCB(CPUState *cpu)
{
    set_u3_r8(1, &cpu->E, cpu);
}
void cpu_op_0xCBCC(CPUState *cpu)
{
    set_u3_r8(1, &cpu->H, cpu);
}
void cpu_op_0xCBCD(CPUState *cpu)
{
    set_u3_r8(1, &cpu->L, cpu);
}
void cpu_op_0xCBCE(CPUState *cpu)
{
    set_u3_hl(1, cpu);
}
void cpu_op_0xCBCF(CPUState *cpu)
{
    set_u3_r8(1, &cpu->A, cpu);
}
void cpu_op_0xCBD0(CPUState *cpu)
{
    set_u3_r8(2, &cpu->B, cpu);
}
void cpu_op_0xCBD1(CPUState *cpu)
{
    set_u3_r8(2, &cpu->C, cpu);
}
void cpu_op_0xCBD2(CPUState *cpu)
{
    set_u3_r8(2, &cpu->D, cpu);
}
void cpu_op_0xCBD3(CPUState *cpu)
{
    set_u3_r8(2, &cpu->E, cpu);
}
void cpu_op_0xCBD4(CPUState *cpu)
{
    set_u3_r8(2, &cpu->H, cpu);
}
void cpu_op_0xCBD5(CPUState *cpu)
{
    set_u3_r8(2, &cpu->L, cpu);
}
void cpu_op_0xCBD6(CPUState *cpu)
{
    set_u3_hl(2, cpu);
}
void cpu_op_0xCBD7(CPUState *cpu)
{
    set_u3_r8(2, &cpu->A, cpu);
}
void cpu_op_0xCBD8(CPUState *cpu)
{
    set_u3_r8(3, &cpu->B, cpu);
}
void cpu_op_0xCBD9(CPUState *cpu)
{
    set_u3_r8(3, &cpu->C, cpu);
}
void cpu_op_0xCBDA(CPUState *cpu)
{
    set_u3_r8(3, &cpu->D, cpu);
}
void cpu_op_0xCBDB(CPUState *cpu)
{
    set_u3_r8(3, &cpu->E, cpu);
}
void cpu_op_0xCBDC(CPUState *cpu)
{
    set_u3_r8(3, &cpu->H, cpu);
}
void cpu_op_0xCBDD(CPUState *cpu)
{
    set_u3_r8(3, &cpu->L, cpu);
}
void cpu_op_0xCBDE(CPUState *cpu)
{
    set_u3_hl(3, cpu);
}
void cpu_op_0xCBDF(CPUState *cpu)
{
    set_u3_r8(3, &cpu->A, cpu);
}
void cpu_op_0xCBE0(CPUState *cpu)
{
    set_u3_r8(4, &cpu->B, cpu);
}
void cpu_op_0xCBE1(CPUState *cpu)
{
    set_u3_r8(4, &cpu->C, cpu);
}
void cpu_op_0xCBE2(CPUState *cpu)
{
    set_u3_r8(4, &cpu->D, cpu);
}
void cpu_op_0xCBE3(CPUState *cpu)
{
    set_u3_r8(4, &cpu->E, cpu);
}
void cpu_op_0xCBE4(CPUState *cpu)
{
    set_u3_r8(4, &cpu->H, cpu);
}
void cpu_op_0xCBE5(CPUState *cpu)
{
    set_u3_r8(4, &cpu->L, cpu);
}
void cpu_op_0xCBE6(CPUState *cpu)
{
    set_u3_hl(4, cpu);
}
void cpu_op_0xCBE7(CPUState *cpu)
{
    set_u3_r8(4, &cpu->A, cpu);
}
void cpu_op_0xCBE8(CPUState *cpu)
{
    set_u3_r8(5, &cpu->B, cpu);
}
void cpu_op_0xCBE9(CPUState *cpu)
{
    set_u3_r8(5, &cpu->C, cpu);
}
void cpu_op_0xCBEA(CPUState *cpu)
{
    set_u3_r8(5, &cpu->D, cpu);
}
void cpu_op_0xCBEB(CPUState *cpu)
{
    set_u3_r8(5, &cpu->E, cpu);
}
void cpu_op_0xCBEC(CPUState *cpu)
{
    set_u3_r8(5, &cpu->H, cpu);
}
void cpu_op_0xCBED(CPUState *cpu)
{
    set_u3_r8(5, &cpu->L, cpu);
}
void cpu_op_0xCBEE(CPUState *cpu)
{
    set_u3_hl(5, cpu);
}
void cpu_op_0xCBEF(CPUState *cpu)
{
    set_u3_r8(5, &cpu->A, cpu);
}
void cpu_op_0xCBF0(CPUState *cpu)
{
    set_u3_r8(6, &cpu->B, cpu);
}
void cpu_op_0xCBF1(CPUState *cpu)
{
    set_u3_r8(6, &cpu->C, cpu);
}
void cpu_op_0xCBF2(CPUState *cpu)
{
    set_u3_r8(6, &cpu->D, cpu);
}
void cpu_op_0xCBF3(CPUState *cpu)
{
    set_u3_r8(6, &cpu->E, cpu);
}
void cpu_op_0xCBF4(CPUState *cpu)
{
    set_u3_r8(6, &cpu->H, cpu);
}
void cpu_op_0xCBF5(CPUState *cpu)
{
    set_u3_r8(6, &cpu->L, cpu);
}
void cpu_op_0xCBF6(CPUState *cpu)
{
    set_u3_hl(6, cpu);
}
void cpu_op_0xCBF7(CPUState *cpu)
{
    set_u3_r8(6, &cpu->A, cpu);
}
void cpu_op_0xCBF8(CPUState *cpu)
{
    set_u3_r8(7, &cpu->B, cpu);
}
void cpu_op_0xCBF9(CPUState *cpu)
{
    set_u3_r8(7, &cpu->C, cpu);
}
void cpu_op_0xCBFA(CPUState *cpu)
{
    set_u3_r8(7, &cpu->D, cpu);
}
void cpu_op_0xCBFB(CPUState *cpu)
{
    set_u3_r8(7, &cpu->E, cpu);
}
void cpu_op_0xCBFC(CPUState *cpu)
{
    set_u3_r8(7, &cpu->H, cpu);
}
void cpu_op_0xCBFD(CPUState *cpu)
{
    set_u3_r8(7, &cpu->L, cpu);
}
void cpu_op_0xCBFE(CPUState *cpu)
{
    set_u3_hl(7, cpu);
}
void cpu_op_0xCBFF(CPUState *cpu)
{
    set_u3_r8(7, &cpu->A, cpu);
}

const void (*opcodes[])(CPUState *) = {
    cpu_op_0x00,
    cpu_op_0x01,
    cpu_op_0x02,
    cpu_op_0x03,
    cpu_op_0x04,
    cpu_op_0x05,
    cpu_op_0x06,
    cpu_op_0x07,
    cpu_op_0x08,
    cpu_op_0x09,
    cpu_op_0x0A,
    cpu_op_0x0B,
    cpu_op_0x0C,
    cpu_op_0x0D,
    cpu_op_0x0E,
    cpu_op_0x0F,
    cpu_op_0x10,
    cpu_op_0x11,
    cpu_op_0x12,
    cpu_op_0x13,
    cpu_op_0x14,
    cpu_op_0x15,
    cpu_op_0x16,
    cpu_op_0x17,
    cpu_op_0x18,
    cpu_op_0x19,
    cpu_op_0x1A,
    cpu_op_0x1B,
    cpu_op_0x1C,
    cpu_op_0x1D,
    cpu_op_0x1E,
    cpu_op_0x1F,
    cpu_op_0x20,
    cpu_op_0x21,
    cpu_op_0x22,
    cpu_op_0x23,
    cpu_op_0x24,
    cpu_op_0x25,
    cpu_op_0x26,
    cpu_op_0x27,
    cpu_op_0x28,
    cpu_op_0x29,
    cpu_op_0x2A,
    cpu_op_0x2B,
    cpu_op_0x2C,
    cpu_op_0x2D,
    cpu_op_0x2E,
    cpu_op_0x2F,
    cpu_op_0x30,
    cpu_op_0x31,
    cpu_op_0x32,
    cpu_op_0x33,
    cpu_op_0x34,
    cpu_op_0x35,
    cpu_op_0x36,
    cpu_op_0x37,
    cpu_op_0x38,
    cpu_op_0x39,
    cpu_op_0x3A,
    cpu_op_0x3B,
    cpu_op_0x3C,
    cpu_op_0x3D,
    cpu_op_0x3E,
    cpu_op_0x3F,
    cpu_op_0x40,
    cpu_op_0x41,
    cpu_op_0x42,
    cpu_op_0x43,
    cpu_op_0x44,
    cpu_op_0x45,
    cpu_op_0x46,
    cpu_op_0x47,
    cpu_op_0x48,
    cpu_op_0x49,
    cpu_op_0x4A,
    cpu_op_0x4B,
    cpu_op_0x4C,
    cpu_op_0x4D,
    cpu_op_0x4E,
    cpu_op_0x4F,
    cpu_op_0x50,
    cpu_op_0x51,
    cpu_op_0x52,
    cpu_op_0x53,
    cpu_op_0x54,
    cpu_op_0x55,
    cpu_op_0x56,
    cpu_op_0x57,
    cpu_op_0x58,
    cpu_op_0x59,
    cpu_op_0x5A,
    cpu_op_0x5B,
    cpu_op_0x5C,
    cpu_op_0x5D,
    cpu_op_0x5E,
    cpu_op_0x5F,
    cpu_op_0x60,
    cpu_op_0x61,
    cpu_op_0x62,
    cpu_op_0x63,
    cpu_op_0x64,
    cpu_op_0x65,
    cpu_op_0x66,
    cpu_op_0x67,
    cpu_op_0x68,
    cpu_op_0x69,
    cpu_op_0x6A,
    cpu_op_0x6B,
    cpu_op_0x6C,
    cpu_op_0x6D,
    cpu_op_0x6E,
    cpu_op_0x6F,
    cpu_op_0x70,
    cpu_op_0x71,
    cpu_op_0x72,
    cpu_op_0x73,
    cpu_op_0x74,
    cpu_op_0x75,
    cpu_op_0x76,
    cpu_op_0x77,
    cpu_op_0x78,
    cpu_op_0x79,
    cpu_op_0x7A,
    cpu_op_0x7B,
    cpu_op_0x7C,
    cpu_op_0x7D,
    cpu_op_0x7E,
    cpu_op_0x7F,
    cpu_op_0x80,
    cpu_op_0x81,
    cpu_op_0x82,
    cpu_op_0x83,
    cpu_op_0x84,
    cpu_op_0x85,
    cpu_op_0x86,
    cpu_op_0x87,
    cpu_op_0x88,
    cpu_op_0x89,
    cpu_op_0x8A,
    cpu_op_0x8B,
    cpu_op_0x8C,
    cpu_op_0x8D,
    cpu_op_0x8E,
    cpu_op_0x8F,
    cpu_op_0x90,
    cpu_op_0x91,
    cpu_op_0x92,
    cpu_op_0x93,
    cpu_op_0x94,
    cpu_op_0x95,
    cpu_op_0x96,
    cpu_op_0x97,
    cpu_op_0x98,
    cpu_op_0x99,
    cpu_op_0x9A,
    cpu_op_0x9B,
    cpu_op_0x9C,
    cpu_op_0x9D,
    cpu_op_0x9E,
    cpu_op_0x9F,
    cpu_op_0xA0,
    cpu_op_0xA1,
    cpu_op_0xA2,
    cpu_op_0xA3,
    cpu_op_0xA4,
    cpu_op_0xA5,
    cpu_op_0xA6,
    cpu_op_0xA7,
    cpu_op_0xA8,
    cpu_op_0xA9,
    cpu_op_0xAA,
    cpu_op_0xAB,
    cpu_op_0xAC,
    cpu_op_0xAD,
    cpu_op_0xAE,
    cpu_op_0xAF,
    cpu_op_0xB0,
    cpu_op_0xB1,
    cpu_op_0xB2,
    cpu_op_0xB3,
    cpu_op_0xB4,
    cpu_op_0xB5,
    cpu_op_0xB6,
    cpu_op_0xB7,
    cpu_op_0xB8,
    cpu_op_0xB9,
    cpu_op_0xBA,
    cpu_op_0xBB,
    cpu_op_0xBC,
    cpu_op_0xBD,
    cpu_op_0xBE,
    cpu_op_0xBF,
    cpu_op_0xC0,
    cpu_op_0xC1,
    cpu_op_0xC2,
    cpu_op_0xC3,
    cpu_op_0xC4,
    cpu_op_0xC5,
    cpu_op_0xC6,
    cpu_op_0xC7,
    cpu_op_0xC8,
    cpu_op_0xC9,
    cpu_op_0xCA,
    cpu_op_0xCB,
    cpu_op_0xCC,
    cpu_op_0xCD,
    cpu_op_0xCE,
    cpu_op_0xCF,
    cpu_op_0xD0,
    cpu_op_0xD1,
    cpu_op_0xD2,
    cpu_op_0xD3,
    cpu_op_0xD4,
    cpu_op_0xD5,
    cpu_op_0xD6,
    cpu_op_0xD7,
    cpu_op_0xD8,
    cpu_op_0xD9,
    cpu_op_0xDA,
    cpu_op_0xDB,
    cpu_op_0xDC,
    cpu_op_0xDD,
    cpu_op_0xDE,
    cpu_op_0xDF,
    cpu_op_0xE0,
    cpu_op_0xE1,
    cpu_op_0xE2,
    cpu_op_0xE3,
    cpu_op_0xE4,
    cpu_op_0xE5,
    cpu_op_0xE6,
    cpu_op_0xE7,
    cpu_op_0xE8,
    cpu_op_0xE9,
    cpu_op_0xEA,
    cpu_op_0xEB,
    cpu_op_0xEC,
    cpu_op_0xED,
    cpu_op_0xEE,
    cpu_op_0xEF,
    cpu_op_0xF0,
    cpu_op_0xF1,
    cpu_op_0xF2,
    cpu_op_0xF3,
    cpu_op_0xF4,
    cpu_op_0xF5,
    cpu_op_0xF6,
    cpu_op_0xF7,
    cpu_op_0xF8,
    cpu_op_0xF9,
    cpu_op_0xFA,
    cpu_op_0xFB,
    cpu_op_0xFC,
    cpu_op_0xFD,
    cpu_op_0xFE,
    cpu_op_0xFF,
};

const void (*extended_opcodes[])(CPUState *) = {
    cpu_op_0xCB00,
    cpu_op_0xCB01,
    cpu_op_0xCB02,
    cpu_op_0xCB03,
    cpu_op_0xCB04,
    cpu_op_0xCB05,
    cpu_op_0xCB06,
    cpu_op_0xCB07,
    cpu_op_0xCB08,
    cpu_op_0xCB09,
    cpu_op_0xCB0A,
    cpu_op_0xCB0B,
    cpu_op_0xCB0C,
    cpu_op_0xCB0D,
    cpu_op_0xCB0E,
    cpu_op_0xCB0F,
    cpu_op_0xCB10,
    cpu_op_0xCB11,
    cpu_op_0xCB12,
    cpu_op_0xCB13,
    cpu_op_0xCB14,
    cpu_op_0xCB15,
    cpu_op_0xCB16,
    cpu_op_0xCB17,
    cpu_op_0xCB18,
    cpu_op_0xCB19,
    cpu_op_0xCB1A,
    cpu_op_0xCB1B,
    cpu_op_0xCB1C,
    cpu_op_0xCB1D,
    cpu_op_0xCB1E,
    cpu_op_0xCB1F,
    cpu_op_0xCB20,
    cpu_op_0xCB21,
    cpu_op_0xCB22,
    cpu_op_0xCB23,
    cpu_op_0xCB24,
    cpu_op_0xCB25,
    cpu_op_0xCB26,
    cpu_op_0xCB27,
    cpu_op_0xCB28,
    cpu_op_0xCB29,
    cpu_op_0xCB2A,
    cpu_op_0xCB2B,
    cpu_op_0xCB2C,
    cpu_op_0xCB2D,
    cpu_op_0xCB2E,
    cpu_op_0xCB2F,
    cpu_op_0xCB30,
    cpu_op_0xCB31,
    cpu_op_0xCB32,
    cpu_op_0xCB33,
    cpu_op_0xCB34,
    cpu_op_0xCB35,
    cpu_op_0xCB36,
    cpu_op_0xCB37,
    cpu_op_0xCB38,
    cpu_op_0xCB39,
    cpu_op_0xCB3A,
    cpu_op_0xCB3B,
    cpu_op_0xCB3C,
    cpu_op_0xCB3D,
    cpu_op_0xCB3E,
    cpu_op_0xCB3F,
    cpu_op_0xCB40,
    cpu_op_0xCB41,
    cpu_op_0xCB42,
    cpu_op_0xCB43,
    cpu_op_0xCB44,
    cpu_op_0xCB45,
    cpu_op_0xCB46,
    cpu_op_0xCB47,
    cpu_op_0xCB48,
    cpu_op_0xCB49,
    cpu_op_0xCB4A,
    cpu_op_0xCB4B,
    cpu_op_0xCB4C,
    cpu_op_0xCB4D,
    cpu_op_0xCB4E,
    cpu_op_0xCB4F,
    cpu_op_0xCB50,
    cpu_op_0xCB51,
    cpu_op_0xCB52,
    cpu_op_0xCB53,
    cpu_op_0xCB54,
    cpu_op_0xCB55,
    cpu_op_0xCB56,
    cpu_op_0xCB57,
    cpu_op_0xCB58,
    cpu_op_0xCB59,
    cpu_op_0xCB5A,
    cpu_op_0xCB5B,
    cpu_op_0xCB5C,
    cpu_op_0xCB5D,
    cpu_op_0xCB5E,
    cpu_op_0xCB5F,
    cpu_op_0xCB60,
    cpu_op_0xCB61,
    cpu_op_0xCB62,
    cpu_op_0xCB63,
    cpu_op_0xCB64,
    cpu_op_0xCB65,
    cpu_op_0xCB66,
    cpu_op_0xCB67,
    cpu_op_0xCB68,
    cpu_op_0xCB69,
    cpu_op_0xCB6A,
    cpu_op_0xCB6B,
    cpu_op_0xCB6C,
    cpu_op_0xCB6D,
    cpu_op_0xCB6E,
    cpu_op_0xCB6F,
    cpu_op_0xCB70,
    cpu_op_0xCB71,
    cpu_op_0xCB72,
    cpu_op_0xCB73,
    cpu_op_0xCB74,
    cpu_op_0xCB75,
    cpu_op_0xCB76,
    cpu_op_0xCB77,
    cpu_op_0xCB78,
    cpu_op_0xCB79,
    cpu_op_0xCB7A,
    cpu_op_0xCB7B,
    cpu_op_0xCB7C,
    cpu_op_0xCB7D,
    cpu_op_0xCB7E,
    cpu_op_0xCB7F,
    cpu_op_0xCB80,
    cpu_op_0xCB81,
    cpu_op_0xCB82,
    cpu_op_0xCB83,
    cpu_op_0xCB84,
    cpu_op_0xCB85,
    cpu_op_0xCB86,
    cpu_op_0xCB87,
    cpu_op_0xCB88,
    cpu_op_0xCB89,
    cpu_op_0xCB8A,
    cpu_op_0xCB8B,
    cpu_op_0xCB8C,
    cpu_op_0xCB8D,
    cpu_op_0xCB8E,
    cpu_op_0xCB8F,
    cpu_op_0xCB90,
    cpu_op_0xCB91,
    cpu_op_0xCB92,
    cpu_op_0xCB93,
    cpu_op_0xCB94,
    cpu_op_0xCB95,
    cpu_op_0xCB96,
    cpu_op_0xCB97,
    cpu_op_0xCB98,
    cpu_op_0xCB99,
    cpu_op_0xCB9A,
    cpu_op_0xCB9B,
    cpu_op_0xCB9C,
    cpu_op_0xCB9D,
    cpu_op_0xCB9E,
    cpu_op_0xCB9F,
    cpu_op_0xCBA0,
    cpu_op_0xCBA1,
    cpu_op_0xCBA2,
    cpu_op_0xCBA3,
    cpu_op_0xCBA4,
    cpu_op_0xCBA5,
    cpu_op_0xCBA6,
    cpu_op_0xCBA7,
    cpu_op_0xCBA8,
    cpu_op_0xCBA9,
    cpu_op_0xCBAA,
    cpu_op_0xCBAB,
    cpu_op_0xCBAC,
    cpu_op_0xCBAD,
    cpu_op_0xCBAE,
    cpu_op_0xCBAF,
    cpu_op_0xCBB0,
    cpu_op_0xCBB1,
    cpu_op_0xCBB2,
    cpu_op_0xCBB3,
    cpu_op_0xCBB4,
    cpu_op_0xCBB5,
    cpu_op_0xCBB6,
    cpu_op_0xCBB7,
    cpu_op_0xCBB8,
    cpu_op_0xCBB9,
    cpu_op_0xCBBA,
    cpu_op_0xCBBB,
    cpu_op_0xCBBC,
    cpu_op_0xCBBD,
    cpu_op_0xCBBE,
    cpu_op_0xCBBF,
    cpu_op_0xCBC0,
    cpu_op_0xCBC1,
    cpu_op_0xCBC2,
    cpu_op_0xCBC3,
    cpu_op_0xCBC4,
    cpu_op_0xCBC5,
    cpu_op_0xCBC6,
    cpu_op_0xCBC7,
    cpu_op_0xCBC8,
    cpu_op_0xCBC9,
    cpu_op_0xCBCA,
    cpu_op_0xCBCB,
    cpu_op_0xCBCC,
    cpu_op_0xCBCD,
    cpu_op_0xCBCE,
    cpu_op_0xCBCF,
    cpu_op_0xCBD0,
    cpu_op_0xCBD1,
    cpu_op_0xCBD2,
    cpu_op_0xCBD3,
    cpu_op_0xCBD4,
    cpu_op_0xCBD5,
    cpu_op_0xCBD6,
    cpu_op_0xCBD7,
    cpu_op_0xCBD8,
    cpu_op_0xCBD9,
    cpu_op_0xCBDA,
    cpu_op_0xCBDB,
    cpu_op_0xCBDC,
    cpu_op_0xCBDD,
    cpu_op_0xCBDE,
    cpu_op_0xCBDF,
    cpu_op_0xCBE0,
    cpu_op_0xCBE1,
    cpu_op_0xCBE2,
    cpu_op_0xCBE3,
    cpu_op_0xCBE4,
    cpu_op_0xCBE5,
    cpu_op_0xCBE6,
    cpu_op_0xCBE7,
    cpu_op_0xCBE8,
    cpu_op_0xCBE9,
    cpu_op_0xCBEA,
    cpu_op_0xCBEB,
    cpu_op_0xCBEC,
    cpu_op_0xCBED,
    cpu_op_0xCBEE,
    cpu_op_0xCBEF,
    cpu_op_0xCBF0,
    cpu_op_0xCBF1,
    cpu_op_0xCBF2,
    cpu_op_0xCBF3,
    cpu_op_0xCBF4,
    cpu_op_0xCBF5,
    cpu_op_0xCBF6,
    cpu_op_0xCBF7,
    cpu_op_0xCBF8,
    cpu_op_0xCBF9,
    cpu_op_0xCBFA,
    cpu_op_0xCBFB,
    cpu_op_0xCBFC,
    cpu_op_0xCBFD,
    cpu_op_0xCBFE,
    cpu_op_0xCBFF,
};

#include "CPU.h"

static const int timer_bit_table[4] = {9, 3, 5, 7};

static inline bool cpu_timer_enabled(const CPUState *cpu)
{
    return (cpu->memory->memory.TAC & 0x04) != 0;
}

static inline int cpu_timer_selected_bit(const CPUState *cpu)
{
    return timer_bit_table[cpu->memory->memory.TAC & 0x03];
}

static inline bool cpu_timer_signal(const CPUState *cpu)
{
    return ((cpu->div_counter >> cpu_timer_selected_bit(cpu)) & 0x01u) != 0;
}

static inline uint8_t cpu_pending_interrupts(const CPUState *cpu)
{
    return (uint8_t)(cpu->memory->memory.IF & cpu->memory->memory.IE & 0x1F);
}

static void cpu_timer_increment(CPUState *cpu)
{
    if (cpu->memory->memory.TIMA == 0xFF)
    {
        cpu->memory->memory.TIMA = 0x00;
        cpu->timer_reload_active = true;
        cpu->timer_reload_delay = 4;
    }
    else
    {
        cpu->memory->memory.TIMA++;
    }
}

static void cpu_process_timer_reload(CPUState *cpu)
{
    if (cpu->timer_reload_active && cpu->timer_reload_delay > 0)
    {
        cpu->timer_reload_delay--;
        if (cpu->timer_reload_delay == 0)
        {
            cpu->timer_reload_active = false;
            cpu->memory->memory.TIMA = cpu->memory->memory.TMA;
            cpu->memory->memory.IF |= IF_TIMER;
        }
    }
}

static void cpu_update_timers(CPUState *cpu, uint32_t cycles)
{
    if (cycles == 0)
        return;

    for (uint32_t i = 0; i < cycles; ++i)
    {
        cpu_process_timer_reload(cpu);

        bool old_signal = cpu->timer_prev_signal;
        cpu->div_counter = (uint16_t)(cpu->div_counter + 4);
        bool new_signal = cpu_timer_signal(cpu);
        cpu->timer_prev_signal = new_signal;

        if (cpu_timer_enabled(cpu) && old_signal && !new_signal)
        {
            cpu_timer_increment(cpu);
        }
    }

    cpu->memory->memory.DIV = (uint8_t)(cpu->div_counter >> 8);
}

static uint32_t cpu_idle_cycles(CPUState *cpu, uint32_t cycles)
{
    cpu_update_timers(cpu, cycles);
    cpu->cycle_count += cycles;
    return cycles;
}

static uint32_t cpu_service_interrupts(CPUState *cpu)
{
    if (!cpu->IME)
        return 0;

    uint8_t pending = cpu_pending_interrupts(cpu);
    if (pending == 0)
        return 0;

    static const uint16_t vectors[5] = {0x40, 0x48, 0x50, 0x58, 0x60};

    cpu->halt = false;
    cpu->stop = false;
    cpu->halt_bug = false;
    cpu->IME = false;

    for (int i = 0; i < 5; ++i)
    {
        uint8_t mask = (uint8_t)(1u << i);
        if (pending & mask)
        {
            cpu->memory->memory.IF &= (uint8_t)~mask;
            cpu->memory->memory.IF &= 0x1F;

            cpu->cycle_count += 5;

            memory_write(cpu->memory, --cpu->SP, (uint8_t)((cpu->PC >> 8) & 0xFF));
            memory_write(cpu->memory, --cpu->SP, (uint8_t)(cpu->PC & 0xFF));
            cpu->PC = vectors[i];
            return 5;
        }
    }

    return 0;
}

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
        cpu->PC = 0x0100;
    }

    cpu->memory->cpu = cpu;

    cpu->IME = false;
    cpu->EI_pending = false;
    cpu->IME_enable_pending = false;
    cpu->halt = false;
    cpu->stop = false;
    cpu->halt_bug = false;
    cpu->div_counter = 0;
    cpu->timer_reload_active = false;
    cpu->timer_reload_delay = 0;
    cpu->timer_prev_signal = cpu_timer_signal(cpu);

    cpu->cycle_count = 2;
    cpu->memory->memory.DIV = 0;
    cpu->memory->memory.IF = 0;
    cpu->memory->memory.TIMA = 0;
}

uint64_t instruction_count = 0;

uint16_t breakpoints[] = {};

uint32_t cpu_execute_instruction(CPUState *cpu)
{
    if (cpu->EI_pending)
    {
        cpu->IME_enable_pending = true;
        cpu->EI_pending = false;
    }

    if (cpu->stop && cpu_pending_interrupts(cpu))
    {
        cpu->stop = false;
    }

    if (cpu->halt && cpu_pending_interrupts(cpu))
    {
        cpu->halt = false;
        if (!cpu->IME)
        {
            cpu->halt_bug = false;
        }
    }

    uint32_t total_cycles = 0;
    bool executed_instruction = false;

    uint32_t interrupt_cycles = cpu_service_interrupts(cpu);
    if (interrupt_cycles)
    {
        cpu_update_timers(cpu, interrupt_cycles);
        return interrupt_cycles;
    }

    if (cpu->stop)
    {
        return cpu_idle_cycles(cpu, 1);
    }

    if (cpu->halt)
    {
        return cpu_idle_cycles(cpu, 1);
    }

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

    if (instruction_count % 20000 == 0)
    {
        printf("PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X IME=%d IE=%02X IF=%02X LY=%02X OP=%02X\n",
               cpu->PC, cpu->SP, cpu->AF, cpu->BC, cpu->DE, cpu->HL, cpu->IME,
               cpu->memory->memory.IE, cpu->memory->memory.IF, cpu->memory->memory.LY,
               memory_read(cpu->memory, cpu->PC));
    }

    uint64_t prev_cycles = cpu->cycle_count;

    uint16_t fetch_pc = cpu->PC;
    if (!cpu->halt_bug)
    {
        cpu->PC++;
    }
    else
    {
        cpu->halt_bug = false;
    }

    uint8_t opcode = memory_read(cpu->memory, fetch_pc);
    opcodes[opcode](cpu);
    executed_instruction = true;

    instruction_count++;

    uint32_t cycles = (uint32_t)(cpu->cycle_count - prev_cycles);
    cpu_update_timers(cpu, cycles);
    total_cycles += cycles;

    if (executed_instruction && cpu->IME_enable_pending)
    {
        cpu->IME = true;
        cpu->IME_enable_pending = false;
    }

    return total_cycles;
}

void cpu_timer_div_write(CPUState *cpu)
{
    bool old_signal = cpu_timer_signal(cpu);
    cpu->div_counter = 0;
    cpu->memory->memory.DIV = 0;
    bool new_signal = cpu_timer_signal(cpu);
    cpu->timer_prev_signal = new_signal;

    if (cpu_timer_enabled(cpu) && old_signal && !new_signal)
    {
        cpu_timer_increment(cpu);
    }
}

void cpu_timer_tima_write(CPUState *cpu, uint8_t value)
{
    cpu->memory->memory.TIMA = value;
    cpu->timer_reload_active = false;
    cpu->timer_reload_delay = 0;
}

void cpu_timer_tma_write(CPUState *cpu, uint8_t value)
{
    cpu->memory->memory.TMA = value;
}

void cpu_timer_tac_write(CPUState *cpu, uint8_t value)
{
    bool old_signal = cpu_timer_signal(cpu);
    bool old_enabled = cpu_timer_enabled(cpu);

    uint8_t tac = (uint8_t)(0xF8 | (value & 0x07));
    cpu->memory->memory.TAC = tac;

    bool new_signal = cpu_timer_signal(cpu);
    cpu->timer_prev_signal = new_signal;
    bool new_enabled = cpu_timer_enabled(cpu);

    if (old_enabled && new_enabled && old_signal && !new_signal)
    {
        cpu_timer_increment(cpu);
    }
}

void cpu_dma_transfer(CPUState *cpu, uint16_t source)
{
    (void)source;
    const uint32_t dma_cycles = 160;
    cpu_update_timers(cpu, dma_cycles);
    cpu->cycle_count += dma_cycles;
}

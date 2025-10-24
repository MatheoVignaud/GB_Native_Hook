#include "Memory.h"
#include "CPU.h"

#include <string.h>

static uint8_t joypad_read(const MemoryState *mem)
{
    uint8_t select = mem->joypad_select & 0x30;
    uint8_t value = (uint8_t)(0xC0 | select);
    uint8_t low = 0x0F;

    if ((select & 0x10) == 0)
    {
        low &= mem->joypad_buttons;
    }
    if ((select & 0x20) == 0)
    {
        low &= mem->joypad_dpad;
    }

    value |= low;
    return value;
}

static uint8_t memory_raw_read(MemoryState *mem, uint16_t address)
{
    if (!mem->memory.BOOT && mem->bios_enabled && address < 0x0100)
    {
        return mem->bios[address];
    }

    if (address >= 0xE000 && address <= 0xFDFF)
    {
        address = (uint16_t)(address - 0x2000);
    }
    if (address >= 0xFEA0 && address <= 0xFEFF)
    {
        return 0x00;
    }
    return mem->memory.data[address];
}

static void memory_raw_write(MemoryState *mem, uint16_t address, uint8_t value)
{
    if (address >= 0xE000 && address <= 0xFDFF)
    {
        uint16_t mirror = (uint16_t)(address - 0x2000);
        mem->memory.data[address] = value;
        mem->memory.data[mirror] = value;
        return;
    }

    if (address >= 0xFEA0 && address <= 0xFEFF)
    {
        return;
    }

    mem->memory.data[address] = value;
}

void memory_init(MemoryState *mem)
{
    memset(mem, 0, sizeof(*mem));
    mem->bios_enabled = false;
    mem->joypad_buttons = 0x0F;
    mem->joypad_dpad = 0x0F;
    mem->joypad_select = 0x30;
    mem->memory.P1_JOYP = (uint8_t)(0xC0 | mem->joypad_select | 0x0F);
}

int load_rom(const char *path, uint8_t *mem)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size > 0x8000)
        size = 0x8000;
    if (fread(mem + 0x0000, 1, size, f) != size)
    {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

int load_bios(const char *path, uint8_t *bios)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size != 0x100)
    {
        fclose(f);
        return -1;
    }
    if (fread(bios, 1, size, f) != size)
    {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

uint8_t memory_read(MemoryState *mem, uint16_t address)
{

    if (address == 0xFF00)
    {
        uint8_t value = joypad_read(mem);
        mem->memory.P1_JOYP = value;
        return value;
    }

    return memory_raw_read(mem, address);
}

void memory_write(MemoryState *mem, uint16_t address, uint8_t value)
{
    if (mem->bios_enabled && address == 0xFF50 && value == 1)
    {
        printf("Disabling BIOS mapping at address  \n");
        // getchar();
        mem->bios_enabled = false;
    }

    switch (address)
    {
    case 0xFF00:
    {
        mem->joypad_select = value & 0x30;
        uint8_t joy = joypad_read(mem);
        mem->memory.P1_JOYP = joy;
        return;
    }
    case 0xFF04: // DIV
        if (mem->cpu)
        {
            cpu_timer_div_write(mem->cpu);
        }
        else
        {
            mem->memory.DIV = 0;
        }
        return;
    case 0xFF05: // TIMA
        if (mem->cpu)
        {
            cpu_timer_tima_write(mem->cpu, value);
        }
        else
        {
            mem->memory.TIMA = value;
        }
        return;
    case 0xFF06: // TMA
        if (mem->cpu)
        {
            cpu_timer_tma_write(mem->cpu, value);
        }
        else
        {
            mem->memory.TMA = value;
        }
        return;
    case 0xFF07: // TAC
        if (mem->cpu)
        {
            cpu_timer_tac_write(mem->cpu, value);
        }
        else
        {
            mem->memory.TAC = (uint8_t)(value | 0xF8);
        }
        return;
    case 0xFF0F:
        mem->memory.IF = value & 0x1F;
        return;
    case 0xFF46: // DMA
    {
        mem->memory.DMA = value;
        uint16_t source = (uint16_t)(value << 8);
        for (int i = 0; i < 0xA0; ++i)
        {
            uint8_t data = memory_raw_read(mem, (uint16_t)(source + i));
            mem->memory.oam[i] = data;
        }
        if (mem->cpu)
        {
            cpu_dma_transfer(mem->cpu, source);
        }
        return;
    }
    case 0xFF41:
        uint8_t keep_ro = mem->memory.STAT & 0x07;
        uint8_t new_rw = (value & 0x78);
        mem->memory.STAT = keep_ro | new_rw;
        return;
    case 0xFF44:
        mem->memory.LY = 0;
        return;
    case 0xFFFF:
        mem->memory.IE = value & 0x1F;
        return;

    default:
        break;
    }

    memory_raw_write(mem, address, value);
}

void memory_set_button_state(MemoryState *mem, JoypadInput input, bool pressed)
{
    uint8_t bit = (uint8_t)(1u << (input & 0x03));
    bool trigger_interrupt = false;

    if (input <= JOYPAD_DOWN)
    {
        bool was_pressed = (mem->joypad_dpad & bit) == 0;
        if (pressed)
        {
            mem->joypad_dpad &= (uint8_t)~bit;
        }
        else
        {
            mem->joypad_dpad |= bit;
        }
        bool now_pressed = (mem->joypad_dpad & bit) == 0;
        if (!was_pressed && now_pressed)
        {
            trigger_interrupt = true;
        }
    }
    else
    {
        bool was_pressed = (mem->joypad_buttons & bit) == 0;
        if (pressed)
        {
            mem->joypad_buttons &= (uint8_t)~bit;
        }
        else
        {
            mem->joypad_buttons |= bit;
        }
        bool now_pressed = (mem->joypad_buttons & bit) == 0;
        if (!was_pressed && now_pressed)
        {
            trigger_interrupt = true;
        }
    }

    mem->memory.P1_JOYP = joypad_read(mem);
    if (trigger_interrupt)
    {
        mem->memory.IF |= IF_JOYPAD;
        mem->memory.IF &= 0x1F;
    }
}

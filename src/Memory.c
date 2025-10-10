#include "Memory.h"

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

    if (!mem->memory.BOOT && mem->bios_enabled && address < 0x0100)
    {
        return mem->bios[address];
    }

    return mem->memory.data[address];
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
    case 0xFF0F:
        mem->memory.IF = value & 0x1F;
        return;
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

    mem->memory.data[address] = value;
}
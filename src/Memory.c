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
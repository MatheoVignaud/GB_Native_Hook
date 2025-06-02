#include "CPU.h"
#include "Hooks.h"
#include "Memory.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        printf("Usage: %s <rom.gb>\n", argv[0]);
        return 1;
    }
    return 0;
}
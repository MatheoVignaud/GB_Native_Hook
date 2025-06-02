#ifndef HOOKS
#define HOOKS
#include "CPU.h"

#define MAX_HOOKS 256

typedef void (*HookFunc)(CPUState *);

static struct
{
    uint16_t address;
    HookFunc func;
} hooks[MAX_HOOKS];
static int hook_count = 0;

void register_hook(uint16_t addr, HookFunc f);
int try_hook(uint16_t addr, CPUState *cpu);

#endif
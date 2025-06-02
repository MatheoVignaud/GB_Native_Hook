#include "Hooks.h"

void register_hook(uint16_t addr, HookFunc f)
{
    if (hook_count < MAX_HOOKS)
    {
        hooks[hook_count++] = (typeof(hooks[0])){addr, f};
    }
    else
    {
        printf("Hook limit reached, cannot register more hooks.\n");
    }
}

int try_hook(uint16_t addr, CPUState *cpu)
{
    for (int i = 0; i < hook_count; i++)
    {
        if (hooks[i].address == addr)
        {
            hooks[i].func(cpu);
            return 1;
        }
    }
    return 0;
}

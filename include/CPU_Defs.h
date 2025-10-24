#ifndef CPU_DEFS
#define CPU_DEFS
#include "Memory.h"
#include <stdbool.h>
#include <stdint.h>

// Forward declaration to avoid circular dependency

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define CPU_BIG_ENDIAN
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define CPU_LITTLE_ENDIAN
#else
#error "Impossible de détecter l'endianness"
#endif

// For the flags in the F register
enum
{
    FLAG_Z = 1 << 7,
    FLAG_N = 1 << 6,
    FLAG_H = 1 << 5,
    FLAG_C = 1 << 4,
};

typedef struct CPUState
{
    union
    {
        uint16_t AF;
        struct
        {
#ifdef CPU_LITTLE_ENDIAN
            uint8_t F; // Flags (ZNH	C) – low byte
            uint8_t A; // Accumulateur   – high byte
#else                  // big endian
            uint8_t A;
            uint8_t F;
#endif
        };
    };

    union
    {
        uint16_t BC;
        struct
        {
#ifdef CPU_LITTLE_ENDIAN
            uint8_t C;
            uint8_t B;
#else
            uint8_t B;
            uint8_t C;
#endif
        };
    };

    union
    {
        uint16_t DE;
        struct
        {
#ifdef CPU_LITTLE_ENDIAN
            uint8_t E;
            uint8_t D;
#else
            uint8_t D;
            uint8_t E;
#endif
        };
    };

    union
    {
        uint16_t HL;
        struct
        {
#ifdef CPU_LITTLE_ENDIAN
            uint8_t L;
            uint8_t H;
#else
            uint8_t H;
            uint8_t L;
#endif
        };
    };

    uint16_t SP;                 // Stack Pointer
    uint16_t PC;                 // Program Counter
    bool IME : 1;                // Interrupt Master Enable
    bool EI_pending : 1;         // EI executed, enable IME after one instruction delay
    bool IME_enable_pending : 1; // IME will be enabled on the next instruction fetch
    bool halt : 1;               // CPU is halted
    bool stop : 1;               // CPU is stopped
    MemoryState *memory;         // Pointer to the memory state structure
    uint64_t cycle_count;        // Cycle count for performance tracking
    bool halt_bug;               // HALT bug active (next fetch without PC increment)
    uint16_t div_counter;        // Internal divider counter
    bool timer_prev_signal;      // Previous timer signal state (for falling edge detection)
    bool timer_reload_active;    // TIMA reload in progress (after overflow)
    uint8_t timer_reload_delay;  // Cycles remaining before reload completes
} CPUState;

static inline void setZ(CPUState *c, bool v)
{
    if (v)
        c->F |= FLAG_Z;
    else
        c->F &= ~FLAG_Z;
    c->F &= 0xF0;
}
static inline void setN(CPUState *c, bool v)
{
    if (v)
        c->F |= FLAG_N;
    else
        c->F &= ~FLAG_N;
    c->F &= 0xF0;
}
static inline void setH(CPUState *c, bool v)
{
    if (v)
        c->F |= FLAG_H;
    else
        c->F &= ~FLAG_H;
    c->F &= 0xF0;
}
static inline void setC(CPUState *c, bool v)
{
    if (v)
        c->F |= FLAG_C;
    else
        c->F &= ~FLAG_C;
    c->F &= 0xF0;
}

#endif

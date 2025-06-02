#ifndef CPU_DEFS
#define CPU_DEFS
#include <Memory.h>
#include <stdint.h>

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

typedef struct
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

    uint16_t SP;          // Stack Pointer
    uint16_t PC;          // Program Counter
    Memory *memory;       // Pointer to the memory structure
    uint64_t cycle_count; // Cycle count for performance tracking
} CPUState;

#define A_REGISTER (cpu)((cpu)->AF >> 8)

#endif
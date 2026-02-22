#ifndef RECOMP_PROBE_H
#define RECOMP_PROBE_H

#include "CPU_Defs.h"

#include <stdint.h>
#include <stddef.h>

typedef struct
{
    size_t functions_discovered;
    size_t functions_processed;
    size_t branches_discovered;
    size_t paths_discovered;
} RecompProbeStats;

void recomp_probe_init(const char *rom_path, MemoryState *mem);
void recomp_probe_shutdown(void);
void recomp_probe_set_logging(bool enabled);
void recomp_probe_run_static_reachability(uint16_t entry_pc, bool include_interrupts, bool include_boot_entry);

void recomp_probe_seed_entry(CPUState *cpu, uint16_t pc);
void recomp_probe_on_instruction(CPUState *cpu, uint16_t pc, uint8_t opcode);
void recomp_probe_on_interrupt(CPUState *cpu, uint16_t vector);
void recomp_probe_on_data_read(CPUState *cpu, uint16_t address, uint8_t value);
void recomp_probe_dyn_dump_init(const char *rom_path, MemoryState *mem);
void recomp_probe_dyn_dump_code(CPUState *cpu, uint16_t pc, const char *reason);
uint32_t recomp_probe_dyn_body_hash(CPUState *cpu, uint16_t pc);

size_t recomp_probe_discovered_count(void);
void recomp_probe_get_stats(RecompProbeStats *out_stats);

#endif

#include "CPU.h"
#include "Memory.h"
#include "PPU.h"
#include "RecompProbe.h"

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#if defined(_WIN32)
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(path) mkdir(path, 0755)
#endif

#include <dirent.h>

typedef struct
{
    bool enabled;
    bool initialized;
    bool snapshot_requested;
    int scale;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
} DisplayContext;

typedef struct
{
    bool display_enabled;
    bool auto_recomp;
    bool static_recomp;
} AppOptions;

typedef struct
{
    CPUState cpu;
    PPUState ppu;
    Memory memory_image;
    CartridgeState cartridge;
    bool bios_enabled;
    uint8_t bios[0x100];
    uint8_t joypad_buttons;
    uint8_t joypad_dpad;
    uint8_t joypad_select;
    uint8_t *ram_copy;
    size_t ram_size;
} AutoPathSnapshot;

typedef struct
{
    char magic[8];
    uint32_t version;
    uint32_t core_size;
    uint64_t ram_size;
} AutoSnapshotFileHeader;

typedef struct
{
    CPUState cpu;
    PPUState ppu;
    Memory memory_image;
    CartridgeState cartridge;
    bool bios_enabled;
    uint8_t bios[0x100];
    uint8_t joypad_buttons;
    uint8_t joypad_dpad;
    uint8_t joypad_select;
    size_t ram_size;
} AutoPathSnapshotDisk;

typedef struct
{
    bool enabled;
    bool heuristic_enabled;
    uint32_t scenario;
    uint32_t frames_in_scenario;
    uint32_t frames_per_scenario;
    uint32_t stagnation_frames;
    uint32_t max_scenarios;
    uint32_t hold_frames_left;
    uint8_t current_mask;
    uint64_t rng;
    uint64_t total_frames;
    uint64_t last_progress_frame;
    size_t last_seen_count;
    uint32_t max_clones;
    uint32_t path_step_budget;
    uint64_t total_step_budget;
    uint64_t current_path_steps;
    uint64_t total_path_steps;
    uint64_t paths_spawned;
    uint64_t paths_completed;
    uint64_t branch_splits;
    AutoPathSnapshot *snapshots;
    size_t snapshot_count;
    size_t snapshot_cap;
    uint64_t *split_keys;
    size_t split_key_count;
    size_t split_key_cap;
} AutoRecompState;

static bool read_env_enabled(const char *name)
{
    const char *value = getenv(name);
    if (!value || value[0] == '\0' || value[0] == '0')
        return false;
    return true;
}

static double read_env_double(const char *name, double fallback)
{
    const char *value = getenv(name);
    if (!value || value[0] == '\0')
        return fallback;

    char *endptr = NULL;
    double parsed = strtod(value, &endptr);
    if (endptr == value || parsed <= 0.0)
        return fallback;
    return parsed;
}

static uint32_t read_env_u32(const char *name, uint32_t fallback)
{
    const char *value = getenv(name);
    if (!value || value[0] == '\0')
        return fallback;

    char *endptr = NULL;
    unsigned long parsed = strtoul(value, &endptr, 10);
    if (endptr == value || parsed == 0 || parsed > 0xFFFFFFFFul)
        return fallback;
    return (uint32_t)parsed;
}

static uint64_t read_env_u64(const char *name, uint64_t fallback)
{
    const char *value = getenv(name);
    if (!value || value[0] == '\0')
        return fallback;

    char *endptr = NULL;
    unsigned long long parsed = strtoull(value, &endptr, 10);
    if (endptr == value || parsed == 0)
        return fallback;
    return (uint64_t)parsed;
}

static size_t auto_effective_bank(const MemoryState *mem, uint16_t address)
{
    if (!mem || address >= 0x8000u)
        return 0;
    if (address < 0x4000u)
        return 0;

    const CartridgeState *cart = &mem->cartridge;
    size_t bank = 1;
    switch (cart->mbc_type)
    {
    case MBC1:
    {
        uint8_t low = (uint8_t)(cart->mbc1_low5 & 0x1F);
        if (low == 0)
            low = 1;
        bank = (size_t)(low | ((cart->mbc1_high2 & 0x03u) << 5u));
        break;
    }
    case MBC2:
        bank = (size_t)(cart->mbc2_rom_bank & 0x0Fu);
        if (bank == 0)
            bank = 1;
        break;
    case MBC3:
        bank = (size_t)(cart->mbc3_rom_bank & 0x7Fu);
        if (bank == 0)
            bank = 1;
        break;
    case MBC5:
        bank = (size_t)(cart->mbc5_rom_bank & 0x01FFu);
        break;
    case MBC_NONE:
    default:
        bank = 1;
        break;
    }

    if (cart->rom_banks > 0)
    {
        bank %= cart->rom_banks;
    }
    return bank;
}

static void auto_snapshot_free(AutoPathSnapshot *snapshot)
{
    if (!snapshot)
        return;
    free(snapshot->ram_copy);
    memset(snapshot, 0, sizeof(*snapshot));
}

static bool auto_recomp_push_snapshot(AutoRecompState *auto_recomp, AutoPathSnapshot *snapshot);

static bool auto_snapshot_capture(AutoPathSnapshot *snapshot,
                                  const CPUState *cpu,
                                  const PPUState *ppu,
                                  const MemoryState *memory)
{
    if (!snapshot || !cpu || !ppu || !memory)
        return false;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->cpu = *cpu;
    snapshot->ppu = *ppu;
    snapshot->memory_image = memory->memory;
    snapshot->cartridge = memory->cartridge;
    snapshot->bios_enabled = memory->bios_enabled;
    memcpy(snapshot->bios, memory->bios, sizeof(snapshot->bios));
    snapshot->joypad_buttons = memory->joypad_buttons;
    snapshot->joypad_dpad = memory->joypad_dpad;
    snapshot->joypad_select = memory->joypad_select;

    snapshot->ram_size = memory->cartridge.ram_size;
    if (snapshot->ram_size > 0 && memory->cartridge.ram_data)
    {
        snapshot->ram_copy = (uint8_t *)malloc(snapshot->ram_size);
        if (!snapshot->ram_copy)
        {
            auto_snapshot_free(snapshot);
            return false;
        }
        memcpy(snapshot->ram_copy, memory->cartridge.ram_data, snapshot->ram_size);
    }
    snapshot->cartridge.ram_data = snapshot->ram_copy;
    return true;
}

static void auto_snapshot_restore(const AutoPathSnapshot *snapshot,
                                  CPUState *cpu,
                                  PPUState *ppu,
                                  MemoryState *memory)
{
    if (!snapshot || !cpu || !ppu || !memory)
        return;

    uint8_t *rom_data = memory->cartridge.rom_data;
    uint8_t *ram_data = memory->cartridge.ram_data;
    size_t ram_size = memory->cartridge.ram_size;

    memory->memory = snapshot->memory_image;
    memory->cartridge = snapshot->cartridge;
    memory->cartridge.rom_data = rom_data;
    memory->cartridge.ram_data = ram_data;
    memory->bios_enabled = snapshot->bios_enabled;
    memcpy(memory->bios, snapshot->bios, sizeof(memory->bios));
    memory->joypad_buttons = snapshot->joypad_buttons;
    memory->joypad_dpad = snapshot->joypad_dpad;
    memory->joypad_select = snapshot->joypad_select;

    if (ram_data && snapshot->ram_copy && ram_size > 0)
    {
        size_t copy_size = (ram_size < snapshot->ram_size) ? ram_size : snapshot->ram_size;
        memcpy(ram_data, snapshot->ram_copy, copy_size);
    }

    *cpu = snapshot->cpu;
    cpu->memory = memory;
    *ppu = snapshot->ppu;
    ppu->mem = &memory->memory;
    memory->cpu = cpu;
}

static bool ensure_dir_if_needed(const char *path)
{
    if (!path || path[0] == '\0')
        return false;
    if (MKDIR(path) == 0)
        return true;
#if defined(_WIN32)
    return errno == EEXIST;
#else
    return errno == EEXIST;
#endif
}

static void path_to_stem_local(const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    out[0] = '\0';
    if (!path || path[0] == '\0')
    {
        snprintf(out, out_size, "snapshot");
        return;
    }
    const char *base = path;
    for (const char *p = path; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    size_t len = strlen(base);
    while (len > 0 && base[len - 1] != '.' && base[len - 1] != '/' && base[len - 1] != '\\')
        len--;
    if (len > 0 && base[len - 1] == '.')
        len--;
    else
        len = strlen(base);
    if (len == 0)
        len = strlen(base);
    if (len >= out_size)
        len = out_size - 1;
    memcpy(out, base, len);
    out[len] = '\0';
}

static bool auto_snapshot_save_file(const char *path, const AutoPathSnapshot *snapshot)
{
    if (!path || !snapshot)
        return false;

    FILE *f = fopen(path, "wb");
    if (!f)
        return false;

    AutoSnapshotFileHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, "GBSNAP1", 7);
    hdr.version = 1u;
    hdr.core_size = (uint32_t)sizeof(AutoPathSnapshotDisk);
    hdr.ram_size = (uint64_t)snapshot->ram_size;

    AutoPathSnapshotDisk disk;
    memset(&disk, 0, sizeof(disk));
    disk.cpu = snapshot->cpu;
    disk.ppu = snapshot->ppu;
    disk.memory_image = snapshot->memory_image;
    disk.cartridge = snapshot->cartridge;
    disk.bios_enabled = snapshot->bios_enabled;
    memcpy(disk.bios, snapshot->bios, sizeof(disk.bios));
    disk.joypad_buttons = snapshot->joypad_buttons;
    disk.joypad_dpad = snapshot->joypad_dpad;
    disk.joypad_select = snapshot->joypad_select;
    disk.ram_size = snapshot->ram_size;

    /* Pointers are process-local and rebound on restore. */
    disk.cpu.memory = NULL;
    disk.ppu.mem = NULL;
    disk.cartridge.rom_data = NULL;
    disk.cartridge.ram_data = NULL;

    bool ok = true;
    ok = ok && (fwrite(&hdr, 1, sizeof(hdr), f) == sizeof(hdr));
    ok = ok && (fwrite(&disk, 1, sizeof(disk), f) == sizeof(disk));
    if (ok && snapshot->ram_size > 0 && snapshot->ram_copy)
    {
        ok = (fwrite(snapshot->ram_copy, 1, snapshot->ram_size, f) == snapshot->ram_size);
    }
    fclose(f);
    return ok;
}

static bool auto_snapshot_load_file(const char *path, AutoPathSnapshot *snapshot)
{
    if (!path || !snapshot)
        return false;

    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    AutoSnapshotFileHeader hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr))
    {
        fclose(f);
        return false;
    }
    if (memcmp(hdr.magic, "GBSNAP1", 7) != 0 || hdr.version != 1u || hdr.core_size != sizeof(AutoPathSnapshotDisk))
    {
        fclose(f);
        return false;
    }

    AutoPathSnapshotDisk disk;
    if (fread(&disk, 1, sizeof(disk), f) != sizeof(disk))
    {
        fclose(f);
        return false;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->cpu = disk.cpu;
    snapshot->ppu = disk.ppu;
    snapshot->memory_image = disk.memory_image;
    snapshot->cartridge = disk.cartridge;
    snapshot->bios_enabled = disk.bios_enabled;
    memcpy(snapshot->bios, disk.bios, sizeof(snapshot->bios));
    snapshot->joypad_buttons = disk.joypad_buttons;
    snapshot->joypad_dpad = disk.joypad_dpad;
    snapshot->joypad_select = disk.joypad_select;
    snapshot->ram_size = (size_t)hdr.ram_size;
    if (snapshot->ram_size > 0)
    {
        snapshot->ram_copy = (uint8_t *)malloc(snapshot->ram_size);
        if (!snapshot->ram_copy)
        {
            fclose(f);
            auto_snapshot_free(snapshot);
            return false;
        }
        if (fread(snapshot->ram_copy, 1, snapshot->ram_size, f) != snapshot->ram_size)
        {
            fclose(f);
            auto_snapshot_free(snapshot);
            return false;
        }
    }
    snapshot->cartridge.ram_data = snapshot->ram_copy;
    fclose(f);
    return true;
}

static bool save_manual_snapshot(const char *rom_path,
                                 const CPUState *cpu,
                                 const PPUState *ppu,
                                 const MemoryState *memory,
                                 uint64_t frame_index)
{
    AutoPathSnapshot snap;
    if (!auto_snapshot_capture(&snap, cpu, ppu, memory))
        return false;

    char stem[260];
    path_to_stem_local(rom_path, stem, sizeof(stem));
    if (!ensure_dir_if_needed(stem))
    {
        auto_snapshot_free(&snap);
        return false;
    }
    char snap_dir[512];
    snprintf(snap_dir, sizeof(snap_dir), "%s/snapshots", stem);
    if (!ensure_dir_if_needed(snap_dir))
    {
        auto_snapshot_free(&snap);
        return false;
    }

    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    char tbuf[32] = {0};
    if (tmv)
        strftime(tbuf, sizeof(tbuf), "%Y%m%d_%H%M%S", tmv);
    else
        snprintf(tbuf, sizeof(tbuf), "%llu", (unsigned long long)now);

    char path[768];
    snprintf(path, sizeof(path), "%s/snap_%s_f%llu_pc%04X.gbsnap",
             snap_dir,
             tbuf,
             (unsigned long long)frame_index,
             (unsigned)snap.cpu.PC);

    bool ok = auto_snapshot_save_file(path, &snap);
    if (ok)
        printf("[SNAPSHOT] saved %s\n", path);
    else
        fprintf(stderr, "[SNAPSHOT] failed to save %s\n", path);
    auto_snapshot_free(&snap);
    return ok;
}

static size_t auto_recomp_load_seed_snapshots(AutoRecompState *auto_recomp, const char *rom_path)
{
    if (!auto_recomp || !auto_recomp->enabled || !auto_recomp->heuristic_enabled)
        return 0;

    char dir_path[512] = {0};
    const char *env_dir = getenv("GB_AUTO_RECOMP_SNAPSHOT_DIR");
    if (env_dir && env_dir[0] != '\0')
    {
        snprintf(dir_path, sizeof(dir_path), "%s", env_dir);
    }
    else
    {
        char stem[260];
        path_to_stem_local(rom_path, stem, sizeof(stem));
        snprintf(dir_path, sizeof(dir_path), "%s/snapshots", stem);
    }

    DIR *dir = opendir(dir_path);
    if (!dir)
        return 0;

    size_t loaded = 0;
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL)
    {
        const char *name = ent->d_name;
        if (!name)
            continue;
        size_t nlen = strlen(name);
        if (nlen < 7 || strcmp(name + nlen - 7, ".gbsnap") != 0)
            continue;

        char path[768];
        snprintf(path, sizeof(path), "%s/%s", dir_path, name);
        AutoPathSnapshot snap;
        if (!auto_snapshot_load_file(path, &snap))
            continue;
        if (!auto_recomp_push_snapshot(auto_recomp, &snap))
        {
            auto_snapshot_free(&snap);
            continue;
        }
        loaded++;
    }
    closedir(dir);
    return loaded;
}

static bool auto_recomp_push_snapshot(AutoRecompState *auto_recomp, AutoPathSnapshot *snapshot)
{
    if (!auto_recomp || !snapshot || !auto_recomp->heuristic_enabled)
        return false;

    if (auto_recomp->snapshot_count >= auto_recomp->max_clones)
    {
        auto_snapshot_free(snapshot);
        return false;
    }

    if (auto_recomp->snapshot_count == auto_recomp->snapshot_cap)
    {
        size_t next_cap = (auto_recomp->snapshot_cap == 0) ? 32 : auto_recomp->snapshot_cap * 2;
        if (next_cap > auto_recomp->max_clones)
            next_cap = auto_recomp->max_clones;
        AutoPathSnapshot *next = (AutoPathSnapshot *)realloc(auto_recomp->snapshots, next_cap * sizeof(AutoPathSnapshot));
        if (!next)
        {
            auto_snapshot_free(snapshot);
            return false;
        }
        auto_recomp->snapshots = next;
        auto_recomp->snapshot_cap = next_cap;
    }

    auto_recomp->snapshots[auto_recomp->snapshot_count++] = *snapshot;
    memset(snapshot, 0, sizeof(*snapshot));
    auto_recomp->paths_spawned++;
    return true;
}

static bool auto_recomp_pop_snapshot(AutoRecompState *auto_recomp, AutoPathSnapshot *out_snapshot)
{
    if (!auto_recomp || !out_snapshot || auto_recomp->snapshot_count == 0)
        return false;

    *out_snapshot = auto_recomp->snapshots[--auto_recomp->snapshot_count];
    memset(&auto_recomp->snapshots[auto_recomp->snapshot_count], 0, sizeof(AutoPathSnapshot));
    return true;
}

static bool auto_recomp_mark_split_seen(AutoRecompState *auto_recomp, uint64_t key)
{
    if (!auto_recomp)
        return true;

    for (size_t i = 0; i < auto_recomp->split_key_count; ++i)
    {
        if (auto_recomp->split_keys[i] == key)
            return true;
    }

    if (auto_recomp->split_key_count == auto_recomp->split_key_cap)
    {
        size_t next_cap = (auto_recomp->split_key_cap == 0) ? 256 : auto_recomp->split_key_cap * 2;
        uint64_t *next = (uint64_t *)realloc(auto_recomp->split_keys, next_cap * sizeof(uint64_t));
        if (!next)
            return true;
        auto_recomp->split_keys = next;
        auto_recomp->split_key_cap = next_cap;
    }

    auto_recomp->split_keys[auto_recomp->split_key_count++] = key;
    return false;
}

static uint64_t auto_recomp_next_u64(uint64_t *state)
{
    *state = (*state * 6364136223846793005ULL) + 1442695040888963407ULL;
    return *state;
}

static void auto_recomp_apply_mask(MemoryState *memory, AutoRecompState *auto_recomp, uint8_t next_mask)
{
    if (!memory || !auto_recomp)
        return;

    for (int i = 0; i < 8; ++i)
    {
        uint8_t bit = (uint8_t)(1u << i);
        bool old_pressed = (auto_recomp->current_mask & bit) != 0;
        bool new_pressed = (next_mask & bit) != 0;
        if (old_pressed == new_pressed)
            continue;
        memory_set_button_state(memory, (JoypadInput)i, new_pressed);
    }

    auto_recomp->current_mask = next_mask;
}

static bool auto_recomp_enqueue_alt_path(AutoRecompState *auto_recomp,
                                         CPUState *cpu,
                                         PPUState *ppu,
                                         MemoryState *memory)
{
    if (!auto_recomp || !auto_recomp->enabled || !auto_recomp->heuristic_enabled || !cpu || !ppu || !memory)
        return false;
    if (cpu->PC >= 0x8000u)
        return false;
    if (auto_recomp->snapshot_count >= auto_recomp->max_clones)
        return false;

    uint16_t pc = cpu->PC;
    uint8_t op = memory_read(memory, pc);
    uint8_t n8 = memory_read(memory, (uint16_t)(pc + 1u));
    uint16_t n16 = (uint16_t)(memory_read(memory, (uint16_t)(pc + 1u)) |
                              ((uint16_t)memory_read(memory, (uint16_t)(pc + 2u)) << 8u));
    uint16_t fallthrough = (uint16_t)(pc + ((op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38 || op == 0x18) ? 2u : ((op == 0xC2 || op == 0xCA || op == 0xD2 || op == 0xDA || op == 0xC4 || op == 0xCC || op == 0xD4 || op == 0xDC || op == 0xCD) ? 3u : 1u)));

    bool can_split = false;
    uint16_t alt_pc = 0;
    uint16_t alt_sp = cpu->SP;
    bool write_return = false;
    uint16_t write_return_addr = 0;
    uint16_t write_return_value = 0;

    switch (op)
    {
    case 0x20: // JR NZ,e8
    case 0x28: // JR Z,e8
    case 0x30: // JR NC,e8
    case 0x38: // JR C,e8
    {
        bool cond = false;
        if (op == 0x20)
            cond = (cpu->F & FLAG_Z) == 0;
        else if (op == 0x28)
            cond = (cpu->F & FLAG_Z) != 0;
        else if (op == 0x30)
            cond = (cpu->F & FLAG_C) == 0;
        else
            cond = (cpu->F & FLAG_C) != 0;
        uint16_t target = (uint16_t)(pc + 2u + (int8_t)n8);
        alt_pc = cond ? fallthrough : target;
        can_split = true;
        break;
    }
    case 0xC2: // JP NZ,nn
    case 0xCA: // JP Z,nn
    case 0xD2: // JP NC,nn
    case 0xDA: // JP C,nn
    {
        bool cond = false;
        if (op == 0xC2)
            cond = (cpu->F & FLAG_Z) == 0;
        else if (op == 0xCA)
            cond = (cpu->F & FLAG_Z) != 0;
        else if (op == 0xD2)
            cond = (cpu->F & FLAG_C) == 0;
        else
            cond = (cpu->F & FLAG_C) != 0;
        alt_pc = cond ? fallthrough : n16;
        can_split = true;
        break;
    }
    case 0xC4: // CALL NZ,nn
    case 0xCC: // CALL Z,nn
    case 0xD4: // CALL NC,nn
    case 0xDC: // CALL C,nn
    {
        bool cond = false;
        if (op == 0xC4)
            cond = (cpu->F & FLAG_Z) == 0;
        else if (op == 0xCC)
            cond = (cpu->F & FLAG_Z) != 0;
        else if (op == 0xD4)
            cond = (cpu->F & FLAG_C) == 0;
        else
            cond = (cpu->F & FLAG_C) != 0;

        if (cond)
        {
            alt_pc = fallthrough; // opposite: skip call
        }
        else
        {
            alt_pc = n16; // opposite: force call
            alt_sp = (uint16_t)(cpu->SP - 2u);
            write_return = true;
            write_return_addr = alt_sp;
            write_return_value = fallthrough;
        }
        can_split = true;
        break;
    }
    case 0xCD: // CALL nn (heuristic split: skip callee path)
        alt_pc = fallthrough;
        can_split = true;
        break;
    case 0xC0: // RET NZ
    case 0xC8: // RET Z
    case 0xD0: // RET NC
    case 0xD8: // RET C
    {
        bool cond = false;
        if (op == 0xC0)
            cond = (cpu->F & FLAG_Z) == 0;
        else if (op == 0xC8)
            cond = (cpu->F & FLAG_Z) != 0;
        else if (op == 0xD0)
            cond = (cpu->F & FLAG_C) == 0;
        else
            cond = (cpu->F & FLAG_C) != 0;

        uint16_t ret_addr = (uint16_t)(memory_read(memory, cpu->SP) |
                                       ((uint16_t)memory_read(memory, (uint16_t)(cpu->SP + 1u)) << 8u));
        if (cond)
        {
            alt_pc = fallthrough; // opposite: do not return
        }
        else
        {
            alt_pc = ret_addr; // opposite: force return
            alt_sp = (uint16_t)(cpu->SP + 2u);
        }
        can_split = true;
        break;
    }
    case 0xC7:
    case 0xCF:
    case 0xD7:
    case 0xDF:
    case 0xE7:
    case 0xEF:
    case 0xF7:
    case 0xFF: // RST: heuristic split to skip
        alt_pc = fallthrough;
        can_split = true;
        break;
    default:
        break;
    }

    if (!can_split || alt_pc >= 0x8000u)
        return false;

    size_t bank = auto_effective_bank(memory, pc);
    size_t alt_bank = auto_effective_bank(memory, alt_pc);
    uint64_t key = ((uint64_t)(bank & 0x3FFu) << 54u) ^
                   ((uint64_t)pc << 38u) ^
                   ((uint64_t)(alt_bank & 0x3FFu) << 28u) ^
                   ((uint64_t)alt_pc << 12u) ^
                   ((uint64_t)op << 4u);
    if (auto_recomp_mark_split_seen(auto_recomp, key))
        return false;

    AutoPathSnapshot snapshot;
    if (!auto_snapshot_capture(&snapshot, cpu, ppu, memory))
        return false;

    snapshot.cpu.PC = alt_pc;
    if (alt_sp != snapshot.cpu.SP)
    {
        snapshot.cpu.SP = alt_sp;
    }
    if (write_return && (uint32_t)write_return_addr + 1u < 0x10000u)
    {
        snapshot.memory_image.data[write_return_addr] = (uint8_t)(write_return_value & 0xFFu);
        snapshot.memory_image.data[(uint16_t)(write_return_addr + 1u)] = (uint8_t)(write_return_value >> 8u);
    }

    if (auto_recomp_push_snapshot(auto_recomp, &snapshot))
    {
        auto_recomp->branch_splits++;
        return true;
    }
    return false;
}

static uint8_t auto_recomp_pick_explore_mask(AutoRecompState *auto_recomp)
{
    uint64_t r = auto_recomp_next_u64(&auto_recomp->rng);
    uint8_t mask = 0;

    static const uint8_t dpad_patterns[] = {
        0,
        (uint8_t)(1u << JOYPAD_RIGHT),
        (uint8_t)(1u << JOYPAD_LEFT),
        (uint8_t)(1u << JOYPAD_UP),
        (uint8_t)(1u << JOYPAD_DOWN),
        (uint8_t)((1u << JOYPAD_UP) | (1u << JOYPAD_RIGHT)),
        (uint8_t)((1u << JOYPAD_UP) | (1u << JOYPAD_LEFT)),
        (uint8_t)((1u << JOYPAD_DOWN) | (1u << JOYPAD_RIGHT)),
        (uint8_t)((1u << JOYPAD_DOWN) | (1u << JOYPAD_LEFT)),
    };

    uint32_t mode = auto_recomp->scenario % 6;
    size_t dpad_index = 0;
    switch (mode)
    {
    case 0:
        dpad_index = (size_t)((r >> 1) % (sizeof(dpad_patterns) / sizeof(dpad_patterns[0])));
        break;
    case 1:
        dpad_index = (size_t)((r % 3) + 1); // Right/Left/Up heavy.
        break;
    case 2:
        dpad_index = (size_t)(((r >> 3) % 2) ? 4 : 3); // Up/Down exploration.
        break;
    case 3:
        dpad_index = (size_t)(5 + ((r >> 5) % 4)); // Diagonal exploration.
        break;
    case 4:
        dpad_index = (size_t)((r >> 7) % 5); // Mostly cardinal + idle.
        break;
    default:
        dpad_index = (size_t)((r >> 9) % (sizeof(dpad_patterns) / sizeof(dpad_patterns[0])));
        break;
    }
    mask |= dpad_patterns[dpad_index];

    uint32_t action_roll = (uint32_t)((r >> 12) & 0x0F);
    if (action_roll >= 6 && action_roll <= 9)
    {
        mask |= (uint8_t)(1u << JOYPAD_A);
    }
    else if (action_roll >= 10 && action_roll <= 12)
    {
        mask |= (uint8_t)(1u << JOYPAD_B);
    }
    else if (action_roll == 13)
    {
        mask |= (uint8_t)((1u << JOYPAD_A) | (1u << JOYPAD_B));
    }

    if (((r >> 18) & 0x1F) == 0)
    {
        mask |= (uint8_t)(1u << JOYPAD_START);
    }
    if (((r >> 23) & 0x3F) == 0)
    {
        mask |= (uint8_t)(1u << JOYPAD_SELECT);
    }

    return mask;
}

static void auto_recomp_step_inputs(AutoRecompState *auto_recomp, MemoryState *memory)
{
    if (!auto_recomp || !auto_recomp->enabled || !memory)
        return;

    uint8_t next_mask = 0;
    uint32_t f = auto_recomp->frames_in_scenario;

    // Boot/menu assist: START pulses, then A taps to move into gameplay.
    if (f < 360)
    {
        if ((f >= 120 && f < 156) || (f >= 220 && f < 252))
        {
            next_mask |= (uint8_t)(1u << JOYPAD_START);
        }
        if ((f >= 280 && f < 292) || (f >= 320 && f < 332))
        {
            next_mask |= (uint8_t)(1u << JOYPAD_A);
        }
        auto_recomp->hold_frames_left = 0;
    }
    else
    {
        if (auto_recomp->hold_frames_left == 0)
        {
            uint64_t r = auto_recomp_next_u64(&auto_recomp->rng);
            auto_recomp->hold_frames_left = (uint32_t)(8u + (r & 0x1Fu)); // 8..39 frames
            next_mask = auto_recomp_pick_explore_mask(auto_recomp);
        }
        else
        {
            auto_recomp->hold_frames_left--;
            next_mask = auto_recomp->current_mask;
        }
    }

    auto_recomp_apply_mask(memory, auto_recomp, next_mask);
}

static void auto_recomp_on_progress(AutoRecompState *auto_recomp, size_t discovered_count)
{
    if (!auto_recomp || !auto_recomp->enabled)
        return;

    if (discovered_count > auto_recomp->last_seen_count)
    {
        auto_recomp->last_seen_count = discovered_count;
        auto_recomp->last_progress_frame = auto_recomp->total_frames;
    }
}

static void format_elapsed_hms(uint64_t seconds, char *out, size_t out_size)
{
    uint64_t h = seconds / 3600u;
    uint64_t m = (seconds % 3600u) / 60u;
    uint64_t s = seconds % 60u;
    snprintf(out, out_size, "%02llu:%02llu:%02llu",
             (unsigned long long)h,
             (unsigned long long)m,
             (unsigned long long)s);
}

static void print_updating_status_line(uint64_t elapsed_seconds,
                                       const AutoRecompState *auto_recomp,
                                       size_t *last_line_len)
{
    if (!auto_recomp || !auto_recomp->enabled || !last_line_len)
        return;

    RecompProbeStats stats = {0};
    recomp_probe_get_stats(&stats);

    double pct = 0.0;
    if (stats.functions_discovered > 0)
    {
        pct = (100.0 * (double)stats.functions_processed) / (double)stats.functions_discovered;
        if (pct > 100.0)
            pct = 100.0;
    }
    double frontier_pct = 0.0;
    {
        double denom = (double)auto_recomp->paths_completed + (double)auto_recomp->snapshot_count + 1.0;
        frontier_pct = denom > 0.0 ? (100.0 * (double)auto_recomp->paths_completed / denom) : 0.0;
    }

    char tbuf[32];
    format_elapsed_hms(elapsed_seconds, tbuf, sizeof(tbuf));

    char line[512];
    snprintf(line,
             sizeof(line),
             "\r[AUTO-RECOMP] t=%s funcs=%zu branches=%zu paths=%zu progress=%.1f%% frontier=%.1f%% active_steps=%llu queued=%zu spawned=%llu done=%llu splits=%llu",
             tbuf,
             stats.functions_discovered,
             stats.branches_discovered,
             stats.paths_discovered,
             pct,
             frontier_pct,
             (unsigned long long)auto_recomp->current_path_steps,
             auto_recomp->snapshot_count,
             (unsigned long long)auto_recomp->paths_spawned,
             (unsigned long long)auto_recomp->paths_completed,
             (unsigned long long)auto_recomp->branch_splits);

    fputs(line, stdout);
    size_t len = strlen(line);
    if (len < *last_line_len)
    {
        for (size_t i = len; i < *last_line_len; ++i)
        {
            fputc(' ', stdout);
        }
    }
    fflush(stdout);
    *last_line_len = len;
}

static bool auto_recomp_should_restart(const AutoRecompState *auto_recomp, const char **reason)
{
    if (!auto_recomp || !auto_recomp->enabled)
        return false;

    if (auto_recomp->frames_per_scenario > 0 && auto_recomp->frames_in_scenario >= auto_recomp->frames_per_scenario)
    {
        if (reason)
            *reason = "scenario_limit";
        return true;
    }

    if (auto_recomp->stagnation_frames > 0)
    {
        uint64_t since_progress = auto_recomp->total_frames - auto_recomp->last_progress_frame;
        if (since_progress >= auto_recomp->stagnation_frames)
        {
            if (reason)
                *reason = "stagnation";
            return true;
        }
    }

    return false;
}

static bool auto_recomp_switch_to_next_path(AutoRecompState *auto_recomp,
                                            CPUState *cpu,
                                            PPUState *ppu,
                                            MemoryState *memory)
{
    if (!auto_recomp || !auto_recomp->heuristic_enabled)
        return false;

    AutoPathSnapshot snapshot;
    if (!auto_recomp_pop_snapshot(auto_recomp, &snapshot))
        return false;

    auto_snapshot_restore(&snapshot, cpu, ppu, memory);
    auto_snapshot_free(&snapshot);
    auto_recomp->paths_completed++;
    auto_recomp->current_path_steps = 0;
    return true;
}

static void auto_recomp_release(AutoRecompState *auto_recomp)
{
    if (!auto_recomp)
        return;

    for (size_t i = 0; i < auto_recomp->snapshot_count; ++i)
    {
        auto_snapshot_free(&auto_recomp->snapshots[i]);
    }
    free(auto_recomp->snapshots);
    free(auto_recomp->split_keys);
    auto_recomp->snapshots = NULL;
    auto_recomp->snapshot_count = 0;
    auto_recomp->snapshot_cap = 0;
    auto_recomp->split_keys = NULL;
    auto_recomp->split_key_count = 0;
    auto_recomp->split_key_cap = 0;
}

static void auto_recomp_init(AutoRecompState *auto_recomp, bool enabled)
{
    memset(auto_recomp, 0, sizeof(*auto_recomp));
    if (!enabled)
        return;

    auto_recomp->enabled = true;
    auto_recomp->heuristic_enabled = !read_env_enabled("GB_AUTO_RECOMP_NO_HEURISTIC");
    auto_recomp->frames_per_scenario = read_env_u32("GB_AUTO_RECOMP_SCENARIO_FRAMES", 60u * 45u);
    auto_recomp->stagnation_frames = read_env_u32("GB_AUTO_RECOMP_STAGNATION_FRAMES", 60u * 20u);
    auto_recomp->max_scenarios = read_env_u32("GB_AUTO_RECOMP_MAX_SCENARIOS", 0);
    auto_recomp->max_clones = read_env_u32("GB_AUTO_RECOMP_MAX_CLONES", 24u);
    auto_recomp->path_step_budget = read_env_u32("GB_AUTO_RECOMP_PATH_STEPS", 50000u);
    auto_recomp->total_step_budget = read_env_u64("GB_AUTO_RECOMP_TOTAL_STEPS", 3000000ULL);
    auto_recomp->paths_spawned = 1;

    uint32_t seed = read_env_u32("GB_AUTO_RECOMP_SEED", 0);
    if (seed == 0)
    {
        seed = 0x00C0DE42u;
    }
    auto_recomp->rng = ((uint64_t)seed << 32) ^ 0x9E3779B97F4A7C15ULL;
}

static void display_shutdown(DisplayContext *display)
{
    if (!display->enabled)
        return;

    if (display->texture)
    {
        SDL_DestroyTexture(display->texture);
        display->texture = NULL;
    }
    if (display->renderer)
    {
        SDL_DestroyRenderer(display->renderer);
        display->renderer = NULL;
    }
    if (display->window)
    {
        SDL_DestroyWindow(display->window);
        display->window = NULL;
    }
    if (display->initialized)
    {
        SDL_Quit();
        display->initialized = false;
    }
    display->enabled = false;
}

static bool display_init(DisplayContext *display)
{
    if (!display->enabled)
        return true;

    display->scale = 3;
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
    {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        display->enabled = false;
        return false;
    }

    display->initialized = true;

    display->window = SDL_CreateWindow("GB Native Hook",
                                       GB_SCREEN_WIDTH * display->scale,
                                       GB_SCREEN_HEIGHT * display->scale,
                                       0);
    if (!display->window)
    {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        display_shutdown(display);
        return false;
    }

    display->renderer = SDL_CreateRenderer(display->window, NULL);
    if (!display->renderer)
    {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        display_shutdown(display);
        return false;
    }

    SDL_SetRenderVSync(display->renderer, true);
    SDL_SetRenderDrawColor(display->renderer, 0.0f, 0.0f, 0.0f, 1.0f);

    display->texture = SDL_CreateTexture(display->renderer,
                                         SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING,
                                         GB_SCREEN_WIDTH,
                                         GB_SCREEN_HEIGHT);
    if (!display->texture)
    {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        display_shutdown(display);
        return false;
    }
    SDL_SetTextureScaleMode(display->texture, SDL_SCALEMODE_NEAREST);

    return true;
}

static void handle_key_event(MemoryState *memory, SDL_Keycode key, bool pressed)
{
    switch (key)
    {
    case SDLK_RIGHT:
        memory_set_button_state(memory, JOYPAD_RIGHT, pressed);
        break;
    case SDLK_LEFT:
        memory_set_button_state(memory, JOYPAD_LEFT, pressed);
        break;
    case SDLK_UP:
        memory_set_button_state(memory, JOYPAD_UP, pressed);
        break;
    case SDLK_DOWN:
        memory_set_button_state(memory, JOYPAD_DOWN, pressed);
        break;
    case SDLK_Z:
        memory_set_button_state(memory, JOYPAD_B, pressed);
        break;
    case SDLK_A:
        memory_set_button_state(memory, JOYPAD_A, pressed);
        break;
    case SDLK_S:
        memory_set_button_state(memory, JOYPAD_SELECT, pressed);
        break;
    case SDLK_Q:
        memory_set_button_state(memory, JOYPAD_START, pressed);
        break;
    default:
        break;
    }
}

static bool display_poll_events(DisplayContext *display, MemoryState *memory)
{
    if (!display->enabled)
        return true;

    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        switch (event.type)
        {
        case SDL_EVENT_QUIT:
            return false;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.repeat)
                break;
            if (event.key.key == SDLK_ESCAPE)
            {
                return false;
            }
            if (event.key.key == SDLK_F5)
            {
                display->snapshot_requested = true;
                break;
            }
            handle_key_event(memory, event.key.key, true);
            break;
        case SDL_EVENT_KEY_UP:
            handle_key_event(memory, event.key.key, false);
            break;
        default:
            break;
        }
    }
    return true;
}

static void display_present(DisplayContext *display, const PPUState *ppu)
{
    if (!display->enabled || !display->texture || !display->renderer)
        return;

    if (SDL_UpdateTexture(display->texture, NULL, ppu->fb, GB_SCREEN_WIDTH * (int)sizeof(uint32_t)) < 0)
    {
        fprintf(stderr, "SDL_UpdateTexture failed: %s\n", SDL_GetError());
        return;
    }

    SDL_RenderClear(display->renderer);
    SDL_FRect dst = {
        0.0f,
        0.0f,
        (float)(GB_SCREEN_WIDTH * display->scale),
        (float)(GB_SCREEN_HEIGHT * display->scale)};
    SDL_RenderTexture(display->renderer, display->texture, NULL, &dst);
    SDL_RenderPresent(display->renderer);
}

static bool reset_runtime_state(const char *rom_path, MemoryState *memory, CPUState *cpu, PPUState *ppu, bool verbose)
{
    memory_shutdown(memory);
    memory_init(memory);

    if (load_bios("dmg_rom.bin", memory->bios) == 0)
    {
        memory->bios_enabled = true;
        if (verbose)
            printf("BIOS loaded and enabled.\n");
    }
    else
    {
        memory->bios_enabled = false;
        if (verbose)
            printf("BIOS not found, starting without BIOS.\n");
    }

    if (load_rom(rom_path, memory) != 0)
    {
        fprintf(stderr, "Failed to load ROM: %s\n", rom_path);
        return false;
    }
    if (verbose)
        printf("Loaded ROM: %s\n", rom_path);

    cpu->memory = memory;
    memory->cpu = cpu;
    ppu->mem = &memory->memory;

    cpu_reset(cpu);
    ppu_reset(ppu, memory->bios_enabled);
    return true;
}

static const char *parse_arguments(int argc, char **argv, AppOptions *options)
{
    const char *rom_path = NULL;
    options->display_enabled = true;
    options->auto_recomp = false;
    options->static_recomp = false;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--no-display") == 0)
        {
            options->display_enabled = false;
        }
        else if (strcmp(argv[i], "--display") == 0)
        {
            options->display_enabled = true;
        }
        else if (strcmp(argv[i], "--auto-recomp") == 0)
        {
            options->auto_recomp = true;
        }
        else if (strcmp(argv[i], "--static-recomp") == 0)
        {
            options->static_recomp = true;
        }
        else if (argv[i][0] == '-')
        {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
        }
        else if (!rom_path)
        {
            rom_path = argv[i];
        }
    }

    return rom_path;
}

int main(int argc, char **argv)
{
    MemoryState memory;
    memory_init(&memory);

    AppOptions options = {0};
    const char *rom_path = parse_arguments(argc, argv, &options);
    if (read_env_enabled("GB_STATIC_RECOMP"))
    {
        options.static_recomp = true;
    }
    if (read_env_enabled("GB_AUTO_RECOMP"))
    {
        options.auto_recomp = true;
    }
    if (options.static_recomp)
    {
        options.auto_recomp = false;
    }
    if ((options.auto_recomp || options.static_recomp) && !read_env_enabled("GB_AUTO_RECOMP_DISPLAY"))
    {
        options.display_enabled = false;
    }
    const bool quiet_auto_recomp_logs = (options.auto_recomp || options.static_recomp) && !read_env_enabled("GB_AUTO_RECOMP_VERBOSE");
    memory_set_logging(!quiet_auto_recomp_logs);

    if (load_bios("dmg_rom.bin", memory.bios) == 0)
    {
        memory.bios_enabled = true;
        if (!quiet_auto_recomp_logs)
        {
            printf("BIOS loaded and enabled.\n");
        }
    }
    else
    {
        memory.bios_enabled = false;
        if (!quiet_auto_recomp_logs)
        {
            printf("BIOS not found, starting without BIOS.\n");
        }
    }

    const char *primary_rom = rom_path ? rom_path : "Pokemon.gb";
    const char *loaded_rom = primary_rom;
    if (load_rom(primary_rom, &memory) != 0)
    {
        fprintf(stderr, "Failed to load ROM: %s\n", primary_rom);
        if (strcmp(primary_rom, "Pokemon.gb") != 0 && load_rom("Pokemon.gb", &memory) == 0)
        {
            if (!quiet_auto_recomp_logs)
            {
                printf("Loaded default ROM: Pokemon.gb\n");
            }
            loaded_rom = "Pokemon.gb";
        }
        else
        {
            fprintf(stderr, "Failed to load default ROM: Pokemon.gb\n");
            return 1;
        }
    }
    else
    {
        if (!quiet_auto_recomp_logs)
        {
            printf("Loaded ROM: %s\n", primary_rom);
        }
    }

    CPUState cpu = {0};
    PPUState ppu = {0};
    recomp_probe_set_logging(!quiet_auto_recomp_logs);
    recomp_probe_init(loaded_rom, &memory);
    cpu.memory = &memory;
    memory.cpu = &cpu;
    ppu.mem = &memory.memory;
    cpu_reset(&cpu);
    ppu_reset(&ppu, memory.bios_enabled);

    if (options.static_recomp)
    {
        uint64_t perf_freq = SDL_GetPerformanceFrequency();
        if (perf_freq == 0)
            perf_freq = 1;
        uint64_t t0 = SDL_GetPerformanceCounter();

        recomp_probe_run_static_reachability(cpu.PC, true, true);

        uint64_t t1 = SDL_GetPerformanceCounter();
        uint64_t elapsed_s = (t1 - t0) / perf_freq;
        RecompProbeStats stats = {0};
        recomp_probe_get_stats(&stats);
        double pct = stats.functions_discovered ? (100.0 * (double)stats.functions_processed / (double)stats.functions_discovered) : 0.0;
        if (pct > 100.0)
            pct = 100.0;

        char tbuf[32];
        format_elapsed_hms(elapsed_s, tbuf, sizeof(tbuf));
        printf("[STATIC-RECOMP] done t=%s funcs=%zu branches=%zu paths=%zu progress=%.1f%%\n",
               tbuf,
               stats.functions_discovered,
               stats.branches_discovered,
               stats.paths_discovered,
               pct);

        recomp_probe_shutdown();
        memory_shutdown(&memory);
        return 0;
    }

    DisplayContext display = {
        .enabled = options.display_enabled,
        .initialized = false,
        .snapshot_requested = false,
        .scale = 3,
        .window = NULL,
        .renderer = NULL,
        .texture = NULL};

    display_init(&display);

    AutoRecompState auto_recomp = {0};
    auto_recomp_init(&auto_recomp, options.auto_recomp);
    if (auto_recomp.enabled)
    {
        auto_recomp.last_seen_count = recomp_probe_discovered_count();
        auto_recomp.last_progress_frame = 0;
        size_t seeded_snapshots = auto_recomp_load_seed_snapshots(&auto_recomp, loaded_rom);
        if (!quiet_auto_recomp_logs)
        {
            printf("[AUTO-RECOMP] enabled heuristic=%s max_clones=%u path_steps=%u total_steps=%llu scenario_frames=%u stagnation_frames=%u max_scenarios=%u seed=%llu seed_snapshots=%zu\n",
                   auto_recomp.heuristic_enabled ? "on" : "off",
                   auto_recomp.max_clones,
                   auto_recomp.path_step_budget,
                   (unsigned long long)auto_recomp.total_step_budget,
                   auto_recomp.frames_per_scenario,
                   auto_recomp.stagnation_frames,
                   auto_recomp.max_scenarios,
                   (unsigned long long)auto_recomp.rng,
                   seeded_snapshots);
        }
    }

    const bool auto_start = !auto_recomp.enabled && read_env_enabled("GB_AUTOSTART");
    const bool realtime_limit_requested = !read_env_enabled("GB_NO_FRAME_LIMIT");
    const bool realtime_limit_enabled = realtime_limit_requested && !auto_recomp.enabled;
    const double speed_mult = read_env_double("GB_SPEED_MULT", 1.0);
    const double target_mcycles_per_sec = 1048576.0 * speed_mult;
    const bool fps_diag_enabled = !auto_recomp.enabled && read_env_enabled("GB_DIAG_FPS");
    double fps_diag_interval_s = read_env_double("GB_DIAG_FPS_INTERVAL", 1.0);
    if (fps_diag_interval_s < 0.1)
    {
        fps_diag_interval_s = 0.1;
    }
    uint64_t perf_freq = SDL_GetPerformanceFrequency();
    if (perf_freq == 0)
    {
        perf_freq = 1;
    }
    uint64_t fps_diag_interval_ticks = (uint64_t)((double)perf_freq * fps_diag_interval_s);
    if (fps_diag_interval_ticks == 0)
    {
        fps_diag_interval_ticks = 1;
    }
    uint64_t throttle_epoch = SDL_GetPerformanceCounter();
    double emulated_mcycles = 0.0;
    uint64_t fps_diag_last_tick = throttle_epoch;
    uint64_t fps_diag_emu_frames = 0;
    uint64_t fps_diag_presented_frames = 0;
    uint64_t fps_diag_mcycles = 0;
    uint64_t fps_diag_lcdc_on_samples = 0;
    uint64_t fps_diag_samples = 0;
    uint64_t run_start_tick = throttle_epoch;
    uint64_t status_last_tick = throttle_epoch;
    size_t status_line_len = 0;

    if (fps_diag_enabled)
    {
        printf("[FPS-DIAG] enabled interval=%.2fs limiter=%s speed_mult=%.3f\n",
               fps_diag_interval_s,
               realtime_limit_enabled ? "on" : "off",
               speed_mult);
    }

    uint64_t frame_index = 0;

    bool running = true;
    while (running)
    {
        if (auto_recomp.enabled && auto_recomp.heuristic_enabled)
        {
            (void)auto_recomp_enqueue_alt_path(&auto_recomp, &cpu, &ppu, &memory);
        }

        uint32_t cycles = cpu_execute_instruction(&cpu);
        ppu_step(&ppu, cycles);
        if (auto_recomp.enabled && auto_recomp.heuristic_enabled)
        {
            auto_recomp.total_path_steps++;
            auto_recomp.current_path_steps++;
            auto_recomp_on_progress(&auto_recomp, recomp_probe_discovered_count());

            bool total_budget_hit = auto_recomp.total_step_budget > 0 && auto_recomp.total_path_steps >= auto_recomp.total_step_budget;
            bool path_budget_hit = auto_recomp.path_step_budget > 0 && auto_recomp.current_path_steps >= auto_recomp.path_step_budget;
            if (total_budget_hit || path_budget_hit)
            {
                if (!auto_recomp_switch_to_next_path(&auto_recomp, &cpu, &ppu, &memory))
                {
                    running = false;
                }
                if (total_budget_hit)
                {
                    running = false;
                }
            }
        }
        if (fps_diag_enabled)
        {
            fps_diag_mcycles += cycles;
            fps_diag_samples++;
            if ((memory.memory.LCDC & 0x80u) != 0)
            {
                fps_diag_lcdc_on_samples++;
            }
        }

        if (realtime_limit_enabled)
        {
            emulated_mcycles += (double)cycles;
            uint64_t now = SDL_GetPerformanceCounter();
            double real_elapsed = (double)(now - throttle_epoch) / (double)perf_freq;
            double emu_elapsed = emulated_mcycles / target_mcycles_per_sec;

            if (emu_elapsed > real_elapsed)
            {
                double ahead_s = emu_elapsed - real_elapsed;
                uint32_t sleep_ms = (uint32_t)(ahead_s * 1000.0);
                if (sleep_ms > 1)
                {
                    SDL_Delay(sleep_ms - 1);
                }

                while (((double)(SDL_GetPerformanceCounter() - throttle_epoch) / (double)perf_freq) < emu_elapsed)
                {
                }
            }
            else if ((real_elapsed - emu_elapsed) > 0.25)
            {
                // Resync timing if host stalls for a while.
                throttle_epoch = now;
                emulated_mcycles = 0.0;
            }
        }

        if (ppu.frame_ready)
        {
            if (fps_diag_enabled)
            {
                fps_diag_emu_frames++;
            }

            if (!display_poll_events(&display, &memory))
            {
                running = false;
            }
            if (display.snapshot_requested)
            {
                (void)save_manual_snapshot(loaded_rom, &cpu, &ppu, &memory, frame_index);
                display.snapshot_requested = false;
            }

            frame_index++;

            if (auto_recomp.enabled)
            {
                if (auto_recomp.heuristic_enabled)
                {
                    auto_recomp.total_frames++;
                }
                else
                {
                    auto_recomp.total_frames++;
                    auto_recomp.frames_in_scenario++;
                    auto_recomp_step_inputs(&auto_recomp, &memory);
                    auto_recomp_on_progress(&auto_recomp, recomp_probe_discovered_count());

                    if (auto_recomp_should_restart(&auto_recomp, NULL))
                    {
                        auto_recomp_apply_mask(&memory, &auto_recomp, 0);
                        auto_recomp.scenario++;

                        if (auto_recomp.max_scenarios > 0 && auto_recomp.scenario >= auto_recomp.max_scenarios)
                        {
                            running = false;
                        }
                        else
                        {
                            auto_recomp.frames_in_scenario = 0;
                            auto_recomp.hold_frames_left = 0;
                            auto_recomp.current_mask = 0;
                            auto_recomp.rng ^= (0x9E3779B97F4A7C15ULL + (uint64_t)auto_recomp.scenario);
                            auto_recomp.last_progress_frame = auto_recomp.total_frames;

                            if (!reset_runtime_state(loaded_rom, &memory, &cpu, &ppu, false))
                            {
                                running = false;
                            }
                            throttle_epoch = SDL_GetPerformanceCounter();
                            emulated_mcycles = 0.0;
                        }
                    }
                }
            }
            else if (auto_start)
            {
                if (frame_index == 120)
                {
                    memory_set_button_state(&memory, JOYPAD_START, true);
                }
                else if (frame_index == 240)
                {
                    memory_set_button_state(&memory, JOYPAD_START, false);
                }
            }

            if (display.enabled)
            {
                display_present(&display, &ppu);
                if (fps_diag_enabled)
                {
                    fps_diag_presented_frames++;
                }
            }

            ppu.frame_ready = false;
        }

        if (fps_diag_enabled)
        {
            uint64_t now = SDL_GetPerformanceCounter();
            uint64_t elapsed_ticks = now - fps_diag_last_tick;
            if (elapsed_ticks >= fps_diag_interval_ticks)
            {
                double elapsed_s = (double)elapsed_ticks / (double)perf_freq;
                double emu_fps = (double)fps_diag_emu_frames / elapsed_s;
                double presented_fps = (double)fps_diag_presented_frames / elapsed_s;
                double mcycles_per_s = (double)fps_diag_mcycles / elapsed_s;
                double speed_native_pct = (mcycles_per_s / 1048576.0) * 100.0;
                double speed_target_pct = (mcycles_per_s / target_mcycles_per_sec) * 100.0;
                double mcycles_per_frame = fps_diag_emu_frames ? ((double)fps_diag_mcycles / (double)fps_diag_emu_frames) : 0.0;
                double lcd_on_pct = fps_diag_samples ? ((double)fps_diag_lcdc_on_samples * 100.0 / (double)fps_diag_samples) : 0.0;

                printf("[FPS-DIAG] dt=%.3fs emu_fps=%.2f present_fps=%.2f mcycles/s=%.0f mcycles/frame=%.0f speed_native=%.1f%% speed_target=%.1f%% lcd_on=%.1f%% lcdc=%02X stat=%02X ly=%02X\n",
                       elapsed_s,
                       emu_fps,
                       presented_fps,
                       mcycles_per_s,
                       mcycles_per_frame,
                       speed_native_pct,
                       speed_target_pct,
                       lcd_on_pct,
                       memory.memory.LCDC,
                       memory.memory.STAT,
                       memory.memory.LY);

                fps_diag_last_tick = now;
                fps_diag_emu_frames = 0;
                fps_diag_presented_frames = 0;
                fps_diag_mcycles = 0;
                fps_diag_lcdc_on_samples = 0;
                fps_diag_samples = 0;
            }
        }

        if (auto_recomp.enabled)
        {
            uint64_t now = SDL_GetPerformanceCounter();
            if ((now - status_last_tick) >= perf_freq)
            {
                uint64_t elapsed_seconds = (now - run_start_tick) / perf_freq;
                print_updating_status_line(elapsed_seconds, &auto_recomp, &status_line_len);
                status_last_tick = now;
            }
        }
    }

    if (auto_recomp.enabled)
    {
        if (!auto_recomp.heuristic_enabled)
        {
            auto_recomp_apply_mask(&memory, &auto_recomp, 0);
        }
        uint64_t end_tick = SDL_GetPerformanceCounter();
        uint64_t elapsed_seconds = (end_tick - run_start_tick) / perf_freq;
        RecompProbeStats stats = {0};
        recomp_probe_get_stats(&stats);
        double pct = stats.functions_discovered ? (100.0 * (double)stats.functions_processed / (double)stats.functions_discovered) : 0.0;
        if (pct > 100.0)
            pct = 100.0;

        if (status_line_len > 0)
        {
            printf("\n");
        }
        if (!quiet_auto_recomp_logs)
        {
            char tbuf[32];
            format_elapsed_hms(elapsed_seconds, tbuf, sizeof(tbuf));
            printf("[AUTO-RECOMP] done t=%s funcs=%zu branches=%zu paths=%zu progress=%.1f%% spawned=%llu done=%llu splits=%llu queued=%zu\n",
                   tbuf,
                   stats.functions_discovered,
                   stats.branches_discovered,
                   stats.paths_discovered,
                   pct,
                   (unsigned long long)auto_recomp.paths_spawned,
                   (unsigned long long)auto_recomp.paths_completed,
                   (unsigned long long)auto_recomp.branch_splits,
                   auto_recomp.snapshot_count);
        }
        auto_recomp_release(&auto_recomp);
    }
    display_shutdown(&display);
    recomp_probe_shutdown();
    memory_shutdown(&memory);

    return 0;
}

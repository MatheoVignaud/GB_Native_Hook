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
    bool show_help;
    bool auto_recomp_random;
    bool auto_recomp_cascade;
    bool auto_recomp_force_display;
    uint32_t auto_recomp_random_frames;
    const char *static_recomp_dump_dir;
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

static bool file_exists_local(const char *path)
{
    if (!path || path[0] == '\0')
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

static bool set_env_local(const char *name, const char *value, bool overwrite)
{
    if (!name || name[0] == '\0' || !value)
        return false;
    if (!overwrite)
    {
        const char *cur = getenv(name);
        if (cur && cur[0] != '\0')
            return true;
    }
#if defined(_WIN32)
    return _putenv_s(name, value) == 0;
#else
    return setenv(name, value, overwrite ? 1 : 0) == 0;
#endif
}

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

static uint8_t auto_recomp_mask_from_memory(const MemoryState *memory)
{
    if (!memory)
        return 0u;

    uint8_t mask = 0u;
    if ((memory->joypad_dpad & 0x01u) == 0u)
        mask |= (uint8_t)(1u << JOYPAD_RIGHT);
    if ((memory->joypad_dpad & 0x02u) == 0u)
        mask |= (uint8_t)(1u << JOYPAD_LEFT);
    if ((memory->joypad_dpad & 0x04u) == 0u)
        mask |= (uint8_t)(1u << JOYPAD_UP);
    if ((memory->joypad_dpad & 0x08u) == 0u)
        mask |= (uint8_t)(1u << JOYPAD_DOWN);

    if ((memory->joypad_buttons & 0x01u) == 0u)
        mask |= (uint8_t)(1u << JOYPAD_A);
    if ((memory->joypad_buttons & 0x02u) == 0u)
        mask |= (uint8_t)(1u << JOYPAD_B);
    if ((memory->joypad_buttons & 0x04u) == 0u)
        mask |= (uint8_t)(1u << JOYPAD_SELECT);
    if ((memory->joypad_buttons & 0x08u) == 0u)
        mask |= (uint8_t)(1u << JOYPAD_START);

    return mask;
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

static size_t auto_cascade_build_input_candidates(uint64_t total_frames, uint8_t *out_masks, size_t out_cap)
{
    if (!out_masks || out_cap == 0)
        return 0;

    const uint8_t M_RIGHT = (uint8_t)(1u << JOYPAD_RIGHT);
    const uint8_t M_LEFT = (uint8_t)(1u << JOYPAD_LEFT);
    const uint8_t M_UP = (uint8_t)(1u << JOYPAD_UP);
    const uint8_t M_DOWN = (uint8_t)(1u << JOYPAD_DOWN);
    const uint8_t M_A = (uint8_t)(1u << JOYPAD_A);
    const uint8_t M_B = (uint8_t)(1u << JOYPAD_B);
    const uint8_t M_SELECT = (uint8_t)(1u << JOYPAD_SELECT);
    const uint8_t M_START = (uint8_t)(1u << JOYPAD_START);

    uint8_t preferred = 0u;
    if ((total_frames >= 90u && total_frames < 150u) ||
        (total_frames >= 210u && total_frames < 270u) ||
        (total_frames >= 420u && total_frames < 480u))
    {
        preferred = M_START;
    }
    else if ((total_frames >= 300u && total_frames < 336u) ||
             (total_frames >= 520u && total_frames < 556u))
    {
        preferred = M_A;
    }

    const uint8_t base_masks[] = {
        0u,
        M_START,
        M_A,
        M_B,
        M_UP,
        M_DOWN,
        M_LEFT,
        M_RIGHT,
        M_SELECT,
        (uint8_t)(M_A | M_B),
        (uint8_t)(M_A | M_START),
        (uint8_t)(M_DOWN | M_A),
        (uint8_t)(M_UP | M_A),
    };

    size_t count = 0;
    if (preferred != 0u)
    {
        out_masks[count++] = preferred;
        if (count >= out_cap)
            return count;
    }

    for (size_t i = 0; i < (sizeof(base_masks) / sizeof(base_masks[0])); ++i)
    {
        uint8_t m = base_masks[i];
        bool dup = false;
        for (size_t j = 0; j < count; ++j)
        {
            if (out_masks[j] == m)
            {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        out_masks[count++] = m;
        if (count >= out_cap)
            break;
    }
    return count;
}

static size_t auto_cascade_enqueue_input_variants(AutoRecompState *auto_recomp,
                                                  CPUState *cpu,
                                                  PPUState *ppu,
                                                  MemoryState *memory,
                                                  const uint8_t *masks,
                                                  size_t mask_count,
                                                  size_t primary_index,
                                                  uint32_t max_alt_variants)
{
    if (!auto_recomp || !cpu || !ppu || !memory || !masks || mask_count == 0 || max_alt_variants == 0u)
        return 0;
    if (!auto_recomp->enabled || !auto_recomp->heuristic_enabled)
        return 0;
    if (auto_recomp->snapshot_count >= auto_recomp->max_clones)
        return 0;

    AutoPathSnapshot base;
    if (!auto_snapshot_capture(&base, cpu, ppu, memory))
        return 0;

    const uint8_t original_mask = auto_recomp->current_mask;
    size_t enqueued = 0;

    for (size_t step = 1; step < mask_count && enqueued < (size_t)max_alt_variants; ++step)
    {
        size_t idx = (primary_index + step) % mask_count;
        uint8_t mask = masks[idx];

        auto_snapshot_restore(&base, cpu, ppu, memory);
        auto_recomp->current_mask = original_mask;
        auto_recomp_apply_mask(memory, auto_recomp, mask);

        AutoPathSnapshot snap;
        if (!auto_snapshot_capture(&snap, cpu, ppu, memory))
            continue;
        if (auto_recomp_push_snapshot(auto_recomp, &snap))
        {
            enqueued++;
        }
    }

    auto_snapshot_restore(&base, cpu, ppu, memory);
    auto_recomp->current_mask = original_mask;
    auto_snapshot_free(&base);
    return enqueued;
}

static void auto_cascade_step_inputs(AutoRecompState *auto_recomp,
                                     CPUState *cpu,
                                     PPUState *ppu,
                                     MemoryState *memory,
                                     uint32_t decision_period_frames,
                                     uint32_t hold_frames,
                                     uint32_t alt_variants_per_decision)
{
    if (!auto_recomp || !cpu || !ppu || !memory || !auto_recomp->enabled || !auto_recomp->heuristic_enabled)
        return;

    if (decision_period_frames == 0u)
        decision_period_frames = 1u;
    if (hold_frames == 0u)
        hold_frames = 1u;
    if (hold_frames > decision_period_frames)
        hold_frames = decision_period_frames;

    uint8_t candidate_masks[16];
    size_t candidate_count = auto_cascade_build_input_candidates(auto_recomp->total_frames, candidate_masks, 16u);
    if (candidate_count == 0u)
    {
        auto_recomp_apply_mask(memory, auto_recomp, 0u);
        return;
    }

    bool decision_frame = ((auto_recomp->total_frames % (uint64_t)decision_period_frames) == 0u);
    if (decision_frame)
    {
        uint64_t decision_index = (auto_recomp->total_frames / (uint64_t)decision_period_frames) + auto_recomp->paths_completed;
        size_t primary_index = (size_t)(decision_index % (uint64_t)candidate_count);

        if (alt_variants_per_decision > 0u)
        {
            (void)auto_cascade_enqueue_input_variants(auto_recomp,
                                                      cpu,
                                                      ppu,
                                                      memory,
                                                      candidate_masks,
                                                      candidate_count,
                                                      primary_index,
                                                      alt_variants_per_decision);
        }

        auto_recomp->hold_frames_left = hold_frames;
        auto_recomp_apply_mask(memory, auto_recomp, candidate_masks[primary_index]);
        return;
    }

    if (auto_recomp->hold_frames_left > 1u)
    {
        auto_recomp->hold_frames_left--;
        auto_recomp_apply_mask(memory, auto_recomp, auto_recomp->current_mask);
    }
    else
    {
        auto_recomp->hold_frames_left = 0u;
        auto_recomp_apply_mask(memory, auto_recomp, 0u);
    }
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

static void print_cascade_progress_log(uint64_t elapsed_seconds,
                                       const AutoRecompState *auto_recomp,
                                       const MemoryState *memory)
{
    if (!auto_recomp || !auto_recomp->enabled || !memory)
        return;

    RecompProbeStats stats = {0};
    recomp_probe_get_stats(&stats);

    size_t rom_reads = memory_get_rom_read_unique_count();
    size_t rom_size = memory_get_rom_read_track_size();
    double rom_reads_pct = (rom_size > 0) ? (100.0 * (double)rom_reads / (double)rom_size) : 0.0;

    size_t dyn_unique_pcs = recomp_probe_dyn_dump_unique_starts_count();
    uint64_t dyn_dumps = recomp_probe_dyn_dump_count();

    char tbuf[32];
    format_elapsed_hms(elapsed_seconds, tbuf, sizeof(tbuf));

    printf("[AUTO-CASCADE] t=%s funcs=%zu proc=%zu branches=%zu paths=%zu rom_reads=%zu/%zu(%.2f%%) dyn_pcs=%zu dyn_dumps=%llu queued=%zu spawned=%llu done=%llu splits=%llu path_steps=%u total_steps=%llu frames=%llu\n",
           tbuf,
           stats.functions_discovered,
           stats.functions_processed,
           stats.branches_discovered,
           stats.paths_discovered,
           rom_reads,
           rom_size,
           rom_reads_pct,
           dyn_unique_pcs,
           (unsigned long long)dyn_dumps,
           auto_recomp->snapshot_count,
           (unsigned long long)auto_recomp->paths_spawned,
           (unsigned long long)auto_recomp->paths_completed,
           (unsigned long long)auto_recomp->branch_splits,
           auto_recomp->path_step_budget,
           (unsigned long long)auto_recomp->total_path_steps,
           (unsigned long long)auto_recomp->total_frames);
    fflush(stdout);
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
    auto_recomp->current_mask = auto_recomp_mask_from_memory(memory);
    auto_recomp->hold_frames_left = 0u;
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

static bool seed_static_recomp_from_dump_dir(const char *dump_dir,
                                             size_t *out_candidates,
                                             size_t *out_seeded_increase)
{
    if (out_candidates)
        *out_candidates = 0;
    if (out_seeded_increase)
        *out_seeded_increase = 0;
    if (!dump_dir || dump_dir[0] == '\0')
        return false;

    DIR *dir = opendir(dump_dir);
    if (!dir)
        return false;

    size_t before = recomp_probe_discovered_count();
    size_t candidates = 0;
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL)
    {
        const char *name = ent->d_name;
        if (!name || name[0] == '\0')
            continue;

        unsigned bank = 0;
        unsigned addr = 0;
        int nconsumed = 0;
        if (sscanf(name, "func_b%u_%x.json%n", &bank, &addr, &nconsumed) == 2 &&
            nconsumed > 0 &&
            name[nconsumed] == '\0' &&
            addr < 0x8000u)
        {
            recomp_probe_seed_entry_bank((size_t)bank, (uint16_t)addr);
            candidates++;
        }
    }
    closedir(dir);

    if (out_candidates)
        *out_candidates = candidates;
    if (out_seeded_increase)
    {
        size_t after = recomp_probe_discovered_count();
        *out_seeded_increase = (after >= before) ? (after - before) : 0;
    }
    return true;
}

static void print_usage(const char *argv0)
{
    const char *prog = (argv0 && argv0[0] != '\0') ? argv0 : "GB_Native_Hook";
    printf("Usage: %s [options] [rom_path]\n", prog);
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help       Show this help and exit\n");
    printf("  --display        Enable SDL display window (default)\n");
    printf("  --no-display     Disable SDL display window\n");
    printf("  --auto-recomp    Run auto-recomp exploration mode\n");
    printf("  --auto-recomp-cascade  Algorithmic branch/snapshot exploration with long-run coverage logging\n");
    printf("  --auto-recomp-random  Run auto-recomp in random-input mode (heuristic off)\n");
    printf("  --auto-recomp-random-frames <n>  Random-input duration in frames (also sets max_scenarios=1)\n");
    printf("  --auto-recomp-display  Keep display enabled during auto/static recomp (CLI override)\n");
    printf("  --static-recomp  Run static reachability analysis and exit\n");
    printf("  --static-recomp-from-dumps <dir>  Seed static walk from func_bXXX_YYYY.json in <dir>\n");
    printf("\n");
    printf("Notes:\n");
    printf("  - If no ROM is provided, defaults to Pokemon.gb\n");
    printf("  - Some behavior can be controlled with env vars (GB_AUTO_RECOMP_*, GB_STATIC_RECOMP, etc.)\n");
}

static const char *parse_arguments(int argc, char **argv, AppOptions *options)
{
    const char *rom_path = NULL;
    options->display_enabled = true;
    options->auto_recomp = false;
    options->static_recomp = false;
    options->show_help = false;
    options->auto_recomp_random = false;
    options->auto_recomp_cascade = false;
    options->auto_recomp_force_display = false;
    options->auto_recomp_random_frames = 0;
    options->static_recomp_dump_dir = NULL;

    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            options->show_help = true;
        }
        else if (strcmp(argv[i], "--no-display") == 0)
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
        else if (strcmp(argv[i], "--auto-recomp-cascade") == 0)
        {
            options->auto_recomp = true;
            options->auto_recomp_cascade = true;
        }
        else if (strcmp(argv[i], "--auto-recomp-random") == 0)
        {
            options->auto_recomp = true;
            options->auto_recomp_random = true;
        }
        else if (strcmp(argv[i], "--auto-recomp-random-frames") == 0 && (i + 1) < argc)
        {
            unsigned long v = strtoul(argv[++i], NULL, 10);
            if (v == 0 || v > 0xFFFFFFFFul)
            {
                fprintf(stderr, "Invalid value for --auto-recomp-random-frames: %s\n", argv[i]);
            }
            else
            {
                options->auto_recomp = true;
                options->auto_recomp_random = true;
                options->auto_recomp_random_frames = (uint32_t)v;
            }
        }
        else if (strcmp(argv[i], "--auto-recomp-display") == 0)
        {
            options->display_enabled = true;
            options->auto_recomp_force_display = true;
        }
        else if (strcmp(argv[i], "--static-recomp") == 0)
        {
            options->static_recomp = true;
        }
        else if (strcmp(argv[i], "--static-recomp-from-dumps") == 0 && (i + 1) < argc)
        {
            options->static_recomp = true;
            options->static_recomp_dump_dir = argv[++i];
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
    AppOptions options = {0};
    const char *rom_path = parse_arguments(argc, argv, &options);
    if (options.show_help)
    {
        print_usage(argc > 0 ? argv[0] : "GB_Native_Hook");
        return 0;
    }

    bool auto_recomp_cascade_requested = options.auto_recomp_cascade;
    bool auto_recomp_random_requested = options.auto_recomp_random;
    char auto_recomp_cascade_seed_dir[512] = {0};
    char auto_recomp_cascade_trace_path[512] = {0};
    char auto_recomp_random_trace_path[512] = {0};
    if (auto_recomp_cascade_requested || auto_recomp_random_requested)
    {
        const char *candidate_rom = rom_path;
        if (!candidate_rom || candidate_rom[0] == '\0' || !file_exists_local(candidate_rom))
        {
            candidate_rom = "Pokemon.gb";
        }

        char stem[260] = {0};
        path_to_stem_local(candidate_rom, stem, sizeof(stem));
        if (stem[0] != '\0')
        {
            (void)ensure_dir_if_needed(stem);
            if (auto_recomp_cascade_requested)
            {
                snprintf(auto_recomp_cascade_seed_dir, sizeof(auto_recomp_cascade_seed_dir), "%s", stem);
                snprintf(auto_recomp_cascade_trace_path, sizeof(auto_recomp_cascade_trace_path), "%s/rom_reads.txt", stem);
                (void)set_env_local("GB_ROM_READ_TRACE_PATH", auto_recomp_cascade_trace_path, false);
            }
            if (auto_recomp_random_requested)
            {
                snprintf(auto_recomp_random_trace_path, sizeof(auto_recomp_random_trace_path), "%s/rom_reads.txt", stem);
                (void)set_env_local("GB_ROM_READ_TRACE_PATH", auto_recomp_random_trace_path, false);
            }
        }

        if (auto_recomp_cascade_requested)
        {
            (void)set_env_local("GB_RECOMP_PROBE", "1", false);
            (void)set_env_local("GB_RECOMP_INPUT_PROFILE", "1", false);
            (void)set_env_local("GB_RECOMP_INPUT_SNAPSHOTS", "1", false);
            (void)set_env_local("GB_RECOMP_INPUT_SAMPLES_MAX", "16", false);
            (void)set_env_local("GB_RECOMP_NO_DOT", "1", false);
        }
    }

    MemoryState memory;
    memory_init(&memory);

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
    if ((options.auto_recomp || options.static_recomp) &&
        !options.auto_recomp_force_display &&
        !read_env_enabled("GB_AUTO_RECOMP_DISPLAY"))
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
    recomp_probe_dyn_dump_init(loaded_rom, &memory);
    cpu.memory = &memory;
    memory.cpu = &cpu;
    ppu.mem = &memory.memory;
    cpu_reset(&cpu);
    ppu_reset(&ppu, memory.bios_enabled);

    if (options.auto_recomp_cascade && !options.static_recomp)
    {
        char stem[260] = {0};
        path_to_stem_local(loaded_rom, stem, sizeof(stem));
        if (stem[0] != '\0')
        {
            snprintf(auto_recomp_cascade_seed_dir, sizeof(auto_recomp_cascade_seed_dir), "%s", stem);
        }

        uint64_t static_t0 = SDL_GetPerformanceCounter();
        size_t seed_candidates = 0;
        size_t seed_new = 0;
        bool seed_ok = false;
        if (auto_recomp_cascade_seed_dir[0] != '\0')
        {
            seed_ok = seed_static_recomp_from_dump_dir(auto_recomp_cascade_seed_dir, &seed_candidates, &seed_new);
        }
        if (seed_ok)
        {
            printf("[AUTO-CASCADE] static seed from dumps dir=%s candidates=%zu new=%zu\n",
                   auto_recomp_cascade_seed_dir,
                   seed_candidates,
                   seed_new);
        }
        else if (auto_recomp_cascade_seed_dir[0] != '\0')
        {
            printf("[AUTO-CASCADE] no dump seed dir found (expected %s), continuing without seed\n",
                   auto_recomp_cascade_seed_dir);
        }

        recomp_probe_run_static_reachability(cpu.PC, true, true);
        uint64_t static_t1 = SDL_GetPerformanceCounter();
        uint64_t pf = SDL_GetPerformanceFrequency();
        if (pf == 0)
            pf = 1;
        RecompProbeStats pre_stats = {0};
        recomp_probe_get_stats(&pre_stats);
        printf("[AUTO-CASCADE] static prepass done dt=%.2fs funcs=%zu branches=%zu paths=%zu\n",
               (double)(static_t1 - static_t0) / (double)pf,
               pre_stats.functions_discovered,
               pre_stats.branches_discovered,
               pre_stats.paths_discovered);
    }

    if (options.static_recomp)
    {
        if (options.static_recomp_dump_dir && options.static_recomp_dump_dir[0] != '\0')
        {
            size_t seed_candidates = 0;
            size_t seed_increase = 0;
            if (!seed_static_recomp_from_dump_dir(options.static_recomp_dump_dir, &seed_candidates, &seed_increase))
            {
                fprintf(stderr, "[STATIC-RECOMP] failed to open dump dir: %s\n", options.static_recomp_dump_dir);
            }
            else
            {
                printf("[STATIC-RECOMP] seeded from dumps dir=%s candidates=%zu new=%zu\n",
                       options.static_recomp_dump_dir,
                       seed_candidates,
                       seed_increase);
            }
        }

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
    if ((options.auto_recomp_cascade || options.auto_recomp_random) && display.enabled && display.renderer)
    {
        (void)SDL_SetRenderVSync(display.renderer, false);
    }

    AutoRecompState auto_recomp = {0};
    uint32_t auto_cascade_warmup_frames = 0u;
    uint32_t auto_cascade_input_decision_period_frames = 12u;
    uint32_t auto_cascade_input_hold_frames = 4u;
    uint32_t auto_cascade_input_alt_variants = 2u;
    auto_recomp_init(&auto_recomp, options.auto_recomp);
    if (auto_recomp.enabled && options.auto_recomp_random)
    {
        auto_recomp.heuristic_enabled = false;
        if (options.auto_recomp_random_frames > 0)
        {
            auto_recomp.frames_per_scenario = options.auto_recomp_random_frames;
            auto_recomp.max_scenarios = 1u;
            auto_recomp.stagnation_frames = 0u; // honor explicit duration unless max_scenarios/env says otherwise
        }
    }
    if (auto_recomp.enabled && options.auto_recomp_cascade)
    {
        auto_recomp.heuristic_enabled = true;
        auto_recomp.total_step_budget = 0u; // cascade mode is intended for multi-hour/day runs
        {
            uint32_t cascade_path_steps = read_env_u32("GB_AUTO_CASCADE_PATH_STEPS", 10000000u);
            uint32_t cascade_max_clones = read_env_u32("GB_AUTO_CASCADE_MAX_CLONES", 512u);
            auto_cascade_warmup_frames = read_env_u32("GB_AUTO_CASCADE_WARMUP_FRAMES", 1200u);
            auto_cascade_input_decision_period_frames = read_env_u32("GB_AUTO_CASCADE_INPUT_PERIOD_FRAMES", 12u);
            auto_cascade_input_hold_frames = read_env_u32("GB_AUTO_CASCADE_INPUT_HOLD_FRAMES", 4u);
            auto_cascade_input_alt_variants = read_env_u32("GB_AUTO_CASCADE_INPUT_ALT_VARIANTS", 2u);
            if (cascade_path_steps > 0u)
            {
                auto_recomp.path_step_budget = cascade_path_steps;
            }
            if (cascade_max_clones > auto_recomp.max_clones)
            {
                auto_recomp.max_clones = cascade_max_clones;
            }
            if (auto_cascade_input_decision_period_frames == 0u)
                auto_cascade_input_decision_period_frames = 1u;
            if (auto_cascade_input_hold_frames == 0u)
                auto_cascade_input_hold_frames = 1u;
        }
    }
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
            if (options.auto_recomp_random)
            {
                printf("[AUTO-RECOMP] random-input mode enabled frames_per_scenario=%u max_scenarios=%u display=%s\n",
                       auto_recomp.frames_per_scenario,
                       auto_recomp.max_scenarios,
                       display.enabled ? "on" : "off");
            }
        }
        if (options.auto_recomp_cascade)
        {
            printf("[AUTO-CASCADE] mode=algorithmic-tree random_inputs=off total_limit=unlimited path_steps=%u warmup_frames=%u max_clones=%u input_period=%u input_hold=%u input_alt=%u display=%s trace=%s\n",
                   auto_recomp.path_step_budget,
                   auto_cascade_warmup_frames,
                   auto_recomp.max_clones,
                   auto_cascade_input_decision_period_frames,
                   auto_cascade_input_hold_frames,
                   auto_cascade_input_alt_variants,
                   display.enabled ? "on" : "off",
                   auto_recomp_cascade_trace_path[0] ? auto_recomp_cascade_trace_path : "(env/default)");
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
    uint64_t auto_recomp_status_interval_ticks = options.auto_recomp_cascade ? (perf_freq * 2u) : perf_freq;
    if (auto_recomp_status_interval_ticks == 0)
        auto_recomp_status_interval_ticks = 1;
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
            if (options.auto_recomp_cascade &&
                auto_cascade_warmup_frames > 0u &&
                auto_recomp.total_frames < (uint64_t)auto_cascade_warmup_frames)
            {
                path_budget_hit = false;
            }
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
                    if (options.auto_recomp_cascade)
                    {
                        auto_cascade_step_inputs(&auto_recomp,
                                                 &cpu,
                                                 &ppu,
                                                 &memory,
                                                 auto_cascade_input_decision_period_frames,
                                                 auto_cascade_input_hold_frames,
                                                 auto_cascade_input_alt_variants);
                    }
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
            if ((now - status_last_tick) >= auto_recomp_status_interval_ticks)
            {
                uint64_t elapsed_seconds = (now - run_start_tick) / perf_freq;
                if (options.auto_recomp_cascade)
                {
                    if (status_line_len > 0)
                    {
                        printf("\n");
                        status_line_len = 0;
                    }
                    print_cascade_progress_log(elapsed_seconds, &auto_recomp, &memory);
                    memory_flush_rom_read_trace();
                }
                else
                {
                    print_updating_status_line(elapsed_seconds, &auto_recomp, &status_line_len);
                    if (options.auto_recomp_random)
                    {
                        memory_flush_rom_read_trace();
                    }
                }
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

#include "RecompProbe.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(path) mkdir(path, 0755)
#endif

typedef struct
{
    size_t bank;
    uint16_t addr;
} FunctionKey;

typedef struct
{
    uint16_t address;
    uint32_t read_count;
    uint8_t value_bits[32]; // 256-bit set: values seen at this address
    uint8_t rom_bank_bits[64]; // up to 512 ROM banks (MBC5)
} InputReadProfile;

typedef struct
{
    uint16_t address;
    uint8_t value;
} InputSampleReadEvent;

typedef struct
{
    uint16_t pc;
    uint16_t sp;
    uint16_t af;
    uint16_t bc;
    uint16_t de;
    uint16_t hl;
    uint64_t cycle_count;
    uint32_t total_reads;
    bool truncated;
    InputSampleReadEvent *events;
    size_t event_count;
} FunctionInputSample;

typedef struct
{
    FunctionKey function;
    InputReadProfile *reads;
    size_t read_count;
    size_t read_cap;
    uint64_t total_reads;
} FunctionInputProfile;

typedef struct
{
    FunctionKey function;
    FunctionInputSample *samples;
    size_t sample_count;
    size_t sample_cap;
} FunctionInputSampleSet;

typedef struct
{
    bool active;
    FunctionKey function;
    uint16_t pc;
    uint16_t sp;
    uint16_t af;
    uint16_t bc;
    uint16_t de;
    uint16_t hl;
    uint64_t cycle_count;
    uint32_t total_reads;
    bool truncated;
    InputSampleReadEvent events[128];
    size_t event_count;
} ActiveFunctionInputSample;

typedef struct
{
    size_t bank;
    uint16_t addr;
    const char *reason;
} PendingFunction;

typedef struct
{
    bool enabled;
    bool force_regen;
    bool follow_static_calls;
    bool export_dot;
    bool logging_enabled;
    MemoryState *mem;
    char output_dir[512];
    char rom_name[260];
    FILE *global_cfg_dot;
    bool global_cfg_opened;

    FunctionKey *seen;
    size_t seen_count;
    size_t seen_cap;
    PendingFunction *pending;
    size_t pending_count;
    size_t pending_cap;
    bool processing_pending;
    size_t processed_count;
    size_t branch_count;
    size_t path_count;

    bool profile_inputs;
    bool profile_input_snapshots;
    size_t input_samples_max_per_function;
    FunctionKey exec_stack[256];
    ActiveFunctionInputSample exec_sample_stack[256];
    size_t exec_stack_depth;
    FunctionInputProfile *input_profiles;
    size_t input_profiles_count;
    size_t input_profiles_cap;
    FunctionInputSampleSet *input_sample_sets;
    size_t input_sample_sets_count;
    size_t input_sample_sets_cap;
} ProbeState;

typedef struct
{
    uint8_t len;
    bool stop;
    char text[96];
} DecodedLine;

typedef enum
{
    CFG_EDGE_JUMP = 0,
    CFG_EDGE_BRANCH_TRUE,
    CFG_EDGE_BRANCH_FALSE,
    CFG_EDGE_FALLTHROUGH
} CfgEdgeKind;

typedef struct
{
    uint16_t from_block;
    uint16_t to;
    size_t to_bank;
    CfgEdgeKind kind;
} CfgEdge;

typedef struct
{
    uint16_t at;
    uint16_t target;
    size_t target_bank;
    bool conditional;
    bool rst;
} CallSite;

typedef struct
{
    CfgEdge *items;
    size_t count;
    size_t cap;
} CfgEdgeVec;

typedef struct
{
    CallSite *items;
    size_t count;
    size_t cap;
} CallSiteVec;

static ProbeState g_probe = {0};
static bool g_probe_log_enabled = true;
typedef struct
{
    bool enabled;
    MemoryState *mem;
    char output_dir[512];
    uint8_t dumped_start[8192];
    uint32_t dumped_sig[65536];
} DynDumpState;
static DynDumpState g_dyn_dump = {0};

static const char *reg8_names[8] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
static const char *reg16_names[4] = {"BC", "DE", "HL", "SP"};
static const char *reg16_stack_names[4] = {"BC", "DE", "HL", "AF"};
static const char *cond_names[4] = {"NZ", "Z", "NC", "C"};
static const char *alu_names[8] = {"ADD A,", "ADC A,", "SUB", "SBC A,", "AND", "XOR", "OR", "CP"};
static const char *cb_rot_names[8] = {"RLC", "RRC", "RL", "RR", "SLA", "SRA", "SWAP", "SRL"};

static bool dyn_same_region(uint16_t a, uint16_t b);
static size_t effective_bank(const MemoryState *mem, uint16_t address);

static bool ensure_dir(const char *path)
{
    if (!path || path[0] == '\0')
        return false;

    if (MKDIR(path) == 0)
        return true;
    if (errno == EEXIST)
        return true;
    return false;
}

static bool env_flag_enabled(const char *name)
{
    const char *value = getenv(name);
    return value && value[0] != '\0' && value[0] != '0';
}

static bool file_exists(const char *path)
{
    if (!path || path[0] == '\0')
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

static bool function_key_equal(FunctionKey a, FunctionKey b)
{
    return a.bank == b.bank && a.addr == b.addr;
}

static void probe_exec_stack_pop(void);
static void probe_exec_sample_start(CPUState *cpu, FunctionKey key);
static void probe_exec_sample_record_read(uint16_t address, uint8_t value);

static void probe_exec_stack_clear(void)
{
    while (g_probe.exec_stack_depth > 0u)
        probe_exec_stack_pop();
}

static void probe_exec_stack_push(FunctionKey key)
{
    if (key.addr >= 0x8000u)
        return;
    if (g_probe.exec_stack_depth < (sizeof(g_probe.exec_stack) / sizeof(g_probe.exec_stack[0])))
    {
        g_probe.exec_stack[g_probe.exec_stack_depth++] = key;
        return;
    }
    memmove(&g_probe.exec_stack[0],
            &g_probe.exec_stack[1],
            (g_probe.exec_stack_depth - 1u) * sizeof(g_probe.exec_stack[0]));
    if (g_probe.profile_input_snapshots && g_probe.exec_stack_depth > 1u)
    {
        memmove(&g_probe.exec_sample_stack[0],
                &g_probe.exec_sample_stack[1],
                (g_probe.exec_stack_depth - 1u) * sizeof(g_probe.exec_sample_stack[0]));
    }
    g_probe.exec_stack[g_probe.exec_stack_depth - 1u] = key;
}

static void probe_exec_stack_pop(void)
{
    if (g_probe.profile_input_snapshots && g_probe.exec_stack_depth > 0u)
    {
        ActiveFunctionInputSample *active = &g_probe.exec_sample_stack[g_probe.exec_stack_depth - 1u];
        if (active->active)
        {
            FunctionInputSampleSet *set = NULL;
            for (size_t i = 0; i < g_probe.input_sample_sets_count; ++i)
            {
                if (function_key_equal(g_probe.input_sample_sets[i].function, active->function))
                {
                    set = &g_probe.input_sample_sets[i];
                    break;
                }
            }
            if (!set)
            {
                if (g_probe.input_sample_sets_count == g_probe.input_sample_sets_cap)
                {
                    size_t next_cap = (g_probe.input_sample_sets_cap == 0u) ? 64u : (g_probe.input_sample_sets_cap * 2u);
                    FunctionInputSampleSet *next = (FunctionInputSampleSet *)realloc(
                        g_probe.input_sample_sets, next_cap * sizeof(FunctionInputSampleSet));
                    if (next)
                    {
                        g_probe.input_sample_sets = next;
                        g_probe.input_sample_sets_cap = next_cap;
                    }
                }
                if (g_probe.input_sample_sets_count < g_probe.input_sample_sets_cap)
                {
                    set = &g_probe.input_sample_sets[g_probe.input_sample_sets_count++];
                    memset(set, 0, sizeof(*set));
                    set->function = active->function;
                }
            }

            if (set && (g_probe.input_samples_max_per_function == 0u || set->sample_count < g_probe.input_samples_max_per_function))
            {
                if (set->sample_count == set->sample_cap)
                {
                    size_t next_cap = (set->sample_cap == 0u) ? 4u : (set->sample_cap * 2u);
                    FunctionInputSample *next = (FunctionInputSample *)realloc(
                        set->samples, next_cap * sizeof(FunctionInputSample));
                    if (next)
                    {
                        set->samples = next;
                        set->sample_cap = next_cap;
                    }
                }
                if (set->sample_count < set->sample_cap)
                {
                    FunctionInputSample *dst = &set->samples[set->sample_count++];
                    memset(dst, 0, sizeof(*dst));
                    dst->pc = active->pc;
                    dst->sp = active->sp;
                    dst->af = active->af;
                    dst->bc = active->bc;
                    dst->de = active->de;
                    dst->hl = active->hl;
                    dst->cycle_count = active->cycle_count;
                    dst->total_reads = active->total_reads;
                    dst->truncated = active->truncated;
                    dst->event_count = active->event_count;
                    if (active->event_count > 0u)
                    {
                        dst->events = (InputSampleReadEvent *)malloc(active->event_count * sizeof(InputSampleReadEvent));
                        if (dst->events)
                            memcpy(dst->events, active->events, active->event_count * sizeof(InputSampleReadEvent));
                        else
                            dst->event_count = 0u;
                    }
                }
            }
        }
        memset(active, 0, sizeof(*active));
    }
    if (g_probe.exec_stack_depth > 0u)
        g_probe.exec_stack_depth--;
}

static void probe_exec_stack_set_top(FunctionKey key)
{
    if (key.addr >= 0x8000u)
        return;
    if (g_probe.exec_stack_depth == 0u)
    {
        probe_exec_stack_push(key);
        return;
    }
    g_probe.exec_stack[g_probe.exec_stack_depth - 1u] = key;
}

static void probe_exec_sample_start(CPUState *cpu, FunctionKey key)
{
    if (!g_probe.enabled || !g_probe.profile_input_snapshots || !cpu || g_probe.exec_stack_depth == 0u)
        return;
    ActiveFunctionInputSample *active = &g_probe.exec_sample_stack[g_probe.exec_stack_depth - 1u];
    memset(active, 0, sizeof(*active));
    if (key.addr >= 0x8000u)
        return;

    if (g_probe.input_samples_max_per_function > 0u)
    {
        for (size_t i = 0; i < g_probe.input_sample_sets_count; ++i)
        {
            if (function_key_equal(g_probe.input_sample_sets[i].function, key) &&
                g_probe.input_sample_sets[i].sample_count >= g_probe.input_samples_max_per_function)
            {
                return;
            }
        }
    }

    active->active = true;
    active->function = key;
    active->pc = cpu->PC;
    active->sp = cpu->SP;
    active->af = cpu->AF;
    active->bc = cpu->BC;
    active->de = cpu->DE;
    active->hl = cpu->HL;
    active->cycle_count = cpu->cycle_count;
}

static void probe_exec_sample_record_read(uint16_t address, uint8_t value)
{
    if (!g_probe.enabled || !g_probe.profile_input_snapshots || g_probe.exec_stack_depth == 0u)
        return;
    ActiveFunctionInputSample *active = &g_probe.exec_sample_stack[g_probe.exec_stack_depth - 1u];
    if (!active->active)
        return;
    active->total_reads++;
    if (active->event_count >= (sizeof(active->events) / sizeof(active->events[0])))
    {
        active->truncated = true;
        return;
    }
    active->events[active->event_count].address = address;
    active->events[active->event_count].value = value;
    active->event_count++;
}

static FunctionKey probe_current_function_fallback(CPUState *cpu)
{
    FunctionKey fk = {0u, 0u};
    if (!cpu || !cpu->memory)
        return fk;
    uint16_t pc = cpu->PC;
    if (pc >= 0x8000u)
        return fk;
    fk.bank = effective_bank(cpu->memory, pc);
    if (pc < 0x4000u)
        fk.bank = 0u;
    fk.addr = pc;
    return fk;
}

static FunctionInputProfile *probe_get_or_add_function_input_profile(FunctionKey fk)
{
    if (fk.addr >= 0x8000u)
        return NULL;
    for (size_t i = 0; i < g_probe.input_profiles_count; ++i)
    {
        if (function_key_equal(g_probe.input_profiles[i].function, fk))
            return &g_probe.input_profiles[i];
    }
    if (g_probe.input_profiles_count == g_probe.input_profiles_cap)
    {
        size_t next_cap = (g_probe.input_profiles_cap == 0) ? 64u : (g_probe.input_profiles_cap * 2u);
        FunctionInputProfile *next = (FunctionInputProfile *)realloc(
            g_probe.input_profiles, next_cap * sizeof(FunctionInputProfile));
        if (!next)
            return NULL;
        g_probe.input_profiles = next;
        g_probe.input_profiles_cap = next_cap;
    }
    FunctionInputProfile *slot = &g_probe.input_profiles[g_probe.input_profiles_count++];
    memset(slot, 0, sizeof(*slot));
    slot->function = fk;
    return slot;
}

static InputReadProfile *probe_get_or_add_read_profile(FunctionInputProfile *fp, uint16_t address)
{
    if (!fp)
        return NULL;
    for (size_t i = 0; i < fp->read_count; ++i)
    {
        if (fp->reads[i].address == address)
            return &fp->reads[i];
    }
    if (fp->read_count == fp->read_cap)
    {
        size_t next_cap = (fp->read_cap == 0) ? 16u : (fp->read_cap * 2u);
        InputReadProfile *next = (InputReadProfile *)realloc(fp->reads, next_cap * sizeof(InputReadProfile));
        if (!next)
            return NULL;
        fp->reads = next;
        fp->read_cap = next_cap;
    }
    InputReadProfile *slot = &fp->reads[fp->read_count++];
    memset(slot, 0, sizeof(*slot));
    slot->address = address;
    return slot;
}

static const char *probe_input_space_name(uint16_t address)
{
    if (address < 0x4000u)
        return "rom0";
    if (address < 0x8000u)
        return "romx";
    if (address < 0xA000u)
        return "vram";
    if (address < 0xC000u)
        return "eram";
    if (address < 0xE000u)
        return "wram";
    if (address < 0xFE00u)
        return "echo";
    if (address < 0xFEA0u)
        return "oam";
    if (address < 0xFF00u)
        return "unusable";
    if (address < 0xFF80u)
        return "io";
    if (address < 0xFFFFu)
        return "hram";
    return "ie";
}

static size_t probe_wrap_rom_bank(size_t bank, size_t rom_banks)
{
    if (rom_banks == 0u)
        return 0u;
    if (bank >= rom_banks)
        bank %= rom_banks;
    return bank;
}

static bool probe_resolve_rom_bank_for_address(MemoryState *mem, uint16_t address, size_t *out_bank)
{
    if (!mem || !out_bank || address >= 0x8000u)
        return false;

    CartridgeState *cart = &mem->cartridge;
    if (!cart->rom_data || cart->rom_size == 0u)
        return false;

    size_t bank = 0u;
    if (address < 0x4000u)
    {
        switch (cart->mbc_type)
        {
        case MBC1:
            bank = cart->mbc1_mode ? ((size_t)(cart->mbc1_high2 & 0x03u) << 5u) : 0u;
            break;
        default:
            bank = 0u;
            break;
        }
    }
    else
    {
        switch (cart->mbc_type)
        {
        case MBC1:
        {
            uint8_t low = (uint8_t)(cart->mbc1_low5 & 0x1Fu);
            if (low == 0u)
                low = 1u;
            bank = (size_t)low | (((size_t)cart->mbc1_high2 & 0x03u) << 5u);
            break;
        }
        case MBC2:
            bank = (size_t)(cart->mbc2_rom_bank & 0x0Fu);
            if (bank == 0u)
                bank = 1u;
            break;
        case MBC3:
            bank = (size_t)(cart->mbc3_rom_bank & 0x7Fu);
            if (bank == 0u)
                bank = 1u;
            break;
        case MBC5:
            bank = (size_t)(cart->mbc5_rom_bank & 0x01FFu);
            break;
        case MBC_NONE:
        default:
            bank = 1u;
            break;
        }
    }

    *out_bank = probe_wrap_rom_bank(bank, cart->rom_banks);
    return true;
}

static void probe_record_input_read(CPUState *cpu, uint16_t address, uint8_t value)
{
    if (!g_probe.enabled || !cpu || !cpu->memory)
        return;
    if (!g_probe.profile_inputs && !g_probe.profile_input_snapshots)
        return;
    FunctionKey fk = {0u, 0u};
    if (g_probe.exec_stack_depth > 0u)
        fk = g_probe.exec_stack[g_probe.exec_stack_depth - 1u];
    if (fk.addr >= 0x8000u || (fk.addr == 0 && fk.bank == 0 && cpu->PC != 0))
        fk = probe_current_function_fallback(cpu);
    if (fk.addr >= 0x8000u)
        return;

    if (g_probe.profile_inputs)
    {
        FunctionInputProfile *fp = probe_get_or_add_function_input_profile(fk);
        if (!fp)
        {
            probe_exec_sample_record_read(address, value);
            return;
        }
        InputReadProfile *rp = probe_get_or_add_read_profile(fp, address);
        if (!rp)
        {
            probe_exec_sample_record_read(address, value);
            return;
        }

        rp->read_count++;
        fp->total_reads++;
        rp->value_bits[(value >> 3) & 31u] |= (uint8_t)(1u << (value & 7u));
        if (address < 0x8000u)
        {
            size_t rom_bank = 0u;
            if (probe_resolve_rom_bank_for_address(cpu->memory, address, &rom_bank) && rom_bank < 512u)
                rp->rom_bank_bits[rom_bank >> 3] |= (uint8_t)(1u << (rom_bank & 7u));
        }
    }
    probe_exec_sample_record_read(address, value);
}

static int input_read_profile_cmp_addr(const void *a, const void *b)
{
    const InputReadProfile *ra = (const InputReadProfile *)a;
    const InputReadProfile *rb = (const InputReadProfile *)b;
    return (int)ra->address - (int)rb->address;
}

static int function_input_profile_cmp_key(const void *a, const void *b)
{
    const FunctionInputProfile *fa = (const FunctionInputProfile *)a;
    const FunctionInputProfile *fb = (const FunctionInputProfile *)b;
    if (fa->function.bank < fb->function.bank)
        return -1;
    if (fa->function.bank > fb->function.bank)
        return 1;
    return (int)fa->function.addr - (int)fb->function.addr;
}

static int function_input_sample_set_cmp_key(const void *a, const void *b)
{
    const FunctionInputSampleSet *fa = (const FunctionInputSampleSet *)a;
    const FunctionInputSampleSet *fb = (const FunctionInputSampleSet *)b;
    if (fa->function.bank < fb->function.bank)
        return -1;
    if (fa->function.bank > fb->function.bank)
        return 1;
    return (int)fa->function.addr - (int)fb->function.addr;
}

static void probe_write_input_profiles(void)
{
    if (!g_probe.enabled || !g_probe.profile_inputs || g_probe.input_profiles_count == 0u)
        return;

    qsort(g_probe.input_profiles, g_probe.input_profiles_count, sizeof(g_probe.input_profiles[0]), function_input_profile_cmp_key);

    for (size_t i = 0; i < g_probe.input_profiles_count; ++i)
    {
        FunctionInputProfile *fp = &g_probe.input_profiles[i];
        if (fp->read_count == 0u)
            continue;
        qsort(fp->reads, fp->read_count, sizeof(fp->reads[0]), input_read_profile_cmp_addr);

        char path[1024];
        snprintf(path,
                 sizeof(path),
                 "%s/func_b%03zu_%04X.inputs.json",
                 g_probe.output_dir,
                 fp->function.bank,
                 fp->function.addr);
        FILE *f = fopen(path, "w");
        if (!f)
            continue;

        fprintf(f, "{\n");
        fprintf(f, "  \"entry\": {\"bank\": %zu, \"addr\": %u},\n", fp->function.bank, (unsigned)fp->function.addr);
        fprintf(f, "  \"total_reads\": %llu,\n", (unsigned long long)fp->total_reads);
        fprintf(f, "  \"reads\": [\n");
        for (size_t r = 0; r < fp->read_count; ++r)
        {
            InputReadProfile *rp = &fp->reads[r];
            if (r > 0)
                fprintf(f, ",\n");
            unsigned value_count = 0u;
            for (unsigned v = 0u; v < 256u; ++v)
            {
                if (rp->value_bits[(v >> 3) & 31u] & (1u << (v & 7u)))
                    value_count++;
            }
            fprintf(f, "    {\"addr\": %u, \"read_count\": %u, \"value_count\": %u, \"values\": [",
                    (unsigned)rp->address,
                    (unsigned)rp->read_count,
                    value_count);
            bool first = true;
            for (unsigned v = 0u; v < 256u; ++v)
            {
                if ((rp->value_bits[(v >> 3) & 31u] & (1u << (v & 7u))) == 0)
                    continue;
                if (!first)
                    fprintf(f, ", ");
                first = false;
                fprintf(f, "\"%02X\"", v);
            }
            fprintf(f, "], \"space\": \"%s\"", probe_input_space_name(rp->address));
            if (rp->address < 0x8000u)
            {
                bool first_bank = true;
                fprintf(f, ", \"rom_banks\": [");
                if (rp->address < 0x4000u)
                {
                    fprintf(f, "0");
                    first_bank = false;
                }
                for (unsigned b = 0u; b < 512u; ++b)
                {
                    if ((rp->rom_bank_bits[b >> 3] & (1u << (b & 7u))) == 0)
                        continue;
                    if (!first_bank)
                        fprintf(f, ", ");
                    first_bank = false;
                    fprintf(f, "%u", b);
                }
                fprintf(f, "]");

                fprintf(f, ", \"rom_offsets\": [");
                bool first_off = true;
                if (rp->address < 0x4000u)
                {
                    fprintf(f, "%u", (unsigned)rp->address);
                    first_off = false;
                }
                else
                {
                    for (unsigned b = 0u; b < 512u; ++b)
                    {
                        if ((rp->rom_bank_bits[b >> 3] & (1u << (b & 7u))) == 0)
                            continue;
                        unsigned off = (unsigned)(b * 0x4000u + (rp->address & 0x3FFFu));
                        if (!first_off)
                            fprintf(f, ", ");
                        first_off = false;
                        fprintf(f, "%u", off);
                    }
                }
                fprintf(f, "]");
            }
            fprintf(f, "}");
        }
        fprintf(f, "\n  ]\n");
        fprintf(f, "}\n");
        fclose(f);
    }
}

static void probe_write_input_samples(void)
{
    if (!g_probe.enabled || !g_probe.profile_input_snapshots || g_probe.input_sample_sets_count == 0u)
        return;

    qsort(g_probe.input_sample_sets,
          g_probe.input_sample_sets_count,
          sizeof(g_probe.input_sample_sets[0]),
          function_input_sample_set_cmp_key);

    for (size_t i = 0; i < g_probe.input_sample_sets_count; ++i)
    {
        FunctionInputSampleSet *set = &g_probe.input_sample_sets[i];
        if (set->sample_count == 0u)
            continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/func_b%03zu_%04X.samples.json",
                 g_probe.output_dir, set->function.bank, set->function.addr);
        FILE *f = fopen(path, "w");
        if (!f)
            continue;

        fprintf(f, "{\n");
        fprintf(f, "  \"entry\": {\"bank\": %zu, \"addr\": %u},\n", set->function.bank, (unsigned)set->function.addr);
        fprintf(f, "  \"sample_count\": %zu,\n", set->sample_count);
        fprintf(f, "  \"samples\": [\n");
        for (size_t s = 0; s < set->sample_count; ++s)
        {
            FunctionInputSample *sm = &set->samples[s];
            if (s > 0)
                fprintf(f, ",\n");
            fprintf(f, "    {\n");
            fprintf(f, "      \"pc\": %u,\n", (unsigned)sm->pc);
            fprintf(f, "      \"sp\": %u,\n", (unsigned)sm->sp);
            fprintf(f, "      \"af\": %u,\n", (unsigned)sm->af);
            fprintf(f, "      \"bc\": %u,\n", (unsigned)sm->bc);
            fprintf(f, "      \"de\": %u,\n", (unsigned)sm->de);
            fprintf(f, "      \"hl\": %u,\n", (unsigned)sm->hl);
            fprintf(f, "      \"cycle_count\": %llu,\n", (unsigned long long)sm->cycle_count);
            fprintf(f, "      \"total_reads\": %u,\n", (unsigned)sm->total_reads);
            fprintf(f, "      \"truncated\": %s,\n", sm->truncated ? "true" : "false");
            fprintf(f, "      \"reads\": [");
            for (size_t r = 0; r < sm->event_count; ++r)
            {
                if (r > 0)
                    fprintf(f, ", ");
                fprintf(f,
                        "{\"addr\": %u, \"value\": \"%02X\", \"space\": \"%s\"}",
                        (unsigned)sm->events[r].address,
                        (unsigned)sm->events[r].value,
                        probe_input_space_name(sm->events[r].address));
            }
            fprintf(f, "]\n");
            fprintf(f, "    }");
        }
        fprintf(f, "\n  ]\n");
        fprintf(f, "}\n");
        fclose(f);
    }
}

static void function_output_paths(char *asm_path,
                                  size_t asm_path_size,
                                  char *json_path,
                                  size_t json_path_size,
                                  size_t bank,
                                  uint16_t addr)
{
    if (asm_path && asm_path_size > 0)
    {
        snprintf(asm_path, asm_path_size, "%s/func_b%03zu_%04X.asm", g_probe.output_dir, bank, addr);
    }

    if (json_path && json_path_size > 0)
    {
        snprintf(json_path, json_path_size, "%s/func_b%03zu_%04X.json", g_probe.output_dir, bank, addr);
    }
}

static void function_dot_path(char *dot_path, size_t dot_path_size, size_t bank, uint16_t addr)
{
    if (dot_path && dot_path_size > 0)
    {
        snprintf(dot_path, dot_path_size, "%s/func_b%03zu_%04X.dot", g_probe.output_dir, bank, addr);
    }
}

static bool function_outputs_complete(size_t bank, uint16_t addr)
{
    char asm_path[1024];
    char json_path[1024];
    char dot_path[1024];
    function_output_paths(asm_path, sizeof(asm_path), json_path, sizeof(json_path), bank, addr);
    function_dot_path(dot_path, sizeof(dot_path), bank, addr);
    if (!file_exists(asm_path) || !file_exists(json_path))
        return false;
    if (g_probe.export_dot && !file_exists(dot_path))
        return false;
    return true;
}

static void accumulate_cached_stats(size_t bank, uint16_t addr)
{
    char json_path[1024];
    function_output_paths(NULL, 0, json_path, sizeof(json_path), bank, addr);

    FILE *f = fopen(json_path, "r");
    if (!f)
        return;

    size_t branches = 0;
    size_t paths = 0;
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        char *b = strstr(line, "\"branches\"");
        if (b)
        {
            char *colon = strchr(b, ':');
            if (colon)
            {
                branches = (size_t)strtoull(colon + 1, NULL, 10);
            }
        }

        char *p = strstr(line, "\"paths\"");
        if (p)
        {
            char *colon = strchr(p, ':');
            if (colon)
            {
                paths = (size_t)strtoull(colon + 1, NULL, 10);
            }
        }
    }
    fclose(f);

    g_probe.branch_count += branches;
    g_probe.path_count += paths;
}

static void path_to_stem(const char *path, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;

    out[0] = '\0';
    if (!path || path[0] == '\0')
    {
        snprintf(out, out_size, "rom");
        return;
    }

    const char *base = path;
    for (const char *p = path; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }

    snprintf(out, out_size, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot)
        *dot = '\0';

    if (out[0] == '\0')
    {
        snprintf(out, out_size, "rom");
    }

    for (char *p = out; *p; ++p)
    {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '_' || c == '-'))
            *p = '_';
    }
}

static size_t wrap_bank(size_t bank, size_t count)
{
    if (count == 0)
        return 0;
    return bank % count;
}

static size_t effective_bank(const MemoryState *mem, uint16_t address)
{
    if (!mem || address >= 0x8000)
        return 0;

    const CartridgeState *cart = &mem->cartridge;
    if (!cart->rom_data || cart->rom_size == 0)
    {
        return address < 0x4000 ? 0 : 1;
    }

    size_t bank = 0;
    switch (cart->mbc_type)
    {
    case MBC1:
        if (address < 0x4000)
        {
            bank = cart->mbc1_mode ? (((size_t)cart->mbc1_high2 & 0x03u) << 5u) : 0u;
        }
        else
        {
            uint8_t low = (uint8_t)(cart->mbc1_low5 & 0x1F);
            if (low == 0)
                low = 1;
            bank = low | (((size_t)cart->mbc1_high2 & 0x03u) << 5u);
        }
        break;
    case MBC2:
        bank = (address < 0x4000) ? 0u : (size_t)(cart->mbc2_rom_bank & 0x0F);
        if (address >= 0x4000 && bank == 0)
            bank = 1;
        break;
    case MBC3:
        bank = (address < 0x4000) ? 0u : (size_t)(cart->mbc3_rom_bank & 0x7F);
        if (address >= 0x4000 && bank == 0)
            bank = 1;
        break;
    case MBC5:
        bank = (address < 0x4000) ? 0u : (size_t)(cart->mbc5_rom_bank & 0x01FF);
        break;
    case MBC_NONE:
    default:
        bank = (address < 0x4000) ? 0u : 1u;
        break;
    }

    return wrap_bank(bank, cart->rom_banks);
}

static bool read_bank_byte(const MemoryState *mem, size_t bank, uint16_t address, uint8_t *out)
{
    if (!mem || !out)
        return false;

    if (address >= 0x8000)
    {
        *out = mem->memory.data[address];
        return true;
    }

    if (mem->cartridge.rom_data && mem->cartridge.rom_size > 0)
    {
        size_t mapped_bank = bank;
        if (address < 0x4000)
        {
            // 0000-3FFF is the fixed region on GB cartridges.
            mapped_bank = 0;
        }
        mapped_bank = wrap_bank(mapped_bank, mem->cartridge.rom_banks);
        size_t abs = mapped_bank * 0x4000u + (size_t)(address & 0x3FFFu);
        if (abs >= mem->cartridge.rom_size)
            return false;
        *out = mem->cartridge.rom_data[abs];
        return true;
    }

    *out = mem->memory.data[address];
    return true;
}

static uint8_t read_bank_u8(const MemoryState *mem, size_t bank, uint16_t address)
{
    uint8_t v = 0xFF;
    (void)read_bank_byte(mem, bank, address, &v);
    return v;
}

static uint16_t read_bank_u16(const MemoryState *mem, size_t bank, uint16_t address)
{
    uint8_t lo = read_bank_u8(mem, bank, address);
    uint8_t hi = read_bank_u8(mem, bank, (uint16_t)(address + 1));
    return (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

static void decode_cb(uint8_t cb, DecodedLine *out)
{
    out->len = 2;
    out->stop = false;

    if (cb < 0x40)
    {
        uint8_t group = (uint8_t)(cb >> 3);
        uint8_t reg = (uint8_t)(cb & 0x07);
        snprintf(out->text, sizeof(out->text), "%s %s", cb_rot_names[group], reg8_names[reg]);
        return;
    }

    if (cb < 0x80)
    {
        uint8_t bit = (uint8_t)((cb - 0x40) >> 3);
        uint8_t reg = (uint8_t)(cb & 0x07);
        snprintf(out->text, sizeof(out->text), "BIT %u,%s", bit, reg8_names[reg]);
        return;
    }

    if (cb < 0xC0)
    {
        uint8_t bit = (uint8_t)((cb - 0x80) >> 3);
        uint8_t reg = (uint8_t)(cb & 0x07);
        snprintf(out->text, sizeof(out->text), "RES %u,%s", bit, reg8_names[reg]);
        return;
    }

    uint8_t bit = (uint8_t)((cb - 0xC0) >> 3);
    uint8_t reg = (uint8_t)(cb & 0x07);
    snprintf(out->text, sizeof(out->text), "SET %u,%s", bit, reg8_names[reg]);
}

static void decode_line(const MemoryState *mem, size_t bank, uint16_t pc, DecodedLine *out)
{
    memset(out, 0, sizeof(*out));
    out->len = 1;
    out->stop = false;

    uint8_t op = read_bank_u8(mem, bank, pc);
    uint8_t n8 = read_bank_u8(mem, bank, (uint16_t)(pc + 1));
    uint16_t n16 = read_bank_u16(mem, bank, (uint16_t)(pc + 1));

    if (op >= 0x40 && op <= 0x7F)
    {
        if (op == 0x76)
        {
            snprintf(out->text, sizeof(out->text), "HALT");
        }
        else
        {
            uint8_t dst = (uint8_t)((op >> 3) & 0x07);
            uint8_t src = (uint8_t)(op & 0x07);
            snprintf(out->text, sizeof(out->text), "LD %s,%s", reg8_names[dst], reg8_names[src]);
        }
        return;
    }

    if (op >= 0x80 && op <= 0xBF)
    {
        uint8_t grp = (uint8_t)((op - 0x80) >> 3);
        uint8_t src = (uint8_t)(op & 0x07);
        snprintf(out->text, sizeof(out->text), "%s %s", alu_names[grp], reg8_names[src]);
        return;
    }

    switch (op)
    {
    case 0x00:
        snprintf(out->text, sizeof(out->text), "NOP");
        break;
    case 0x01:
    case 0x11:
    case 0x21:
    case 0x31:
        out->len = 3;
        snprintf(out->text, sizeof(out->text), "LD %s,$%04X", reg16_names[(op >> 4) & 0x03], n16);
        break;
    case 0x02:
        snprintf(out->text, sizeof(out->text), "LD (BC),A");
        break;
    case 0x03:
    case 0x13:
    case 0x23:
    case 0x33:
        snprintf(out->text, sizeof(out->text), "INC %s", reg16_names[(op >> 4) & 0x03]);
        break;
    case 0x04:
    case 0x0C:
    case 0x14:
    case 0x1C:
    case 0x24:
    case 0x2C:
    case 0x3C:
        snprintf(out->text, sizeof(out->text), "INC %s", reg8_names[(op >> 3) & 0x07]);
        break;
    case 0x05:
    case 0x0D:
    case 0x15:
    case 0x1D:
    case 0x25:
    case 0x2D:
    case 0x3D:
        snprintf(out->text, sizeof(out->text), "DEC %s", reg8_names[(op >> 3) & 0x07]);
        break;
    case 0x06:
    case 0x0E:
    case 0x16:
    case 0x1E:
    case 0x26:
    case 0x2E:
    case 0x36:
    case 0x3E:
        out->len = 2;
        snprintf(out->text, sizeof(out->text), "LD %s,$%02X", reg8_names[(op >> 3) & 0x07], n8);
        break;
    case 0x07:
        snprintf(out->text, sizeof(out->text), "RLCA");
        break;
    case 0x08:
        out->len = 3;
        snprintf(out->text, sizeof(out->text), "LD ($%04X),SP", n16);
        break;
    case 0x09:
    case 0x19:
    case 0x29:
    case 0x39:
        snprintf(out->text, sizeof(out->text), "ADD HL,%s", reg16_names[(op >> 4) & 0x03]);
        break;
    case 0x0A:
        snprintf(out->text, sizeof(out->text), "LD A,(BC)");
        break;
    case 0x0B:
    case 0x1B:
    case 0x2B:
    case 0x3B:
        snprintf(out->text, sizeof(out->text), "DEC %s", reg16_names[(op >> 4) & 0x03]);
        break;
    case 0x0F:
        snprintf(out->text, sizeof(out->text), "RRCA");
        break;
    case 0x10:
        out->len = 2;
        out->stop = true;
        snprintf(out->text, sizeof(out->text), "STOP");
        break;
    case 0x12:
        snprintf(out->text, sizeof(out->text), "LD (DE),A");
        break;
    case 0x17:
        snprintf(out->text, sizeof(out->text), "RLA");
        break;
    case 0x18:
    {
        out->len = 2;
        out->stop = true;
        uint16_t dst = (uint16_t)(pc + 2 + (int8_t)n8);
        snprintf(out->text, sizeof(out->text), "JR $%04X", dst);
        break;
    }
    case 0x1A:
        snprintf(out->text, sizeof(out->text), "LD A,(DE)");
        break;
    case 0x1F:
        snprintf(out->text, sizeof(out->text), "RRA");
        break;
    case 0x20:
    case 0x28:
    case 0x30:
    case 0x38:
    {
        out->len = 2;
        uint16_t dst = (uint16_t)(pc + 2 + (int8_t)n8);
        snprintf(out->text, sizeof(out->text), "JR %s,$%04X", cond_names[(op - 0x20) >> 3], dst);
        break;
    }
    case 0x22:
        snprintf(out->text, sizeof(out->text), "LD (HL+),A");
        break;
    case 0x27:
        snprintf(out->text, sizeof(out->text), "DAA");
        break;
    case 0x2A:
        snprintf(out->text, sizeof(out->text), "LD A,(HL+)");
        break;
    case 0x2F:
        snprintf(out->text, sizeof(out->text), "CPL");
        break;
    case 0x32:
        snprintf(out->text, sizeof(out->text), "LD (HL-),A");
        break;
    case 0x34:
        snprintf(out->text, sizeof(out->text), "INC (HL)");
        break;
    case 0x35:
        snprintf(out->text, sizeof(out->text), "DEC (HL)");
        break;
    case 0x37:
        snprintf(out->text, sizeof(out->text), "SCF");
        break;
    case 0x3A:
        snprintf(out->text, sizeof(out->text), "LD A,(HL-)");
        break;
    case 0x3F:
        snprintf(out->text, sizeof(out->text), "CCF");
        break;
    case 0xC0:
    case 0xC8:
    case 0xD0:
    case 0xD8:
        snprintf(out->text, sizeof(out->text), "RET %s", cond_names[(op - 0xC0) >> 3]);
        break;
    case 0xC1:
    case 0xD1:
    case 0xE1:
    case 0xF1:
        snprintf(out->text, sizeof(out->text), "POP %s", reg16_stack_names[(op - 0xC1) >> 4]);
        break;
    case 0xC2:
    case 0xCA:
    case 0xD2:
    case 0xDA:
        out->len = 3;
        snprintf(out->text, sizeof(out->text), "JP %s,$%04X", cond_names[(op - 0xC2) >> 3], n16);
        break;
    case 0xC3:
        out->len = 3;
        out->stop = true;
        snprintf(out->text, sizeof(out->text), "JP $%04X", n16);
        break;
    case 0xC4:
    case 0xCC:
    case 0xD4:
    case 0xDC:
        out->len = 3;
        snprintf(out->text, sizeof(out->text), "CALL %s,$%04X", cond_names[(op - 0xC4) >> 3], n16);
        break;
    case 0xC5:
    case 0xD5:
    case 0xE5:
    case 0xF5:
        snprintf(out->text, sizeof(out->text), "PUSH %s", reg16_stack_names[(op - 0xC5) >> 4]);
        break;
    case 0xC6:
    case 0xCE:
    case 0xD6:
    case 0xDE:
    case 0xE6:
    case 0xEE:
    case 0xF6:
    case 0xFE:
        out->len = 2;
        snprintf(out->text, sizeof(out->text), "%s $%02X", alu_names[(op - 0xC6) >> 3], n8);
        break;
    case 0xC7:
    case 0xCF:
    case 0xD7:
    case 0xDF:
    case 0xE7:
    case 0xEF:
    case 0xF7:
    case 0xFF:
        snprintf(out->text, sizeof(out->text), "RST $%02X", op & 0x38);
        break;
    case 0xC9:
        out->stop = true;
        snprintf(out->text, sizeof(out->text), "RET");
        break;
    case 0xCB:
    {
        uint8_t cb = read_bank_u8(mem, bank, (uint16_t)(pc + 1));
        decode_cb(cb, out);
        break;
    }
    case 0xCD:
        out->len = 3;
        snprintf(out->text, sizeof(out->text), "CALL $%04X", n16);
        break;
    case 0xD9:
        out->stop = true;
        snprintf(out->text, sizeof(out->text), "RETI");
        break;
    case 0xE0:
        out->len = 2;
        snprintf(out->text, sizeof(out->text), "LDH ($FF%02X),A", n8);
        break;
    case 0xE2:
        snprintf(out->text, sizeof(out->text), "LD (C),A");
        break;
    case 0xE8:
        out->len = 2;
        snprintf(out->text, sizeof(out->text), "ADD SP,%d", (int8_t)n8);
        break;
    case 0xE9:
        out->stop = true;
        snprintf(out->text, sizeof(out->text), "JP (HL)");
        break;
    case 0xEA:
        out->len = 3;
        snprintf(out->text, sizeof(out->text), "LD ($%04X),A", n16);
        break;
    case 0xF0:
        out->len = 2;
        snprintf(out->text, sizeof(out->text), "LDH A,($FF%02X)", n8);
        break;
    case 0xF2:
        snprintf(out->text, sizeof(out->text), "LD A,(C)");
        break;
    case 0xF3:
        snprintf(out->text, sizeof(out->text), "DI");
        break;
    case 0xF8:
        out->len = 2;
        snprintf(out->text, sizeof(out->text), "LD HL,SP+%d", (int8_t)n8);
        break;
    case 0xF9:
        snprintf(out->text, sizeof(out->text), "LD SP,HL");
        break;
    case 0xFA:
        out->len = 3;
        snprintf(out->text, sizeof(out->text), "LD A,($%04X)", n16);
        break;
    case 0xFB:
        snprintf(out->text, sizeof(out->text), "EI");
        break;
    default:
        // Dense decode for remaining 0xC0-0xFF slots.
        if ((op & 0x0F) == 0x0B || (op & 0x0F) == 0x03 || (op & 0x0F) == 0x04 ||
            (op & 0x0F) == 0x0D || op == 0xEB || op == 0xD3 || op == 0xDB ||
            op == 0xDD || op == 0xE3 || op == 0xE4 || op == 0xEC || op == 0xED ||
            op == 0xF4 || op == 0xFC || op == 0xFD)
        {
            out->stop = true;
            snprintf(out->text, sizeof(out->text), "DB $%02X ; invalid", op);
        }
        else
        {
            out->stop = true;
            snprintf(out->text, sizeof(out->text), "DB $%02X", op);
        }
        break;
    }
}

static uint32_t dyn_dump_sig_window(const CPUState *cpu, uint16_t pc)
{
    if (!cpu || !cpu->memory)
        return 0u;

    // Body hash: hash only the decoded instruction bytes that would be dumped
    // (same stop/region rules as the dynram .asm emitter).
    uint32_t h = 2166136261u; // FNV-1a
    uint16_t cur = pc;
    for (size_t i = 0; i < 128; ++i)
    {
        if (cur < 0x8000u)
            break;

        DecodedLine line;
        decode_line(cpu->memory, 0, cur, &line);

        uint8_t len = line.len ? line.len : 1u;
        for (uint8_t b = 0; b < len; ++b)
        {
            uint16_t a = (uint16_t)(cur + b);
            uint8_t v = cpu->memory->memory.data[a];
            h ^= (uint32_t)v;
            h *= 16777619u;
        }

        uint16_t next = (uint16_t)(cur + len);
        if (line.stop)
            break;
        if (next == cur)
            break;
        if (next < 0x8000u)
            break;
        if (!dyn_same_region(pc, next))
            break;
        cur = next;
    }
    return h;
}

static bool dyn_dump_marked_same_signature(const CPUState *cpu, uint16_t pc)
{
    uint16_t idx = (uint16_t)(pc >> 3);
    uint8_t mask = (uint8_t)(1u << (pc & 7u));
    uint32_t sig = dyn_dump_sig_window(cpu, pc);
    if ((g_dyn_dump.dumped_start[idx] & mask) != 0)
    {
        if (g_dyn_dump.dumped_sig[pc] == sig)
            return true;
        g_dyn_dump.dumped_sig[pc] = sig;
        return false;
    }
    g_dyn_dump.dumped_start[idx] |= mask;
    g_dyn_dump.dumped_sig[pc] = sig;
    return false;
}

static const char *dyn_region_name(uint16_t pc)
{
    if (pc >= 0xFF80u && pc <= 0xFFFEu)
        return "hram";
    if (pc >= 0xC000u && pc <= 0xDFFFu)
        return "wram";
    if (pc >= 0xA000u && pc <= 0xBFFFu)
        return "sram";
    if (pc >= 0x8000u && pc <= 0x9FFFu)
        return "vram";
    return "ram";
}

static bool dyn_same_region(uint16_t a, uint16_t b)
{
    if (a >= 0xFF80u && a <= 0xFFFEu)
        return (b >= 0xFF80u && b <= 0xFFFEu);
    if (a >= 0xC000u && a <= 0xDFFFu)
        return (b >= 0xC000u && b <= 0xDFFFu);
    if (a >= 0xA000u && a <= 0xBFFFu)
        return (b >= 0xA000u && b <= 0xBFFFu);
    if (a >= 0x8000u && a <= 0x9FFFu)
        return (b >= 0x8000u && b <= 0x9FFFu);
    return (b >= 0x8000u);
}

void recomp_probe_dyn_dump_init(const char *rom_path, MemoryState *mem)
{
    g_dyn_dump.mem = mem;
    g_dyn_dump.enabled = false;

    char stem[260];
    path_to_stem(rom_path, stem, sizeof(stem));
    if (!ensure_dir(stem))
        return;

    snprintf(g_dyn_dump.output_dir, sizeof(g_dyn_dump.output_dir), "%s/dynram", stem);
    if (!ensure_dir(g_dyn_dump.output_dir))
        return;

    memset(g_dyn_dump.dumped_start, 0, sizeof(g_dyn_dump.dumped_start));
    memset(g_dyn_dump.dumped_sig, 0, sizeof(g_dyn_dump.dumped_sig));
    g_dyn_dump.enabled = true;
}

void recomp_probe_dyn_dump_code(CPUState *cpu, uint16_t pc, const char *reason)
{
    if (!cpu || !cpu->memory)
        return;
    if (pc < 0x8000u)
        return;
    if (!g_dyn_dump.enabled || !g_dyn_dump.mem)
        return;
    uint32_t sig = dyn_dump_sig_window(cpu, pc);
    if (dyn_dump_marked_same_signature(cpu, pc))
        return;

    char path[1024];
    const char *region = dyn_region_name(pc);
    snprintf(path, sizeof(path), "%s/dyn_%s_%04X_%08X.asm", g_dyn_dump.output_dir, region, pc, sig);

    FILE *f = fopen(path, "w");
    if (!f)
        return;

    fprintf(f, "; Dynamic RAM code dump\n");
    fprintf(f, "; entry=%04X region=%s reason=%s sig=%08X\n\n", pc, region, reason ? reason : "runtime", sig);

    uint16_t cur = pc;
    for (size_t i = 0; i < 128; ++i)
    {
        DecodedLine line;
        decode_line(cpu->memory, 0, cur, &line);

        char bytes[32] = {0};
        size_t bytes_pos = 0;
        for (uint8_t b = 0; b < line.len && b < 3; ++b)
        {
            uint8_t v = cpu->memory->memory.data[(uint16_t)(cur + b)];
            int wrote = snprintf(bytes + bytes_pos, sizeof(bytes) - bytes_pos, "%02X ", v);
            if (wrote <= 0 || (size_t)wrote >= (sizeof(bytes) - bytes_pos))
                break;
            bytes_pos += (size_t)wrote;
        }

        fprintf(f, "    %04X: %-10s %s\n", cur, bytes, line.text[0] ? line.text : "DB ?");

        uint16_t next = (uint16_t)(cur + (line.len ? line.len : 1u));
        if (line.stop)
            break;
        if (next == cur)
            break;
        if (next < 0x8000u)
            break;
        if (!dyn_same_region(pc, next))
            break;
        cur = next;
    }

    fclose(f);
}

uint32_t recomp_probe_dyn_body_hash(CPUState *cpu, uint16_t pc)
{
    return dyn_dump_sig_window(cpu, pc);
}

static bool seen_has(size_t bank, uint16_t addr)
{
    for (size_t i = 0; i < g_probe.seen_count; ++i)
    {
        if (g_probe.seen[i].bank == bank && g_probe.seen[i].addr == addr)
            return true;
    }
    return false;
}

static bool seen_add(size_t bank, uint16_t addr)
{
    if (seen_has(bank, addr))
        return true;

    if (g_probe.seen_count == g_probe.seen_cap)
    {
        size_t next_cap = (g_probe.seen_cap == 0) ? 64 : g_probe.seen_cap * 2;
        FunctionKey *next = (FunctionKey *)realloc(g_probe.seen, next_cap * sizeof(FunctionKey));
        if (!next)
            return false;
        g_probe.seen = next;
        g_probe.seen_cap = next_cap;
    }

    g_probe.seen[g_probe.seen_count].bank = bank;
    g_probe.seen[g_probe.seen_count].addr = addr;
    g_probe.seen_count++;
    return true;
}

static bool pending_push(size_t bank, uint16_t addr, const char *reason)
{
    if (g_probe.pending_count == g_probe.pending_cap)
    {
        size_t next_cap = (g_probe.pending_cap == 0) ? 64 : g_probe.pending_cap * 2;
        PendingFunction *next = (PendingFunction *)realloc(g_probe.pending, next_cap * sizeof(PendingFunction));
        if (!next)
            return false;
        g_probe.pending = next;
        g_probe.pending_cap = next_cap;
    }

    PendingFunction *slot = &g_probe.pending[g_probe.pending_count++];
    slot->bank = bank;
    slot->addr = addr;
    slot->reason = reason ? reason : "DISCOVERED";
    return true;
}

static bool pending_pop(PendingFunction *out)
{
    if (!out || g_probe.pending_count == 0)
        return false;

    *out = g_probe.pending[--g_probe.pending_count];
    return true;
}

static size_t code_bank_for_addr(size_t bank, uint16_t addr)
{
    return (addr < 0x4000u) ? 0u : bank;
}

static void write_function_disasm(size_t bank, uint16_t start, const char *reason);
static void discover_cached_calls(size_t bank, uint16_t addr);
static void run_static_reference_scan(bool include_jumps);
static void write_function_dot(size_t bank,
                               uint16_t start,
                               const uint16_t *blocks,
                               size_t block_count,
                               const CfgEdge *edges,
                               size_t edge_count,
                               const CallSite *calls,
                               size_t call_count,
                               const uint16_t *owner_block);

static void discover_function(size_t bank, uint16_t target, const char *reason)
{
    if (!g_probe.enabled || !g_probe.mem)
        return;
    if (target >= 0x8000)
        return;

    bank = code_bank_for_addr(bank, target);
    if (seen_has(bank, target))
        return;

    if (!g_probe.force_regen && function_outputs_complete(bank, target))
    {
        if (!seen_add(bank, target))
            return;
        accumulate_cached_stats(bank, target);
        if (g_probe.follow_static_calls)
        {
            discover_cached_calls(bank, target);
        }
        g_probe.processed_count++;
        return;
    }

    if (!seen_add(bank, target))
        return;

    if (!pending_push(bank, target, reason))
        return;

    if (g_probe.processing_pending)
        return;

    g_probe.processing_pending = true;
    PendingFunction item;
    while (pending_pop(&item))
    {
        write_function_disasm(item.bank, item.addr, item.reason);
        g_probe.processed_count++;
    }
    g_probe.processing_pending = false;
}

static void discover_cached_calls(size_t bank, uint16_t addr)
{
    char json_path[1024];
    function_output_paths(NULL, 0, json_path, sizeof(json_path), bank, addr);

    FILE *f = fopen(json_path, "r");
    if (!f)
        return;

    char line[1024];
    while (fgets(line, sizeof(line), f))
    {
        char *target_ptr = strstr(line, "\"target\":");
        char *bank_ptr = strstr(line, "\"target_bank\":");
        if (!target_ptr || !bank_ptr)
            continue;

        char *target_colon = strchr(target_ptr, ':');
        char *bank_colon = strchr(bank_ptr, ':');
        if (!target_colon || !bank_colon)
            continue;

        uint32_t parsed_target = (uint32_t)strtoul(target_colon + 1, NULL, 10);
        size_t parsed_bank = (size_t)strtoull(bank_colon + 1, NULL, 10);
        if (parsed_target >= 0x8000u)
            continue;

        discover_function(parsed_bank, (uint16_t)parsed_target, "CALL_STATIC_CACHE");
    }

    fclose(f);
}

static bool is_call_imm_opcode(uint8_t op)
{
    return op == 0xCD || op == 0xC4 || op == 0xCC || op == 0xD4 || op == 0xDC;
}

static bool is_jp_imm_opcode(uint8_t op)
{
    return op == 0xC3 || op == 0xC2 || op == 0xCA || op == 0xD2 || op == 0xDA;
}

static void seed_reference_target(size_t source_bank, uint16_t target, const char *reason)
{
    if (target >= 0x8000u)
        return;

    size_t target_bank = code_bank_for_addr(source_bank, target);
    DecodedLine line;
    decode_line(g_probe.mem, target_bank, target, &line);
    if (line.len == 0)
        return;
    if (strncmp(line.text, "DB $", 4) == 0)
        return;

    discover_function(target_bank, target, reason);
}

static void scan_reference_window(size_t bank,
                                  uint16_t start,
                                  uint16_t end_exclusive,
                                  bool include_jumps)
{
    if (end_exclusive <= start + 2u)
        return;

    for (uint32_t pc = start; pc + 2u < end_exclusive; ++pc)
    {
        uint16_t addr = (uint16_t)pc;
        uint8_t op = read_bank_u8(g_probe.mem, bank, addr);
        if (!is_call_imm_opcode(op) && !(include_jumps && is_jp_imm_opcode(op)))
            continue;

        uint16_t target = read_bank_u16(g_probe.mem, bank, (uint16_t)(addr + 1u));
        seed_reference_target(bank, target, is_call_imm_opcode(op) ? "REF_CALL_SCAN" : "REF_JP_SCAN");
    }
}

static void run_static_reference_scan(bool include_jumps)
{
    if (!g_probe.mem)
        return;

    size_t banks = g_probe.mem->cartridge.rom_banks;
    if (banks == 0)
    {
        banks = 1;
    }

    // Fixed bank 0 window.
    scan_reference_window(0, 0x0000u, 0x4000u, include_jumps);

    // Switchable window for each bank.
    for (size_t bank = 0; bank < banks; ++bank)
    {
        scan_reference_window(bank, 0x4000u, 0x8000u, include_jumps);
    }
}

static void format_function_id(char *out, size_t out_size, size_t bank, uint16_t addr)
{
    if (!out || out_size == 0)
        return;
    snprintf(out, out_size, "func_b%03zu_%04X", bank, addr);
}

static void json_write_escaped(FILE *f, const char *text)
{
    fputc('"', f);
    if (text)
    {
        for (const unsigned char *p = (const unsigned char *)text; *p; ++p)
        {
            switch (*p)
            {
            case '\\':
                fputs("\\\\", f);
                break;
            case '"':
                fputs("\\\"", f);
                break;
            case '\n':
                fputs("\\n", f);
                break;
            case '\r':
                fputs("\\r", f);
                break;
            case '\t':
                fputs("\\t", f);
                break;
            default:
                if (*p < 0x20)
                {
                    fprintf(f, "\\u%04X", (unsigned)*p);
                }
                else
                {
                    fputc((int)*p, f);
                }
                break;
            }
        }
    }
    fputc('"', f);
}

static const char *cfg_edge_kind_name(CfgEdgeKind kind)
{
    switch (kind)
    {
    case CFG_EDGE_JUMP:
        return "jump";
    case CFG_EDGE_BRANCH_TRUE:
        return "branch_true";
    case CFG_EDGE_BRANCH_FALSE:
        return "branch_false";
    case CFG_EDGE_FALLTHROUGH:
    default:
        return "fallthrough";
    }
}

static bool cfg_edge_vec_push(CfgEdgeVec *vec, const CfgEdge *edge)
{
    if (!vec || !edge)
        return false;

    for (size_t i = 0; i < vec->count; ++i)
    {
        const CfgEdge *it = &vec->items[i];
        if (it->from_block == edge->from_block &&
            it->to == edge->to &&
            it->to_bank == edge->to_bank &&
            it->kind == edge->kind)
        {
            return true;
        }
    }

    if (vec->count == vec->cap)
    {
        size_t next_cap = (vec->cap == 0) ? 32 : vec->cap * 2;
        CfgEdge *next = (CfgEdge *)realloc(vec->items, next_cap * sizeof(CfgEdge));
        if (!next)
            return false;
        vec->items = next;
        vec->cap = next_cap;
    }

    vec->items[vec->count++] = *edge;
    return true;
}

static bool call_site_vec_push(CallSiteVec *vec, const CallSite *site)
{
    if (!vec || !site)
        return false;

    for (size_t i = 0; i < vec->count; ++i)
    {
        const CallSite *it = &vec->items[i];
        if (it->at == site->at &&
            it->target == site->target &&
            it->target_bank == site->target_bank &&
            it->conditional == site->conditional &&
            it->rst == site->rst)
        {
            return true;
        }
    }

    if (vec->count == vec->cap)
    {
        size_t next_cap = (vec->cap == 0) ? 16 : vec->cap * 2;
        CallSite *next = (CallSite *)realloc(vec->items, next_cap * sizeof(CallSite));
        if (!next)
            return false;
        vec->items = next;
        vec->cap = next_cap;
    }

    vec->items[vec->count++] = *site;
    return true;
}

static const CallSite *call_site_find(const CallSite *sites, size_t count, uint16_t at)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (sites[i].at == at)
            return &sites[i];
    }
    return NULL;
}

static void write_function_json(size_t bank,
                                uint16_t start,
                                const char *reason,
                                const uint8_t *decoded,
                                const uint16_t *owner_block,
                                const DecodedLine *lines,
                                const uint8_t (*insn_bytes)[3],
                                const uint16_t *blocks,
                                size_t block_count,
                                size_t insn_count,
                                const CfgEdge *edges,
                                size_t edge_count,
                                const CallSite *calls,
                                size_t call_count,
                                size_t branch_count,
                                size_t path_count,
                                size_t addr_space)
{
    char path[1024];
    function_output_paths(NULL, 0, path, sizeof(path), bank, start);

    FILE *f = fopen(path, "w");
    if (!f)
        return;

    char function_id[64];
    format_function_id(function_id, sizeof(function_id), bank, start);

    fprintf(f, "{\n");
    fprintf(f, "  \"rom\": ");
    json_write_escaped(f, g_probe.rom_name);
    fprintf(f, ",\n");
    fprintf(f, "  \"function\": ");
    json_write_escaped(f, function_id);
    fprintf(f, ",\n");
    fprintf(f, "  \"entry\": {\"bank\": %zu, \"addr\": %u},\n", bank, (unsigned)start);
    fprintf(f, "  \"reason\": ");
    json_write_escaped(f, reason ? reason : "UNKNOWN");
    fprintf(f, ",\n");
    fprintf(f, "  \"stats\": {\"blocks\": %zu, \"instructions\": %zu, \"branches\": %zu, \"paths\": %zu},\n",
            block_count,
            insn_count,
            branch_count,
            path_count);

    fprintf(f, "  \"blocks\": [\n");
    for (size_t i = 0; i < block_count; ++i)
    {
        if (i > 0)
            fprintf(f, ",\n");
        fprintf(f, "    {\"addr\": %u, \"bank\": %zu, \"label\": \"loc_%04X\"}",
                (unsigned)blocks[i],
                code_bank_for_addr(bank, blocks[i]),
                (unsigned)blocks[i]);
    }
    fprintf(f, "\n  ],\n");

    fprintf(f, "  \"instructions\": [\n");
    bool first_insn = true;
    for (size_t addr = 0; addr < addr_space; ++addr)
    {
        if (!decoded[addr])
            continue;

        if (!first_insn)
            fprintf(f, ",\n");
        first_insn = false;

        char bytes[32] = {0};
        size_t bytes_pos = 0;
        for (uint8_t b = 0; b < lines[addr].len && b < 3; ++b)
        {
            int wrote = snprintf(bytes + bytes_pos, sizeof(bytes) - bytes_pos, "%02X%s",
                                 insn_bytes[addr][b], (b + 1 < lines[addr].len && b + 1 < 3) ? " " : "");
            if (wrote <= 0 || (size_t)wrote >= (sizeof(bytes) - bytes_pos))
                break;
            bytes_pos += (size_t)wrote;
        }

        fprintf(f, "    {\"addr\": %u, \"bank\": %zu, \"block\": %u, \"bytes\": ",
                (unsigned)addr,
                code_bank_for_addr(bank, (uint16_t)addr),
                (unsigned)owner_block[addr]);
        json_write_escaped(f, bytes);
        fprintf(f, ", \"text\": ");
        json_write_escaped(f, lines[addr].text);
        fprintf(f, "}");
    }
    fprintf(f, "\n  ],\n");

    fprintf(f, "  \"edges\": [\n");
    for (size_t i = 0; i < edge_count; ++i)
    {
        if (i > 0)
            fprintf(f, ",\n");

        fprintf(f, "    {\"from\": %u, \"to\": %u, \"to_bank\": %zu, \"kind\": ",
                (unsigned)edges[i].from_block,
                (unsigned)edges[i].to,
                edges[i].to_bank);
        json_write_escaped(f, cfg_edge_kind_name(edges[i].kind));
        fprintf(f, "}");
    }
    fprintf(f, "\n  ],\n");

    fprintf(f, "  \"calls\": [\n");
    for (size_t i = 0; i < call_count; ++i)
    {
        if (i > 0)
            fprintf(f, ",\n");

        char callee_id[64];
        format_function_id(callee_id, sizeof(callee_id), calls[i].target_bank, calls[i].target);

        fprintf(f, "    {\"at\": %u, \"target\": %u, \"target_bank\": %zu, \"conditional\": %s, \"kind\": ",
                (unsigned)calls[i].at,
                (unsigned)calls[i].target,
                calls[i].target_bank,
                calls[i].conditional ? "true" : "false");
        json_write_escaped(f, calls[i].rst ? "rst" : "call");
        fprintf(f, ", \"callee\": ");
        json_write_escaped(f, callee_id);
        fprintf(f, "}");
    }
    fprintf(f, "\n  ]\n");

    fprintf(f, "}\n");
    fclose(f);
}

static bool block_list_has(const uint16_t *blocks, size_t block_count, uint16_t addr)
{
    for (size_t i = 0; i < block_count; ++i)
    {
        if (blocks[i] == addr)
            return true;
    }
    return false;
}

static const char *cfg_edge_color(CfgEdgeKind kind)
{
    switch (kind)
    {
    case CFG_EDGE_JUMP:
        return "#3B82F6";
    case CFG_EDGE_BRANCH_TRUE:
        return "#16A34A";
    case CFG_EDGE_BRANCH_FALSE:
        return "#DC2626";
    case CFG_EDGE_FALLTHROUGH:
    default:
        return "#6B7280";
    }
}

static void write_function_dot(size_t bank,
                               uint16_t start,
                               const uint16_t *blocks,
                               size_t block_count,
                               const CfgEdge *edges,
                               size_t edge_count,
                               const CallSite *calls,
                               size_t call_count,
                               const uint16_t *owner_block)
{
    if (!g_probe.export_dot)
        return;

    char path[1024];
    function_dot_path(path, sizeof(path), bank, start);
    FILE *f = fopen(path, "w");
    if (!f)
        return;

    fprintf(f, "digraph func_b%03zu_%04X {\n", bank, start);
    fprintf(f, "  rankdir=LR;\n");
    fprintf(f, "  graph [fontname=\"Consolas\", fontsize=11, label=\"func_b%03zu_%04X\", labelloc=t];\n", bank, start);
    fprintf(f, "  node [shape=box, fontname=\"Consolas\", fontsize=10];\n");
    fprintf(f, "  edge [fontname=\"Consolas\", fontsize=9];\n");

    for (size_t i = 0; i < block_count; ++i)
    {
        uint16_t baddr = blocks[i];
        fprintf(f,
                "  n_%04X [label=\"loc_%04X\\nb%03zu\"];\n",
                baddr,
                baddr,
                code_bank_for_addr(bank, baddr));
    }

    for (size_t i = 0; i < edge_count; ++i)
    {
        uint16_t from = edges[i].from_block;
        uint16_t to = edges[i].to;
        if (!block_list_has(blocks, block_count, to))
        {
            fprintf(f, "  n_%04X [shape=oval, label=\"ext_%04X\\nb%03zu\"];\n", to, to, edges[i].to_bank);
        }
        fprintf(f,
                "  n_%04X -> n_%04X [label=\"%s\", color=\"%s\"];\n",
                from,
                to,
                cfg_edge_kind_name(edges[i].kind),
                cfg_edge_color(edges[i].kind));
    }

    for (size_t i = 0; i < call_count; ++i)
    {
        uint16_t from_block = owner_block ? owner_block[calls[i].at] : start;
        if (from_block == 0 && start != 0)
            from_block = start;
        fprintf(f, "  callee_%03zu_%04X [shape=oval, label=\"func_b%03zu_%04X\"];\n",
                calls[i].target_bank,
                calls[i].target,
                calls[i].target_bank,
                calls[i].target);
        fprintf(f,
                "  n_%04X -> callee_%03zu_%04X [style=dashed, color=\"#7C3AED\", label=\"%s\"];\n",
                from_block,
                calls[i].target_bank,
                calls[i].target,
                calls[i].rst ? "rst" : (calls[i].conditional ? "call?" : "call"));
    }

    fprintf(f, "}\n");
    fclose(f);

    if (!g_probe.global_cfg_dot)
        return;

    FILE *g = g_probe.global_cfg_dot;
    fprintf(g, "  subgraph cluster_b%03zu_%04X {\n", bank, start);
    fprintf(g, "    label=\"func_b%03zu_%04X\";\n", bank, start);
    fprintf(g, "    color=\"#9CA3AF\";\n");
    for (size_t i = 0; i < block_count; ++i)
    {
        uint16_t baddr = blocks[i];
        fprintf(g,
                "    b_%03zu_%04X_%04X [label=\"loc_%04X\", shape=box];\n",
                bank,
                start,
                baddr,
                baddr);
    }
    for (size_t i = 0; i < edge_count; ++i)
    {
        uint16_t from = edges[i].from_block;
        uint16_t to = edges[i].to;
        if (!block_list_has(blocks, block_count, to))
        {
            fprintf(g,
                    "    b_%03zu_%04X_%04X [label=\"ext_%04X:b%03zu\", shape=oval];\n",
                    bank,
                    start,
                    to,
                    to,
                    edges[i].to_bank);
        }
        fprintf(g,
                "    b_%03zu_%04X_%04X -> b_%03zu_%04X_%04X [label=\"%s\", color=\"%s\"];\n",
                bank,
                start,
                from,
                bank,
                start,
                to,
                cfg_edge_kind_name(edges[i].kind),
                cfg_edge_color(edges[i].kind));
    }
    fprintf(g, "  }\n");

    for (size_t i = 0; i < call_count; ++i)
    {
        uint16_t from_block = owner_block ? owner_block[calls[i].at] : start;
        if (from_block == 0 && start != 0)
            from_block = start;
        fprintf(g, "  f_%03zu_%04X [shape=oval, label=\"func_b%03zu_%04X\"];\n",
                calls[i].target_bank,
                calls[i].target,
                calls[i].target_bank,
                calls[i].target);
        fprintf(g,
                "  b_%03zu_%04X_%04X -> f_%03zu_%04X [style=dashed, color=\"#7C3AED\", label=\"%s\"];\n",
                bank,
                start,
                from_block,
                calls[i].target_bank,
                calls[i].target,
                calls[i].rst ? "rst" : (calls[i].conditional ? "call?" : "call"));
    }
}

static void write_function_disasm(size_t bank, uint16_t start, const char *reason)
{
    if (!g_probe.mem)
        return;

    enum
    {
        ADDR_SPACE = 0x8000,
        MAX_BLOCK_QUEUE = 8192,
        MAX_TOTAL_INSNS = 16384,
        MAX_INSNS_PER_BLOCK = 1024
    };

    char asm_path[1024];
    function_output_paths(asm_path, sizeof(asm_path), NULL, 0, bank, start);

    FILE *f = fopen(asm_path, "w");
    if (!f)
        return;

    fprintf(f, "; ROM: %s\n", g_probe.rom_name);
    fprintf(f, "; Entry: $%04X (bank %zu)\n", start, bank);
    fprintf(f, "; Reason: %s\n", reason ? reason : "UNKNOWN");

    uint8_t *decoded = (uint8_t *)calloc(ADDR_SPACE, 1);
    uint8_t *labels = (uint8_t *)calloc(ADDR_SPACE, 1);
    uint8_t *queued = (uint8_t *)calloc(ADDR_SPACE, 1);
    uint8_t *done_block = (uint8_t *)calloc(ADDR_SPACE, 1);
    DecodedLine *lines = (DecodedLine *)calloc(ADDR_SPACE, sizeof(DecodedLine));
    uint8_t(*insn_bytes)[3] = (uint8_t(*)[3])calloc(ADDR_SPACE, sizeof(*insn_bytes));
    uint16_t *owner_block = (uint16_t *)calloc(ADDR_SPACE, sizeof(uint16_t));
    uint16_t *queue = (uint16_t *)malloc(MAX_BLOCK_QUEUE * sizeof(uint16_t));
    uint16_t *blocks = (uint16_t *)malloc(MAX_BLOCK_QUEUE * sizeof(uint16_t));
    CfgEdgeVec edges = {0};
    CallSiteVec calls = {0};

    if (!decoded || !labels || !queued || !done_block || !lines || !insn_bytes || !owner_block || !queue || !blocks)
    {
        fprintf(f, "; ERROR: out of memory, fallback linear dump\n\n");
        uint16_t pc = start;
        for (size_t i = 0; i < 1024 && pc < 0x8000; ++i)
        {
            DecodedLine line;
            decode_line(g_probe.mem, bank, pc, &line);
            if (line.len == 0)
                break;

            char bytes[32] = {0};
            size_t bytes_pos = 0;
            for (uint8_t b = 0; b < line.len; ++b)
            {
                uint8_t val = read_bank_u8(g_probe.mem, bank, (uint16_t)(pc + b));
                int wrote = snprintf(bytes + bytes_pos, sizeof(bytes) - bytes_pos, "%02X ", val);
                if (wrote <= 0 || (size_t)wrote >= (sizeof(bytes) - bytes_pos))
                    break;
                bytes_pos += (size_t)wrote;
            }
            fprintf(f, "%04X: %-10s %s\n", pc, bytes, line.text);
            pc = (uint16_t)(pc + line.len);
            if (line.stop)
                break;
        }

        free(decoded);
        free(labels);
        free(queued);
        free(done_block);
        free(lines);
        free(insn_bytes);
        free(owner_block);
        free(queue);
        free(blocks);
        free(edges.items);
        free(calls.items);
        fclose(f);
        return;
    }

    size_t q_head = 0;
    size_t q_tail = 0;
    size_t insn_count = 0;
    size_t block_count = 0;
    size_t local_branch_count = 0;

    labels[start] = 1;
    queued[start] = 1;
    queue[q_tail++] = start;

    while (q_head < q_tail && insn_count < MAX_TOTAL_INSNS)
    {
        uint16_t block_pc = queue[q_head++];
        if (block_pc >= ADDR_SPACE || done_block[block_pc])
            continue;
        done_block[block_pc] = 1;
        if (block_count < MAX_BLOCK_QUEUE)
        {
            blocks[block_count++] = block_pc;
        }

        uint16_t pc = block_pc;
        for (size_t step = 0; step < MAX_INSNS_PER_BLOCK && pc < ADDR_SPACE && insn_count < MAX_TOTAL_INSNS; ++step)
        {
            if (decoded[pc])
            {
                if (pc != block_pc)
                {
                    labels[pc] = 1;
                    CfgEdge edge = {
                        .from_block = block_pc,
                        .to = pc,
                        .to_bank = code_bank_for_addr(bank, pc),
                        .kind = CFG_EDGE_FALLTHROUGH};
                    (void)cfg_edge_vec_push(&edges, &edge);
                }
                break;
            }

            DecodedLine line;
            decode_line(g_probe.mem, bank, pc, &line);
            if (line.len == 0)
                break;

            if ((uint16_t)(pc + line.len) > ADDR_SPACE)
                break;

            decoded[pc] = 1;
            owner_block[pc] = block_pc;
            lines[pc] = line;
            for (uint8_t b = 0; b < line.len && b < 3; ++b)
            {
                insn_bytes[pc][b] = read_bank_u8(g_probe.mem, bank, (uint16_t)(pc + b));
            }
            insn_count++;

            uint8_t op = insn_bytes[pc][0];
            uint16_t next = (uint16_t)(pc + line.len);
            bool end_block = false;
            bool follow_fallthrough = true;
            CfgEdgeKind fallthrough_kind = CFG_EDGE_FALLTHROUGH;
            bool has_branch_target = false;
            uint16_t branch_target = 0;
            CfgEdgeKind branch_kind = CFG_EDGE_JUMP;

            switch (op)
            {
            case 0x18: // JR e8
                has_branch_target = true;
                branch_target = (uint16_t)(pc + 2 + (int8_t)insn_bytes[pc][1]);
                branch_kind = CFG_EDGE_JUMP;
                end_block = true;
                follow_fallthrough = false;
                break;
            case 0x20:
            case 0x28:
            case 0x30:
            case 0x38: // JR cc,e8
                local_branch_count++;
                has_branch_target = true;
                branch_target = (uint16_t)(pc + 2 + (int8_t)insn_bytes[pc][1]);
                branch_kind = CFG_EDGE_BRANCH_TRUE;
                fallthrough_kind = CFG_EDGE_BRANCH_FALSE;
                end_block = true;
                follow_fallthrough = true;
                break;
            case 0xC2:
            case 0xCA:
            case 0xD2:
            case 0xDA: // JP cc,nn
                local_branch_count++;
                has_branch_target = true;
                branch_target = (uint16_t)((uint16_t)insn_bytes[pc][1] | ((uint16_t)insn_bytes[pc][2] << 8));
                branch_kind = CFG_EDGE_BRANCH_TRUE;
                fallthrough_kind = CFG_EDGE_BRANCH_FALSE;
                end_block = true;
                follow_fallthrough = true;
                break;
            case 0xC3: // JP nn
                has_branch_target = true;
                branch_target = (uint16_t)((uint16_t)insn_bytes[pc][1] | ((uint16_t)insn_bytes[pc][2] << 8));
                branch_kind = CFG_EDGE_JUMP;
                end_block = true;
                follow_fallthrough = false;
                break;
            case 0xE9: // JP (HL)
                end_block = true;
                follow_fallthrough = false;
                break;
            case 0xC9: // RET
            case 0xD9: // RETI
                end_block = true;
                follow_fallthrough = false;
                break;
            case 0xC0:
            case 0xC8:
            case 0xD0:
            case 0xD8: // RET cc
                local_branch_count++;
                end_block = true;
                follow_fallthrough = true;
                fallthrough_kind = CFG_EDGE_BRANCH_FALSE;
                break;
            case 0x10: // STOP
            case 0x76: // HALT
                end_block = true;
                follow_fallthrough = false;
                break;
            default:
                if (line.stop)
                {
                    end_block = true;
                    follow_fallthrough = false;
                }
                break;
            }

            switch (op)
            {
            case 0xCD:
            case 0xC4:
            case 0xCC:
            case 0xD4:
            case 0xDC:
            {
                if (op != 0xCD)
                {
                    local_branch_count++;
                }
                CallSite site = {0};
                site.at = pc;
                site.target = (uint16_t)((uint16_t)insn_bytes[pc][1] | ((uint16_t)insn_bytes[pc][2] << 8));
                site.target_bank = code_bank_for_addr(bank, site.target);
                site.conditional = (op != 0xCD);
                site.rst = false;
                (void)call_site_vec_push(&calls, &site);
                break;
            }
            case 0xC7:
            case 0xCF:
            case 0xD7:
            case 0xDF:
            case 0xE7:
            case 0xEF:
            case 0xF7:
            case 0xFF:
            {
                CallSite site = {0};
                site.at = pc;
                site.target = (uint16_t)(op & 0x38);
                site.target_bank = code_bank_for_addr(bank, site.target);
                site.conditional = false;
                site.rst = true;
                (void)call_site_vec_push(&calls, &site);
                break;
            }
            default:
                break;
            }

            if (has_branch_target && branch_target < ADDR_SPACE)
            {
                labels[branch_target] = 1;
                if (!queued[branch_target] && q_tail < MAX_BLOCK_QUEUE)
                {
                    queued[branch_target] = 1;
                    queue[q_tail++] = branch_target;
                }

                CfgEdge edge = {
                    .from_block = block_pc,
                    .to = branch_target,
                    .to_bank = code_bank_for_addr(bank, branch_target),
                    .kind = branch_kind};
                (void)cfg_edge_vec_push(&edges, &edge);
            }

            if (follow_fallthrough && next < ADDR_SPACE)
            {
                if (end_block)
                {
                    CfgEdge edge = {
                        .from_block = block_pc,
                        .to = next,
                        .to_bank = code_bank_for_addr(bank, next),
                        .kind = fallthrough_kind};
                    (void)cfg_edge_vec_push(&edges, &edge);

                    labels[next] = 1;
                    if (!queued[next] && q_tail < MAX_BLOCK_QUEUE)
                    {
                        queued[next] = 1;
                        queue[q_tail++] = next;
                    }
                    break;
                }

                pc = next;
                continue;
            }

            break;
        }
    }

    fprintf(f, "; Blocks: %zu\n", block_count);
    fprintf(f, "; Instructions: %zu\n\n", insn_count);

    bool first = true;
    for (uint32_t addr = 0; addr < ADDR_SPACE; ++addr)
    {
        if (!decoded[addr])
            continue;

        if (labels[addr])
        {
            if (!first)
                fprintf(f, "\n");
            fprintf(f, "loc_%04X:\n", (unsigned)addr);
        }
        first = false;

        char bytes[32] = {0};
        size_t bytes_pos = 0;
        for (uint8_t b = 0; b < lines[addr].len && b < 3; ++b)
        {
            int wrote = snprintf(bytes + bytes_pos, sizeof(bytes) - bytes_pos, "%02X ", insn_bytes[addr][b]);
            if (wrote <= 0 || (size_t)wrote >= (sizeof(bytes) - bytes_pos))
                break;
            bytes_pos += (size_t)wrote;
        }

        const CallSite *call = call_site_find(calls.items, calls.count, (uint16_t)addr);
        if (call)
        {
            char callee_id[64];
            format_function_id(callee_id, sizeof(callee_id), call->target_bank, call->target);
            fprintf(f, "    %04X: %-10s %-24s ; %s %s\n",
                    (unsigned)addr,
                    bytes,
                    lines[addr].text,
                    call->rst ? "rst ->" : (call->conditional ? "call? ->" : "call ->"),
                    callee_id);
        }
        else
        {
            fprintf(f, "    %04X: %-10s %s\n", (unsigned)addr, bytes, lines[addr].text);
        }
    }

    write_function_json(bank,
                        start,
                        reason,
                        decoded,
                        owner_block,
                        lines,
                        insn_bytes,
                        blocks,
                        block_count,
                        insn_count,
                        edges.items,
                        edges.count,
                        calls.items,
                        calls.count,
                        local_branch_count,
                        edges.count + calls.count,
                        ADDR_SPACE);

    write_function_dot(bank,
                       start,
                       blocks,
                       block_count,
                       edges.items,
                       edges.count,
                       calls.items,
                       calls.count,
                       owner_block);

    if (g_probe.follow_static_calls)
    {
        for (size_t i = 0; i < calls.count; ++i)
        {
            const CallSite *site = &calls.items[i];
            discover_function(site->target_bank,
                              site->target,
                              site->rst ? "RST_STATIC" : (site->conditional ? "CALL_COND_STATIC" : "CALL_STATIC"));
        }
    }

    g_probe.branch_count += local_branch_count;
    g_probe.path_count += edges.count + calls.count;

    free(decoded);
    free(labels);
    free(queued);
    free(done_block);
    free(lines);
    free(insn_bytes);
    free(owner_block);
    free(queue);
    free(blocks);
    free(edges.items);
    free(calls.items);
    fclose(f);
}

static void record_function_entry(CPUState *cpu, uint16_t target, const char *reason)
{
    if (!g_probe.enabled || !cpu || !cpu->memory)
        return;
    discover_function(effective_bank(cpu->memory, target), target, reason);
}

void recomp_probe_init(const char *rom_path, MemoryState *mem)
{
    recomp_probe_shutdown();

    g_probe.logging_enabled = g_probe_log_enabled;
    g_probe.mem = mem;
    g_probe.force_regen = env_flag_enabled("GB_RECOMP_FORCE_REGEN") || env_flag_enabled("GB_RECOMP_REGEN");
    g_probe.follow_static_calls = true;
    const char *static_walk_env = getenv("GB_RECOMP_STATIC_CALL_WALK");
    const char *static_calls_env = getenv("GB_RECOMP_STATIC_CALLS");
    if ((static_walk_env && static_walk_env[0] == '0') ||
        (static_calls_env && static_calls_env[0] == '0') ||
        env_flag_enabled("GB_RECOMP_NO_STATIC_CALL_WALK"))
    {
        g_probe.follow_static_calls = false;
    }
    g_probe.export_dot = true;
    if (env_flag_enabled("GB_RECOMP_NO_DOT"))
    {
        g_probe.export_dot = false;
    }
    const char *dot_env = getenv("GB_RECOMP_DOT");
    if (dot_env && dot_env[0] == '0')
    {
        g_probe.export_dot = false;
    }
    const char *env = getenv("GB_RECOMP_PROBE");
    if (env && env[0] == '0')
    {
        g_probe.enabled = false;
        return;
    }
    g_probe.profile_inputs = env_flag_enabled("GB_RECOMP_INPUT_PROFILE");
    g_probe.profile_input_snapshots = env_flag_enabled("GB_RECOMP_INPUT_SNAPSHOTS") || env_flag_enabled("GB_RECOMP_INPUT_SAMPLES");
    g_probe.input_samples_max_per_function = 4u;
    {
        const char *samples_max_env = getenv("GB_RECOMP_INPUT_SAMPLES_MAX");
        if (samples_max_env && samples_max_env[0] != '\0')
        {
            unsigned long parsed = strtoul(samples_max_env, NULL, 10);
            if (parsed <= 1024ul)
                g_probe.input_samples_max_per_function = (size_t)parsed;
        }
    }
    probe_exec_stack_clear();

    path_to_stem(rom_path, g_probe.rom_name, sizeof(g_probe.rom_name));
    snprintf(g_probe.output_dir, sizeof(g_probe.output_dir), "%s", g_probe.rom_name);

    if (!ensure_dir(g_probe.output_dir))
    {
        g_probe.enabled = false;
        return;
    }

    g_probe.enabled = true;
    if (g_probe.export_dot)
    {
        char global_dot_path[1024];
        snprintf(global_dot_path, sizeof(global_dot_path), "%s/cfg_all.dot", g_probe.output_dir);
        bool rewrite_global = g_probe.force_regen || !file_exists(global_dot_path);
        if (!rewrite_global)
        {
            g_probe.global_cfg_dot = NULL;
        }
        else
        {
            g_probe.global_cfg_dot = fopen(global_dot_path, "w");
        }
        if (g_probe.global_cfg_dot)
        {
            g_probe.global_cfg_opened = true;
            fprintf(g_probe.global_cfg_dot, "digraph cfg_all {\n");
            fprintf(g_probe.global_cfg_dot, "  rankdir=LR;\n");
            fprintf(g_probe.global_cfg_dot, "  graph [fontname=\"Consolas\", fontsize=11, label=\"CFG ALL - %s\", labelloc=t];\n", g_probe.rom_name);
            fprintf(g_probe.global_cfg_dot, "  node [fontname=\"Consolas\", fontsize=9];\n");
            fprintf(g_probe.global_cfg_dot, "  edge [fontname=\"Consolas\", fontsize=8];\n");
            fflush(g_probe.global_cfg_dot);
        }
    }
    if (g_probe.logging_enabled)
    {
        printf("[RECOMP] probe output: %s (cache=%s static_calls=%s dot=%s)\n",
               g_probe.output_dir,
               g_probe.force_regen ? "force-regenerate" : "reuse-existing",
               g_probe.follow_static_calls ? "on" : "off",
               g_probe.export_dot ? "on" : "off");
        if (g_probe.profile_inputs)
        {
            printf("[RECOMP] runtime input profiling: on\n");
        }
        if (g_probe.profile_input_snapshots)
        {
            printf("[RECOMP] runtime input snapshots: on (max_per_fn=%zu)\n", g_probe.input_samples_max_per_function);
        }
    }
}

void recomp_probe_shutdown(void)
{
    probe_write_input_profiles();
    probe_write_input_samples();
    if (g_probe.global_cfg_dot)
    {
        if (g_probe.global_cfg_opened)
        {
            fprintf(g_probe.global_cfg_dot, "}\n");
        }
        fclose(g_probe.global_cfg_dot);
    }
    free(g_probe.seen);
    free(g_probe.pending);
    if (g_probe.input_profiles)
    {
        for (size_t i = 0; i < g_probe.input_profiles_count; ++i)
        {
            free(g_probe.input_profiles[i].reads);
        }
        free(g_probe.input_profiles);
    }
    if (g_probe.input_sample_sets)
    {
        for (size_t i = 0; i < g_probe.input_sample_sets_count; ++i)
        {
            if (!g_probe.input_sample_sets[i].samples)
                continue;
            for (size_t s = 0; s < g_probe.input_sample_sets[i].sample_count; ++s)
            {
                free(g_probe.input_sample_sets[i].samples[s].events);
            }
            free(g_probe.input_sample_sets[i].samples);
        }
        free(g_probe.input_sample_sets);
    }
    memset(&g_probe, 0, sizeof(g_probe));
    memset(&g_dyn_dump, 0, sizeof(g_dyn_dump));
}

void recomp_probe_set_logging(bool enabled)
{
    g_probe_log_enabled = enabled;
    g_probe.logging_enabled = enabled;
}

void recomp_probe_seed_entry(CPUState *cpu, uint16_t pc)
{
    record_function_entry(cpu, pc, "ENTRY");
    if (g_probe.enabled && (g_probe.profile_inputs || g_probe.profile_input_snapshots) && cpu && cpu->memory && pc < 0x8000u)
    {
        FunctionKey k = {effective_bank(cpu->memory, pc), pc};
        probe_exec_stack_clear();
        probe_exec_stack_push(k);
        probe_exec_sample_start(cpu, k);
    }
}

void recomp_probe_on_interrupt(CPUState *cpu, uint16_t vector)
{
    record_function_entry(cpu, vector, "INT");
    if (g_probe.enabled && (g_probe.profile_inputs || g_probe.profile_input_snapshots) && cpu && cpu->memory && vector < 0x8000u)
    {
        FunctionKey k = {effective_bank(cpu->memory, vector), vector};
        probe_exec_stack_push(k);
        probe_exec_sample_start(cpu, k);
    }
}

void recomp_probe_on_data_read(CPUState *cpu, uint16_t address, uint8_t value)
{
    probe_record_input_read(cpu, address, value);
}

void recomp_probe_on_instruction(CPUState *cpu, uint16_t pc, uint8_t opcode)
{
    if (!g_probe.enabled || !cpu || !cpu->memory)
        return;

    uint16_t target = 0;
    bool take = false;
    bool is_call_like = false;
    const char *reason = NULL;

    uint8_t lo = memory_read(cpu->memory, (uint16_t)(pc + 1));
    uint8_t hi = memory_read(cpu->memory, (uint16_t)(pc + 2));
    uint16_t n16 = (uint16_t)(lo | (uint16_t)(hi << 8));

    switch (opcode)
    {
    case 0xCD: // CALL nn
        target = n16;
        take = true;
        is_call_like = true;
        reason = "CALL";
        break;
    case 0xC4: // CALL NZ,nn
        if ((cpu->F & FLAG_Z) == 0)
        {
            target = n16;
            take = true;
            is_call_like = true;
            reason = "CALL";
        }
        break;
    case 0xCC: // CALL Z,nn
        if ((cpu->F & FLAG_Z) != 0)
        {
            target = n16;
            take = true;
            is_call_like = true;
            reason = "CALL";
        }
        break;
    case 0xD4: // CALL NC,nn
        if ((cpu->F & FLAG_C) == 0)
        {
            target = n16;
            take = true;
            is_call_like = true;
            reason = "CALL";
        }
        break;
    case 0xDC: // CALL C,nn
        if ((cpu->F & FLAG_C) != 0)
        {
            target = n16;
            take = true;
            is_call_like = true;
            reason = "CALL";
        }
        break;

    case 0xC3: // JP nn
        target = n16;
        take = true;
        reason = "JP";
        break;
    case 0xC2: // JP NZ,nn
        if ((cpu->F & FLAG_Z) == 0)
        {
            target = n16;
            take = true;
            reason = "JP";
        }
        break;
    case 0xCA: // JP Z,nn
        if ((cpu->F & FLAG_Z) != 0)
        {
            target = n16;
            take = true;
            reason = "JP";
        }
        break;
    case 0xD2: // JP NC,nn
        if ((cpu->F & FLAG_C) == 0)
        {
            target = n16;
            take = true;
            reason = "JP";
        }
        break;
    case 0xDA: // JP C,nn
        if ((cpu->F & FLAG_C) != 0)
        {
            target = n16;
            take = true;
            reason = "JP";
        }
        break;
    case 0xE9: // JP (HL)
        target = cpu->HL;
        take = true;
        reason = "JPHL";
        break;
    case 0x18: // JR e8
        target = (uint16_t)(pc + 2u + (int8_t)lo);
        take = true;
        reason = "JR";
        break;
    case 0x20: // JR NZ,e8
        if ((cpu->F & FLAG_Z) == 0)
        {
            target = (uint16_t)(pc + 2u + (int8_t)lo);
            take = true;
            reason = "JR";
        }
        break;
    case 0x28: // JR Z,e8
        if ((cpu->F & FLAG_Z) != 0)
        {
            target = (uint16_t)(pc + 2u + (int8_t)lo);
            take = true;
            reason = "JR";
        }
        break;
    case 0x30: // JR NC,e8
        if ((cpu->F & FLAG_C) == 0)
        {
            target = (uint16_t)(pc + 2u + (int8_t)lo);
            take = true;
            reason = "JR";
        }
        break;
    case 0x38: // JR C,e8
        if ((cpu->F & FLAG_C) != 0)
        {
            target = (uint16_t)(pc + 2u + (int8_t)lo);
            take = true;
            reason = "JR";
        }
        break;
    case 0xC9: // RET
    case 0xD9: // RETI
        target = (uint16_t)(memory_read(cpu->memory, cpu->SP) |
                            ((uint16_t)memory_read(cpu->memory, (uint16_t)(cpu->SP + 1u)) << 8));
        take = true;
        reason = "RET";
        break;
    case 0xC0: // RET NZ
        if ((cpu->F & FLAG_Z) == 0)
        {
            target = (uint16_t)(memory_read(cpu->memory, cpu->SP) |
                                ((uint16_t)memory_read(cpu->memory, (uint16_t)(cpu->SP + 1u)) << 8));
            take = true;
            reason = "RET";
        }
        break;
    case 0xC8: // RET Z
        if ((cpu->F & FLAG_Z) != 0)
        {
            target = (uint16_t)(memory_read(cpu->memory, cpu->SP) |
                                ((uint16_t)memory_read(cpu->memory, (uint16_t)(cpu->SP + 1u)) << 8));
            take = true;
            reason = "RET";
        }
        break;
    case 0xD0: // RET NC
        if ((cpu->F & FLAG_C) == 0)
        {
            target = (uint16_t)(memory_read(cpu->memory, cpu->SP) |
                                ((uint16_t)memory_read(cpu->memory, (uint16_t)(cpu->SP + 1u)) << 8));
            take = true;
            reason = "RET";
        }
        break;
    case 0xD8: // RET C
        if ((cpu->F & FLAG_C) != 0)
        {
            target = (uint16_t)(memory_read(cpu->memory, cpu->SP) |
                                ((uint16_t)memory_read(cpu->memory, (uint16_t)(cpu->SP + 1u)) << 8));
            take = true;
            reason = "RET";
        }
        break;
    case 0xC7:
    case 0xCF:
    case 0xD7:
    case 0xDF:
    case 0xE7:
    case 0xEF:
    case 0xF7:
    case 0xFF:
        target = (uint16_t)(opcode & 0x38);
        take = true;
        is_call_like = true;
        reason = "CALL";
        break;
    default:
        break;
    }

    if (take)
    {
        record_function_entry(cpu, target, reason ? reason : "CTRL");

        if ((g_probe.profile_inputs || g_probe.profile_input_snapshots) && target < 0x8000u)
        {
            FunctionKey k = {effective_bank(cpu->memory, target), target};
            if (is_call_like)
            {
                probe_exec_stack_push(k);
                probe_exec_sample_start(cpu, k);
            }
            else if (opcode == 0xC9 || opcode == 0xD9 || opcode == 0xC0 || opcode == 0xC8 || opcode == 0xD0 || opcode == 0xD8)
            {
                probe_exec_stack_pop();
                if (g_probe.exec_stack_depth == 0u)
                {
                    probe_exec_stack_push(k);
                    probe_exec_sample_start(cpu, k);
                }
            }
        }

        if (is_call_like && target >= 0x8000u)
        {
            uint16_t ret_pc = pc;
            switch (opcode)
            {
            case 0xCD:
            case 0xC4:
            case 0xCC:
            case 0xD4:
            case 0xDC:
                ret_pc = (uint16_t)(pc + 3u);
                break;
            default:
                ret_pc = (uint16_t)(pc + 1u);
                break;
            }
            record_function_entry(cpu, ret_pc, "RET_AFTER_RAMCALL");
        }
    }
}

static void recomp_probe_seed_extra_entries_from_env(void)
{
    const char *path = getenv("GB_RECOMP_STATIC_EXTRA_ENTRIES_FILE");
    if (!path || path[0] == '\0')
        return;

    FILE *f = fopen(path, "r");
    if (!f)
        return;

    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p == '\0' || *p == '\r' || *p == '\n' || *p == '#')
            continue;

        unsigned bank = 0;
        unsigned addr = 0;
        unsigned key = 0;
        if (sscanf(p, "%x:%x", &bank, &addr) == 2 ||
            sscanf(p, "%x %x", &bank, &addr) == 2)
        {
            if (addr < 0x8000u)
                discover_function((size_t)bank, (uint16_t)addr, "EXTRA_SEED");
            continue;
        }
        if (sscanf(p, "%x", &key) == 1)
        {
            bank = (key >> 16) & 0xFFFFu;
            addr = key & 0xFFFFu;
            if (addr < 0x8000u)
                discover_function((size_t)bank, (uint16_t)addr, "EXTRA_SEED");
            continue;
        }
    }

    fclose(f);
}

void recomp_probe_run_static_reachability(uint16_t entry_pc, bool include_interrupts, bool include_boot_entry)
{
    if (!g_probe.enabled || !g_probe.mem)
        return;

    bool ref_scan_enabled = !env_flag_enabled("GB_RECOMP_STATIC_NO_REF_SCAN");
    bool ref_scan_include_jumps = !env_flag_enabled("GB_RECOMP_STATIC_CALL_SCAN_ONLY");

    discover_function(0, entry_pc, "ENTRY_STATIC");

    if (include_boot_entry)
    {
        discover_function(0, 0x0000u, "BOOT_STATIC");
    }

    if (include_interrupts)
    {
        static const uint16_t vectors[5] = {0x40u, 0x48u, 0x50u, 0x58u, 0x60u};
        for (size_t i = 0; i < 5; ++i)
        {
            discover_function(0, vectors[i], "INT_STATIC");
        }
    }

    recomp_probe_seed_extra_entries_from_env();

    if (ref_scan_enabled)
    {
        run_static_reference_scan(ref_scan_include_jumps);
    }
}

size_t recomp_probe_discovered_count(void)
{
    return g_probe.seen_count;
}

void recomp_probe_get_stats(RecompProbeStats *out_stats)
{
    if (!out_stats)
        return;

    out_stats->functions_discovered = g_probe.seen_count;
    out_stats->functions_processed = g_probe.processed_count;
    out_stats->branches_discovered = g_probe.branch_count;
    out_stats->paths_discovered = g_probe.path_count;
}

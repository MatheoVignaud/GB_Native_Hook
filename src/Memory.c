#include "Memory.h"
#include "CPU.h"

#include <SDL3/SDL.h>

#include <string.h>
#include <time.h>

static inline bool ppu_lcd_enabled(const MemoryState *mem)
{
    return (mem->memory.LCDC & 0x80u) != 0;
}

static inline uint8_t ppu_mode_bits(const MemoryState *mem)
{
    return (uint8_t)(mem->memory.STAT & 0x03u);
}

static inline bool ppu_vram_locked(const MemoryState *mem)
{
    return ppu_lcd_enabled(mem) && (ppu_mode_bits(mem) == 0x03u);
}

static inline bool ppu_oam_locked(const MemoryState *mem)
{
    if (!ppu_lcd_enabled(mem))
        return false;
    uint8_t mode = ppu_mode_bits(mem);
    return mode == 0x02u || mode == 0x03u;
}

static FILE *mbc_trace_file = NULL;
static bool mbc_trace_enabled = false;
static bool mbc_trace_inited = false;
static char mbc_trace_path[512] = {0};
static bool memory_logging_enabled = true;
static bool rom_read_trace_inited = false;
static bool rom_read_trace_enabled = false;
static char rom_read_trace_path[512] = {0};
static uint8_t *rom_read_bitmap = NULL;
static size_t rom_read_bitmap_size = 0;
static size_t rom_read_track_size = 0;
static size_t rom_read_unique_count = 0;
static uint8_t *rom_data_read_bitmap = NULL;
static size_t rom_data_read_bitmap_size = 0;
static size_t rom_data_read_unique_count = 0;
static SDL_Mutex *rom_trace_mutex = NULL;

static uint64_t mbc3_rtc_now_seconds(void);
static void mbc3_rtc_update(CartridgeState *cart);
static void cartridge_persist_save(CartridgeState *cart, bool force);

typedef struct
{
    char magic[8];
    uint8_t regs[5];
    uint8_t latched[5];
    uint8_t latch_state;
    uint8_t latched_valid;
    uint8_t _pad[6];
    uint64_t last_unix;
} CartridgeRtcPersist;

typedef struct
{
    bool active;
    uint16_t start;
    uint8_t len;
    uint8_t bytes[4];
} FetchPatchState;

static FetchPatchState fetch_patch = {0};

static void rom_trace_lock(void)
{
    if (!rom_trace_mutex)
    {
        rom_trace_mutex = SDL_CreateMutex();
    }
    if (rom_trace_mutex)
    {
        SDL_LockMutex(rom_trace_mutex);
    }
}

static void rom_trace_unlock(void)
{
    if (rom_trace_mutex)
    {
        SDL_UnlockMutex(rom_trace_mutex);
    }
}

void memory_set_logging(bool enabled)
{
    memory_logging_enabled = enabled;
}

void memory_set_fetch_patch(uint16_t start, const uint8_t *bytes, uint8_t len)
{
    fetch_patch.active = false;
    fetch_patch.start = start;
    fetch_patch.len = 0;
    memset(fetch_patch.bytes, 0, sizeof(fetch_patch.bytes));
    if (!bytes || len == 0)
        return;

    if (len > (uint8_t)sizeof(fetch_patch.bytes))
        len = (uint8_t)sizeof(fetch_patch.bytes);
    memcpy(fetch_patch.bytes, bytes, len);
    fetch_patch.len = len;
    fetch_patch.active = true;
}

void memory_clear_fetch_patch(void)
{
    fetch_patch.active = false;
    fetch_patch.start = 0;
    fetch_patch.len = 0;
    memset(fetch_patch.bytes, 0, sizeof(fetch_patch.bytes));
}

static void rom_read_trace_reset(size_t rom_size)
{
    rom_trace_lock();
    free(rom_read_bitmap);
    rom_read_bitmap = NULL;
    rom_read_bitmap_size = 0;
    free(rom_data_read_bitmap);
    rom_data_read_bitmap = NULL;
    rom_data_read_bitmap_size = 0;
    rom_read_track_size = rom_size;
    rom_read_unique_count = 0;
    rom_data_read_unique_count = 0;

    if (rom_size == 0)
    {
        rom_trace_unlock();
        return;
    }

    if (rom_read_trace_enabled)
    {
        rom_read_bitmap_size = (rom_size + 7u) / 8u;
        rom_read_bitmap = (uint8_t *)calloc(rom_read_bitmap_size, 1);
        if (!rom_read_bitmap)
        {
            rom_read_bitmap_size = 0;
            rom_read_trace_enabled = false;
            if (memory_logging_enabled)
            {
                fprintf(stderr, "[ROM-TRACE] failed to allocate bitmap, disabling trace.\n");
            }
        }
    }

    rom_data_read_bitmap_size = (rom_size + 7u) / 8u;
    rom_data_read_bitmap = (uint8_t *)calloc(rom_data_read_bitmap_size, 1);
    if (!rom_data_read_bitmap)
    {
        rom_data_read_bitmap_size = 0;
    }
    rom_trace_unlock();
}

static void rom_read_trace_init(void)
{
    if (rom_read_trace_inited)
        return;
    rom_read_trace_inited = true;

    const char *path = getenv("GB_ROM_READ_TRACE_PATH");
    if (!path || path[0] == '\0')
        return;

    size_t n = strlen(path);
    if (n >= sizeof(rom_read_trace_path))
        n = sizeof(rom_read_trace_path) - 1;
    memcpy(rom_read_trace_path, path, n);
    rom_read_trace_path[n] = '\0';
    rom_read_trace_enabled = true;
}

static void rom_read_trace_mark(size_t absolute)
{
    rom_trace_lock();
    if (!rom_read_trace_enabled || !rom_read_bitmap || absolute >= rom_read_track_size)
    {
        rom_trace_unlock();
        return;
    }

    size_t byte_index = absolute >> 3u;
    uint8_t mask = (uint8_t)(1u << (absolute & 7u));
    if ((rom_read_bitmap[byte_index] & mask) == 0)
    {
        rom_read_bitmap[byte_index] |= mask;
        rom_read_unique_count++;
    }
    rom_trace_unlock();
}

static void rom_data_read_mark(size_t absolute)
{
    rom_trace_lock();
    if (!rom_data_read_bitmap || absolute >= rom_read_track_size)
    {
        rom_trace_unlock();
        return;
    }

    size_t byte_index = absolute >> 3u;
    uint8_t mask = (uint8_t)(1u << (absolute & 7u));
    if ((rom_data_read_bitmap[byte_index] & mask) == 0)
    {
        rom_data_read_bitmap[byte_index] |= mask;
        rom_data_read_unique_count++;
    }
    rom_trace_unlock();
}

static void rom_read_trace_flush(void)
{
    rom_trace_lock();
    if (!rom_read_trace_enabled || !rom_read_bitmap || rom_read_trace_path[0] == '\0')
    {
        rom_trace_unlock();
        return;
    }

    FILE *f = fopen(rom_read_trace_path, "w");
    if (!f)
    {
        rom_trace_unlock();
        return;
    }

    fprintf(f, "# GB ROM read trace\n");
    fprintf(f, "# rom_size=%zu unique_reads=%zu\n", rom_read_track_size, rom_read_unique_count);

    size_t i = 0;
    while (i < rom_read_track_size)
    {
        size_t byte_index = i >> 3u;
        uint8_t mask = (uint8_t)(1u << (i & 7u));
        if ((rom_read_bitmap[byte_index] & mask) == 0)
        {
            i++;
            continue;
        }

        size_t start = i;
        size_t end = i;
        while ((end + 1u) < rom_read_track_size)
        {
            size_t next = end + 1u;
            size_t next_byte_index = next >> 3u;
            uint8_t next_mask = (uint8_t)(1u << (next & 7u));
            if ((rom_read_bitmap[next_byte_index] & next_mask) == 0)
                break;
            end = next;
        }

        if (start == end)
            fprintf(f, "%06zX\n", start);
        else
            fprintf(f, "%06zX-%06zX\n", start, end);

        i = end + 1u;
    }

    fclose(f);
    if (memory_logging_enabled)
    {
        printf("[ROM-TRACE] wrote %s (unique=%zu)\n", rom_read_trace_path, rom_read_unique_count);
    }
    rom_trace_unlock();
}

static void mbc_trace_init(void)
{
    if (mbc_trace_inited)
        return;
    mbc_trace_inited = true;

    const char *trace_env = getenv("GB_TRACE_MBC");
    if (!trace_env || trace_env[0] == '\0' || trace_env[0] == '0')
        return;

    const char *path = getenv("GB_TRACE_MBC_PATH");
    if (path && path[0] != '\0')
    {
        size_t n = strlen(path);
        if (n >= sizeof(mbc_trace_path))
            n = sizeof(mbc_trace_path) - 1;
        memcpy(mbc_trace_path, path, n);
        mbc_trace_path[n] = '\0';
    }
    else
    {
        snprintf(mbc_trace_path, sizeof(mbc_trace_path), "%s", "mbc_trace.log");
    }

    mbc_trace_file = fopen(mbc_trace_path, "w");
    if (!mbc_trace_file)
        return;

    mbc_trace_enabled = true;
    fprintf(mbc_trace_file, "MBC trace enabled\n");
    fflush(mbc_trace_file);
}

static void mbc_trace_log(const MemoryState *mem, const char *event, uint16_t address, uint8_t value)
{
    if (!mbc_trace_enabled || !mbc_trace_file || !mem)
        return;

    const CartridgeState *cart = &mem->cartridge;
    uint16_t pc = mem->cpu ? mem->cpu->PC : 0xFFFFu;
    fprintf(mbc_trace_file,
            "%s PC=%04X A=%04X V=%02X TYPE=%02X MBC=%d ROMB=%zu RAMB=%zu EN=%d M1(low=%02X hi=%02X mode=%u) M2=%02X M3(rom=%02X sel=%02X latch=%u) M5(rom=%03X ram=%02X)\n",
            event,
            pc,
            address,
            value,
            cart->cartridge_type,
            (int)cart->mbc_type,
            cart->rom_banks,
            cart->ram_banks,
            cart->ram_enabled ? 1 : 0,
            cart->mbc1_low5,
            cart->mbc1_high2,
            cart->mbc1_mode,
            cart->mbc2_rom_bank,
            cart->mbc3_rom_bank,
            cart->mbc3_ram_rtc_select,
            cart->mbc3_latch_state,
            cart->mbc5_rom_bank,
            cart->mbc5_ram_bank);
    fflush(mbc_trace_file);
}

static const char *cartridge_type_name(uint8_t type)
{
    switch (type)
    {
    case 0x00:
        return "ROM ONLY";
    case 0x01:
        return "MBC1";
    case 0x02:
        return "MBC1+RAM";
    case 0x03:
        return "MBC1+RAM+BATTERY";
    case 0x05:
        return "MBC2";
    case 0x06:
        return "MBC2+BATTERY";
    case 0x08:
        return "ROM+RAM";
    case 0x09:
        return "ROM+RAM+BATTERY";
    case 0x0F:
        return "MBC3+TIMER+BATTERY";
    case 0x10:
        return "MBC3+TIMER+RAM+BATTERY";
    case 0x11:
        return "MBC3";
    case 0x12:
        return "MBC3+RAM";
    case 0x13:
        return "MBC3+RAM+BATTERY";
    case 0x19:
        return "MBC5";
    case 0x1A:
        return "MBC5+RAM";
    case 0x1B:
        return "MBC5+RAM+BATTERY";
    case 0x1C:
        return "MBC5+RUMBLE";
    case 0x1D:
        return "MBC5+RUMBLE+RAM";
    case 0x1E:
        return "MBC5+RUMBLE+RAM+BATTERY";
    default:
        return "UNKNOWN";
    }
}

static size_t cartridge_rom_banks_from_header(uint8_t rom_size_code)
{
    switch (rom_size_code)
    {
    case 0x00:
        return 2;
    case 0x01:
        return 4;
    case 0x02:
        return 8;
    case 0x03:
        return 16;
    case 0x04:
        return 32;
    case 0x05:
        return 64;
    case 0x06:
        return 128;
    case 0x07:
        return 256;
    case 0x08:
        return 512;
    case 0x52:
        return 72;
    case 0x53:
        return 80;
    case 0x54:
        return 96;
    default:
        return 0;
    }
}

static size_t cartridge_ram_size_from_header(uint8_t ram_size_code)
{
    switch (ram_size_code)
    {
    case 0x00:
        return 0;
    case 0x01:
        return 2 * 1024;
    case 0x02:
        return 8 * 1024;
    case 0x03:
        return 32 * 1024;
    case 0x04:
        return 128 * 1024;
    case 0x05:
        return 64 * 1024;
    default:
        return 0;
    }
}

static size_t cartridge_size_to_rom_banks(size_t rom_size)
{
    if (rom_size == 0)
        return 0;
    return (rom_size + 0x3FFFu) / 0x4000u;
}

static size_t cartridge_size_to_ram_banks(size_t ram_size)
{
    if (ram_size == 0)
        return 0;
    return (ram_size + 0x1FFFu) / 0x2000u;
}

static void persist_path_from_rom(const char *rom_path, const char *ext, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    out[0] = '\0';
    if (!rom_path || rom_path[0] == '\0')
        return;

    snprintf(out, out_size, "%s", rom_path);
    char *slash1 = strrchr(out, '/');
    char *slash2 = strrchr(out, '\\');
    char *slash = slash1;
    if (!slash || (slash2 && slash2 > slash))
        slash = slash2;
    char *dot = strrchr(out, '.');
    if (dot && (!slash || dot > slash))
    {
        *dot = '\0';
    }
    if (ext && ext[0] != '\0')
    {
        size_t len = strlen(out);
        if (len + strlen(ext) + 1 < out_size)
            strcat(out, ext);
    }
}

static void cartridge_set_persist_paths(CartridgeState *cart, const char *rom_path)
{
    if (!cart)
        return;
    if (!rom_path || rom_path[0] == '\0')
        return;
    persist_path_from_rom(rom_path, ".sav", cart->save_path, sizeof(cart->save_path));
    persist_path_from_rom(rom_path, ".rtc", cart->rtc_path, sizeof(cart->rtc_path));
}

static void cartridge_persist_load(CartridgeState *cart)
{
    if (!cart || cart->persist_loaded || !cart->has_battery)
    {
        if (cart)
            cart->persist_loaded = true;
        return;
    }

    if (cart->save_path[0] == '\0' && cart->rtc_path[0] == '\0')
    {
        return;
    }

    if (cart->has_ram && cart->ram_data && cart->ram_size > 0 && cart->save_path[0] != '\0')
    {
        FILE *f = fopen(cart->save_path, "rb");
        if (f)
        {
            size_t got = fread(cart->ram_data, 1, cart->ram_size, f);
            if (got < cart->ram_size)
            {
                memset(cart->ram_data + got, 0, cart->ram_size - got);
            }
            fclose(f);
        }
    }

    if (cart->has_rtc && cart->rtc_path[0] != '\0')
    {
        FILE *f = fopen(cart->rtc_path, "rb");
        if (f)
        {
            CartridgeRtcPersist rtc = {0};
            size_t got = fread(&rtc, 1, sizeof(rtc), f);
            fclose(f);
            if (got == sizeof(rtc) && memcmp(rtc.magic, "GBRTC1", 6) == 0)
            {
                memcpy(cart->mbc3_rtc_regs, rtc.regs, sizeof(cart->mbc3_rtc_regs));
                memcpy(cart->mbc3_rtc_latched, rtc.latched, sizeof(cart->mbc3_rtc_latched));
                cart->mbc3_latch_state = (uint8_t)(rtc.latch_state & 0x01u);
                cart->mbc3_rtc_latched_valid = (rtc.latched_valid != 0);
                cart->mbc3_rtc_last_unix = rtc.last_unix;
            }
        }
    }

    cart->ram_dirty = false;
    cart->rtc_dirty = false;
    cart->persist_loaded = true;
}

static void cartridge_persist_save(CartridgeState *cart, bool force)
{
    if (!cart || !cart->has_battery)
        return;

    if (cart->has_ram && cart->ram_data && cart->ram_size > 0 &&
        cart->save_path[0] != '\0' && (force || cart->ram_dirty))
    {
        FILE *f = fopen(cart->save_path, "wb");
        if (f)
        {
            size_t wrote = fwrite(cart->ram_data, 1, cart->ram_size, f);
            fclose(f);
            if (wrote == cart->ram_size)
                cart->ram_dirty = false;
        }
    }

    if (cart->has_rtc && cart->rtc_path[0] != '\0' && (force || cart->rtc_dirty))
    {
        mbc3_rtc_update(cart);
        CartridgeRtcPersist rtc = {0};
        memcpy(rtc.magic, "GBRTC1", 6);
        memcpy(rtc.regs, cart->mbc3_rtc_regs, sizeof(rtc.regs));
        memcpy(rtc.latched, cart->mbc3_rtc_latched, sizeof(rtc.latched));
        rtc.latch_state = (uint8_t)(cart->mbc3_latch_state & 0x01u);
        rtc.latched_valid = cart->mbc3_rtc_latched_valid ? 1u : 0u;
        rtc.last_unix = cart->mbc3_rtc_last_unix;

        FILE *f = fopen(cart->rtc_path, "wb");
        if (f)
        {
            size_t wrote = fwrite(&rtc, 1, sizeof(rtc), f);
            fclose(f);
            if (wrote == sizeof(rtc))
                cart->rtc_dirty = false;
        }
    }
}

static void cartridge_reset(CartridgeState *cart)
{
    memset(cart, 0, sizeof(*cart));
    cart->mbc_type = MBC_NONE;
    cart->mbc1_low5 = 1;
    cart->mbc2_rom_bank = 1;
    cart->mbc3_rom_bank = 1;
    cart->mbc5_rom_bank = 1;
}

static void cartridge_release(CartridgeState *cart)
{
    free(cart->rom_data);
    free(cart->ram_data);
    cartridge_reset(cart);
}

static void cartridge_configure(CartridgeState *cart, const uint8_t *rom_data, size_t rom_size)
{
    if (rom_size < 0x150)
    {
        return;
    }

    cart->cartridge_type = rom_data[0x0147];
    uint8_t rom_size_code = rom_data[0x0148];
    uint8_t ram_size_code = rom_data[0x0149];

    switch (cart->cartridge_type)
    {
    case 0x00:
        cart->mbc_type = MBC_NONE;
        break;
    case 0x01:
        cart->mbc_type = MBC1;
        break;
    case 0x02:
        cart->mbc_type = MBC1;
        cart->has_ram = true;
        break;
    case 0x03:
        cart->mbc_type = MBC1;
        cart->has_ram = true;
        cart->has_battery = true;
        break;
    case 0x05:
        cart->mbc_type = MBC2;
        cart->has_ram = true;
        break;
    case 0x06:
        cart->mbc_type = MBC2;
        cart->has_ram = true;
        cart->has_battery = true;
        break;
    case 0x08:
        cart->mbc_type = MBC_NONE;
        cart->has_ram = true;
        break;
    case 0x09:
        cart->mbc_type = MBC_NONE;
        cart->has_ram = true;
        cart->has_battery = true;
        break;
    case 0x0F:
        cart->mbc_type = MBC3;
        cart->has_battery = true;
        cart->has_rtc = true;
        break;
    case 0x10:
        cart->mbc_type = MBC3;
        cart->has_ram = true;
        cart->has_battery = true;
        cart->has_rtc = true;
        break;
    case 0x11:
        cart->mbc_type = MBC3;
        break;
    case 0x12:
        cart->mbc_type = MBC3;
        cart->has_ram = true;
        break;
    case 0x13:
        cart->mbc_type = MBC3;
        cart->has_ram = true;
        cart->has_battery = true;
        break;
    case 0x19:
        cart->mbc_type = MBC5;
        break;
    case 0x1A:
        cart->mbc_type = MBC5;
        cart->has_ram = true;
        break;
    case 0x1B:
        cart->mbc_type = MBC5;
        cart->has_ram = true;
        cart->has_battery = true;
        break;
    case 0x1C:
        cart->mbc_type = MBC5;
        break;
    case 0x1D:
        cart->mbc_type = MBC5;
        cart->has_ram = true;
        break;
    case 0x1E:
        cart->mbc_type = MBC5;
        cart->has_ram = true;
        cart->has_battery = true;
        break;
    default:
        // Fallback to ROM-only behavior for unknown types.
        cart->mbc_type = MBC_NONE;
        break;
    }

    size_t header_rom_banks = cartridge_rom_banks_from_header(rom_size_code);
    size_t file_rom_banks = cartridge_size_to_rom_banks(rom_size);
    if (header_rom_banks == 0 || header_rom_banks < file_rom_banks)
    {
        cart->rom_banks = file_rom_banks;
    }
    else
    {
        cart->rom_banks = header_rom_banks;
    }

    if (cart->mbc_type == MBC2)
    {
        cart->ram_size = 512;
    }
    else if (cart->has_ram)
    {
        cart->ram_size = cartridge_ram_size_from_header(ram_size_code);
    }

    cart->ram_banks = cartridge_size_to_ram_banks(cart->ram_size);

    if (cart->mbc_type == MBC_NONE)
    {
        cart->ram_enabled = true;
    }

    if (cart->has_rtc)
    {
        cart->mbc3_rtc_last_unix = mbc3_rtc_now_seconds();
        cart->mbc3_rtc_latched_valid = false;
    }
}

static size_t cartridge_wrap_bank(size_t bank, size_t bank_count)
{
    if (bank_count == 0)
        return 0;
    return bank % bank_count;
}

static uint64_t mbc3_rtc_now_seconds(void)
{
    time_t now = time(NULL);
    if (now < 0)
        return 0;
    return (uint64_t)now;
}

static void mbc3_rtc_update(CartridgeState *cart)
{
    if (!cart || !cart->has_rtc)
        return;

    uint64_t now = mbc3_rtc_now_seconds();
    if (now == 0)
        return;

    if (cart->mbc3_rtc_last_unix == 0)
    {
        cart->mbc3_rtc_last_unix = now;
        return;
    }

    if (cart->mbc3_rtc_regs[4] & 0x40u)
    {
        // Halt bit set: RTC stops advancing.
        cart->mbc3_rtc_last_unix = now;
        return;
    }

    uint64_t elapsed = now - cart->mbc3_rtc_last_unix;
    if (elapsed == 0)
        return;
    cart->mbc3_rtc_last_unix = now;

    uint64_t seconds = (uint64_t)(cart->mbc3_rtc_regs[0] % 60u);
    uint64_t minutes = (uint64_t)(cart->mbc3_rtc_regs[1] % 60u);
    uint64_t hours = (uint64_t)(cart->mbc3_rtc_regs[2] % 24u);
    uint64_t days = (uint64_t)cart->mbc3_rtc_regs[3] | ((uint64_t)(cart->mbc3_rtc_regs[4] & 0x01u) << 8u);

    uint64_t total = seconds + (minutes * 60u) + (hours * 3600u) + (days * 86400u);
    total += elapsed;

    uint64_t new_days = total / 86400u;
    uint8_t carry = (cart->mbc3_rtc_regs[4] & 0x80u);
    if (new_days > 511u)
    {
        carry = 0x80u;
        new_days %= 512u;
    }

    uint64_t rem = total % 86400u;
    cart->mbc3_rtc_regs[2] = (uint8_t)(rem / 3600u);
    rem %= 3600u;
    cart->mbc3_rtc_regs[1] = (uint8_t)(rem / 60u);
    cart->mbc3_rtc_regs[0] = (uint8_t)(rem % 60u);
    cart->mbc3_rtc_regs[3] = (uint8_t)(new_days & 0xFFu);
    cart->mbc3_rtc_regs[4] = (uint8_t)((cart->mbc3_rtc_regs[4] & 0x40u) | (uint8_t)((new_days >> 8u) & 0x01u) | carry);
}

static void mbc3_rtc_latch(CartridgeState *cart)
{
    if (!cart || !cart->has_rtc)
        return;
    mbc3_rtc_update(cart);
    for (int i = 0; i < 5; ++i)
        cart->mbc3_rtc_latched[i] = cart->mbc3_rtc_regs[i];
    cart->mbc3_rtc_latched_valid = true;
}

static uint8_t cartridge_rom_read(const MemoryState *mem, uint16_t address)
{
    const CartridgeState *cart = &mem->cartridge;
    if (!cart->rom_data || cart->rom_size == 0 || address >= 0x8000)
    {
        return 0xFF;
    }

    size_t bank = 0;
    size_t bank_offset = (size_t)(address & 0x3FFF);

    switch (cart->mbc_type)
    {
    case MBC1:
        if (address < 0x4000)
        {
            if (cart->mbc1_mode)
            {
                bank = (size_t)(cart->mbc1_high2 & 0x03) << 5;
            }
            else
            {
                bank = 0;
            }
        }
        else
        {
            uint8_t low = (uint8_t)(cart->mbc1_low5 & 0x1F);
            if (low == 0)
                low = 1;
            bank = low | ((size_t)(cart->mbc1_high2 & 0x03) << 5);
        }
        break;
    case MBC2:
        bank = (address < 0x4000) ? 0 : (size_t)(cart->mbc2_rom_bank & 0x0F);
        if (address >= 0x4000 && bank == 0)
            bank = 1;
        break;
    case MBC3:
        bank = (address < 0x4000) ? 0 : (size_t)(cart->mbc3_rom_bank & 0x7F);
        if (address >= 0x4000 && bank == 0)
            bank = 1;
        break;
    case MBC5:
        bank = (address < 0x4000) ? 0 : (size_t)(cart->mbc5_rom_bank & 0x01FF);
        break;
    case MBC_NONE:
    default:
        bank = (address < 0x4000) ? 0 : 1;
        break;
    }

    bank = cartridge_wrap_bank(bank, cart->rom_banks);
    size_t absolute = bank * 0x4000u + bank_offset;
    if (absolute >= cart->rom_size)
    {
        return 0xFF;
    }
    rom_read_trace_mark(absolute);
    if (mem->cpu && mem->cpu->profile_data_reads_active)
    {
        rom_data_read_mark(absolute);
    }
    return cart->rom_data[absolute];
}

static uint8_t cartridge_ram_read(MemoryState *mem, uint16_t address)
{
    CartridgeState *cart = &mem->cartridge;
    if (address < 0xA000 || address > 0xBFFF || !cart->has_ram || !cart->ram_data || cart->ram_size == 0)
    {
        return 0xFF;
    }

    if (!cart->ram_enabled && cart->mbc_type != MBC_NONE)
    {
        return 0xFF;
    }

    size_t offset = (size_t)(address - 0xA000);
    size_t bank = 0;

    switch (cart->mbc_type)
    {
    case MBC1:
        bank = cart->mbc1_mode ? (size_t)(cart->mbc1_high2 & 0x03) : 0;
        break;
    case MBC2:
    {
        size_t idx = offset & 0x01FFu;
        if (idx >= cart->ram_size)
            return 0xFF;
        return (uint8_t)(0xF0u | (cart->ram_data[idx] & 0x0Fu));
    }
    case MBC3:
        if (cart->mbc3_ram_rtc_select <= 0x03)
        {
            bank = cart->mbc3_ram_rtc_select;
        }
        else if (cart->has_rtc && cart->mbc3_ram_rtc_select >= 0x08 && cart->mbc3_ram_rtc_select <= 0x0C)
        {
            mbc3_rtc_update(cart);
            uint8_t idx = (uint8_t)(cart->mbc3_ram_rtc_select - 0x08u);
            if (idx < 5)
            {
                if (cart->mbc3_rtc_latched_valid)
                    return cart->mbc3_rtc_latched[idx];
                return cart->mbc3_rtc_regs[idx];
            }
            return 0xFF;
        }
        else
        {
            return 0xFF;
        }
        break;
    case MBC5:
        bank = (size_t)(cart->mbc5_ram_bank & 0x0F);
        break;
    case MBC_NONE:
    default:
        bank = 0;
        break;
    }

    bank = cartridge_wrap_bank(bank, cart->ram_banks == 0 ? 1 : cart->ram_banks);
    size_t absolute = bank * 0x2000u + offset;
    if (absolute >= cart->ram_size)
    {
        return 0xFF;
    }
    return cart->ram_data[absolute];
}

static void cartridge_ram_write(MemoryState *mem, uint16_t address, uint8_t value)
{
    CartridgeState *cart = &mem->cartridge;
    if (address < 0xA000 || address > 0xBFFF || !cart->has_ram || !cart->ram_data || cart->ram_size == 0)
    {
        return;
    }

    if (!cart->ram_enabled && cart->mbc_type != MBC_NONE)
    {
        return;
    }

    size_t offset = (size_t)(address - 0xA000);
    size_t bank = 0;

    switch (cart->mbc_type)
    {
    case MBC1:
        bank = cart->mbc1_mode ? (size_t)(cart->mbc1_high2 & 0x03) : 0;
        break;
    case MBC2:
    {
        size_t idx = offset & 0x01FFu;
        if (idx < cart->ram_size)
        {
            cart->ram_data[idx] = (uint8_t)(value & 0x0Fu);
            if (cart->has_battery)
                cart->ram_dirty = true;
        }
        return;
    }
    case MBC3:
        if (cart->mbc3_ram_rtc_select <= 0x03)
        {
            bank = cart->mbc3_ram_rtc_select;
        }
        else if (cart->has_rtc && cart->mbc3_ram_rtc_select >= 0x08 && cart->mbc3_ram_rtc_select <= 0x0C)
        {
            mbc3_rtc_update(cart);
            uint8_t idx = (uint8_t)(cart->mbc3_ram_rtc_select - 0x08u);
            if (idx < 5)
            {
                switch (idx)
                {
                case 0:
                    cart->mbc3_rtc_regs[0] = (uint8_t)(value % 60u);
                    break;
                case 1:
                    cart->mbc3_rtc_regs[1] = (uint8_t)(value % 60u);
                    break;
                case 2:
                    cart->mbc3_rtc_regs[2] = (uint8_t)(value % 24u);
                    break;
                case 3:
                    cart->mbc3_rtc_regs[3] = value;
                    break;
                case 4:
                    cart->mbc3_rtc_regs[4] = (uint8_t)(value & 0xC1u); // day high + halt + carry
                    break;
                default:
                    break;
                }
                cart->mbc3_rtc_last_unix = mbc3_rtc_now_seconds();
                if (cart->has_battery)
                    cart->rtc_dirty = true;
            }
            return;
        }
        else
        {
            return;
        }
        break;
    case MBC5:
        bank = (size_t)(cart->mbc5_ram_bank & 0x0F);
        break;
    case MBC_NONE:
    default:
        bank = 0;
        break;
    }

    bank = cartridge_wrap_bank(bank, cart->ram_banks == 0 ? 1 : cart->ram_banks);
    size_t absolute = bank * 0x2000u + offset;
    if (absolute < cart->ram_size)
    {
        cart->ram_data[absolute] = value;
        if (cart->has_battery)
            cart->ram_dirty = true;
    }
}

static void cartridge_write_control(MemoryState *mem, uint16_t address, uint8_t value)
{
    CartridgeState *cart = &mem->cartridge;
    if (!cart->rom_data || address >= 0x8000)
    {
        return;
    }

    bool changed = false;
    bool flush_battery = false;

    switch (cart->mbc_type)
    {
    case MBC1:
        if (address <= 0x1FFF)
        {
            bool ram_enable = ((value & 0x0F) == 0x0A);
            if (cart->ram_enabled != ram_enable)
            {
                if (cart->ram_enabled && !ram_enable)
                    flush_battery = true;
                cart->ram_enabled = ram_enable;
                changed = true;
            }
        }
        else if (address <= 0x3FFF)
        {
            uint8_t low5 = (uint8_t)(value & 0x1F);
            if (low5 == 0)
                low5 = 1;
            if (cart->mbc1_low5 != low5)
            {
                cart->mbc1_low5 = low5;
                changed = true;
            }
        }
        else if (address <= 0x5FFF)
        {
            uint8_t high2 = (uint8_t)(value & 0x03);
            if (cart->mbc1_high2 != high2)
            {
                cart->mbc1_high2 = high2;
                changed = true;
            }
        }
        else
        {
            uint8_t mode = (uint8_t)(value & 0x01);
            if (cart->mbc1_mode != mode)
            {
                cart->mbc1_mode = mode;
                changed = true;
            }
        }
        break;
    case MBC2:
        if (address <= 0x3FFF)
        {
            if ((address & 0x0100) == 0)
            {
                bool ram_enable = ((value & 0x0F) == 0x0A);
                if (cart->ram_enabled != ram_enable)
                {
                    if (cart->ram_enabled && !ram_enable)
                        flush_battery = true;
                    cart->ram_enabled = ram_enable;
                    changed = true;
                }
            }
            else
            {
                uint8_t bank = (uint8_t)(value & 0x0F);
                if (bank == 0)
                    bank = 1;
                if (cart->mbc2_rom_bank != bank)
                {
                    cart->mbc2_rom_bank = bank;
                    changed = true;
                }
            }
        }
        break;
    case MBC3:
        if (address <= 0x1FFF)
        {
            bool ram_enable = ((value & 0x0F) == 0x0A);
            if (cart->ram_enabled != ram_enable)
            {
                if (cart->ram_enabled && !ram_enable)
                    flush_battery = true;
                cart->ram_enabled = ram_enable;
                changed = true;
            }
        }
        else if (address <= 0x3FFF)
        {
            mbc3_rtc_update(cart);
            uint8_t bank = (uint8_t)(value & 0x7F);
            if (bank == 0)
                bank = 1;
            if (cart->mbc3_rom_bank != bank)
            {
                cart->mbc3_rom_bank = bank;
                changed = true;
            }
        }
        else if (address <= 0x5FFF)
        {
            mbc3_rtc_update(cart);
            uint8_t select = (uint8_t)(value & 0x0F);
            if (cart->mbc3_ram_rtc_select != select)
            {
                cart->mbc3_ram_rtc_select = select;
                changed = true;
            }
        }
        else
        {
            uint8_t latch = (uint8_t)(value & 0x01);
            if (cart->mbc3_latch_state == 0 && latch == 1)
            {
                mbc3_rtc_latch(cart);
                changed = true;
            }
            if (cart->mbc3_latch_state != latch)
            {
                cart->mbc3_latch_state = latch;
                changed = true;
            }
        }
        break;
    case MBC5:
        if (address <= 0x1FFF)
        {
            bool ram_enable = ((value & 0x0F) == 0x0A);
            if (cart->ram_enabled != ram_enable)
            {
                if (cart->ram_enabled && !ram_enable)
                    flush_battery = true;
                cart->ram_enabled = ram_enable;
                changed = true;
            }
        }
        else if (address <= 0x2FFF)
        {
            uint16_t bank = (uint16_t)((cart->mbc5_rom_bank & 0x0100u) | value);
            if (cart->mbc5_rom_bank != bank)
            {
                cart->mbc5_rom_bank = bank;
                changed = true;
            }
        }
        else if (address <= 0x3FFF)
        {
            uint16_t bank = (uint16_t)((cart->mbc5_rom_bank & 0x00FFu) | ((value & 0x01u) << 8u));
            if (cart->mbc5_rom_bank != bank)
            {
                cart->mbc5_rom_bank = bank;
                changed = true;
            }
        }
        else if (address <= 0x5FFF)
        {
            uint8_t bank = (uint8_t)(value & 0x0F);
            if (cart->mbc5_ram_bank != bank)
            {
                cart->mbc5_ram_bank = bank;
                changed = true;
            }
        }
        break;
    case MBC_NONE:
    default:
        break;
    }

    if (changed)
    {
        mbc_trace_log(mem, "CTRL", address, value);
    }
    if (flush_battery && cart->has_battery)
    {
        cartridge_persist_save(cart, false);
    }
}

static uint8_t joypad_read(const MemoryState *mem)
{
    uint8_t select = mem->joypad_select & 0x30;
    uint8_t value = (uint8_t)(0xC0 | select);
    uint8_t low = 0x0F;

    // P15 (bit 5) low: select action buttons (A, B, Select, Start)
    if ((select & 0x20) == 0)
    {
        low &= mem->joypad_buttons;
    }
    // P14 (bit 4) low: select d-pad (Right, Left, Up, Down)
    if ((select & 0x10) == 0)
    {
        low &= mem->joypad_dpad;
    }

    value |= low;
    return value;
}

static bool joypad_is_dpad_input(JoypadInput input)
{
    switch (input)
    {
    case JOYPAD_RIGHT:
    case JOYPAD_LEFT:
    case JOYPAD_UP:
    case JOYPAD_DOWN:
        return true;
    case JOYPAD_A:
    case JOYPAD_B:
    case JOYPAD_SELECT:
    case JOYPAD_START:
        return false;
    default:
        return true;
    }
}

static uint8_t joypad_input_bit(JoypadInput input)
{
    switch (input)
    {
    case JOYPAD_RIGHT:
    case JOYPAD_A:
        return 0x01; // bit 0
    case JOYPAD_LEFT:
    case JOYPAD_B:
        return 0x02; // bit 1
    case JOYPAD_UP:
    case JOYPAD_SELECT:
        return 0x04; // bit 2
    case JOYPAD_DOWN:
    case JOYPAD_START:
        return 0x08; // bit 3
    default:
        return 0x00;
    }
}

static uint8_t memory_raw_read(MemoryState *mem, uint16_t address)
{
    if (!mem->memory.BOOT && mem->bios_enabled && address < 0x0100)
    {
        return mem->bios[address];
    }

    if (address < 0x8000)
    {
        if (mem->cartridge.rom_data)
            return cartridge_rom_read(mem, address);
        return mem->memory.data[address];
    }

    if (address >= 0xA000 && address <= 0xBFFF)
    {
        if (mem->cartridge.rom_data)
            return cartridge_ram_read(mem, address);
        return mem->memory.data[address];
    }

    if (address >= 0xE000 && address <= 0xFDFF)
    {
        address = (uint16_t)(address - 0x2000);
    }
    if (address >= 0xFEA0 && address <= 0xFEFF)
    {
        return 0x00;
    }
    return mem->memory.data[address];
}

static void memory_raw_write(MemoryState *mem, uint16_t address, uint8_t value)
{
    if (address < 0x8000)
    {
        cartridge_write_control(mem, address, value);
        return;
    }

    if (address >= 0xA000 && address <= 0xBFFF)
    {
        if (mem->cartridge.rom_data)
        {
            cartridge_ram_write(mem, address, value);
            return;
        }
        mem->memory.data[address] = value;
        return;
    }

    if (address >= 0xE000 && address <= 0xFDFF)
    {
        uint16_t mirror = (uint16_t)(address - 0x2000);
        mem->memory.data[address] = value;
        mem->memory.data[mirror] = value;
        return;
    }

    if (address >= 0xFEA0 && address <= 0xFEFF)
    {
        return;
    }

    mem->memory.data[address] = value;
}

void memory_init(MemoryState *mem)
{
    memset(mem, 0, sizeof(*mem));
    mbc_trace_init();
    rom_read_trace_init();
    memory_clear_fetch_patch();
    cartridge_reset(&mem->cartridge);
    rom_read_trace_reset(0);
    mem->joypad_read_count = 0;
    mem->bios_enabled = false;
    mem->joypad_buttons = 0x0F;
    mem->joypad_dpad = 0x0F;
    mem->joypad_select = 0x30;
    mem->memory.P1_JOYP = (uint8_t)(0xC0 | mem->joypad_select | 0x0F);
}

void memory_shutdown(MemoryState *mem)
{
    if (!mem)
        return;
    cartridge_persist_save(&mem->cartridge, true);
    memory_clear_fetch_patch();
    rom_read_trace_flush();
    rom_read_trace_reset(0);
    cartridge_release(&mem->cartridge);
}

static int load_rom_image(MemoryState *mem, uint8_t *rom_data, size_t rom_size, const char *rom_path_hint)
{
    cartridge_release(&mem->cartridge);
    mem->cartridge.rom_data = rom_data;
    mem->cartridge.rom_size = rom_size;
    cartridge_configure(&mem->cartridge, rom_data, rom_size);
    cartridge_set_persist_paths(&mem->cartridge, rom_path_hint);

    if (mem->cartridge.has_ram && mem->cartridge.ram_size > 0)
    {
        mem->cartridge.ram_data = (uint8_t *)calloc(1, mem->cartridge.ram_size);
        if (!mem->cartridge.ram_data)
        {
            cartridge_release(&mem->cartridge);
            return -1;
        }
    }
    cartridge_persist_load(&mem->cartridge);

    memset(mem->memory.rom, 0xFF, sizeof(mem->memory.rom));
    size_t copy_size = mem->cartridge.rom_size < sizeof(mem->memory.rom) ? mem->cartridge.rom_size : sizeof(mem->memory.rom);
    memcpy(mem->memory.rom, mem->cartridge.rom_data, copy_size);
    rom_read_trace_reset(mem->cartridge.rom_size);

    if (memory_logging_enabled)
    {
        printf("Cartridge: type=%02X (%s), MBC=%d, ROM=%zu banks, RAM=%zu bytes\n",
               mem->cartridge.cartridge_type,
               cartridge_type_name(mem->cartridge.cartridge_type),
               (int)mem->cartridge.mbc_type,
               mem->cartridge.rom_banks,
               mem->cartridge.ram_size);
    }

    mbc_trace_log(mem, "LOAD", 0, 0);

    return 0;
}

int load_rom(const char *path, MemoryState *mem)
{
    mbc_trace_init();
    rom_read_trace_init();

    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    if (size <= 0)
    {
        fclose(f);
        return -1;
    }

    uint8_t *rom_data = (uint8_t *)malloc((size_t)size);
    if (!rom_data)
    {
        fclose(f);
        return -1;
    }

    if (fread(rom_data, 1, (size_t)size, f) != (size_t)size)
    {
        free(rom_data);
        fclose(f);
        return -1;
    }
    fclose(f);

    return load_rom_image(mem, rom_data, (size_t)size, path);
}

int load_rom_from_buffer(const uint8_t *data, size_t size, MemoryState *mem)
{
    mbc_trace_init();
    rom_read_trace_init();

    if (!data || size == 0)
        return -1;

    uint8_t *rom_data = (uint8_t *)malloc(size);
    if (!rom_data)
        return -1;

    memcpy(rom_data, data, size);
    return load_rom_image(mem, rom_data, size, NULL);
}

void memory_set_save_path_hint(MemoryState *mem, const char *rom_path)
{
    if (!mem)
        return;
    cartridge_set_persist_paths(&mem->cartridge, rom_path);
    if (!mem->cartridge.persist_loaded)
        cartridge_persist_load(&mem->cartridge);
}

int load_bios(const char *path, uint8_t *bios)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size != 0x100)
    {
        fclose(f);
        return -1;
    }
    if (fread(bios, 1, size, f) != size)
    {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static inline void memory_probe_data_write(MemoryState *mem, uint16_t address, uint8_t value)
{
    (void)mem; (void)address; (void)value;
}

uint8_t memory_read(MemoryState *mem, uint16_t address)
{
    uint8_t value = 0xFF;
    if (fetch_patch.active)
    {
        uint16_t delta = (uint16_t)(address - fetch_patch.start);
        if (delta < fetch_patch.len)
        {
            value = fetch_patch.bytes[delta];
            return value;
        }
    }

    if (address == 0xFF00)
    {
        mem->joypad_read_count++;
        value = joypad_read(mem);
        mem->memory.P1_JOYP = value;
        return value;
    }

    if (address >= 0x8000 && address <= 0x9FFF && ppu_vram_locked(mem))
        return 0xFF;

    if (address >= 0xFE00 && address <= 0xFE9F && ppu_oam_locked(mem))
        return 0xFF;

    return memory_raw_read(mem, address);
}

void memory_write(MemoryState *mem, uint16_t address, uint8_t value)
{
    if (mem->bios_enabled && address == 0xFF50 && value == 1)
    {
        if (memory_logging_enabled)
        {
            printf("Disabling BIOS mapping at address  \n");
        }
        mem->bios_enabled = false;
    }

    if (address >= 0x8000 && address <= 0x9FFF && ppu_vram_locked(mem))
    {
        return;
    }

    if (address >= 0xFE00 && address <= 0xFE9F && ppu_oam_locked(mem))
    {
        return;
    }

    switch (address)
    {
    case 0xFF00:
    {
        mem->joypad_select = value & 0x30;
        uint8_t joy = joypad_read(mem);
        mem->memory.P1_JOYP = joy;
        memory_probe_data_write(mem, address, joy);
        return;
    }
    case 0xFF04: // DIV
        if (mem->cpu)
        {
            cpu_timer_div_write(mem->cpu);
            memory_probe_data_write(mem, address, mem->memory.DIV);
        }
        else
        {
            mem->memory.DIV = 0;
            memory_probe_data_write(mem, address, mem->memory.DIV);
        }
        return;
    case 0xFF05: // TIMA
        if (mem->cpu)
        {
            cpu_timer_tima_write(mem->cpu, value);
            memory_probe_data_write(mem, address, mem->memory.TIMA);
        }
        else
        {
            mem->memory.TIMA = value;
            memory_probe_data_write(mem, address, mem->memory.TIMA);
        }
        return;
    case 0xFF06: // TMA
        if (mem->cpu)
        {
            cpu_timer_tma_write(mem->cpu, value);
            memory_probe_data_write(mem, address, mem->memory.TMA);
        }
        else
        {
            mem->memory.TMA = value;
            memory_probe_data_write(mem, address, mem->memory.TMA);
        }
        return;
    case 0xFF07: // TAC
        if (mem->cpu)
        {
            cpu_timer_tac_write(mem->cpu, value);
            memory_probe_data_write(mem, address, mem->memory.TAC);
        }
        else
        {
            mem->memory.TAC = (uint8_t)(value | 0xF8);
            memory_probe_data_write(mem, address, mem->memory.TAC);
        }
        return;
    case 0xFF0F:
        mem->memory.IF = value & 0x1F;
        memory_probe_data_write(mem, address, mem->memory.IF);
        return;
    case 0xFF46: // DMA
    {
        mem->memory.DMA = value;
        memory_probe_data_write(mem, address, mem->memory.DMA);
        uint16_t source = (uint16_t)(value << 8);
        for (int i = 0; i < 0xA0; ++i)
        {
            uint8_t data = memory_read(mem, (uint16_t)(source + i));
            mem->memory.oam[i] = data;
            memory_probe_data_write(mem, (uint16_t)(0xFE00u + i), data);
        }
        if (mem->cpu)
        {
            cpu_dma_transfer(mem->cpu, source);
        }
        return;
    }
    case 0xFF41:
        uint8_t keep_ro = mem->memory.STAT & 0x07;
        uint8_t new_rw = (value & 0x78);
        mem->memory.STAT = keep_ro | new_rw;
        memory_probe_data_write(mem, address, mem->memory.STAT);
        return;
    case 0xFF44:
        mem->memory.LY = 0;
        memory_probe_data_write(mem, address, mem->memory.LY);
        return;
    case 0xFFFF:
        mem->memory.IE = value & 0x1F;
        memory_probe_data_write(mem, address, mem->memory.IE);
        return;

    default:
        break;
    }

    memory_raw_write(mem, address, value);
    memory_probe_data_write(mem, address, value);
}

void memory_set_button_state(MemoryState *mem, JoypadInput input, bool pressed)
{
    uint8_t bit = joypad_input_bit(input);
    if (bit == 0)
        return;

    bool is_dpad = joypad_is_dpad_input(input);
    uint8_t old_joyp = joypad_read(mem);

    if (is_dpad)
    {
        if (pressed)
        {
            mem->joypad_dpad &= (uint8_t)~bit;
        }
        else
        {
            mem->joypad_dpad |= bit;
        }
    }
    else
    {
        if (pressed)
        {
            mem->joypad_buttons &= (uint8_t)~bit;
        }
        else
        {
            mem->joypad_buttons |= bit;
        }
    }

    uint8_t new_joyp = joypad_read(mem);
    mem->memory.P1_JOYP = new_joyp;

    // Joypad interrupt is raised on 1->0 transition of an active input line.
    if (((old_joyp & (uint8_t)~new_joyp) & 0x0F) != 0)
    {
        mem->memory.IF |= IF_JOYPAD;
        mem->memory.IF &= 0x1F;
    }
}

void memory_flush_rom_read_trace(void)
{
    rom_read_trace_flush();
}

size_t memory_get_rom_read_unique_count(void)
{
    rom_trace_lock();
    size_t v = rom_read_unique_count;
    rom_trace_unlock();
    return v;
}

size_t memory_get_rom_read_track_size(void)
{
    rom_trace_lock();
    size_t v = rom_read_track_size;
    rom_trace_unlock();
    return v;
}

size_t memory_get_rom_data_read_unique_count(void)
{
    rom_trace_lock();
    size_t v = rom_data_read_unique_count;
    rom_trace_unlock();
    return v;
}

uint64_t memory_get_joypad_read_count(const MemoryState *mem)
{
    if (!mem)
        return 0u;
    return mem->joypad_read_count;
}

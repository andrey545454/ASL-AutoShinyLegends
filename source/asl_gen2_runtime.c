#include "asl_gen2_runtime.h"
#include "asl_platform.h"
#include <string.h>

#define PROBE_SCAN_BEGIN                    0x00100000u
#define PROBE_SCAN_END                      0x40000000u
#define PROBE_FRAME_BUDGET                  (256u * 1024u)
#define CRYSTAL_UPDATE_DVS_FILE_OFFSET       0x0003E9A8u
#define CRYSTAL_GENERATE_DVS_FILE_OFFSET     0x0003E9A0u
#define CRYSTAL_BANK0_SIZE                   0x00004000u
#define GATE_CONTROL_OPERAND_OFFSET          8u
#define GATE_CONTROL_OFFSET                  ((u32)sizeof(k_celebi_vblank_gate))
#define GATE_PATCH_SPAN                      (GATE_CONTROL_OFFSET + 1u)
#define SAFE_DISARM_FRAMES                   60u

static const u8 k_title[] = { 'P','M','_','C','R','Y','S','T','A','L' };
static const u8 k_nintendo_logo[48] = {
    0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,0x00,0x0C,0x00,0x0D,
    0x00,0x08,0x11,0x1F,0x88,0x89,0x00,0x0E,0xDC,0xCC,0x6E,0xE6,0xDD,0xDD,0xD9,0x99,
    0xBB,0xBB,0x67,0x63,0x6E,0x0E,0xEC,0xCC,0xDD,0xDC,0x99,0x9F,0xBB,0xB9,0x33,0x3E
};

/* 0f:69a8 LoadEnemyMon.UpdateDVs on Crystal Rev 1. */
static const u8 k_update_dvs_signature[] = {
    0x21,0x0C,0xD2, /* ld hl, wEnemyMonDVs */
    0x78,           /* ld a, b */
    0x22,           /* ld [hli], a */
    0x71            /* ld [hl], c */
};

/* Original Crystal Rev 1 code at LoadEnemyMon.GenerateDVs (0f:69a0). */
static const u8 k_generate_dvs_original[8] = {
    0xCD,0x9F,0x2F,0x47,0xCD,0x9F,0x2F,0x4F
};

/*
 * Timing-critical LR35902 gate. Keep this byte sequence unchanged unless the
 * Crystal VC timing is intentionally recalibrated. The verified DIV samples
 * are base+3, base+3, base+3, base+5.
 */
static const u8 k_celebi_vblank_gate[] = {
    0xFA,0x30,0xD2,
    0xFE,0x0B,
    0x20,0x40,
    0xFA,0x00,0x00,
    0xA7,
    0x28,0x3A,
    0xCD,0x5A,0x04,
    0xF0,0x04,
    0x57,
    0xF0,0x04,
    0xBA,
    0x28,0xFB,
    0x3C,
    0x3C,
    0x3C,
    0x5F,
    0xF0,0xE1,
    0x67,
    0xF0,0xE2,
    0x6F,
    0x7C,0xA7,0x83,0x67,
    0x7D,0x9B,0x6F,0x47,
    0x7C,0xA7,0x83,0x67,
    0x1C,
    0x1C,
    0x7D,0x9B,0x4F,
    0xFE,0xAA,
    0x20,0xD0,
    0x78,
    0xE6,0x2F,
    0xFE,0x2A,
    0x20,0xC9,
    0x78,
    0xEA,0xA9,0xD2,
    0x3E,0x11,
    0x3D,
    0x20,0xFD,
    0xC3,0x9F,0x2F
};

typedef struct CrystalProbe
{
    bool started;
    bool scan_done;
    u32 scan_cursor;
    u32 rom_hit_count;
    u32 rom_base;
    bool rom_writable;
    bool patch_signature_checked;
    bool patch_signature_match;
    u16 bank0_cave_guest_address;
    u32 bank0_trailing_zero_bytes;
} CrystalProbe;

typedef struct GuestPatchRuntime
{
    AslGen2RuntimeState state;
    bool prepared;
    bool installed;
    u32 rom_base;
    u32 generate_host_address;
    u32 cave_host_address;
    u16 cave_guest_address;
    u32 error_code;
    u8 saved_generate[3];
    u8 saved_cave[GATE_PATCH_SPAN];
    u32 disarm_frames;
} GuestPatchRuntime;

static CrystalProbe g_probe;
static GuestPatchRuntime g_patch;

static bool valid_crystal_header(u32 base)
{
    if (!asl_address_is_readable(base, 0x150u)) return false;
    if (*((volatile const u8 *)(base + 0x100u)) != 0x00u) return false;
    if (*((volatile const u8 *)(base + 0x101u)) != 0xC3u) return false;
    if (!asl_bytes_equal(base + 0x104u, k_nintendo_logo, sizeof(k_nintendo_logo))) return false;
    if (!asl_bytes_equal(base + 0x134u, k_title, sizeof(k_title))) return false;
    if (*((volatile const u8 *)(base + 0x143u)) != 0xC0u) return false;
    if (*((volatile const u8 *)(base + 0x147u)) != 0x10u) return false;
    return true;
}

static void inspect_primary_rom(void)
{
    u32 zero_count = 0u;
    u32 pos;
    const u32 base = g_probe.rom_base;

    if (g_probe.rom_hit_count == 0u || g_probe.patch_signature_checked) return;
    g_probe.patch_signature_checked = true;
    g_probe.patch_signature_match = asl_bytes_equal(
        base + CRYSTAL_UPDATE_DVS_FILE_OFFSET,
        k_update_dvs_signature,
        sizeof(k_update_dvs_signature));

    if (!asl_address_is_readable(base, CRYSTAL_BANK0_SIZE)) return;
    pos = CRYSTAL_BANK0_SIZE;
    while (pos > 0u)
    {
        if (*((volatile const u8 *)(base + pos - 1u)) != 0u) break;
        ++zero_count;
        --pos;
    }
    g_probe.bank0_trailing_zero_bytes = zero_count;
    if (zero_count > 0u && zero_count <= CRYSTAL_BANK0_SIZE)
        g_probe.bank0_cave_guest_address = (u16)(CRYSTAL_BANK0_SIZE - zero_count);
}

static void record_rom_hit(u32 base)
{
    if (g_probe.rom_hit_count != 0u)
    {
        ++g_probe.rom_hit_count;
        return;
    }

    g_probe.rom_hit_count = 1u;
    g_probe.rom_base = base;
    g_probe.rom_writable = asl_address_is_writable(base, 1u);
    inspect_primary_rom();
    if (g_probe.patch_signature_match && g_probe.rom_writable)
        g_probe.scan_done = true;
}

static void scan_for_title_in_range(u32 begin, u32 end)
{
    u32 p;
    if (end <= begin || end - begin < sizeof(k_title)) return;
    for (p = begin; p + sizeof(k_title) <= end; ++p)
    {
        if (*((volatile const u8 *)p) != 'P') continue;
        if (memcmp((const void *)p, k_title, sizeof(k_title)) == 0 && p >= 0x134u)
        {
            const u32 base = p - 0x134u;
            if (valid_crystal_header(base)) record_rom_hit(base);
        }
    }
}

static void probe_initialize(void)
{
    memset(&g_probe, 0, sizeof(g_probe));
    g_probe.started = true;
    g_probe.scan_cursor = PROBE_SCAN_BEGIN;
}

static void probe_step(void)
{
    u32 budget = PROBE_FRAME_BUDGET;
    if (!g_probe.started) probe_initialize();

    while (!g_probe.scan_done && budget > 0u)
    {
        MemInfo info;
        u32 end;
        u32 start;
        u32 avail;
        u32 chunk;
        u32 scan_end;

        if (g_probe.scan_cursor >= PROBE_SCAN_END)
        {
            g_probe.scan_done = true;
            break;
        }
        if (!asl_query_mapping(g_probe.scan_cursor, &info) || info.size == 0u)
        {
            g_probe.scan_cursor = asl_add_sat(g_probe.scan_cursor, 0x1000u);
            continue;
        }
        end = asl_mapping_end(&info);
        if (end <= g_probe.scan_cursor)
        {
            g_probe.scan_cursor = asl_add_sat(g_probe.scan_cursor, 0x1000u);
            continue;
        }
        if ((info.perm & MEMPERM_READ) == 0u)
        {
            g_probe.scan_cursor = end;
            continue;
        }

        start = g_probe.scan_cursor < info.base_addr ? info.base_addr : g_probe.scan_cursor;
        if (start >= end)
        {
            g_probe.scan_cursor = end;
            continue;
        }

        avail = end - start;
        chunk = avail < budget ? avail : budget;
        scan_end = start + chunk;
        if (scan_end < end)
        {
            const u32 extra = (u32)sizeof(k_title) - 1u;
            const u32 room = end - scan_end;
            scan_end += room < extra ? room : extra;
        }
        scan_for_title_in_range(start, scan_end);
        g_probe.scan_cursor = start + chunk;
        budget -= chunk;
    }
}

static void patch_initialize(void)
{
    memset(&g_patch, 0, sizeof(g_patch));
    g_patch.state = ASL_GEN2_RUNTIME_WAITING;
}

static void make_generate_patch(u8 out[3], u16 cave_guest_address)
{
    out[0] = 0xCDu;
    out[1] = (u8)(cave_guest_address & 0xFFu);
    out[2] = (u8)(cave_guest_address >> 8);
}

static bool prepare_patch(void)
{
    u32 gen;
    u32 cave;
    const u16 cave_guest = g_probe.bank0_cave_guest_address;

    if (!g_probe.scan_done || g_probe.rom_hit_count == 0u) return false;
    if (!g_probe.rom_base || !g_probe.rom_writable || !g_probe.patch_signature_match) return false;

    if (cave_guest == 0u || cave_guest >= CRYSTAL_BANK0_SIZE ||
        (u32)cave_guest + GATE_PATCH_SPAN > CRYSTAL_BANK0_SIZE ||
        g_probe.bank0_trailing_zero_bytes < GATE_PATCH_SPAN)
    {
        g_patch.error_code = 2u;
        g_patch.state = ASL_GEN2_RUNTIME_ERROR;
        return false;
    }

    gen = g_probe.rom_base + CRYSTAL_GENERATE_DVS_FILE_OFFSET;
    cave = g_probe.rom_base + (u32)cave_guest;

    if (!asl_bytes_equal(gen, k_generate_dvs_original, sizeof(k_generate_dvs_original)))
    {
        g_patch.error_code = 1u;
        g_patch.state = ASL_GEN2_RUNTIME_ERROR;
        return false;
    }
    if (!asl_bytes_zero(cave, GATE_PATCH_SPAN))
    {
        g_patch.error_code = 2u;
        g_patch.state = ASL_GEN2_RUNTIME_ERROR;
        return false;
    }
    if (!asl_address_is_writable(gen, sizeof(g_patch.saved_generate)) ||
        !asl_address_is_writable(cave, GATE_PATCH_SPAN))
    {
        g_patch.error_code = 3u;
        g_patch.state = ASL_GEN2_RUNTIME_ERROR;
        return false;
    }

    g_patch.rom_base = g_probe.rom_base;
    g_patch.generate_host_address = gen;
    g_patch.cave_host_address = cave;
    g_patch.cave_guest_address = cave_guest;
    memcpy(g_patch.saved_generate, (const void *)gen, sizeof(g_patch.saved_generate));
    memcpy(g_patch.saved_cave, (const void *)cave, sizeof(g_patch.saved_cave));
    g_patch.prepared = true;
    g_patch.state = ASL_GEN2_RUNTIME_READY;
    return true;
}

static bool install_patch(void)
{
    u8 generate_patch[3];
    u8 gate[sizeof(k_celebi_vblank_gate)];
    const u16 control_guest = (u16)(g_patch.cave_guest_address + GATE_CONTROL_OFFSET);
    const u32 gen = g_patch.generate_host_address;
    const u32 cave = g_patch.cave_host_address;

    if (!g_patch.prepared || !gen || !cave) return false;

    make_generate_patch(generate_patch, g_patch.cave_guest_address);
    memcpy(gate, k_celebi_vblank_gate, sizeof(gate));
    gate[GATE_CONTROL_OPERAND_OFFSET] = (u8)(control_guest & 0xFFu);
    gate[GATE_CONTROL_OPERAND_OFFSET + 1u] = (u8)(control_guest >> 8);

    memcpy((void *)cave, gate, sizeof(gate));
    *((volatile u8 *)(cave + GATE_CONTROL_OFFSET)) = 1u;
    asl_memory_barrier();
    memcpy((void *)gen, generate_patch, sizeof(generate_patch));
    asl_memory_barrier();

    if (!asl_bytes_equal(cave, gate, sizeof(gate)) ||
        *((volatile const u8 *)(cave + GATE_CONTROL_OFFSET)) != 1u ||
        !asl_bytes_equal(gen, generate_patch, sizeof(generate_patch)))
    {
        g_patch.error_code = 4u;
        g_patch.state = ASL_GEN2_RUNTIME_ERROR;
        return false;
    }

    g_patch.installed = true;
    g_patch.disarm_frames = 0u;
    g_patch.state = ASL_GEN2_RUNTIME_INSTALLED;
    asl_debug_log("[ASL2] real-VBlank Celebi guest gate installed\n");
    return true;
}

static bool restore_patch(void)
{
    const u32 gen = g_patch.generate_host_address;
    const u32 cave = g_patch.cave_host_address;

    if (!g_patch.prepared || !gen || !cave) return true;

    memcpy((void *)gen, g_patch.saved_generate, sizeof(g_patch.saved_generate));
    asl_memory_barrier();
    memcpy((void *)cave, g_patch.saved_cave, sizeof(g_patch.saved_cave));
    asl_memory_barrier();

    if (!asl_bytes_equal(gen, g_patch.saved_generate, sizeof(g_patch.saved_generate)) ||
        !asl_bytes_equal(cave, g_patch.saved_cave, sizeof(g_patch.saved_cave)))
    {
        g_patch.error_code = 5u;
        g_patch.state = ASL_GEN2_RUNTIME_ERROR;
        return false;
    }

    g_patch.installed = false;
    g_patch.state = ASL_GEN2_RUNTIME_READY;
    asl_debug_log("[ASL2] real-VBlank Celebi guest gate restored\n");
    return true;
}

static void patch_step(bool armed)
{
    if (g_patch.state == ASL_GEN2_RUNTIME_ERROR) return;

    if (g_patch.prepared && g_probe.rom_base && g_probe.rom_base != g_patch.rom_base)
    {
        if (g_patch.installed) (void)restore_patch();
        patch_initialize();
    }

    if (!g_patch.prepared)
    {
        if (!g_probe.scan_done)
        {
            g_patch.state = ASL_GEN2_RUNTIME_WAITING;
            return;
        }
        if (!prepare_patch())
        {
            if (g_patch.state != ASL_GEN2_RUNTIME_ERROR)
            {
                g_patch.error_code = 6u;
                g_patch.state = ASL_GEN2_RUNTIME_ERROR;
            }
            return;
        }
    }

    if (armed)
    {
        g_patch.disarm_frames = 0u;
        if (!g_patch.installed)
            (void)install_patch();
        else
        {
            *((volatile u8 *)(g_patch.cave_host_address + GATE_CONTROL_OFFSET)) = 1u;
            asl_memory_barrier();
        }
    }
    else if (g_patch.installed)
    {
        *((volatile u8 *)(g_patch.cave_host_address + GATE_CONTROL_OFFSET)) = 0u;
        asl_memory_barrier();
        if (g_patch.disarm_frames < SAFE_DISARM_FRAMES)
            ++g_patch.disarm_frames;
        else
            (void)restore_patch();
    }
}

void asl_gen2_runtime_initialize(void)
{
    probe_initialize();
    patch_initialize();
}

void asl_gen2_runtime_step(bool armed)
{
    probe_step();
    patch_step(armed);
}

bool asl_gen2_runtime_reset(void)
{
    if (g_patch.installed && !restore_patch()) return false;
    patch_initialize();
    return true;
}

void asl_gen2_runtime_get_status(AslGen2RuntimeStatus *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->state = g_patch.state;
    out->rom_found = g_probe.rom_hit_count != 0u;
    out->detected = out->rom_found && g_probe.rom_writable && g_probe.patch_signature_match;
    out->rom_base = g_probe.rom_base;
    out->error_code = g_patch.error_code;
}

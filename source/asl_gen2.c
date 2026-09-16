#include "asl_gen2.h"
#include "asl_gen2_runtime.h"
#include "asl_platform.h"
#include <string.h>

#define SCAN_BEGIN             0x00100000u
#define SCAN_END               0x40000000u
#define SCAN_FRAME_BUDGET      (256u * 1024u)
#define CRYSTAL_ROM_SIZE       0x00200000u

#define CELEBI_SPECIES         0xFBu
#define WILD_BATTLE            0x01u
#define BATTLETYPE_CELEBI      0x0Bu
#define W_TEMP_ENEMY_SPECIES   0xD204u
#define W_ENEMY_MON_DVS        0xD20Cu
#define W_BATTLE_MODE          0xD22Du
#define W_BATTLE_TYPE          0xD230u
#define W_PRED_TELEMETRY       0xD2A9u

#define OFF_ENEMY_SPECIES      (0xD206u - W_TEMP_ENEMY_SPECIES)
#define OFF_ENEMY_DVS          (W_ENEMY_MON_DVS - W_TEMP_ENEMY_SPECIES)
#define OFF_ENEMY_LEVEL        (0xD213u - W_TEMP_ENEMY_SPECIES)
#define OFF_BATTLE_MODE        (W_BATTLE_MODE - W_TEMP_ENEMY_SPECIES)
#define OFF_BATTLE_TYPE        (W_BATTLE_TYPE - W_TEMP_ENEMY_SPECIES)
#define OFF_PRED_TELEMETRY     (W_PRED_TELEMETRY - W_TEMP_ENEMY_SPECIES)
#define CELEBI_LEVEL           30u
#define TELEMETRY_EMPTY        0x00u

typedef enum Gen2Stage
{
    GEN2_WAITING = 0,
    GEN2_READY,
    GEN2_SEARCHING,
    GEN2_RELEASED,
    GEN2_VERIFIED,
    GEN2_ABORTED,
    GEN2_ERROR
} Gen2Stage;

typedef struct Gen2Runtime
{
    bool enabled;
    Gen2Stage stage;
    u32 scan_cursor;
    u32 context_host;
    bool context_found;
    bool telemetry_cleared;
    bool ignore_context_until_gone;

    bool prediction_valid;
    u8 predicted_atk_def;
    u8 predicted_spd_spc;

    bool verification_done;
    bool verification_match;
    u8 actual_atk_def;
    u8 actual_spd_spc;

    u32 extra_vblanks;
    bool abort_latched;
} Gen2Runtime;

static Gen2Runtime g_gen2;

static bool gen2_is_shiny(u8 atk_def, u8 spd_spc)
{
    const u8 atk = (u8)(atk_def >> 4);
    if (spd_spc != 0xAAu || (atk_def & 0x0Fu) != 0x0Au) return false;
    return atk == 2u || atk == 3u || atk == 6u || atk == 7u ||
           atk == 10u || atk == 11u || atk == 14u || atk == 15u;
}

static void clear_result_fields(void)
{
    g_gen2.context_found = false;
    g_gen2.telemetry_cleared = false;
    g_gen2.prediction_valid = false;
    g_gen2.predicted_atk_def = 0u;
    g_gen2.predicted_spd_spc = 0u;
    g_gen2.verification_done = false;
    g_gen2.verification_match = false;
    g_gen2.actual_atk_def = 0u;
    g_gen2.actual_spd_spc = 0u;
    g_gen2.extra_vblanks = 0u;
    g_gen2.abort_latched = false;
}

static void reset_result_state(void)
{
    clear_result_fields();
    g_gen2.stage = GEN2_WAITING;
    g_gen2.scan_cursor = SCAN_BEGIN;
    g_gen2.context_host = 0u;
    g_gen2.ignore_context_until_gone = false;
}

static void clear_result_for_next_run(void)
{
    const bool context_present = g_gen2.context_host != 0u;
    clear_result_fields();
    g_gen2.stage = GEN2_READY;
    g_gen2.ignore_context_until_gone = context_present;
}

static bool address_in_rom(u32 address, u32 rom_base)
{
    const u32 end = rom_base + CRYSTAL_ROM_SIZE;
    if (rom_base == 0u || end < rom_base) return false;
    return address >= rom_base && address < end;
}

static bool looks_like_celebi_context(u32 address)
{
    const volatile u8 *p;
    if (!asl_address_is_readable(address, OFF_PRED_TELEMETRY + 1u)) return false;
    p = (const volatile u8 *)address;
    return p[0] == CELEBI_SPECIES &&
           p[OFF_BATTLE_MODE] == WILD_BATTLE &&
           p[OFF_BATTLE_TYPE] == BATTLETYPE_CELEBI;
}

static bool context_still_valid(void)
{
    return g_gen2.context_host != 0u && looks_like_celebi_context(g_gen2.context_host);
}

static void clear_prediction_telemetry(void)
{
    volatile u8 *p;
    if (!g_gen2.context_host || g_gen2.telemetry_cleared) return;
    if (!asl_address_is_readable(g_gen2.context_host, OFF_PRED_TELEMETRY + 1u)) return;
    p = (volatile u8 *)g_gen2.context_host;
    p[OFF_PRED_TELEMETRY] = TELEMETRY_EMPTY;
    asl_memory_barrier();
    g_gen2.telemetry_cleared = true;
}

static bool scan_for_celebi_context(u32 rom_base)
{
    u32 budget = SCAN_FRAME_BUDGET;

    if (g_gen2.ignore_context_until_gone)
    {
        if (context_still_valid()) return false;
        g_gen2.ignore_context_until_gone = false;
        g_gen2.context_host = 0u;
        g_gen2.telemetry_cleared = false;
        g_gen2.scan_cursor = SCAN_BEGIN;
    }

    if (context_still_valid())
    {
        g_gen2.context_found = true;
        clear_prediction_telemetry();
        return true;
    }

    g_gen2.context_host = 0u;
    g_gen2.context_found = false;
    g_gen2.telemetry_cleared = false;

    while (budget > 0u)
    {
        MemInfo info;
        u32 end;
        u32 start;
        u32 avail;
        u32 chunk;
        u32 limit;
        u32 p;

        if (g_gen2.scan_cursor >= SCAN_END) g_gen2.scan_cursor = SCAN_BEGIN;
        if (!asl_query_mapping(g_gen2.scan_cursor, &info) || info.size == 0u)
        {
            g_gen2.scan_cursor = asl_add_sat(g_gen2.scan_cursor, 0x1000u);
            continue;
        }

        end = asl_mapping_end(&info);
        if (end <= g_gen2.scan_cursor)
        {
            g_gen2.scan_cursor = asl_add_sat(g_gen2.scan_cursor, 0x1000u);
            continue;
        }
        if ((info.perm & (MEMPERM_READ | MEMPERM_WRITE)) != (MEMPERM_READ | MEMPERM_WRITE))
        {
            g_gen2.scan_cursor = end;
            continue;
        }

        start = g_gen2.scan_cursor < info.base_addr ? info.base_addr : g_gen2.scan_cursor;
        if (start >= end)
        {
            g_gen2.scan_cursor = end;
            continue;
        }

        avail = end - start;
        chunk = avail < budget ? avail : budget;
        limit = start + chunk;
        p = start;

        while (p < limit)
        {
            const u8 *hit;
            const volatile u8 *candidate;
            const u32 remaining = limit - p;
            hit = (const u8 *)memchr((const void *)p, CELEBI_SPECIES, remaining);
            if (!hit) break;
            p = (u32)hit;
            if (address_in_rom(p, rom_base))
            {
                ++p;
                continue;
            }
            if (end - p <= OFF_PRED_TELEMETRY) break;

            candidate = (const volatile u8 *)p;
            if (candidate[OFF_BATTLE_MODE] == WILD_BATTLE &&
                candidate[OFF_BATTLE_TYPE] == BATTLETYPE_CELEBI)
            {
                g_gen2.context_host = p;
                g_gen2.context_found = true;
                g_gen2.scan_cursor = p;
                clear_prediction_telemetry();
                return true;
            }
            ++p;
        }

        g_gen2.scan_cursor = limit;
        budget -= chunk;
    }
    return false;
}

static bool read_released_prediction(void)
{
    const volatile u8 *p;
    u8 predicted;
    if (!g_gen2.context_host || g_gen2.prediction_valid) return g_gen2.prediction_valid;
    if (!asl_address_is_readable(g_gen2.context_host, OFF_PRED_TELEMETRY + 1u)) return false;

    p = (const volatile u8 *)g_gen2.context_host;
    predicted = p[OFF_PRED_TELEMETRY];
    if (predicted == TELEMETRY_EMPTY || !gen2_is_shiny(predicted, 0xAAu)) return false;

    g_gen2.prediction_valid = true;
    g_gen2.predicted_atk_def = predicted;
    g_gen2.predicted_spd_spc = 0xAAu;
    g_gen2.stage = GEN2_RELEASED;
    asl_debug_log("[ASL2] shiny Celebi candidate released\n");
    return true;
}

static void update_search_and_verification(u32 rom_base)
{
    const volatile u8 *p;
    u8 atk_def;
    u8 spd_spc;

    if (g_gen2.verification_done) return;
    if (!scan_for_celebi_context(rom_base))
    {
        if (!g_gen2.ignore_context_until_gone) g_gen2.stage = GEN2_READY;
        return;
    }

    p = (const volatile u8 *)g_gen2.context_host;
    if (!read_released_prediction())
    {
        g_gen2.stage = GEN2_SEARCHING;
        if (g_gen2.extra_vblanks != 0xFFFFFFFFu) ++g_gen2.extra_vblanks;
        return;
    }

    if (p[OFF_ENEMY_SPECIES] != CELEBI_SPECIES || p[OFF_ENEMY_LEVEL] != CELEBI_LEVEL)
    {
        g_gen2.stage = GEN2_RELEASED;
        return;
    }

    atk_def = p[OFF_ENEMY_DVS];
    spd_spc = p[OFF_ENEMY_DVS + 1u];
    g_gen2.actual_atk_def = atk_def;
    g_gen2.actual_spd_spc = spd_spc;
    g_gen2.verification_done = true;
    g_gen2.verification_match =
        g_gen2.prediction_valid &&
        atk_def == g_gen2.predicted_atk_def &&
        spd_spc == g_gen2.predicted_spd_spc &&
        gen2_is_shiny(atk_def, spd_spc);
    g_gen2.stage = GEN2_VERIFIED;
    asl_debug_log(g_gen2.verification_match
        ? "[ASL2] Celebi PRED == REAL verified\n"
        : "[ASL2] Celebi PRED/REAL mismatch\n");
}

void asl_gen2_initialize(void)
{
    memset(&g_gen2, 0, sizeof(g_gen2));
    g_gen2.enabled = true;
    reset_result_state();
    asl_gen2_runtime_initialize();
}

void asl_gen2_set_enabled(bool enabled)
{
    if (g_gen2.enabled == enabled) return;
    g_gen2.enabled = enabled;
    reset_result_state();
}

void asl_gen2_reset_run(void)
{
    reset_result_state();
}

void asl_gen2_handle_x(void)
{
    AslGen2RuntimeStatus runtime;
    asl_gen2_runtime_get_status(&runtime);

    if (g_gen2.stage == GEN2_SEARCHING)
    {
        g_gen2.abort_latched = true;
        g_gen2.stage = GEN2_ABORTED;
        asl_debug_log("[ASL2] Celebi search aborted; releasing gate\n");
    }
    else if (g_gen2.stage == GEN2_VERIFIED || g_gen2.stage == GEN2_RELEASED)
    {
        clear_result_for_next_run();
    }
    else if (g_gen2.stage == GEN2_ABORTED)
    {
        if (runtime.state != ASL_GEN2_RUNTIME_INSTALLED)
        {
            clear_result_for_next_run();
            if (!asl_gen2_runtime_reset()) g_gen2.stage = GEN2_ERROR;
        }
    }
    else
    {
        reset_result_state();
        if (!asl_gen2_runtime_reset()) g_gen2.stage = GEN2_ERROR;
    }
}

void asl_gen2_on_frame(void)
{
    AslGen2RuntimeStatus runtime;

    asl_gen2_runtime_step(g_gen2.enabled && !g_gen2.abort_latched);
    asl_gen2_runtime_get_status(&runtime);

    if (!g_gen2.enabled)
    {
        g_gen2.stage = GEN2_WAITING;
        return;
    }
    if (runtime.state == ASL_GEN2_RUNTIME_ERROR)
    {
        g_gen2.stage = GEN2_ERROR;
        return;
    }
    if (g_gen2.abort_latched)
    {
        g_gen2.stage = GEN2_ABORTED;
        return;
    }
    if (runtime.state != ASL_GEN2_RUNTIME_INSTALLED)
    {
        g_gen2.stage = GEN2_WAITING;
        return;
    }

    update_search_and_verification(runtime.rom_base);
}

bool asl_gen2_detected(void)
{
    AslGen2RuntimeStatus runtime;
    asl_gen2_runtime_get_status(&runtime);
    return runtime.detected;
}

void asl_gen2_get_snapshot(AslGateSnapshot *out)
{
    AslGen2RuntimeStatus runtime;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    asl_gen2_runtime_get_status(&runtime);

    out->enabled = g_gen2.enabled;
    out->generation = ASL_GENERATION_2;
    out->supported = runtime.detected && runtime.error_code == 0u;
    out->runtime_ready = runtime.state == ASL_GEN2_RUNTIME_INSTALLED ||
                         runtime.state == ASL_GEN2_RUNTIME_READY;
    out->rom_title_seen = runtime.rom_found;
    if (out->rom_title_seen) memcpy(out->rom_title, "PM_CRYSTAL", 11u);
    memcpy(out->game_name, "CRYSTAL", 8u);
    out->target_active = g_gen2.context_found ||
                         g_gen2.stage == GEN2_RELEASED ||
                         g_gen2.stage == GEN2_VERIFIED;
    out->target_species = CELEBI_SPECIES;
    memcpy(out->target_name, "CELEBI", 7u);
    out->extra_vblanks = g_gen2.extra_vblanks;
    out->prediction_valid = g_gen2.prediction_valid;
    out->predicted_dv1 = g_gen2.predicted_atk_def;
    out->predicted_dv2 = g_gen2.predicted_spd_spc;
    out->verification_done = g_gen2.verification_done;
    out->verification_match = g_gen2.verification_match;
    out->actual_dv1 = g_gen2.actual_atk_def;
    out->actual_dv2 = g_gen2.actual_spd_spc;
    out->error_code = runtime.error_code;

    switch (g_gen2.stage)
    {
        case GEN2_READY: out->stage = ASL_GATE_READY; break;
        case GEN2_SEARCHING: out->stage = ASL_GATE_HOLDING; break;
        case GEN2_RELEASED:
        case GEN2_VERIFIED: out->stage = ASL_GATE_RELEASED_SHINY; break;
        case GEN2_ABORTED: out->stage = ASL_GATE_ABORTED; break;
        case GEN2_ERROR: out->stage = ASL_GATE_ERROR; break;
        case GEN2_WAITING:
        default: out->stage = ASL_GATE_DETECTING; break;
    }
}

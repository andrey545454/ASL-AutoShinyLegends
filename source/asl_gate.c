#include "asl_gate.h"
#include "asl_gen1.h"
#include "asl_gen2.h"
#include "asl_platform.h"
#include <string.h>

#define SOFT_RESET_KEYS (KEY_A | KEY_B | KEY_START | KEY_SELECT)

typedef struct GateRouter
{
    bool enabled;
    bool select_pending;
    bool soft_reset_latched;
    bool gen1_host_detected;
    AslGeneration active_generation;
} GateRouter;

static GateRouter g_router;

static void set_enabled(bool enabled)
{
    g_router.enabled = enabled;
    asl_gen1_set_enabled(enabled);
    asl_gen2_set_enabled(enabled);
}

static void reset_active_run(void)
{
    if (g_router.active_generation == ASL_GENERATION_1)
        asl_gen1_reset_run();
    else if (g_router.active_generation == ASL_GENERATION_2)
        asl_gen2_reset_run();
    else
    {
        asl_gen1_reset_run();
        asl_gen2_reset_run();
    }
}

bool asl_gate_initialize(u8 *code_base, u32 code_size)
{
    memset(&g_router, 0, sizeof(g_router));
    g_router.enabled = true;

    /*
     * Gen I and Gen II use different VC emulator builds.  The Gen I backend
     * validates its fixed F0/E0 ARM sites before writing anything.  Crystal
     * fails those checks and falls through to the Gen II writable-ROM probe.
     */
    g_router.gen1_host_detected = asl_gen1_initialize(code_base, code_size);
    asl_gen2_initialize();

    if (g_router.gen1_host_detected)
        g_router.active_generation = ASL_GENERATION_1;
    else
        g_router.active_generation = ASL_GENERATION_UNKNOWN;

    set_enabled(true);
    asl_debug_log(g_router.gen1_host_detected
        ? "[ASL] backend auto-detect: Gen I host\n"
        : "[ASL] backend auto-detect: probing Gen II Crystal\n");
    return g_router.gen1_host_detected;
}

void asl_gate_on_frame(u32 keys_down, u32 keys_held)
{
    const u32 reset_modifiers = KEY_A | KEY_B | KEY_START;
    const bool soft_reset_held = (keys_held & SOFT_RESET_KEYS) == SOFT_RESET_KEYS;

    if (soft_reset_held)
    {
        g_router.select_pending = false;
        if (!g_router.soft_reset_latched)
        {
            g_router.soft_reset_latched = true;
            reset_active_run();
            asl_debug_log("[ASL] soft reset: run state cleared\n");
        }
    }
    else
    {
        if (g_router.soft_reset_latched)
        {
            if ((keys_held & SOFT_RESET_KEYS) != 0u)
                goto tick_backend;
            g_router.soft_reset_latched = false;
        }

        if ((keys_down & KEY_SELECT) != 0u)
            g_router.select_pending = true;

        if (g_router.select_pending)
        {
            if ((keys_held & KEY_SELECT) == 0u &&
                (keys_held & reset_modifiers) == 0u)
            {
                g_router.select_pending = false;
                set_enabled(!g_router.enabled);
                asl_debug_log(g_router.enabled
                    ? "[ASL] enabled by SELECT\n"
                    : "[ASL] disabled by SELECT\n");
            }
        }

        if (g_router.enabled && (keys_down & KEY_X) != 0u)
        {
            if (g_router.active_generation == ASL_GENERATION_1)
                asl_gen1_handle_x();
            else if (g_router.active_generation == ASL_GENERATION_2)
                asl_gen2_handle_x();
            else
                asl_gen2_reset_run();
        }
    }

tick_backend:
    if (g_router.active_generation == ASL_GENERATION_1)
    {
        asl_gen1_on_frame();
        return;
    }

    /* Keep the Crystal backend alive while disabled so it can safely disarm
       and restore a guest patch that was already executing. */
    asl_gen2_on_frame();
    if (g_router.active_generation == ASL_GENERATION_UNKNOWN && asl_gen2_detected())
    {
        g_router.active_generation = ASL_GENERATION_2;
        asl_gen2_set_enabled(g_router.enabled);
        asl_debug_log("[ASL] backend auto-detect: Crystal Gen II\n");
    }
}

void asl_gate_get_snapshot(AslGateSnapshot *out)
{
    if (!out) return;

    if (g_router.active_generation == ASL_GENERATION_1)
    {
        asl_gen1_get_snapshot(out);
        out->enabled = g_router.enabled;
        return;
    }
    if (g_router.active_generation == ASL_GENERATION_2)
    {
        asl_gen2_get_snapshot(out);
        out->enabled = g_router.enabled;
        return;
    }

    memset(out, 0, sizeof(*out));
    out->enabled = g_router.enabled;
    out->generation = ASL_GENERATION_UNKNOWN;
    out->stage = ASL_GATE_DETECTING;
    out->runtime_ready = true;
    memcpy(out->game_name, "AUTO", 5u);
}

#pragma once

#include <3ds.h>

typedef enum AslGateStage
{
    ASL_GATE_READY = 0,
    ASL_GATE_HOLDING,
    ASL_GATE_RELEASED_SHINY,
    ASL_GATE_ABORTED,
    ASL_GATE_ERROR
} AslGateStage;

typedef struct AslGateSnapshot
{
    bool enabled;
    bool supported;
    bool hooks_installed;

    bool rom_title_seen;
    const char *rom_title;
    const char *game_name;

    u8 target_species;
    const char *target_name;

    AslGateStage stage;
    u32 extra_vblanks;

    bool prediction_valid;
    u8 predicted_dv1;
    u8 predicted_dv2;

    bool verification_done;
    bool verification_match;
    u8 actual_dv1;
    u8 actual_dv2;
} AslGateSnapshot;

bool asl_gate_initialize(u8 *code_base, u32 code_size);
void asl_gate_on_frame(u32 keys_down, u32 keys_held);
void asl_gate_get_snapshot(AslGateSnapshot *out);

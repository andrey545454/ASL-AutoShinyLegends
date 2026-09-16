#pragma once
#include <3ds.h>

typedef enum AslGen2RuntimeState
{
    ASL_GEN2_RUNTIME_WAITING = 0,
    ASL_GEN2_RUNTIME_READY,
    ASL_GEN2_RUNTIME_INSTALLED,
    ASL_GEN2_RUNTIME_ERROR
} AslGen2RuntimeState;

typedef struct AslGen2RuntimeStatus
{
    AslGen2RuntimeState state;
    bool rom_found;
    bool detected;
    u32 rom_base;
    u32 error_code;
} AslGen2RuntimeStatus;

void asl_gen2_runtime_initialize(void);
void asl_gen2_runtime_step(bool armed);
bool asl_gen2_runtime_reset(void);
void asl_gen2_runtime_get_status(AslGen2RuntimeStatus *out);

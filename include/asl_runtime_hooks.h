#pragma once

#include <3ds.h>

typedef struct AslRuntimeHookStatus
{
    bool render_installed;
    bool input_installed;
} AslRuntimeHookStatus;

void asl_runtime_hooks_install(
    u8 *code_base,
    u32 code_size,
    AslRuntimeHookStatus *status);

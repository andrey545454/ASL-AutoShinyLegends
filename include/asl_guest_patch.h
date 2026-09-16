#pragma once
#include <3ds.h>
#include "asl_rom_probe.h"

typedef enum AslGuestPatchState
{
    ASLC_GPATCH_WAITING = 0,
    ASLC_GPATCH_READY,
    ASLC_GPATCH_INSTALLED,
    ASLC_GPATCH_ERROR
} AslGuestPatchState;

typedef struct AslGuestPatchSnapshot
{
    AslGuestPatchState state;
    bool generate_signature_ok;
    bool cave_ok;
    bool install_verified;
    bool restore_verified;
    u32 rom_base;
    u32 generate_host_address;
    u32 cave_host_address;
    u16 cave_guest_address;
    u32 routine_size;
    u32 error_code;
} AslGuestPatchSnapshot;

void asl_guest_patch_initialize(void);
void asl_guest_patch_step(const AslRomProbeSnapshot *probe, bool armed);
bool asl_guest_patch_reset(void);
void asl_guest_patch_get_snapshot(AslGuestPatchSnapshot *out);

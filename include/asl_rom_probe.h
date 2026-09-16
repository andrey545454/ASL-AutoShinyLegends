#pragma once
#include <3ds.h>

typedef struct AslRomProbeSnapshot
{
    bool started;
    bool scan_done;
    u32 scan_cursor;
    u32 readable_bytes_scanned;

    u32 rom_hit_count;
    u32 rom_base[3];
    bool rom_writable[3];

    bool patch_signature_checked;
    bool patch_signature_match;
    u32 patch_host_address;

    u16 bank0_cave_guest_address;
    u32 bank0_trailing_zero_bytes;

    bool ptr_scan_started;
    bool ptr_scan_done;
    u32 ptr_scan_cursor;
    u32 ptr_ref_count;
    u32 ptr_ref_address[3];
    u8 ptr_ref_kind[3]; /* 1=ROM base, 2=title, 3=bank1 */
} AslRomProbeSnapshot;

void asl_rom_probe_initialize(void);
void asl_rom_probe_step(void);
void asl_rom_probe_get_snapshot(AslRomProbeSnapshot *out);

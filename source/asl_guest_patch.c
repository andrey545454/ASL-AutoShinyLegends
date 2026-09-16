#include "asl_guest_patch.h"
#include "asl_platform.h"
#include <string.h>

/* Crystal Rev 1 / VC file offset for LoadEnemyMon.GenerateDVs (0f:69a0). */
#define CRYSTAL_GENERATE_DVS_FILE_OFFSET 0x0003E9A0u
#define CRYSTAL_BANK0_SIZE               0x00004000u
#define GATE_CONTROL_OPERAND_OFFSET        8u
#define GATE_CONTROL_OFFSET               ((u32)sizeof(k_celebi_vblank_gate))
#define GATE_PATCH_SPAN                   (GATE_CONTROL_OFFSET+1u)
#define SAFE_DISARM_FRAMES                60u

/*
 * Original Crystal Rev 1 code at LoadEnemyMon.GenerateDVs:
 *   call BattleRandom  ; 00:2f9f
 *   ld b, a
 *   call BattleRandom
 *   ld c, a
 *
 * ASL 2.0.0 patches ONLY the first three bytes (the first CALL).  The second
 * BattleRandom remains completely original.  On release the guest gate
 * tail-jumps to the stock BattleRandom, whose RET returns to 0f:69a3.  Thus
 * both final DV rolls are still performed by Crystal itself.
 */
static const u8 k_generate_dvs_original[8] = {
    0xCD,0x9F,0x2F,0x47,0xCD,0x9F,0x2F,0x4F
};

/*
 * LR35902 guest gate injected into the verified trailing-zero cave in ROM0.
 *
 * This is the ASL-style path: for Celebi it advances the game by one genuine
 * DelayFrame/VBlank at a time, predicts the two stock BattleRandom results,
 * and releases only when that future pair is shiny.  It never writes RNG
 * state or enemy DVs and never calls BattleRandom for rejected candidates.
 *
 * The one guest-RAM write is telemetry only: on release the gate stores the
 * predicted Attack/Defense byte in Crystal Rev 1's documented
 * wOTPartyMon1Unused ($D2A9).  The host uses that unused scratch byte only to
 * show PRED and to require an exact PRED==REAL verification.  Speed/Special is
 * necessarily $AA for every accepted candidate.
 *
 * Timing model after the detected DIV edge:
 *   the calibrated Crystal VC path predicts the four rDIV samples as
 *       base+3, base+3, base+3, base+5
 *   Calibration on the tested Crystal VC produced repeatable samples where
 *   DV1 matched exactly while DV2 was one lower (3A AA -> 3A A9 and
 *   6A AA -> 6A A9).  The final rDIV sample is therefore base+5 rather than
 *   base+4.  The extra INC E is cycle-compensated by storing raw telemetry
 *   (no CPL), preserving the measured release timing.
 */
static const u8 k_celebi_vblank_gate[] = {
    0xFA,0x30,0xD2,              /* ld a,[wBattleType] */
    0xFE,0x0B,                   /* cp BATTLETYPE_CELEBI */
    0x20,0x40,                   /* jr nz,.release */

                                    /* .loop */
    0xFA,0x00,0x00,              /* ld a,[control] - address fixed at install */
    0xA7,                         /* and a */
    0x28,0x3A,                   /* jr z,.release */
    0xCD,0x5A,0x04,              /* call DelayFrame ($045a) */
    0xF0,0x04,                   /* ldh a,[rDIV] */
    0x57,                         /* ld d,a */
                                    /* .wait_div */
    0xF0,0x04,                   /* ldh a,[rDIV] */
    0xBA,                         /* cp d */
    0x28,0xFB,                   /* jr z,.wait_div */

    0x3C,                         /* inc a */
    0x3C,                         /* inc a */
    0x3C,                         /* inc a  ; Crystal VC calibrated base+3 */
    0x5F,                         /* ld e,a */
    0xF0,0xE1,                   /* ldh a,[hRandomAdd] */
    0x67,                         /* ld h,a */
    0xF0,0xE2,                   /* ldh a,[hRandomSub] */
    0x6F,                         /* ld l,a */

                                    /* predict DV1: div add/sub = base+3 */
    0x7C,0xA7,0x83,0x67,
    0x7D,0x9B,0x6F,0x47,         /* ld b,a ; predicted Atk/Def */

                                    /* predict DV2: add=base+3, sub=base+5 */
    0x7C,0xA7,0x83,0x67,
    0x1C,                         /* inc e -> base+4; carry preserved */
    0x1C,                         /* inc e -> base+5; carry preserved */
    0x7D,0x9B,0x4F,              /* ld c,a ; predicted Spd/Spc */

    0xFE,0xAA,                   /* cp $aa */
    0x20,0xD0,                   /* jr nz,.loop */
    0x78,                         /* ld a,b */
    0xE6,0x2F,                   /* keep Defense nibble + Attack bit 1 */
    0xFE,0x2A,                   /* shiny Attack/Defense pattern? */
    0x20,0xC9,                   /* jr nz,.loop */

                                    /* Release telemetry.  wOTPartyMon1Unused is unused in the
                                       wild non-link Celebi path.  Store raw B; removing the old
                                       one-cycle CPL compensates the extra INC E above. */
    0x78,                         /* ld a,b */
    0xEA,0xA9,0xD2,              /* ld [$D2A9],a */

                                    /* 69-M-cycle accepted-path pad */
    0x3E,0x11,                   /* ld a,$11 */
                                    /* .pad */
    0x3D,                         /* dec a */
    0x20,0xFD,                   /* jr nz,.pad */

                                    /* .release */
    0xC3,0x9F,0x2F               /* jp BattleRandom ($2f9f) */
};

typedef struct GuestPatchRuntime
{
    AslGuestPatchSnapshot snap;
    bool prepared;
    bool installed;
    u8 saved_generate[3];
    u8 saved_cave[GATE_PATCH_SPAN];
    u32 disarm_frames;
} GuestPatchRuntime;

static GuestPatchRuntime g_patch;

static bool writable_address(u32 address)
{
    MemInfo info;
    PageInfo page;
    if (R_FAILED(svcQueryMemory(&info,&page,address))) return false;
    return (info.perm & MEMPERM_WRITE)!=0u;
}

static bool bytes_equal(u32 address,const u8 *bytes,u32 size)
{
    if (!bytes || size==0u || !asl_address_is_readable(address,size)) return false;
    return memcmp((const void *)address,bytes,size)==0;
}

static bool bytes_zero(u32 address,u32 size)
{
    u32 i;
    if (size==0u || !asl_address_is_readable(address,size)) return false;
    for (i=0;i<size;++i)
        if (*((volatile const u8 *)(address+i))!=0u) return false;
    return true;
}

static void memory_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static void make_generate_patch(u8 out[3],u16 cave_guest_address)
{
    out[0]=0xCDu; /* CALL a16 */
    out[1]=(u8)(cave_guest_address&0xFFu);
    out[2]=(u8)(cave_guest_address>>8);
}

static bool prepare_from_probe(const AslRomProbeSnapshot *probe)
{
    u32 base,gen,cave;
    u16 cave_guest;

    if (!probe || !probe->scan_done || probe->rom_hit_count==0u) return false;
    if (!probe->rom_base[0] || !probe->rom_writable[0] || !probe->patch_signature_match) return false;

    cave_guest=probe->bank0_cave_guest_address;
    if (cave_guest==0u || cave_guest>=CRYSTAL_BANK0_SIZE ||
        (u32)cave_guest+GATE_PATCH_SPAN>CRYSTAL_BANK0_SIZE ||
        probe->bank0_trailing_zero_bytes<GATE_PATCH_SPAN)
    {
        g_patch.snap.error_code=2u;
        g_patch.snap.state=ASLC_GPATCH_ERROR;
        return false;
    }

    base=probe->rom_base[0];
    gen=base+CRYSTAL_GENERATE_DVS_FILE_OFFSET;
    cave=base+(u32)cave_guest;

    g_patch.snap.rom_base=base;
    g_patch.snap.generate_host_address=gen;
    g_patch.snap.cave_host_address=cave;
    g_patch.snap.cave_guest_address=cave_guest;
    g_patch.snap.routine_size=GATE_PATCH_SPAN;

    g_patch.snap.generate_signature_ok=bytes_equal(gen,k_generate_dvs_original,sizeof(k_generate_dvs_original));
    g_patch.snap.cave_ok=bytes_zero(cave,GATE_PATCH_SPAN);

    if (!g_patch.snap.generate_signature_ok)
    {
        g_patch.snap.error_code=1u;
        g_patch.snap.state=ASLC_GPATCH_ERROR;
        return false;
    }
    if (!g_patch.snap.cave_ok)
    {
        g_patch.snap.error_code=2u;
        g_patch.snap.state=ASLC_GPATCH_ERROR;
        return false;
    }
    if (!writable_address(gen) || !writable_address(cave))
    {
        g_patch.snap.error_code=3u;
        g_patch.snap.state=ASLC_GPATCH_ERROR;
        return false;
    }

    memcpy(g_patch.saved_generate,(const void *)gen,sizeof(g_patch.saved_generate));
    memcpy(g_patch.saved_cave,(const void *)cave,sizeof(g_patch.saved_cave));
    g_patch.prepared=true;
    g_patch.snap.state=ASLC_GPATCH_READY;
    return true;
}

static bool install_patch(void)
{
    const u32 gen=g_patch.snap.generate_host_address;
    const u32 cave=g_patch.snap.cave_host_address;
    u8 generate_patch[3];
    u8 gate[sizeof(k_celebi_vblank_gate)];
    const u16 control_guest=(u16)(g_patch.snap.cave_guest_address+GATE_CONTROL_OFFSET);

    if (!g_patch.prepared || !gen || !cave) return false;

    make_generate_patch(generate_patch,g_patch.snap.cave_guest_address);
    memcpy(gate,k_celebi_vblank_gate,sizeof(gate));
    gate[GATE_CONTROL_OPERAND_OFFSET]=(u8)(control_guest&0xFFu);
    gate[GATE_CONTROL_OPERAND_OFFSET+1u]=(u8)(control_guest>>8);

    /* Install target first; expose the CALL only when the gate is complete. */
    memcpy((void *)cave,gate,sizeof(gate));
    *((volatile u8 *)(cave+GATE_CONTROL_OFFSET))=1u;
    memory_barrier();
    memcpy((void *)gen,generate_patch,sizeof(generate_patch));
    memory_barrier();

    g_patch.snap.install_verified=
        bytes_equal(cave,gate,sizeof(gate)) &&
        *((volatile const u8 *)(cave+GATE_CONTROL_OFFSET))==1u &&
        bytes_equal(gen,generate_patch,sizeof(generate_patch));
    if (!g_patch.snap.install_verified)
    {
        g_patch.snap.error_code=4u;
        g_patch.snap.state=ASLC_GPATCH_ERROR;
        return false;
    }

    g_patch.installed=true;
    g_patch.disarm_frames=0u;
    g_patch.snap.state=ASLC_GPATCH_INSTALLED;
    asl_debug_log("[ASL2] real-VBlank Celebi guest gate installed\n");
    return true;
}

static bool restore_patch(void)
{
    const u32 gen=g_patch.snap.generate_host_address;
    const u32 cave=g_patch.snap.cave_host_address;

    if (!g_patch.prepared || !gen || !cave) return true;

    /* Remove the CALL before restoring the code cave. */
    memcpy((void *)gen,g_patch.saved_generate,sizeof(g_patch.saved_generate));
    memory_barrier();
    memcpy((void *)cave,g_patch.saved_cave,sizeof(g_patch.saved_cave));
    memory_barrier();

    g_patch.snap.restore_verified=
        bytes_equal(gen,g_patch.saved_generate,sizeof(g_patch.saved_generate)) &&
        bytes_equal(cave,g_patch.saved_cave,sizeof(g_patch.saved_cave));
    if (!g_patch.snap.restore_verified)
    {
        g_patch.snap.error_code=5u;
        g_patch.snap.state=ASLC_GPATCH_ERROR;
        return false;
    }

    g_patch.installed=false;
    g_patch.snap.install_verified=false;
    g_patch.snap.state=ASLC_GPATCH_READY;
    asl_debug_log("[ASL2] real-VBlank Celebi guest gate restored\n");
    return true;
}

void asl_guest_patch_initialize(void)
{
    memset(&g_patch,0,sizeof(g_patch));
    g_patch.snap.state=ASLC_GPATCH_WAITING;
    g_patch.snap.routine_size=GATE_PATCH_SPAN;
}

void asl_guest_patch_step(const AslRomProbeSnapshot *probe,bool armed)
{
    if (g_patch.snap.state==ASLC_GPATCH_ERROR) return;

    if (g_patch.prepared && probe && probe->rom_base[0] && probe->rom_base[0]!=g_patch.snap.rom_base)
    {
        if (g_patch.installed) (void)restore_patch();
        asl_guest_patch_initialize();
    }

    if (!g_patch.prepared)
    {
        if (!probe || !probe->scan_done)
        {
            g_patch.snap.state=ASLC_GPATCH_WAITING;
            return;
        }
        if (!prepare_from_probe(probe))
        {
            if (g_patch.snap.state!=ASLC_GPATCH_ERROR)
            {
                g_patch.snap.error_code=6u;
                g_patch.snap.state=ASLC_GPATCH_ERROR;
            }
            return;
        }
    }

    if (armed)
    {
        g_patch.disarm_frames=0u;
        if (!g_patch.installed)
        {
            (void)install_patch();
        }
        else
        {
            *((volatile u8 *)(g_patch.snap.cave_host_address+GATE_CONTROL_OFFSET))=1u;
            memory_barrier();
        }
    }
    else if (g_patch.installed)
    {
        /* First tell any gate already executing to release on its next loop.
           Only restore executable bytes after a one-second grace period. */
        *((volatile u8 *)(g_patch.snap.cave_host_address+GATE_CONTROL_OFFSET))=0u;
        memory_barrier();
        if (g_patch.disarm_frames<SAFE_DISARM_FRAMES)
            ++g_patch.disarm_frames;
        else
            (void)restore_patch();
    }
}

bool asl_guest_patch_reset(void)
{
    if (g_patch.installed && !restore_patch())
        return false;

    asl_guest_patch_initialize();
    return true;
}

void asl_guest_patch_get_snapshot(AslGuestPatchSnapshot *out)
{
    if (!out) return;
    *out=g_patch.snap;
}

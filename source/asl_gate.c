#include "asl_gate.h"

#include "asl_game.h"
#include "asl_platform.h"
#include "asl_predictor.h"
#include "csvc.h"

#include <string.h>

/*
 * Host/emulator addresses for the tested Gen I Virtual Console build.
 *
 * The plugin deliberately keeps these values in one place because they are the
 * part most likely to change between VC revisions. If a different build moves
 * the emulator state or helper functions, the hook sanity checks should fail
 * open instead of patching an unexpected instruction stream.
 */
#define GB_CPU_STATE_ADDRESS       0x0021A5F8u
#define WRAM_SLOT_ADDRESS          0x0021A6CCu
#define HRAM_SLOT_ADDRESS          0x0021A6DCu
#define GB_READ8_ADDRESS           0x00165264u

#define F0_CALL_ADDRESS            0x001A7FF8u
#define E0_CALL_ADDRESS            0x001A7D64u
#define F0_RETURN_ADDRESS          0x001A7FFCu
#define E0_RETURN_ADDRESS          0x001A7D68u

#define DIV_COUNTDOWN_ADDRESS      0x0021AA5Cu
#define CURRENT_MCYCLES_ADDRESS    0x0021A608u

/* Game Boy I/O and HRAM locations used by the gate. */
#define DIV_REGISTER              0xFF04u
#define IF_REGISTER               0xFF0Fu
#define IE_REGISTER               0xFFFFu
#define SB_REGISTER               0xFF01u
#define SC_REGISTER               0xFF02u
#define SCX_REGISTER              0xFF43u
#define SCY_REGISTER              0xFF42u
#define WX_REGISTER               0xFF4Bu
#define WY_REGISTER               0xFF4Au
#define VBLANK_OCCURRED           0xFFD6u

#define RANDOM_ADD_OFFSET         0x53u /* FFD3 - FF80 */
#define LOADED_ROM_BANK_OFFSET    0x38u /* FFB8 - FF80 */

#define SOFT_RESET_KEYS (KEY_A | KEY_B | KEY_START | KEY_SELECT)
#define ARRAY_SIZE(array) ((u32)(sizeof(array) / sizeof((array)[0])))
#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)

typedef u32 (*GbRead8Function)(u32);

typedef struct GateRuntime
{
    const AslGameLayout * volatile layout;

    volatile bool hooks_installed;
    volatile bool enabled;
    volatile AslGateStage stage;

    /* SCX writes provide one validated marker per guest VBlank. */
    volatile u64 vblank;

    /* State shared by the F0 before/after halves of the raw sub-wrap. */
    volatile u32 last_f0_address;
    volatile u16 last_f0_pc;
    volatile bool origin_candidate;

    /* DelayFrame hold state. */
    volatile bool holding;
    volatile bool have_checked_vblank;
    volatile u64 last_checked_vblank;
    volatile u32 extra_vblanks;
    volatile bool abort_requested;

    AslPrediction prediction;
    volatile u8 target_species;

    /* Post-release verification state. */
    volatile bool verification_pending;
    volatile bool verification_done;
    volatile bool verification_match;
    volatile u64 release_vblank;
    volatile u8 actual_dv1;
    volatile u8 actual_dv2;

    /* Guest reset detection. */
    volatile u8 guest_init_signature_pos;

    /* Lazy ROM detection. */
    volatile u32 rom_probe_ticks;
    volatile bool rom_title_seen;
    char rom_title[17];

    /* UI-side input latches. */
    bool soft_reset_latched;
    bool select_pending;
} GateRuntime;

static GateRuntime g_gate;

/*
 * The original destinations of the two patched BL instructions are referenced
 * by name from naked assembly below, so they must remain addressable symbols.
 */
static volatile u32 g_f0_original_target __attribute__((used));
static volatile u32 g_e0_original_target __attribute__((used));

/*
 * VC menu Reset and the in-game soft reset both eventually execute the game's
 * Init routine. Red, Blue and Yellow begin Init with this same ordered sequence
 * of LDH (a8),A writes. Matching all eight writes is much safer than treating a
 * single register write as a reset signal, because SCX and several other I/O
 * registers are also touched during normal gameplay.
 */
static const u16 k_guest_init_signature[] = {
    IF_REGISTER,
    IE_REGISTER,
    SCX_REGISTER,
    SCY_REGISTER,
    SB_REGISTER,
    SC_REGISTER,
    WX_REGISTER,
    WY_REGISTER
};

static void reset_run_state(void);

/* -------------------------------------------------------------------------
 * Guest emulator access and ROM detection
 * ------------------------------------------------------------------------- */

static u16 guest_pc(void)
{
    return *(volatile u16 *)(GB_CPU_STATE_ADDRESS + 4u);
}

static u16 guest_sp(void)
{
    return *(volatile u16 *)(GB_CPU_STATE_ADDRESS + 6u);
}

static u8 read_guest8(u16 address)
{
    return (u8)((GbRead8Function)GB_READ8_ADDRESS)((u32)address);
}

static u16 read_guest16(u16 address)
{
    return (u16)read_guest8(address) |
           (u16)((u16)read_guest8((u16)(address + 1u)) << 8);
}

static bool read_mapped_ram_base(u32 slot_address, u32 *out)
{
    const u32 base = *(volatile u32 *)slot_address;

    if (!out || base < 0x08000000u || base >= 0x0A000000u)
        return false;

    *out = base;
    return true;
}

static bool read_loaded_rom_bank(u8 *out)
{
    u32 hram;

    if (!out || !read_mapped_ram_base(HRAM_SLOT_ADDRESS, &hram))
        return false;

    *out = *(volatile u8 *)(hram + LOADED_ROM_BANK_OFFSET);
    return true;
}

static bool read_random_add(u8 *out)
{
    u32 hram;

    if (!out || !read_mapped_ram_base(HRAM_SLOT_ADDRESS, &hram))
        return false;

    *out = *(volatile u8 *)(hram + RANDOM_ADD_OFFSET);
    return true;
}

static bool read_divider_countdown(s32 *out)
{
    const s32 countdown = *(volatile s32 *)DIV_COUNTDOWN_ADDRESS;

    if (!out || countdown < 1 || countdown > 64)
        return false;

    *out = countdown;
    return true;
}

static s32 read_current_mcycles(void)
{
    return *(volatile s32 *)CURRENT_MCYCLES_ADDRESS;
}

static const AslGameLayout *detect_game_layout(void)
{
    u8 raw_title[16];
    char display_title[17];
    bool meaningful = false;
    u32 i;

    /*
     * A raw 3GX can start before the emulated ROM header is populated. Treat an
     * all-00/all-FF header as "not ready" rather than "unsupported", then probe
     * again later from the hot emulator callbacks.
     */
    for (i = 0; i < 16u; ++i)
    {
        const u8 byte = read_guest8((u16)(0x0134u + i));
        raw_title[i] = byte;

        if (byte != 0x00u && byte != 0xFFu)
            meaningful = true;

        display_title[i] =
            (byte >= 0x20u && byte <= 0x7Eu) ? (char)byte : ' ';
    }
    display_title[16] = '\0';

    if (!meaningful)
        return NULL;

    /* Keep a printable diagnostic title for unsupported ROMs. */
    for (i = 16u; i > 0u; --i)
    {
        if (display_title[i - 1u] != ' ')
            break;
        display_title[i - 1u] = '\0';
    }

    memcpy(g_gate.rom_title, display_title, sizeof(g_gate.rom_title));
    g_gate.rom_title_seen = true;

    return asl_game_find_by_rom_title(raw_title);
}

static void try_detect_game_layout(void)
{
    const AslGameLayout *detected;

    if (g_gate.layout)
        return;

    /* F0/E0 are hot emulator paths, so avoid reading the ROM title every hit. */
    if ((g_gate.rom_probe_ticks++ & 0x3Fu) != 0u)
        return;

    detected = detect_game_layout();
    if (!detected)
        return;

    g_gate.layout = detected;
    g_gate.stage = g_gate.hooks_installed ? ASL_GATE_READY : ASL_GATE_ERROR;
    asl_debug_log("[ASL] guest ROM detected after VC initialization\n");
}

/* -------------------------------------------------------------------------
 * Candidate sampling and exact gate identification
 * ------------------------------------------------------------------------- */

static u16 legendary_timing_adjustment(u8 species)
{
    /*
     * The base timing constants were measured on Mewtwo (#150). Before the
     * DV RNG calls, GetMonHeader walks the base-stats table with AddNTimes.
     * The three legendary birds have lower Pokédex numbers, so they complete
     * that loop earlier than Mewtwo.
     *
     * One normal AddNTimes iteration costs 6 M-cycles. Subtracting the
     * species-dependent difference keeps the predictor aligned with the two
     * BattleRandom calls without changing the already validated Mewtwo path.
     */
    switch (species)
    {
        case 0x49u: /* Moltres   #146: (150 - 146) * 6 */
            return 0x18u;

        case 0x4Au: /* Articuno  #144: (150 - 144) * 6 */
            return 0x24u;

        case 0x4Bu: /* Zapdos    #145: (150 - 145) * 6 */
            return 0x1Eu;

        case 0x83u: /* Mewtwo    #150 */
        default:
            return 0u;
    }
}

static bool sample_prediction(u8 species, AslPrediction *out)
{
    const AslGameLayout *layout = g_gate.layout;
    const u16 timing_adjustment = legendary_timing_adjustment(species);
    u8 random_add;
    u8 div;
    s32 divider_countdown;
    s32 current_mcycles;

    if (!out || !layout)
        return false;

    if (layout->origin_to_dv1 < timing_adjustment ||
        layout->origin_to_dv2 < timing_adjustment)
    {
        return false;
    }

    if (!read_random_add(&random_add) ||
        !read_divider_countdown(&divider_countdown))
    {
        return false;
    }

    div = read_guest8(DIV_REGISTER);
    current_mcycles = read_current_mcycles();

    return asl_predictor_predict(
        random_add,
        div,
        divider_countdown,
        current_mcycles,
        (u16)(layout->origin_to_dv1 - timing_adjustment),
        (u16)(layout->origin_to_dv2 - timing_adjustment),
        out);
}

static bool is_exact_final_delay_frame_read(u16 pc, u32 address)
{
    const AslGameLayout *layout = g_gate.layout;
    u16 return_address;
    u8 bank;

    if (!layout ||
        pc != layout->delay_frame_poll_pc ||
        address != VBLANK_OCCURRED)
    {
        return false;
    }

    /*
     * The same LDH helper is used all over the emulator. Guest PC alone is not
     * enough to identify the gate safely, so also validate the Game Boy return
     * address and currently loaded ROM bank. Together these identify the final
     * DelayFrame inside PlayBattleMusic immediately before DV generation.
     */
    return_address = read_guest16(guest_sp());
    if (!read_loaded_rom_bank(&bank))
        return false;

    return return_address == layout->return_after_delay_frame &&
           bank == layout->play_battle_music_bank;
}

/* -------------------------------------------------------------------------
 * F0 read hook: shiny gate state machine
 * ------------------------------------------------------------------------- */

static __attribute__((used, noinline)) void capture_f0_before(u32 unused)
{
    u16 pc;
    u8 immediate;

    (void)unused;

    if (!g_gate.enabled)
    {
        g_gate.last_f0_address = 0xFFFFFFFFu;
        g_gate.last_f0_pc = 0;
        g_gate.origin_candidate = false;
        return;
    }

    try_detect_game_layout();

    pc = guest_pc();
    immediate = read_guest8(pc);

    g_gate.last_f0_pc = pc;
    g_gate.last_f0_address = 0xFF00u | (u32)immediate;
    g_gate.origin_candidate =
        g_gate.layout &&
        is_exact_final_delay_frame_read(pc, g_gate.last_f0_address);
}

static void arm_verification(void)
{
    g_gate.release_vblank = g_gate.vblank;
    g_gate.verification_pending = true;
    g_gate.verification_done = false;
    g_gate.verification_match = false;
    g_gate.actual_dv1 = 0;
    g_gate.actual_dv2 = 0;
}

static __attribute__((used, noinline)) u32 filter_f0_result(u32 value)
{
    const u32 address = g_gate.last_f0_address;
    const u16 pc = g_gate.last_f0_pc;
    const bool origin_candidate = g_gate.origin_candidate;
    const AslGameLayout *layout = g_gate.layout;
    AslPrediction prediction;
    u8 species;

    /* Consume the before-callback snapshot exactly once. */
    g_gate.last_f0_address = 0xFFFFFFFFu;
    g_gate.last_f0_pc = 0;
    g_gate.origin_candidate = false;

    /* Disabled mode is a strict pass-through. */
    if (!g_gate.enabled || !layout || !g_gate.hooks_installed)
        return value;

    if (!origin_candidate ||
        address != VBLANK_OCCURRED ||
        pc != layout->delay_frame_poll_pc)
    {
        return value;
    }

    /* A non-zero hVBlankOccurred already keeps DelayFrame inside its loop. */
    if (value != 0u)
        return value;

    if (g_gate.stage == ASL_GATE_RELEASED_SHINY ||
        g_gate.stage == ASL_GATE_ABORTED ||
        g_gate.stage == ASL_GATE_ERROR)
    {
        return value;
    }

    species = read_guest8(layout->w_cur_opponent);
    if (!asl_game_is_supported_legendary(species))
    {
        g_gate.target_species = 0;
        g_gate.stage = ASL_GATE_READY;
        return 0;
    }
    g_gate.target_species = species;

    if (g_gate.abort_requested)
    {
        g_gate.abort_requested = false;
        g_gate.holding = false;
        g_gate.stage = ASL_GATE_ABORTED;
        return 0;
    }

    /*
     * We spoof only the CPU return value of this one hVBlankOccurred read; the
     * guest byte itself remains untouched. A non-VBlank interrupt can therefore
     * wake HALT while the real byte is still zero. If SCX has not advanced, keep
     * returning 1 so the original DelayFrame waits for a real new VBlank.
     */
    if (g_gate.holding &&
        g_gate.have_checked_vblank &&
        g_gate.vblank == g_gate.last_checked_vblank)
    {
        return 1;
    }

    if (!sample_prediction(species, &prediction))
    {
        g_gate.holding = false;
        g_gate.stage = ASL_GATE_ERROR;
        return 0; /* Fail open rather than trap the game in DelayFrame. */
    }

    g_gate.prediction = prediction;

    /* SELECT can be processed by the present hook while this callback runs. */
    if (!g_gate.enabled)
    {
        g_gate.holding = false;
        return value;
    }

    g_gate.have_checked_vblank = true;
    g_gate.last_checked_vblank = g_gate.vblank;

    if (prediction.shiny)
    {
        arm_verification();
        g_gate.holding = false;
        g_gate.stage = ASL_GATE_RELEASED_SHINY;
        return 0;
    }

    g_gate.holding = true;
    ++g_gate.extra_vblanks;
    g_gate.stage = ASL_GATE_HOLDING;

    /* Make this read look non-zero so the game's own DelayFrame loops once. */
    return 1;
}

/* -------------------------------------------------------------------------
 * E0 write hook: VBlank marker and guest reset detection
 * ------------------------------------------------------------------------- */

static void observe_guest_init_write(u32 address)
{
    const u16 guest_address = (u16)address;
    const u8 count = (u8)ARRAY_SIZE(k_guest_init_signature);
    u8 position = g_gate.guest_init_signature_pos;

    if (position >= count)
        position = 0;

    if (guest_address == k_guest_init_signature[position])
    {
        ++position;

        if (position == count)
        {
            g_gate.guest_init_signature_pos = 0;
            reset_run_state();
            asl_debug_log("[ASL] guest Init detected; run state cleared\n");
            return;
        }

        g_gate.guest_init_signature_pos = position;
        return;
    }

    /* The mismatching write may itself be the start of the next signature. */
    g_gate.guest_init_signature_pos =
        guest_address == k_guest_init_signature[0] ? 1u : 0u;
}

static __attribute__((used, noinline)) void capture_e0_before(
    u32 address,
    u32 value)
{
    (void)value;

    /*
     * Guest-reset bookkeeping stays active while the gate is disabled. It does
     * not alter emulation and ensures stale result text is cleared before the
     * user enables ASL again after a VC-menu Reset.
     */
    observe_guest_init_write(address);

    if (!g_gate.enabled)
        return;

    try_detect_game_layout();

    if (address == SCX_REGISTER)
        ++g_gate.vblank;
}

/* -------------------------------------------------------------------------
 * Raw ARM sub-wrap bridges
 * ------------------------------------------------------------------------- */

/*
 * Raw replacements for CTRPluginFramework's sub-wrap hook.
 *
 * The call-site instruction is replaced with B (not BL), so the bridge must
 * reproduce the original BL semantics explicitly:
 *   1. preserve original arguments;
 *   2. run the before callback;
 *   3. call the original helper;
 *   4. run the optional after/filter callback;
 *   5. restore LR to the original call-site continuation and jump there.
 *
 * F0 may replace r0 because r0 is the LDH read result. E0 only observes the
 * write, so it has no after callback.
 */
static __attribute__((naked, noinline, used)) void f0_bridge(void)
{
    __asm__ volatile(
        "push {r0-r3, r12, lr}\n"
        "bl capture_f0_before\n"
        "pop {r0-r3, r12, lr}\n"

        "ldr r12, =g_f0_original_target\n"
        "ldr r12, [r12]\n"
        "blx r12\n"

        "push {r1-r4, r12, lr}\n"
        "bl filter_f0_result\n"
        "pop {r1-r4, r12, lr}\n"

        "ldr lr, =" STRINGIFY(F0_RETURN_ADDRESS) "\n"
        "ldr pc, =" STRINGIFY(F0_RETURN_ADDRESS) "\n"
    );
}

static __attribute__((naked, noinline, used)) void e0_bridge(void)
{
    __asm__ volatile(
        "push {r0-r3, r12, lr}\n"
        "bl capture_e0_before\n"
        "pop {r0-r3, r12, lr}\n"

        "ldr r12, =g_e0_original_target\n"
        "ldr r12, [r12]\n"
        "blx r12\n"

        "ldr lr, =" STRINGIFY(E0_RETURN_ADDRESS) "\n"
        "ldr pc, =" STRINGIFY(E0_RETURN_ADDRESS) "\n"
    );
}

/* -------------------------------------------------------------------------
 * Post-release verification and run-state reset
 * ------------------------------------------------------------------------- */

static void try_verify_generated_dvs(void)
{
    const AslGameLayout *layout = g_gate.layout;
    u32 wram;
    u32 offset;
    const volatile u8 *dvs;
    u8 actual_dv1;
    u8 actual_dv2;

    if (!g_gate.verification_pending ||
        g_gate.verification_done ||
        !layout ||
        g_gate.stage != ASL_GATE_RELEASED_SHINY)
    {
        return;
    }

    /*
     * There is no VBlank between the gate origin and DV generation. Waiting for
     * the next validated VBlank guarantees wEnemyMonDVs has already been filled
     * without needing another invasive hook in the guest battle path.
     */
    if (g_gate.vblank <= g_gate.release_vblank)
        return;

    if (!read_mapped_ram_base(WRAM_SLOT_ADDRESS, &wram))
        return;

    offset = (u32)(layout->w_enemy_mon_dvs - 0xC000u);
    dvs = (const volatile u8 *)(wram + offset);

    /* WRAM stores Attack/Defense first, then Speed/Special. */
    actual_dv2 = dvs[0];
    actual_dv1 = dvs[1];

    g_gate.actual_dv1 = actual_dv1;
    g_gate.actual_dv2 = actual_dv2;
    g_gate.verification_match =
        actual_dv1 == g_gate.prediction.dv1 &&
        actual_dv2 == g_gate.prediction.dv2;
    g_gate.verification_done = true;
    g_gate.verification_pending = false;
}

static void reset_run_state(void)
{
    memset(&g_gate.prediction, 0, sizeof(g_gate.prediction));

    g_gate.last_f0_address = 0xFFFFFFFFu;
    g_gate.last_f0_pc = 0;
    g_gate.origin_candidate = false;

    g_gate.holding = false;
    g_gate.have_checked_vblank = false;
    g_gate.last_checked_vblank = 0;
    g_gate.extra_vblanks = 0;
    g_gate.abort_requested = false;
    g_gate.target_species = 0;
    g_gate.guest_init_signature_pos = 0;

    g_gate.verification_pending = false;
    g_gate.verification_done = false;
    g_gate.verification_match = false;
    g_gate.release_vblank = 0;
    g_gate.actual_dv1 = 0;
    g_gate.actual_dv2 = 0;

    g_gate.stage =
        (g_gate.layout && g_gate.hooks_installed)
            ? ASL_GATE_READY
            : ASL_GATE_ERROR;
}

/* -------------------------------------------------------------------------
 * ARM patching helpers and gate-hook installation
 * ------------------------------------------------------------------------- */

static bool address_in_mapping(u32 address, const u8 *base, u32 size)
{
    const u32 begin = (u32)base;
    const u32 end = begin + size;

    if (!base || size < 4u || end < begin)
        return false;

    return address >= begin && address <= end - 4u;
}

static bool is_arm_bl(u32 instruction)
{
    return (instruction >> 24) == 0xEBu;
}

static u32 decode_arm_branch_target(u32 source, u32 instruction)
{
    s32 words = (s32)(instruction & 0x00FFFFFFu);

    if ((words & 0x00800000) != 0)
        words |= (s32)0xFF000000u;

    return (u32)((s64)(source + 8u) + (s64)words * 4LL);
}

static bool encode_arm_branch(u32 source, u32 destination, u32 *out)
{
    const s64 delta64 = (s64)destination - (s64)(source + 8u);
    s32 delta;

    if (!out ||
        (delta64 & 3LL) != 0 ||
        delta64 < -33554432LL ||
        delta64 > 33554428LL)
    {
        return false;
    }

    delta = (s32)delta64;
    *out = 0xEA000000u | (((u32)(delta >> 2)) & 0x00FFFFFFu);
    return true;
}

static u8 *find_zero_code_cave(u8 *base, u32 size, u32 minimum_bytes)
{
    u32 position;

    if (!base ||
        minimum_bytes == 0u ||
        (minimum_bytes & 3u) != 0u ||
        size < minimum_bytes)
    {
        return NULL;
    }

    /*
     * The raw plugin lives near 0x07000000, far outside the +/-32 MiB range of
     * an ARM B from the emulator code around 0x001Axxxx. We therefore need two
     * tiny absolute-jump islands inside the title's own executable mapping.
     *
     * Search backward for zero padding and require at least 32 bytes even
     * though the two islands use only 16. The larger requirement reduces the
     * chance of mistaking an isolated literal/data word for intentional padding.
     */
    if (minimum_bytes < 32u)
        minimum_bytes = 32u;

    position = (size - minimum_bytes) & ~3u;

    for (;;)
    {
        u32 i;
        bool all_zero = true;

        for (i = 0; i < minimum_bytes; ++i)
        {
            if (base[position + i] != 0u)
            {
                all_zero = false;
                break;
            }
        }

        if (all_zero)
            return base + position;

        if (position < 4u)
            break;
        position -= 4u;
    }

    return NULL;
}

static bool install_gate_hooks(u8 *code_base, u32 code_size)
{
    u32 f0_instruction;
    u32 e0_instruction;
    u8 *code_cave;
    u32 *code_cave_patch;
    u32 *f0_patch;
    u32 *e0_patch;
    u32 f0_branch;
    u32 e0_branch;

    if (!address_in_mapping(F0_CALL_ADDRESS, code_base, code_size) ||
        !address_in_mapping(E0_CALL_ADDRESS, code_base, code_size))
    {
        asl_debug_log("[ASL] gate hook addresses are outside the title code mapping\n");
        return false;
    }

    f0_instruction = *(volatile u32 *)F0_CALL_ADDRESS;
    e0_instruction = *(volatile u32 *)E0_CALL_ADDRESS;

    if (!is_arm_bl(f0_instruction) || !is_arm_bl(e0_instruction))
    {
        asl_debug_log("[ASL] gate hook sanity check failed: expected ARM BL\n");
        return false;
    }

    code_cave = find_zero_code_cave(code_base, code_size, 32u);
    if (!code_cave)
    {
        asl_debug_log("[ASL] no suitable code cave found for branch islands\n");
        return false;
    }

    if ((u32)code_cave == F0_CALL_ADDRESS ||
        (u32)code_cave == E0_CALL_ADDRESS)
    {
        return false;
    }

    if (!encode_arm_branch(F0_CALL_ADDRESS, (u32)code_cave, &f0_branch) ||
        !encode_arm_branch(E0_CALL_ADDRESS, (u32)code_cave + 8u, &e0_branch))
    {
        asl_debug_log("[ASL] code cave is outside ARM branch range\n");
        return false;
    }

    code_cave_patch = (u32 *)asl_physical_alias(code_cave);
    f0_patch = (u32 *)asl_physical_alias((void *)F0_CALL_ADDRESS);
    e0_patch = (u32 *)asl_physical_alias((void *)E0_CALL_ADDRESS);

    if (!code_cave_patch || !f0_patch || !e0_patch)
    {
        asl_debug_log("[ASL] physical alias failed while installing gate hooks\n");
        return false;
    }

    g_f0_original_target =
        decode_arm_branch_target(F0_CALL_ADDRESS, f0_instruction);
    g_e0_original_target =
        decode_arm_branch_target(E0_CALL_ADDRESS, e0_instruction);

    /*
     * Each island is an 8-byte absolute jump:
     *
     *     ldr pc, [pc, #-4]
     *     .word bridge_address
     */
    code_cave_patch[0] = 0xE51FF004u;
    code_cave_patch[1] = (u32)f0_bridge;
    code_cave_patch[2] = 0xE51FF004u;
    code_cave_patch[3] = (u32)e0_bridge;

    *f0_patch = f0_branch;
    *e0_patch = e0_branch;

    svcInvalidateEntireInstructionCache();
    asl_debug_log("[ASL] F0/E0 gate hooks installed\n");
    return true;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

bool asl_gate_initialize(u8 *code_base, u32 code_size)
{
    memset(&g_gate, 0, sizeof(g_gate));

    g_gate.enabled = true;
    g_gate.last_f0_address = 0xFFFFFFFFu;
    g_gate.stage = ASL_GATE_ERROR;

    /*
     * The guest ROM may not be initialized yet. Install host-side hooks first;
     * ROM detection is intentionally lazy and completes from F0/E0 callbacks
     * once the emulator is actively executing Game Boy code.
     */
    g_gate.hooks_installed = install_gate_hooks(code_base, code_size);
    reset_run_state();

    if (g_gate.hooks_installed)
        asl_debug_log("[ASL] gate ready; waiting for guest ROM\n");

    return g_gate.hooks_installed;
}

void asl_gate_on_frame(u32 keys_down, u32 keys_held)
{
    const u32 reset_modifiers = KEY_A | KEY_B | KEY_START;
    const bool soft_reset_held =
        (keys_held & SOFT_RESET_KEYS) == SOFT_RESET_KEYS;

    /*
     * Handle A+B+START+SELECT before the standalone SELECT toggle. SELECT is
     * toggled on release, not press, so a slightly staggered reset chord cannot
     * accidentally enable or disable the plugin.
     */
    if (soft_reset_held)
    {
        g_gate.select_pending = false;

        if (!g_gate.soft_reset_latched)
        {
            g_gate.soft_reset_latched = true;
            reset_run_state();
            asl_debug_log("[ASL] in-game soft reset detected; run state cleared\n");
        }
        return;
    }

    if (g_gate.soft_reset_latched)
    {
        if ((keys_held & SOFT_RESET_KEYS) != 0u)
            return;
        g_gate.soft_reset_latched = false;
    }

    if ((keys_down & KEY_SELECT) != 0u)
        g_gate.select_pending = true;

    if (g_gate.select_pending)
    {
        if ((keys_held & KEY_SELECT) != 0u ||
            (keys_held & reset_modifiers) != 0u)
        {
            return;
        }

        g_gate.select_pending = false;
        g_gate.enabled = !g_gate.enabled;

        /*
         * Clearing a live hold is essential when disabling. The next F0 result
         * is then passed through unchanged and the game's original DelayFrame
         * exits normally instead of remaining trapped in an ASL-owned state.
         */
        reset_run_state();

        asl_debug_log(
            g_gate.enabled
                ? "[ASL] enabled by SELECT\n"
                : "[ASL] disabled by SELECT\n");
        return;
    }

    if (!g_gate.enabled)
        return;

    if ((keys_down & KEY_X) != 0u)
    {
        if (g_gate.stage == ASL_GATE_HOLDING)
            g_gate.abort_requested = true;
        else
            reset_run_state();
    }

    try_verify_generated_dvs();
}

void asl_gate_get_snapshot(AslGateSnapshot *out)
{
    if (!out)
        return;

    out->enabled = g_gate.enabled;
    out->supported = g_gate.layout != NULL;
    out->hooks_installed = g_gate.hooks_installed;

    out->rom_title_seen = g_gate.rom_title_seen;
    out->rom_title = g_gate.rom_title_seen ? g_gate.rom_title : "";
    out->game_name = g_gate.layout ? g_gate.layout->name : "?";

    out->target_species = g_gate.target_species;
    out->target_name = asl_game_species_name(g_gate.target_species);

    out->stage = g_gate.stage;
    out->extra_vblanks = g_gate.extra_vblanks;

    out->prediction_valid = g_gate.prediction.valid;
    out->predicted_dv1 = g_gate.prediction.dv1;
    out->predicted_dv2 = g_gate.prediction.dv2;

    out->verification_done = g_gate.verification_done;
    out->verification_match = g_gate.verification_match;
    out->actual_dv1 = g_gate.actual_dv1;
    out->actual_dv2 = g_gate.actual_dv2;
}

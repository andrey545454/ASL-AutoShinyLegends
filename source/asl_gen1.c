#include "asl_gen1.h"
#include "asl_platform.h"
#include "csvc.h"
#include <string.h>

/* Generation I game layouts and RNG predictor are private to this backend. */
typedef struct AslGen1GameLayout
{
    const char *name;
    const char *rom_title_prefix;
    u16 delay_frame_poll_pc;
    u16 return_after_delay_frame;
    u8 play_battle_music_bank;
    u16 w_cur_opponent;
    u16 w_enemy_mon_dvs;
    u16 origin_to_dv1;
    u16 origin_to_dv2;
} AslGen1GameLayout;

typedef struct AslGen1Prediction
{
    bool valid;
    u8 dv1; /* Speed / Special */
    u8 dv2; /* Attack / Defense */
    bool shiny;
} AslGen1Prediction;

static const AslGen1GameLayout k_yellow = {
    "YELLOW", "POKEMON YELLOW",
    0x1E6Au, 0x5071u, 0x02u,
    0xD058u, 0xCFF0u,
    0x0CF8u, 0x0D6Fu
};
static const AslGen1GameLayout k_red = {
    "RED", "POKEMON RED",
    0x20B5u, 0x50D7u, 0x02u,
    0xD059u, 0xCFF1u,
    0x0C68u, 0x0CE0u
};
static const AslGen1GameLayout k_blue = {
    "BLUE", "POKEMON BLUE",
    0x20B5u, 0x50D7u, 0x02u,
    0xD059u, 0xCFF1u,
    0x0C68u, 0x0CE0u
};

enum {
    SPECIES_MOLTRES = 0x49,
    SPECIES_ARTICUNO = 0x4A,
    SPECIES_ZAPDOS = 0x4B,
    SPECIES_MEWTWO = 0x83
};

static bool title_starts_with(const u8 title[16], const char *prefix)
{
    const size_t length = strlen(prefix);
    return length <= 16u && memcmp(title, prefix, length) == 0;
}

static const AslGen1GameLayout *gen1_find_game_by_rom_title(const u8 title[16])
{
    if (title_starts_with(title, k_yellow.rom_title_prefix)) return &k_yellow;
    if (title_starts_with(title, k_red.rom_title_prefix)) return &k_red;
    if (title_starts_with(title, k_blue.rom_title_prefix)) return &k_blue;
    return NULL;
}

static bool gen1_is_supported_legendary(u8 species)
{
    switch (species)
    {
        case SPECIES_MOLTRES:
        case SPECIES_ARTICUNO:
        case SPECIES_ZAPDOS:
        case SPECIES_MEWTWO:
            return true;
        default:
            return false;
    }
}

static const char *gen1_species_name(u8 species)
{
    switch (species)
    {
        case SPECIES_MOLTRES: return "MOLTRES";
        case SPECIES_ARTICUNO: return "ARTICUNO";
        case SPECIES_ZAPDOS: return "ZAPDOS";
        case SPECIES_MEWTWO: return "MEWTWO";
        default: return "?";
    }
}

static u16 reconstruct_full_divider(u8 div, s32 countdown)
{
    const u16 phase = (u16)(64 - countdown);
    return (u16)((((u16)div << 6) + phase) & 0x3FFFu);
}

static u16 effective_full_divider(u8 div, s32 countdown, s32 current_mcycles)
{
    const u16 full = reconstruct_full_divider(div, countdown);
    return (u16)(((s32)full + current_mcycles) & 0x3FFF);
}

static u8 div_at_delta(u16 effective_divider, u16 delta_mcycles)
{
    const u16 future = (u16)((effective_divider + delta_mcycles) & 0x3FFFu);
    return (u8)(future >> 6);
}

static bool gen1_is_shiny_dv_pair(u8 dv1, u8 dv2)
{
    if (dv1 != 0xAAu || (dv2 & 0x0Fu) != 0x0Au) return false;
    switch ((u8)(dv2 >> 4))
    {
        case 2:
        case 3:
        case 6:
        case 7:
        case 10:
        case 11:
        case 14:
        case 15:
            return true;
        default:
            return false;
    }
}

static bool gen1_predict(
    u8 random_add,
    u8 div,
    s32 divider_countdown,
    s32 current_mcycles,
    u16 origin_to_dv1,
    u16 origin_to_dv2,
    AslGen1Prediction *out)
{
    u16 effective_divider;
    u8 div1;
    u8 div2;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (divider_countdown < 1 || divider_countdown > 64) return false;

    effective_divider = effective_full_divider(div, divider_countdown, current_mcycles);
    div1 = div_at_delta(effective_divider, origin_to_dv1);
    div2 = div_at_delta(effective_divider, origin_to_dv2);

    out->dv1 = (u8)((u16)random_add + (u16)div1 + 1u);
    out->dv2 = (u8)((u16)out->dv1 + (u16)div2 + 1u);
    out->shiny = gen1_is_shiny_dv_pair(out->dv1, out->dv2);
    out->valid = true;
    return true;
}

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
#define RANDOM_ADD_OFFSET         0x53u
#define LOADED_ROM_BANK_OFFSET    0x38u
#define ARRAY_SIZE(array) ((u32)(sizeof(array) / sizeof((array)[0])))
#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)

typedef u32 (*GbRead8Function)(u32);

typedef enum Gen1Stage
{
    GEN1_WAITING = 0,
    GEN1_READY,
    GEN1_HOLDING,
    GEN1_RELEASED,
    GEN1_ABORTED,
    GEN1_ERROR
} Gen1Stage;

typedef struct Gen1Runtime
{
    const AslGen1GameLayout * volatile layout;
    volatile bool hooks_installed;
    volatile bool enabled;
    volatile Gen1Stage stage;
    volatile u64 vblank;

    volatile u32 last_f0_address;
    volatile u16 last_f0_pc;
    volatile bool origin_candidate;

    volatile bool holding;
    volatile bool have_checked_vblank;
    volatile u64 last_checked_vblank;
    volatile u32 extra_vblanks;
    volatile bool abort_requested;
    AslGen1Prediction prediction;
    volatile u8 target_species;

    volatile bool verification_pending;
    volatile bool verification_done;
    volatile bool verification_match;
    volatile u64 release_vblank;
    volatile u8 actual_dv1;
    volatile u8 actual_dv2;

    volatile u8 guest_init_signature_pos;
    volatile u32 rom_probe_ticks;
    volatile bool rom_title_seen;
    char rom_title[17];
} Gen1Runtime;

static Gen1Runtime g_gen1;
static volatile u32 g_f0_original_target __attribute__((used));
static volatile u32 g_e0_original_target __attribute__((used));

static const u16 k_guest_init_signature[] = {
    IF_REGISTER, IE_REGISTER, SCX_REGISTER, SCY_REGISTER,
    SB_REGISTER, SC_REGISTER, WX_REGISTER, WY_REGISTER
};

static void reset_run_state(void);

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
    if (!out || base < 0x08000000u || base >= 0x0A000000u) return false;
    *out = base;
    return true;
}

static bool read_loaded_rom_bank(u8 *out)
{
    u32 hram;
    if (!out || !read_mapped_ram_base(HRAM_SLOT_ADDRESS, &hram)) return false;
    *out = *(volatile u8 *)(hram + LOADED_ROM_BANK_OFFSET);
    return true;
}

static bool read_random_add(u8 *out)
{
    u32 hram;
    if (!out || !read_mapped_ram_base(HRAM_SLOT_ADDRESS, &hram)) return false;
    *out = *(volatile u8 *)(hram + RANDOM_ADD_OFFSET);
    return true;
}

static bool read_divider_countdown(s32 *out)
{
    const s32 countdown = *(volatile s32 *)DIV_COUNTDOWN_ADDRESS;
    if (!out || countdown < 1 || countdown > 64) return false;
    *out = countdown;
    return true;
}

static s32 read_current_mcycles(void)
{
    return *(volatile s32 *)CURRENT_MCYCLES_ADDRESS;
}

static const AslGen1GameLayout *detect_game_layout(void)
{
    u8 raw_title[16];
    char display_title[17];
    bool meaningful = false;
    u32 i;

    for (i = 0; i < 16u; ++i)
    {
        const u8 byte = read_guest8((u16)(0x0134u + i));
        raw_title[i] = byte;
        if (byte != 0x00u && byte != 0xFFu) meaningful = true;
        display_title[i] = (byte >= 0x20u && byte <= 0x7Eu) ? (char)byte : ' ';
    }
    display_title[16] = '\0';
    if (!meaningful) return NULL;

    for (i = 16u; i > 0u; --i)
    {
        if (display_title[i - 1u] != ' ') break;
        display_title[i - 1u] = '\0';
    }

    memcpy(g_gen1.rom_title, display_title, sizeof(g_gen1.rom_title));
    g_gen1.rom_title_seen = true;
    return gen1_find_game_by_rom_title(raw_title);
}

static void try_detect_game_layout(void)
{
    const AslGen1GameLayout *detected;
    if (g_gen1.layout) return;
    if ((g_gen1.rom_probe_ticks++ & 0x3Fu) != 0u) return;
    detected = detect_game_layout();
    if (!detected) return;
    g_gen1.layout = detected;
    g_gen1.stage = g_gen1.hooks_installed ? GEN1_READY : GEN1_ERROR;
    asl_debug_log("[ASL1] guest ROM detected\n");
}

static u16 legendary_timing_adjustment(u8 species)
{
    switch (species)
    {
        case 0x49u: return 0x18u; /* Moltres #146 */
        case 0x4Au: return 0x24u; /* Articuno #144 */
        case 0x4Bu: return 0x1Eu; /* Zapdos #145 */
        case 0x83u:
        default: return 0u;       /* Mewtwo #150 */
    }
}

static bool sample_prediction(u8 species, AslGen1Prediction *out)
{
    const AslGen1GameLayout *layout = g_gen1.layout;
    const u16 adjustment = legendary_timing_adjustment(species);
    u8 random_add;
    u8 div;
    s32 countdown;
    s32 current_mcycles;

    if (!out || !layout) return false;
    if (layout->origin_to_dv1 < adjustment || layout->origin_to_dv2 < adjustment) return false;
    if (!read_random_add(&random_add) || !read_divider_countdown(&countdown)) return false;

    div = read_guest8(DIV_REGISTER);
    current_mcycles = read_current_mcycles();
    return gen1_predict(
        random_add,
        div,
        countdown,
        current_mcycles,
        (u16)(layout->origin_to_dv1 - adjustment),
        (u16)(layout->origin_to_dv2 - adjustment),
        out);
}

static bool is_exact_final_delay_frame_read(u16 pc, u32 address)
{
    const AslGen1GameLayout *layout = g_gen1.layout;
    u16 return_address;
    u8 bank;
    if (!layout || pc != layout->delay_frame_poll_pc || address != VBLANK_OCCURRED) return false;
    return_address = read_guest16(guest_sp());
    if (!read_loaded_rom_bank(&bank)) return false;
    return return_address == layout->return_after_delay_frame &&
           bank == layout->play_battle_music_bank;
}

static __attribute__((used, noinline)) void capture_f0_before(u32 unused)
{
    u16 pc;
    u8 immediate;
    (void)unused;

    if (!g_gen1.enabled)
    {
        g_gen1.last_f0_address = 0xFFFFFFFFu;
        g_gen1.last_f0_pc = 0;
        g_gen1.origin_candidate = false;
        return;
    }

    try_detect_game_layout();
    pc = guest_pc();
    immediate = read_guest8(pc);
    g_gen1.last_f0_pc = pc;
    g_gen1.last_f0_address = 0xFF00u | (u32)immediate;
    g_gen1.origin_candidate = g_gen1.layout &&
        is_exact_final_delay_frame_read(pc, g_gen1.last_f0_address);
}

static void arm_verification(void)
{
    g_gen1.release_vblank = g_gen1.vblank;
    g_gen1.verification_pending = true;
    g_gen1.verification_done = false;
    g_gen1.verification_match = false;
    g_gen1.actual_dv1 = 0;
    g_gen1.actual_dv2 = 0;
}

static __attribute__((used, noinline)) u32 filter_f0_result(u32 value)
{
    const u32 address = g_gen1.last_f0_address;
    const u16 pc = g_gen1.last_f0_pc;
    const bool origin_candidate = g_gen1.origin_candidate;
    const AslGen1GameLayout *layout = g_gen1.layout;
    AslGen1Prediction prediction;
    u8 species;

    g_gen1.last_f0_address = 0xFFFFFFFFu;
    g_gen1.last_f0_pc = 0;
    g_gen1.origin_candidate = false;

    if (!g_gen1.enabled || !layout || !g_gen1.hooks_installed) return value;
    if (!origin_candidate || address != VBLANK_OCCURRED || pc != layout->delay_frame_poll_pc) return value;
    if (value != 0u) return value;
    if (g_gen1.stage == GEN1_RELEASED || g_gen1.stage == GEN1_ABORTED || g_gen1.stage == GEN1_ERROR) return value;

    species = read_guest8(layout->w_cur_opponent);
    if (!gen1_is_supported_legendary(species))
    {
        g_gen1.target_species = 0;
        g_gen1.stage = GEN1_READY;
        return 0;
    }
    g_gen1.target_species = species;

    if (g_gen1.abort_requested)
    {
        g_gen1.abort_requested = false;
        g_gen1.holding = false;
        g_gen1.stage = GEN1_ABORTED;
        return 0;
    }

    if (g_gen1.holding && g_gen1.have_checked_vblank && g_gen1.vblank == g_gen1.last_checked_vblank)
        return 1u;

    if (!sample_prediction(species, &prediction))
    {
        g_gen1.holding = false;
        g_gen1.stage = GEN1_ERROR;
        return 0;
    }
    g_gen1.prediction = prediction;
    if (!g_gen1.enabled)
    {
        g_gen1.holding = false;
        return value;
    }

    g_gen1.have_checked_vblank = true;
    g_gen1.last_checked_vblank = g_gen1.vblank;
    if (prediction.shiny)
    {
        arm_verification();
        g_gen1.holding = false;
        g_gen1.stage = GEN1_RELEASED;
        return 0;
    }

    g_gen1.holding = true;
    ++g_gen1.extra_vblanks;
    g_gen1.stage = GEN1_HOLDING;
    return 1u;
}

static void observe_guest_init_write(u32 address)
{
    const u16 guest_address = (u16)address;
    const u8 count = (u8)ARRAY_SIZE(k_guest_init_signature);
    u8 position = g_gen1.guest_init_signature_pos;

    if (position >= count) position = 0;
    if (guest_address == k_guest_init_signature[position])
    {
        ++position;
        if (position == count)
        {
            g_gen1.guest_init_signature_pos = 0;
            reset_run_state();
            asl_debug_log("[ASL1] guest Init detected; run state cleared\n");
            return;
        }
        g_gen1.guest_init_signature_pos = position;
        return;
    }
    g_gen1.guest_init_signature_pos = guest_address == k_guest_init_signature[0] ? 1u : 0u;
}

static __attribute__((used, noinline)) void capture_e0_before(u32 address, u32 value)
{
    (void)value;
    observe_guest_init_write(address);
    if (!g_gen1.enabled) return;
    try_detect_game_layout();
    if (address == SCX_REGISTER) ++g_gen1.vblank;
}

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

static void try_verify_generated_dvs(void)
{
    const AslGen1GameLayout *layout = g_gen1.layout;
    u32 wram;
    u32 offset;
    const volatile u8 *dvs;
    u8 actual_dv1;
    u8 actual_dv2;

    if (!g_gen1.verification_pending || g_gen1.verification_done ||
        !layout || g_gen1.stage != GEN1_RELEASED) return;
    if (g_gen1.vblank <= g_gen1.release_vblank) return;
    if (!read_mapped_ram_base(WRAM_SLOT_ADDRESS, &wram)) return;

    offset = (u32)(layout->w_enemy_mon_dvs - 0xC000u);
    dvs = (const volatile u8 *)(wram + offset);
    actual_dv2 = dvs[0];
    actual_dv1 = dvs[1];
    g_gen1.actual_dv1 = actual_dv1;
    g_gen1.actual_dv2 = actual_dv2;
    g_gen1.verification_match =
        actual_dv1 == g_gen1.prediction.dv1 &&
        actual_dv2 == g_gen1.prediction.dv2;
    g_gen1.verification_done = true;
    g_gen1.verification_pending = false;
}

static void reset_run_state(void)
{
    memset(&g_gen1.prediction, 0, sizeof(g_gen1.prediction));
    g_gen1.last_f0_address = 0xFFFFFFFFu;
    g_gen1.last_f0_pc = 0;
    g_gen1.origin_candidate = false;
    g_gen1.holding = false;
    g_gen1.have_checked_vblank = false;
    g_gen1.last_checked_vblank = 0;
    g_gen1.extra_vblanks = 0;
    g_gen1.abort_requested = false;
    g_gen1.target_species = 0;
    g_gen1.guest_init_signature_pos = 0;
    g_gen1.verification_pending = false;
    g_gen1.verification_done = false;
    g_gen1.verification_match = false;
    g_gen1.release_vblank = 0;
    g_gen1.actual_dv1 = 0;
    g_gen1.actual_dv2 = 0;
    g_gen1.stage = g_gen1.layout && g_gen1.hooks_installed ? GEN1_READY : GEN1_WAITING;
}

static bool address_in_mapping(u32 address, const u8 *base, u32 size)
{
    const u32 begin = (u32)base;
    const u32 end = begin + size;
    if (!base || size < 4u || end < begin) return false;
    return address >= begin && address <= end - 4u;
}

static bool is_arm_bl(u32 instruction)
{
    return (instruction >> 24) == 0xEBu;
}

static u32 decode_arm_branch_target(u32 source, u32 instruction)
{
    s32 words = (s32)(instruction & 0x00FFFFFFu);
    if ((words & 0x00800000) != 0) words |= (s32)0xFF000000u;
    return (u32)((s64)(source + 8u) + (s64)words * 4LL);
}

static bool encode_arm_branch(u32 source, u32 destination, u32 *out)
{
    const s64 delta64 = (s64)destination - (s64)(source + 8u);
    s32 delta;
    if (!out || (delta64 & 3LL) != 0 || delta64 < -33554432LL || delta64 > 33554428LL)
        return false;
    delta = (s32)delta64;
    *out = 0xEA000000u | (((u32)(delta >> 2)) & 0x00FFFFFFu);
    return true;
}

static u8 *find_zero_code_cave(u8 *base, u32 size, u32 minimum_bytes)
{
    u32 position;
    if (!base || minimum_bytes == 0u || (minimum_bytes & 3u) != 0u || size < minimum_bytes)
        return NULL;
    if (minimum_bytes < 32u) minimum_bytes = 32u;
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
        if (all_zero) return base + position;
        if (position < 4u) break;
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
        !address_in_mapping(E0_CALL_ADDRESS, code_base, code_size)) return false;

    f0_instruction = *(volatile u32 *)F0_CALL_ADDRESS;
    e0_instruction = *(volatile u32 *)E0_CALL_ADDRESS;
    if (!is_arm_bl(f0_instruction) || !is_arm_bl(e0_instruction)) return false;

    code_cave = find_zero_code_cave(code_base, code_size, 32u);
    if (!code_cave) return false;
    if ((u32)code_cave == F0_CALL_ADDRESS || (u32)code_cave == E0_CALL_ADDRESS) return false;
    if (!encode_arm_branch(F0_CALL_ADDRESS, (u32)code_cave, &f0_branch) ||
        !encode_arm_branch(E0_CALL_ADDRESS, (u32)code_cave + 8u, &e0_branch)) return false;

    code_cave_patch = (u32 *)asl_physical_alias(code_cave);
    f0_patch = (u32 *)asl_physical_alias((void *)F0_CALL_ADDRESS);
    e0_patch = (u32 *)asl_physical_alias((void *)E0_CALL_ADDRESS);
    if (!code_cave_patch || !f0_patch || !e0_patch) return false;

    g_f0_original_target = decode_arm_branch_target(F0_CALL_ADDRESS, f0_instruction);
    g_e0_original_target = decode_arm_branch_target(E0_CALL_ADDRESS, e0_instruction);
    code_cave_patch[0] = 0xE51FF004u;
    code_cave_patch[1] = (u32)f0_bridge;
    code_cave_patch[2] = 0xE51FF004u;
    code_cave_patch[3] = (u32)e0_bridge;
    *f0_patch = f0_branch;
    *e0_patch = e0_branch;
    svcInvalidateEntireInstructionCache();
    asl_debug_log("[ASL1] F0/E0 gate hooks installed\n");
    return true;
}

bool asl_gen1_initialize(u8 *code_base, u32 code_size)
{
    memset(&g_gen1, 0, sizeof(g_gen1));
    g_gen1.enabled = true;
    g_gen1.last_f0_address = 0xFFFFFFFFu;
    g_gen1.stage = GEN1_WAITING;
    g_gen1.hooks_installed = install_gate_hooks(code_base, code_size);
    reset_run_state();
    return g_gen1.hooks_installed;
}

void asl_gen1_set_enabled(bool enabled)
{
    if (g_gen1.enabled == enabled) return;
    g_gen1.enabled = enabled;
    reset_run_state();
}

void asl_gen1_reset_run(void)
{
    reset_run_state();
}

void asl_gen1_handle_x(void)
{
    if (g_gen1.stage == GEN1_HOLDING) g_gen1.abort_requested = true;
    else reset_run_state();
}

void asl_gen1_on_frame(void)
{
    if (g_gen1.enabled) try_verify_generated_dvs();
}

void asl_gen1_get_snapshot(AslGateSnapshot *out)
{
    const char *name;
    const char *target;
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->enabled = g_gen1.enabled;
    out->generation = ASL_GENERATION_1;
    out->supported = g_gen1.layout != NULL;
    out->runtime_ready = g_gen1.hooks_installed;
    out->rom_title_seen = g_gen1.rom_title_seen;
    memcpy(out->rom_title, g_gen1.rom_title, sizeof(out->rom_title));

    name = g_gen1.layout ? g_gen1.layout->name : "?";
    strncpy(out->game_name, name, sizeof(out->game_name) - 1u);
    out->target_species = g_gen1.target_species;
    out->target_active = g_gen1.target_species != 0u;
    target = gen1_species_name(g_gen1.target_species);
    strncpy(out->target_name, target, sizeof(out->target_name) - 1u);

    out->extra_vblanks = g_gen1.extra_vblanks;
    out->prediction_valid = g_gen1.prediction.valid;
    out->predicted_dv1 = g_gen1.prediction.dv1;
    out->predicted_dv2 = g_gen1.prediction.dv2;
    out->verification_done = g_gen1.verification_done;
    out->verification_match = g_gen1.verification_match;
    out->actual_dv1 = g_gen1.actual_dv1;
    out->actual_dv2 = g_gen1.actual_dv2;

    switch (g_gen1.stage)
    {
        case GEN1_READY: out->stage = ASL_GATE_READY; break;
        case GEN1_HOLDING: out->stage = ASL_GATE_HOLDING; break;
        case GEN1_RELEASED: out->stage = ASL_GATE_RELEASED_SHINY; break;
        case GEN1_ABORTED: out->stage = ASL_GATE_ABORTED; break;
        case GEN1_ERROR: out->stage = ASL_GATE_ERROR; break;
        case GEN1_WAITING:
        default: out->stage = ASL_GATE_DETECTING; break;
    }
}

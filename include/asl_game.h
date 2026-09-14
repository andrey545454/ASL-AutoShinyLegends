#pragma once

#include <3ds.h>

typedef struct AslGameLayout
{
    const char *name;
    const char *rom_title_prefix;

    /* Guest PC of the immediate byte in the final LDH A,(hVBlankOccurred). */
    u16 delay_frame_poll_pc;

    /* Return address and ROM bank identify the exact PlayBattleMusic DelayFrame. */
    u16 return_after_delay_frame;
    u8 play_battle_music_bank;

    /* Game-specific WRAM addresses. */
    u16 w_cur_opponent;
    u16 w_enemy_mon_dvs;

    /* M-cycle distances from the gate origin to the two BattleRandom results. */
    u16 origin_to_dv1;
    u16 origin_to_dv2;
} AslGameLayout;

const AslGameLayout *asl_game_find_by_rom_title(const u8 title[16]);
bool asl_game_is_supported_legendary(u8 species);
const char *asl_game_species_name(u8 species);

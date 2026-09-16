#pragma once
#include <3ds.h>

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

const AslGen1GameLayout *asl_gen1_game_find_by_rom_title(const u8 title[16]);
bool asl_gen1_game_is_supported_legendary(u8 species);
const char *asl_gen1_game_species_name(u8 species);

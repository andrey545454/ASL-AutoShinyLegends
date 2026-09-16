#include "asl_gen2_predictor.h"

bool asl_gen2_is_shiny(u8 atk_def, u8 spd_spc)
{
    u8 atk;
    if (spd_spc != 0xAAu || (atk_def & 0x0Fu) != 0x0Au) return false;
    atk = (u8)(atk_def >> 4);
    return atk == 2u || atk == 3u || atk == 6u || atk == 7u ||
           atk == 10u || atk == 11u || atk == 14u || atk == 15u;
}

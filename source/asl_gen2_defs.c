#include "asl_gen2_defs.h"

bool asl_gen2_is_celebi_context(u8 species, u8 battle_mode, u8 battle_type)
{
    return species == ASLC_CELEBI_SPECIES &&
           battle_mode == ASLC_WILD_BATTLE &&
           battle_type == ASLC_BATTLETYPE_CELEBI;
}

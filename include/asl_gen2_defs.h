#pragma once
#include <3ds.h>

#define ASLC_ROM_TITLE_PREFIX       "PM_CRYSTAL"
#define ASLC_CELEBI_SPECIES         0xFBu
#define ASLC_WILD_BATTLE            0x01u
#define ASLC_BATTLETYPE_CELEBI      0x0Bu

#define ASLC_W_TEMP_ENEMY_SPECIES   0xD204u
#define ASLC_W_ENEMY_MON_DVS        0xD20Cu
#define ASLC_W_BATTLE_MODE          0xD22Du
#define ASLC_W_BATTLE_TYPE          0xD230u
#define ASLC_W_PRED_TELEMETRY       0xD2A9u

bool asl_gen2_is_celebi_context(u8 species, u8 battle_mode, u8 battle_type);

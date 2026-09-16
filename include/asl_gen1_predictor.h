#pragma once
#include <3ds.h>

typedef struct AslGen1Prediction
{
    bool valid;
    u8 dv1; /* Speed / Special */
    u8 dv2; /* Attack / Defense */
    bool shiny;
} AslGen1Prediction;

bool asl_gen1_predict(
    u8 random_add,
    u8 div,
    s32 divider_countdown,
    s32 current_mcycles,
    u16 origin_to_dv1,
    u16 origin_to_dv2,
    AslGen1Prediction *out);

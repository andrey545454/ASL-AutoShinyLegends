#include "asl_predictor.h"

#include <string.h>

/*
 * The VC emulator represents the Game Boy divider as an 8-bit DIV value plus
 * a 1..64 countdown to the next increment. Reconstructing both pieces gives a
 * 14-bit divider phase with single M-cycle resolution.
 */
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

static bool is_shiny_dv_pair(u8 dv1, u8 dv2)
{
    /*
     * Gen II shiny-compatible Gen I DVs:
     *   Speed   = 10
     *   Special = 10
     *   Defense = 10
     *   Attack  = 2, 3, 6, 7, 10, 11, 14 or 15
     *
     * dv1 stores Speed/Special and dv2 stores Attack/Defense.
     */
    if (dv1 != 0xAAu || (dv2 & 0x0Fu) != 0x0Au)
        return false;

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

bool asl_predictor_predict(
    u8 random_add,
    u8 div,
    s32 divider_countdown,
    s32 current_mcycles,
    u16 origin_to_dv1,
    u16 origin_to_dv2,
    AslPrediction *out)
{
    u16 effective_divider;
    u8 div1;
    u8 div2;

    if (!out)
        return false;

    memset(out, 0, sizeof(*out));

    if (divider_countdown < 1 || divider_countdown > 64)
        return false;

    effective_divider = effective_full_divider(
        div, divider_countdown, current_mcycles);

    div1 = div_at_delta(effective_divider, origin_to_dv1);
    div2 = div_at_delta(effective_divider, origin_to_dv2);

    /*
     * On the validated non-link legendary path, BattleRandom advances
     * hRandomAdd as:
     *
     *     next = previous + rDIV + 1
     *
     * The new hRandomAdd byte is the RNG result consumed as a DV byte.
     */
    out->dv1 = (u8)((u16)random_add + (u16)div1 + 1u);
    out->dv2 = (u8)((u16)out->dv1 + (u16)div2 + 1u);
    out->shiny = is_shiny_dv_pair(out->dv1, out->dv2);
    out->valid = true;
    return true;
}

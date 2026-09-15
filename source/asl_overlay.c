#include "asl_overlay.h"

#include <stdio.h>
#define SCREEN_WIDTH  400
#define SCREEN_HEIGHT 240
#define PANEL_X       5
#define PANEL_Y       5
#define LINE_HEIGHT   9

typedef struct Color
{
    u8 r;
    u8 g;
    u8 b;
} Color;
static const Color k_white      = {255, 255, 255};
static const Color k_muted      = {255, 190, 80};
static const Color k_info       = {200, 220, 255};
static const Color k_ready      = {180, 255, 180};
static const Color k_warning    = {255, 230, 120};
static const Color k_error      = {255, 120, 120};
static const Color k_warm       = {255, 180, 120};
static const Color k_success    = {120, 255, 150};
static u16 pack_rgb565(Color color)
{
    return (u16)(((u16)(color.r >> 3) << 11) |
                 ((u16)(color.g >> 2) << 5) |
                 (u16)(color.b >> 3));
}

static u16 pack_rgb5a1(Color color)
{
    return (u16)(((u16)(color.r >> 3) << 11) |
                 ((u16)(color.g >> 3) << 6) |
                 ((u16)(color.b >> 3) << 1) |
                 1u);
}
static u16 pack_rgba4(Color color)
{
    return (u16)(((u16)(color.r >> 4) << 12) |
                 ((u16)(color.g >> 4) << 8) |
                 ((u16)(color.b >> 4) << 4) |
                 0xFu);
}

static void put_pixel(
    u8 *framebuffer,
    u32 stride,
    u32 format,
    int x,
    int y,
    Color color)
{
    if (!framebuffer ||
        x < 0 || x >= SCREEN_WIDTH ||
        y < 0 || y >= SCREEN_HEIGHT)
    {
        return;
    }

    format &= 0x0Fu;
    if (format == GSP_BGR8_OES)
    {
        u8 *pixel = framebuffer +
            stride * (u32)x +
            (u32)(SCREEN_HEIGHT - 1 - y) * 3u;
        pixel[0] = color.b;
        pixel[1] = color.g;
        pixel[2] = color.r;
    }
    else if (format == GSP_RGB565_OES)
    {
        u16 *pixel = (u16 *)(framebuffer +
            stride * (u32)x +
            (u32)(SCREEN_HEIGHT - 1 - y) * 2u);
        *pixel = pack_rgb565(color);
    }
    else if (format == GSP_RGB5_A1_OES)
    {
        u16 *pixel = (u16 *)(framebuffer +
            stride * (u32)x +
            (u32)(SCREEN_HEIGHT - 1 - y) * 2u);
        *pixel = pack_rgb5a1(color);
    }
    else if (format == GSP_RGBA4_OES)
    {
        u16 *pixel = (u16 *)(framebuffer +
            stride * (u32)x +
            (u32)(SCREEN_HEIGHT - 1 - y) * 2u);
        *pixel = pack_rgba4(color);
    }
    else if (format == GSP_RGBA8_OES)
    {
        u8 *pixel = framebuffer +
            stride * (u32)x +
            (u32)(SCREEN_HEIGHT - 1 - y) * 4u;
        pixel[0] = 0xFF;
        pixel[1] = color.b;
        pixel[2] = color.g;
        pixel[3] = color.r;
    }
}
/* Compact 5x7 font. Each row uses bits 4..0. */
static const u8 k_digits[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E},
    {0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E},
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    {0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E}
};
static const u8 k_letters[26][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    {0x01,0x01,0x01,0x01,0x11,0x11,0x0E},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}
};
static u8 glyph_row(char character, int row)
{
    if (row < 0 || row >= 7)
        return 0;
    if (character >= '0' && character <= '9')
        return k_digits[(int)(character - '0')][row];
    if (character >= 'A' && character <= 'Z')
        return k_letters[(int)(character - 'A')][row];
    switch (character)
    {
        case ':':
            return row == 2 || row == 5 ? 0x04u : 0u;
        case '.':
            return row == 6 ? 0x04u : 0u;
        case '-':
            return row == 3 ? 0x0Eu : 0u;
        case '/':
            return (row == 0 || row == 1) ? 0x01u :
                   (row == 2 || row == 3) ? 0x02u :
                   row == 4 ? 0x04u :
                   row == 5 ? 0x08u : 0x10u;
        case '?':
        {
            static const u8 question[7] = {
                0x0E,0x11,0x01,0x02,0x04,0x00,0x04
            };
            return question[row];
        }
        default:
            return 0;
    }
}
static void draw_character(
    u8 *framebuffer,
    u32 stride,
    u32 format,
    int x,
    int y,
    char character,
    Color color)
{
    int row;
    int column;
    for (row = 0; row < 7; ++row)
    {
        const u8 bits = glyph_row(character, row);
        for (column = 0; column < 5; ++column)
        {
            if ((bits & (1u << (4 - column))) != 0u)
            {
                put_pixel(
                    framebuffer,
                    stride,
                    format,
                    x + column,
                    y + row,
                    color);
            }
        }
    }
}
static void draw_text(
    u8 *framebuffer,
    u32 stride,
    u32 format,
    int x,
    int y,
    const char *text,
    Color color)
{
    while (*text != '\0')
    {
        draw_character(framebuffer, stride, format, x, y, *text, color);
        x += 6;
        ++text;
    }
}
static void draw_line(
    u8 *framebuffer,
    u32 stride,
    u32 format,
    int *y,
    const char *text,
    Color color)
{
    draw_text(framebuffer, stride, format, PANEL_X + 6, *y, text, color);
    *y += LINE_HEIGHT;
}

void asl_overlay_render(
    u8 *framebuffer,
    u32 stride,
    u32 format,
    const AslGateSnapshot *snapshot)
{
    char line[64];
    int y = PANEL_Y + 6;

    if (!framebuffer || !snapshot)
        return;

    draw_line(
        framebuffer, stride, format, &y,
        "ASL - AUTO SHINY LEGENDS",
        k_white);
    if (!snapshot->enabled)
    {
        draw_line(framebuffer, stride, format, &y, "PLUGIN: OFF", k_warm);
        draw_line(framebuffer, stride, format, &y, "SELECT: ENABLE", k_muted);
        return;
    }
    if (!snapshot->hooks_installed)
    {
        draw_line(framebuffer, stride, format, &y, "HOOK ERROR", k_error);
        draw_line(framebuffer, stride, format, &y, "HOST VC BUILD MISMATCH", k_warm);
        draw_line(framebuffer, stride, format, &y, "SELECT: DISABLE", k_muted);
        return;
    }
    if (!snapshot->supported)
    {
        if (!snapshot->rom_title_seen)
        {
            draw_line(framebuffer, stride, format, &y, "WAITING FOR VC ROM INIT", k_warning);
            draw_line(framebuffer, stride, format, &y, "GUEST HEADER NOT READY", k_muted);
        }
        else
        {
            draw_line(framebuffer, stride, format, &y, "ROM NOT RECOGNIZED", k_error);
            snprintf(line, sizeof(line), "TITLE: %.16s", snapshot->rom_title);
            draw_line(framebuffer, stride, format, &y, line, k_warm);
            draw_line(framebuffer, stride, format, &y, "EXPECTED RED/BLUE/YELLOW", k_muted);
        }
        draw_line(framebuffer, stride, format, &y, "SELECT: DISABLE", k_muted);
        return;
    }

    snprintf(line, sizeof(line), "GAME: %s", snapshot->game_name);
    draw_line(framebuffer, stride, format, &y, line, k_info);
    if (snapshot->target_species != 0u &&
        snapshot->stage != ASL_GATE_READY &&
        snapshot->stage != ASL_GATE_ERROR)
    {
        snprintf(line, sizeof(line), "TARGET: %s", snapshot->target_name);
        draw_line(framebuffer, stride, format, &y, line, k_info);
    }
    switch (snapshot->stage)
    {
        case ASL_GATE_READY:
            draw_line(framebuffer, stride, format, &y, "READY - APPROACH LEGENDARY", k_ready);
            draw_line(framebuffer, stride, format, &y, "X: RESET", k_muted);
            break;
        case ASL_GATE_HOLDING:
            draw_line(framebuffer, stride, format, &y, "SEARCHING...", k_warning);
            snprintf(
                line,
                sizeof(line),
                "EXTRA VBLANK: %lu",
                (unsigned long)snapshot->extra_vblanks);
            draw_line(framebuffer, stride, format, &y, line, k_white);
            if (snapshot->prediction_valid)
            {
                snprintf(
                    line,
                    sizeof(line),
                    "CANDIDATE: %02X %02X",
                    (unsigned)snapshot->predicted_dv1,
                    (unsigned)snapshot->predicted_dv2);
                draw_line(framebuffer, stride, format, &y, line, k_info);
            }

            draw_line(framebuffer, stride, format, &y, "X: ABORT AND RELEASE", k_error);
            break;
        case ASL_GATE_RELEASED_SHINY:
            draw_line(framebuffer, stride, format, &y, "SHINY FOUND - RELEASED", k_success);
            snprintf(
                line,
                sizeof(line),
                "WAITED VBLANK: %lu",
                (unsigned long)snapshot->extra_vblanks);
            draw_line(framebuffer, stride, format, &y, line, k_white);
            snprintf(
                line,
                sizeof(line),
                "PRED: %02X %02X",
                (unsigned)snapshot->predicted_dv1,
                (unsigned)snapshot->predicted_dv2);
            draw_line(framebuffer, stride, format, &y, line, k_info);
            if (!snapshot->verification_done)
            {
                draw_line(framebuffer, stride, format, &y, "REAL: WAITING...", k_muted);
            }
            else
            {
                snprintf(
                    line,
                    sizeof(line),
                    "REAL: %02X %02X",
                    (unsigned)snapshot->actual_dv1,
                    (unsigned)snapshot->actual_dv2);
                draw_line(framebuffer, stride, format, &y, line, k_muted);
                draw_line(
                    framebuffer,
                    stride,
                    format,
                    &y,
                    snapshot->verification_match ? "VERIFIED" : "MISMATCH",
                    snapshot->verification_match ? k_success : k_error);
            }
            draw_line(framebuffer, stride, format, &y, "X: CLEAR FOR NEXT RUN", k_muted);
            break;

        case ASL_GATE_ABORTED:
            draw_line(framebuffer, stride, format, &y, "ABORTED - RELEASED", k_warm);
            draw_line(framebuffer, stride, format, &y, "X: CLEAR", k_muted);
            break;
        case ASL_GATE_ERROR:
        default:
            draw_line(framebuffer, stride, format, &y, "ERROR", k_error);
            draw_line(framebuffer, stride, format, &y, "X: RESET", k_muted);
            break;
    }

    draw_line(framebuffer, stride, format, &y, "SELECT: DISABLE", k_muted);
}

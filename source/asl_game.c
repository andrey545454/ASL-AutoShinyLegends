#include "asl_game.h"

#include <string.h>

/*
 * Game-specific values for the tested Gen I Virtual Console emulator build.
 * Red and Blue share the relevant guest layout and timing constants.
 */
static const AslGameLayout k_yellow = {
    "YELLOW", "POKEMON YELLOW",
    0x1E6Au, 0x5071u, 0x02u,
    0xD058u, 0xCFF0u,
    0x0CF8u, 0x0D6Fu
};

static const AslGameLayout k_red = {
    "RED", "POKEMON RED",
    0x20B5u, 0x50D7u, 0x02u,
    0xD059u, 0xCFF1u,
    0x0C68u, 0x0CE0u
};

static const AslGameLayout k_blue = {
    "BLUE", "POKEMON BLUE",
    0x20B5u, 0x50D7u, 0x02u,
    0xD059u, 0xCFF1u,
    0x0C68u, 0x0CE0u
};

enum
{
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

const AslGameLayout *asl_game_find_by_rom_title(const u8 title[16])
{
    if (title_starts_with(title, k_yellow.rom_title_prefix))
        return &k_yellow;
    if (title_starts_with(title, k_red.rom_title_prefix))
        return &k_red;
    if (title_starts_with(title, k_blue.rom_title_prefix))
        return &k_blue;
    return NULL;
}

bool asl_game_is_supported_legendary(u8 species)
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

const char *asl_game_species_name(u8 species)
{
    switch (species)
    {
        case SPECIES_MOLTRES:  return "MOLTRES";
        case SPECIES_ARTICUNO: return "ARTICUNO";
        case SPECIES_ZAPDOS:   return "ZAPDOS";
        case SPECIES_MEWTWO:   return "MEWTWO";
        default:               return "?";
    }
}

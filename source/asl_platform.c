#include "asl_platform.h"
#include "csvc.h"

#include <stddef.h>

static u32 string_length(const char *text)
{
    u32 length = 0;
    while (text[length] != '\0')
        ++length;
    return length;
}

void asl_debug_log(const char *text)
{
    (void)svcOutputDebugString(text, (s32)string_length(text));
}

void *asl_find_bytes(
    const void *haystack,
    u32 haystack_size,
    const void *needle,
    u32 needle_size)
{
    const u8 *bytes = (const u8 *)haystack;
    const u8 *pattern = (const u8 *)needle;
    u32 offset;

    if (!haystack || !needle || needle_size == 0u || haystack_size < needle_size)
        return NULL;

    for (offset = 0; offset <= haystack_size - needle_size; ++offset)
    {
        u32 i = 0;
        while (i < needle_size && bytes[offset + i] == pattern[i])
            ++i;

        if (i == needle_size)
            return (void *)(bytes + offset);
    }

    return NULL;
}

void *asl_physical_alias(void *virtual_address)
{
    const u32 physical = svcConvertVAToPA(virtual_address, false);
    if (physical == 0u)
        return NULL;

    /* Kernel physical aliases are writable even when the title mapping is RX. */
    return (void *)(physical | 0x80000000u);
}

#include "asl_platform.h"
#include "csvc.h"
#include <stddef.h>
#include <string.h>

static u32 string_length(const char *text)
{
    u32 length = 0;
    while (text[length] != '\0') ++length;
    return length;
}

void asl_debug_log(const char *text)
{
    (void)svcOutputDebugString(text, (s32)string_length(text));
}

void *asl_find_bytes(const void *haystack, u32 haystack_size, const void *needle, u32 needle_size)
{
    const u8 *bytes = (const u8 *)haystack;
    const u8 *pattern = (const u8 *)needle;
    u32 offset;
    if (!haystack || !needle || needle_size == 0u || haystack_size < needle_size) return NULL;
    for (offset = 0; offset <= haystack_size - needle_size; ++offset)
    {
        u32 i = 0;
        while (i < needle_size && bytes[offset + i] == pattern[i]) ++i;
        if (i == needle_size) return (void *)(bytes + offset);
    }
    return NULL;
}

void *asl_physical_alias(void *virtual_address)
{
    const u32 physical = svcConvertVAToPA(virtual_address, false);
    if (physical == 0u) return NULL;
    return (void *)(physical | 0x80000000u);
}

u32 asl_add_sat(u32 a, u32 b)
{
    const u32 result = a + b;
    return result < a ? 0xFFFFFFFFu : result;
}

bool asl_query_mapping(u32 address, MemInfo *info)
{
    PageInfo page;
    if (!info) return false;
    return R_SUCCEEDED(svcQueryMemory(info, &page, address));
}

u32 asl_mapping_end(const MemInfo *info)
{
    if (!info || info->size == 0u) return 0u;
    return asl_add_sat(info->base_addr, info->size);
}

static bool address_has_permissions(u32 address, u32 size, u32 permissions)
{
    MemInfo info;
    u32 end;
    if (size == 0u) return false;
    end = address + size - 1u;
    if (end < address || !asl_query_mapping(address, &info)) return false;
    if ((info.perm & permissions) != permissions) return false;
    return address >= info.base_addr && end < asl_mapping_end(&info);
}

bool asl_address_is_readable(u32 address, u32 size)
{
    return address_has_permissions(address, size, MEMPERM_READ);
}

bool asl_address_is_writable(u32 address, u32 size)
{
    return address_has_permissions(address, size, MEMPERM_WRITE);
}

bool asl_bytes_equal(u32 address, const void *bytes, u32 size)
{
    if (!bytes || size == 0u || !asl_address_is_readable(address, size)) return false;
    return memcmp((const void *)address, bytes, size) == 0;
}

bool asl_bytes_zero(u32 address, u32 size)
{
    u32 i;
    if (size == 0u || !asl_address_is_readable(address, size)) return false;
    for (i = 0; i < size; ++i)
        if (*((volatile const u8 *)(address + i)) != 0u) return false;
    return true;
}

void asl_memory_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

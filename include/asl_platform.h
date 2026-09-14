#pragma once

#include <3ds.h>

void asl_debug_log(const char *text);
void *asl_find_bytes(
    const void *haystack,
    u32 haystack_size,
    const void *needle,
    u32 needle_size);
void *asl_physical_alias(void *virtual_address);

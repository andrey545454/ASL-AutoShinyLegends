#pragma once
#include <3ds.h>

void asl_debug_log(const char *text);
void *asl_find_bytes(const void *haystack, u32 haystack_size, const void *needle, u32 needle_size);
void *asl_physical_alias(void *virtual_address);

u32 asl_add_sat(u32 a, u32 b);
bool asl_query_mapping(u32 address, MemInfo *info);
u32 asl_mapping_end(const MemInfo *info);
bool asl_address_is_readable(u32 address, u32 size);
bool asl_address_is_writable(u32 address, u32 size);
bool asl_bytes_equal(u32 address, const void *bytes, u32 size);
bool asl_bytes_zero(u32 address, u32 size);
void asl_memory_barrier(void);

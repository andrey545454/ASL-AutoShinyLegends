#pragma once

#include <3ds/types.h>

u32 svcConvertVAToPA(const void *va, bool write_check);
void svcInvalidateEntireInstructionCache(void);

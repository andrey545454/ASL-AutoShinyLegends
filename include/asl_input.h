#pragma once

#include <3ds.h>

void asl_input_set_shared_keys(vu32 *address);
void asl_input_scan(void);
u32 asl_input_down(void);
u32 asl_input_held(void);

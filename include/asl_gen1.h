#pragma once
#include <3ds.h>
#include "asl_gate.h"

bool asl_gen1_initialize(u8 *code_base, u32 code_size);
void asl_gen1_set_enabled(bool enabled);
void asl_gen1_reset_run(void);
void asl_gen1_handle_x(void);
void asl_gen1_on_frame(void);
void asl_gen1_get_snapshot(AslGateSnapshot *out);

#pragma once
#include <3ds.h>
#include "asl_gate.h"

void asl_gen2_initialize(void);
void asl_gen2_set_enabled(bool enabled);
void asl_gen2_reset_run(void);
void asl_gen2_handle_x(void);
void asl_gen2_on_frame(void);
bool asl_gen2_detected(void);
void asl_gen2_get_snapshot(AslGateSnapshot *out);

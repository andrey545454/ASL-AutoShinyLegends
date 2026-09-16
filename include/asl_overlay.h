#pragma once
#include <3ds.h>
#include "asl_gate.h"
void asl_overlay_render(u8 *framebuffer, u32 stride, u32 format, const AslGateSnapshot *snapshot);

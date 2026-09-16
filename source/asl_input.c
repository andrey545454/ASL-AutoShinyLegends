#include "asl_input.h"
static vu32 *g_shared_keys;
static u32 g_current;
static u32 g_previous;
static u32 g_down;
void asl_input_set_shared_keys(vu32 *address)
{
    g_shared_keys = address;
    g_current = g_previous = g_down = 0;
}
void asl_input_scan(void)
{
    if (!g_shared_keys) { g_down = 0; return; }
    g_previous = g_current;
    g_current = *g_shared_keys;
    g_down = g_current & ~g_previous;
}
u32 asl_input_down(void) { return g_down; }
u32 asl_input_held(void) { return g_current; }

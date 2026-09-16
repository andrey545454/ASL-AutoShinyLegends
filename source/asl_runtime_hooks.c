#include "asl_runtime_hooks.h"
#include "asl_gate.h"
#include "asl_overlay.h"
#include "asl_platform.h"
#include "csvc.h"

static vu32 *g_shared_keys;
static u32 g_input_current;
static u32 g_input_previous;
static u32 g_input_down;

static void input_set_shared_keys(vu32 *address)
{
    g_shared_keys = address;
    g_input_current = 0u;
    g_input_previous = 0u;
    g_input_down = 0u;
}

static void input_scan(void)
{
    if (!g_shared_keys)
    {
        g_input_down = 0u;
        return;
    }
    g_input_previous = g_input_current;
    g_input_current = *g_shared_keys;
    g_input_down = g_input_current & ~g_input_previous;
}

static const u32 k_present_pattern[] = {
    0xE28D0028u, 0xE3A08000u, 0xE1A07001u, 0xE8900E00u,
};
static const u32 k_input_pattern[] = {
    0x13A02001u, 0x03A02003u, 0xE3A03201u, 0xEF00001Fu,
    0xE1B01FA0u, 0x03A01001u, 0x05C41018u,
};
static const u32 k_present_patch_template[30] = {
    0xE92D5FF0u, 0xE92D000Fu, 0xE59FC064u, 0xE12FFF3Cu,
    0xE8BD00F0u, 0xE28D0028u, 0xE8900E00u, 0xEB000000u,
    0xE280105Cu, 0xE7912104u, 0xE3A03004u, 0xE5D20000u,
    0xE2600001u, 0xE20000FFu, 0xE060E180u, 0xE083310Eu,
    0xE0823003u, 0xE8830EE0u, 0xEE078F9Au, 0xE7912104u,
    0xE1923F9Fu, 0xE3C330FFu, 0xE1833000u, 0xE3C33CFFu,
    0xE3833C01u, 0xE1826F93u, 0xE3560000u, 0x1AFFFFF6u,
    0xE8BD9FF0u, 0x00000000u,
};

static void on_present(u32 a0,u32 a1,u32 a2,u32 a3,u32 screen_id,u32 swap,
                       u8 *framebuffer_a,u8 *framebuffer_b,u32 stride,u32 format)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)swap;
    if (screen_id == GSP_SCREEN_TOP && framebuffer_a)
    {
        AslGateSnapshot snapshot;
        input_scan();
        asl_gate_on_frame(g_input_down, g_input_current);
        asl_gate_get_snapshot(&snapshot);
        asl_overlay_render(framebuffer_a, stride, format, &snapshot);
    }
    if (framebuffer_a)
    {
        const u32 width = screen_id == GSP_SCREEN_TOP ? 400u : 320u;
        (void)svcFlushProcessDataCache(CUR_PROCESS_HANDLE,(u32)framebuffer_a,stride*width);
    }
    if (screen_id == GSP_SCREEN_TOP && framebuffer_b && framebuffer_b != framebuffer_a)
        (void)svcFlushProcessDataCache(CUR_PROCESS_HANDLE,(u32)framebuffer_b,stride*400u);
}

static Result on_input_map(u32 memblock_handle,u32 address,u32 r2,u32 r3,u32 r4,u32 r5)
{
    const bool game_requested_write = r5 == 0u;
    const MemPerm permissions = game_requested_write ? (MemPerm)(MEMPERM_READ|MEMPERM_WRITE) : MEMPERM_READ;
    (void)r2; (void)r3; (void)r4;
    if (!game_requested_write) input_set_shared_keys((vu32 *)(address + 0x28u));
    return svcMapMemoryBlock(memblock_handle,address,permissions,MEMPERM_DONTCARE);
}

static bool install_render_hook(u8 *code_base,u32 code_size)
{
    u8 *match=(u8 *)asl_find_bytes(code_base,code_size,k_present_pattern,sizeof(k_present_pattern));
    u8 *patch_address; u32 original_bl; u32 *patch; u32 i;
    if (!match || match < code_base + 8) { asl_debug_log("[ASLC] framebuffer signature not found\n"); return false; }
    patch_address=match-8; original_bl=*(u32 *)(patch_address+0x20u);
    if ((original_bl & 0x0F000000u) != 0x0B000000u) { asl_debug_log("[ASLC] framebuffer sanity failed\n"); return false; }
    patch=(u32 *)asl_physical_alias(patch_address); if (!patch) return false;
    for (i=0;i<30u;++i) patch[i]=k_present_patch_template[i];
    patch[7]=original_bl+1u; patch[29]=(u32)on_present;
    asl_debug_log("[ASLC] framebuffer hook installed\n"); return true;
}

static bool install_input_hook(u8 *code_base,u32 code_size)
{
    u8 *hook_address=(u8 *)asl_find_bytes(code_base,code_size,k_input_pattern,sizeof(k_input_pattern));
    u32 *patch;
    if (!hook_address) { asl_debug_log("[ASLC] HID signature not found\n"); return false; }
    patch=(u32 *)asl_physical_alias(hook_address); if (!patch) return false;
    patch[0]=0xE59FE000u; patch[1]=0xE59FF000u; patch[2]=(u32)hook_address+16u; patch[3]=(u32)on_input_map;
    asl_debug_log("[ASLC] HID hook installed\n"); return true;
}

void asl_runtime_hooks_install(u8 *code_base,u32 code_size,AslRuntimeHookStatus *status)
{
    AslRuntimeHookStatus local={false,false};
    local.render_installed=install_render_hook(code_base,code_size);
    local.input_installed=install_input_hook(code_base,code_size);
    if (local.render_installed || local.input_installed) svcInvalidateEntireInstructionCache();
    if (status) *status=local;
}

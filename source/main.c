#include <3ds.h>
#include "asl_gate.h"
#include "asl_platform.h"
#include "asl_runtime_hooks.h"
#include "csvc.h"

/*
 * Raw 3GX entrypoint. Runtime framebuffer/HID hooks are common to both
 * backends. The gate router then selects Gen I host hooks or the Crystal
 * Gen II guest-ROM backend without requiring separate plugin binaries.
 */
int main(void)
{
    MemInfo code_mapping;
    PageInfo page_info;
    AslRuntimeHookStatus runtime_hooks;
    Result result;
    bool host_code_patched;

    asl_debug_log("[ASL] v2.0.0 raw 3GX entrypoint\n");
    result = svcQueryMemory(&code_mapping, &page_info, 0x00100000u);
    if (R_FAILED(result) || code_mapping.base_addr == 0u || code_mapping.size == 0u)
    {
        asl_debug_log("[ASL] failed to locate the title code mapping\n");
        return 0;
    }

    /* Preserve the original ordering: runtime hooks consume their branch
       islands before the Gen I backend scans for its own. */
    asl_runtime_hooks_install(
        (u8 *)code_mapping.base_addr,
        code_mapping.size,
        &runtime_hooks);

    host_code_patched = asl_gate_initialize(
        (u8 *)code_mapping.base_addr,
        code_mapping.size);
    if (host_code_patched)
        svcInvalidateEntireInstructionCache();

    if (!runtime_hooks.input_installed)
        asl_debug_log("[ASL] HID hook unavailable; button controls are disabled\n");
    if (!runtime_hooks.render_installed)
        asl_debug_log("[ASL] framebuffer hook unavailable; overlay and frame input are disabled\n");

    return 0;
}

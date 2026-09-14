.arm
.align 4
.section .text
.global _start
.type _start, %function

/*
 * Luma/Azahar call the plugin entry at 0x07000100.
 * Preserve the host thread state, clear our BSS, call main(), then return.
 */
_start:
    stmfd   sp!, {r0-r12, lr}
    mrs     r0, cpsr
    stmfd   sp!, {r0}

    ldr     r0, =__c_bss_start
    ldr     r1, =__c_bss_end
    sub     r1, r1, r0
    bl      clear_bss

    bl      main

    ldmfd   sp!, {r0}
    msr     cpsr, r0
    ldmfd   sp!, {r0-r12, pc}

clear_bss:
    add     r1, r1, #3
    bic     r1, r1, #3
    cmp     r1, #0
    bxeq    lr
    mov     r2, #0
1:
    stmia   r0!, {r2}
    subs    r1, r1, #4
    bne     1b
    bx      lr

.section .__bss_start,"aw",%nobits
.global __c_bss_start
__c_bss_start:

.section .__bss_end,"aw",%nobits
.global __c_bss_end
__c_bss_end:

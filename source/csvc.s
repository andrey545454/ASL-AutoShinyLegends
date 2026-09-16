.arm
.balign 4
.section .text.svcConvertVAToPA,"ax",%progbits
.global svcConvertVAToPA
.type svcConvertVAToPA,%function
svcConvertVAToPA:
    svc 0x90
    bx  lr
.section .text.svcInvalidateEntireInstructionCache,"ax",%progbits
.global svcInvalidateEntireInstructionCache
.type svcInvalidateEntireInstructionCache,%function
svcInvalidateEntireInstructionCache:
    svc 0x94
    bx  lr

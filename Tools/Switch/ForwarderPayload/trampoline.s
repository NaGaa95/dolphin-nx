.section .text.nroEntrypointTrampoline, "ax", %progbits
.align 2
.global nroEntrypointTrampoline
.type   nroEntrypointTrampoline, %function
.cfi_startproc
nroEntrypointTrampoline:

    adrp x8, __stack_top
    ldr  x8, [x8, #:lo12:__stack_top]
    mov  sp, x8

    blr  x2

    adrp x1, g_lastRet
    str  w0, [x1, #:lo12:g_lastRet]

    adrp x8, __stack_top
    ldr  x8, [x8, #:lo12:__stack_top]
    mov  sp, x8

    b    loadNro

.cfi_endproc

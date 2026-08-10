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

// User exceptions initially return to the forwarder's NSO entrypoint. Forward them to the
// currently loaded NRO so its libnx exception handler can recover fastmem faults and resume.
.section .text.__libnx_exception_entry, "ax", %progbits
.align 2
.global __libnx_exception_entry
.type   __libnx_exception_entry, %function
.cfi_startproc
__libnx_exception_entry:
    adrp x7, g_nroAddr
    ldr  x7, [x7, #:lo12:g_nroAddr]
    cbz  x7, .Lexception_unhandled
    br   x7

.Lexception_unhandled:
    mov  w0, #0xf801
    svc  0x28

.cfi_endproc

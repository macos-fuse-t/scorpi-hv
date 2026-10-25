/* SPDX-License-Identifier: BSD-2-Clause */
/* Microsoft ARM64 legacy fast register ABI. Only volatile registers are used;
 * x18 (the Windows platform register) and x19-x30 are preserved.
 */
.text
.p2align 2
.globl ScorpiSetVpRegister
ScorpiSetVpRegister:
    mov w3, w0
    mov x5, x1
    mov x0, #0x51
    movk x0, #1, lsl #16
    movk x0, #1, lsl #32
    mov x1, #-1
    movn w2, #1
    mov x4, xzr
    mov x6, xzr
    hvc #1
    and w0, w0, #0xffff
    ret

.p2align 2
.globl ScorpiGetVpRegister
ScorpiGetVpRegister:
    mov w3, w0
    mov x0, #0x50
    movk x0, #1, lsl #16
    movk x0, #1, lsl #32
    mov x1, #-1
    movn w2, #1
    hvc #1
    tst x0, #0xffff
    csel x0, x15, xzr, eq
    ret

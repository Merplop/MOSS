/*
 * setjmp / longjmp for i386 — MOSS user libc.
 *
 * jmp_buf layout (6 x uint32_t):
 *   [0] = EBX
 *   [1] = ESI
 *   [2] = EDI
 *   [3] = EBP
 *   [4] = ESP  (caller's stack pointer, after return address)
 *   [5] = EIP  (return address)
 */
.section .text

/* int setjmp(jmp_buf env) — returns 0 on direct call */
.global setjmp
.type setjmp, @function
setjmp:
    movl 4(%esp), %eax        /* eax = pointer to jmp_buf */
    movl %ebx,  0(%eax)       /* save EBX */
    movl %esi,  4(%eax)       /* save ESI */
    movl %edi,  8(%eax)       /* save EDI */
    movl %ebp, 12(%eax)       /* save EBP */
    leal 4(%esp), %ecx        /* caller's ESP (skip return address) */
    movl %ecx, 16(%eax)       /* save ESP */
    movl (%esp), %ecx         /* return address */
    movl %ecx, 20(%eax)       /* save EIP */
    xorl %eax, %eax           /* return 0 */
    ret
.size setjmp, . - setjmp

/* void longjmp(jmp_buf env, int val) — never returns */
.global longjmp
.type longjmp, @function
longjmp:
    movl 4(%esp), %edx        /* edx = pointer to jmp_buf */
    movl 8(%esp), %eax        /* eax = val */
    testl %eax, %eax
    jnz 1f
    incl %eax                 /* if val==0, return 1 instead */
1:
    movl  0(%edx), %ebx       /* restore EBX */
    movl  4(%edx), %esi       /* restore ESI */
    movl  8(%edx), %edi       /* restore EDI */
    movl 12(%edx), %ebp       /* restore EBP */
    movl 16(%edx), %esp       /* restore ESP */
    jmp *20(%edx)              /* jump to saved EIP */
.size longjmp, . - longjmp

/*
 * crt0.S — C runtime startup for MOSS user-space programs.
 *
 * The ELF loader jumps here (ring 3).  We call main() and then
 * invoke SYS_EXIT with main's return value.
 *
 * Stack layout at entry (Linux-style ABI):
 *   (%esp)   = argc
 *   4(%esp)  = argv[0]
 *              ...
 *              NULL
 *              envp[0]
 *              ...
 *              NULL
 */
.section .text
.global _start
.global environ

_start:
    /* Clear the frame pointer for clean backtraces. */
    xorl %ebp, %ebp

    /* Read argc from (%esp) */
    movl (%esp), %ecx        /* ecx = argc */

    /* argv = esp + 4 */
    leal 4(%esp), %esi       /* esi = argv */

    /* envp = argv + (argc + 1) * 4  (skip argv[] + NULL terminator) */
    movl %ecx, %eax
    addl $1, %eax
    shll $2, %eax
    leal (%esi, %eax), %edx  /* edx = envp */

    /* Store envp into the global 'environ' variable */
    movl %edx, environ

    /* Call main(argc, argv, envp) — push right to left */
    pushl %edx               /* envp */
    pushl %esi               /* argv */
    pushl %ecx               /* argc */
    call  main
    addl  $12, %esp

    /* main() returned in %eax — use it as the exit code */
    movl %eax, %ebx   /* ebx = exit code */
    movl $0,   %eax   /* eax = SYS_EXIT  */
    int  $0x80

    /* Should never reach here */
1:  hlt
    jmp 1b

.section .bss
.align 4
environ:
    .long 0

/*
 * crt0.S — C runtime startup for MOSS user-space programs.
 *
 * The ELF loader jumps here (ring 3).  We call main() and then
 * invoke SYS_EXIT with main's return value.
 */
.section .text
.global _start
.type _start, @function

_start:
    /* The stack is already set up by the ELF loader.
     * Clear the frame pointer for clean backtraces. */
    xorl %ebp, %ebp

    /* The ELF loader placed argc and argv on the stack for us:
     *   (%esp)   = argc
     *   4(%esp)  = argv (char **)
     * Push them as arguments to main(int argc, char **argv). */
    movl 4(%esp), %eax   /* eax = argv */
    movl 0(%esp), %ecx   /* ecx = argc */
    pushl %eax            /* push argv */
    pushl %ecx            /* push argc */
    call  main
    addl  $8, %esp

    /* main() returned in %eax — use it as the exit code */
    movl %eax, %ebx   /* ebx = exit code */
    movl $0,   %eax   /* eax = SYS_EXIT  */
    int  $0x80

    /* Should never reach here */
1:  hlt
    jmp 1b

.size _start, . - _start

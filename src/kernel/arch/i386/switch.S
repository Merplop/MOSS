/*
 * Context switch for i386.
 *
 * void switch_context(uint32_t *old_esp, uint32_t new_esp);
 *
 * Saves callee-saved registers on the current (old) stack, stores ESP
 * into *old_esp, loads new_esp into ESP, restores callee-saved registers
 * from the new stack, and returns into the new task's execution flow.
 */

.section .text
.global switch_context
.type switch_context, @function
switch_context:
    /* Grab arguments before we push anything */
    movl 4(%esp), %eax      /* eax = &old->esp  */
    movl 8(%esp), %ecx      /* ecx = new->esp   */

    /* Save callee-saved registers on old task's stack */
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi

    /* Store old task's stack pointer */
    movl %esp, (%eax)

    /* Switch to new task's stack */
    movl %ecx, %esp

    /* Restore callee-saved registers from new task's stack */
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp

    ret
.size switch_context, . - switch_context

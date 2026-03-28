/*
 * Low-level ISR and IRQ stubs for i386.
 *
 * CPU exceptions 0-31:
 *   - Some push an error code automatically; for those that don't we push
 *     a dummy zero so the stack frame is uniform.
 *
 * Hardware IRQs 0-15 are remapped to vectors 32-47 by the PIC init code.
 *
 * All stubs jump to a common handler that saves state, calls the C
 * dispatcher (isr_handler), restores state, and returns via iret.
 */

/* ---------- macros ---------------------------------------------------- */

/* ISR with no hardware error code (we push 0 ourselves). */
.macro ISR_NOERR num
.global isr\num
.type   isr\num, @function
isr\num:
    cli
    pushl $0            /* dummy error code  */
    pushl $\num         /* interrupt number   */
    jmp   isr_common_stub
.endm

/* ISR where the CPU already pushes an error code. */
.macro ISR_ERR num
.global isr\num
.type   isr\num, @function
isr\num:
    cli
    pushl $\num         /* interrupt number   */
    jmp   isr_common_stub
.endm

/* Hardware IRQ stub. */
.macro IRQ irq_num, int_num
.global irq\irq_num
.type   irq\irq_num, @function
irq\irq_num:
    cli
    pushl $0            /* dummy error code  */
    pushl $\int_num     /* mapped vector      */
    jmp   isr_common_stub
.endm

/* ---------- CPU exception stubs (0-31) -------------------------------- */

ISR_NOERR 0    /* #DE  Divide Error                */
ISR_NOERR 1    /* #DB  Debug                       */
ISR_NOERR 2    /* NMI                              */
ISR_NOERR 3    /* #BP  Breakpoint                  */
ISR_NOERR 4    /* #OF  Overflow                    */
ISR_NOERR 5    /* #BR  Bound Range Exceeded        */
ISR_NOERR 6    /* #UD  Invalid Opcode              */
ISR_NOERR 7    /* #NM  Device Not Available         */
ISR_ERR   8    /* #DF  Double Fault                */
ISR_NOERR 9    /* Coprocessor Segment Overrun       */
ISR_ERR   10   /* #TS  Invalid TSS                 */
ISR_ERR   11   /* #NP  Segment Not Present         */
ISR_ERR   12   /* #SS  Stack-Segment Fault         */
ISR_ERR   13   /* #GP  General Protection          */
ISR_ERR   14   /* #PF  Page Fault                  */
ISR_NOERR 15   /* (reserved)                        */
ISR_NOERR 16   /* #MF  x87 FP Exception            */
ISR_ERR   17   /* #AC  Alignment Check             */
ISR_NOERR 18   /* #MC  Machine Check               */
ISR_NOERR 19   /* #XM  SIMD FP Exception           */
ISR_NOERR 20   /* #VE  Virtualization Exception     */
ISR_ERR   21   /* #CP  Control Protection Exception */
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

/* ---------- Hardware IRQ stubs (IRQ 0-15 => INT 32-47) ---------------- */

IRQ  0, 32
IRQ  1, 33
IRQ  2, 34
IRQ  3, 35
IRQ  4, 36
IRQ  5, 37
IRQ  6, 38
IRQ  7, 39
IRQ  8, 40
IRQ  9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

/* ---------- Software interrupt: syscall (int 0x80 = vector 128) ----- */

ISR_NOERR 128

/* ---------- common handler -------------------------------------------- */

.extern isr_handler

isr_common_stub:
    /* Save all general-purpose registers */
    pushal

    /* Save the data segment */
    movw  %ds, %ax
    pushl %eax

    /* Load the kernel data segment (selector 0x10) */
    movw  $0x10, %ax
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %fs
    movw  %ax, %gs

    /* Call the C dispatcher; pass pointer to the saved-register frame */
    pushl %esp
    call  isr_handler
    addl  $4, %esp

    /* Restore the original data segment */
    popl  %eax
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %fs
    movw  %ax, %gs

    /* Restore general-purpose registers */
    popal

    /* Clean up the pushed error code and interrupt number */
    addl  $8, %esp

    /* Return from interrupt */
    iret

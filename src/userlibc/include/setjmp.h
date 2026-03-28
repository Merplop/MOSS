/* <setjmp.h> — MOSS user libc.
 * Non-local jumps for i386. */
#ifndef _SETJMP_H
#define _SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * jmp_buf saves:  EBX, ESI, EDI, EBP, ESP, EIP  → 6 x uint32_t
 */
typedef unsigned int jmp_buf[6];

int  setjmp(jmp_buf env)  __attribute__((returns_twice));
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* _SETJMP_H */

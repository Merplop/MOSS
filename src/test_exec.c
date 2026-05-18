/*
 * Test program for fork + execve on MOSS.
 * Compile with: i686-elf-gcc -O2 -ffreestanding -nostdlib \
 *   -Iuserlibc/include -c test_exec.c -o test_exec.o
 * Link with: i686-elf-ld -T userlibc/user.ld -o test_exec \
 *   userlibc/crt0.o test_exec.o userlibc/libc.a
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    printf("test_exec: PID=%d starting\n", getpid());

    pid_t child = fork();
    if (child < 0) {
        printf("fork failed!\n");
        return 1;
    }

    if (child == 0) {
        /* Child process: exec into hello */
        printf("child PID=%d: about to exec /bin/hello\n", getpid());
        char *args[] = { "hello", NULL };
        execvp("hello", args);
        /* If we get here, exec failed */
        printf("execvp failed!\n");
        _exit(1);
    } else {
        /* Parent: wait for child */
        int status = 0;
        pid_t w = waitpid(child, &status, 0);
        printf("parent: child %d exited with status %d\n", w, status);
    }

    printf("test_exec: done\n");
    return 0;
}
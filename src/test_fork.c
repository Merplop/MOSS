/*
 * test_fork.c — Test program for fork() and waitpid() on MOSS.
 * Compile with the MOSS userlibc cross-compiler, then run via `exec test_fork`.
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

int main(void) {
    printf("Parent PID: %d\n", getpid());
    printf("Calling fork()...\n");

    pid_t pid = fork();

    if (pid < 0) {
        printf("fork() failed!\n");
        return 1;
    }

    if (pid == 0) {
        /* Child process */
        printf("Child here! PID: %d\n", getpid());
        printf("Child exiting with code 42\n");
        return 42;
    } else {
        /* Parent process */
        printf("Parent here! Child PID: %d\n", pid);
        int status;
        pid_t waited = waitpid(pid, &status, 0);
        printf("waitpid returned: %d, exit code: %d\n", waited, status);
    }

    printf("Parent done.\n");
    return 0;
}

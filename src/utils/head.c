/* head — output first N lines */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char **argv) {
    int nlines = 10;
    int file_start = 1;

    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'n' && argc > 2) {
        nlines = atoi(argv[2]);
        file_start = 3;
    } else if (argc > 1 && argv[1][0] == '-' && argv[1][1] >= '0' && argv[1][1] <= '9') {
        nlines = atoi(argv[1] + 1);
        file_start = 2;
    }

    int fd = STDIN_FILENO;
    if (file_start < argc) {
        fd = open(argv[file_start], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "head: %s: No such file\n", argv[file_start]);
            return 1;
        }
    }

    char buf[1];
    int count = 0;
    while (count < nlines && read(fd, buf, 1) == 1) {
        write(STDOUT_FILENO, buf, 1);
        if (buf[0] == '\n') count++;
    }

    if (fd != STDIN_FILENO) close(fd);
    return 0;
}

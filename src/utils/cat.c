/* cat — concatenate files to stdout */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char **argv) {
    char buf[4096];

    if (argc <= 1) {
        /* Read from stdin */
        int n;
        while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0)
            write(STDOUT_FILENO, buf, n);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == '\0') {
            int n;
            while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0)
                write(STDOUT_FILENO, buf, n);
            continue;
        }
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "cat: %s: No such file or directory\n", argv[i]);
            return 1;
        }
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(STDOUT_FILENO, buf, n);
        close(fd);
    }
    return 0;
}

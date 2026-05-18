/* cp — copy files */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: cp <source> <dest>\n");
        return 1;
    }

    int src = open(argv[1], O_RDONLY);
    if (src < 0) {
        fprintf(stderr, "cp: %s: No such file\n", argv[1]);
        return 1;
    }

    int dst = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC);
    if (dst < 0) {
        fprintf(stderr, "cp: %s: Cannot create\n", argv[2]);
        close(src);
        return 1;
    }

    char buf[4096];
    int n;
    while ((n = read(src, buf, sizeof(buf))) > 0)
        write(dst, buf, n);

    close(src);
    close(dst);
    return 0;
}

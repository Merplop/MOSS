/* echo — print arguments */
#include <unistd.h>
#include <string.h>

int main(int argc, char **argv) {
    int no_newline = 0;
    int start = 1;

    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        no_newline = 1;
        start = 2;
    }

    for (int i = start; i < argc; i++) {
        if (i > start)
            write(STDOUT_FILENO, " ", 1);
        write(STDOUT_FILENO, argv[i], strlen(argv[i]));
    }

    if (!no_newline)
        write(STDOUT_FILENO, "\n", 1);

    return 0;
}

/* wc — word/line/character count */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

static void count(int fd, const char *name, int show_lines, int show_words, int show_chars) {
    char buf[4096];
    long lines = 0, words = 0, chars = 0;
    int in_word = 0;
    int n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            chars++;
            if (buf[i] == '\n') lines++;
            if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n') {
                in_word = 0;
            } else {
                if (!in_word) { words++; in_word = 1; }
            }
        }
    }

    if (show_lines) printf("%7ld ", lines);
    if (show_words) printf("%7ld ", words);
    if (show_chars) printf("%7ld ", chars);
    if (name) printf("%s", name);
    printf("\n");
}

int main(int argc, char **argv) {
    int show_l = 0, show_w = 0, show_c = 0;
    int file_start = 1;

    /* Parse flags */
    for (int i = 1; i < argc && argv[i][0] == '-'; i++) {
        for (int j = 1; argv[i][j]; j++) {
            if (argv[i][j] == 'l') show_l = 1;
            else if (argv[i][j] == 'w') show_w = 1;
            else if (argv[i][j] == 'c') show_c = 1;
        }
        file_start = i + 1;
    }

    /* Default: show all */
    if (!show_l && !show_w && !show_c) {
        show_l = show_w = show_c = 1;
    }

    if (file_start >= argc) {
        count(STDIN_FILENO, NULL, show_l, show_w, show_c);
    } else {
        for (int i = file_start; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "wc: %s: No such file\n", argv[i]);
                continue;
            }
            count(fd, argv[i], show_l, show_w, show_c);
            close(fd);
        }
    }
    return 0;
}

/* grep — search for patterns in files */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int match_line(const char *line, const char *pattern) {
    return strstr(line, pattern) != NULL;
}

static void grep_fd(int fd, const char *fname, const char *pattern, int show_name, int invert) {
    char buf[4096];
    int buf_len = 0;

    while (1) {
        int n = read(fd, buf + buf_len, sizeof(buf) - buf_len - 1);
        if (n <= 0 && buf_len == 0) break;
        if (n > 0) buf_len += n;
        buf[buf_len] = '\0';

        /* Process lines */
        char *start = buf;
        char *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            int m = match_line(start, pattern);
            if (m != invert) {
                if (show_name) printf("%s:", fname);
                printf("%s\n", start);
            }
            start = nl + 1;
        }

        /* Move remainder to beginning */
        int remain = buf_len - (int)(start - buf);
        if (remain > 0) memmove(buf, start, remain);
        buf_len = remain;

        if (n <= 0) {
            /* Process last line without newline */
            if (buf_len > 0) {
                buf[buf_len] = '\0';
                int m = match_line(buf, pattern);
                if (m != invert) {
                    if (show_name) printf("%s:", fname);
                    printf("%s\n", buf);
                }
            }
            break;
        }
    }
}

int main(int argc, char **argv) {
    int invert = 0;
    int arg_start = 1;

    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        invert = 1;
        arg_start = 2;
    }

    if (arg_start >= argc) {
        fprintf(stderr, "usage: grep [-v] pattern [file...]\n");
        return 2;
    }

    const char *pattern = argv[arg_start++];
    int nfiles = argc - arg_start;

    if (nfiles == 0) {
        grep_fd(STDIN_FILENO, "(stdin)", pattern, 0, invert);
    } else {
        for (int i = arg_start; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                fprintf(stderr, "grep: %s: No such file\n", argv[i]);
                continue;
            }
            grep_fd(fd, argv[i], pattern, nfiles > 1, invert);
            close(fd);
        }
    }
    return 0;
}

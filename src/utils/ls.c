/* ls — list directory contents */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>

/* stat structure (matches kernel moss_stat_t) */
struct moss_stat {
    uint32_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_size;
    uint32_t st_atime;
    uint32_t st_mtime;
    uint32_t st_ctime;
};

extern int32_t _syscall2(uint32_t num, uint32_t a1, uint32_t a2);
#define SYS_STAT 15

static int do_stat(const char *path, struct moss_stat *st) {
    return (int)_syscall2(SYS_STAT, (uint32_t)path, (uint32_t)st);
}

int main(int argc, char **argv) {
    const char *path = ".";
    int show_long = 0;
    int show_all = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'l') show_long = 1;
                else if (argv[i][j] == 'a') show_all = 1;
            }
        } else {
            path = argv[i];
        }
    }

    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "ls: cannot access '%s': No such file or directory\n", path);
        return 1;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        /* Skip . and .. unless -a */
        if (!show_all && ent->d_name[0] == '.')
            continue;

        if (show_long) {
            char type = '-';
            if (ent->d_type == DT_DIR) type = 'd';

            /* Try to stat for size */
            char fullpath[512];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", path, ent->d_name);
            struct moss_stat st;
            int sz = 0;
            if (do_stat(fullpath, &st) == 0)
                sz = (int)st.st_size;

            printf("%c  %7d  %s\n", type, sz, ent->d_name);
        } else {
            printf("%s  ", ent->d_name);
        }
    }

    if (!show_long)
        printf("\n");

    closedir(d);
    return 0;
}

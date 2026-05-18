/* <dirent.h> — MOSS user libc directory operations. */
#ifndef _DIRENT_H
#define _DIRENT_H

#include <stdint.h>
#include <syscall.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Directory entry types (match ext2 file_type values) */
#define DT_UNKNOWN 0
#define DT_REG     1
#define DT_DIR     2
#define DT_CHR     3
#define DT_BLK     4
#define DT_FIFO    5
#define DT_SOCK    6
#define DT_LNK     7

struct dirent {
    uint32_t d_ino;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
};

typedef struct {
    int fd;
    char buf[1024];
    int buf_pos;
    int buf_len;
} DIR;

static inline DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return (DIR *)0;
    /* Allocate DIR on heap (use a static for simplicity — not thread-safe) */
    static DIR _dir;
    _dir.fd = fd;
    _dir.buf_pos = 0;
    _dir.buf_len = 0;
    return &_dir;
}

static inline struct dirent *readdir(DIR *dirp) {
    static struct dirent _de;

    if (!dirp)
        return (struct dirent *)0;

    /* Refill buffer if exhausted */
    if (dirp->buf_pos >= dirp->buf_len) {
        int32_t n = _syscall3(SYS_GETDENTS, (uint32_t)dirp->fd,
                              (uint32_t)dirp->buf, sizeof(dirp->buf));
        if (n <= 0)
            return (struct dirent *)0;
        dirp->buf_len = n;
        dirp->buf_pos = 0;
    }

    /* Parse the current entry from buffer */
    uint8_t *p = (uint8_t *)dirp->buf + dirp->buf_pos;
    _de.d_ino = *(uint32_t *)p;
    _de.d_reclen = *(uint16_t *)(p + 4);
    _de.d_type = p[6];

    /* Copy name */
    const char *name = (const char *)(p + 7);
    int i = 0;
    while (name[i] && i < 255) {
        _de.d_name[i] = name[i];
        i++;
    }
    _de.d_name[i] = '\0';

    dirp->buf_pos += _de.d_reclen;
    return &_de;
}

static inline int closedir(DIR *dirp) {
    if (!dirp)
        return -1;
    int r = close(dirp->fd);
    dirp->fd = -1;
    return r;
}

#ifdef __cplusplus
}
#endif

#endif /* _DIRENT_H */

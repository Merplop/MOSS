// MOSS KERNEL - Filesystem Commands (ext2-backed)
// (C) Miro Haapalainen, 2024

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <kernel/kernel.h>
#include <kernel/ext2.h>

extern uint32_t cwd_ino;

/* Helper: look up a name in the cwd. Returns inode number or 0. */
static uint32_t find_in_cwd(const char *name)
{
    return ext2_lookup(cwd_ino, name);
}

/* Forward declaration for readdir callback */
static void ls_print_entry(const char *name, uint8_t name_len,
                           uint32_t inode, uint8_t file_type);

void ls_cmd(void) {
    ext2_readdir(cwd_ino, ls_print_entry);
}

/* Callback used by ls_cmd via ext2_readdir */
static void ls_print_entry(const char *name, uint8_t name_len,
                           uint32_t inode, uint8_t file_type)
{
    /* Copy name to a null-terminated buffer */
    char buf[256];
    memcpy(buf, name, name_len);
    buf[name_len] = '\0';

    if (file_type == EXT2_FT_DIR) {
        printf("[%s]", buf);
    } else {
        printf("%s", buf);
    }

    /* Print size */
    ext2_inode_t ino;
    if (ext2_read_inode(inode, &ino) == 0) {
        printf("   %d bytes", ino.i_size);
    }
    printf("\r\n");
}

void mkdir_cmd(void) {
    if (argc != 2) {
        printf(ARG_COUNT_ERROR);
        return;
    }
    if (ext2_lookup(cwd_ino, argv[1]) != 0) {
        printf(DIR_EXISTS_ERROR);
        return;
    }
    uint32_t ino = ext2_create(cwd_ino, argv[1],
                               EXT2_S_IFDIR | EXT2_S_IRWXU | EXT2_S_IRWXG | EXT2_S_IRWXO);
    if (ino == 0) {
        printf(DISK_ERROR);
    }
}

void touch_cmd(void) {
    if (argc != 2) {
        printf(ARG_COUNT_ERROR);
        return;
    }
    if (ext2_lookup(cwd_ino, argv[1]) != 0) {
        printf(FILE_EXISTS_ERROR);
        return;
    }
    uint32_t ino = ext2_create(cwd_ino, argv[1],
                               EXT2_S_IFREG | EXT2_S_IRWXU | EXT2_S_IRWXG | EXT2_S_IRWXO);
    if (ino == 0) {
        printf(DISK_ERROR);
    }
}

void cd_cmd(void) {
    if (argc == 1) {
        cwd_ino = EXT2_ROOT_INO;
    } else if (argc == 2) {
        if (memcmp(argv[1], "..", 2) == 0 && strlen(argv[1]) == 2) {
            uint32_t parent = ext2_lookup(cwd_ino, "..");
            if (parent != 0)
                cwd_ino = parent;
        } else if (memcmp(argv[1], ".", 1) == 0 && strlen(argv[1]) == 1) {
            /* stay in same dir */
        } else {
            uint32_t target = ext2_lookup(cwd_ino, argv[1]);
            if (target == 0) {
                printf(DIR_NOT_FOUND_ERROR);
                return;
            }
            /* Verify it's a directory */
            ext2_inode_t ino;
            if (ext2_read_inode(target, &ino) != 0 || !(ino.i_mode & EXT2_S_IFDIR)) {
                printf(ARG_ERROR);
                return;
            }
            cwd_ino = target;
        }
    } else {
        printf(ARG_COUNT_ERROR);
    }
}

void mv_cmd(void) {
    printf("TODO: implement mv with ext2\r\n");
}

void rm_cmd(void) {
    if (argc != 2) {
        printf(ARG_COUNT_ERROR);
        return;
    }
    uint32_t target = find_in_cwd(argv[1]);
    if (target == 0) {
        printf(FILE_NOT_FOUND_ERROR);
        return;
    }
    ext2_inode_t ino;
    if (ext2_read_inode(target, &ino) != 0) {
        printf(DISK_ERROR);
        return;
    }
    if (ino.i_mode & EXT2_S_IFDIR) {
        printf("Invalid argument - Use rmdir to remove directories\r\n");
        return;
    }
    if (ext2_remove(cwd_ino, argv[1]) != 0) {
        printf(DISK_ERROR);
    }
}

void cat_cmd(void) {
    if (argc != 2) {
        printf(ARG_COUNT_ERROR);
        return;
    }
    uint32_t ino_num = find_in_cwd(argv[1]);
    if (ino_num == 0) {
        printf(FILE_NOT_FOUND_ERROR);
        return;
    }
    ext2_inode_t ino;
    if (ext2_read_inode(ino_num, &ino) != 0) {
        printf(DISK_ERROR);
        return;
    }
    if (ino.i_size == 0) {
        printf(FILE_EMPTY_ERROR);
        return;
    }
    /* Read file in chunks */
    uint8_t buf[1024];
    uint32_t offset = 0;
    while (offset < ino.i_size) {
        uint32_t chunk = sizeof(buf);
        if (chunk > ino.i_size - offset)
            chunk = ino.i_size - offset;
        int rd = ext2_read_file(ino_num, buf, offset, chunk);
        if (rd <= 0)
            break;
        for (int i = 0; i < rd; i++)
            putchar(buf[i]);
        offset += (uint32_t)rd;
    }
    printf("\r\n");
}

void rmdir_cmd(void) {
    if (argc != 2) {
        printf(ARG_COUNT_ERROR);
        return;
    }
    uint32_t target = find_in_cwd(argv[1]);
    if (target == 0) {
        printf(DIR_NOT_FOUND_ERROR);
        return;
    }
    ext2_inode_t ino;
    if (ext2_read_inode(target, &ino) != 0 || !(ino.i_mode & EXT2_S_IFDIR)) {
        printf(ARG_ERROR);
        return;
    }
    if (ext2_remove(cwd_ino, argv[1]) != 0) {
        printf("Cannot remove directory (may not be empty)\r\n");
    }
}

/* Helper used by the text editor and run_file: get inode for a file in cwd */
uint32_t fs_find_file(const char *name)
{
    return find_in_cwd(name);
}

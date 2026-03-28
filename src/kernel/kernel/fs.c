// MOSS KERNEL - Filesystem Commands (ext2-backed)
// (C) Miro Haapalainen, 2024

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <kernel/kernel.h>
#include <kernel/ext2.h>
#include <kernel/blkdev.h>
#include <kernel/keyboard.h>

/* ATA driver access */
#include "../arch/i386/ata.h"

extern uint32_t cwd_ino;
extern char cwd_path[256];

/* Resolve a path (absolute or relative to cwd) to an inode number.
 * Returns 0 if not found. */
uint32_t fs_resolve_path(const char *path)
{
    if (!path || path[0] == '\0')
        return 0;

    uint32_t dir_ino;
    if (path[0] == '/') {
        dir_ino = EXT2_ROOT_INO;
        path++;
        if (path[0] == '\0')
            return EXT2_ROOT_INO;
    } else {
        dir_ino = cwd_ino;
    }

    char component[256];
    while (*path) {
        while (*path == '/')
            path++;
        if (*path == '\0')
            break;

        int len = 0;
        while (path[len] != '\0' && path[len] != '/' && len < 255)
            len++;
        memcpy(component, path, len);
        component[len] = '\0';
        path += len;

        uint32_t ino = ext2_lookup(dir_ino, component);
        if (ino == 0)
            return 0;

        while (*path == '/')
            path++;
        if (*path != '\0') {
            ext2_inode_t inode;
            if (ext2_read_inode(ino, &inode) != 0)
                return 0;
            if ((inode.i_mode & 0xF000) != EXT2_S_IFDIR)
                return 0;
            dir_ino = ino;
        } else {
            return ino;
        }
    }
    return dir_ino;
}

/* Helper: look up a name in the cwd. Returns inode number or 0. */
static uint32_t find_in_cwd(const char *name)
{
    return ext2_lookup(cwd_ino, name);
}

/* Forward declaration for readdir callback */
static void ls_print_entry(const char *name, uint8_t name_len,
                           uint32_t inode, uint8_t file_type);

void ls_cmd(void) {
    ext2_fs_t *f = ext2_get_fs();
    if (!f) {
        printf("DEBUG: ext2 not ready\r\n");
        return;
    }
    //printf("DEBUG ls: cwd=%d free_ino=%d free_blk=%d\r\n",
    //       cwd_ino, f->sb.s_free_inodes_count, f->sb.s_free_blocks_count);
    int rc = ext2_readdir(cwd_ino, ls_print_entry);
    //printf("DEBUG ls: readdir returned %d\r\n", rc);
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

/* Update cwd_path after a cd operation. */
static void update_cwd_path(const char *arg)
{
    if (!arg) {
        /* cd with no args -> root */
        cwd_path[0] = '/';
        cwd_path[1] = '\0';
        return;
    }
    if (arg[0] == '/') {
        /* Absolute path */
        size_t len = strlen(arg);
        if (len >= 255) len = 255;
        memcpy(cwd_path, arg, len);
        cwd_path[len] = '\0';
        /* Remove trailing slash (unless root) */
        while (len > 1 && cwd_path[len - 1] == '/') {
            cwd_path[--len] = '\0';
        }
        return;
    }
    if (memcmp(arg, "..", 3) == 0) {
        /* Go up: remove last component */
        size_t cur_len = strlen(cwd_path);
        if (cur_len <= 1) return; /* already at root */
        /* Find last '/' before trailing component */
        size_t i = cur_len - 1;
        while (i > 0 && cwd_path[i] != '/')
            i--;
        if (i == 0) {
            cwd_path[0] = '/';
            cwd_path[1] = '\0';
        } else {
            cwd_path[i] = '\0';
        }
        return;
    }
    if (arg[0] == '.' && arg[1] == '\0')
        return; /* stay */
    /* Append component */
    size_t cur_len = strlen(cwd_path);
    if (cur_len > 1) {
        cwd_path[cur_len] = '/';
        cur_len++;
    }
    size_t arg_len = strlen(arg);
    if (cur_len + arg_len >= 255) arg_len = 255 - cur_len;
    memcpy(cwd_path + cur_len, arg, arg_len);
    cwd_path[cur_len + arg_len] = '\0';
}

void cd_cmd(void) {
    if (argc == 1) {
        cwd_ino = EXT2_ROOT_INO;
        update_cwd_path(NULL);
    } else if (argc == 2) {
        if (argv[1][0] == '/') {
            /* Absolute path */
            uint32_t target = fs_resolve_path(argv[1]);
            if (target == 0) {
                printf(DIR_NOT_FOUND_ERROR);
                return;
            }
            ext2_inode_t ino;
            if (ext2_read_inode(target, &ino) != 0 || !(ino.i_mode & EXT2_S_IFDIR)) {
                printf(ARG_ERROR);
                return;
            }
            cwd_ino = target;
            update_cwd_path(argv[1]);
        } else if (memcmp(argv[1], "..", 2) == 0 && strlen(argv[1]) == 2) {
            uint32_t parent = ext2_lookup(cwd_ino, "..");
            if (parent != 0) {
                cwd_ino = parent;
                update_cwd_path("..");
            }
        } else if (memcmp(argv[1], ".", 1) == 0 && strlen(argv[1]) == 1) {
            /* stay in same dir */
        } else {
            uint32_t target = ext2_lookup(cwd_ino, argv[1]);
            if (target == 0) {
                printf(DIR_NOT_FOUND_ERROR);
                return;
            }
            ext2_inode_t ino;
            if (ext2_read_inode(target, &ino) != 0 || !(ino.i_mode & EXT2_S_IFDIR)) {
                printf(ARG_ERROR);
                return;
            }
            cwd_ino = target;
            update_cwd_path(argv[1]);
        }
    } else {
        printf(ARG_COUNT_ERROR);
    }
}

void mv_cmd(void) {
    if (argc != 3) {
        printf(ARG_COUNT_ERROR);
        return;
    }

    /* Resolve the source file */
    uint32_t src_ino = fs_resolve_path(argv[1]);
    if (src_ino == 0) {
        printf(FILE_NOT_FOUND_ERROR);
        return;
    }

    /* Determine source directory and basename */
    uint32_t src_dir = cwd_ino;
    const char *src_base = argv[1];
    /* Find last '/' to split dir/basename */
    const char *last_slash = NULL;
    for (const char *p = argv[1]; *p; p++) {
        if (*p == '/') last_slash = p;
    }
    if (last_slash) {
        /* Resolve source directory */
        char src_dir_path[256];
        size_t dir_len = last_slash - argv[1];
        if (dir_len == 0) {
            src_dir = EXT2_ROOT_INO;
        } else {
            if (dir_len >= 255) dir_len = 255;
            memcpy(src_dir_path, argv[1], dir_len);
            src_dir_path[dir_len] = '\0';
            src_dir = fs_resolve_path(src_dir_path);
            if (src_dir == 0) {
                printf(DIR_NOT_FOUND_ERROR);
                return;
            }
        }
        src_base = last_slash + 1;
    }

    /* Determine destination directory and new name */
    uint32_t dst_dir = cwd_ino;
    const char *dst_name = argv[2];

    /* Check if destination is an existing directory */
    uint32_t dst_check = fs_resolve_path(argv[2]);
    if (dst_check != 0) {
        ext2_inode_t dst_inode;
        if (ext2_read_inode(dst_check, &dst_inode) == 0 &&
            (dst_inode.i_mode & EXT2_S_IFDIR)) {
            /* Moving into a directory — keep original name */
            dst_dir = dst_check;
            dst_name = src_base;
        } else {
            /* Destination exists and is a file — overwrite */
            /* Find dest directory */
            last_slash = NULL;
            for (const char *p = argv[2]; *p; p++) {
                if (*p == '/') last_slash = p;
            }
            if (last_slash) {
                char dst_dir_path[256];
                size_t dlen = last_slash - argv[2];
                if (dlen == 0) {
                    dst_dir = EXT2_ROOT_INO;
                } else {
                    if (dlen >= 255) dlen = 255;
                    memcpy(dst_dir_path, argv[2], dlen);
                    dst_dir_path[dlen] = '\0';
                    dst_dir = fs_resolve_path(dst_dir_path);
                    if (dst_dir == 0) {
                        printf(DIR_NOT_FOUND_ERROR);
                        return;
                    }
                }
                dst_name = last_slash + 1;
            }
            /* Remove existing destination */
            ext2_remove(dst_dir, dst_name);
        }
    } else {
        /* Destination doesn't exist — it's the new name */
        last_slash = NULL;
        for (const char *p = argv[2]; *p; p++) {
            if (*p == '/') last_slash = p;
        }
        if (last_slash) {
            char dst_dir_path[256];
            size_t dlen = last_slash - argv[2];
            if (dlen == 0) {
                dst_dir = EXT2_ROOT_INO;
            } else {
                if (dlen >= 255) dlen = 255;
                memcpy(dst_dir_path, argv[2], dlen);
                dst_dir_path[dlen] = '\0';
                dst_dir = fs_resolve_path(dst_dir_path);
                if (dst_dir == 0) {
                    printf(DIR_NOT_FOUND_ERROR);
                    return;
                }
            }
            dst_name = last_slash + 1;
        }
    }

    /* Check for duplicate in destination */
    if (ext2_lookup(dst_dir, dst_name) != 0) {
        printf("Destination already exists\r\n");
        return;
    }

    /* Read source inode to determine file type */
    ext2_inode_t src_inode;
    if (ext2_read_inode(src_ino, &src_inode) != 0) {
        printf(DISK_ERROR);
        return;
    }
    uint8_t ft = (src_inode.i_mode & EXT2_S_IFDIR) ? EXT2_FT_DIR : EXT2_FT_REG_FILE;

    /* Add entry in destination directory */
    if (ext2_link(dst_dir, src_ino, dst_name, ft) != 0) {
        printf(DISK_ERROR);
        return;
    }

    /* If moving a directory, update its '..' entry to point to new parent */
    if (ft == EXT2_FT_DIR && dst_dir != src_dir) {
        ext2_update_dotdot(src_ino, dst_dir);
        /* Adjust link counts: old parent loses a link, new parent gains one */
        ext2_inode_t old_parent;
        if (ext2_read_inode(src_dir, &old_parent) == 0) {
            old_parent.i_links_count--;
            ext2_write_inode(src_dir, &old_parent);
        }
        ext2_inode_t new_parent;
        if (ext2_read_inode(dst_dir, &new_parent) == 0) {
            new_parent.i_links_count++;
            ext2_write_inode(dst_dir, &new_parent);
        }
    }

    /* Remove source directory entry (without freeing the inode) */
    ext2_unlink(src_dir, src_base);
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

/* Helper used by the text editor, run_file, and exec: resolves paths */
uint32_t fs_find_file(const char *name)
{
    return fs_resolve_path(name);
}

/* ------------------------------------------------------------------ */
/*  Disk management commands                                          */
/* ------------------------------------------------------------------ */

/* MBR partition table structures (duplicated from kernel.c for self-contained use) */
#define MBR_SIGNATURE    0xAA55
#define MBR_PART_LINUX   0x83
#define MBR_PART_COUNT   4

typedef struct {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t sector_count;
} __attribute__((packed)) mbr_partition_t;

void disks_cmd(void) {
    printf("ATA drives:\r\n");
    int found = 0;
    for (int d = 0; d < 4; d++) {
        blkdev_t *bdev = ata_get_blkdev(d);
        if (bdev) {
            uint32_t mb = bdev->sector_count / 2048;
            const char *bus = (d < 2) ? "primary" : "secondary";
            const char *role = (d % 2 == 0) ? "master" : "slave";
            printf("  %d: %s %s  %d MiB (%d sectors)\r\n",
                   d, bus, role, mb, bdev->sector_count);

            /* Check for MBR partition table */
            static uint8_t mbr_buf[512];
            if (bdev->read_sectors(bdev, 0, 1, mbr_buf) == 0) {
                uint16_t sig = *(uint16_t *)(mbr_buf + 510);
                if (sig == MBR_SIGNATURE) {
                    mbr_partition_t *parts = (mbr_partition_t *)(mbr_buf + 446);
                    for (int p = 0; p < MBR_PART_COUNT; p++) {
                        if (parts[p].type == 0 || parts[p].sector_count == 0)
                            continue;
                        uint32_t pmb = parts[p].sector_count / 2048;
                        printf("     p%d: type=0x%x  LBA %d  %d MiB\r\n",
                               p + 1, parts[p].type,
                               parts[p].lba_start, pmb);
                    }
                }
            }

            found = 1;
        }
    }
    if (!found) {
        printf("  No drives detected\r\n");
    }

    /* Show current mount status */
    ext2_fs_t *f = ext2_get_fs();
    if (f) {
        char vol[17];
        memcpy(vol, f->sb.s_volume_name, 16);
        vol[16] = '\0';
        printf("Mounted: vol=\"%s\" %d blocks, %d free\r\n",
               vol, f->total_blocks, f->sb.s_free_blocks_count);
    } else {
        printf("No filesystem mounted\r\n");
    }
}

void mkfs_cmd(void) {
    if (argc != 2) {
        printf("Usage: mkfs <drive_number>\r\n");
        printf("Formats a drive with MOSS ext2 filesystem.\r\n");
        printf("Use 'disks' to see available drives.\r\n");
        return;
    }

    int drive_num = atoi(argv[1]);
    if (drive_num < 0 || drive_num > 3) {
        printf("Invalid drive number (0-3)\r\n");
        return;
    }

    blkdev_t *bdev = ata_get_blkdev(drive_num);
    if (!bdev) {
        printf("Drive %d not detected\r\n", drive_num);
        return;
    }

    uint32_t mb = bdev->sector_count / 2048;
    printf("WARNING: This will ERASE ALL DATA on drive %d (%d MiB)!\r\n",
           drive_num, mb);
    printf("Type 'yes' to confirm: ");

    /* Read confirmation */
    char confirm[8];
    int clen = 0;
    memset(confirm, 0, sizeof(confirm));
    while (1) {
        uint8_t ch = get_key();
        if (ch == 0x0D) {
            printf("\r\n");
            break;
        }
        if (ch == 0x08) {
            if (clen > 0) {
                confirm[--clen] = '\0';
                putchar(ch);
            }
            continue;
        }
        if (clen < 6) {
            confirm[clen++] = ch;
            putchar(ch);
        }
    }

    if (memcmp(confirm, "yes", 4) != 0) {
        printf("Aborted.\r\n");
        return;
    }

    uint32_t sectors = bdev->sector_count;

    printf("Formatting drive %d (%d sectors)...\r\n", drive_num, sectors);
    int rc = ext2_format(bdev, 0, sectors);
    if (rc == 0) {
        printf("Format complete. Filesystem mounted.\r\n");
        cwd_ino = EXT2_ROOT_INO;
    } else {
        printf("Format failed (rc=%d)\r\n", rc);
    }
}

void mount_cmd(void) {
    if (argc != 2) {
        printf("Usage: mount <drive_number>\r\n");
        printf("Mounts an existing MOSS ext2 filesystem.\r\n");
        printf("Use 'disks' to see available drives.\r\n");
        return;
    }

    int drive_num = atoi(argv[1]);
    if (drive_num < 0 || drive_num > 3) {
        printf("Invalid drive number (0-3)\r\n");
        return;
    }

    blkdev_t *bdev = ata_get_blkdev(drive_num);
    if (!bdev) {
        printf("Drive %d not detected\r\n", drive_num);
        return;
    }

    /* First try raw ext2 at LBA 0 */
    int rc = ext2_init(bdev, 0, 0);
    if (rc == 0) {
        printf("ext2 mounted from drive %d (raw)\r\n", drive_num);
        cwd_ino = EXT2_ROOT_INO;
        return;
    }

    /* Check MBR partition table for Linux partitions */
    static uint8_t mbr_buf[512];
    if (bdev->read_sectors(bdev, 0, 1, mbr_buf) != 0) {
        printf("Cannot read drive %d\r\n", drive_num);
        return;
    }

    uint16_t sig = *(uint16_t *)(mbr_buf + 510);
    if (sig == MBR_SIGNATURE) {
        mbr_partition_t *parts = (mbr_partition_t *)(mbr_buf + 446);
        for (int p = 0; p < MBR_PART_COUNT; p++) {
            if (parts[p].type != MBR_PART_LINUX)
                continue;
            if (parts[p].lba_start == 0 || parts[p].sector_count == 0)
                continue;
            rc = ext2_init(bdev, parts[p].lba_start, 0);
            if (rc == 0) {
                printf("ext2 mounted from drive %d partition %d (LBA %d)\r\n",
                       drive_num, p + 1, parts[p].lba_start);
                cwd_ino = EXT2_ROOT_INO;
                return;
            }
        }
    }

    printf("No valid ext2 filesystem on drive %d\r\n", drive_num);
    printf("Use 'mkfs %d' to create one.\r\n", drive_num);
}

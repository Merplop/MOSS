/*
 * ext2 filesystem implementation.
 *
 * Supports a single block group with 1024-byte blocks, direct + singly
 * indirect block pointers, regular files and directories.
 *
 * MOSS Kernel
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <kernel/ext2.h>

/* Pull in the concrete ATA type so we can call read/write */
#include "../arch/i386/ata.h"

/* ------------------------------------------------------------------ */
/*  Global filesystem state (single-mount only)                       */
/* ------------------------------------------------------------------ */

static ext2_fs_t fs;
static int       fs_ready = 0;

ext2_fs_t *ext2_get_fs(void)
{
    return fs_ready ? &fs : 0;
}

/* ------------------------------------------------------------------ */
/*  Block I/O helpers                                                 */
/* ------------------------------------------------------------------ */

/* Read one filesystem block into `buf`. */
static int read_block(uint32_t block, void *buf)
{
    uint32_t sectors_per_block = fs.block_size / ATA_SECTOR_SIZE;
    uint32_t lba = fs.part_lba + block * sectors_per_block;
    return ata_read_sectors((ata_drive_t *)fs.drive, lba, sectors_per_block, buf);
}

/* Write one filesystem block from `buf`. */
static int write_block(uint32_t block, const void *buf)
{
    uint32_t sectors_per_block = fs.block_size / ATA_SECTOR_SIZE;
    uint32_t lba = fs.part_lba + block * sectors_per_block;
    return ata_write_sectors((ata_drive_t *)fs.drive, lba, sectors_per_block, buf);
}

/* Temporary block buffer (1024 bytes – fits our block size) */
static uint8_t tmp_blk[4096] __attribute__((aligned(4)));

/* ------------------------------------------------------------------ */
/*  Superblock / group descriptor helpers                             */
/* ------------------------------------------------------------------ */

static int read_superblock(void)
{
    /* Superblock is at byte offset 1024 = sector 2 (for 512-byte sectors) */
    static uint8_t buf[1024];
    if (ata_read_sectors((ata_drive_t *)fs.drive, fs.part_lba + 2, 2, buf) != 0)
        return -1;
    memcpy(&fs.sb, buf, sizeof(ext2_superblock_t));
    return 0;
}

static int write_superblock(void)
{
    static uint8_t buf[1024];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, &fs.sb, sizeof(ext2_superblock_t));
    return ata_write_sectors((ata_drive_t *)fs.drive, fs.part_lba + 2, 2, buf);
}

static int read_group_desc(void)
{
    printf("DEBUG read_gd: bgdt_block=%d blk_size=%d\r\n",
           fs.bgdt_block, fs.block_size);
    if (read_block(fs.bgdt_block, tmp_blk) != 0) {
        printf("DEBUG read_gd: read_block FAILED\r\n");
        return -1;
    }
    /* Dump first 32 bytes of the block to see raw content */
    printf("DEBUG read_gd raw:");
    for (int __i = 0; __i < 32; __i++)
        printf(" %d", (int)tmp_blk[__i]);
    printf("\r\n");
    memcpy(&fs.gd, tmp_blk, sizeof(ext2_group_desc_t));
    return 0;
}

static int write_group_desc(void)
{
    if (read_block(fs.bgdt_block, tmp_blk) != 0)
        return -1;
    memcpy(tmp_blk, &fs.gd, sizeof(ext2_group_desc_t));
    return write_block(fs.bgdt_block, tmp_blk);
}

/* ------------------------------------------------------------------ */
/*  Bitmap helpers                                                    */
/* ------------------------------------------------------------------ */

/* Allocate a single bit from a bitmap block.
 * Returns the 0-based bit index, or 0 on failure. */
static uint32_t bitmap_alloc(uint32_t bitmap_block, uint32_t max_bits)
{
    if (read_block(bitmap_block, tmp_blk) != 0) {
        printf("bitmap_alloc: failed to read bitmap block %u\n", bitmap_block);
        return 0; 
    }

    uint32_t bytes = (max_bits + 7) / 8;
    for (uint32_t i = 0; i < bytes; i++) {
        if (tmp_blk[i] == 0xFF)
            continue;
        for (int bit = 0; bit < 8; bit++) {
            uint32_t idx = i * 8 + bit;
            if (idx >= max_bits) {
                printf("bitmap_alloc: reached end of bitmap without finding free bit (max_bits=%u)\n", max_bits);
                return 0;
            }
            if (!(tmp_blk[i] & (1 << bit))) {
                tmp_blk[i] |= (1 << bit);
                write_block(bitmap_block, tmp_blk);
                return idx + 1;  /* ext2 bitmap indices are 1-based for inodes,
                                    but 0-based for blocks. We return the raw
                                    index; caller adjusts. */
            }
        }
    }
    return 0;  /* full */
}

/* Free a single bit in a bitmap block (0-based index). */
static int bitmap_free(uint32_t bitmap_block, uint32_t idx)
{
    if (read_block(bitmap_block, tmp_blk) != 0)
        return -1;
    uint32_t byte = idx / 8;
    uint32_t bit  = idx % 8;
    tmp_blk[byte] &= ~(1 << bit);
    return write_block(bitmap_block, tmp_blk);
}

/* ------------------------------------------------------------------ */
/*  Block allocation / deallocation                                   */
/* ------------------------------------------------------------------ */

static uint32_t alloc_block(void)
{
    if (fs.sb.s_free_blocks_count == 0 || fs.gd.bg_free_blocks_count == 0)
        return 0;

    /* bitmap_alloc returns a 1-based value; ext2 block bitmap is 0-based
     * relative to the group, so block number = returned_val - 1 + first_data_block
     * But for a single group with first_data_block=1 and block_size=1024,
     * the bitmap bit N represents block N.  For simplicity: */
    uint32_t bit = bitmap_alloc(fs.gd.bg_block_bitmap, fs.sb.s_blocks_count);
    if (bit == 0)
        return 0;
    uint32_t block_no = bit - 1;  /* convert from 1-based bitmap_alloc return */

    fs.sb.s_free_blocks_count--;
    fs.gd.bg_free_blocks_count--;
    write_superblock();
    write_group_desc();

    /* Zero the newly allocated block */
    memset(tmp_blk, 0, fs.block_size);
    write_block(block_no, tmp_blk);

    return block_no;
}

static void free_block(uint32_t block_no)
{
    if (block_no == 0)
        return;
    bitmap_free(fs.gd.bg_block_bitmap, block_no);
    fs.sb.s_free_blocks_count++;
    fs.gd.bg_free_blocks_count++;
    write_superblock();
    write_group_desc();
}

/* ------------------------------------------------------------------ */
/*  Inode allocation / deallocation                                   */
/* ------------------------------------------------------------------ */

static uint32_t alloc_inode(void)
{
    if (fs.sb.s_free_inodes_count == 0 || fs.gd.bg_free_inodes_count == 0) {
        printf("alloc_inode: no free inodes available\n");
        return 0; 
    }

    uint32_t bit = bitmap_alloc(fs.gd.bg_inode_bitmap, fs.sb.s_inodes_count);
    if (bit == 0) {
        printf("alloc_inode: bitmap_alloc failed to allocate inode\n");
        return 0;
    }
    /* bit is 1-based from bitmap_alloc, and ext2 inodes are 1-based.
     * bitmap bit 0 = inode 1, so inode number = bit. */
    uint32_t ino = bit;

    fs.sb.s_free_inodes_count--;
    fs.gd.bg_free_inodes_count--;
    write_superblock();
    write_group_desc();
    return ino;
}

static void free_inode(uint32_t ino)
{
    if (ino == 0)
        return;
    bitmap_free(fs.gd.bg_inode_bitmap, ino - 1);
    fs.sb.s_free_inodes_count++;
    fs.gd.bg_free_inodes_count++;
    write_superblock();
    write_group_desc();
}

/* ------------------------------------------------------------------ */
/*  Inode read/write                                                  */
/* ------------------------------------------------------------------ */

int ext2_read_inode(uint32_t ino, ext2_inode_t *out)
{
    if (ino == 0 || !fs_ready)
        return -1;

    uint32_t index = ino - 1;  /* inodes are 1-based */
    uint32_t inodes_per_block = fs.block_size / fs.inode_size;
    uint32_t block_offset = index / inodes_per_block;
    uint32_t local_index  = index % inodes_per_block;
    uint32_t block_no = fs.gd.bg_inode_table + block_offset;

//    printf("DEBUG read_inode: ino=%d tbl=%d blk=%d local=%d isz=%d\r\n",
//           ino, fs.gd.bg_inode_table, block_no, local_index, fs.inode_size);

    if (read_block(block_no, tmp_blk) != 0)
        return -1;

    memcpy(out, tmp_blk + local_index * fs.inode_size, sizeof(ext2_inode_t));
    return 0;
}

int ext2_write_inode(uint32_t ino, const ext2_inode_t *in)
{
    if (ino == 0 || !fs_ready)
        return -1;

    uint32_t index = ino - 1;
    uint32_t inodes_per_block = fs.block_size / fs.inode_size;
    uint32_t block_offset = index / inodes_per_block;
    uint32_t local_index  = index % inodes_per_block;
    uint32_t block_no = fs.gd.bg_inode_table + block_offset;

    if (read_block(block_no, tmp_blk) != 0)
        return -1;

    memcpy(tmp_blk + local_index * fs.inode_size, in, sizeof(ext2_inode_t));
    return write_block(block_no, tmp_blk);
}

/* ------------------------------------------------------------------ */
/*  Data block resolution (handles indirect pointers)                 */
/* ------------------------------------------------------------------ */

/* Return the disk block number for logical block `logical` of inode.
 * Returns 0 if not allocated. */
static uint32_t inode_get_block(const ext2_inode_t *inode, uint32_t logical)
{
    uint32_t ptrs_per_block = fs.block_size / 4;

    if (logical < EXT2_NDIR_BLOCKS) {
        return inode->i_block[logical];
    }

    logical -= EXT2_NDIR_BLOCKS;
    if (logical < ptrs_per_block) {
        /* Singly indirect */
        if (inode->i_block[EXT2_IND_BLOCK] == 0)
            return 0;
        uint8_t ind_buf[4096] __attribute__((aligned(4)));
        if (read_block(inode->i_block[EXT2_IND_BLOCK], ind_buf) != 0)
            return 0;
        uint32_t *ptrs = (uint32_t *)ind_buf;
        return ptrs[logical];
    }

    /* Doubly/triply indirect not implemented for this basic version */
    return 0;
}

/* Assign disk block `disk_block` to logical block `logical` of inode.
 * Allocates indirect block if needed. */
static int inode_set_block(ext2_inode_t *inode, uint32_t logical,
                           uint32_t disk_block)
{
    uint32_t ptrs_per_block = fs.block_size / 4;

    if (logical < EXT2_NDIR_BLOCKS) {
        inode->i_block[logical] = disk_block;
        return 0;
    }

    logical -= EXT2_NDIR_BLOCKS;
    if (logical < ptrs_per_block) {
        /* Singly indirect – allocate indirect block if needed */
        if (inode->i_block[EXT2_IND_BLOCK] == 0) {
            uint32_t ind = alloc_block();
            if (ind == 0)
                return -1;
            inode->i_block[EXT2_IND_BLOCK] = ind;
        }
        uint8_t ind_buf[4096] __attribute__((aligned(4)));
        if (read_block(inode->i_block[EXT2_IND_BLOCK], ind_buf) != 0)
            return -1;
        uint32_t *ptrs = (uint32_t *)ind_buf;
        ptrs[logical] = disk_block;
        return write_block(inode->i_block[EXT2_IND_BLOCK], ind_buf);
    }

    return -1;  /* too large */
}

/* ------------------------------------------------------------------ */
/*  File read/write                                                   */
/* ------------------------------------------------------------------ */

int ext2_read_file(uint32_t ino, void *buf, uint32_t offset, uint32_t size)
{
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -1;

    if (offset >= inode.i_size)
        return 0;
    if (offset + size > inode.i_size)
        size = inode.i_size - offset;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t bytes_read = 0;

    while (bytes_read < size) {
        uint32_t logical_block = (offset + bytes_read) / fs.block_size;
        uint32_t block_off     = (offset + bytes_read) % fs.block_size;
        uint32_t chunk = fs.block_size - block_off;
        if (chunk > size - bytes_read)
            chunk = size - bytes_read;

        uint32_t disk_block = inode_get_block(&inode, logical_block);
        if (disk_block == 0) {
            memset(dst + bytes_read, 0, chunk);  /* sparse: read as zeroes */
        } else {
            uint8_t blk_buf[4096] __attribute__((aligned(4)));
            if (read_block(disk_block, blk_buf) != 0)
                return -1;
            memcpy(dst + bytes_read, blk_buf + block_off, chunk);
        }
        bytes_read += chunk;
    }
    return (int)bytes_read;
}

int ext2_write_file(uint32_t ino, const void *buf, uint32_t offset,
                    uint32_t size)
{
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -1;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t bytes_written = 0;

    while (bytes_written < size) {
        uint32_t logical_block = (offset + bytes_written) / fs.block_size;
        uint32_t block_off     = (offset + bytes_written) % fs.block_size;
        uint32_t chunk = fs.block_size - block_off;
        if (chunk > size - bytes_written)
            chunk = size - bytes_written;

        uint32_t disk_block = inode_get_block(&inode, logical_block);
        if (disk_block == 0) {
            disk_block = alloc_block();
            if (disk_block == 0)
                return -1;  /* out of space */
            if (inode_set_block(&inode, logical_block, disk_block) != 0)
                return -1;
            inode.i_blocks += fs.block_size / 512;
        }

        uint8_t blk_buf[4096] __attribute__((aligned(4)));
        if (block_off != 0 || chunk != fs.block_size) {
            /* Partial write – need to read existing block first */
            if (read_block(disk_block, blk_buf) != 0)
                return -1;
        }
        memcpy(blk_buf + block_off, src + bytes_written, chunk);
        if (write_block(disk_block, blk_buf) != 0)
            return -1;

        bytes_written += chunk;
    }

    /* Update size if we extended the file */
    if (offset + size > inode.i_size)
        inode.i_size = offset + size;

    ext2_write_inode(ino, &inode);
    return (int)bytes_written;
}

int ext2_truncate(uint32_t ino)
{
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -1;

    uint32_t ptrs_per_block = fs.block_size / 4;
    uint32_t num_blocks = (inode.i_size + fs.block_size - 1) / fs.block_size;

    /* Free direct blocks */
    for (uint32_t i = 0; i < EXT2_NDIR_BLOCKS && i < num_blocks; i++) {
        if (inode.i_block[i]) {
            free_block(inode.i_block[i]);
            inode.i_block[i] = 0;
        }
    }

    /* Free singly indirect blocks */
    if (inode.i_block[EXT2_IND_BLOCK]) {
        uint8_t ind_buf[4096] __attribute__((aligned(4)));
        if (read_block(inode.i_block[EXT2_IND_BLOCK], ind_buf) == 0) {
            uint32_t *ptrs = (uint32_t *)ind_buf;
            for (uint32_t i = 0; i < ptrs_per_block; i++) {
                if (ptrs[i])
                    free_block(ptrs[i]);
            }
        }
        free_block(inode.i_block[EXT2_IND_BLOCK]);
        inode.i_block[EXT2_IND_BLOCK] = 0;
    }

    inode.i_size = 0;
    inode.i_blocks = 0;
    return ext2_write_inode(ino, &inode);
}

/* ------------------------------------------------------------------ */
/*  Directory operations                                              */
/* ------------------------------------------------------------------ */

uint32_t ext2_lookup(uint32_t dir_ino, const char *name)
{
    ext2_inode_t dir;
    if (ext2_read_inode(dir_ino, &dir) != 0)
        return 0;
    if (!(dir.i_mode & EXT2_S_IFDIR))
        return 0;

    uint32_t name_len = strlen(name);
    uint32_t offset = 0;

    while (offset < dir.i_size) {
        uint32_t logical_block = offset / fs.block_size;
        uint32_t disk_block = inode_get_block(&dir, logical_block);
        if (disk_block == 0)
            break;

        uint8_t blk_buf[4096] __attribute__((aligned(4)));
        if (read_block(disk_block, blk_buf) != 0)
            break;

        uint32_t blk_off = offset % fs.block_size;
        while (blk_off < fs.block_size && (offset + blk_off - (offset - offset % fs.block_size)) < dir.i_size) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(blk_buf + blk_off);
            if (de->rec_len == 0)
                break;
            if (de->inode != 0 && de->name_len == name_len &&
                memcmp(de->name, name, name_len) == 0) {
                return de->inode;
            }
            blk_off += de->rec_len;
        }
        /* Move to next block */
        offset = (offset / fs.block_size + 1) * fs.block_size;
    }
    return 0;
}

int ext2_readdir(uint32_t dir_ino,
                 void (*callback)(const char *name, uint8_t name_len,
                                  uint32_t inode, uint8_t file_type))
{
    ext2_inode_t dir;
    if (ext2_read_inode(dir_ino, &dir) != 0) {
        //printf("DEBUG readdir: read_inode(%d) FAILED\r\n", dir_ino);
        return -1;
    }
    //printf("DEBUG readdir: ino=%d mode=%d size=%d blk0=%d\r\n",
    //       dir_ino, dir.i_mode, dir.i_size, dir.i_block[0]);
    if (!(dir.i_mode & EXT2_S_IFDIR)) {
    //    printf("DEBUG readdir: NOT A DIR (mode=%d, expected bit 0x4000=%d)\r\n",
    //           (int)dir.i_mode, (int)EXT2_S_IFDIR);
        return -1;
    }

    uint32_t offset = 0;

    while (offset < dir.i_size) {
        uint32_t logical_block = offset / fs.block_size;
        uint32_t disk_block = inode_get_block(&dir, logical_block);
        if (disk_block == 0)
            break;

        uint8_t blk_buf[4096] __attribute__((aligned(4)));
        if (read_block(disk_block, blk_buf) != 0)
            break;

        uint32_t blk_off = 0;
        while (blk_off < fs.block_size) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(blk_buf + blk_off);
            if (de->rec_len == 0)
                break;
            if (de->inode != 0) {
                callback(de->name, de->name_len, de->inode, de->file_type);
            }
            blk_off += de->rec_len;
        }
        offset += fs.block_size;
    }
    return 0;
}

/* Add a directory entry to dir_ino. */
static int dir_add_entry(uint32_t dir_ino, uint32_t child_ino,
                         const char *name, uint8_t file_type)
{
    ext2_inode_t dir;
    if (ext2_read_inode(dir_ino, &dir) != 0)
        return -1;

    uint8_t name_len = (uint8_t)strlen(name);
    /* Required space for the new entry (aligned to 4 bytes) */
    uint16_t need = ((uint16_t)(sizeof(uint32_t) + sizeof(uint16_t) +
                     sizeof(uint8_t) + sizeof(uint8_t) + name_len) + 3) & ~3;

    uint32_t num_blocks = (dir.i_size + fs.block_size - 1) / fs.block_size;
    if (num_blocks == 0) num_blocks = 1;

    /* Scan existing blocks for space */
    for (uint32_t b = 0; b < num_blocks; b++) {
        uint32_t disk_block = inode_get_block(&dir, b);
        if (disk_block == 0) {
            /* Allocate the block for the directory */
            disk_block = alloc_block();
            if (disk_block == 0)
                return -1;
            inode_set_block(&dir, b, disk_block);
            dir.i_blocks += fs.block_size / 512;
            dir.i_size = (b + 1) * fs.block_size;
            ext2_write_inode(dir_ino, &dir);

            /* New empty block: create entry spanning entire block */
            uint8_t blk_buf[4096] __attribute__((aligned(4)));
            memset(blk_buf, 0, fs.block_size);
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)blk_buf;
            de->inode = child_ino;
            de->rec_len = (uint16_t)fs.block_size;
            de->name_len = name_len;
            de->file_type = file_type;
            memcpy(de->name, name, name_len);
            return write_block(disk_block, blk_buf);
        }

        uint8_t blk_buf[4096] __attribute__((aligned(4)));
        if (read_block(disk_block, blk_buf) != 0)
            return -1;

        /* Walk entries in this block looking for space */
        uint32_t off = 0;
        while (off < fs.block_size) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(blk_buf + off);
            if (de->rec_len == 0)
                break;

            /* Actual size of this entry */
            uint16_t actual = ((uint16_t)(8 + de->name_len) + 3) & ~3;
            if (de->inode == 0)
                actual = 0;

            uint16_t slack = de->rec_len - actual;
            if (slack >= need) {
                /* Split: shrink current entry, add new one in the gap */
                if (de->inode != 0) {
                    uint16_t old_rec_len = de->rec_len;
                    de->rec_len = actual;
                    ext2_dir_entry_t *new_de =
                        (ext2_dir_entry_t *)(blk_buf + off + actual);
                    new_de->inode = child_ino;
                    new_de->rec_len = old_rec_len - actual;
                    new_de->name_len = name_len;
                    new_de->file_type = file_type;
                    memcpy(new_de->name, name, name_len);
                } else {
                    /* Reuse deleted entry */
                    de->inode = child_ino;
                    de->name_len = name_len;
                    de->file_type = file_type;
                    memcpy(de->name, name, name_len);
                }
                return write_block(disk_block, blk_buf);
            }
            off += de->rec_len;
        }
    }

    /* Need a new block for the directory */
    uint32_t new_b = num_blocks;
    uint32_t new_disk_block = alloc_block();
    if (new_disk_block == 0)
        return -1;
    inode_set_block(&dir, new_b, new_disk_block);
    dir.i_blocks += fs.block_size / 512;
    dir.i_size = (new_b + 1) * fs.block_size;
    ext2_write_inode(dir_ino, &dir);

    uint8_t blk_buf[4096] __attribute__((aligned(4)));
    memset(blk_buf, 0, fs.block_size);
    ext2_dir_entry_t *de = (ext2_dir_entry_t *)blk_buf;
    de->inode = child_ino;
    de->rec_len = (uint16_t)fs.block_size;
    de->name_len = name_len;
    de->file_type = file_type;
    memcpy(de->name, name, name_len);
    return write_block(new_disk_block, blk_buf);
}

/* ------------------------------------------------------------------ */
/*  Create file / directory                                           */
/* ------------------------------------------------------------------ */

uint32_t ext2_create(uint32_t dir_ino, const char *name, uint16_t mode)
{
    /* Check for duplicates */
    if (ext2_lookup(dir_ino, name) != 0) {
        printf("ext2_create: entry '%s' already exists in dir inode %u\n", name, dir_ino);
        return 0;  /* already exists */
    }

    uint32_t ino = alloc_inode();
    if (ino == 0) {
        printf("ext2_create: failed to allocate inode for '%s'\n", name);
        return 0;
    }

    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = mode;
    inode.i_links_count = 1;

    uint8_t ft = EXT2_FT_REG_FILE;

    if (mode & EXT2_S_IFDIR) {
        ft = EXT2_FT_DIR;
        inode.i_links_count = 2;  /* . and parent's link */

        /* Allocate one block for the directory entries (. and ..) */
        uint32_t blk = alloc_block();
        if (blk == 0) {
            free_inode(ino);
            printf("ext2_create: failed to allocate block for directory '%s'\n", name);
            return 0;
        }
        inode.i_block[0] = blk;
        inode.i_size = fs.block_size;
        inode.i_blocks = fs.block_size / 512;

        /* Create . and .. entries */
        uint8_t dir_data[4096] __attribute__((aligned(4)));
        memset(dir_data, 0, fs.block_size);

        ext2_dir_entry_t *dot = (ext2_dir_entry_t *)dir_data;
        dot->inode = ino;
        dot->rec_len = 12;  /* minimum entry size for "." */
        dot->name_len = 1;
        dot->file_type = EXT2_FT_DIR;
        dot->name[0] = '.';

        ext2_dir_entry_t *dotdot = (ext2_dir_entry_t *)(dir_data + 12);
        dotdot->inode = dir_ino;
        dotdot->rec_len = (uint16_t)(fs.block_size - 12);
        dotdot->name_len = 2;
        dotdot->file_type = EXT2_FT_DIR;
        dotdot->name[0] = '.';
        dotdot->name[1] = '.';

        write_block(blk, dir_data);

        /* Increment parent's link count (for ..) */
        ext2_inode_t parent;
        if (ext2_read_inode(dir_ino, &parent) == 0) {
            parent.i_links_count++;
            ext2_write_inode(dir_ino, &parent);
        }

        fs.gd.bg_used_dirs_count++;
        write_group_desc();
    }

    ext2_write_inode(ino, &inode);

    /* Add directory entry in the parent */
    if (dir_add_entry(dir_ino, ino, name, ft) != 0) {
        free_inode(ino);
        printf("ext2_create: failed to add directory entry for '%s' in dir inode %u\n", name, dir_ino);
        return 0;
    }

    return ino;
}

/* ------------------------------------------------------------------ */
/*  Remove file / directory                                           */
/* ------------------------------------------------------------------ */

int ext2_remove(uint32_t dir_ino, const char *name)
{
    ext2_inode_t dir;
    if (ext2_read_inode(dir_ino, &dir) != 0)
        return -1;
    if (!(dir.i_mode & EXT2_S_IFDIR))
        return -1;

    uint8_t name_len = (uint8_t)strlen(name);
    uint32_t offset = 0;

    while (offset < dir.i_size) {
        uint32_t logical_block = offset / fs.block_size;
        uint32_t disk_block = inode_get_block(&dir, logical_block);
        if (disk_block == 0)
            break;

        uint8_t blk_buf[4096] __attribute__((aligned(4)));
        if (read_block(disk_block, blk_buf) != 0)
            break;

        uint32_t prev_off = 0;
        uint32_t blk_off = 0;
        int has_prev = 0;

        while (blk_off < fs.block_size) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(blk_buf + blk_off);
            if (de->rec_len == 0)
                break;

            if (de->inode != 0 && de->name_len == name_len &&
                memcmp(de->name, name, name_len) == 0) {
                /* Found it – read the inode to free its data */
                uint32_t target_ino = de->inode;
                ext2_inode_t target;
                if (ext2_read_inode(target_ino, &target) != 0)
                    return -1;

                /* Don't allow removing non-empty directories */
                if (target.i_mode & EXT2_S_IFDIR) {
                    if (target.i_size > fs.block_size)
                        return -1;  /* has more than . and .. */
                    /* Check if directory has entries other than . and .. */
                    if (target.i_block[0]) {
                        uint8_t dblk[4096] __attribute__((aligned(4)));
                        read_block(target.i_block[0], dblk);
                        uint32_t doff = 0;
                        while (doff < fs.block_size) {
                            ext2_dir_entry_t *dde = (ext2_dir_entry_t *)(dblk + doff);
                            if (dde->rec_len == 0)
                                break;
                            if (dde->inode != 0 &&
                                !(dde->name_len == 1 && dde->name[0] == '.') &&
                                !(dde->name_len == 2 && dde->name[0] == '.' && dde->name[1] == '.')) {
                                return -1;  /* directory not empty */
                            }
                            doff += dde->rec_len;
                        }
                    }

                    fs.gd.bg_used_dirs_count--;
                    write_group_desc();

                    /* Decrement parent link count (for ..) */
                    dir.i_links_count--;
                    ext2_write_inode(dir_ino, &dir);
                }

                /* Decrement link count; free if zero */
                target.i_links_count--;
                if (target.i_links_count == 0) {
                    ext2_truncate(target_ino);
                    free_inode(target_ino);
                }
                ext2_write_inode(target_ino, &target);

                /* Remove directory entry by merging with previous */
                if (has_prev) {
                    ext2_dir_entry_t *prev =
                        (ext2_dir_entry_t *)(blk_buf + prev_off);
                    prev->rec_len += de->rec_len;
                } else {
                    de->inode = 0;
                }
                return write_block(disk_block, blk_buf);
            }

            has_prev = 1;
            prev_off = blk_off;
            blk_off += de->rec_len;
        }
        offset += fs.block_size;
    }
    return -1;  /* not found */
}

/* ------------------------------------------------------------------ */
/*  Format (mkfs.ext2)                                                */
/* ------------------------------------------------------------------ */

int ext2_format(void *ata_drive, uint32_t part_lba, uint32_t total_sectors)
{
    /* We use 1024-byte blocks.  2 sectors per block. */
    uint32_t block_size = 1024;
    uint32_t total_blocks = (total_sectors * ATA_SECTOR_SIZE) / block_size;

    printf("ext2_format: total_sectors=%d, total_blocks=%d\n", total_sectors, total_blocks);

    if (total_blocks < 64)
        return -1;  /* too small */

    /* Cap to what a single block group can handle.
     * One bitmap block = block_size * 8 bits = 8192 blocks max (8 MiB). */
    uint32_t max_blocks_per_group = block_size * 8;
    if (total_blocks > max_blocks_per_group)
        total_blocks = max_blocks_per_group;

    /* Layout for single block group:
     * Block 0: boot block (unused)
     * Block 1: superblock
     * Block 2: block group descriptor table
     * Block 3: block bitmap
     * Block 4: inode bitmap
     * Blocks 5..(5+inode_table_blocks-1): inode table
     * Rest: data blocks
     *
     * Inodes: cap to what one inode bitmap block can track (block_size * 8). */
    uint32_t inodes_count = total_blocks / 4;
    if (inodes_count < 16)
        inodes_count = 16;
    if (inodes_count > max_blocks_per_group)
        inodes_count = max_blocks_per_group;

    uint32_t inode_size = 128;
    uint32_t inodes_per_block = block_size / inode_size;
    uint32_t inode_table_blocks = (inodes_count + inodes_per_block - 1) / inodes_per_block;

    uint32_t first_data_block = 5 + inode_table_blocks;

    /* If metadata is larger than the volume, shrink inodes */
    while (first_data_block >= total_blocks && inodes_count > 16) {
        inodes_count /= 2;
        inode_table_blocks = (inodes_count + inodes_per_block - 1) / inodes_per_block;
        first_data_block = 5 + inode_table_blocks;
    }
    if (first_data_block >= total_blocks)
        return -1;

    /* Build the superblock */
    ext2_superblock_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.s_inodes_count      = inodes_count;
    sb.s_blocks_count      = total_blocks;
    sb.s_r_blocks_count    = 0;
    sb.s_free_blocks_count = total_blocks - first_data_block;
    sb.s_free_inodes_count = inodes_count - 1;  /* inode 1 is reserved */
    sb.s_first_data_block  = 1;  /* for 1024-byte blocks, superblock is in block 1 */
    sb.s_log_block_size    = 0;  /* 1024 << 0 = 1024 */
    sb.s_log_frag_size     = 0;
    sb.s_blocks_per_group  = total_blocks;
    sb.s_frags_per_group   = total_blocks;
    sb.s_inodes_per_group  = inodes_count;
    sb.s_magic             = EXT2_MAGIC;
    sb.s_state             = 1;  /* clean */
    sb.s_errors            = 1;  /* continue on errors */
    sb.s_rev_level         = EXT2_GOOD_OLD_REV;
    sb.s_first_ino         = 11; /* first non-reserved inode */
    sb.s_inode_size        = (uint16_t)inode_size;
    sb.s_max_mnt_count     = 20;
    memcpy(sb.s_volume_name, "MOSS", 4);

    /* Write superblock at byte offset 1024 (sector 2) */
    static uint8_t sb_buf[1024];
    memset(sb_buf, 0, sizeof(sb_buf));
    memcpy(sb_buf, &sb, sizeof(sb));
    if (ata_write_sectors((ata_drive_t *)ata_drive, part_lba + 2, 2, sb_buf) != 0)
        return -1;

    /* Build block group descriptor */
    ext2_group_desc_t gd;
    memset(&gd, 0, sizeof(gd));
    gd.bg_block_bitmap      = 3;
    gd.bg_inode_bitmap       = 4;
    gd.bg_inode_table       = 5;
    gd.bg_free_blocks_count = (uint16_t)sb.s_free_blocks_count;
    gd.bg_free_inodes_count = (uint16_t)sb.s_free_inodes_count;
    gd.bg_used_dirs_count   = 0;

    /* Write BGDT at block 2 */
    static uint8_t bgdt_buf[1024];
    memset(bgdt_buf, 0, sizeof(bgdt_buf));
    memcpy(bgdt_buf, &gd, sizeof(gd));
    uint32_t bgdt_lba = part_lba + 2 * 2;  /* block 2 = sector 4 */
    if (ata_write_sectors((ata_drive_t *)ata_drive, bgdt_lba, 2, bgdt_buf) != 0)
        return -1;

    /* Initialize block bitmap – mark metadata blocks as used */
    static uint8_t bbitmap[1024];
    memset(bbitmap, 0, sizeof(bbitmap));
    for (uint32_t i = 0; i < first_data_block; i++) {
        bbitmap[i / 8] |= (1 << (i % 8));
    }
    /* Also mark blocks beyond total_blocks */
    for (uint32_t i = total_blocks; i < total_blocks + 8; i++) {
        if (i / 8 < sizeof(bbitmap))
            bbitmap[i / 8] |= (1 << (i % 8));
    }
    uint32_t bbitmap_lba = part_lba + 3 * 2;
    if (ata_write_sectors((ata_drive_t *)ata_drive, bbitmap_lba, 2, bbitmap) != 0)
        return -1;

    /* Initialize inode bitmap – mark inode 1 as used (reserved) */
    static uint8_t ibitmap[1024];
    memset(ibitmap, 0, sizeof(ibitmap));
    ibitmap[0] = 0x01;  /* inode 1 reserved */
    uint32_t ibitmap_lba = part_lba + 4 * 2;
    if (ata_write_sectors((ata_drive_t *)ata_drive, ibitmap_lba, 2, ibitmap) != 0)
        return -1;

    /* Zero the inode table */
    static uint8_t zero_buf[1024];
    memset(zero_buf, 0, sizeof(zero_buf));
    for (uint32_t i = 0; i < inode_table_blocks; i++) {
        uint32_t lba = part_lba + (5 + i) * 2;
        if (ata_write_sectors((ata_drive_t *)ata_drive, lba, 2, zero_buf) != 0)
            return -1;
    }

    /* Now set up the context so we can use ext2_create */
    fs.drive            = (struct ata_drive *)ata_drive;
    fs.part_lba         = part_lba;
    fs.block_size       = block_size;
    fs.inodes_per_group = inodes_count;
    fs.inode_size       = inode_size;
    fs.blocks_per_group = total_blocks;
    fs.total_blocks     = total_blocks;
    fs.total_inodes     = inodes_count;
    fs.bgdt_block       = 2;
    memcpy(&fs.sb, &sb, sizeof(sb));
    memcpy(&fs.gd, &gd, sizeof(gd));
    fs_ready = 1;

    printf("Free blocks according to superblock in fs: %d\n", fs.sb.s_free_blocks_count);
    printf("Free inodes according to superblock in fs: %d\n", fs.sb.s_free_inodes_count);

    /* Create the root directory (inode 2) */
    uint32_t root_ino = alloc_inode();
    if (root_ino != EXT2_ROOT_INO) {
        /* Should be inode 2 (inode 1 is reserved) */
        return -1;
    }

    ext2_inode_t root;
    memset(&root, 0, sizeof(root));
    root.i_mode = EXT2_S_IFDIR | EXT2_S_IRWXU | EXT2_S_IRWXG | EXT2_S_IRWXO;
    root.i_links_count = 2;

    /* Allocate one block for root's directory entries */
    uint32_t root_blk = alloc_block();
    if (root_blk == 0)
        return -1;
    root.i_block[0] = root_blk;
    root.i_size = block_size;
    root.i_blocks = block_size / 512;

    ext2_write_inode(root_ino, &root);

    /* Write . and .. for root (both point to root) */
    static uint8_t root_dir_data[1024];
    memset(root_dir_data, 0, sizeof(root_dir_data));

    ext2_dir_entry_t *dot = (ext2_dir_entry_t *)root_dir_data;
    dot->inode = EXT2_ROOT_INO;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0] = '.';

    ext2_dir_entry_t *dotdot = (ext2_dir_entry_t *)(root_dir_data + 12);
    dotdot->inode = EXT2_ROOT_INO;
    dotdot->rec_len = (uint16_t)(block_size - 12);
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    write_block(root_blk, root_dir_data);

    fs.gd.bg_used_dirs_count = 1;
    write_group_desc();

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Mount existing ext2                                               */
/* ------------------------------------------------------------------ */

int ext2_init(void *ata_drive, uint32_t part_lba, int format_if_missing)
{
    fs.drive    = (struct ata_drive *)ata_drive;
    fs.part_lba = part_lba;
    fs_ready    = 0;

    /* Read the superblock */
    if (read_superblock() != 0) {
        printf("[ext2] No superblock found at LBA %u\n", part_lba + 2);
        if (format_if_missing) {
            printf("[ext2] Formatting new ext2 filesystem at LBA %u\n", part_lba);
            return ext2_format(ata_drive, part_lba, 16384);
        }
        return -1;
    }

    if (fs.sb.s_magic != EXT2_MAGIC) {
        printf("[ext2] Invalid magic number\n",
               EXT2_MAGIC, fs.sb.s_magic);
        if (format_if_missing) {
            printf("[ext2] Formatting new ext2 filesystem at LBA %d\n", part_lba);
            /* Determine size from the drive's sector count */
            ata_drive_t *drv = (ata_drive_t *)ata_drive;
            uint32_t sectors = drv->sector_count;
            if (sectors <= part_lba)
                sectors = 16384;  /* 8 MiB fallback */
            else
                sectors -= part_lba;
            /* Cap: format only uses what one block group can handle anyway */
            if (sectors > 16384)
                sectors = 16384;  /* 8 MiB = 8192 blocks of 1024 bytes */
            return ext2_format(ata_drive, part_lba, sectors);
        }
        return -1;
    }

    fs.block_size       = 1024U << fs.sb.s_log_block_size;
    fs.inodes_per_group = fs.sb.s_inodes_per_group;
    fs.inode_size       = (fs.sb.s_rev_level >= 1) ? fs.sb.s_inode_size : 128;
    fs.blocks_per_group = fs.sb.s_blocks_per_group;
    fs.total_blocks     = fs.sb.s_blocks_count;
    fs.total_inodes     = fs.sb.s_inodes_count;
    fs.bgdt_block       = fs.sb.s_first_data_block + 1;

    if (read_group_desc() != 0) {
        printf("[ext2] Failed to read block group descriptor\n");
        return -1;
    }

    printf("DEBUG init: bgdt_blk=%d bbitmap=%d ibitmap=%d itable=%d\r\n",
           fs.bgdt_block, fs.gd.bg_block_bitmap,
           fs.gd.bg_inode_bitmap, fs.gd.bg_inode_table);
    printf("DEBUG init: free_blk=%d free_ino=%d dirs=%d\r\n",
           fs.gd.bg_free_blocks_count, fs.gd.bg_free_inodes_count,
           fs.gd.bg_used_dirs_count);

    fs_ready = 1;
    return 0;
}

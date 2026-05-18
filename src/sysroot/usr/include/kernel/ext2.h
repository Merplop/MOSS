#ifndef _KERNEL_EXT2_H
#define _KERNEL_EXT2_H

#include <stdint.h>
#include <kernel/blkdev.h>

/* ---------- On-disk structures (all fields little-endian on x86) ---------- */

#define EXT2_MAGIC          0xEF53
#define EXT2_ROOT_INO       2      /* Root directory is always inode 2 */
#define EXT2_GOOD_OLD_REV   0
#define EXT2_NAME_LEN       255

/* File type flags stored in inode i_mode (upper 4 bits of 16-bit mode) */
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFDIR  0x4000

/* File type values stored in directory entries (EXT2_FT_*) */
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2

/* Permissions */
#define EXT2_S_IRWXU 0x01C0
#define EXT2_S_IRWXG 0x0038
#define EXT2_S_IRWXO 0x0007

/* Number of direct block pointers in an inode */
#define EXT2_NDIR_BLOCKS  12
#define EXT2_IND_BLOCK    12   /* index of singly-indirect pointer */
#define EXT2_DIND_BLOCK   13   /* doubly-indirect */
#define EXT2_TIND_BLOCK   14   /* triply-indirect */
#define EXT2_N_BLOCKS     15

/* Superblock – located at byte offset 1024 from start of partition */
typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;       /* block size = 1024 << s_log_block_size */
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* -- EXT2_DYNAMIC_REV fields -- */
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    /* padding to 1024 bytes */
    uint8_t  s_padding[820 - 64];
} __attribute__((packed)) ext2_superblock_t;

/* Block Group Descriptor */
typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed)) ext2_group_desc_t;

/* Inode (128 bytes for revision 0) */
typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;         /* count in 512-byte units */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[EXT2_N_BLOCKS];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed)) ext2_inode_t;

/* Directory entry (variable-length) */
typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];           /* variable length, NOT nul-terminated */
} __attribute__((packed)) ext2_dir_entry_t;

/* ---------- In-memory filesystem context ---------- */

/* Maximum number of block groups (32 groups × 8 MiB/group = 256 MiB max) */
#define EXT2_MAX_BLOCK_GROUPS 32

typedef struct {
    blkdev_t        *drive;          /* underlying block device */
    uint32_t         part_lba;      /* LBA of the partition start on disk */
    uint32_t         block_size;
    uint32_t         inodes_per_group;
    uint32_t         inode_size;
    uint32_t         blocks_per_group;
    uint32_t         total_blocks;
    uint32_t         total_inodes;
    uint32_t         bgdt_block;    /* block containing group descriptor table */
    uint32_t         num_groups;    /* number of block groups */
    ext2_superblock_t sb;
    ext2_group_desc_t gd;           /* block-group 0 descriptor (compat) */
    ext2_group_desc_t gds[EXT2_MAX_BLOCK_GROUPS]; /* all group descriptors */
} ext2_fs_t;

/* ---------- Public API ---------- */

/* Initialise the ext2 filesystem on the given block device + partition start.
 * Returns 0 on success, -1 if no valid ext2 superblock found.
 * If `format_if_missing` is non-zero, creates a fresh filesystem. */
int ext2_init(blkdev_t *dev, uint32_t part_lba, int format_if_missing);

/* Format the region as ext2 (mkfs). */
int ext2_format(blkdev_t *dev, uint32_t part_lba, uint32_t total_sectors);

/* Look up a file/dir by name within directory `dir_ino`.
 * Returns the inode number, or 0 on failure. */
uint32_t ext2_lookup(uint32_t dir_ino, const char *name);

/* Read an inode from disk. */
int ext2_read_inode(uint32_t ino, ext2_inode_t *out);

/* Write an inode back to disk. */
int ext2_write_inode(uint32_t ino, const ext2_inode_t *in);

/* Read file data. Returns bytes read, or -1 on error. */
int ext2_read_file(uint32_t ino, void *buf, uint32_t offset, uint32_t size);

/* Read file data using a pre-loaded inode (avoids re-reading inode from disk). */
int ext2_read_file_cached(const ext2_inode_t *inode, void *buf, uint32_t offset, uint32_t size);

/* Write file data. Returns bytes written, or -1 on error.
 * Grows the file and allocates blocks as needed. */
int ext2_write_file(uint32_t ino, const void *buf, uint32_t offset,
                    uint32_t size);

/* Truncate file to zero length, freeing all data blocks. */
int ext2_truncate(uint32_t ino);

/* Create a new file or directory inside `dir_ino`.
 * `mode` should include EXT2_S_IFREG or EXT2_S_IFDIR.
 * Returns the new inode number, or 0 on failure. */
uint32_t ext2_create(uint32_t dir_ino, const char *name, uint16_t mode);

/* Remove a directory entry and free the inode (if link count drops to 0). */
int ext2_remove(uint32_t dir_ino, const char *name);

/* Add a directory entry pointing to an existing inode (for mv/link). */
int ext2_link(uint32_t dir_ino, uint32_t child_ino,
              const char *name, uint8_t file_type);

/* Remove a directory entry WITHOUT freeing the inode (for mv). */
int ext2_unlink(uint32_t dir_ino, const char *name);

/* Update a directory's '..' entry to point to a new parent (for mv). */
int ext2_update_dotdot(uint32_t dir_ino, uint32_t new_parent_ino);

/* List directory contents. Calls `callback(name, name_len, inode, file_type)`
 * for each entry. */
int ext2_readdir(uint32_t dir_ino,
                 void (*callback)(const char *name, uint8_t name_len,
                                  uint32_t inode, uint8_t file_type));

/* Get the global filesystem context (NULL if not initialised). */
ext2_fs_t *ext2_get_fs(void);

#endif /* _KERNEL_EXT2_H */

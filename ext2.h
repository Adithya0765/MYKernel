// ext2.h - ext2 Filesystem Driver for Alteo OS
// Read-write ext2 support with directory traversal and file I/O
#ifndef EXT2_H
#define EXT2_H

#include "stdint.h"
#include "vfs.h"

// ---- ext2 On-Disk Structures ----

// Superblock (located at byte offset 1024 on disk)
#define EXT2_SUPER_OFFSET    1024
#define EXT2_SUPER_MAGIC     0xEF53

typedef struct {
    uint32_t s_inodes_count;        // Total number of inodes
    uint32_t s_blocks_count;        // Total number of blocks
    uint32_t s_r_blocks_count;      // Reserved blocks for superuser
    uint32_t s_free_blocks_count;   // Free blocks
    uint32_t s_free_inodes_count;   // Free inodes
    uint32_t s_first_data_block;    // First data block (0 for 4K blocks, 1 for 1K blocks)
    uint32_t s_log_block_size;      // Block size = 1024 << s_log_block_size
    uint32_t s_log_frag_size;       // Fragment size
    uint32_t s_blocks_per_group;    // Blocks per group
    uint32_t s_frags_per_group;     // Fragments per group
    uint32_t s_inodes_per_group;    // Inodes per group
    uint32_t s_mtime;               // Last mount time
    uint32_t s_wtime;               // Last write time
    uint16_t s_mnt_count;           // Mount count
    uint16_t s_max_mnt_count;       // Max mount count before fsck
    uint16_t s_magic;               // Magic number (0xEF53)
    uint16_t s_state;               // Filesystem state
    uint16_t s_errors;              // Error handling method
    uint16_t s_minor_rev_level;     // Minor revision level
    uint32_t s_lastcheck;           // Last check time
    uint32_t s_checkinterval;       // Check interval
    uint32_t s_creator_os;          // Creator OS
    uint32_t s_rev_level;           // Revision level
    uint16_t s_def_resuid;          // Default UID for reserved blocks
    uint16_t s_def_resgid;          // Default GID for reserved blocks
    // EXT2_DYNAMIC_REV fields
    uint32_t s_first_ino;           // First non-reserved inode
    uint16_t s_inode_size;          // Size of inode structure
    uint16_t s_block_group_nr;      // Block group of this superblock
    uint32_t s_feature_compat;      // Compatible feature set
    uint32_t s_feature_incompat;    // Incompatible feature set
    uint32_t s_feature_ro_compat;   // Read-only compatible feature set
    uint8_t  s_uuid[16];           // Volume UUID
    char     s_volume_name[16];     // Volume name
    char     s_last_mounted[64];    // Path where last mounted
    uint32_t s_algo_bitmap;         // Compression algorithm bitmap
    // Performance hints
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_padding1;
    // Journaling (ext3/ext4)
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    // Padding to 1024 bytes
    uint8_t  s_padding2[788];
} __attribute__((packed)) ext2_superblock_t;

// Block Group Descriptor
typedef struct {
    uint32_t bg_block_bitmap;       // Block bitmap block
    uint32_t bg_inode_bitmap;       // Inode bitmap block
    uint32_t bg_inode_table;        // Inode table start block
    uint16_t bg_free_blocks_count;  // Free blocks in group
    uint16_t bg_free_inodes_count;  // Free inodes in group
    uint16_t bg_used_dirs_count;    // Directories in group
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed)) ext2_group_desc_t;

// Inode
#define EXT2_NDIR_BLOCKS   12
#define EXT2_IND_BLOCK     12   // Singly indirect
#define EXT2_DIND_BLOCK    13   // Doubly indirect
#define EXT2_TIND_BLOCK    14   // Triply indirect
#define EXT2_N_BLOCKS      15

typedef struct {
    uint16_t i_mode;                // File type and permissions
    uint16_t i_uid;                 // Owner UID
    uint32_t i_size;                // File size (lower 32 bits)
    uint32_t i_atime;               // Access time
    uint32_t i_ctime;               // Creation time
    uint32_t i_mtime;               // Modification time
    uint32_t i_dtime;               // Deletion time
    uint16_t i_gid;                 // Group ID
    uint16_t i_links_count;         // Hard links count
    uint32_t i_blocks;              // 512-byte blocks count
    uint32_t i_flags;               // File flags
    uint32_t i_osd1;               // OS-dependent
    uint32_t i_block[EXT2_N_BLOCKS]; // Block pointers
    uint32_t i_generation;          // File generation (for NFS)
    uint32_t i_file_acl;           // File ACL
    uint32_t i_dir_acl;            // Directory ACL (or size_high for files)
    uint32_t i_faddr;              // Fragment address
    uint8_t  i_osd2[12];          // OS-dependent
} __attribute__((packed)) ext2_inode_t;

// Inode types (i_mode upper 4 bits)
#define EXT2_S_IFSOCK  0xC000
#define EXT2_S_IFLNK   0xA000
#define EXT2_S_IFREG   0x8000
#define EXT2_S_IFBLK   0x6000
#define EXT2_S_IFDIR   0x4000
#define EXT2_S_IFCHR   0x2000
#define EXT2_S_IFIFO   0x1000

// Inode permissions
#define EXT2_S_IRUSR   0x0100
#define EXT2_S_IWUSR   0x0080
#define EXT2_S_IXUSR   0x0040
#define EXT2_S_IRGRP   0x0020
#define EXT2_S_IWGRP   0x0010
#define EXT2_S_IXGRP   0x0008
#define EXT2_S_IROTH   0x0004
#define EXT2_S_IWOTH   0x0002
#define EXT2_S_IXOTH   0x0001

// Directory entry
#define EXT2_NAME_LEN  255

typedef struct {
    uint32_t inode;             // Inode number
    uint16_t rec_len;           // Record length
    uint8_t  name_len;          // Name length
    uint8_t  file_type;         // File type
    char     name[EXT2_NAME_LEN + 1]; // Filename (NOT null-terminated on disk)
} __attribute__((packed)) ext2_dir_entry_t;

// Directory entry file types
#define EXT2_FT_UNKNOWN   0
#define EXT2_FT_REG_FILE  1
#define EXT2_FT_DIR       2
#define EXT2_FT_CHRDEV    3
#define EXT2_FT_BLKDEV    4
#define EXT2_FT_FIFO      5
#define EXT2_FT_SOCK      6
#define EXT2_FT_SYMLINK   7

// Well-known inodes
#define EXT2_ROOT_INO     2     // Root directory inode

// ---- ext2 Driver State ----

#define EXT2_MAX_OPEN     16    // Max open files on ext2
#define EXT2_READ_BUF     4096  // Read buffer size

typedef struct {
    int      block_device;      // blkdev ID
    uint32_t block_size;        // Block size (1024, 2048, or 4096)
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t inode_size;
    uint32_t group_count;
    uint32_t first_data_block;
    ext2_superblock_t sb;       // Cached superblock
} ext2_state_t;

// ---- API ----

// Initialize ext2 on a block device, returns 0 on success
int ext2_init(int blkdev_id);

// Get the VFS filesystem operations for ext2
vfs_fs_ops_t* ext2_get_ops(void);

// Get ext2 state (for mounting)
ext2_state_t* ext2_get_state(void);

// Read an inode from the filesystem
int ext2_read_inode(ext2_state_t* state, uint32_t inode_num, ext2_inode_t* inode);

// Read data from a file inode (returns bytes read)
int ext2_read_file(ext2_state_t* state, ext2_inode_t* inode, void* buf,
                   uint32_t offset, uint32_t count);

// List directory entries
int ext2_read_dir(ext2_state_t* state, uint32_t dir_inode,
                  vfs_dirent_t* entries, int max_entries);

// Lookup a name in a directory, returns inode number or 0 on failure
uint32_t ext2_lookup(ext2_state_t* state, uint32_t dir_inode, const char* name);

// Resolve a full path to an inode number
uint32_t ext2_resolve_path(ext2_state_t* state, const char* path);

#endif

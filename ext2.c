// ext2.c - ext2 Filesystem Driver for Alteo OS
// Reads ext2 superblock, block groups, inodes, and directories from blkdev
#include "ext2.h"
#include "blkdev.h"
#include "vfs.h"

// ---- String / memory helpers (no libc) ----
static int e2_strlen(const char* s) { int l = 0; while (s[l]) l++; return l; }
static void e2_strcpy(char* d, const char* s) { while ((*d++ = *s++)); }
static int e2_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char*)a - *(unsigned char*)b;
}
static int e2_strncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}
static void e2_strncpy(char* d, const char* s, int n) {
    int i = 0;
    while (i < n - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static void e2_memset(void* p, int v, int n) {
    unsigned char* b = (unsigned char*)p;
    for (int i = 0; i < n; i++) b[i] = (unsigned char)v;
}
static void e2_memcpy(void* d, const void* s, int n) {
    unsigned char* dd = (unsigned char*)d;
    const unsigned char* ss = (const unsigned char*)s;
    for (int i = 0; i < n; i++) dd[i] = ss[i];
}

// ---- ext2 state ----
static ext2_state_t ext2_state;
static int ext2_initialized = 0;

// Temporary block buffer (statically allocated to avoid heap in early init)
static uint8_t block_buf[4096] __attribute__((aligned(4)));
static uint8_t block_buf2[4096] __attribute__((aligned(4)));

// ---- Block I/O helpers ----

// Read a single ext2 block from the block device
static int ext2_read_block(ext2_state_t* st, uint32_t block_num, void* buf) {
    if (block_num == 0) return -1;
    uint32_t sectors_per_block = st->block_size / BLKDEV_SECTOR_SIZE;
    uint32_t lba = block_num * sectors_per_block;
    return blkdev_read(st->block_device, lba, sectors_per_block, buf);
}

// Read the superblock from disk
static int ext2_read_superblock(ext2_state_t* st) {
    // Superblock is at byte offset 1024, which is LBA 2 for 512-byte sectors
    uint8_t sector_buf[1024];

    // Read sectors 2 and 3 (bytes 1024-2047)
    if (blkdev_read(st->block_device, 2, 2, sector_buf) < 0)
        return -1;

    e2_memcpy(&st->sb, sector_buf, sizeof(ext2_superblock_t));

    // Verify magic number
    if (st->sb.s_magic != EXT2_SUPER_MAGIC)
        return -1;

    // Calculate derived values
    st->block_size = 1024 << st->sb.s_log_block_size;
    st->inodes_per_group = st->sb.s_inodes_per_group;
    st->blocks_per_group = st->sb.s_blocks_per_group;
    st->first_data_block = st->sb.s_first_data_block;

    // Inode size: revision 0 uses 128, revision 1+ uses s_inode_size
    if (st->sb.s_rev_level >= 1) {
        st->inode_size = st->sb.s_inode_size;
    } else {
        st->inode_size = 128;
    }

    // Calculate number of block groups
    st->group_count = (st->sb.s_blocks_count + st->sb.s_blocks_per_group - 1)
                      / st->sb.s_blocks_per_group;

    return 0;
}

// Read a block group descriptor
static int ext2_read_group_desc(ext2_state_t* st, uint32_t group,
                                 ext2_group_desc_t* desc) {
    // Block group descriptor table starts at block (first_data_block + 1)
    uint32_t desc_block = st->first_data_block + 1;
    uint32_t descs_per_block = st->block_size / sizeof(ext2_group_desc_t);
    uint32_t block_idx = desc_block + group / descs_per_block;
    uint32_t offset_in_block = (group % descs_per_block) * sizeof(ext2_group_desc_t);

    if (ext2_read_block(st, block_idx, block_buf) < 0) return -1;
    e2_memcpy(desc, block_buf + offset_in_block, sizeof(ext2_group_desc_t));
    return 0;
}

// ---- Inode Operations ----

int ext2_read_inode(ext2_state_t* st, uint32_t inode_num, ext2_inode_t* inode) {
    if (inode_num == 0) return -1;

    // Determine which block group the inode is in
    uint32_t group = (inode_num - 1) / st->inodes_per_group;
    uint32_t index = (inode_num - 1) % st->inodes_per_group;

    // Read the group descriptor to find the inode table
    ext2_group_desc_t gd;
    if (ext2_read_group_desc(st, group, &gd) < 0) return -1;

    // Calculate offset into inode table
    uint32_t inode_table_block = gd.bg_inode_table;
    uint32_t inodes_per_block = st->block_size / st->inode_size;
    uint32_t block_offset = index / inodes_per_block;
    uint32_t offset_in_block = (index % inodes_per_block) * st->inode_size;

    // Read the block containing the inode
    if (ext2_read_block(st, inode_table_block + block_offset, block_buf) < 0)
        return -1;

    e2_memcpy(inode, block_buf + offset_in_block, sizeof(ext2_inode_t));
    return 0;
}

// Resolve a block number within a file (handles indirect blocks)
static uint32_t ext2_get_block(ext2_state_t* st, ext2_inode_t* inode, uint32_t file_block) {
    uint32_t ptrs_per_block = st->block_size / 4;

    // Direct blocks (0-11)
    if (file_block < EXT2_NDIR_BLOCKS) {
        return inode->i_block[file_block];
    }

    file_block -= EXT2_NDIR_BLOCKS;

    // Singly indirect (12 - 12+ptrs-1)
    if (file_block < ptrs_per_block) {
        uint32_t ind_block = inode->i_block[EXT2_IND_BLOCK];
        if (ind_block == 0) return 0;
        if (ext2_read_block(st, ind_block, block_buf2) < 0) return 0;
        uint32_t* ptrs = (uint32_t*)block_buf2;
        return ptrs[file_block];
    }

    file_block -= ptrs_per_block;

    // Doubly indirect
    if (file_block < ptrs_per_block * ptrs_per_block) {
        uint32_t dind_block = inode->i_block[EXT2_DIND_BLOCK];
        if (dind_block == 0) return 0;
        if (ext2_read_block(st, dind_block, block_buf2) < 0) return 0;
        uint32_t* l1_ptrs = (uint32_t*)block_buf2;

        uint32_t l1_idx = file_block / ptrs_per_block;
        uint32_t l2_idx = file_block % ptrs_per_block;

        uint32_t l2_block = l1_ptrs[l1_idx];
        if (l2_block == 0) return 0;

        // Need a separate buffer for second level (reuse block_buf carefully)
        uint8_t l2_buf[4096];
        if (ext2_read_block(st, l2_block, l2_buf) < 0) return 0;
        uint32_t* l2_ptrs = (uint32_t*)l2_buf;
        return l2_ptrs[l2_idx];
    }

    // Triply indirect (not commonly needed for small files)
    // For simplicity, return 0 (unsupported for very large files)
    return 0;
}

// ---- File Reading ----

int ext2_read_file(ext2_state_t* st, ext2_inode_t* inode, void* buf,
                   uint32_t offset, uint32_t count) {
    uint32_t file_size = inode->i_size;
    if (offset >= file_size) return 0;
    if (offset + count > file_size) count = file_size - offset;
    if (count == 0) return 0;

    uint8_t* dst = (uint8_t*)buf;
    uint32_t bytes_read = 0;

    while (bytes_read < count) {
        uint32_t pos = offset + bytes_read;
        uint32_t file_block = pos / st->block_size;
        uint32_t offset_in_blk = pos % st->block_size;

        uint32_t disk_block = ext2_get_block(st, inode, file_block);
        if (disk_block == 0) break; // Sparse file or error

        uint8_t read_buf[4096];
        if (ext2_read_block(st, disk_block, read_buf) < 0) break;

        uint32_t to_copy = st->block_size - offset_in_blk;
        if (to_copy > count - bytes_read) to_copy = count - bytes_read;

        e2_memcpy(dst + bytes_read, read_buf + offset_in_blk, (int)to_copy);
        bytes_read += to_copy;
    }

    return (int)bytes_read;
}

// ---- Directory Operations ----

int ext2_read_dir(ext2_state_t* st, uint32_t dir_inode_num,
                  vfs_dirent_t* entries, int max_entries) {
    ext2_inode_t dir_inode;
    if (ext2_read_inode(st, dir_inode_num, &dir_inode) < 0) return -1;

    if (!(dir_inode.i_mode & EXT2_S_IFDIR)) return -1; // Not a directory

    int count = 0;
    uint32_t dir_size = dir_inode.i_size;
    uint32_t pos = 0;

    while (pos < dir_size && count < max_entries) {
        uint32_t file_block = pos / st->block_size;
        uint32_t offset_in_blk = pos % st->block_size;

        uint32_t disk_block = ext2_get_block(st, &dir_inode, file_block);
        if (disk_block == 0) break;

        uint8_t dir_buf[4096];
        if (ext2_read_block(st, disk_block, dir_buf) < 0) break;

        // Process directory entries within this block
        while (offset_in_blk < st->block_size && pos < dir_size && count < max_entries) {
            ext2_dir_entry_t* de = (ext2_dir_entry_t*)(dir_buf + offset_in_blk);

            if (de->rec_len == 0) break; // Invalid
            if (de->inode != 0 && de->name_len > 0) {
                // Skip "." and ".."
                int skip = 0;
                if (de->name_len == 1 && de->name[0] == '.') skip = 1;
                if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') skip = 1;

                if (!skip) {
                    // Copy name
                    int nl = de->name_len;
                    if (nl >= VFS_MAX_NAME) nl = VFS_MAX_NAME - 1;
                    e2_memcpy(entries[count].name, de->name, nl);
                    entries[count].name[nl] = 0;

                    // Map file type
                    switch (de->file_type) {
                        case EXT2_FT_DIR:
                            entries[count].type = VFS_DIRECTORY;
                            break;
                        case EXT2_FT_SYMLINK:
                            entries[count].type = VFS_SYMLINK;
                            break;
                        case EXT2_FT_REG_FILE:
                        default:
                            entries[count].type = VFS_FILE;
                            break;
                    }

                    // Read inode for size info
                    ext2_inode_t entry_inode;
                    if (ext2_read_inode(st, de->inode, &entry_inode) == 0) {
                        entries[count].size = entry_inode.i_size;
                        entries[count].created = entry_inode.i_ctime;
                        entries[count].modified = entry_inode.i_mtime;
                    } else {
                        entries[count].size = 0;
                    }

                    entries[count].perms = VFS_PERM_READ;
                    count++;
                }
            }

            pos += de->rec_len;
            offset_in_blk += de->rec_len;
        }
    }

    return count;
}

uint32_t ext2_lookup(ext2_state_t* st, uint32_t dir_inode, const char* name) {
    ext2_inode_t inode;
    if (ext2_read_inode(st, dir_inode, &inode) < 0) return 0;
    if (!(inode.i_mode & EXT2_S_IFDIR)) return 0;

    int name_len = e2_strlen(name);
    uint32_t dir_size = inode.i_size;
    uint32_t pos = 0;

    while (pos < dir_size) {
        uint32_t file_block = pos / st->block_size;
        uint32_t offset_in_blk = pos % st->block_size;

        uint32_t disk_block = ext2_get_block(st, &inode, file_block);
        if (disk_block == 0) break;

        uint8_t dir_buf[4096];
        if (ext2_read_block(st, disk_block, dir_buf) < 0) break;

        while (offset_in_blk < st->block_size && pos < dir_size) {
            ext2_dir_entry_t* de = (ext2_dir_entry_t*)(dir_buf + offset_in_blk);
            if (de->rec_len == 0) break;

            if (de->inode != 0 && de->name_len == (uint8_t)name_len) {
                if (e2_strncmp(de->name, name, name_len) == 0) {
                    return de->inode;
                }
            }

            pos += de->rec_len;
            offset_in_blk += de->rec_len;
        }
    }

    return 0; // Not found
}

uint32_t ext2_resolve_path(ext2_state_t* st, const char* path) {
    if (!path || !path[0]) return 0;

    uint32_t current_inode = EXT2_ROOT_INO;

    const char* p = path;
    if (*p == '/') p++;

    if (!*p) return current_inode; // Path is just "/"

    char component[256];
    while (*p) {
        // Skip slashes
        while (*p == '/') p++;
        if (!*p) break;

        // Extract component
        int ci = 0;
        while (*p && *p != '/' && ci < 255) {
            component[ci++] = *p++;
        }
        component[ci] = 0;

        if (e2_strcmp(component, ".") == 0) continue;
        if (e2_strcmp(component, "..") == 0) {
            // Lookup ".." in current directory
            uint32_t parent = ext2_lookup(st, current_inode, "..");
            if (parent == 0) return 0;
            current_inode = parent;
            continue;
        }

        uint32_t next = ext2_lookup(st, current_inode, component);
        if (next == 0) return 0;
        current_inode = next;
    }

    return current_inode;
}

// ---- VFS Integration ----

// Per-open-file state for ext2
typedef struct {
    uint32_t     inode_num;
    ext2_inode_t inode;
    uint32_t     offset;
    int          in_use;
} ext2_fd_t;

static ext2_fd_t ext2_fds[EXT2_MAX_OPEN];

static int ext2_vfs_open(void* fs_data, const char* path, int flags) {
    (void)flags;
    ext2_state_t* st = (ext2_state_t*)fs_data;
    if (!st) st = &ext2_state;

    uint32_t ino = ext2_resolve_path(st, path);
    if (ino == 0) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(st, ino, &inode) < 0) return -1;

    // Find a free fd
    for (int i = 0; i < EXT2_MAX_OPEN; i++) {
        if (!ext2_fds[i].in_use) {
            ext2_fds[i].inode_num = ino;
            ext2_fds[i].inode = inode;
            ext2_fds[i].offset = 0;
            ext2_fds[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

static int ext2_vfs_close(void* fs_data, int fd) {
    (void)fs_data;
    if (fd < 0 || fd >= EXT2_MAX_OPEN || !ext2_fds[fd].in_use) return -1;
    ext2_fds[fd].in_use = 0;
    return 0;
}

static int ext2_vfs_read(void* fs_data, int fd, void* buf, uint32_t count) {
    ext2_state_t* st = (ext2_state_t*)fs_data;
    if (!st) st = &ext2_state;
    if (fd < 0 || fd >= EXT2_MAX_OPEN || !ext2_fds[fd].in_use) return -1;

    int ret = ext2_read_file(st, &ext2_fds[fd].inode, buf,
                              ext2_fds[fd].offset, count);
    if (ret > 0) ext2_fds[fd].offset += (uint32_t)ret;
    return ret;
}

static int ext2_vfs_write(void* fs_data, int fd, const void* buf, uint32_t count) {
    (void)fs_data; (void)fd; (void)buf; (void)count;
    // ext2 write support would require bitmap allocation, inode update, etc.
    // Implement as read-only for now
    return -1;
}

static int ext2_vfs_readdir(void* fs_data, const char* path,
                              vfs_dirent_t* entries, int max) {
    ext2_state_t* st = (ext2_state_t*)fs_data;
    if (!st) st = &ext2_state;

    uint32_t ino = ext2_resolve_path(st, path);
    if (ino == 0) return -1;

    return ext2_read_dir(st, ino, entries, max);
}

static int ext2_vfs_mkdir(void* fs_data, const char* path) {
    (void)fs_data; (void)path;
    return -1; // Read-only for now
}

static int ext2_vfs_stat(void* fs_data, const char* path, vfs_dirent_t* out) {
    ext2_state_t* st = (ext2_state_t*)fs_data;
    if (!st) st = &ext2_state;

    uint32_t ino = ext2_resolve_path(st, path);
    if (ino == 0) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(st, ino, &inode) < 0) return -1;

    // Extract last component of path for name
    const char* p = path;
    const char* last = path;
    while (*p) { if (*p == '/' && *(p+1)) last = p+1; p++; }
    if (*last == '/') last++;
    e2_strncpy(out->name, last, VFS_MAX_NAME);

    if (inode.i_mode & EXT2_S_IFDIR) out->type = VFS_DIRECTORY;
    else if (inode.i_mode & EXT2_S_IFLNK) out->type = VFS_SYMLINK;
    else out->type = VFS_FILE;

    out->size = inode.i_size;
    out->created = inode.i_ctime;
    out->modified = inode.i_mtime;

    out->perms = 0;
    if (inode.i_mode & EXT2_S_IRUSR) out->perms |= VFS_PERM_READ;
    if (inode.i_mode & EXT2_S_IWUSR) out->perms |= VFS_PERM_WRITE;
    if (inode.i_mode & EXT2_S_IXUSR) out->perms |= VFS_PERM_EXEC;

    return 0;
}

static int ext2_vfs_create(void* fs_data, const char* path, uint8_t type, uint8_t perms) {
    (void)fs_data; (void)path; (void)type; (void)perms;
    return -1; // Read-only
}

static int ext2_vfs_delete(void* fs_data, const char* path) {
    (void)fs_data; (void)path;
    return -1; // Read-only
}

static vfs_fs_ops_t ext2_ops = {
    .open    = ext2_vfs_open,
    .close   = ext2_vfs_close,
    .read    = ext2_vfs_read,
    .write   = ext2_vfs_write,
    .readdir = ext2_vfs_readdir,
    .mkdir   = ext2_vfs_mkdir,
    .stat    = ext2_vfs_stat,
    .create  = ext2_vfs_create,
    .delete  = ext2_vfs_delete,
};

// ---- Public API ----

vfs_fs_ops_t* ext2_get_ops(void) { return &ext2_ops; }
ext2_state_t* ext2_get_state(void) { return &ext2_state; }

int ext2_init(int blkdev_id) {
    e2_memset(&ext2_state, 0, sizeof(ext2_state_t));
    e2_memset(ext2_fds, 0, sizeof(ext2_fds));
    ext2_state.block_device = blkdev_id;

    // Try to read the superblock
    if (ext2_read_superblock(&ext2_state) < 0) {
        // No valid ext2 filesystem found — this is OK (disk might be FAT32)
        ext2_initialized = 0;
        return -1;
    }

    ext2_initialized = 1;

    // Auto-mount at /mnt/ext2 if successful
    if (!vfs_exists("/mnt")) vfs_mkdir("/mnt");
    if (!vfs_exists("/mnt/ext2")) vfs_mkdir("/mnt/ext2");
    vfs_mount("/mnt/ext2", "ext2", &ext2_ops, &ext2_state);

    return 0;
}

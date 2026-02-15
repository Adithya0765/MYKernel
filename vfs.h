// vfs.h - Virtual File System Layer for Alteo OS
#ifndef VFS_H
#define VFS_H

#include "stdint.h"

// File types
#define VFS_FILE        0x01
#define VFS_DIRECTORY   0x02
#define VFS_SYMLINK     0x04
#define VFS_DEVICE      0x08

// Permissions
#define VFS_PERM_READ   0x04
#define VFS_PERM_WRITE  0x02
#define VFS_PERM_EXEC   0x01

// Open flags
#define VFS_O_RDONLY    0x00
#define VFS_O_WRONLY    0x01
#define VFS_O_RDWR      0x02
#define VFS_O_CREAT     0x40
#define VFS_O_TRUNC     0x200
#define VFS_O_APPEND    0x400

// Seek modes
#define VFS_SEEK_SET    0
#define VFS_SEEK_CUR    1
#define VFS_SEEK_END    2

// Limits
#define VFS_MAX_NAME    64
#define VFS_MAX_PATH    256
#define VFS_MAX_FILES   128
#define VFS_MAX_OPEN    32
#define VFS_MAX_CHILDREN 32
#define VFS_MAX_DATA    4096

// Directory entry (returned by readdir)
typedef struct {
    char name[VFS_MAX_NAME];
    uint8_t type;
    uint32_t size;
    uint8_t perms;         // rwx bits (owner)
    uint32_t created;      // tick stamp
    uint32_t modified;     // tick stamp
} vfs_dirent_t;

// VFS node (inode)
typedef struct vfs_node {
    char name[VFS_MAX_NAME];
    uint8_t type;          // VFS_FILE, VFS_DIRECTORY, etc.
    uint8_t perms;         // permission bits
    uint32_t size;         // file size in bytes
    uint32_t created;      // creation tick
    uint32_t modified;     // modification tick
    uint16_t uid;          // owner user id
    uint16_t gid;          // owner group id
    // For files: inline data storage
    uint8_t data[VFS_MAX_DATA];
    // For directories: children
    int child_count;
    int children[VFS_MAX_CHILDREN]; // indices into node table
    int parent;            // parent node index (-1 for root)
    int node_id;           // index in node table
    int in_use;            // is this node allocated
} vfs_node_t;

// Open file descriptor
typedef struct {
    int node_id;           // which vfs_node
    int flags;             // open flags
    uint32_t offset;       // current read/write position
    int in_use;
} vfs_fd_t;

// Initialize VFS with default directory structure
void vfs_init(void);

// File operations
int vfs_open(const char* path, int flags);
int vfs_close(int fd);
int vfs_read(int fd, void* buf, uint32_t count);
int vfs_write(int fd, const void* buf, uint32_t count);
int vfs_seek(int fd, int32_t offset, int whence);
int vfs_tell(int fd);

// File management
int vfs_create(const char* path, uint8_t type, uint8_t perms);
int vfs_delete(const char* path);
int vfs_rename(const char* oldpath, const char* newpath);
int vfs_stat(const char* path, vfs_dirent_t* out);

// Directory operations
int vfs_mkdir(const char* path);
int vfs_rmdir(const char* path);
int vfs_readdir(const char* path, vfs_dirent_t* entries, int max_entries);
int vfs_chdir(const char* path);
const char* vfs_getcwd(void);

// Path utilities
int vfs_exists(const char* path);
int vfs_is_dir(const char* path);
vfs_node_t* vfs_resolve(const char* path);
int vfs_get_node_count(void);

// ---- Filesystem Mount Support ----

#define VFS_MAX_MOUNTS   8
#define VFS_FS_NAME_MAX  16

// Filesystem operations structure (implemented by each filesystem driver)
typedef struct vfs_fs_ops {
    int  (*open)(void* fs_data, const char* path, int flags);
    int  (*close)(void* fs_data, int fd);
    int  (*read)(void* fs_data, int fd, void* buf, uint32_t count);
    int  (*write)(void* fs_data, int fd, const void* buf, uint32_t count);
    int  (*readdir)(void* fs_data, const char* path, vfs_dirent_t* entries, int max);
    int  (*mkdir)(void* fs_data, const char* path);
    int  (*stat)(void* fs_data, const char* path, vfs_dirent_t* out);
    int  (*create)(void* fs_data, const char* path, uint8_t type, uint8_t perms);
    int  (*delete)(void* fs_data, const char* path);
} vfs_fs_ops_t;

// Mount point entry
typedef struct {
    char mount_point[VFS_MAX_PATH];   // Where in the VFS tree this is mounted
    char fs_type[VFS_FS_NAME_MAX];    // Filesystem type name (e.g., "fat32")
    vfs_fs_ops_t* ops;                // Filesystem operations
    void* fs_data;                    // Opaque filesystem-specific data
    int active;                       // Is this mount active?
} vfs_mount_t;

// Mount a filesystem at a given path
// device: device path (e.g., "/dev/hda"), fs_type: "fat32", "ext2", etc.
// mount_point: where to mount in the VFS tree
// ops: filesystem driver operations, fs_data: driver-specific data
int vfs_mount(const char* mount_point, const char* fs_type,
              vfs_fs_ops_t* ops, void* fs_data);

// Unmount a filesystem
int vfs_umount(const char* mount_point);

// Check if a path is under a mount point (returns mount index or -1)
int vfs_find_mount(const char* path);

// Get mount table (for system info display)
const vfs_mount_t* vfs_get_mounts(void);

#endif

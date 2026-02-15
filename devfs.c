// devfs.c - Device Filesystem for Alteo OS
// Implements /dev with null, zero, random, and console devices
#include "devfs.h"
#include "vfs.h"

// ---- String helpers ----
static int dev_strlen(const char* s) { int l = 0; while (s[l]) l++; return l; }
static void dev_strcpy(char* d, const char* s) { while ((*d++ = *s++)); }
static int dev_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char*)a - *(unsigned char*)b;
}
static void dev_strncpy(char* d, const char* s, int n) {
    int i = 0;
    while (i < n - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static void dev_memset(void* p, int v, int n) {
    unsigned char* b = (unsigned char*)p;
    for (int i = 0; i < n; i++) b[i] = (unsigned char)v;
}

// ---- Device registry ----
typedef struct {
    char       name[64];
    int        dev_type;
    int        major;
    int        minor;
    dev_ops_t  ops;
    int        active;
} dev_entry_t;

static dev_entry_t devices[DEVFS_MAX_DEVICES];
static int devfs_initialized = 0;

// ---- Built-in device implementations ----

// /dev/null: reads return 0 (EOF), writes succeed silently
static int null_read(void* data, void* buf, uint32_t count, uint32_t offset) {
    (void)data; (void)buf; (void)count; (void)offset;
    return 0; // EOF
}
static int null_write(void* data, const void* buf, uint32_t count, uint32_t offset) {
    (void)data; (void)buf; (void)offset;
    return (int)count; // Accept everything
}

// /dev/zero: reads return zero bytes, writes succeed silently
static int zero_read(void* data, void* buf, uint32_t count, uint32_t offset) {
    (void)data; (void)offset;
    dev_memset(buf, 0, (int)count);
    return (int)count;
}
static int zero_write(void* data, const void* buf, uint32_t count, uint32_t offset) {
    (void)data; (void)buf; (void)offset;
    return (int)count;
}

// /dev/random and /dev/urandom: returns pseudo-random bytes
static uint32_t rand_seed = 12345;
static uint32_t dev_rand(void) {
    rand_seed = rand_seed * 1103515245 + 12345;
    return (rand_seed >> 16) & 0x7FFF;
}
static int random_read(void* data, void* buf, uint32_t count, uint32_t offset) {
    (void)data; (void)offset;
    uint8_t* b = (uint8_t*)buf;
    for (uint32_t i = 0; i < count; i++) {
        b[i] = (uint8_t)(dev_rand() & 0xFF);
    }
    return (int)count;
}
static int random_write(void* data, const void* buf, uint32_t count, uint32_t offset) {
    (void)data; (void)offset;
    // Writing to random adds entropy (simplified: update seed)
    const uint8_t* b = (const uint8_t*)buf;
    for (uint32_t i = 0; i < count; i++) {
        rand_seed ^= ((uint32_t)b[i] << (i % 4 * 8));
    }
    return (int)count;
}

// /dev/console: writes go to the VFS /dev/console file (for kernel log)
static int console_read(void* data, void* buf, uint32_t count, uint32_t offset) {
    (void)data; (void)buf; (void)count; (void)offset;
    return 0; // No input from console device
}
static int console_write(void* data, const void* buf, uint32_t count, uint32_t offset) {
    (void)data; (void)offset;
    // In a full implementation, this would write to the framebuffer terminal
    // For now, just accept the data
    (void)buf;
    return (int)count;
}

// ---- devfs VFS operations ----

// Internal per-fd state for devfs
#define DEVFS_MAX_FDS 32
typedef struct {
    int dev_idx;    // Index into devices[]
    uint32_t offset;
    int in_use;
} devfs_fd_t;

static devfs_fd_t devfs_fds[DEVFS_MAX_FDS];

static int find_dev_by_name(const char* name) {
    // Strip "/dev/" prefix if present
    const char* basename = name;
    if (name[0] == '/') {
        // Find last component
        const char* p = name;
        const char* last = name;
        while (*p) {
            if (*p == '/' && *(p+1)) last = p + 1;
            p++;
        }
        basename = last;
    }

    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (devices[i].active && dev_strcmp(devices[i].name, basename) == 0)
            return i;
    }
    return -1;
}

static int devfs_open(void* fs_data, const char* path, int flags) {
    (void)fs_data; (void)flags;
    int dev_idx = find_dev_by_name(path);
    if (dev_idx < 0) return -1;

    for (int i = 0; i < DEVFS_MAX_FDS; i++) {
        if (!devfs_fds[i].in_use) {
            devfs_fds[i].dev_idx = dev_idx;
            devfs_fds[i].offset = 0;
            devfs_fds[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

static int devfs_close(void* fs_data, int fd) {
    (void)fs_data;
    if (fd < 0 || fd >= DEVFS_MAX_FDS || !devfs_fds[fd].in_use) return -1;
    devfs_fds[fd].in_use = 0;
    return 0;
}

static int devfs_read(void* fs_data, int fd, void* buf, uint32_t count) {
    (void)fs_data;
    if (fd < 0 || fd >= DEVFS_MAX_FDS || !devfs_fds[fd].in_use) return -1;
    dev_entry_t* dev = &devices[devfs_fds[fd].dev_idx];
    if (!dev->ops.read) return -1;
    int ret = dev->ops.read(dev->ops.dev_data, buf, count, devfs_fds[fd].offset);
    if (ret > 0) devfs_fds[fd].offset += (uint32_t)ret;
    return ret;
}

static int devfs_write(void* fs_data, int fd, const void* buf, uint32_t count) {
    (void)fs_data;
    if (fd < 0 || fd >= DEVFS_MAX_FDS || !devfs_fds[fd].in_use) return -1;
    dev_entry_t* dev = &devices[devfs_fds[fd].dev_idx];
    if (!dev->ops.write) return -1;
    int ret = dev->ops.write(dev->ops.dev_data, buf, count, devfs_fds[fd].offset);
    if (ret > 0) devfs_fds[fd].offset += (uint32_t)ret;
    return ret;
}

static int devfs_readdir(void* fs_data, const char* path, vfs_dirent_t* entries, int max) {
    (void)fs_data; (void)path;
    int count = 0;
    for (int i = 0; i < DEVFS_MAX_DEVICES && count < max; i++) {
        if (devices[i].active) {
            dev_strncpy(entries[count].name, devices[i].name, VFS_MAX_NAME);
            entries[count].type = VFS_DEVICE;
            entries[count].size = 0;
            entries[count].perms = VFS_PERM_READ | VFS_PERM_WRITE;
            count++;
        }
    }
    return count;
}

static int devfs_stat(void* fs_data, const char* path, vfs_dirent_t* out) {
    (void)fs_data;
    int dev_idx = find_dev_by_name(path);
    if (dev_idx < 0) return -1;
    dev_strncpy(out->name, devices[dev_idx].name, VFS_MAX_NAME);
    out->type = VFS_DEVICE;
    out->size = 0;
    out->perms = VFS_PERM_READ | VFS_PERM_WRITE;
    return 0;
}

static int devfs_mkdir(void* fs_data, const char* path) {
    (void)fs_data; (void)path;
    return -1; // Can't create directories in devfs
}

static int devfs_create(void* fs_data, const char* path, uint8_t type, uint8_t perms) {
    (void)fs_data; (void)path; (void)type; (void)perms;
    return -1; // Can't create files in devfs (use devfs_register)
}

static int devfs_delete(void* fs_data, const char* path) {
    (void)fs_data; (void)path;
    return -1; // Can't delete devices through VFS
}

static vfs_fs_ops_t devfs_ops = {
    .open    = devfs_open,
    .close   = devfs_close,
    .read    = devfs_read,
    .write   = devfs_write,
    .readdir = devfs_readdir,
    .mkdir   = devfs_mkdir,
    .stat    = devfs_stat,
    .create  = devfs_create,
    .delete  = devfs_delete,
};

// ---- Public API ----

vfs_fs_ops_t* devfs_get_ops(void) {
    return &devfs_ops;
}

int devfs_register(const char* name, int dev_type, int major, int minor, dev_ops_t* ops) {
    if (!name || !ops) return -1;

    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (!devices[i].active) {
            dev_strncpy(devices[i].name, name, 64);
            devices[i].dev_type = dev_type;
            devices[i].major = major;
            devices[i].minor = minor;
            devices[i].ops = *ops;
            devices[i].active = 1;

            // Create the device node in VFS
            char path[128];
            dev_strcpy(path, "/dev/");
            int plen = dev_strlen(path);
            dev_strncpy(path + plen, name, 128 - plen);
            if (!vfs_exists(path)) {
                vfs_create(path, VFS_DEVICE, VFS_PERM_READ | VFS_PERM_WRITE);
            }

            return i;
        }
    }
    return -1;
}

int devfs_unregister(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (devices[i].active && dev_strcmp(devices[i].name, name) == 0) {
            devices[i].active = 0;
            char path[128];
            dev_strcpy(path, "/dev/");
            int plen = dev_strlen(path);
            dev_strncpy(path + plen, name, 128 - plen);
            vfs_delete(path);
            return 0;
        }
    }
    return -1;
}

void devfs_init(void) {
    dev_memset(devices, 0, sizeof(devices));
    dev_memset(devfs_fds, 0, sizeof(devfs_fds));

    // Register built-in devices

    // /dev/null
    dev_ops_t null_ops = { .read = null_read, .write = null_write, .ioctl = 0, .dev_data = 0 };
    devfs_register("null", DEV_TYPE_CHAR, DEV_MAJOR_MEM, DEV_MINOR_NULL, &null_ops);

    // /dev/zero
    dev_ops_t zero_ops = { .read = zero_read, .write = zero_write, .ioctl = 0, .dev_data = 0 };
    devfs_register("zero", DEV_TYPE_CHAR, DEV_MAJOR_MEM, DEV_MINOR_ZERO, &zero_ops);

    // /dev/random
    dev_ops_t random_ops = { .read = random_read, .write = random_write, .ioctl = 0, .dev_data = 0 };
    devfs_register("random", DEV_TYPE_CHAR, DEV_MAJOR_MEM, DEV_MINOR_RANDOM, &random_ops);

    // /dev/urandom (same as random for now)
    dev_ops_t urandom_ops = { .read = random_read, .write = random_write, .ioctl = 0, .dev_data = 0 };
    devfs_register("urandom", DEV_TYPE_CHAR, DEV_MAJOR_MEM, DEV_MINOR_URANDOM, &urandom_ops);

    // /dev/console
    dev_ops_t console_ops = { .read = console_read, .write = console_write, .ioctl = 0, .dev_data = 0 };
    devfs_register("console", DEV_TYPE_CHAR, DEV_MAJOR_TTY, 1, &console_ops);

    // /dev/tty
    dev_ops_t tty_ops = { .read = console_read, .write = console_write, .ioctl = 0, .dev_data = 0 };
    devfs_register("tty", DEV_TYPE_CHAR, DEV_MAJOR_TTY, 0, &tty_ops);

    // Mount devfs at /dev
    vfs_mount("/dev", "devfs", &devfs_ops, 0);

    devfs_initialized = 1;
}

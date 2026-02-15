// devfs.h - Device Filesystem for Alteo OS
// Provides /dev pseudo-filesystem with null, zero, random, console, etc.
#ifndef DEVFS_H
#define DEVFS_H

#include "stdint.h"
#include "vfs.h"

// Device types
#define DEV_TYPE_CHAR    1
#define DEV_TYPE_BLOCK   2

// Well-known device major numbers
#define DEV_MAJOR_MEM     1    // /dev/null, /dev/zero, /dev/random
#define DEV_MAJOR_TTY     5    // /dev/console, /dev/tty
#define DEV_MAJOR_BLOCK   8    // /dev/sda, etc.

// Device minor numbers for memory devices
#define DEV_MINOR_NULL    1
#define DEV_MINOR_ZERO    3
#define DEV_MINOR_RANDOM  8
#define DEV_MINOR_URANDOM 9

// Device operations (per device)
typedef struct {
    int  (*read)(void* dev_data, void* buf, uint32_t count, uint32_t offset);
    int  (*write)(void* dev_data, const void* buf, uint32_t count, uint32_t offset);
    int  (*ioctl)(void* dev_data, uint64_t request, uint64_t arg);
    void* dev_data;
} dev_ops_t;

// Maximum devices in devfs
#define DEVFS_MAX_DEVICES  32

// Initialize devfs (registers built-in devices and mounts at /dev)
void devfs_init(void);

// Register a new device
int devfs_register(const char* name, int dev_type, int major, int minor, dev_ops_t* ops);

// Unregister a device
int devfs_unregister(const char* name);

// Get the VFS filesystem operations for devfs
vfs_fs_ops_t* devfs_get_ops(void);

#endif

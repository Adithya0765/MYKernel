// blkdev.h - Block Device Layer for Alteo OS
// Provides a common block I/O interface with page cache
// Sits between filesystems (fat32.c) and raw drivers (ata.c)
#ifndef BLKDEV_H
#define BLKDEV_H

#include "stdint.h"

// Block device types
#define BLKDEV_TYPE_ATA         0
#define BLKDEV_TYPE_AHCI        1
#define BLKDEV_TYPE_NVME        2
#define BLKDEV_TYPE_USB_MASS    3
#define BLKDEV_TYPE_RAMDISK     4

// Sector size
#define BLKDEV_SECTOR_SIZE      512

// Cache configuration
#define BLKDEV_CACHE_ENTRIES     256     // Number of cached blocks
#define BLKDEV_CACHE_BLOCK_SIZE  4096    // 4KB per cache block (8 sectors)
#define BLKDEV_SECTORS_PER_BLOCK (BLKDEV_CACHE_BLOCK_SIZE / BLKDEV_SECTOR_SIZE)

// Max block devices
#define BLKDEV_MAX_DEVICES       8

// Block device operations (driver provides these)
typedef struct {
    // Read `count` sectors starting at `lba` into `buf`
    // Returns number of sectors read, or negative on error
    int (*read_sectors)(void* driver_data, uint32_t lba, uint32_t count, void* buf);

    // Write `count` sectors starting at `lba` from `buf`
    // Returns number of sectors written, or negative on error
    int (*write_sectors)(void* driver_data, uint32_t lba, uint32_t count, const void* buf);

    // Flush write cache to disk
    int (*flush)(void* driver_data);
} blkdev_ops_t;

// Block device descriptor
typedef struct {
    int         active;         // Device is registered
    int         type;           // BLKDEV_TYPE_*
    char        name[16];       // e.g., "ata0", "nvme0"
    uint64_t    total_sectors;  // Total size in sectors
    uint32_t    sector_size;    // Usually 512
    void*       driver_data;    // Opaque pointer for the driver
    blkdev_ops_t ops;           // Driver operations
} blkdev_t;

// Cache entry
typedef struct {
    int      valid;             // Entry contains valid data
    int      dirty;             // Entry has been modified (needs writeback)
    int      device_id;         // Which block device
    uint32_t block_lba;         // Starting LBA of this cache block (aligned)
    uint32_t access_count;      // LRU counter
    uint8_t  data[BLKDEV_CACHE_BLOCK_SIZE];
} blkdev_cache_entry_t;

// ---- API ----

// Initialize the block device layer
void blkdev_init(void);

// Register a new block device. Returns device ID (0+) or -1 on error.
int blkdev_register(const char* name, int type, uint64_t total_sectors,
                    uint32_t sector_size, void* driver_data, blkdev_ops_t* ops);

// Unregister a block device
void blkdev_unregister(int device_id);

// Read sectors from a block device (goes through cache)
int blkdev_read(int device_id, uint32_t lba, uint32_t count, void* buf);

// Write sectors to a block device (goes through cache)
int blkdev_write(int device_id, uint32_t lba, uint32_t count, const void* buf);

// Flush a device's dirty cache entries to disk
int blkdev_flush(int device_id);

// Flush all devices
void blkdev_flush_all(void);

// Get a block device descriptor
blkdev_t* blkdev_get(int device_id);

// Get number of registered devices
int blkdev_get_count(void);

// Convenience: read a single sector
int blkdev_read_sector(int device_id, uint32_t lba, void* buf);

// Convenience: write a single sector
int blkdev_write_sector(int device_id, uint32_t lba, const void* buf);

#endif

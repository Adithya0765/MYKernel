// blkdev.c - Block Device Layer for Alteo OS
// Common block I/O interface with a simple page cache
#include "blkdev.h"
#include "ata.h"

// ---- Helpers ----
static void blk_memset(void* dst, int val, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

static void blk_memcpy(void* dst, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

static int blk_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (int)*(const unsigned char*)a - (int)*(const unsigned char*)b;
}

static void blk_strncpy(char* dst, const char* src, int n) {
    int i;
    for (i = 0; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

// ---- Storage ----
static blkdev_t devices[BLKDEV_MAX_DEVICES];
static int device_count = 0;

static blkdev_cache_entry_t cache[BLKDEV_CACHE_ENTRIES];
static uint32_t cache_access_counter = 0;

// ---- ATA Backend ----
// Adapter functions to bridge ATA driver to blkdev_ops_t interface

typedef struct {
    int drive_index;    // ATA drive index (0-3)
} ata_blkdev_data_t;

static ata_blkdev_data_t ata_data[4]; // Up to 4 ATA drives

static int ata_blkdev_read(void* driver_data, uint32_t lba, uint32_t count, void* buf) {
    ata_blkdev_data_t* data = (ata_blkdev_data_t*)driver_data;
    // ATA driver uses uint8_t count, so we may need multiple calls
    uint8_t* dst = (uint8_t*)buf;
    uint32_t remaining = count;
    uint32_t current_lba = lba;

    while (remaining > 0) {
        uint8_t chunk = (remaining > 255) ? 255 : (uint8_t)remaining;
        int ret = ata_read_sectors(data->drive_index, current_lba, chunk, dst);
        if (ret < 0) return ret;
        dst += chunk * BLKDEV_SECTOR_SIZE;
        current_lba += chunk;
        remaining -= chunk;
    }
    return (int)count;
}

static int ata_blkdev_write(void* driver_data, uint32_t lba, uint32_t count, const void* buf) {
    ata_blkdev_data_t* data = (ata_blkdev_data_t*)driver_data;
    const uint8_t* src = (const uint8_t*)buf;
    uint32_t remaining = count;
    uint32_t current_lba = lba;

    while (remaining > 0) {
        uint8_t chunk = (remaining > 255) ? 255 : (uint8_t)remaining;
        int ret = ata_write_sectors(data->drive_index, current_lba, chunk, src);
        if (ret < 0) return ret;
        src += chunk * BLKDEV_SECTOR_SIZE;
        current_lba += chunk;
        remaining -= chunk;
    }
    return (int)count;
}

static int ata_blkdev_flush(void* driver_data) {
    ata_blkdev_data_t* data = (ata_blkdev_data_t*)driver_data;
    return ata_flush(data->drive_index);
}

// ---- Cache Operations ----

// Find a cache entry for the given device and block LBA
static blkdev_cache_entry_t* cache_find(int device_id, uint32_t block_lba) {
    for (int i = 0; i < BLKDEV_CACHE_ENTRIES; i++) {
        if (cache[i].valid && cache[i].device_id == device_id &&
            cache[i].block_lba == block_lba) {
            cache[i].access_count = ++cache_access_counter;
            return &cache[i];
        }
    }
    return (blkdev_cache_entry_t*)0;
}

// Find or evict a cache entry (LRU)
static blkdev_cache_entry_t* cache_alloc(int device_id, uint32_t block_lba) {
    // First look for an empty slot
    for (int i = 0; i < BLKDEV_CACHE_ENTRIES; i++) {
        if (!cache[i].valid) {
            cache[i].valid = 1;
            cache[i].dirty = 0;
            cache[i].device_id = device_id;
            cache[i].block_lba = block_lba;
            cache[i].access_count = ++cache_access_counter;
            return &cache[i];
        }
    }

    // Evict LRU entry
    int lru_idx = 0;
    uint32_t lru_count = cache[0].access_count;
    for (int i = 1; i < BLKDEV_CACHE_ENTRIES; i++) {
        if (cache[i].access_count < lru_count) {
            lru_count = cache[i].access_count;
            lru_idx = i;
        }
    }

    blkdev_cache_entry_t* entry = &cache[lru_idx];

    // Writeback if dirty
    if (entry->dirty && entry->device_id >= 0 && entry->device_id < BLKDEV_MAX_DEVICES) {
        blkdev_t* dev = &devices[entry->device_id];
        if (dev->active && dev->ops.write_sectors) {
            dev->ops.write_sectors(dev->driver_data, entry->block_lba,
                                   BLKDEV_SECTORS_PER_BLOCK, entry->data);
        }
    }

    entry->valid = 1;
    entry->dirty = 0;
    entry->device_id = device_id;
    entry->block_lba = block_lba;
    entry->access_count = ++cache_access_counter;
    return entry;
}

// Read a cache block from disk
static int cache_fill(blkdev_cache_entry_t* entry) {
    if (!entry || entry->device_id < 0 || entry->device_id >= BLKDEV_MAX_DEVICES) return -1;
    blkdev_t* dev = &devices[entry->device_id];
    if (!dev->active || !dev->ops.read_sectors) return -1;

    int ret = dev->ops.read_sectors(dev->driver_data, entry->block_lba,
                                     BLKDEV_SECTORS_PER_BLOCK, entry->data);
    return (ret >= 0) ? 0 : -1;
}

// ---- Public API ----

void blkdev_init(void) {
    blk_memset(devices, 0, sizeof(devices));
    blk_memset(cache, 0, sizeof(cache));
    device_count = 0;
    cache_access_counter = 0;

    // Auto-register detected ATA drives
    int ata_count = ata_get_drive_count();
    for (int i = 0; i < 4 && device_count < BLKDEV_MAX_DEVICES; i++) {
        ata_drive_t* drv = ata_get_drive(i);
        if (drv && drv->present) {
            ata_data[i].drive_index = i;
            blkdev_ops_t ops;
            ops.read_sectors = ata_blkdev_read;
            ops.write_sectors = ata_blkdev_write;
            ops.flush = ata_blkdev_flush;

            char name[16] = "ata0";
            name[3] = '0' + (char)i;
            blkdev_register(name, BLKDEV_TYPE_ATA, drv->sectors, BLKDEV_SECTOR_SIZE,
                           &ata_data[i], &ops);
        }
    }
    (void)ata_count;
}

int blkdev_register(const char* name, int type, uint64_t total_sectors,
                    uint32_t sector_size, void* driver_data, blkdev_ops_t* ops) {
    if (device_count >= BLKDEV_MAX_DEVICES || !ops) return -1;

    // Find free slot
    int id = -1;
    for (int i = 0; i < BLKDEV_MAX_DEVICES; i++) {
        if (!devices[i].active) { id = i; break; }
    }
    if (id < 0) return -1;

    blkdev_t* dev = &devices[id];
    blk_memset(dev, 0, sizeof(blkdev_t));
    dev->active = 1;
    dev->type = type;
    blk_strncpy(dev->name, name, 16);
    dev->total_sectors = total_sectors;
    dev->sector_size = sector_size ? sector_size : BLKDEV_SECTOR_SIZE;
    dev->driver_data = driver_data;
    dev->ops = *ops;

    device_count++;
    return id;
}

void blkdev_unregister(int device_id) {
    if (device_id < 0 || device_id >= BLKDEV_MAX_DEVICES) return;
    if (!devices[device_id].active) return;

    // Flush cache entries for this device
    blkdev_flush(device_id);

    // Invalidate cache entries
    for (int i = 0; i < BLKDEV_CACHE_ENTRIES; i++) {
        if (cache[i].valid && cache[i].device_id == device_id) {
            cache[i].valid = 0;
        }
    }

    devices[device_id].active = 0;
    device_count--;
}

int blkdev_read(int device_id, uint32_t lba, uint32_t count, void* buf) {
    if (device_id < 0 || device_id >= BLKDEV_MAX_DEVICES) return -1;
    blkdev_t* dev = &devices[device_id];
    if (!dev->active || !dev->ops.read_sectors) return -1;
    if (!buf || count == 0) return 0;

    uint8_t* dst = (uint8_t*)buf;
    uint32_t sectors_read = 0;

    while (sectors_read < count) {
        uint32_t current_lba = lba + sectors_read;

        // Align to cache block boundary
        uint32_t block_lba = (current_lba / BLKDEV_SECTORS_PER_BLOCK) * BLKDEV_SECTORS_PER_BLOCK;
        uint32_t offset_in_block = current_lba - block_lba;

        // Try cache
        blkdev_cache_entry_t* entry = cache_find(device_id, block_lba);
        if (!entry) {
            // Cache miss: allocate and fill
            entry = cache_alloc(device_id, block_lba);
            if (!entry) {
                // Cache allocation failed, read directly
                return dev->ops.read_sectors(dev->driver_data, lba, count, buf);
            }
            if (cache_fill(entry) < 0) {
                entry->valid = 0;
                return dev->ops.read_sectors(dev->driver_data, lba, count, buf);
            }
        }

        // Copy from cache to output buffer
        uint32_t sectors_available = BLKDEV_SECTORS_PER_BLOCK - offset_in_block;
        uint32_t sectors_to_copy = count - sectors_read;
        if (sectors_to_copy > sectors_available) sectors_to_copy = sectors_available;

        blk_memcpy(dst, entry->data + offset_in_block * BLKDEV_SECTOR_SIZE,
                   sectors_to_copy * BLKDEV_SECTOR_SIZE);

        dst += sectors_to_copy * BLKDEV_SECTOR_SIZE;
        sectors_read += sectors_to_copy;
    }

    return (int)count;
}

int blkdev_write(int device_id, uint32_t lba, uint32_t count, const void* buf) {
    if (device_id < 0 || device_id >= BLKDEV_MAX_DEVICES) return -1;
    blkdev_t* dev = &devices[device_id];
    if (!dev->active || !dev->ops.write_sectors) return -1;
    if (!buf || count == 0) return 0;

    const uint8_t* src = (const uint8_t*)buf;
    uint32_t sectors_written = 0;

    while (sectors_written < count) {
        uint32_t current_lba = lba + sectors_written;
        uint32_t block_lba = (current_lba / BLKDEV_SECTORS_PER_BLOCK) * BLKDEV_SECTORS_PER_BLOCK;
        uint32_t offset_in_block = current_lba - block_lba;

        // Find or create cache entry
        blkdev_cache_entry_t* entry = cache_find(device_id, block_lba);
        if (!entry) {
            entry = cache_alloc(device_id, block_lba);
            if (!entry) {
                // Bypass cache
                return dev->ops.write_sectors(dev->driver_data, lba, count, buf);
            }
            // If we're not writing a full block, read existing data first
            uint32_t sectors_to_write = count - sectors_written;
            if (offset_in_block != 0 || sectors_to_write < BLKDEV_SECTORS_PER_BLOCK) {
                if (cache_fill(entry) < 0) {
                    blk_memset(entry->data, 0, BLKDEV_CACHE_BLOCK_SIZE);
                }
            }
        }

        // Copy from input buffer to cache
        uint32_t sectors_available = BLKDEV_SECTORS_PER_BLOCK - offset_in_block;
        uint32_t sectors_to_copy = count - sectors_written;
        if (sectors_to_copy > sectors_available) sectors_to_copy = sectors_available;

        blk_memcpy(entry->data + offset_in_block * BLKDEV_SECTOR_SIZE,
                   src, sectors_to_copy * BLKDEV_SECTOR_SIZE);

        entry->dirty = 1;
        src += sectors_to_copy * BLKDEV_SECTOR_SIZE;
        sectors_written += sectors_to_copy;
    }

    return (int)count;
}

int blkdev_flush(int device_id) {
    if (device_id < 0 || device_id >= BLKDEV_MAX_DEVICES) return -1;
    blkdev_t* dev = &devices[device_id];
    if (!dev->active) return -1;

    for (int i = 0; i < BLKDEV_CACHE_ENTRIES; i++) {
        if (cache[i].valid && cache[i].dirty && cache[i].device_id == device_id) {
            if (dev->ops.write_sectors) {
                dev->ops.write_sectors(dev->driver_data, cache[i].block_lba,
                                       BLKDEV_SECTORS_PER_BLOCK, cache[i].data);
            }
            cache[i].dirty = 0;
        }
    }

    if (dev->ops.flush) {
        dev->ops.flush(dev->driver_data);
    }

    return 0;
}

void blkdev_flush_all(void) {
    for (int i = 0; i < BLKDEV_MAX_DEVICES; i++) {
        if (devices[i].active) {
            blkdev_flush(i);
        }
    }
}

blkdev_t* blkdev_get(int device_id) {
    if (device_id < 0 || device_id >= BLKDEV_MAX_DEVICES) return (blkdev_t*)0;
    if (!devices[device_id].active) return (blkdev_t*)0;
    return &devices[device_id];
}

int blkdev_get_count(void) {
    return device_count;
}

int blkdev_read_sector(int device_id, uint32_t lba, void* buf) {
    return blkdev_read(device_id, lba, 1, buf);
}

int blkdev_write_sector(int device_id, uint32_t lba, const void* buf) {
    return blkdev_write(device_id, lba, 1, buf);
}

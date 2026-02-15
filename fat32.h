// fat32.h - FAT32 File System for Alteo OS
#ifndef FAT32_H
#define FAT32_H

#include "stdint.h"

// FAT32 constants
#define FAT32_SECTOR_SIZE     512
#define FAT32_MAX_FILENAME    11     // 8.3 format
#define FAT32_LONG_NAME       255
#define FAT32_ENTRIES_PER_SEC 16     // 512/32

// Directory entry attributes
#define FAT32_ATTR_READONLY   0x01
#define FAT32_ATTR_HIDDEN     0x02
#define FAT32_ATTR_SYSTEM     0x04
#define FAT32_ATTR_VOLUME_ID  0x08
#define FAT32_ATTR_DIRECTORY  0x10
#define FAT32_ATTR_ARCHIVE    0x20
#define FAT32_ATTR_LONG_NAME  0x0F

// Special cluster values
#define FAT32_CLUSTER_FREE    0x00000000
#define FAT32_CLUSTER_BAD     0x0FFFFFF7
#define FAT32_CLUSTER_EOF     0x0FFFFFF8

// Boot sector / BPB
typedef struct {
    uint8_t  jump[3];
    char     oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;      // 0 for FAT32
    uint16_t total_sectors_16;      // 0 for FAT32
    uint8_t  media_type;
    uint16_t fat_size_16;           // 0 for FAT32
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    // FAT32 specific
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_serial;
    char     volume_label[11];
    char     fs_type[8];
} __attribute__((packed)) fat32_bpb_t;

// Directory entry (32 bytes)
typedef struct {
    char     name[11];             // 8.3 filename
    uint8_t  attr;                 // attributes
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_hi;     // High 16 bits of cluster
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_lo;     // Low 16 bits of cluster
    uint32_t file_size;
} __attribute__((packed)) fat32_dir_entry_t;

// FAT32 filesystem info
typedef struct {
    int drive;                     // ATA drive index
    uint32_t fat_start_lba;        // LBA of first FAT
    uint32_t data_start_lba;       // LBA of data region
    uint32_t root_cluster;         // Root directory cluster
    uint32_t sectors_per_cluster;
    uint32_t fat_size;             // FAT size in sectors
    uint32_t total_clusters;
    uint32_t bytes_per_cluster;
    int mounted;                   // Is filesystem mounted?
} fat32_fs_t;

// Initialize FAT32 (attempt to mount from ATA drive)
int fat32_init(int drive);

// Check if FAT32 is mounted
int fat32_is_mounted(void);

// Read a file's contents
int fat32_read_file(const char* path, void* buf, uint32_t max_size);

// Write to a file
int fat32_write_file(const char* path, const void* buf, uint32_t size);

// List directory contents
int fat32_list_dir(const char* path, fat32_dir_entry_t* entries, int max);

// Create a file
int fat32_create_file(const char* path);

// Create a directory
int fat32_mkdir(const char* path);

// Delete a file
int fat32_delete(const char* path);

// Get filesystem info
fat32_fs_t* fat32_get_info(void);

#endif

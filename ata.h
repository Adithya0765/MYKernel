// ata.h - ATA/IDE Hard Disk Driver for Alteo OS
#ifndef ATA_H
#define ATA_H

#include "stdint.h"

// ATA I/O Ports (Primary bus)
#define ATA_PRIMARY_DATA        0x1F0
#define ATA_PRIMARY_ERROR       0x1F1
#define ATA_PRIMARY_SECCOUNT    0x1F2
#define ATA_PRIMARY_LBA_LO     0x1F3
#define ATA_PRIMARY_LBA_MID    0x1F4
#define ATA_PRIMARY_LBA_HI     0x1F5
#define ATA_PRIMARY_DRIVE      0x1F6
#define ATA_PRIMARY_STATUS     0x1F7
#define ATA_PRIMARY_COMMAND    0x1F7

// ATA I/O Ports (Secondary bus)
#define ATA_SECONDARY_DATA      0x170
#define ATA_SECONDARY_ERROR     0x171
#define ATA_SECONDARY_SECCOUNT  0x172
#define ATA_SECONDARY_LBA_LO   0x173
#define ATA_SECONDARY_LBA_MID  0x174
#define ATA_SECONDARY_LBA_HI   0x175
#define ATA_SECONDARY_DRIVE    0x176
#define ATA_SECONDARY_STATUS   0x177
#define ATA_SECONDARY_COMMAND  0x177

// ATA Control ports
#define ATA_PRIMARY_CTRL       0x3F6
#define ATA_SECONDARY_CTRL     0x376

// ATA Status bits
#define ATA_SR_BSY   0x80   // Busy
#define ATA_SR_DRDY  0x40   // Drive ready
#define ATA_SR_DF    0x20   // Drive fault
#define ATA_SR_DSC   0x10   // Drive seek complete
#define ATA_SR_DRQ   0x08   // Data request ready
#define ATA_SR_CORR  0x04   // Corrected data
#define ATA_SR_IDX   0x02   // Index
#define ATA_SR_ERR   0x01   // Error

// ATA Commands
#define ATA_CMD_READ_PIO       0x20
#define ATA_CMD_WRITE_PIO      0x30
#define ATA_CMD_CACHE_FLUSH    0xE7
#define ATA_CMD_IDENTIFY       0xEC

// Drive select
#define ATA_MASTER  0xE0
#define ATA_SLAVE   0xF0

// Sector size
#define ATA_SECTOR_SIZE  512

// Drive info structure
typedef struct {
    int present;           // Drive detected?
    int bus;               // 0=primary, 1=secondary
    int drive;             // 0=master, 1=slave
    uint32_t sectors;      // Total sector count
    uint32_t size_mb;      // Size in MB
    char model[41];        // Model string
    char serial[21];       // Serial number
} ata_drive_t;

// Initialize ATA subsystem, detect drives
void ata_init(void);

// Read sectors from drive
// buf must be at least count*512 bytes
int ata_read_sectors(int drive, uint32_t lba, uint8_t count, void* buf);

// Write sectors to drive
int ata_write_sectors(int drive, uint32_t lba, uint8_t count, const void* buf);

// Get drive info
ata_drive_t* ata_get_drive(int index);
int ata_get_drive_count(void);

// Flush write cache
int ata_flush(int drive);

#endif

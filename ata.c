// ata.c - ATA/IDE Hard Disk Driver for Alteo OS
// PIO mode driver for reading/writing sectors
#include "ata.h"

// Port I/O (duplicated here for standalone compilation)
static inline uint8_t ata_inb(uint16_t port) {
    uint8_t ret;
    __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void ata_outb(uint16_t port, uint8_t val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t ata_inw(uint16_t port) {
    uint16_t ret;
    __asm__ __volatile__("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void ata_outw(uint16_t port, uint16_t val) {
    __asm__ __volatile__("outw %0, %1" : : "a"(val), "Nd"(port));
}

// String helper
static void ata_memset(void* p, int v, int n) {
    unsigned char* b = (unsigned char*)p;
    for (int i = 0; i < n; i++) b[i] = (unsigned char)v;
}

// Drive table (up to 4: primary master/slave, secondary master/slave)
static ata_drive_t drives[4];
static int drive_count = 0;

// Wait for BSY to clear
static void ata_wait(uint16_t io_base) {
    for (int i = 0; i < 4; i++) ata_inb(io_base + 7); // 400ns delay
    while (ata_inb(io_base + 7) & ATA_SR_BSY);
}

// Wait for DRQ
static int ata_wait_drq(uint16_t io_base) {
    int timeout = 100000;
    while (timeout-- > 0) {
        uint8_t status = ata_inb(io_base + 7);
        if (status & ATA_SR_ERR) return -1;
        if (status & ATA_SR_DF) return -1;
        if (status & ATA_SR_DRQ) return 0;
    }
    return -1; // timeout
}

// Software reset
static void ata_soft_reset(uint16_t ctrl_port) {
    ata_outb(ctrl_port, 0x04);
    for (int i = 0; i < 5000; i++) ata_inb(ctrl_port); // delay
    ata_outb(ctrl_port, 0x00);
    for (int i = 0; i < 5000; i++) ata_inb(ctrl_port); // delay
}

// Identify drive
static int ata_identify(uint16_t io_base, uint16_t ctrl_port, int slave, ata_drive_t* drv) {
    (void)ctrl_port;
    ata_memset(drv, 0, sizeof(ata_drive_t));

    // Select drive
    ata_outb(io_base + 6, slave ? ATA_SLAVE : ATA_MASTER);
    for (int i = 0; i < 20; i++) ata_inb(io_base + 7); // 400ns delay

    // Check if drive exists
    uint8_t status = ata_inb(io_base + 7);
    if (status == 0xFF || status == 0x00) return 0; // no drive

    // Send IDENTIFY
    ata_outb(io_base + 2, 0);
    ata_outb(io_base + 3, 0);
    ata_outb(io_base + 4, 0);
    ata_outb(io_base + 5, 0);
    ata_outb(io_base + 7, ATA_CMD_IDENTIFY);

    status = ata_inb(io_base + 7);
    if (status == 0) return 0; // no drive

    // Wait for BSY to clear
    int timeout = 100000;
    while (timeout-- > 0) {
        status = ata_inb(io_base + 7);
        if (!(status & ATA_SR_BSY)) break;
    }
    if (timeout <= 0) return 0;

    // Check for ATAPI (not ATA)
    if (ata_inb(io_base + 4) != 0 || ata_inb(io_base + 5) != 0) return 0;

    // Wait for DRQ or ERR
    timeout = 100000;
    while (timeout-- > 0) {
        status = ata_inb(io_base + 7);
        if (status & ATA_SR_ERR) return 0;
        if (status & ATA_SR_DRQ) break;
    }
    if (timeout <= 0) return 0;

    // Read 256 words of identify data
    uint16_t ident[256];
    for (int i = 0; i < 256; i++) {
        ident[i] = ata_inw(io_base);
    }

    // Parse identify data
    drv->present = 1;

    // Model string (words 27-46, byte-swapped)
    for (int i = 0; i < 20; i++) {
        drv->model[i * 2] = (char)(ident[27 + i] >> 8);
        drv->model[i * 2 + 1] = (char)(ident[27 + i] & 0xFF);
    }
    drv->model[40] = 0;
    // Trim trailing spaces
    for (int i = 39; i >= 0 && drv->model[i] == ' '; i--) drv->model[i] = 0;

    // Serial (words 10-19)
    for (int i = 0; i < 10; i++) {
        drv->serial[i * 2] = (char)(ident[10 + i] >> 8);
        drv->serial[i * 2 + 1] = (char)(ident[10 + i] & 0xFF);
    }
    drv->serial[20] = 0;
    for (int i = 19; i >= 0 && drv->serial[i] == ' '; i--) drv->serial[i] = 0;

    // LBA48 sector count (words 100-103)
    if (ident[83] & (1 << 10)) {
        drv->sectors = (uint32_t)ident[100] | ((uint32_t)ident[101] << 16);
    } else {
        // LBA28 sector count (words 60-61)
        drv->sectors = (uint32_t)ident[60] | ((uint32_t)ident[61] << 16);
    }

    drv->size_mb = drv->sectors / 2048; // sectors * 512 / (1024*1024)

    return 1;
}

void ata_init(void) {
    ata_memset(drives, 0, sizeof(drives));
    drive_count = 0;

    // Soft reset both channels
    ata_soft_reset(ATA_PRIMARY_CTRL);
    ata_soft_reset(ATA_SECONDARY_CTRL);

    // Probe all 4 possible drives
    uint16_t io_bases[] = {ATA_PRIMARY_DATA, ATA_PRIMARY_DATA,
                           ATA_SECONDARY_DATA, ATA_SECONDARY_DATA};
    uint16_t ctrl_ports[] = {ATA_PRIMARY_CTRL, ATA_PRIMARY_CTRL,
                             ATA_SECONDARY_CTRL, ATA_SECONDARY_CTRL};
    int slaves[] = {0, 1, 0, 1};

    for (int i = 0; i < 4; i++) {
        if (ata_identify(io_bases[i], ctrl_ports[i], slaves[i], &drives[i])) {
            drives[i].bus = (i >= 2) ? 1 : 0;
            drives[i].drive = slaves[i];
            drive_count++;
        }
    }
}

int ata_read_sectors(int drive, uint32_t lba, uint8_t count, void* buf) {
    if (drive < 0 || drive >= 4 || !drives[drive].present) return -1;
    if (count == 0) return 0;

    uint16_t io_base = (drives[drive].bus == 0) ? ATA_PRIMARY_DATA : ATA_SECONDARY_DATA;

    ata_wait(io_base);

    // Select drive + LBA28 addressing
    ata_outb(io_base + 6, (drives[drive].drive ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    ata_outb(io_base + 1, 0x00); // features
    ata_outb(io_base + 2, count);
    ata_outb(io_base + 3, (uint8_t)(lba & 0xFF));
    ata_outb(io_base + 4, (uint8_t)((lba >> 8) & 0xFF));
    ata_outb(io_base + 5, (uint8_t)((lba >> 16) & 0xFF));
    ata_outb(io_base + 7, ATA_CMD_READ_PIO);

    uint16_t* wbuf = (uint16_t*)buf;

    for (int s = 0; s < count; s++) {
        if (ata_wait_drq(io_base) < 0) return -1;
        for (int i = 0; i < 256; i++) {
            wbuf[s * 256 + i] = ata_inw(io_base);
        }
    }

    return count;
}

int ata_write_sectors(int drive, uint32_t lba, uint8_t count, const void* buf) {
    if (drive < 0 || drive >= 4 || !drives[drive].present) return -1;
    if (count == 0) return 0;

    uint16_t io_base = (drives[drive].bus == 0) ? ATA_PRIMARY_DATA : ATA_SECONDARY_DATA;

    ata_wait(io_base);

    ata_outb(io_base + 6, (drives[drive].drive ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
    ata_outb(io_base + 1, 0x00);
    ata_outb(io_base + 2, count);
    ata_outb(io_base + 3, (uint8_t)(lba & 0xFF));
    ata_outb(io_base + 4, (uint8_t)((lba >> 8) & 0xFF));
    ata_outb(io_base + 5, (uint8_t)((lba >> 16) & 0xFF));
    ata_outb(io_base + 7, ATA_CMD_WRITE_PIO);

    const uint16_t* wbuf = (const uint16_t*)buf;

    for (int s = 0; s < count; s++) {
        if (ata_wait_drq(io_base) < 0) return -1;
        for (int i = 0; i < 256; i++) {
            ata_outw(io_base, wbuf[s * 256 + i]);
        }
    }

    // Flush cache
    ata_outb(io_base + 7, ATA_CMD_CACHE_FLUSH);
    ata_wait(io_base);

    return count;
}

ata_drive_t* ata_get_drive(int index) {
    if (index < 0 || index >= 4) return (ata_drive_t*)0;
    if (!drives[index].present) return (ata_drive_t*)0;
    return &drives[index];
}

int ata_get_drive_count(void) {
    return drive_count;
}

int ata_flush(int drive) {
    if (drive < 0 || drive >= 4 || !drives[drive].present) return -1;
    uint16_t io_base = (drives[drive].bus == 0) ? ATA_PRIMARY_DATA : ATA_SECONDARY_DATA;
    ata_outb(io_base + 7, ATA_CMD_CACHE_FLUSH);
    ata_wait(io_base);
    return 0;
}

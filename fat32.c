// fat32.c - FAT32 File System for Alteo OS
// Provides FAT32 filesystem support on top of ATA driver
#include "fat32.h"
#include "ata.h"

// String helpers
static void fat_memset(void* p, int v, int n) {
    unsigned char* b = (unsigned char*)p;
    for (int i = 0; i < n; i++) b[i] = (unsigned char)v;
}
static void fat_memcpy(void* dst, const void* src, int n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (int i = 0; i < n; i++) d[i] = s[i];
}
static int fat_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char*)a - *(unsigned char*)b;
}
static int fat_strlen(const char* s) { int l = 0; while (s[l]) l++; return l; }
static void fat_strncpy(char* d, const char* s, int n) {
    int i = 0;
    while (i < n - 1 && s[i]) { d[i] = s[i]; i++; }
    d[i] = 0;
}

// Filesystem state
static fat32_fs_t fs;
static uint8_t sector_buf[FAT32_SECTOR_SIZE];

// Convert cluster number to LBA
static uint32_t cluster_to_lba(uint32_t cluster) {
    return fs.data_start_lba + (cluster - 2) * fs.sectors_per_cluster;
}

// Read a single entry from the FAT
static uint32_t fat32_read_fat_entry(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fs.fat_start_lba + (fat_offset / FAT32_SECTOR_SIZE);
    uint32_t entry_offset = fat_offset % FAT32_SECTOR_SIZE;

    if (ata_read_sectors(fs.drive, fat_sector, 1, sector_buf) < 0) {
        return FAT32_CLUSTER_BAD;
    }

    uint32_t val = *(uint32_t*)(sector_buf + entry_offset);
    return val & 0x0FFFFFFF;
}

// Convert 8.3 name from directory entry to readable string
static void fat32_decode_name(const fat32_dir_entry_t* entry, char* out) {
    int i, j = 0;

    // Base name (8 chars, space-padded)
    for (i = 0; i < 8 && entry->name[i] != ' '; i++) {
        char c = entry->name[i];
        // Convert to lowercase
        if (c >= 'A' && c <= 'Z') c += 32;
        out[j++] = c;
    }

    // Extension (3 chars, space-padded)
    if (entry->name[8] != ' ') {
        out[j++] = '.';
        for (i = 8; i < 11 && entry->name[i] != ' '; i++) {
            char c = entry->name[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            out[j++] = c;
        }
    }

    out[j] = 0;
}

// Encode a filename to 8.3 format
static void fat32_encode_name(const char* name, char* out) {
    fat_memset(out, ' ', 11);

    int i = 0, j = 0;
    // Find the dot
    int dot = -1;
    for (i = 0; name[i]; i++) {
        if (name[i] == '.') dot = i;
    }

    // Base name
    int limit = (dot >= 0) ? dot : fat_strlen(name);
    if (limit > 8) limit = 8;
    for (i = 0; i < limit; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[j++] = c;
    }

    // Extension
    if (dot >= 0) {
        j = 8;
        for (i = dot + 1; name[i] && j < 11; i++) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            out[j++] = c;
        }
    }
}

int fat32_init(int drive) {
    fat_memset(&fs, 0, sizeof(fs));
    fs.drive = drive;
    fs.mounted = 0;

    // Read boot sector
    if (ata_read_sectors(drive, 0, 1, sector_buf) < 0) {
        return -1;
    }

    fat32_bpb_t* bpb = (fat32_bpb_t*)sector_buf;

    // Validate
    if (bpb->bytes_per_sector != 512) return -1;
    if (bpb->num_fats < 1 || bpb->num_fats > 2) return -1;
    if (bpb->fat_size_32 == 0) return -1;

    // Calculate filesystem geometry
    fs.sectors_per_cluster = bpb->sectors_per_cluster;
    fs.fat_start_lba = bpb->reserved_sectors;
    fs.fat_size = bpb->fat_size_32;
    fs.data_start_lba = bpb->reserved_sectors + (bpb->num_fats * bpb->fat_size_32);
    fs.root_cluster = bpb->root_cluster;
    fs.bytes_per_cluster = fs.sectors_per_cluster * FAT32_SECTOR_SIZE;

    uint32_t data_sectors = bpb->total_sectors_32 - fs.data_start_lba;
    fs.total_clusters = data_sectors / fs.sectors_per_cluster;

    fs.mounted = 1;
    return 0;
}

int fat32_is_mounted(void) {
    return fs.mounted;
}

int fat32_read_file(const char* path, void* buf, uint32_t max_size) {
    if (!fs.mounted) return -1;
    (void)path; (void)buf; (void)max_size;
    // TODO: Traverse directory tree to find file, then read cluster chain
    return -1;
}

int fat32_write_file(const char* path, const void* buf, uint32_t size) {
    if (!fs.mounted) return -1;
    (void)path; (void)buf; (void)size;
    // TODO: Find or create file entry, allocate clusters, write data
    return -1;
}

int fat32_list_dir(const char* path, fat32_dir_entry_t* entries, int max) {
    if (!fs.mounted) return -1;
    (void)path;

    // For now, list root directory
    uint32_t cluster = fs.root_cluster;
    int count = 0;

    while (cluster < FAT32_CLUSTER_EOF && count < max) {
        uint32_t lba = cluster_to_lba(cluster);

        for (uint32_t s = 0; s < fs.sectors_per_cluster && count < max; s++) {
            if (ata_read_sectors(fs.drive, lba + s, 1, sector_buf) < 0) {
                return count;
            }

            fat32_dir_entry_t* dir = (fat32_dir_entry_t*)sector_buf;
            for (int i = 0; i < FAT32_ENTRIES_PER_SEC && count < max; i++) {
                if (dir[i].name[0] == 0x00) return count;    // End of directory
                if ((uint8_t)dir[i].name[0] == 0xE5) continue; // Deleted
                if (dir[i].attr == FAT32_ATTR_LONG_NAME) continue; // LFN
                if (dir[i].attr & FAT32_ATTR_VOLUME_ID) continue;

                fat_memcpy(&entries[count], &dir[i], sizeof(fat32_dir_entry_t));
                count++;
            }
        }

        cluster = fat32_read_fat_entry(cluster);
    }

    return count;
}

int fat32_create_file(const char* path) {
    if (!fs.mounted) return -1;
    (void)path;
    // TODO: Implement file creation
    return -1;
}

int fat32_mkdir(const char* path) {
    if (!fs.mounted) return -1;
    (void)path;
    // TODO: Implement directory creation
    return -1;
}

int fat32_delete(const char* path) {
    if (!fs.mounted) return -1;
    (void)path;
    // TODO: Implement file deletion
    return -1;
}

fat32_fs_t* fat32_get_info(void) {
    return &fs;
}

// pe.h - PE (Portable Executable) Loader for Alteo OS
// Parses and loads Windows PE/PE32+ binaries for .exe support
#ifndef PE_H
#define PE_H

#include "stdint.h"
#include "vmm.h"

// DOS Header magic
#define PE_DOS_MAGIC        0x5A4D      // "MZ"
#define PE_SIGNATURE        0x00004550  // "PE\0\0"

// Machine types
#define PE_MACHINE_AMD64    0x8664

// PE characteristics
#define PE_CHAR_EXECUTABLE  0x0002
#define PE_CHAR_LARGE_ADDR  0x0020
#define PE_CHAR_DLL         0x2000

// Optional header magic
#define PE_OPT_MAGIC_PE32   0x010B
#define PE_OPT_MAGIC_PE32P  0x020B  // PE32+ (64-bit)

// Section characteristics
#define PE_SCN_CODE         0x00000020  // Contains code
#define PE_SCN_INITIALIZED  0x00000040  // Contains initialized data
#define PE_SCN_UNINITIALIZED 0x00000080 // Contains uninitialized data (.bss)
#define PE_SCN_MEM_EXECUTE  0x20000000  // Section is executable
#define PE_SCN_MEM_READ     0x40000000  // Section is readable
#define PE_SCN_MEM_WRITE    0x80000000  // Section is writable

// Data directory indices
#define PE_DIR_EXPORT       0
#define PE_DIR_IMPORT       1
#define PE_DIR_RESOURCE     2
#define PE_DIR_EXCEPTION    3
#define PE_DIR_SECURITY     4
#define PE_DIR_BASERELOC    5
#define PE_DIR_DEBUG        6
#define PE_DIR_TLS          9
#define PE_DIR_IAT          12
#define PE_NUM_DIRS         16

// Relocation types
#define PE_REL_ABSOLUTE     0   // No relocation
#define PE_REL_DIR64        10  // 64-bit address

// DOS Header (first 64 bytes of a PE file)
typedef struct {
    uint16_t e_magic;       // Magic "MZ"
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    uint32_t e_lfanew;      // Offset to PE signature
} __attribute__((packed)) pe_dos_header_t;

// COFF File Header
typedef struct {
    uint16_t machine;
    uint16_t number_of_sections;
    uint32_t time_date_stamp;
    uint32_t pointer_to_symbol_table;
    uint32_t number_of_symbols;
    uint16_t size_of_optional_header;
    uint16_t characteristics;
} __attribute__((packed)) pe_coff_header_t;

// Data Directory entry
typedef struct {
    uint32_t virtual_address;
    uint32_t size;
} __attribute__((packed)) pe_data_dir_t;

// Optional Header (PE32+, 64-bit)
typedef struct {
    uint16_t magic;
    uint8_t  major_linker_version;
    uint8_t  minor_linker_version;
    uint32_t size_of_code;
    uint32_t size_of_initialized_data;
    uint32_t size_of_uninitialized_data;
    uint32_t address_of_entry_point;
    uint32_t base_of_code;
    uint64_t image_base;
    uint32_t section_alignment;
    uint32_t file_alignment;
    uint16_t major_os_version;
    uint16_t minor_os_version;
    uint16_t major_image_version;
    uint16_t minor_image_version;
    uint16_t major_subsystem_version;
    uint16_t minor_subsystem_version;
    uint32_t win32_version_value;
    uint32_t size_of_image;
    uint32_t size_of_headers;
    uint32_t checksum;
    uint16_t subsystem;
    uint16_t dll_characteristics;
    uint64_t size_of_stack_reserve;
    uint64_t size_of_stack_commit;
    uint64_t size_of_heap_reserve;
    uint64_t size_of_heap_commit;
    uint32_t loader_flags;
    uint32_t number_of_rva_and_sizes;
    pe_data_dir_t data_directory[PE_NUM_DIRS];
} __attribute__((packed)) pe_opt_header_64_t;

// Section Header
typedef struct {
    char     name[8];
    uint32_t virtual_size;
    uint32_t virtual_address;
    uint32_t size_of_raw_data;
    uint32_t pointer_to_raw_data;
    uint32_t pointer_to_relocations;
    uint32_t pointer_to_linenumbers;
    uint16_t number_of_relocations;
    uint16_t number_of_linenumbers;
    uint32_t characteristics;
} __attribute__((packed)) pe_section_header_t;

// Import Directory Table entry
typedef struct {
    uint32_t import_lookup_table_rva;
    uint32_t time_date_stamp;
    uint32_t forwarder_chain;
    uint32_t name_rva;
    uint32_t import_address_table_rva;
} __attribute__((packed)) pe_import_dir_t;

// Base Relocation Block
typedef struct {
    uint32_t page_rva;
    uint32_t block_size;
    // Followed by variable-length array of uint16_t entries
} __attribute__((packed)) pe_reloc_block_t;

// PE load result
typedef struct {
    uint64_t entry_point;       // Absolute entry point address
    uint64_t image_base;        // Base address of loaded image
    uint64_t image_size;        // Size of loaded image
    int      is_dll;            // 1 if DLL, 0 if EXE
    int      success;           // 1 if loaded successfully
} pe_load_result_t;

// Validate a PE file header
int pe_validate(const void* data, uint64_t size);

// Load a PE binary from a buffer into a process address space
int pe_load(const void* data, uint64_t size, pte_t* pml4, pe_load_result_t* result);

// Load a PE binary from a VFS file descriptor
int pe_load_from_fd(int fd, pte_t* pml4, pe_load_result_t* result);

#endif

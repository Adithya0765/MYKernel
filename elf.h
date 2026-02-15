// elf.h - ELF64 Loader for Alteo OS
// Parses and loads ELF64 binaries into process address spaces
#ifndef ELF_H
#define ELF_H

#include "stdint.h"
#include "vmm.h"

// ELF Magic
#define ELF_MAGIC       0x464C457F   // "\x7FELF" as uint32_t

// ELF Class
#define ELFCLASS64      2

// ELF Data encoding
#define ELFDATA2LSB     1   // Little-endian

// ELF Object types
#define ET_EXEC         2   // Executable file
#define ET_DYN          3   // Shared object / PIE executable

// ELF Machine types
#define EM_X86_64       62  // AMD x86-64

// Program header types
#define PT_NULL         0
#define PT_LOAD         1   // Loadable segment
#define PT_DYNAMIC      2   // Dynamic linking info
#define PT_INTERP       3   // Program interpreter
#define PT_NOTE         4
#define PT_PHDR         6   // Program header table

// Program header flags
#define PF_X            0x1  // Executable
#define PF_W            0x2  // Writable
#define PF_R            0x4  // Readable

// Section header types
#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_STRTAB      3
#define SHT_NOBITS      8   // .bss section

// ELF64 Header
typedef struct {
    uint8_t  e_ident[16];    // Magic number and other info
    uint16_t e_type;         // Object file type
    uint16_t e_machine;      // Architecture
    uint32_t e_version;      // Object file version
    uint64_t e_entry;        // Entry point virtual address
    uint64_t e_phoff;        // Program header table file offset
    uint64_t e_shoff;        // Section header table file offset
    uint32_t e_flags;        // Processor-specific flags
    uint16_t e_ehsize;       // ELF header size
    uint16_t e_phentsize;    // Program header table entry size
    uint16_t e_phnum;        // Program header table entry count
    uint16_t e_shentsize;    // Section header table entry size
    uint16_t e_shnum;        // Section header table entry count
    uint16_t e_shstrndx;     // Section name string table index
} __attribute__((packed)) Elf64_Ehdr;

// ELF64 Program Header
typedef struct {
    uint32_t p_type;         // Segment type
    uint32_t p_flags;        // Segment flags
    uint64_t p_offset;       // Segment file offset
    uint64_t p_vaddr;        // Segment virtual address
    uint64_t p_paddr;        // Segment physical address
    uint64_t p_filesz;       // Segment size in file
    uint64_t p_memsz;        // Segment size in memory
    uint64_t p_align;        // Segment alignment
} __attribute__((packed)) Elf64_Phdr;

// ELF64 Section Header
typedef struct {
    uint32_t sh_name;        // Section name (string table index)
    uint32_t sh_type;        // Section type
    uint64_t sh_flags;       // Section flags
    uint64_t sh_addr;        // Section virtual address
    uint64_t sh_offset;      // Section file offset
    uint64_t sh_size;        // Section size
    uint32_t sh_link;        // Link to another section
    uint32_t sh_info;        // Additional section info
    uint64_t sh_addralign;   // Section alignment
    uint64_t sh_entsize;     // Entry size (if section holds table)
} __attribute__((packed)) Elf64_Shdr;

// ELF load result
typedef struct {
    uint64_t entry_point;    // Entry point address
    uint64_t load_base;      // Lowest mapped virtual address
    uint64_t load_end;       // Highest mapped virtual address + size
    uint64_t bss_start;      // Start of BSS segment
    uint64_t bss_end;        // End of BSS segment
    int      success;        // 1 if loaded successfully, 0 otherwise
} elf_load_result_t;

// Validate an ELF64 header (check magic, class, machine)
int elf_validate(const void* data, uint64_t size);

// Load an ELF64 binary from a buffer into a process address space
// data: pointer to the full ELF file in memory
// size: size of the ELF file
// pml4: process's page table (PML4)
// result: output structure with entry point and memory layout
int elf_load(const void* data, uint64_t size, pte_t* pml4, elf_load_result_t* result);

// Load an ELF64 binary from a VFS file descriptor
// fd: open file descriptor to read the ELF from
// pml4: process's page table
// result: output structure
int elf_load_from_fd(int fd, pte_t* pml4, elf_load_result_t* result);

#endif

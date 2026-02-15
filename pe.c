// pe.c - PE (Portable Executable) Loader for Alteo OS
// Parses PE/PE32+ binaries and maps sections into process address spaces
#include "pe.h"
#include "vmm.h"
#include "pmm.h"
#include "vfs.h"

// ---------- Helpers ----------

static void pe_memset(void* ptr, int val, uint64_t size) {
    uint8_t* p = (uint8_t*)ptr;
    for (uint64_t i = 0; i < size; i++) p[i] = (uint8_t)val;
}

static void pe_memcpy(void* dst, const void* src, uint64_t size) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < size; i++) d[i] = s[i];
}

// Convert PE section characteristics to VMM page flags
static uint64_t pe_flags_to_vmm(uint32_t characteristics) {
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (characteristics & PE_SCN_MEM_WRITE) flags |= VMM_FLAG_WRITABLE;
    // Note: NX bit handling omitted for now
    return flags;
}

// ---------- Validation ----------

int pe_validate(const void* data, uint64_t size) {
    if (!data || size < sizeof(pe_dos_header_t)) return 0;

    const pe_dos_header_t* dos = (const pe_dos_header_t*)data;

    // Check DOS magic "MZ"
    if (dos->e_magic != PE_DOS_MAGIC) return 0;

    // Check PE signature offset is within file
    if (dos->e_lfanew == 0 || (uint64_t)dos->e_lfanew + 4 > size) return 0;

    // Check PE signature "PE\0\0"
    const uint8_t* file = (const uint8_t*)data;
    uint32_t pe_sig = *(uint32_t*)(file + dos->e_lfanew);
    if (pe_sig != PE_SIGNATURE) return 0;

    // Check COFF header
    uint64_t coff_offset = dos->e_lfanew + 4;
    if (coff_offset + sizeof(pe_coff_header_t) > size) return 0;

    const pe_coff_header_t* coff = (const pe_coff_header_t*)(file + coff_offset);

    // Check machine type (must be x86_64)
    if (coff->machine != PE_MACHINE_AMD64) return 0;

    // Check optional header exists and is PE32+
    uint64_t opt_offset = coff_offset + sizeof(pe_coff_header_t);
    if (opt_offset + sizeof(pe_opt_header_64_t) > size) return 0;

    const pe_opt_header_64_t* opt = (const pe_opt_header_64_t*)(file + opt_offset);
    if (opt->magic != PE_OPT_MAGIC_PE32P) return 0;

    return 1;
}

// ---------- Relocation processing ----------

static int pe_apply_relocations(const void* data, uint64_t size,
                                uint64_t image_base, uint64_t actual_base,
                                const pe_opt_header_64_t* opt) {
    if (image_base == actual_base) return 0; // No relocation needed

    // Check if relocation directory exists
    if (opt->number_of_rva_and_sizes <= PE_DIR_BASERELOC) return 0;
    if (opt->data_directory[PE_DIR_BASERELOC].size == 0) return -1; // Needs reloc but none

    int64_t delta = (int64_t)(actual_base - image_base);
    const uint8_t* file = (const uint8_t*)data;

    uint32_t reloc_rva = opt->data_directory[PE_DIR_BASERELOC].virtual_address;
    uint32_t reloc_size = opt->data_directory[PE_DIR_BASERELOC].size;

    // Walk the base relocation directory
    uint32_t offset = 0;
    while (offset < reloc_size) {
        const pe_reloc_block_t* block = (const pe_reloc_block_t*)(file + reloc_rva + offset);

        if (block->block_size == 0) break;
        if (block->block_size < sizeof(pe_reloc_block_t)) break;

        uint32_t num_entries = (block->block_size - sizeof(pe_reloc_block_t)) / 2;
        const uint16_t* entries = (const uint16_t*)((uint8_t*)block + sizeof(pe_reloc_block_t));

        for (uint32_t i = 0; i < num_entries; i++) {
            uint16_t entry = entries[i];
            uint8_t  type = (entry >> 12) & 0xF;
            uint16_t off  = entry & 0xFFF;

            if (type == PE_REL_ABSOLUTE) continue; // Padding, skip

            if (type == PE_REL_DIR64) {
                // 64-bit relocation: add delta to the address at this location
                uint64_t reloc_addr = actual_base + block->page_rva + off;
                uint64_t* ptr = (uint64_t*)reloc_addr;
                *ptr += (uint64_t)delta;
            }
            // Other relocation types not yet supported
        }

        offset += block->block_size;
    }

    (void)size;
    return 0;
}

// ---------- Loading ----------

int pe_load(const void* data, uint64_t size, pte_t* pml4, pe_load_result_t* result) {
    if (!data || !pml4 || !result) return -1;

    pe_memset(result, 0, sizeof(pe_load_result_t));

    // Validate PE header
    if (!pe_validate(data, size)) return -1;

    const uint8_t* file = (const uint8_t*)data;
    const pe_dos_header_t* dos = (const pe_dos_header_t*)data;

    uint64_t coff_offset = dos->e_lfanew + 4;
    const pe_coff_header_t* coff = (const pe_coff_header_t*)(file + coff_offset);

    uint64_t opt_offset = coff_offset + sizeof(pe_coff_header_t);
    const pe_opt_header_64_t* opt = (const pe_opt_header_64_t*)(file + opt_offset);

    uint64_t image_base = opt->image_base;
    uint64_t image_size = opt->size_of_image;
    uint32_t section_align = opt->section_alignment;

    // Use the preferred image base
    uint64_t actual_base = image_base;

    result->image_base = actual_base;
    result->image_size = image_size;
    result->is_dll = (coff->characteristics & PE_CHAR_DLL) ? 1 : 0;

    // Map pages for the entire image
    uint64_t page_start = actual_base & ~(VMM_PAGE_SIZE - 1);
    uint64_t page_end = (actual_base + image_size + VMM_PAGE_SIZE - 1) & ~(VMM_PAGE_SIZE - 1);

    for (uint64_t page = page_start; page < page_end; page += VMM_PAGE_SIZE) {
        void* phys = pmm_alloc_block();
        if (!phys) return -1;
        pe_memset(phys, 0, VMM_PAGE_SIZE);
        if (vmm_map_page(pml4, page, (uint64_t)phys,
                         VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER) < 0) {
            pmm_free_block(phys);
            return -1;
        }
    }

    // Copy PE headers to the base address
    uint32_t headers_size = opt->size_of_headers;
    if (headers_size > size) headers_size = (uint32_t)size;
    pe_memcpy((void*)actual_base, file, headers_size);

    // Copy sections
    uint64_t sections_offset = opt_offset + coff->size_of_optional_header;
    for (uint16_t i = 0; i < coff->number_of_sections; i++) {
        if (sections_offset + sizeof(pe_section_header_t) > size) break;

        const pe_section_header_t* sec =
            (const pe_section_header_t*)(file + sections_offset + (uint64_t)i * sizeof(pe_section_header_t));

        uint64_t sec_va = actual_base + sec->virtual_address;
        uint32_t sec_rawsize = sec->size_of_raw_data;
        uint32_t sec_rawptr  = sec->pointer_to_raw_data;

        // Copy raw data if present
        if (sec_rawsize > 0 && sec_rawptr > 0) {
            if ((uint64_t)sec_rawptr + sec_rawsize <= size) {
                pe_memcpy((void*)sec_va, file + sec_rawptr, sec_rawsize);
            }
        }

        // Zero remaining bytes (BSS-like region within section)
        if (sec->virtual_size > sec_rawsize) {
            pe_memset((void*)(sec_va + sec_rawsize), 0,
                      sec->virtual_size - sec_rawsize);
        }
    }

    // Apply base relocations if needed
    pe_apply_relocations(data, size, image_base, actual_base, opt);

    // Set entry point
    result->entry_point = actual_base + opt->address_of_entry_point;
    result->success = 1;

    (void)section_align;
    return 0;
}

int pe_load_from_fd(int fd, pte_t* pml4, pe_load_result_t* result) {
    if (fd < 0 || !pml4 || !result) return -1;

    pe_memset(result, 0, sizeof(pe_load_result_t));

    // Read file size
    int old_pos = vfs_tell(fd);
    vfs_seek(fd, 0, VFS_SEEK_END);
    int file_size = vfs_tell(fd);
    vfs_seek(fd, 0, VFS_SEEK_SET);

    if (file_size <= 0 || file_size > 4 * 1024 * 1024) return -1;

    // Allocate temporary buffer
    void* buf = pmm_alloc_block();
    if (!buf) return -1;

    int bytes_to_read = file_size;
    if (bytes_to_read > (int)VMM_PAGE_SIZE) bytes_to_read = (int)VMM_PAGE_SIZE;

    int n = vfs_read(fd, buf, bytes_to_read);
    if (n <= 0) {
        pmm_free_block(buf);
        return -1;
    }

    vfs_seek(fd, old_pos, VFS_SEEK_SET);

    int ret = pe_load(buf, (uint64_t)n, pml4, result);
    pmm_free_block(buf);

    return ret;
}

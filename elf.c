// elf.c - ELF64 Loader for Alteo OS
// Parses ELF64 binaries and maps segments into process address spaces
#include "elf.h"
#include "vmm.h"
#include "pmm.h"
#include "vfs.h"

// ---------- Helpers ----------

static void elf_memset(void* ptr, int val, uint64_t size) {
    uint8_t* p = (uint8_t*)ptr;
    for (uint64_t i = 0; i < size; i++) p[i] = (uint8_t)val;
}

static void elf_memcpy(void* dst, const void* src, uint64_t size) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < size; i++) d[i] = s[i];
}

// Convert ELF program header flags to VMM page flags
static uint64_t elf_flags_to_vmm(uint32_t p_flags) {
    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
    if (p_flags & PF_W) flags |= VMM_FLAG_WRITABLE;
    // Note: NX bit would be set if PF_X is NOT set, but we skip NX for now
    return flags;
}

// ---------- Validation ----------

int elf_validate(const void* data, uint64_t size) {
    if (!data || size < sizeof(Elf64_Ehdr)) return 0;

    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)data;

    // Check ELF magic: 0x7F 'E' 'L' 'F'
    if (ehdr->e_ident[0] != 0x7F ||
        ehdr->e_ident[1] != 'E'  ||
        ehdr->e_ident[2] != 'L'  ||
        ehdr->e_ident[3] != 'F') {
        return 0;
    }

    // Check 64-bit
    if (ehdr->e_ident[4] != ELFCLASS64) return 0;

    // Check little-endian
    if (ehdr->e_ident[5] != ELFDATA2LSB) return 0;

    // Check x86_64
    if (ehdr->e_machine != EM_X86_64) return 0;

    // Check executable type
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) return 0;

    // Verify program headers are within the file
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return 0;
    if (ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize > size) return 0;

    return 1;
}

// ---------- Loading ----------

int elf_load(const void* data, uint64_t size, pte_t* pml4, elf_load_result_t* result) {
    if (!data || !pml4 || !result) return -1;

    elf_memset(result, 0, sizeof(elf_load_result_t));

    // Validate ELF header
    if (!elf_validate(data, size)) return -1;

    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)data;
    const uint8_t* file = (const uint8_t*)data;

    result->entry_point = ehdr->e_entry;
    result->load_base = 0xFFFFFFFFFFFFFFFF;
    result->load_end = 0;

    // Iterate through program headers and load PT_LOAD segments
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr* phdr = (const Elf64_Phdr*)(file + ehdr->e_phoff
                                   + (uint64_t)i * ehdr->e_phentsize);

        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0) continue;

        // Validate segment is within file bounds
        if (phdr->p_filesz > 0) {
            if (phdr->p_offset + phdr->p_filesz > size) return -1;
        }

        uint64_t vaddr = phdr->p_vaddr;
        uint64_t memsz = phdr->p_memsz;
        uint64_t filesz = phdr->p_filesz;
        uint64_t flags = elf_flags_to_vmm(phdr->p_flags);

        // Track load range
        if (vaddr < result->load_base) result->load_base = vaddr;
        if (vaddr + memsz > result->load_end) result->load_end = vaddr + memsz;

        // Map pages for this segment
        uint64_t page_start = vaddr & ~(VMM_PAGE_SIZE - 1);
        uint64_t page_end   = (vaddr + memsz + VMM_PAGE_SIZE - 1) & ~(VMM_PAGE_SIZE - 1);

        for (uint64_t page = page_start; page < page_end; page += VMM_PAGE_SIZE) {
            // Check if page is already mapped
            uint64_t existing = vmm_get_physical(pml4, page);
            if (existing) continue; // Already mapped (overlapping segments)

            // Allocate a physical page
            void* phys = pmm_alloc_block();
            if (!phys) return -1;
            elf_memset(phys, 0, VMM_PAGE_SIZE);

            // Map it into the process address space
            if (vmm_map_page(pml4, page, (uint64_t)phys, flags) < 0) {
                pmm_free_block(phys);
                return -1;
            }
        }

        // Copy file data into the mapped pages
        // Since pages are identity-mapped in kernel space, we can write directly
        if (filesz > 0) {
            const uint8_t* src = file + phdr->p_offset;
            uint8_t* dst = (uint8_t*)vaddr;
            elf_memcpy(dst, src, filesz);
        }

        // BSS: the region from filesz to memsz is already zeroed (pages were zeroed)
        if (memsz > filesz) {
            result->bss_start = vaddr + filesz;
            result->bss_end = vaddr + memsz;
        }
    }

    result->success = 1;
    return 0;
}

int elf_load_from_fd(int fd, pte_t* pml4, elf_load_result_t* result) {
    if (fd < 0 || !pml4 || !result) return -1;

    elf_memset(result, 0, sizeof(elf_load_result_t));

    // Read the full file into a temporary buffer
    // First, get the file size by seeking to end
    int old_pos = vfs_tell(fd);
    vfs_seek(fd, 0, VFS_SEEK_END);
    int file_size = vfs_tell(fd);
    vfs_seek(fd, 0, VFS_SEEK_SET);

    if (file_size <= 0 || file_size > 4 * 1024 * 1024) {
        // File too large or empty (max 4MB for now)
        return -1;
    }

    // Allocate temporary buffer for the file
    // Use physical pages directly since we're identity-mapped
    uint64_t pages_needed = ((uint64_t)file_size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    void* buf = pmm_alloc_block(); // First page for small files
    if (!buf) return -1;

    // For simplicity, only support files up to one page (4KB) in this initial version
    // TODO: Support larger files by allocating multiple pages
    int bytes_to_read = file_size;
    if (bytes_to_read > (int)VMM_PAGE_SIZE) bytes_to_read = (int)VMM_PAGE_SIZE;

    (void)pages_needed;

    int n = vfs_read(fd, buf, bytes_to_read);
    if (n <= 0) {
        pmm_free_block(buf);
        return -1;
    }

    // Restore file position
    vfs_seek(fd, old_pos, VFS_SEEK_SET);

    // Load the ELF
    int ret = elf_load(buf, (uint64_t)n, pml4, result);

    // Free temporary buffer
    pmm_free_block(buf);

    return ret;
}

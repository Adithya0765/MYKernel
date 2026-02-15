// shm.c - SysV Shared Memory for Alteo OS
// Physical page-backed shared memory segments mapped into process address spaces
#include "shm.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"

// Attachment record: which process has this segment mapped where
typedef struct {
    int      pid;
    uint64_t virt_addr;
    int      active;
} shm_attachment_t;

// Shared memory segment descriptor
typedef struct {
    uint64_t key;                          // IPC key
    uint64_t size;                         // Segment size (bytes)
    uint64_t phys_pages[256];              // Physical page addresses (up to 1MB / 4KB = 256)
    int      num_pages;                    // Number of physical pages
    int      active;                       // Is this segment allocated?
    int      marked_for_delete;            // Pending removal when nattach == 0
    shm_attachment_t attachments[SHM_MAX_ATTACH];
    int      nattach;                      // Current attachment count
} shm_segment_t;

static shm_segment_t segments[SHM_MAX_SEGMENTS];
static int shm_initialized = 0;
static uint64_t shm_vaddr_next = 0x50000000;  // Virtual address allocation region

// ---- Helpers ----
static void shm_memset(void* p, int v, int n) {
    unsigned char* b = (unsigned char*)p;
    for (int i = 0; i < n; i++) b[i] = (unsigned char)v;
}

// ---- Public API ----

void shm_init(void) {
    shm_memset(segments, 0, sizeof(segments));
    shm_initialized = 1;
}

int shm_get(uint64_t key, uint64_t size, int flags) {
    if (!shm_initialized) return -1;
    if (size == 0 || size > SHM_MAX_SIZE) return -1;

    // If key != IPC_PRIVATE, search for existing segment with this key
    if (key != (uint64_t)IPC_PRIVATE) {
        for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
            if (segments[i].active && segments[i].key == key) {
                if (flags & IPC_EXCL) return -1; // Already exists
                return i;
            }
        }
    }

    // Not found (or IPC_PRIVATE) — create new if IPC_CREAT or IPC_PRIVATE
    if (!(flags & IPC_CREAT) && key != (uint64_t)IPC_PRIVATE) return -1;

    // Find a free slot
    int slot = -1;
    for (int i = 0; i < SHM_MAX_SEGMENTS; i++) {
        if (!segments[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    // Allocate physical pages for the segment
    uint64_t page_size = VMM_PAGE_SIZE;
    int num_pages = (int)((size + page_size - 1) / page_size);
    if (num_pages > 256) return -1;

    shm_memset(&segments[slot], 0, sizeof(shm_segment_t));
    segments[slot].key = key;
    segments[slot].size = size;
    segments[slot].num_pages = num_pages;
    segments[slot].active = 1;
    segments[slot].nattach = 0;

    // Allocate physical pages
    for (int i = 0; i < num_pages; i++) {
        uint64_t page = (uint64_t)pmm_alloc_block();
        if (page == 0) {
            // Failed — free already allocated pages
            for (int j = 0; j < i; j++) {
                pmm_free_block((void*)segments[slot].phys_pages[j]);
            }
            segments[slot].active = 0;
            return -1;
        }
        segments[slot].phys_pages[i] = page;
    }

    return slot;
}

uint64_t shm_attach(int shmid, int pid, uint64_t addr) {
    if (!shm_initialized) return 0;
    if (shmid < 0 || shmid >= SHM_MAX_SEGMENTS) return 0;
    if (!segments[shmid].active) return 0;

    shm_segment_t* seg = &segments[shmid];

    // Find a free attachment slot
    int aslot = -1;
    for (int i = 0; i < SHM_MAX_ATTACH; i++) {
        if (!seg->attachments[i].active) { aslot = i; break; }
    }
    if (aslot < 0) return 0;

    // Choose virtual address if not specified
    uint64_t vaddr = addr;
    if (vaddr == 0) {
        vaddr = shm_vaddr_next;
        shm_vaddr_next += (uint64_t)seg->num_pages * VMM_PAGE_SIZE;
        // Align to next 4KB boundary (already aligned since num_pages * 4K)
    }

    // Map physical pages into the process's address space
    // For kernel-mode processes, use the kernel PML4
    pte_t* pml4 = vmm_get_current_address_space();
    process_t* p = process_get(pid);
    if (p && p->page_table) {
        pml4 = (pte_t*)p->page_table;
    }

    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;
    for (int i = 0; i < seg->num_pages; i++) {
        vmm_map_page(pml4, vaddr + (uint64_t)i * VMM_PAGE_SIZE,
                      seg->phys_pages[i], flags);
    }

    // Record attachment
    seg->attachments[aslot].pid = pid;
    seg->attachments[aslot].virt_addr = vaddr;
    seg->attachments[aslot].active = 1;
    seg->nattach++;

    return vaddr;
}

int shm_detach(int pid, uint64_t addr) {
    if (!shm_initialized) return -1;

    // Find which segment this address belongs to
    for (int s = 0; s < SHM_MAX_SEGMENTS; s++) {
        if (!segments[s].active) continue;
        shm_segment_t* seg = &segments[s];

        for (int a = 0; a < SHM_MAX_ATTACH; a++) {
            if (seg->attachments[a].active &&
                seg->attachments[a].pid == pid &&
                seg->attachments[a].virt_addr == addr) {

                // Unmap pages from process address space
                pte_t* pml4 = vmm_get_current_address_space();
                process_t* p = process_get(pid);
                if (p && p->page_table) {
                    pml4 = (pte_t*)p->page_table;
                }

                for (int i = 0; i < seg->num_pages; i++) {
                    uint64_t va = addr + (uint64_t)i * VMM_PAGE_SIZE;
                    vmm_unmap_page(pml4, va);
                    vmm_invlpg(va);
                }

                seg->attachments[a].active = 0;
                seg->nattach--;

                // If marked for deletion and no more attachments, free it
                if (seg->marked_for_delete && seg->nattach == 0) {
                    for (int i = 0; i < seg->num_pages; i++) {
                        pmm_free_block((void*)seg->phys_pages[i]);
                    }
                    seg->active = 0;
                }

                return 0;
            }
        }
    }

    return -1; // Not found
}

int shm_remove(int shmid) {
    if (!shm_initialized) return -1;
    if (shmid < 0 || shmid >= SHM_MAX_SEGMENTS) return -1;
    if (!segments[shmid].active) return -1;

    if (segments[shmid].nattach == 0) {
        // No attachments — free immediately
        for (int i = 0; i < segments[shmid].num_pages; i++) {
            pmm_free_block((void*)segments[shmid].phys_pages[i]);
        }
        segments[shmid].active = 0;
    } else {
        // Mark for deletion when all processes detach
        segments[shmid].marked_for_delete = 1;
    }

    return 0;
}

int shm_get_size(int shmid) {
    if (shmid < 0 || shmid >= SHM_MAX_SEGMENTS || !segments[shmid].active) return 0;
    return (int)segments[shmid].size;
}

int shm_get_nattach(int shmid) {
    if (shmid < 0 || shmid >= SHM_MAX_SEGMENTS || !segments[shmid].active) return 0;
    return segments[shmid].nattach;
}

// nv_fifo.c - NVIDIA PFIFO Command Submission Engine
// Manages DMA push buffer channels for CPU → GPU command submission
// Pre-NV50: Uses CACHE1 DMA push buffer mechanism
// NV50+: Uses indirect push buffer with playlist-based scheduling
// Reference: envytools PFIFO documentation

#include "nv_fifo.h"
#include "gpu.h"
#include "vmm.h"
#include "pmm.h"
#include "heap.h"

// ---- Helpers ----
static void fifo_memset(void* dst, int val, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

// ---- Global state ----
static nv_fifo_state_t fifo_state;

nv_fifo_state_t* nv_fifo_get_state(void) {
    return &fifo_state;
}

// ============================================================
// Push Buffer Operations
// ============================================================

void nv_fifo_push(int channel_id, uint32_t data) {
    if (channel_id < 0 || channel_id >= NV_FIFO_MAX_CHANNELS) return;

    nv_fifo_channel_t* ch = &fifo_state.channels[channel_id];
    if (!ch->active || !ch->pushbuf) return;

    uint32_t max_dwords = ch->pushbuf_size / 4;

    // Check for wrap-around
    if (ch->pushbuf_put >= max_dwords) {
        // Wrap to beginning (would need a jump command in production)
        ch->pushbuf_put = 0;
    }

    ch->pushbuf[ch->pushbuf_put] = data;
    ch->pushbuf_put++;
}

void nv_fifo_push_method(int channel_id, int subchan, uint32_t method, uint32_t data) {
    // Push header + data
    nv_fifo_push(channel_id, NV_FIFO_MTHD(subchan, method, 1));
    nv_fifo_push(channel_id, data);
}

void nv_fifo_push_method_n(int channel_id, int subchan, uint32_t method, uint32_t* data, int count) {
    if (!data || count <= 0) return;

    // Push incrementing method header
    nv_fifo_push(channel_id, NV_FIFO_MTHD_INC(subchan, method, (uint32_t)count));

    for (int i = 0; i < count; i++) {
        nv_fifo_push(channel_id, data[i]);
    }
}

// Kick: tell the GPU there are new commands in the push buffer
void nv_fifo_kick(int channel_id) {
    if (channel_id < 0 || channel_id >= NV_FIFO_MAX_CHANNELS) return;

    nv_fifo_channel_t* ch = &fifo_state.channels[channel_id];
    if (!ch->active) return;

    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped) return;

    if (g->arch >= NV_ARCH_NV50) {
        // NV50+: Write PUT to the channel's doorbell/update register
        // Channel control area is at BAR0 + 0xC00000 + channel_id * 0x1000
        uint32_t chan_base = 0xC00000 + (uint32_t)(channel_id * 0x1000);
        uint32_t put_bytes = ch->pushbuf_put * 4;  // Convert dwords to bytes
        nv_wr32(g->mmio, chan_base + 0x08, put_bytes);  // GP_PUT
    } else {
        // Pre-NV50: Write to CACHE1 DMA_PUT
        uint32_t put_bytes = ch->pushbuf_put * 4;
        nv_wr32(g->mmio, NV_PFIFO_CACHE1_DMA_PUT, put_bytes);
    }
}

uint32_t nv_fifo_space_available(int channel_id) {
    if (channel_id < 0 || channel_id >= NV_FIFO_MAX_CHANNELS) return 0;

    nv_fifo_channel_t* ch = &fifo_state.channels[channel_id];
    if (!ch->active) return 0;

    uint32_t max_dwords = ch->pushbuf_size / 4;
    uint32_t used = ch->pushbuf_put;  // Simplified: assume GET == 0 or lags behind
    if (used >= max_dwords) return 0;
    return max_dwords - used;
}

// ============================================================
// Synchronization
// ============================================================

void nv_fifo_emit_fence(int channel_id) {
    if (channel_id < 0 || channel_id >= NV_FIFO_MAX_CHANNELS) return;

    nv_fifo_channel_t* ch = &fifo_state.channels[channel_id];
    if (!ch->active || !ch->fence_mem) return;

    ch->fence_sequence++;

    gpu_state_t* g = gpu_get_state();

    if (g->arch >= NV_ARCH_NV50) {
        // NV50+: Use semaphore release method
        // Subchannel 0 (null), method 0x0010 = semaphore address high
        // method 0x0014 = semaphore address low
        // method 0x0018 = semaphore sequence
        // method 0x001C = semaphore trigger (1 = release)
        nv_fifo_push(channel_id, NV_FIFO_MTHD_INC(0, 0x0010, 4));
        nv_fifo_push(channel_id, (uint32_t)(ch->fence_mem_phys >> 32));
        nv_fifo_push(channel_id, (uint32_t)(ch->fence_mem_phys & 0xFFFFFFFF));
        nv_fifo_push(channel_id, ch->fence_sequence);
        nv_fifo_push(channel_id, 0x00000001);  // Release
    } else {
        // Pre-NV50: Write fence value through software method or notify
        // Use NV04 notify method on subchannel 0
        nv_fifo_push_method(channel_id, 0, 0x0104, ch->fence_sequence);
    }

    nv_fifo_kick(channel_id);
}

int nv_fifo_fence_completed(int channel_id, uint32_t seq) {
    if (channel_id < 0 || channel_id >= NV_FIFO_MAX_CHANNELS) return 1;

    nv_fifo_channel_t* ch = &fifo_state.channels[channel_id];
    if (!ch->active || !ch->fence_mem) return 1;

    // GPU writes the sequence number to fence_mem when it processes the fence command
    return (*ch->fence_mem >= seq);
}

void nv_fifo_wait_fence(int channel_id, uint32_t seq) {
    int timeout = 10000000;
    while (timeout-- > 0) {
        if (nv_fifo_fence_completed(channel_id, seq)) return;
    }
    // Timeout — GPU might be hung
}

void nv_fifo_wait_idle(int channel_id) {
    // Emit a fence and wait for it
    nv_fifo_emit_fence(channel_id);

    nv_fifo_channel_t* ch = &fifo_state.channels[channel_id];
    if (ch->active) {
        nv_fifo_wait_fence(channel_id, ch->fence_sequence);
    }
}

// ============================================================
// Channel Management
// ============================================================

int nv_fifo_channel_alloc(int channel_id) {
    if (channel_id < 0 || channel_id >= NV_FIFO_MAX_CHANNELS) return -1;

    gpu_state_t* g = gpu_get_state();
    if (!g->initialized) return -1;

    nv_fifo_channel_t* ch = &fifo_state.channels[channel_id];
    fifo_memset(ch, 0, sizeof(nv_fifo_channel_t));

    ch->channel_id = channel_id;

    // Allocate push buffer (physically contiguous, 64 KB)
    // Use PMM to get physical pages
    int num_pages = NV_FIFO_PUSHBUF_SIZE / VMM_PAGE_SIZE;
    uint64_t pb_phys = (uint64_t)pmm_alloc_block();
    if (pb_phys == 0) return -2;

    // Allocate remaining pages (we need contiguous, so allocate from a base)
    // For simplicity, allocate one page at a time and use the first one
    // A production driver would use a DMA-capable contiguous allocator
    for (int i = 1; i < num_pages; i++) {
        uint64_t page = (uint64_t)pmm_alloc_block();
        (void)page;  // We just need them allocated sequentially
    }

    ch->pushbuf_phys = pb_phys;
    ch->pushbuf_size = NV_FIFO_PUSHBUF_SIZE;

    // Map push buffer into kernel virtual space
    uint64_t pb_virt = 0xFFFF8000B0000000ULL + (uint64_t)(channel_id * NV_FIFO_PUSHBUF_SIZE);
    pte_t* pml4 = vmm_get_kernel_pml4();
    for (int i = 0; i < num_pages; i++) {
        vmm_map_page(pml4, pb_virt + (uint64_t)(i * VMM_PAGE_SIZE),
                      pb_phys + (uint64_t)(i * VMM_PAGE_SIZE),
                      VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_NOCACHE);
    }

    ch->pushbuf = (uint32_t*)pb_virt;
    fifo_memset(ch->pushbuf, 0, NV_FIFO_PUSHBUF_SIZE);
    ch->pushbuf_put = 0;
    ch->pushbuf_get = 0;

    // Allocate fence memory (one page, GPU-visible)
    uint64_t fence_phys = (uint64_t)pmm_alloc_block();
    if (fence_phys == 0) return -3;

    uint64_t fence_virt = 0xFFFF8000C0000000ULL + (uint64_t)(channel_id * VMM_PAGE_SIZE);
    vmm_map_page(pml4, fence_virt, fence_phys,
                  VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_NOCACHE);

    ch->fence_mem = (volatile uint32_t*)fence_virt;
    ch->fence_mem_phys = fence_phys;
    *ch->fence_mem = 0;
    ch->fence_sequence = 0;

    // Program the channel in PFIFO hardware
    if (g->arch >= NV_ARCH_NV50) {
        // NV50+: Set up channel in RAMIN (instance memory) or via channel table
        // Channel control registers at BAR0 + 0xC00000 + channel_id * 0x1000
        uint32_t chan_base = 0xC00000 + (uint32_t)(channel_id * 0x1000);

        // Set DMA push buffer base and limit
        nv_wr32(g->mmio, chan_base + 0x00, (uint32_t)(ch->pushbuf_phys >> 12));  // IB_BASE
        nv_wr32(g->mmio, chan_base + 0x04, (uint32_t)(ch->pushbuf_size - 1));     // IB_LIMIT
        nv_wr32(g->mmio, chan_base + 0x08, 0);                                     // GP_PUT = 0
        nv_wr32(g->mmio, chan_base + 0x0C, 0);                                     // GP_GET = 0

        // Enable channel
        nv_wr32(g->mmio, chan_base + 0x10, 0x00000001);
    } else {
        // Pre-NV50: Configure CACHE1 DMA for this channel
        // Disable PFIFO reassignment while configuring
        nv_wr32(g->mmio, NV_PFIFO_REASSIGN, 0);

        // Set channel as DMA mode
        uint32_t mode = nv_rd32(g->mmio, NV_PFIFO_MODE);
        mode |= (1 << channel_id);
        nv_wr32(g->mmio, NV_PFIFO_MODE, mode);

        // Enable DMA for this channel
        uint32_t dma = nv_rd32(g->mmio, NV_PFIFO_DMA);
        dma |= (1 << channel_id);
        nv_wr32(g->mmio, NV_PFIFO_DMA, dma);

        // Set CACHE1 to use this channel
        nv_wr32(g->mmio, NV_PFIFO_CACHE1_PUSH1, (uint32_t)channel_id | 0x00000100);

        // Set DMA push buffer base and limits
        nv_wr32(g->mmio, NV_PFIFO_CACHE1_DMA_PUT, 0);
        nv_wr32(g->mmio, NV_PFIFO_CACHE1_DMA_GET, 0);

        // Enable push buffer access
        nv_wr32(g->mmio, NV_PFIFO_CACHE1_PUSH0, 0x00000001);

        // Re-enable PFIFO
        nv_wr32(g->mmio, NV_PFIFO_REASSIGN, 1);
    }

    ch->active = 1;
    fifo_state.num_channels++;
    return 0;
}

void nv_fifo_channel_free(int channel_id) {
    if (channel_id < 0 || channel_id >= NV_FIFO_MAX_CHANNELS) return;

    nv_fifo_channel_t* ch = &fifo_state.channels[channel_id];
    if (!ch->active) return;

    // Wait for channel to drain
    nv_fifo_wait_idle(channel_id);

    gpu_state_t* g = gpu_get_state();

    if (g->arch >= NV_ARCH_NV50) {
        // Disable channel
        uint32_t chan_base = 0xC00000 + (uint32_t)(channel_id * 0x1000);
        nv_wr32(g->mmio, chan_base + 0x10, 0x00000000);
    } else {
        // Remove from DMA mode
        uint32_t mode = nv_rd32(g->mmio, NV_PFIFO_MODE);
        mode &= ~(1 << channel_id);
        nv_wr32(g->mmio, NV_PFIFO_MODE, mode);
    }

    // Free push buffer pages
    if (ch->pushbuf_phys) {
        int num_pages = (int)(ch->pushbuf_size / VMM_PAGE_SIZE);
        for (int i = 0; i < num_pages; i++) {
            pmm_free_block((void*)(ch->pushbuf_phys + (uint64_t)(i * VMM_PAGE_SIZE)));
        }
    }

    // Free fence memory
    if (ch->fence_mem_phys) {
        pmm_free_block((void*)ch->fence_mem_phys);
    }

    ch->active = 0;
    fifo_state.num_channels--;
}

int nv_fifo_channel_bind_object(int channel_id, int subchan, uint32_t obj_class) {
    if (channel_id < 0 || channel_id >= NV_FIFO_MAX_CHANNELS) return -1;
    if (subchan < 0 || subchan >= NV_FIFO_MAX_SUBCHANNELS) return -1;

    nv_fifo_channel_t* ch = &fifo_state.channels[channel_id];
    if (!ch->active) return -1;

    ch->subchan_class[subchan] = obj_class;

    // Bind object to subchannel via push buffer
    // Method 0x0000 on any subchannel = bind class
    nv_fifo_push_method(channel_id, subchan, 0x0000, obj_class);

    return 0;
}

// ============================================================
// Initialization
// ============================================================

int nv_fifo_init(void) {
    gpu_state_t* g = gpu_get_state();
    if (!g->initialized) return -1;

    fifo_memset(&fifo_state, 0, sizeof(nv_fifo_state_t));

    // Reset PFIFO engine
    uint32_t pmc_enable = nv_rd32(g->mmio, NV_PMC_ENABLE);
    nv_wr32(g->mmio, NV_PMC_ENABLE, pmc_enable & ~NV_PMC_ENABLE_PFIFO);
    for (volatile int i = 0; i < 100000; i++);
    nv_wr32(g->mmio, NV_PMC_ENABLE, pmc_enable | NV_PMC_ENABLE_PFIFO);
    for (volatile int i = 0; i < 100000; i++);

    // Clear PFIFO interrupts
    nv_wr32(g->mmio, NV_PFIFO_INTR_0, 0xFFFFFFFF);
    nv_wr32(g->mmio, NV_PFIFO_INTR_EN_0, 0x00000000);

    if (g->arch >= NV_ARCH_NV50) {
        // NV50+: Initialize playlist (round-robin channel scheduler)
        // The playlist is a table of active channel IDs in VRAM
        // For simplicity, we initialize with no channels
        nv_wr32(g->mmio, NV50_PFIFO_PLAYLIST_0, 0);
        nv_wr32(g->mmio, NV50_PFIFO_PLAYLIST_1, 0);
    } else {
        // Pre-NV50: Enable PFIFO DMA mode
        nv_wr32(g->mmio, NV_PFIFO_REASSIGN, 1);  // Enable reassign
        nv_wr32(g->mmio, NV_PFIFO_DELAY_0, 0);
        nv_wr32(g->mmio, NV_PFIFO_DMA_TIMESLICE, 0x2101FFFF);
    }

    // Allocate default channel (channel 0) for kernel graphics
    int ret = nv_fifo_channel_alloc(0);
    if (ret != 0) {
        // Failed to allocate default channel — FIFO is partially initialized
        fifo_state.initialized = 1;
        return ret;
    }

    fifo_state.active_channel = 0;
    fifo_state.initialized = 1;
    return 0;
}

void nv_fifo_shutdown(void) {
    if (!fifo_state.initialized) return;

    // Free all channels
    for (int i = 0; i < NV_FIFO_MAX_CHANNELS; i++) {
        if (fifo_state.channels[i].active) {
            nv_fifo_channel_free(i);
        }
    }

    gpu_state_t* g = gpu_get_state();
    if (g->mmio_mapped) {
        // Disable PFIFO interrupts
        nv_wr32(g->mmio, NV_PFIFO_INTR_EN_0, 0);
        nv_wr32(g->mmio, NV_PFIFO_INTR_0, 0xFFFFFFFF);
    }

    fifo_state.initialized = 0;
}

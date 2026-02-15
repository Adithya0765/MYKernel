// nv_2d.c - NVIDIA 2D Acceleration Engine
// Hardware-accelerated fills, blits, CPU-to-VRAM copies
// Pre-NV50: Direct PGRAPH register programming (simplified, no FIFO)
// NV50+: Direct register kicks via NV50_2D class methods
// Reference: envytools 2D engine documentation

#include "nv_2d.h"
#include "gpu.h"
#include "heap.h"

// ---- Global 2D state ----
static nv_2d_state_t state_2d;

nv_2d_state_t* nv_2d_get_state(void) {
    return &state_2d;
}

// ---- Helpers ----
static void _2d_memset(void* dst, int val, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

// Write a method to the 2D engine (NV50+ direct method kick)
// On real hardware this would go through a FIFO push buffer channel.
// For simplicity, we write methods directly to PGRAPH subchannel registers.
// This works for single-threaded kernel-mode usage.
static void nv50_2d_method(uint32_t mthd, uint32_t data) {
    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped) return;

    // NV50+ 2D engine methods are accessed through PGRAPH subchannel 3
    // or via direct MMIO at NV_PGRAPH + method offset
    // For direct register access (no FIFO), we use PGRAPH base + subchannel offset
    // Subchannel 3 for 2D class: base 0x400000 + 0x8000 (subchannel 3) + method
    //
    // Simplified approach: Write the method data directly to the 2D engine
    // register space. In a production driver, this would use PFIFO push buffers.
    nv_wr32(g->mmio, NV_PGRAPH + 0x8000 + mthd, data);
}

// Legacy PGRAPH 2D register write (pre-NV50)
static void nv04_pgraph_wr(uint32_t reg, uint32_t data) {
    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped) return;
    nv_wr32(g->mmio, reg, data);
}

static uint32_t nv04_pgraph_rd(uint32_t reg) {
    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped) return 0;
    return nv_rd32(g->mmio, reg);
}

// ============================================================
// Wait for engine idle
// ============================================================

void nv_2d_wait_idle(void) {
    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped) return;

    // Poll PGRAPH status register until idle
    // Bit 0: PGRAPH busy
    int timeout = 2000000;
    while (timeout-- > 0) {
        uint32_t status = nv_rd32(g->mmio, NV04_PGRAPH_STATUS);
        if ((status & 0x00000001) == 0) return;  // Idle
    }
    // Timeout — engine may be hung
}

// ============================================================
// Surface Configuration
// ============================================================

void nv_2d_set_dst(uint64_t vram_addr, int width, int height, int pitch, uint32_t fmt) {
    state_2d.dst_addr = vram_addr;
    state_2d.dst_width = width;
    state_2d.dst_height = height;
    state_2d.dst_pitch = (uint32_t)pitch;
    state_2d.dst_format = fmt;

    if (state_2d.use_nv50_engine) {
        nv50_2d_method(NV50_2D_DST_FORMAT, fmt);
        nv50_2d_method(NV50_2D_DST_LINEAR, 1);  // Linear (pitch) layout
        nv50_2d_method(NV50_2D_DST_PITCH, (uint32_t)pitch);
        nv50_2d_method(NV50_2D_DST_WIDTH, (uint32_t)width);
        nv50_2d_method(NV50_2D_DST_HEIGHT, (uint32_t)height);
        nv50_2d_method(NV50_2D_DST_ADDRESS_HI, (uint32_t)(vram_addr >> 32));
        nv50_2d_method(NV50_2D_DST_ADDRESS_LO, (uint32_t)(vram_addr & 0xFFFFFFFF));
    }
}

void nv_2d_set_src(uint64_t vram_addr, int width, int height, int pitch, uint32_t fmt) {
    state_2d.src_addr = vram_addr;
    state_2d.src_width = width;
    state_2d.src_height = height;
    state_2d.src_pitch = (uint32_t)pitch;
    state_2d.src_format = fmt;

    if (state_2d.use_nv50_engine) {
        nv50_2d_method(NV50_2D_SRC_FORMAT, fmt);
        nv50_2d_method(NV50_2D_SRC_LINEAR, 1);
        nv50_2d_method(NV50_2D_SRC_PITCH, (uint32_t)pitch);
        nv50_2d_method(NV50_2D_SRC_WIDTH, (uint32_t)width);
        nv50_2d_method(NV50_2D_SRC_HEIGHT, (uint32_t)height);
        nv50_2d_method(NV50_2D_SRC_ADDRESS_HI, (uint32_t)(vram_addr >> 32));
        nv50_2d_method(NV50_2D_SRC_ADDRESS_LO, (uint32_t)(vram_addr & 0xFFFFFFFF));
    }
}

void nv_2d_set_clip(int x, int y, int w, int h) {
    state_2d.clip_x = x;
    state_2d.clip_y = y;
    state_2d.clip_w = w;
    state_2d.clip_h = h;

    if (state_2d.use_nv50_engine) {
        nv50_2d_method(NV50_2D_CLIP_X, (uint32_t)x);
        nv50_2d_method(NV50_2D_CLIP_Y, (uint32_t)y);
        nv50_2d_method(NV50_2D_CLIP_W, (uint32_t)w);
        nv50_2d_method(NV50_2D_CLIP_H, (uint32_t)h);
    }
}

// ============================================================
// Rectangle Fill
// ============================================================

// NV50+ hardware rect fill
static void nv50_rect_fill(int x, int y, int w, int h, uint32_t color) {
    // Set operation to SRCCOPY
    nv50_2d_method(NV50_2D_OPERATION, NV50_2D_OP_SRCCOPY);

    // Set shape to rectangle (3)
    nv50_2d_method(NV50_2D_DRAW_SHAPE, 3);

    // Set fill color
    nv50_2d_method(NV50_2D_DRAW_COLOR, color);

    // Set position and size
    nv50_2d_method(NV50_2D_DRAW_POINT_X, (uint32_t)x);
    nv50_2d_method(NV50_2D_DRAW_POINT_Y, (uint32_t)y);
    nv50_2d_method(NV50_2D_DRAW_SIZE_X, (uint32_t)w);
    nv50_2d_method(NV50_2D_DRAW_SIZE_Y, (uint32_t)h);
}

// Pre-NV50 software-assisted rect fill via VRAM aperture
static void legacy_rect_fill(int x, int y, int w, int h, uint32_t color) {
    gpu_state_t* g = gpu_get_state();
    if (!g->vram_mapped || !g->vram) return;

    // Direct write to VRAM via BAR1 aperture
    // This is technically a CPU fill, but writes go to GPU VRAM
    uint32_t pitch = state_2d.dst_pitch;
    uint32_t offset = state_2d.dst_addr / 4;
    uint32_t stride = pitch / 4;

    volatile uint32_t* fb = g->vram + offset;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int px = x + i;
            int py = y + j;
            if (px >= 0 && px < state_2d.dst_width &&
                py >= 0 && py < state_2d.dst_height) {
                fb[(uint32_t)py * stride + (uint32_t)px] = color;
            }
        }
    }
}

void nv_2d_rect_fill(int x, int y, int w, int h, uint32_t color) {
    if (!state_2d.initialized) return;

    if (state_2d.use_nv50_engine) {
        nv50_rect_fill(x, y, w, h, color);
    } else {
        legacy_rect_fill(x, y, w, h, color);
    }

    state_2d.fills_count++;
}

// ============================================================
// Blit (Screen-to-Screen Copy)
// ============================================================

static void nv50_blit(int dst_x, int dst_y, int src_x, int src_y, int w, int h) {
    // Set operation to SRCCOPY
    nv50_2d_method(NV50_2D_OPERATION, NV50_2D_OP_SRCCOPY);

    // Set destination position
    nv50_2d_method(NV50_2D_BLIT_DST_X, (uint32_t)dst_x);
    nv50_2d_method(NV50_2D_BLIT_DST_Y, (uint32_t)dst_y);
    nv50_2d_method(NV50_2D_BLIT_DST_W, (uint32_t)w);
    nv50_2d_method(NV50_2D_BLIT_DST_H, (uint32_t)h);

    // 1:1 scale (fixed-point 32.32: 1.0 = 0x00010000 in 16.16 or simply 1 in integer)
    nv50_2d_method(NV50_2D_BLIT_DU_DX, 0x00010000);  // 1:1 horizontal
    nv50_2d_method(NV50_2D_BLIT_DV_DY, 0x00010000);  // 1:1 vertical

    // Set source position (triggers the blit)
    nv50_2d_method(NV50_2D_BLIT_SRC_X, (uint32_t)(src_x << 16));
    nv50_2d_method(NV50_2D_BLIT_SRC_Y, (uint32_t)(src_y << 16));
}

static void legacy_blit(int dst_x, int dst_y, int src_x, int src_y, int w, int h) {
    gpu_state_t* g = gpu_get_state();
    if (!g->vram_mapped || !g->vram) return;

    // Software blit via VRAM aperture
    uint32_t stride = state_2d.dst_pitch / 4;
    uint32_t offset = state_2d.dst_addr / 4;
    volatile uint32_t* fb = g->vram + offset;

    // Handle overlapping regions
    int y_start, y_end, y_step;
    if (dst_y > src_y) {
        y_start = h - 1; y_end = -1; y_step = -1;
    } else {
        y_start = 0; y_end = h; y_step = 1;
    }

    for (int j = y_start; j != y_end; j += y_step) {
        int x_start, x_end, x_step;
        if (dst_x > src_x) {
            x_start = w - 1; x_end = -1; x_step = -1;
        } else {
            x_start = 0; x_end = w; x_step = 1;
        }
        for (int i = x_start; i != x_end; i += x_step) {
            int sx = src_x + i, sy = src_y + j;
            int dx = dst_x + i, dy = dst_y + j;
            if (sx >= 0 && sx < state_2d.dst_width && sy >= 0 && sy < state_2d.dst_height &&
                dx >= 0 && dx < state_2d.dst_width && dy >= 0 && dy < state_2d.dst_height) {
                fb[(uint32_t)dy * stride + (uint32_t)dx] = fb[(uint32_t)sy * stride + (uint32_t)sx];
            }
        }
    }
}

void nv_2d_blit(int dst_x, int dst_y, int src_x, int src_y, int w, int h) {
    if (!state_2d.initialized) return;

    if (state_2d.use_nv50_engine) {
        nv50_blit(dst_x, dst_y, src_x, src_y, w, h);
    } else {
        legacy_blit(dst_x, dst_y, src_x, src_y, w, h);
    }

    state_2d.blits_count++;
}

void nv_2d_copy_rect(int dst_x, int dst_y, int src_x, int src_y, int w, int h) {
    // Alias for blit
    nv_2d_blit(dst_x, dst_y, src_x, src_y, w, h);
}

// ============================================================
// CPU-to-VRAM Copy
// ============================================================

static void nv50_copy_from_cpu(int dst_x, int dst_y, int w, int h, uint32_t* pixels) {
    // Use SIFC (Scaled Image From CPU) method
    nv50_2d_method(NV50_2D_OPERATION, NV50_2D_OP_SRCCOPY);
    nv50_2d_method(NV50_2D_SIFC_FORMAT, NV50_2D_FMT_A8R8G8B8);
    nv50_2d_method(NV50_2D_SIFC_WIDTH, (uint32_t)w);
    nv50_2d_method(NV50_2D_SIFC_HEIGHT, (uint32_t)h);
    nv50_2d_method(NV50_2D_SIFC_DST_X, (uint32_t)dst_x);
    nv50_2d_method(NV50_2D_SIFC_DST_Y, (uint32_t)dst_y);
    nv50_2d_method(NV50_2D_SIFC_DST_W, (uint32_t)w);
    nv50_2d_method(NV50_2D_SIFC_DST_H, (uint32_t)h);

    // Push pixel data through SIFC_DATA register
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            nv50_2d_method(NV50_2D_SIFC_DATA, pixels[j * w + i]);
        }
    }
}

static void legacy_copy_from_cpu(int dst_x, int dst_y, int w, int h, uint32_t* pixels) {
    gpu_state_t* g = gpu_get_state();
    if (!g->vram_mapped || !g->vram) return;

    // Direct CPU write to VRAM via BAR1 aperture
    uint32_t stride = state_2d.dst_pitch / 4;
    uint32_t offset = state_2d.dst_addr / 4;
    volatile uint32_t* fb = g->vram + offset;

    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int dx = dst_x + i, dy = dst_y + j;
            if (dx >= 0 && dx < state_2d.dst_width && dy >= 0 && dy < state_2d.dst_height) {
                fb[(uint32_t)dy * stride + (uint32_t)dx] = pixels[j * w + i];
            }
        }
    }
}

void nv_2d_copy_from_cpu(int dst_x, int dst_y, int w, int h, uint32_t* pixels) {
    if (!state_2d.initialized || !pixels) return;

    if (state_2d.use_nv50_engine) {
        nv50_copy_from_cpu(dst_x, dst_y, w, h, pixels);
    } else {
        legacy_copy_from_cpu(dst_x, dst_y, w, h, pixels);
    }

    state_2d.cpu_copies_count++;
}

// ============================================================
// Flip (backbuffer → scanout VRAM)
// ============================================================

void nv_2d_flip(uint32_t* backbuf, int width, int height) {
    if (!state_2d.initialized || !backbuf) return;

    gpu_state_t* g = gpu_get_state();

    // If GPU VRAM is mapped, copy backbuffer to VRAM
    if (g->vram_mapped && g->vram) {
        // Use CPU-to-VRAM copy for now (2D engine acceleration requires FIFO setup)
        // This is still faster than writing to the VBE framebuffer because:
        // 1. BAR1 writes can be write-combined by the PCI bridge
        // 2. Data goes directly to GPU VRAM, no CPU cache coherency overhead

        volatile uint32_t* dst = g->vram + (g->fb_offset / 4);
        int stride = g->display_pitch / 4;

        for (int y = 0; y < height; y++) {
            uint32_t* src = &backbuf[y * width];
            volatile uint32_t* d = &dst[y * stride];
            for (int x = 0; x < width; x++) {
                d[x] = src[x];
            }
        }
    }
}

// ============================================================
// Initialization
// ============================================================

int nv_2d_init(void) {
    gpu_state_t* g = gpu_get_state();
    if (!g->initialized) return -1;

    _2d_memset(&state_2d, 0, sizeof(nv_2d_state_t));

    // Select 2D class based on architecture
    if (g->arch >= NV_ARCH_NVE0) {
        state_2d.class_2d = NVE0_2D_CLASS;
        state_2d.use_nv50_engine = 1;
    } else if (g->arch >= NV_ARCH_NVC0) {
        state_2d.class_2d = NVC0_2D_CLASS;
        state_2d.use_nv50_engine = 1;
    } else if (g->arch >= NV_ARCH_NV50) {
        state_2d.class_2d = NV50_2D_CLASS;
        state_2d.use_nv50_engine = 1;
    } else if (g->arch >= NV_ARCH_NV10) {
        state_2d.class_2d = NV10_IMAGE_BLIT;
        state_2d.use_nv50_engine = 0;
    } else {
        state_2d.class_2d = NV04_IMAGE_BLIT;
        state_2d.use_nv50_engine = 0;
    }

    // Set up default destination surface = scanout framebuffer
    if (g->vram_mapped) {
        int width = g->display_width > 0 ? g->display_width : 1024;
        int height = g->display_height > 0 ? g->display_height : 768;
        int pitch = width * 4;  // 32bpp

        nv_2d_set_dst(g->fb_offset, width, height, pitch, NV50_2D_FMT_A8R8G8B8);
        nv_2d_set_src(g->fb_offset, width, height, pitch, NV50_2D_FMT_A8R8G8B8);
        nv_2d_set_clip(0, 0, width, height);

        // Set operation to SRCCOPY by default
        if (state_2d.use_nv50_engine) {
            nv50_2d_method(NV50_2D_OPERATION, NV50_2D_OP_SRCCOPY);
        }
    }

    state_2d.initialized = 1;
    return 0;
}

void nv_2d_shutdown(void) {
    if (!state_2d.initialized) return;

    // Wait for any pending operations
    nv_2d_wait_idle();

    state_2d.initialized = 0;
}

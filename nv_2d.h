// nv_2d.h - NVIDIA 2D Acceleration Engine for Alteo OS
// Hardware-accelerated rectangle fills, blits, screen-to-screen copies
// Pre-NV50: Uses NV04-NV40 2D object classes via PGRAPH
// NV50+: Uses NV50_2D class (0x502D) via PFIFO channels
#ifndef NV_2D_H
#define NV_2D_H

#include "stdint.h"
#include "gpu.h"

// ============================================================
// 2D Object Classes (NVIDIA nomenclature)
// ============================================================

// Pre-NV50 2D classes
#define NV04_GDI_RECT_TEXT      0x004A      // GDI rectangle + text
#define NV04_IMAGE_BLIT         0x005F      // Screen-to-screen blit
#define NV10_IMAGE_BLIT         0x009F      // NV10+ blit
#define NV04_IMAGE_FROM_CPU     0x0061      // CPU-to-screen transfer
#define NV10_IMAGE_FROM_CPU     0x0065      // NV10+ CPU-to-screen
#define NV04_RECT               0x005E      // Rectangle fill

// NV50+ 2D class
#define NV50_2D_CLASS           0x502D      // Tesla 2D engine
#define NVC0_2D_CLASS           0x902D      // Fermi 2D engine
#define NVE0_2D_CLASS           0xA02D      // Kepler 2D engine

// ============================================================
// NV50 2D Engine Methods (push buffer commands)
// ============================================================

// Destination surface setup
#define NV50_2D_DST_FORMAT      0x0200      // Destination pixel format
#define NV50_2D_DST_LINEAR      0x0204      // 0=tiled, 1=linear (pitch)
#define NV50_2D_DST_PITCH       0x0214      // Destination pitch in bytes
#define NV50_2D_DST_WIDTH       0x0218      // Destination width
#define NV50_2D_DST_HEIGHT      0x021C      // Destination height
#define NV50_2D_DST_ADDRESS_HI  0x0220      // Destination VRAM address (high 32)
#define NV50_2D_DST_ADDRESS_LO  0x0224      // Destination VRAM address (low 32)

// Source surface setup
#define NV50_2D_SRC_FORMAT      0x0230      // Source pixel format
#define NV50_2D_SRC_LINEAR      0x0234      // 0=tiled, 1=linear
#define NV50_2D_SRC_PITCH       0x0244      // Source pitch
#define NV50_2D_SRC_WIDTH       0x0248      // Source width
#define NV50_2D_SRC_HEIGHT      0x024C      // Source height
#define NV50_2D_SRC_ADDRESS_HI  0x0250      // Source VRAM address (high 32)
#define NV50_2D_SRC_ADDRESS_LO  0x0254      // Source VRAM address (low 32)

// Clip rectangle
#define NV50_2D_CLIP_X          0x0280      // Clip rect left
#define NV50_2D_CLIP_Y          0x0284      // Clip rect top
#define NV50_2D_CLIP_W          0x0288      // Clip rect width
#define NV50_2D_CLIP_H          0x028C      // Clip rect height

// ROP (raster operation)
#define NV50_2D_OPERATION        0x02AC     // Operation type
#define NV50_2D_ROP              0x02A0     // ROP value

// Operation types
#define NV50_2D_OP_SRCCOPY      0x03        // Direct copy (src → dst)
#define NV50_2D_OP_ROP_AND      0x01        // ROP AND
#define NV50_2D_OP_BLEND        0x05        // Alpha blend

// Pixel formats (for SRC/DST_FORMAT)
#define NV50_2D_FMT_A8R8G8B8   0xCF        // 32-bit ARGB (our framebuffer format)
#define NV50_2D_FMT_X8R8G8B8   0xE8        // 32-bit XRGB (no alpha)
#define NV50_2D_FMT_R5G6B5     0xE9        // 16-bit RGB565
#define NV50_2D_FMT_A8         0xF5        // 8-bit alpha only
#define NV50_2D_FMT_Y8         0xF3        // 8-bit grayscale

// Solid fill
#define NV50_2D_DRAW_SHAPE      0x0580      // Shape: 0=point, 1=line, 2=triangle, 3=rect
#define NV50_2D_DRAW_COLOR      0x0584      // Fill color (ARGB32)
#define NV50_2D_DRAW_POINT_X    0x0590      // Point/rect X start
#define NV50_2D_DRAW_POINT_Y    0x0594      // Point/rect Y start
#define NV50_2D_DRAW_SIZE_X     0x0598      // Rect width
#define NV50_2D_DRAW_SIZE_Y     0x059C      // Rect height

// Blit (copy)
#define NV50_2D_BLIT_CTRL       0x0300      // Blit control
#define NV50_2D_BLIT_DST_X      0x0300      // Blit destination X
#define NV50_2D_BLIT_DST_Y      0x0304      // Blit destination Y
#define NV50_2D_BLIT_DST_W      0x0308      // Blit width
#define NV50_2D_BLIT_DST_H      0x030C      // Blit height
#define NV50_2D_BLIT_DU_DX      0x0310      // Horizontal scale (fixed 32.32)
#define NV50_2D_BLIT_DV_DY      0x0314      // Vertical scale (fixed 32.32)
#define NV50_2D_BLIT_SRC_X      0x0318      // Source X (fixed 32.32)
#define NV50_2D_BLIT_SRC_Y      0x031C      // Source Y (fixed 32.32)

// Sifc (scaled image from CPU)
#define NV50_2D_SIFC_FORMAT     0x0400      // Sifc source format
#define NV50_2D_SIFC_WIDTH      0x0408      // Source width
#define NV50_2D_SIFC_HEIGHT     0x040C      // Source height
#define NV50_2D_SIFC_DST_X      0x0410      // Destination X
#define NV50_2D_SIFC_DST_Y      0x0414      // Destination Y
#define NV50_2D_SIFC_DST_W      0x0418      // Destination width
#define NV50_2D_SIFC_DST_H      0x041C      // Destination height
#define NV50_2D_SIFC_DATA       0x0800      // Pixel data (write consecutively)

// ============================================================
// Pre-NV50 PGRAPH 2D Registers
// ============================================================

// PGRAPH sub-channels (pre-NV50 FIFO-based)
#define NV04_PGRAPH_CTX_SWITCH1 0x400160
#define NV04_PGRAPH_STATUS      0x400700
#define NV04_PGRAPH_TRAPPED_ADDR 0x400704

// Direct rectangle fill via PGRAPH (not via FIFO, for simplicity)
#define NV_PGRAPH_BOFFSET       0x400640    // Buffer offset (pre-NV50)
#define NV_PGRAPH_BPITCH        0x400670    // Buffer pitch
#define NV_PGRAPH_BSWIZZLE2     0x400818    // Swizzle config

// ============================================================
// 2D Engine State
// ============================================================

typedef struct {
    int      initialized;
    uint32_t class_2d;          // Active 2D class
    int      use_nv50_engine;   // Using NV50+ 2D engine?

    // Current surface configuration
    uint32_t dst_format;
    uint32_t dst_pitch;
    int      dst_width;
    int      dst_height;
    uint64_t dst_addr;          // VRAM address of destination surface

    uint32_t src_format;
    uint32_t src_pitch;
    int      src_width;
    int      src_height;
    uint64_t src_addr;          // VRAM address of source surface

    // Clip rectangle
    int      clip_x, clip_y, clip_w, clip_h;

    // Performance counters
    uint64_t fills_count;
    uint64_t blits_count;
    uint64_t cpu_copies_count;
} nv_2d_state_t;

// ---- Initialization ----
int  nv_2d_init(void);                      // Initialize 2D engine
void nv_2d_shutdown(void);                  // Shut down 2D engine

// ---- Surface Setup ----
void nv_2d_set_dst(uint64_t vram_addr, int width, int height, int pitch, uint32_t fmt);
void nv_2d_set_src(uint64_t vram_addr, int width, int height, int pitch, uint32_t fmt);
void nv_2d_set_clip(int x, int y, int w, int h);

// ---- Accelerated Operations ----
void nv_2d_rect_fill(int x, int y, int w, int h, uint32_t color);
void nv_2d_blit(int dst_x, int dst_y, int src_x, int src_y, int w, int h);
void nv_2d_copy_from_cpu(int dst_x, int dst_y, int w, int h, uint32_t* pixels);
void nv_2d_copy_rect(int dst_x, int dst_y, int src_x, int src_y, int w, int h);

// ---- Flip Integration ----
// Replaces software flip_buffer() with GPU-accelerated VRAM blit
void nv_2d_flip(uint32_t* backbuf, int width, int height);

// ---- Wait for idle ----
void nv_2d_wait_idle(void);

// ---- State query ----
nv_2d_state_t* nv_2d_get_state(void);

#endif

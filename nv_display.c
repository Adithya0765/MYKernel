// nv_display.c - NVIDIA Display Engine / Mode Setting
// Pre-NV50: programs PCRTC + PRAMDAC registers directly
// NV50+: programs unified display engine (PDISPLAY) via EVO channel
// Reference: envytools display documentation

#include "nv_display.h"
#include "gpu.h"
#include "heap.h"

// ---- String/mem helpers ----
static void disp_memset(void* dst, int val, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

static void disp_memcpy(void* dst, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

// ---- Global display state ----
static nv_display_state_t display;

// ---- Standard VESA modes ----
// Clock values are pixel clock in kHz
static nv_display_mode_t standard_modes[] = {
    // 640x480 @ 60Hz (VGA)
    { .clock = 25175, .hdisplay = 640, .hsync_start = 656, .hsync_end = 752,
      .htotal = 800, .vdisplay = 480, .vsync_start = 490, .vsync_end = 492,
      .vtotal = 525, .bpp = 32, .flags = NV_MODE_FLAG_HSYNC_NEG | NV_MODE_FLAG_VSYNC_NEG },
    // 800x600 @ 60Hz (SVGA)
    { .clock = 40000, .hdisplay = 800, .hsync_start = 840, .hsync_end = 968,
      .htotal = 1056, .vdisplay = 600, .vsync_start = 601, .vsync_end = 605,
      .vtotal = 628, .bpp = 32, .flags = NV_MODE_FLAG_HSYNC_POS | NV_MODE_FLAG_VSYNC_POS },
    // 1024x768 @ 60Hz (XGA) — default Alteo mode
    { .clock = 65000, .hdisplay = 1024, .hsync_start = 1048, .hsync_end = 1184,
      .htotal = 1344, .vdisplay = 768, .vsync_start = 771, .vsync_end = 777,
      .vtotal = 806, .bpp = 32, .flags = NV_MODE_FLAG_HSYNC_NEG | NV_MODE_FLAG_VSYNC_NEG },
    // 1280x720 @ 60Hz (HD 720p)
    { .clock = 74250, .hdisplay = 1280, .hsync_start = 1390, .hsync_end = 1430,
      .htotal = 1650, .vdisplay = 720, .vsync_start = 725, .vsync_end = 730,
      .vtotal = 750, .bpp = 32, .flags = NV_MODE_FLAG_HSYNC_POS | NV_MODE_FLAG_VSYNC_POS },
    // 1280x1024 @ 60Hz (SXGA)
    { .clock = 108000, .hdisplay = 1280, .hsync_start = 1328, .hsync_end = 1440,
      .htotal = 1688, .vdisplay = 1024, .vsync_start = 1025, .vsync_end = 1028,
      .vtotal = 1066, .bpp = 32, .flags = NV_MODE_FLAG_HSYNC_POS | NV_MODE_FLAG_VSYNC_POS },
    // 1920x1080 @ 60Hz (Full HD)
    { .clock = 148500, .hdisplay = 1920, .hsync_start = 2008, .hsync_end = 2052,
      .htotal = 2200, .vdisplay = 1080, .vsync_start = 1084, .vsync_end = 1089,
      .vtotal = 1125, .bpp = 32, .flags = NV_MODE_FLAG_HSYNC_POS | NV_MODE_FLAG_VSYNC_POS },
    // 1600x900 @ 60Hz (HD+)
    { .clock = 108000, .hdisplay = 1600, .hsync_start = 1624, .hsync_end = 1704,
      .htotal = 1800, .vdisplay = 900, .vsync_start = 901, .vsync_end = 904,
      .vtotal = 1000, .bpp = 32, .flags = NV_MODE_FLAG_HSYNC_POS | NV_MODE_FLAG_VSYNC_POS },
    // 2560x1440 @ 60Hz (QHD)
    { .clock = 241500, .hdisplay = 2560, .hsync_start = 2608, .hsync_end = 2640,
      .htotal = 2720, .vdisplay = 1440, .vsync_start = 1443, .vsync_end = 1448,
      .vtotal = 1481, .bpp = 32, .flags = NV_MODE_FLAG_HSYNC_POS | NV_MODE_FLAG_VSYNC_NEG },
};
#define NUM_STANDARD_MODES (sizeof(standard_modes) / sizeof(standard_modes[0]))

// ============================================================
// PLL Calculation
// ============================================================

// Calculate PLL N, M, P coefficients for a target pixel clock
// PLL output = refclk * N / M / (2^P)
// Constraints (NV50 typical):
//   refclk = 27000 kHz (27 MHz crystal)
//   1 <= M <= 255, 1 <= N <= 255, 0 <= P <= 7
//   VCO = refclk * N / M must be in range [128000, 700000] kHz
int nv_pll_calc(uint32_t target_khz, nv_pll_t* pll) {
    if (!pll || target_khz == 0) return -1;

    pll->refclk = 27000;  // 27 MHz reference crystal

    uint32_t best_err = 0xFFFFFFFF;
    uint32_t best_n = 1, best_m = 1, best_p = 0;

    // Try all P values (post-divider)
    for (uint32_t p = 0; p <= 7; p++) {
        uint32_t vco_target = target_khz << p;

        // VCO must be within valid range
        if (vco_target < 128000 || vco_target > 700000) continue;

        // Try M and N combinations
        for (uint32_t m = 1; m <= 255; m++) {
            // N = vco_target * M / refclk
            uint32_t n = (vco_target * m + pll->refclk / 2) / pll->refclk;
            if (n < 1 || n > 255) continue;

            // Calculate actual output frequency
            uint32_t actual = (pll->refclk * n) / m;
            actual >>= p;

            // Error
            uint32_t err;
            if (actual > target_khz)
                err = actual - target_khz;
            else
                err = target_khz - actual;

            if (err < best_err) {
                best_err = err;
                best_n = n;
                best_m = m;
                best_p = p;
                if (err == 0) goto done;
            }
        }
    }

done:
    pll->n = best_n;
    pll->m = best_m;
    pll->p = best_p;
    return 0;
}

void nv_pll_set(int pll_index, nv_pll_t* pll) {
    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped || !pll) return;

    if (g->arch >= NV_ARCH_NV50) {
        // NV50+: Write to PPLL block
        // Coefficient encoding: bits[7:0]=M, bits[15:8]=N, bits[18:16]=P
        uint32_t coeff = (pll->m & 0xFF) | ((pll->n & 0xFF) << 8) | ((pll->p & 0x7) << 16);

        // Enable PLL
        nv_wr32(g->mmio, NV50_PPLL_CTRL(pll_index), 0x00000001);
        nv_wr32(g->mmio, NV50_PPLL_COEFF(pll_index), coeff);

        // Wait for PLL lock (simple busy wait)
        for (volatile int i = 0; i < 100000; i++);
    } else {
        // Pre-NV50: Write to PRAMDAC VPLL register
        // Coefficient encoding: bits[7:0]=M, bits[15:8]=N, bits[18:16]=P
        uint32_t coeff = (pll->m & 0xFF) | ((pll->n & 0xFF) << 8) | ((pll->p & 0x7) << 16);

        uint32_t reg = (pll_index == 0) ? NV_PRAMDAC_VPLL_COEFF : NV_PRAMDAC_VPLL2_COEFF;
        nv_wr32(g->mmio, reg, coeff);

        for (volatile int i = 0; i < 100000; i++);
    }
}

// ============================================================
// Mode Setting
// ============================================================

nv_display_mode_t* nv_display_find_mode(int width, int height) {
    for (int i = 0; i < (int)NUM_STANDARD_MODES; i++) {
        if (standard_modes[i].hdisplay == width && standard_modes[i].vdisplay == height) {
            return &standard_modes[i];
        }
    }
    return 0;
}

// Program CRTC timing on pre-NV50 hardware
static int nv_crtc_set_mode_legacy(int head, nv_display_mode_t* mode) {
    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped) return -1;

    // Base offset for head (head 0 = 0x600000, head 1 = 0x602000)
    uint32_t crtc_base = 0x600000 + (uint32_t)(head * 0x2000);
    uint32_t ramdac_base = 0x680000 + (uint32_t)(head * 0x2000);

    // Program horizontal timing
    nv_wr32(g->mmio, ramdac_base + 0x820, (uint32_t)(mode->htotal - 1));
    nv_wr32(g->mmio, ramdac_base + 0x824, (uint32_t)(mode->hdisplay - 1));
    nv_wr32(g->mmio, ramdac_base + 0x828, (uint32_t)(mode->hdisplay));
    nv_wr32(g->mmio, ramdac_base + 0x82C, (uint32_t)(mode->htotal - 1));
    nv_wr32(g->mmio, ramdac_base + 0x830, (uint32_t)(mode->hsync_start - 1));
    nv_wr32(g->mmio, ramdac_base + 0x834, (uint32_t)(mode->hsync_end - 1));

    // Program vertical timing
    nv_wr32(g->mmio, ramdac_base + 0x840, (uint32_t)(mode->vtotal - 1));
    nv_wr32(g->mmio, ramdac_base + 0x844, (uint32_t)(mode->vdisplay - 1));
    nv_wr32(g->mmio, ramdac_base + 0x848, (uint32_t)(mode->vdisplay));
    nv_wr32(g->mmio, ramdac_base + 0x84C, (uint32_t)(mode->vtotal - 1));
    nv_wr32(g->mmio, ramdac_base + 0x850, (uint32_t)(mode->vsync_start - 1));
    nv_wr32(g->mmio, ramdac_base + 0x854, (uint32_t)(mode->vsync_end - 1));

    // Set framebuffer start address (offset within VRAM)
    nv_wr32(g->mmio, crtc_base + 0x800, g->fb_offset);

    // Set pixel depth (0x1 = 8bpp, 0x3 = 16bpp, 0x5 = 32bpp)
    uint32_t depth_val = 0x5;  // Default 32bpp
    if (mode->bpp == 16) depth_val = 0x3;
    else if (mode->bpp == 8) depth_val = 0x1;
    nv_wr32(g->mmio, crtc_base + 0x804, depth_val);

    // Calculate and set PLL for pixel clock
    nv_pll_t pll;
    if (nv_pll_calc((uint32_t)mode->clock, &pll) == 0) {
        nv_pll_set(head, &pll);
    }

    return 0;
}

// Program CRTC timing on NV50+ hardware via display engine
static int nv_crtc_set_mode_nv50(int head, nv_display_mode_t* mode) {
    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped) return -1;

    // NV50+ uses a unified display engine with EVO channels
    // Direct register programming (simplified; production would use EVO push buffers)

    // Program horizontal timing
    nv_wr32(g->mmio, NV50_HEAD_TOTAL_H(head), (uint32_t)(mode->htotal));
    nv_wr32(g->mmio, NV50_HEAD_SYNC_START_H(head), (uint32_t)(mode->hsync_start));
    nv_wr32(g->mmio, NV50_HEAD_SYNC_END_H(head), (uint32_t)(mode->hsync_end));
    nv_wr32(g->mmio, NV50_HEAD_BLANK_START_H(head), (uint32_t)(mode->hdisplay));
    nv_wr32(g->mmio, NV50_HEAD_BLANK_END_H(head), (uint32_t)(mode->htotal));

    // Program vertical timing
    nv_wr32(g->mmio, NV50_HEAD_TOTAL_V(head), (uint32_t)(mode->vtotal));
    nv_wr32(g->mmio, NV50_HEAD_SYNC_START_V(head), (uint32_t)(mode->vsync_start));
    nv_wr32(g->mmio, NV50_HEAD_SYNC_END_V(head), (uint32_t)(mode->vsync_end));
    nv_wr32(g->mmio, NV50_HEAD_BLANK_START_V(head), (uint32_t)(mode->vdisplay));
    nv_wr32(g->mmio, NV50_HEAD_BLANK_END_V(head), (uint32_t)(mode->vtotal));

    // Set framebuffer configuration
    nv_wr32(g->mmio, NV50_HEAD_FB_OFFSET(head), g->fb_offset);
    nv_wr32(g->mmio, NV50_HEAD_FB_PITCH(head), (uint32_t)(mode->hdisplay * (mode->bpp / 8)));

    // Set pixel format (32bpp XRGB8888)
    uint32_t fmt = 0;
    switch (mode->bpp) {
        case 8:  fmt = 0x1E; break;    // C8 (indexed)
        case 16: fmt = 0xE8; break;    // RGB565
        case 32: fmt = 0xCF; break;    // XRGB8888
        default: fmt = 0xCF; break;
    }
    nv_wr32(g->mmio, NV50_HEAD_FB_DEPTH(head), fmt);

    // Calculate and set PLL for pixel clock
    nv_pll_t pll;
    if (nv_pll_calc((uint32_t)mode->clock, &pll) == 0) {
        nv_pll_set(head, &pll);
    }

    // Enable head
    nv_wr32(g->mmio, NV50_HEAD_CTRL(head), 0x00000001);

    return 0;
}

int nv_display_set_mode(int head, nv_display_mode_t* mode) {
    if (!mode || head < 0 || head >= NV_MAX_HEADS) return -1;

    gpu_state_t* g = gpu_get_state();
    if (!g->initialized) return -1;

    int ret;
    if (g->arch >= NV_ARCH_NV50) {
        ret = nv_crtc_set_mode_nv50(head, mode);
    } else {
        ret = nv_crtc_set_mode_legacy(head, mode);
    }

    if (ret == 0) {
        // Update software state
        disp_memcpy(&display.current_mode, mode, sizeof(nv_display_mode_t));
        display.active_head = head;
        display.mode_set = 1;

        // Also update GPU state
        g->display_width = mode->hdisplay;
        g->display_height = mode->vdisplay;
        g->display_bpp = mode->bpp;
        g->display_pitch = mode->hdisplay * (mode->bpp / 8);
        g->display_active = 1;
    }

    return ret;
}

int nv_display_set_resolution(int width, int height, int bpp) {
    nv_display_mode_t* mode = nv_display_find_mode(width, height);
    if (!mode) return -1;

    // Override bpp if specified
    nv_display_mode_t m;
    disp_memcpy(&m, mode, sizeof(nv_display_mode_t));
    if (bpp > 0) m.bpp = bpp;

    return nv_display_set_mode(display.active_head, &m);
}

void nv_display_get_mode(int head, nv_display_mode_t* mode) {
    if (!mode) return;
    (void)head;
    disp_memcpy(mode, &display.current_mode, sizeof(nv_display_mode_t));
}

// ============================================================
// Framebuffer Scanout Configuration
// ============================================================

void nv_display_set_fb_offset(int head, uint32_t offset) {
    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped) return;

    if (g->arch >= NV_ARCH_NV50) {
        nv_wr32(g->mmio, NV50_HEAD_FB_OFFSET(head), offset);
    } else {
        uint32_t crtc_base = 0x600000 + (uint32_t)(head * 0x2000);
        nv_wr32(g->mmio, crtc_base + 0x800, offset);
    }

    g->fb_offset = offset;
}

void nv_display_set_fb_pitch(int head, uint32_t pitch) {
    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped) return;

    if (g->arch >= NV_ARCH_NV50) {
        nv_wr32(g->mmio, NV50_HEAD_FB_PITCH(head), pitch);
    }
    // Pre-NV50 pitch is configured via CRTC extended registers
}

void nv_display_set_fb_depth(int head, int bpp) {
    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped) return;

    if (g->arch >= NV_ARCH_NV50) {
        uint32_t fmt = 0xCF;  // XRGB8888
        if (bpp == 16) fmt = 0xE8;
        else if (bpp == 8) fmt = 0x1E;
        nv_wr32(g->mmio, NV50_HEAD_FB_DEPTH(head), fmt);
    }
}

// ============================================================
// Hardware Cursor
// ============================================================

// NVIDIA hardware cursor is a 64x64 ARGB image stored in VRAM
// The cursor engine reads directly from VRAM and overlays on the display
// VRAM offset for cursor is set via cursor control registers

// Cursor VRAM allocation: we reserve space at end of first scanout buffer
#define NV_CURSOR_VRAM_OFFSET(head) (4 * 1024 * 1024 + (head) * 64 * 64 * 4)

int nv_cursor_init(int head) {
    if (head < 0 || head >= NV_MAX_HEADS) return -1;

    gpu_state_t* g = gpu_get_state();
    if (!g->vram_mapped) return -1;

    nv_cursor_t* cursor = &display.cursors[head];
    disp_memset(cursor, 0, sizeof(nv_cursor_t));

    cursor->width = 64;
    cursor->height = 64;
    cursor->vram_offset = NV_CURSOR_VRAM_OFFSET(head);

    // Clear cursor VRAM region
    volatile uint32_t* cursor_vram = g->vram + (cursor->vram_offset / 4);
    for (int i = 0; i < 64 * 64; i++) {
        cursor_vram[i] = 0x00000000;  // Fully transparent
    }

    // Set cursor offset in hardware
    if (g->arch >= NV_ARCH_NV50) {
        nv_wr32(g->mmio, NV50_HEAD_CURSOR_OFFSET(head), cursor->vram_offset);
        nv_wr32(g->mmio, NV50_HEAD_CURSOR_CTRL(head), 0x00000000);  // Disabled initially
    }

    cursor->enabled = 1;
    cursor->visible = 0;
    return 0;
}

void nv_cursor_show(int head) {
    if (head < 0 || head >= NV_MAX_HEADS) return;

    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped) return;

    if (g->arch >= NV_ARCH_NV50) {
        // Enable cursor: bit 0 = enable, bits[3:2] = format (0x2 = ARGB8888)
        nv_wr32(g->mmio, NV50_HEAD_CURSOR_CTRL(head), 0x00000009);
    }

    display.cursors[head].visible = 1;
}

void nv_cursor_hide(int head) {
    if (head < 0 || head >= NV_MAX_HEADS) return;

    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped) return;

    if (g->arch >= NV_ARCH_NV50) {
        nv_wr32(g->mmio, NV50_HEAD_CURSOR_CTRL(head), 0x00000000);
    }

    display.cursors[head].visible = 0;
}

void nv_cursor_move(int head, int x, int y) {
    if (head < 0 || head >= NV_MAX_HEADS) return;

    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped) return;

    display.cursors[head].x = x;
    display.cursors[head].y = y;

    // Position encoding: bits[15:0] = x, bits[31:16] = y
    uint32_t pos = ((uint32_t)(y & 0xFFFF) << 16) | ((uint32_t)(x & 0xFFFF));

    if (g->arch >= NV_ARCH_NV50) {
        nv_wr32(g->mmio, NV50_HEAD_CURSOR_POS(head), pos);
    }
}

void nv_cursor_set_image(int head, uint32_t* argb, int w, int h) {
    if (head < 0 || head >= NV_MAX_HEADS) return;
    if (!argb) return;

    gpu_state_t* g = gpu_get_state();
    if (!g->vram_mapped) return;

    nv_cursor_t* cursor = &display.cursors[head];

    // Copy image to VRAM cursor region (64x64 ARGB format required by hardware)
    volatile uint32_t* cursor_vram = g->vram + (cursor->vram_offset / 4);

    // Clear first
    for (int i = 0; i < 64 * 64; i++) {
        cursor_vram[i] = 0x00000000;
    }

    // Copy source image (may be smaller than 64x64)
    int copy_w = (w > 64) ? 64 : w;
    int copy_h = (h > 64) ? 64 : h;
    for (int y = 0; y < copy_h; y++) {
        for (int x = 0; x < copy_w; x++) {
            cursor_vram[y * 64 + x] = argb[y * w + x];
        }
    }

    // Also keep a software copy
    disp_memcpy(cursor->image, argb,
                (uint64_t)(copy_w * copy_h) * sizeof(uint32_t));

    // Update cursor offset in hardware
    if (g->arch >= NV_ARCH_NV50) {
        nv_wr32(g->mmio, NV50_HEAD_CURSOR_OFFSET(head), cursor->vram_offset);
    }
}

// ============================================================
// Output Detection
// ============================================================

int nv_dac_load_detect(int dac) {
    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped) return 0;

    if (g->arch >= NV_ARCH_NV50) {
        // NV50+: DAC load detection via PDISPLAY
        // Write load detect trigger, read result
        nv_wr32(g->mmio, NV50_DAC_LOAD_DETECT(dac), 0x00000001);

        // Wait for detection to complete
        for (volatile int i = 0; i < 100000; i++);

        uint32_t result = nv_rd32(g->mmio, NV50_DAC_LOAD_DETECT(dac));
        return (result & 0x38) == 0x38;  // All three channels loaded = CRT connected
    } else {
        // Pre-NV50: Legacy DAC load detection
        // Simplified — would need to properly program PRAMDAC test registers
        return 0;
    }
}

int nv_sor_detect(int sor) {
    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped) return 0;

    if (g->arch >= NV_ARCH_NV50) {
        // Read SOR state register — connected output has status bits set
        uint32_t state = nv_rd32(g->mmio, NV50_SOR_STATE(sor));
        return (state & 0x02) ? 1 : 0;  // Bit 1 indicates HPD (hot-plug detect)
    }

    return 0;
}

int nv_detect_outputs(void) {
    gpu_state_t* g = gpu_get_state();
    if (!g->initialized) return -1;

    int num_found = 0;

    // Check DACs (analog/VGA outputs)
    display.num_dacs = (g->arch >= NV_ARCH_NV50) ? 3 : 2;
    for (int d = 0; d < display.num_dacs; d++) {
        if (nv_dac_load_detect(d)) {
            nv_connector_t* c = &display.connectors[num_found];
            c->type = NV_OUTPUT_DAC;
            c->active = 1;
            c->dac_index = d;
            c->head = num_found;  // Assign to next available head
            num_found++;
            if (num_found >= NV_MAX_CONNECTORS) break;
        }
    }

    // Check SORs (digital: DVI/HDMI/DP)
    display.num_sors = (g->arch >= NV_ARCH_NV50) ? 4 : 2;
    for (int s = 0; s < display.num_sors && num_found < NV_MAX_CONNECTORS; s++) {
        if (nv_sor_detect(s)) {
            nv_connector_t* c = &display.connectors[num_found];
            c->type = NV_OUTPUT_TMDS;  // Could be DVI or HDMI; would need EDID to distinguish
            c->active = 1;
            c->sor_index = s;
            c->head = num_found < NV_MAX_HEADS ? num_found : 0;
            num_found++;
        }
    }

    return num_found;
}

// ============================================================
// VBlank
// ============================================================

void nv_display_wait_vblank(int head) {
    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped) return;

    if (g->arch >= NV_ARCH_NV50) {
        // NV50+: Read display interrupt status, wait for vblank bit
        uint32_t vblank_bit = (1 << (head * 2));

        // Clear existing vblank interrupt
        nv_wr32(g->mmio, NV50_DISP_INTR_0, vblank_bit);

        // Wait for next vblank (with timeout)
        int timeout = 1000000;
        while (timeout-- > 0) {
            uint32_t intr = nv_rd32(g->mmio, NV50_DISP_INTR_0);
            if (intr & vblank_bit) break;
        }

        // Clear the interrupt
        nv_wr32(g->mmio, NV50_DISP_INTR_0, vblank_bit);
    } else {
        // Pre-NV50: PCRTC interrupt-based vblank
        uint32_t crtc_base = 0x600000 + (uint32_t)(head * 0x2000);

        // Clear vblank interrupt
        nv_wr32(g->mmio, crtc_base + 0x100, 0x00000001);

        // Wait for it to fire
        int timeout = 1000000;
        while (timeout-- > 0) {
            uint32_t intr = nv_rd32(g->mmio, crtc_base + 0x100);
            if (intr & 0x00000001) break;
        }

        nv_wr32(g->mmio, crtc_base + 0x100, 0x00000001);
    }
}

// ============================================================
// Initialization
// ============================================================

int nv_display_init(void) {
    gpu_state_t* g = gpu_get_state();
    if (!g->initialized) return -1;

    disp_memset(&display, 0, sizeof(nv_display_state_t));

    display.num_heads = NV_MAX_HEADS;

    // Detect connected outputs
    int num_outputs = nv_detect_outputs();
    (void)num_outputs;

    // Initialize hardware cursor for head 0
    if (g->vram_mapped) {
        nv_cursor_init(0);
    }

    // If no outputs detected, set up default mode anyway
    // (display will use whatever output the VBIOS configured)
    if (!display.mode_set) {
        nv_display_mode_t* default_mode = nv_display_find_mode(1024, 768);
        if (default_mode) {
            disp_memcpy(&display.current_mode, default_mode, sizeof(nv_display_mode_t));
        }
    }

    return 0;
}

void nv_display_shutdown(void) {
    // Hide cursors
    for (int h = 0; h < NV_MAX_HEADS; h++) {
        nv_cursor_hide(h);
    }

    gpu_state_t* g = gpu_get_state();
    if (!g->mmio_mapped) return;

    // Disable display interrupts
    if (g->arch >= NV_ARCH_NV50) {
        nv_wr32(g->mmio, NV50_DISP_INTR_EN, 0x00000000);
    }

    display.mode_set = 0;
}

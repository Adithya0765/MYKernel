// nv_display.h - NVIDIA Display Engine / Mode Setting for Alteo OS
// Covers pre-NV50 PCRTC/PRAMDAC and NV50+ unified display engine (PDISPLAY)
// Supports: CRTC programming, PLL configuration, mode setting, hardware cursor
#ifndef NV_DISPLAY_H
#define NV_DISPLAY_H

#include "stdint.h"
#include "gpu.h"

// ============================================================
// Pre-NV50 Display Registers (PCRTC + PRAMDAC)
// ============================================================

// PCRTC registers (BAR0 + 0x600000)
#define NV_PCRTC_INTR_0            0x600100    // CRTC interrupt status
#define NV_PCRTC_INTR_EN_0         0x600140    // CRTC interrupt enable
#define NV_PCRTC_START             0x600800    // Framebuffer start address in VRAM
#define NV_PCRTC_CONFIG            0x600804    // Display configuration

// PRAMDAC registers (BAR0 + 0x680000)
#define NV_PRAMDAC_VPLL_COEFF      0x680500    // VPLL coefficients (N, M, P)
#define NV_PRAMDAC_VPLL2_COEFF     0x680520    // Second VPLL (for dual-head)
#define NV_PRAMDAC_PLL_COEFF_SEL   0x680510    // PLL coefficient select
#define NV_PRAMDAC_GENERAL_CTRL    0x680600    // General RAMDAC control
#define NV_PRAMDAC_FP_HTOTAL       0x680820    // Flat panel H total
#define NV_PRAMDAC_FP_HDISP_END   0x680824    // Flat panel H display end
#define NV_PRAMDAC_FP_HBLANK_S    0x680828    // H blank start
#define NV_PRAMDAC_FP_HBLANK_E    0x68082C    // H blank end
#define NV_PRAMDAC_FP_HSYNC_S     0x680830    // H sync start
#define NV_PRAMDAC_FP_HSYNC_E     0x680834    // H sync end
#define NV_PRAMDAC_FP_VTOTAL      0x680840    // V total
#define NV_PRAMDAC_FP_VDISP_END   0x680844    // V display end
#define NV_PRAMDAC_FP_VBLANK_S    0x680848    // V blank start
#define NV_PRAMDAC_FP_VBLANK_E    0x68084C    // V blank end
#define NV_PRAMDAC_FP_VSYNC_S     0x680850    // V sync start
#define NV_PRAMDAC_FP_VSYNC_E     0x680854    // V sync end

// ============================================================
// NV50+ Unified Display Engine (PDISPLAY) Registers
// ============================================================

// Supervisor / Core
#define NV50_DISP_SUPERVISOR       0x610030    // Display supervisor channel
#define NV50_DISP_INTR_0           0x610020    // Display interrupt status
#define NV50_DISP_INTR_EN          0x610028    // Display interrupt enable

// Display EVO (Evolution) channel descriptors
#define NV50_DISP_CORE_CHANNEL     0x610200    // Core display channel base
#define NV50_DISP_CORE_CTRL        0x610200    // Core channel control
#define NV50_DISP_CORE_STATE       0x610204    // Core channel state

// Per-head (CRTC) registers — head 0 at +0x300, head 1 at +0x340
#define NV50_HEAD_BASE(h)          (0x610300 + (h) * 0x540)
#define NV50_HEAD_CTRL(h)          (NV50_HEAD_BASE(h) + 0x000)
#define NV50_HEAD_SYNC_START_H(h)  (NV50_HEAD_BASE(h) + 0x004)
#define NV50_HEAD_SYNC_END_H(h)    (NV50_HEAD_BASE(h) + 0x008)
#define NV50_HEAD_BLANK_START_H(h) (NV50_HEAD_BASE(h) + 0x00C)
#define NV50_HEAD_BLANK_END_H(h)   (NV50_HEAD_BASE(h) + 0x010)
#define NV50_HEAD_TOTAL_H(h)       (NV50_HEAD_BASE(h) + 0x014)
#define NV50_HEAD_SYNC_START_V(h)  (NV50_HEAD_BASE(h) + 0x018)
#define NV50_HEAD_SYNC_END_V(h)    (NV50_HEAD_BASE(h) + 0x01C)
#define NV50_HEAD_BLANK_START_V(h) (NV50_HEAD_BASE(h) + 0x020)
#define NV50_HEAD_BLANK_END_V(h)   (NV50_HEAD_BASE(h) + 0x024)
#define NV50_HEAD_TOTAL_V(h)       (NV50_HEAD_BASE(h) + 0x028)
#define NV50_HEAD_CLK_CTRL(h)      (NV50_HEAD_BASE(h) + 0x02C)
#define NV50_HEAD_FMT_CTRL(h)      (NV50_HEAD_BASE(h) + 0x030)
#define NV50_HEAD_FB_OFFSET(h)     (NV50_HEAD_BASE(h) + 0x060)
#define NV50_HEAD_FB_SIZE(h)       (NV50_HEAD_BASE(h) + 0x068)
#define NV50_HEAD_FB_PITCH(h)      (NV50_HEAD_BASE(h) + 0x06C)
#define NV50_HEAD_FB_DEPTH(h)      (NV50_HEAD_BASE(h) + 0x070)

// Per-head cursor control
#define NV50_HEAD_CURSOR_CTRL(h)   (NV50_HEAD_BASE(h) + 0x080)
#define NV50_HEAD_CURSOR_OFFSET(h) (NV50_HEAD_BASE(h) + 0x084)
#define NV50_HEAD_CURSOR_POS(h)    (NV50_HEAD_BASE(h) + 0x088)

// DAC (analog) output registers
#define NV50_DAC_BASE(d)           (0x610A00 + (d) * 0x80)
#define NV50_DAC_CTRL(d)           (NV50_DAC_BASE(d) + 0x000)
#define NV50_DAC_LOAD_DETECT(d)    (NV50_DAC_BASE(d) + 0x00C)

// SOR (Serial Output Resource) for DVI/HDMI/DP
#define NV50_SOR_BASE(s)           (0x610B00 + (s) * 0x80)
#define NV50_SOR_CTRL(s)           (NV50_SOR_BASE(s) + 0x000)
#define NV50_SOR_STATE(s)          (NV50_SOR_BASE(s) + 0x004)
#define NV50_SOR_DPMS(s)           (NV50_SOR_BASE(s) + 0x008)

// PLL registers (NV50+ uses PPLL block)
#define NV50_PPLL_BASE(p)          (0x614000 + (p) * 0x800)
#define NV50_PPLL_CTRL(p)          (NV50_PPLL_BASE(p) + 0x000)
#define NV50_PPLL_COEFF(p)         (NV50_PPLL_BASE(p) + 0x004)

// ============================================================
// Mode / Timing Structures
// ============================================================

// Standard CRTC timing descriptor (similar to Linux drm_display_mode)
typedef struct {
    int clock;          // Pixel clock in kHz
    int hdisplay;       // Horizontal display area
    int hsync_start;    // H sync pulse start
    int hsync_end;      // H sync pulse end
    int htotal;         // Total horizontal pixels (incl. blanking)
    int vdisplay;       // Vertical display area
    int vsync_start;    // V sync pulse start
    int vsync_end;      // V sync pulse end
    int vtotal;         // Total vertical lines (incl. blanking)
    int bpp;            // Bits per pixel (usually 32)
    uint32_t flags;     // Mode flags
} nv_display_mode_t;

// Mode flags
#define NV_MODE_FLAG_HSYNC_POS  (1 << 0)
#define NV_MODE_FLAG_HSYNC_NEG  (1 << 1)
#define NV_MODE_FLAG_VSYNC_POS  (1 << 2)
#define NV_MODE_FLAG_VSYNC_NEG  (1 << 3)
#define NV_MODE_FLAG_INTERLACE  (1 << 4)
#define NV_MODE_FLAG_DOUBLESCAN (1 << 5)

// Output types
#define NV_OUTPUT_NONE          0
#define NV_OUTPUT_DAC           1   // VGA analog (CRT)
#define NV_OUTPUT_TMDS          2   // DVI digital
#define NV_OUTPUT_LVDS          3   // Laptop panel
#define NV_OUTPUT_DP            4   // DisplayPort
#define NV_OUTPUT_HDMI          5   // HDMI

// Connector state
typedef struct {
    int  type;              // NV_OUTPUT_*
    int  active;            // Currently connected?
    int  head;              // Which CRTC head drives this output
    int  sor_index;         // SOR index (for digital outputs)
    int  dac_index;         // DAC index (for analog outputs)
    nv_display_mode_t mode; // Current display mode
} nv_connector_t;

// PLL parameters
typedef struct {
    uint32_t n;         // PLL multiplier
    uint32_t m;         // PLL divider
    uint32_t p;         // Post-divider (log2)
    uint32_t refclk;    // Reference clock in kHz (usually 27000)
} nv_pll_t;

// Hardware cursor state
typedef struct {
    int      enabled;
    int      visible;
    int      x, y;
    uint32_t vram_offset;       // Offset in VRAM for cursor image
    int      width, height;     // Cursor dimensions (typically 64x64)
    uint32_t image[64 * 64];    // Cursor ARGB image data
} nv_cursor_t;

// Display state (per GPU)
typedef struct {
    int            num_heads;
    int            num_dacs;
    int            num_sors;
    nv_connector_t connectors[NV_MAX_CONNECTORS];
    nv_cursor_t    cursors[NV_MAX_HEADS];
    nv_display_mode_t current_mode;
    int            active_head;
    int            mode_set;
} nv_display_state_t;

// ---- Display Initialization ----
int  nv_display_init(void);                 // Detect outputs, set up display engine
void nv_display_shutdown(void);             // Disable display outputs

// ---- Mode Setting ----
int  nv_display_set_mode(int head, nv_display_mode_t* mode);  // Program CRTC timing
int  nv_display_set_resolution(int width, int height, int bpp);
void nv_display_get_mode(int head, nv_display_mode_t* mode);  // Read current mode
nv_display_mode_t* nv_display_find_mode(int width, int height); // Find standard mode

// ---- PLL ----
int  nv_pll_calc(uint32_t target_khz, nv_pll_t* pll);  // Calculate PLL coefficients
void nv_pll_set(int pll_index, nv_pll_t* pll);          // Program PLL hardware

// ---- Framebuffer Scanout ----
void nv_display_set_fb_offset(int head, uint32_t offset);  // Set VRAM scanout base
void nv_display_set_fb_pitch(int head, uint32_t pitch);    // Set scanout pitch
void nv_display_set_fb_depth(int head, int bpp);           // Set pixel format

// ---- Hardware Cursor ----
int  nv_cursor_init(int head);                              // Allocate cursor VRAM
void nv_cursor_show(int head);
void nv_cursor_hide(int head);
void nv_cursor_move(int head, int x, int y);
void nv_cursor_set_image(int head, uint32_t* argb, int w, int h);

// ---- Output Detection ----
int  nv_detect_outputs(void);               // Probe DAC/SOR for connected monitors
int  nv_dac_load_detect(int dac);           // Check if CRT is connected to DAC
int  nv_sor_detect(int sor);                // Check SOR connection (DVI/HDMI/DP)

// ---- VBlank ----
void nv_display_wait_vblank(int head);      // Wait for vertical blank period

#endif

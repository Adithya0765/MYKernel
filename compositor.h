// compositor.h - GPU-Accelerated Window Compositor for Alteo OS
// Composites windows, manages surfaces, handles damage tracking & vsync
#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include "stdint.h"

// ============================================================
// Configuration
// ============================================================

#define COMP_MAX_WINDOWS        64
#define COMP_MAX_LAYERS         8
#define COMP_DAMAGE_RECTS       32

// ============================================================
// Surface Format
// ============================================================

#define SURFACE_FORMAT_ARGB8888  0
#define SURFACE_FORMAT_XRGB8888  1
#define SURFACE_FORMAT_RGB565    2

// ============================================================
// Window Flags
// ============================================================

#define COMP_WIN_VISIBLE        0x01
#define COMP_WIN_DECORATED      0x02
#define COMP_WIN_FOCUSED        0x04
#define COMP_WIN_FULLSCREEN     0x08
#define COMP_WIN_MINIMIZED      0x10
#define COMP_WIN_MAXIMIZED      0x20
#define COMP_WIN_TRANSPARENT    0x40
#define COMP_WIN_ALWAYS_ON_TOP  0x80

// ============================================================
// Layer IDs (back to front)
// ============================================================

#define LAYER_WALLPAPER     0
#define LAYER_DESKTOP       1
#define LAYER_NORMAL        2
#define LAYER_ABOVE         3
#define LAYER_NOTIFICATION  4
#define LAYER_OVERLAY       5
#define LAYER_CURSOR        6
#define LAYER_NUM           7

// ============================================================
// Animation Types
// ============================================================

#define ANIM_NONE           0
#define ANIM_FADE_IN        1
#define ANIM_FADE_OUT       2
#define ANIM_SLIDE_UP       3
#define ANIM_SLIDE_DOWN     4
#define ANIM_SCALE_IN       5
#define ANIM_SCALE_OUT      6

// ============================================================
// Structures
// ============================================================

// Damage rectangle
typedef struct {
    int x, y, w, h;
} comp_rect_t;

// Surface - a renderable pixel buffer
typedef struct {
    uint32_t* pixels;       // Pixel data (CPU-accessible)
    int       width;
    int       height;
    int       pitch;        // In pixels
    int       format;       // SURFACE_FORMAT_*
    uint32_t  vram_offset;  // GPU VRAM offset (0 = CPU only)
    int       dirty;        // Needs re-upload to GPU
} comp_surface_t;

// Window
typedef struct {
    int             id;
    int             x, y;
    int             width, height;
    uint32_t        flags;
    int             layer;
    uint8_t         opacity;        // 0-255
    int             z_order;        // Within layer

    comp_surface_t  surface;        // Client content
    comp_surface_t  decoration;     // Title bar + border
    int             dec_height;     // Decoration height (title bar)

    // Animation state
    int             anim_type;
    int             anim_frame;
    int             anim_total;     // Total frames for animation
    float           anim_progress;  // 0.0 - 1.0

    // Title
    char            title[64];

    // Damage tracking
    comp_rect_t     damage[COMP_DAMAGE_RECTS];
    int             damage_count;
    int             full_damage;    // Entire window needs redraw

    int             active;
} comp_window_t;

// Compositor state
typedef struct {
    int             initialized;
    int             screen_w, screen_h;

    // Windows
    comp_window_t   windows[COMP_MAX_WINDOWS];
    int             window_count;
    int             focused_id;
    int             next_window_id;

    // Composition buffer
    comp_surface_t  frontbuf;       // Display output
    comp_surface_t  backbuf;        // Composition target

    // Background
    comp_surface_t  wallpaper;
    uint32_t        bg_color;       // Fallback solid color

    // Cursor
    int             cursor_x, cursor_y;
    comp_surface_t  cursor_surface;
    int             cursor_visible;

    // Global damage
    comp_rect_t     damage[COMP_DAMAGE_RECTS];
    int             damage_count;
    int             full_redraw;

    // Stats
    uint32_t        frame_count;
    uint32_t        composite_time_us;

    // Vsync
    int             vsync_enabled;

    // Use GPU acceleration
    int             use_gpu;
} comp_state_t;

// ============================================================
// API
// ============================================================

// ---- Core ----
int  compositor_init(void);
void compositor_shutdown(void);
void compositor_composite(void);
void compositor_flip(void);

// ---- Windows ----
int  compositor_create_window(int x, int y, int w, int h, uint32_t flags);
void compositor_destroy_window(int win_id);
void compositor_move_window(int win_id, int x, int y);
void compositor_resize_window(int win_id, int w, int h);
void compositor_set_title(int win_id, const char* title);
void compositor_show_window(int win_id);
void compositor_hide_window(int win_id);
void compositor_focus_window(int win_id);
void compositor_set_layer(int win_id, int layer);
void compositor_set_opacity(int win_id, uint8_t opacity);

// ---- Surface Access ----
comp_surface_t* compositor_get_surface(int win_id);
void compositor_window_damage(int win_id, int x, int y, int w, int h);
void compositor_window_damage_full(int win_id);

// ---- Background ----
void compositor_set_wallpaper(const uint32_t* pixels, int w, int h);
void compositor_set_bg_color(uint32_t color);

// ---- Cursor ----
void compositor_set_cursor(const uint32_t* pixels, int w, int h);
void compositor_move_cursor(int x, int y);
void compositor_show_cursor(int show);

// ---- Animation ----
void compositor_animate_window(int win_id, int anim_type, int frames);

// ---- Query ----
int  compositor_window_at(int x, int y);
comp_window_t* compositor_get_window(int win_id);

#endif

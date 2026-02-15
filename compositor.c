// compositor.c - GPU-Accelerated Window Compositor for Alteo OS
// Composites window surfaces, handles damage tracking, vsync, animations
#include "compositor.h"
#include "gpu.h"
#include "nv_2d.h"
#include "nv_display.h"
#include "graphics.h"
#include "heap.h"
#include "font.h"

// ============================================================
// External References
// ============================================================

extern gpu_state_t   gpu_state;
extern nv_2d_state_t nv_2d_state;

extern uint32_t* framebuffer;
extern uint32_t  fb_pitch;
extern uint32_t  backbuf[];
extern int       screen_width;
extern int       screen_height;

// Graphics helpers
extern void draw_rect(int x, int y, int w, int h, uint32_t color);
extern void draw_char(int x, int y, char c, uint32_t color);
extern void draw_string(int x, int y, const char* str, uint32_t color);

// ============================================================
// Global State
// ============================================================

static comp_state_t comp;

// ============================================================
// Internal Helpers
// ============================================================

static int str_len(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int rect_intersect(comp_rect_t* a, comp_rect_t* b, comp_rect_t* out) {
    int x1 = a->x > b->x ? a->x : b->x;
    int y1 = a->y > b->y ? a->y : b->y;
    int x2a = a->x + a->w;
    int x2b = b->x + b->w;
    int x2 = x2a < x2b ? x2a : x2b;
    int y2a = a->y + a->h;
    int y2b = b->y + b->h;
    int y2 = y2a < y2b ? y2a : y2b;

    if (x2 <= x1 || y2 <= y1) return 0;
    if (out) { out->x = x1; out->y = y1; out->w = x2 - x1; out->h = y2 - y1; }
    return 1;
}

static void add_damage(comp_rect_t* list, int* count, int max_rects,
                        int x, int y, int w, int h) {
    if (*count >= max_rects) {
        // Merge all into one bounding rect
        int minx = list[0].x, miny = list[0].y;
        int maxx = list[0].x + list[0].w, maxy = list[0].y + list[0].h;
        for (int i = 1; i < *count; i++) {
            if (list[i].x < minx) minx = list[i].x;
            if (list[i].y < miny) miny = list[i].y;
            if (list[i].x + list[i].w > maxx) maxx = list[i].x + list[i].w;
            if (list[i].y + list[i].h > maxy) maxy = list[i].y + list[i].h;
        }
        // Add new rect to bounding
        if (x < minx) minx = x;
        if (y < miny) miny = y;
        if (x + w > maxx) maxx = x + w;
        if (y + h > maxy) maxy = y + h;
        list[0].x = minx; list[0].y = miny;
        list[0].w = maxx - minx; list[0].h = maxy - miny;
        *count = 1;
        return;
    }
    list[*count].x = x;
    list[*count].y = y;
    list[*count].w = w;
    list[*count].h = h;
    (*count)++;
}

// ============================================================
// Surface Management
// ============================================================

static int surface_alloc(comp_surface_t* surf, int w, int h) {
    surf->width = w;
    surf->height = h;
    surf->pitch = w;
    surf->format = SURFACE_FORMAT_ARGB8888;
    surf->vram_offset = 0;
    surf->dirty = 1;
    surf->pixels = (uint32_t*)kmalloc(w * h * 4);
    if (!surf->pixels) return -1;
    // Clear to transparent
    for (int i = 0; i < w * h; i++) {
        surf->pixels[i] = 0x00000000;
    }
    return 0;
}

static void surface_free(comp_surface_t* surf) {
    if (surf->pixels) {
        kfree(surf->pixels);
        surf->pixels = 0;
    }
    surf->width = 0;
    surf->height = 0;
}

static void surface_clear(comp_surface_t* surf, uint32_t color) {
    if (!surf->pixels) return;
    for (int i = 0; i < surf->width * surf->height; i++) {
        surf->pixels[i] = color;
    }
    surf->dirty = 1;
}

// ============================================================
// Window Decoration Drawing
// ============================================================

#define DECO_TITLE_HEIGHT   24
#define DECO_BORDER_WIDTH   2
#define DECO_TITLE_COLOR_FOCUSED    0xFF4488CC
#define DECO_TITLE_COLOR_UNFOCUSED  0xFF888888
#define DECO_BORDER_COLOR           0xFF333333
#define DECO_CLOSE_COLOR            0xFFCC4444
#define DECO_TEXT_COLOR             0xFFFFFFFF

static void draw_decoration(comp_window_t* win) {
    if (!(win->flags & COMP_WIN_DECORATED)) return;

    comp_surface_t* dec = &win->decoration;
    if (!dec->pixels) return;

    int w = dec->width;
    int h = dec->height;

    // Clear decoration
    surface_clear(dec, 0x00000000);

    uint32_t title_color = (win->flags & COMP_WIN_FOCUSED)
                           ? DECO_TITLE_COLOR_FOCUSED
                           : DECO_TITLE_COLOR_UNFOCUSED;

    // Title bar background
    for (int y = 0; y < DECO_TITLE_HEIGHT && y < h; y++) {
        for (int x = 0; x < w; x++) {
            dec->pixels[y * w + x] = title_color;
        }
    }

    // Border (left, right, bottom)
    for (int y = DECO_TITLE_HEIGHT; y < h; y++) {
        for (int x = 0; x < DECO_BORDER_WIDTH && x < w; x++) {
            dec->pixels[y * w + x] = DECO_BORDER_COLOR;
        }
        for (int x = w - DECO_BORDER_WIDTH; x < w; x++) {
            if (x >= 0) dec->pixels[y * w + x] = DECO_BORDER_COLOR;
        }
    }
    // Bottom border
    for (int y = h - DECO_BORDER_WIDTH; y < h; y++) {
        if (y >= 0) {
            for (int x = 0; x < w; x++) {
                dec->pixels[y * w + x] = DECO_BORDER_COLOR;
            }
        }
    }

    // Close button (red square at top-right)
    int close_x = w - DECO_TITLE_HEIGHT;
    for (int y = 2; y < DECO_TITLE_HEIGHT - 2; y++) {
        for (int x = close_x + 2; x < w - 2; x++) {
            if (x >= 0 && x < w) {
                dec->pixels[y * w + x] = DECO_CLOSE_COLOR;
            }
        }
    }
    // X in close button
    int cx = close_x + (DECO_TITLE_HEIGHT - 4) / 2 + 2;
    int cy = DECO_TITLE_HEIGHT / 2;
    for (int i = -4; i <= 4; i++) {
        int px = cx + i, py1 = cy + i, py2 = cy - i;
        if (px >= 0 && px < w) {
            if (py1 >= 0 && py1 < h) dec->pixels[py1 * w + px] = 0xFFFFFFFF;
            if (py2 >= 0 && py2 < h) dec->pixels[py2 * w + px] = 0xFFFFFFFF;
        }
    }

    // Title text
    int tx = 6;
    int ty = 4;
    int title_len = str_len(win->title);
    for (int i = 0; i < title_len && tx + 8 < close_x; i++) {
        // Draw each char pixel-by-pixel into the decoration surface
        // (Simple embedded 8x16 font rendering into surface)
        for (int gy = 0; gy < 16 && (ty + gy) < h; gy++) {
            for (int gx = 0; gx < 8 && (tx + gx) < w; gx++) {
                // Use the global font to get glyph pixels
                // For simplicity, we'll render title at composite time via draw_string
            }
        }
        tx += 8;
    }

    dec->dirty = 1;
}

// ============================================================
// Compositing
// ============================================================

// Alpha-blend src pixel over dst (ARGB8888)
static uint32_t alpha_blend(uint32_t dst, uint32_t src, uint8_t extra_alpha) {
    uint32_t sa = ((src >> 24) & 0xFF) * extra_alpha / 255;
    if (sa == 0) return dst;
    if (sa == 255) return src | 0xFF000000;

    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >> 8)  & 0xFF;
    uint32_t sb = src & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >> 8)  & 0xFF;
    uint32_t db = dst & 0xFF;

    uint32_t inv = 255 - sa;
    uint32_t or_val = (sr * sa + dr * inv) / 255;
    uint32_t og = (sg * sa + dg * inv) / 255;
    uint32_t ob = (sb * sa + db * inv) / 255;

    return 0xFF000000 | (or_val << 16) | (og << 8) | ob;
}

// Blit surface onto backbuffer at (dx, dy) with clipping
static void blit_surface(comp_surface_t* surf, int dx, int dy, uint8_t opacity,
                          comp_rect_t* clip) {
    if (!surf || !surf->pixels) return;

    int sx_start = 0, sy_start = 0;
    int blit_w = surf->width, blit_h = surf->height;

    // Clip to screen
    if (dx < 0) { sx_start = -dx; blit_w += dx; dx = 0; }
    if (dy < 0) { sy_start = -dy; blit_h += dy; dy = 0; }
    if (dx + blit_w > comp.screen_w) blit_w = comp.screen_w - dx;
    if (dy + blit_h > comp.screen_h) blit_h = comp.screen_h - dy;

    if (blit_w <= 0 || blit_h <= 0) return;

    // Additional clip rect
    if (clip) {
        if (dx < clip->x) {
            int d = clip->x - dx;
            sx_start += d; blit_w -= d; dx = clip->x;
        }
        if (dy < clip->y) {
            int d = clip->y - dy;
            sy_start += d; blit_h -= d; dy = clip->y;
        }
        if (dx + blit_w > clip->x + clip->w) blit_w = clip->x + clip->w - dx;
        if (dy + blit_h > clip->y + clip->h) blit_h = clip->y + clip->h - dy;
        if (blit_w <= 0 || blit_h <= 0) return;
    }

    for (int y = 0; y < blit_h; y++) {
        int src_y = sy_start + y;
        int dst_y = dy + y;
        for (int x = 0; x < blit_w; x++) {
            int src_x = sx_start + x;
            int dst_x = dx + x;
            uint32_t src_pixel = surf->pixels[src_y * surf->pitch + src_x];
            uint32_t dst_pixel = backbuf[dst_y * screen_width + dst_x];
            backbuf[dst_y * screen_width + dst_x] = alpha_blend(dst_pixel, src_pixel, opacity);
        }
    }
}

// Apply animation transform to get effective position/opacity
static void apply_animation(comp_window_t* win, int* eff_x, int* eff_y,
                             uint8_t* eff_opacity) {
    *eff_x = win->x;
    *eff_y = win->y;
    *eff_opacity = win->opacity;

    if (win->anim_type == ANIM_NONE || win->anim_total <= 0) return;

    float t = win->anim_progress;

    switch (win->anim_type) {
    case ANIM_FADE_IN:
        *eff_opacity = (uint8_t)(t * (float)win->opacity);
        break;
    case ANIM_FADE_OUT:
        *eff_opacity = (uint8_t)((1.0f - t) * (float)win->opacity);
        break;
    case ANIM_SLIDE_UP:
        *eff_y = win->y + (int)((1.0f - t) * 50.0f);
        *eff_opacity = (uint8_t)(t * (float)win->opacity);
        break;
    case ANIM_SLIDE_DOWN:
        *eff_y = win->y - (int)((1.0f - t) * 50.0f);
        *eff_opacity = (uint8_t)(t * (float)win->opacity);
        break;
    case ANIM_SCALE_IN:
        // Scale effect approximated by opacity fade
        *eff_opacity = (uint8_t)(t * (float)win->opacity);
        break;
    case ANIM_SCALE_OUT:
        *eff_opacity = (uint8_t)((1.0f - t) * (float)win->opacity);
        break;
    }
}

// Advance animation state
static void tick_animation(comp_window_t* win) {
    if (win->anim_type == ANIM_NONE || win->anim_total <= 0) return;

    win->anim_frame++;
    win->anim_progress = (float)win->anim_frame / (float)win->anim_total;

    if (win->anim_frame >= win->anim_total) {
        // Animation done
        if (win->anim_type == ANIM_FADE_OUT || win->anim_type == ANIM_SCALE_OUT) {
            win->flags &= ~COMP_WIN_VISIBLE;
        }
        win->anim_type = ANIM_NONE;
        win->anim_frame = 0;
        win->anim_progress = 0.0f;
    }
}

// ============================================================
// Initialization
// ============================================================

int compositor_init(void) {
    // Zero state
    for (int i = 0; i < (int)sizeof(comp); i++) {
        ((char*)&comp)[i] = 0;
    }

    comp.screen_w = screen_width;
    comp.screen_h = screen_height;
    comp.next_window_id = 1;
    comp.focused_id = -1;
    comp.bg_color = 0xFF2D5F8A;  // Nice blue
    comp.cursor_visible = 1;
    comp.vsync_enabled = 1;

    // The backbuffer is the existing graphics.c backbuf
    // We composite into it, then flip_buffer() copies to framebuffer
    comp.backbuf.pixels = backbuf;
    comp.backbuf.width = screen_width;
    comp.backbuf.height = screen_height;
    comp.backbuf.pitch = screen_width;
    comp.backbuf.format = SURFACE_FORMAT_ARGB8888;

    comp.use_gpu = gpu_state.initialized;
    comp.full_redraw = 1;

    // Default cursor (simple arrow, 16x16)
    if (surface_alloc(&comp.cursor_surface, 16, 16) == 0) {
        uint32_t* cur = comp.cursor_surface.pixels;
        // Simple arrow cursor
        static const uint8_t arrow[] = {
            0x80,0x00, 0xC0,0x00, 0xE0,0x00, 0xF0,0x00,
            0xF8,0x00, 0xFC,0x00, 0xFE,0x00, 0xFF,0x00,
            0xFF,0x80, 0xFC,0x00, 0xF6,0x00, 0xE3,0x00,
            0xC3,0x00, 0x81,0x80, 0x01,0x80, 0x00,0xC0
        };
        for (int y = 0; y < 16; y++) {
            uint16_t row = ((uint16_t)arrow[y*2] << 8) | arrow[y*2+1];
            for (int x = 0; x < 16; x++) {
                if (row & (0x8000 >> x)) {
                    cur[y * 16 + x] = 0xFFFFFFFF;
                } else {
                    cur[y * 16 + x] = 0x00000000;
                }
            }
        }
    }

    comp.initialized = 1;
    return 0;
}

void compositor_shutdown(void) {
    // Free all window surfaces
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        if (comp.windows[i].active) {
            surface_free(&comp.windows[i].surface);
            surface_free(&comp.windows[i].decoration);
        }
    }
    surface_free(&comp.wallpaper);
    surface_free(&comp.cursor_surface);
    comp.initialized = 0;
}

// ============================================================
// Compositing
// ============================================================

void compositor_composite(void) {
    if (!comp.initialized) return;

    // Step 1: Draw background
    if (comp.wallpaper.pixels) {
        // Blit wallpaper
        blit_surface(&comp.wallpaper, 0, 0, 255, 0);
    } else {
        // Solid color fill
        for (int i = 0; i < comp.screen_w * comp.screen_h; i++) {
            backbuf[i] = comp.bg_color;
        }
    }

    // Step 2: Composite windows back-to-front by layer, then z-order
    for (int layer = 0; layer < LAYER_NUM; layer++) {
        // Find windows in this layer, sorted by z_order
        for (int z = 0; z < COMP_MAX_WINDOWS; z++) {
            for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
                comp_window_t* win = &comp.windows[i];
                if (!win->active) continue;
                if (!(win->flags & COMP_WIN_VISIBLE)) continue;
                if (win->layer != layer) continue;
                if (win->z_order != z) continue;

                int eff_x, eff_y;
                uint8_t eff_opacity;
                apply_animation(win, &eff_x, &eff_y, &eff_opacity);

                if (eff_opacity == 0) continue;

                // Draw decoration first (if decorated)
                if ((win->flags & COMP_WIN_DECORATED) && win->decoration.pixels) {
                    draw_decoration(win);
                    blit_surface(&win->decoration, eff_x - DECO_BORDER_WIDTH,
                                eff_y - DECO_TITLE_HEIGHT, eff_opacity, 0);

                    // Draw title text on top of decoration
                    if (win->title[0]) {
                        int tx = eff_x + 4;
                        int ty = eff_y - DECO_TITLE_HEIGHT + 4;
                        // Use graphics draw_string for title
                        draw_string(tx, ty, win->title, DECO_TEXT_COLOR);
                    }
                }

                // Draw window content
                blit_surface(&win->surface, eff_x, eff_y, eff_opacity, 0);

                // Tick animation
                tick_animation(win);
            }
        }
    }

    // Step 3: Draw cursor
    if (comp.cursor_visible && comp.cursor_surface.pixels) {
        blit_surface(&comp.cursor_surface, comp.cursor_x, comp.cursor_y, 255, 0);
    }

    comp.frame_count++;
    comp.full_redraw = 0;
}

void compositor_flip(void) {
    // Copy backbuf to framebuffer (using existing graphics infrastructure)
    // flip_buffer() in graphics.c does this
    extern void flip_buffer(void);
    flip_buffer();
}

// ============================================================
// Window Management
// ============================================================

int compositor_create_window(int x, int y, int w, int h, uint32_t flags) {
    // Find free slot
    int slot = -1;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        if (!comp.windows[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;

    comp_window_t* win = &comp.windows[slot];
    for (int i = 0; i < (int)sizeof(comp_window_t); i++) {
        ((char*)win)[i] = 0;
    }

    win->id = comp.next_window_id++;
    win->x = x;
    win->y = y;
    win->width = w;
    win->height = h;
    win->flags = flags;
    win->layer = LAYER_NORMAL;
    win->opacity = 255;
    win->z_order = comp.window_count;
    win->active = 1;

    // Allocate surface
    if (surface_alloc(&win->surface, w, h) < 0) {
        win->active = 0;
        return -1;
    }

    // Clear to white
    surface_clear(&win->surface, 0xFFFFFFFF);

    // Allocate decoration if decorated
    if (flags & COMP_WIN_DECORATED) {
        int dec_w = w + 2 * DECO_BORDER_WIDTH;
        int dec_h = h + DECO_TITLE_HEIGHT + DECO_BORDER_WIDTH;
        win->dec_height = DECO_TITLE_HEIGHT;
        if (surface_alloc(&win->decoration, dec_w, dec_h) < 0) {
            surface_free(&win->surface);
            win->active = 0;
            return -1;
        }
        draw_decoration(win);
    }

    comp.window_count++;
    comp.full_redraw = 1;

    return win->id;
}

void compositor_destroy_window(int win_id) {
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        if (comp.windows[i].active && comp.windows[i].id == win_id) {
            // Add damage for the window area
            comp_window_t* win = &comp.windows[i];
            add_damage(comp.damage, &comp.damage_count, COMP_DAMAGE_RECTS,
                      win->x - DECO_BORDER_WIDTH, win->y - DECO_TITLE_HEIGHT,
                      win->width + 2 * DECO_BORDER_WIDTH,
                      win->height + DECO_TITLE_HEIGHT + DECO_BORDER_WIDTH);

            surface_free(&win->surface);
            surface_free(&win->decoration);
            win->active = 0;
            comp.window_count--;
            comp.full_redraw = 1;
            break;
        }
    }
}

void compositor_move_window(int win_id, int x, int y) {
    comp_window_t* win = compositor_get_window(win_id);
    if (!win) return;

    // Damage old position
    add_damage(comp.damage, &comp.damage_count, COMP_DAMAGE_RECTS,
              win->x - DECO_BORDER_WIDTH, win->y - DECO_TITLE_HEIGHT,
              win->width + 2 * DECO_BORDER_WIDTH,
              win->height + DECO_TITLE_HEIGHT + DECO_BORDER_WIDTH);

    win->x = x;
    win->y = y;

    // Damage new position
    add_damage(comp.damage, &comp.damage_count, COMP_DAMAGE_RECTS,
              x - DECO_BORDER_WIDTH, y - DECO_TITLE_HEIGHT,
              win->width + 2 * DECO_BORDER_WIDTH,
              win->height + DECO_TITLE_HEIGHT + DECO_BORDER_WIDTH);
}

void compositor_resize_window(int win_id, int w, int h) {
    comp_window_t* win = compositor_get_window(win_id);
    if (!win) return;

    surface_free(&win->surface);
    if (surface_alloc(&win->surface, w, h) < 0) return;
    surface_clear(&win->surface, 0xFFFFFFFF);

    if (win->flags & COMP_WIN_DECORATED) {
        surface_free(&win->decoration);
        int dec_w = w + 2 * DECO_BORDER_WIDTH;
        int dec_h = h + DECO_TITLE_HEIGHT + DECO_BORDER_WIDTH;
        if (surface_alloc(&win->decoration, dec_w, dec_h) == 0) {
            draw_decoration(win);
        }
    }

    win->width = w;
    win->height = h;
    comp.full_redraw = 1;
}

void compositor_set_title(int win_id, const char* title) {
    comp_window_t* win = compositor_get_window(win_id);
    if (!win) return;
    str_copy(win->title, title, 64);
    if (win->flags & COMP_WIN_DECORATED) {
        draw_decoration(win);
    }
}

void compositor_show_window(int win_id) {
    comp_window_t* win = compositor_get_window(win_id);
    if (!win) return;
    win->flags |= COMP_WIN_VISIBLE;
    comp.full_redraw = 1;
}

void compositor_hide_window(int win_id) {
    comp_window_t* win = compositor_get_window(win_id);
    if (!win) return;
    win->flags &= ~COMP_WIN_VISIBLE;
    comp.full_redraw = 1;
}

void compositor_focus_window(int win_id) {
    // Unfocus old
    comp_window_t* old = compositor_get_window(comp.focused_id);
    if (old) {
        old->flags &= ~COMP_WIN_FOCUSED;
        if (old->flags & COMP_WIN_DECORATED) draw_decoration(old);
    }

    comp.focused_id = win_id;

    // Focus new
    comp_window_t* win = compositor_get_window(win_id);
    if (win) {
        win->flags |= COMP_WIN_FOCUSED;
        if (win->flags & COMP_WIN_DECORATED) draw_decoration(win);
        // Raise to top z-order within layer
        int max_z = 0;
        for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
            if (comp.windows[i].active && comp.windows[i].layer == win->layer) {
                if (comp.windows[i].z_order > max_z) max_z = comp.windows[i].z_order;
            }
        }
        win->z_order = max_z + 1;
    }
    comp.full_redraw = 1;
}

void compositor_set_layer(int win_id, int layer) {
    comp_window_t* win = compositor_get_window(win_id);
    if (!win || layer < 0 || layer >= LAYER_NUM) return;
    win->layer = layer;
    comp.full_redraw = 1;
}

void compositor_set_opacity(int win_id, uint8_t opacity) {
    comp_window_t* win = compositor_get_window(win_id);
    if (!win) return;
    win->opacity = opacity;
    comp.full_redraw = 1;
}

// ============================================================
// Surface Access
// ============================================================

comp_surface_t* compositor_get_surface(int win_id) {
    comp_window_t* win = compositor_get_window(win_id);
    if (!win) return 0;
    return &win->surface;
}

void compositor_window_damage(int win_id, int x, int y, int w, int h) {
    comp_window_t* win = compositor_get_window(win_id);
    if (!win) return;
    add_damage(win->damage, &win->damage_count, COMP_DAMAGE_RECTS, x, y, w, h);
    // Also add to global damage
    add_damage(comp.damage, &comp.damage_count, COMP_DAMAGE_RECTS,
              win->x + x, win->y + y, w, h);
}

void compositor_window_damage_full(int win_id) {
    comp_window_t* win = compositor_get_window(win_id);
    if (!win) return;
    win->full_damage = 1;
    comp.full_redraw = 1;
}

// ============================================================
// Background
// ============================================================

void compositor_set_wallpaper(const uint32_t* pixels, int w, int h) {
    surface_free(&comp.wallpaper);
    if (surface_alloc(&comp.wallpaper, w, h) < 0) return;
    for (int i = 0; i < w * h; i++) {
        comp.wallpaper.pixels[i] = pixels[i];
    }
    comp.full_redraw = 1;
}

void compositor_set_bg_color(uint32_t color) {
    comp.bg_color = color;
    comp.full_redraw = 1;
}

// ============================================================
// Cursor
// ============================================================

void compositor_set_cursor(const uint32_t* pixels, int w, int h) {
    surface_free(&comp.cursor_surface);
    if (surface_alloc(&comp.cursor_surface, w, h) < 0) return;
    for (int i = 0; i < w * h; i++) {
        comp.cursor_surface.pixels[i] = pixels[i];
    }
}

void compositor_move_cursor(int x, int y) {
    // Damage old cursor position
    add_damage(comp.damage, &comp.damage_count, COMP_DAMAGE_RECTS,
              comp.cursor_x, comp.cursor_y,
              comp.cursor_surface.width, comp.cursor_surface.height);

    comp.cursor_x = x;
    comp.cursor_y = y;

    // Damage new cursor position
    add_damage(comp.damage, &comp.damage_count, COMP_DAMAGE_RECTS,
              x, y, comp.cursor_surface.width, comp.cursor_surface.height);
}

void compositor_show_cursor(int show) {
    comp.cursor_visible = show;
    comp.full_redraw = 1;
}

// ============================================================
// Animation
// ============================================================

void compositor_animate_window(int win_id, int anim_type, int frames) {
    comp_window_t* win = compositor_get_window(win_id);
    if (!win) return;
    win->anim_type = anim_type;
    win->anim_frame = 0;
    win->anim_total = frames;
    win->anim_progress = 0.0f;
}

// ============================================================
// Query
// ============================================================

int compositor_window_at(int x, int y) {
    // Search front-to-back (highest z-order first)
    int best_id = -1;
    int best_z = -1;
    int best_layer = -1;

    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        comp_window_t* win = &comp.windows[i];
        if (!win->active || !(win->flags & COMP_WIN_VISIBLE)) continue;

        int wx = win->x;
        int wy = win->y;
        int ww = win->width;
        int wh = win->height;

        // Include decoration area
        if (win->flags & COMP_WIN_DECORATED) {
            wx -= DECO_BORDER_WIDTH;
            wy -= DECO_TITLE_HEIGHT;
            ww += 2 * DECO_BORDER_WIDTH;
            wh += DECO_TITLE_HEIGHT + DECO_BORDER_WIDTH;
        }

        if (x >= wx && x < wx + ww && y >= wy && y < wy + wh) {
            if (win->layer > best_layer ||
                (win->layer == best_layer && win->z_order > best_z)) {
                best_id = win->id;
                best_z = win->z_order;
                best_layer = win->layer;
            }
        }
    }

    return best_id;
}

comp_window_t* compositor_get_window(int win_id) {
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        if (comp.windows[i].active && comp.windows[i].id == win_id) {
            return &comp.windows[i];
        }
    }
    return 0;
}

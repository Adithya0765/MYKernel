// nv_3d.c - NVIDIA 3D Graphics Engine
// Implements fixed-function pipeline, 3D state management, and software rasterizer fallback
// Hardware path: Submits 3D commands through PFIFO channels
// Software path: CPU-based triangle rasterizer for systems without NVIDIA GPU
// Reference: envytools graph engine documentation

#include "nv_3d.h"
#include "nv_fifo.h"
#include "gpu.h"
#include "heap.h"

// ---- Helpers ----
static void _3d_memset(void* dst, int val, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)val;
}

static void _3d_memcpy(void* dst, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

// Minimal float helpers (kernel has no math library)
static float _3d_fabs(float x) { return x < 0 ? -x : x; }

// Integer-based approximation for sin/cos (fixed-point, sufficient for transforms)
// Returns sin(angle) * 1000 for angle in degrees * 10
// For the kernel we use a lookup-free linear approximation
static float _3d_sinf_approx(float deg) {
    // Normalize to 0-360
    while (deg < 0) deg += 360.0f;
    while (deg >= 360.0f) deg -= 360.0f;

    // Piecewise linear approximation
    float rad_approx;
    if (deg <= 90.0f) {
        rad_approx = deg / 90.0f;
    } else if (deg <= 180.0f) {
        rad_approx = (180.0f - deg) / 90.0f;
    } else if (deg <= 270.0f) {
        rad_approx = -(deg - 180.0f) / 90.0f;
    } else {
        rad_approx = -(360.0f - deg) / 90.0f;
    }
    return rad_approx;
}

static float _3d_cosf_approx(float deg) {
    return _3d_sinf_approx(deg + 90.0f);
}

static float _3d_tanf_approx(float deg) {
    float c = _3d_cosf_approx(deg);
    if (_3d_fabs(c) < 0.001f) c = 0.001f;
    return _3d_sinf_approx(deg) / c;
}

// ---- Global 3D state ----
static nv_3d_state_t state_3d;

nv_3d_state_t* nv_3d_get_state(void) {
    return &state_3d;
}

// ---- Push helper (uses default FIFO channel 0, subchannel 3 for 3D) ----
static void push_3d(uint32_t method, uint32_t data) {
    nv_fifo_state_t* fifo = nv_fifo_get_state();
    if (fifo->initialized && fifo->channels[0].active) {
        nv_fifo_push_method(0, NV_FIFO_SUBCHAN_3D, method, data);
    }
}

// Push float as uint32 (bitwise reinterpret)
static void push_3d_float(uint32_t method, float val) {
    union { float f; uint32_t u; } conv;
    conv.f = val;
    push_3d(method, conv.u);
}

// ============================================================
// Matrix Operations
// ============================================================

void nv_3d_load_identity(nv_mat4_t* mat) {
    _3d_memset(mat, 0, sizeof(nv_mat4_t));
    mat->m[0]  = 1.0f;
    mat->m[5]  = 1.0f;
    mat->m[10] = 1.0f;
    mat->m[15] = 1.0f;
}

void nv_3d_load_ortho(nv_mat4_t* mat, float l, float r, float b, float t, float n, float f) {
    _3d_memset(mat, 0, sizeof(nv_mat4_t));

    float rl = r - l;
    float tb = t - b;
    float fn = f - n;

    if (_3d_fabs(rl) < 0.0001f) rl = 0.0001f;
    if (_3d_fabs(tb) < 0.0001f) tb = 0.0001f;
    if (_3d_fabs(fn) < 0.0001f) fn = 0.0001f;

    mat->m[0]  = 2.0f / rl;
    mat->m[5]  = 2.0f / tb;
    mat->m[10] = -2.0f / fn;
    mat->m[12] = -(r + l) / rl;
    mat->m[13] = -(t + b) / tb;
    mat->m[14] = -(f + n) / fn;
    mat->m[15] = 1.0f;
}

void nv_3d_load_perspective(nv_mat4_t* mat, float fov_deg, float aspect, float near, float far) {
    _3d_memset(mat, 0, sizeof(nv_mat4_t));

    float half_fov = fov_deg / 2.0f;
    float t = _3d_tanf_approx(half_fov);
    if (_3d_fabs(t) < 0.0001f) t = 0.0001f;
    float f_val = 1.0f / t;

    float range = near - far;
    if (_3d_fabs(range) < 0.0001f) range = 0.0001f;

    mat->m[0]  = f_val / aspect;
    mat->m[5]  = f_val;
    mat->m[10] = (far + near) / range;
    mat->m[11] = -1.0f;
    mat->m[14] = (2.0f * far * near) / range;
}

void nv_3d_multiply_mat4(nv_mat4_t* result, const nv_mat4_t* a, const nv_mat4_t* b) {
    nv_mat4_t tmp;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            tmp.m[col * 4 + row] =
                a->m[0 * 4 + row] * b->m[col * 4 + 0] +
                a->m[1 * 4 + row] * b->m[col * 4 + 1] +
                a->m[2 * 4 + row] * b->m[col * 4 + 2] +
                a->m[3 * 4 + row] * b->m[col * 4 + 3];
        }
    }
    _3d_memcpy(result, &tmp, sizeof(nv_mat4_t));
}

void nv_3d_set_modelview(const nv_mat4_t* mat) {
    _3d_memcpy(&state_3d.modelview, mat, sizeof(nv_mat4_t));
    nv_3d_update_mvp();
}

void nv_3d_set_projection(const nv_mat4_t* mat) {
    _3d_memcpy(&state_3d.projection, mat, sizeof(nv_mat4_t));
    nv_3d_update_mvp();
}

void nv_3d_update_mvp(void) {
    nv_3d_multiply_mat4(&state_3d.mvp, &state_3d.projection, &state_3d.modelview);
}

// ============================================================
// Pipeline State
// ============================================================

void nv_3d_set_render_target(uint64_t vram_addr, int width, int height, int pitch) {
    state_3d.rt_address = vram_addr;
    state_3d.rt_width = width;
    state_3d.rt_height = height;
    state_3d.rt_pitch = pitch;
    state_3d.rt_format = 0xCF;  // XRGB8888

    gpu_state_t* g = gpu_get_state();
    if (g->initialized && g->arch >= NV_ARCH_NV50) {
        push_3d(NV50_3D_RT_ADDRESS_HI(0), (uint32_t)(vram_addr >> 32));
        push_3d(NV50_3D_RT_ADDRESS_LO(0), (uint32_t)(vram_addr & 0xFFFFFFFF));
        push_3d(NV50_3D_RT_FORMAT(0), 0xCF);  // XRGB8888
        push_3d(NV50_3D_RT_PITCH(0), (uint32_t)pitch);
    }
}

void nv_3d_set_depth_buffer(uint64_t vram_addr) {
    state_3d.depth_address = vram_addr;

    gpu_state_t* g = gpu_get_state();
    if (g->initialized && g->arch >= NV_ARCH_NV50) {
        push_3d(NV50_3D_ZETA_ADDRESS_HI, (uint32_t)(vram_addr >> 32));
        push_3d(NV50_3D_ZETA_ADDRESS_LO, (uint32_t)(vram_addr & 0xFFFFFFFF));
        push_3d(NV50_3D_ZETA_FORMAT, 0x0A);  // Z24S8 (24-bit depth + 8-bit stencil)
    }
}

void nv_3d_set_viewport(int x, int y, int w, int h) {
    state_3d.vp_x = x;
    state_3d.vp_y = y;
    state_3d.vp_w = w;
    state_3d.vp_h = h;

    gpu_state_t* g = gpu_get_state();
    if (g->initialized && g->arch >= NV_ARCH_NV50) {
        push_3d(NV50_3D_VIEWPORT_HORIZ, (uint32_t)x | ((uint32_t)w << 16));
        push_3d(NV50_3D_VIEWPORT_VERT, (uint32_t)y | ((uint32_t)h << 16));
        push_3d(NV50_3D_SCISSOR_HORIZ, (uint32_t)x | ((uint32_t)w << 16));
        push_3d(NV50_3D_SCISSOR_VERT, (uint32_t)y | ((uint32_t)h << 16));
    }
}

void nv_3d_set_depth_range(float near, float far) {
    state_3d.depth_near = near;
    state_3d.depth_far = far;
}

void nv_3d_depth_test(int enable) {
    state_3d.depth_enabled = enable;
    push_3d(NV50_3D_DEPTH_TEST_ENABLE, enable ? 1 : 0);
}

void nv_3d_depth_write(int enable) {
    state_3d.depth_write = enable;
    push_3d(NV50_3D_DEPTH_WRITE_ENABLE, enable ? 1 : 0);
}

void nv_3d_depth_func(uint32_t func) {
    state_3d.depth_func = func;
    push_3d(NV50_3D_DEPTH_TEST_FUNC, func);
}

void nv_3d_blend_enable(int enable) {
    state_3d.blend_enabled = enable;
    push_3d(NV50_3D_BLEND_ENABLE(0), enable ? 1 : 0);
}

void nv_3d_blend_func(uint32_t src_rgb, uint32_t dst_rgb, uint32_t src_a, uint32_t dst_a) {
    state_3d.blend_src_rgb = src_rgb;
    state_3d.blend_dst_rgb = dst_rgb;
    state_3d.blend_src_a = src_a;
    state_3d.blend_dst_a = dst_a;

    push_3d(NV50_3D_BLEND_FUNC_SRC_RGB, src_rgb);
    push_3d(NV50_3D_BLEND_FUNC_DST_RGB, dst_rgb);
    push_3d(NV50_3D_BLEND_FUNC_SRC_A, src_a);
    push_3d(NV50_3D_BLEND_FUNC_DST_A, dst_a);
}

void nv_3d_blend_equation(uint32_t eq_rgb, uint32_t eq_a) {
    state_3d.blend_eq_rgb = eq_rgb;
    state_3d.blend_eq_a = eq_a;
    push_3d(NV50_3D_BLEND_EQUATION_RGB, eq_rgb);
    push_3d(NV50_3D_BLEND_EQUATION_A, eq_a);
}

void nv_3d_cull_face(int enable, uint32_t face) {
    state_3d.cull_enabled = enable;
    state_3d.cull_face = face;
    push_3d(NV50_3D_CULL_FACE_ENABLE, enable ? 1 : 0);
    if (enable) push_3d(NV50_3D_CULL_FACE, face);
}

void nv_3d_front_face(uint32_t winding) {
    state_3d.front_face = winding;
    push_3d(NV50_3D_FRONT_FACE, winding);
}

// ============================================================
// Clear
// ============================================================

void nv_3d_clear_color(float r, float g, float b, float a) {
    state_3d.clear_color[0] = r;
    state_3d.clear_color[1] = g;
    state_3d.clear_color[2] = b;
    state_3d.clear_color[3] = a;
}

void nv_3d_clear_depth_val(float depth) {
    state_3d.clear_depth = depth;
}

void nv_3d_clear(uint32_t buffers) {
    gpu_state_t* g = gpu_get_state();

    if (g->initialized && g->arch >= NV_ARCH_NV50) {
        // Hardware clear via 3D engine
        push_3d_float(NV50_3D_CLEAR_COLOR(0), state_3d.clear_color[0]);
        push_3d_float(NV50_3D_CLEAR_COLOR(1), state_3d.clear_color[1]);
        push_3d_float(NV50_3D_CLEAR_COLOR(2), state_3d.clear_color[2]);
        push_3d_float(NV50_3D_CLEAR_COLOR(3), state_3d.clear_color[3]);
        push_3d_float(NV50_3D_CLEAR_DEPTH, state_3d.clear_depth);
        push_3d(NV50_3D_CLEAR_STENCIL, (uint32_t)state_3d.clear_stencil);
        push_3d(NV50_3D_CLEAR_BUFFERS, buffers);
        nv_fifo_kick(0);
    } else if (g->vram_mapped && g->vram && (buffers & NV50_3D_CLEAR_BUF_COLOR)) {
        // Software clear fallback
        uint8_t r = (uint8_t)(state_3d.clear_color[0] * 255.0f);
        uint8_t gr = (uint8_t)(state_3d.clear_color[1] * 255.0f);
        uint8_t b = (uint8_t)(state_3d.clear_color[2] * 255.0f);
        uint32_t color = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)gr << 8) | (uint32_t)b;

        volatile uint32_t* rt = g->vram + (state_3d.rt_address / 4);
        int pixels = state_3d.rt_width * state_3d.rt_height;
        for (int i = 0; i < pixels; i++) {
            rt[i] = color;
        }
    }
}

// ============================================================
// Immediate Mode Drawing
// ============================================================

void nv_3d_draw_begin(uint32_t primitive) {
    push_3d(NV50_3D_VERTEX_BEGIN_GL, primitive);
}

void nv_3d_draw_vertex_4f(int attr, float x, float y, float z, float w) {
    // Write 4 floats to vertex attribute register
    push_3d_float(NV50_3D_VTX_ATTR_4F(attr) + 0x00, x);
    push_3d_float(NV50_3D_VTX_ATTR_4F(attr) + 0x04, y);
    push_3d_float(NV50_3D_VTX_ATTR_4F(attr) + 0x08, z);
    push_3d_float(NV50_3D_VTX_ATTR_4F(attr) + 0x0C, w);
}

void nv_3d_draw_vertex_3f(int attr, float x, float y, float z) {
    push_3d_float(NV50_3D_VTX_ATTR_3F(attr) + 0x00, x);
    push_3d_float(NV50_3D_VTX_ATTR_3F(attr) + 0x04, y);
    push_3d_float(NV50_3D_VTX_ATTR_3F(attr) + 0x08, z);
}

void nv_3d_draw_vertex_2f(int attr, float x, float y) {
    push_3d_float(NV50_3D_VTX_ATTR_2F(attr) + 0x00, x);
    push_3d_float(NV50_3D_VTX_ATTR_2F(attr) + 0x04, y);
}

void nv_3d_draw_end(void) {
    push_3d(NV50_3D_VERTEX_END_GL, 0);
    nv_fifo_kick(0);
    state_3d.draw_calls++;
}

void nv_3d_draw_arrays(uint32_t primitive, int first, int count) {
    push_3d(NV50_3D_VERTEX_BEGIN_GL, primitive);
    push_3d(NV50_3D_VERTEX_ARRAY_START, (uint32_t)first);
    push_3d(NV50_3D_VERTEX_ARRAY_COUNT, (uint32_t)count);
    push_3d(NV50_3D_VERTEX_END_GL, 0);
    nv_fifo_kick(0);
    state_3d.draw_calls++;
    state_3d.triangles_drawn += (uint64_t)(count / 3);
}

// ============================================================
// Software Rasterizer (CPU fallback)
// ============================================================

// Edge function for triangle rasterization
static float edge_func(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

void nv_3d_sw_draw_triangle(nv_vertex_t* v0, nv_vertex_t* v1, nv_vertex_t* v2,
                             uint32_t* framebuf, int fb_width, int fb_height) {
    if (!v0 || !v1 || !v2 || !framebuf) return;

    // Convert from clip space (-1..1) to screen space
    float x0 = (v0->position.x + 1.0f) * 0.5f * (float)fb_width;
    float y0 = (1.0f - v0->position.y) * 0.5f * (float)fb_height;
    float x1 = (v1->position.x + 1.0f) * 0.5f * (float)fb_width;
    float y1 = (1.0f - v1->position.y) * 0.5f * (float)fb_height;
    float x2 = (v2->position.x + 1.0f) * 0.5f * (float)fb_width;
    float y2 = (1.0f - v2->position.y) * 0.5f * (float)fb_height;

    // Bounding box
    int min_x = (int)x0; if ((int)x1 < min_x) min_x = (int)x1; if ((int)x2 < min_x) min_x = (int)x2;
    int max_x = (int)x0; if ((int)x1 > max_x) max_x = (int)x1; if ((int)x2 > max_x) max_x = (int)x2;
    int min_y = (int)y0; if ((int)y1 < min_y) min_y = (int)y1; if ((int)y2 < min_y) min_y = (int)y2;
    int max_y = (int)y0; if ((int)y1 > max_y) max_y = (int)y1; if ((int)y2 > max_y) max_y = (int)y2;

    // Clamp to framebuffer
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= fb_width) max_x = fb_width - 1;
    if (max_y >= fb_height) max_y = fb_height - 1;

    // Triangle area (2x)
    float area = edge_func(x0, y0, x1, y1, x2, y2);
    if (_3d_fabs(area) < 0.001f) return;  // Degenerate triangle

    // Rasterize
    for (int py = min_y; py <= max_y; py++) {
        for (int px = min_x; px <= max_x; px++) {
            float fpx = (float)px + 0.5f;
            float fpy = (float)py + 0.5f;

            float w0 = edge_func(x1, y1, x2, y2, fpx, fpy);
            float w1 = edge_func(x2, y2, x0, y0, fpx, fpy);
            float w2 = edge_func(x0, y0, x1, y1, fpx, fpy);

            // Inside test (all same sign as area)
            if (area > 0) {
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            } else {
                if (w0 > 0 || w1 > 0 || w2 > 0) continue;
            }

            // Barycentric interpolation
            w0 /= area;
            w1 /= area;
            w2 /= area;

            // Interpolate color
            float r = v0->color.x * w0 + v1->color.x * w1 + v2->color.x * w2;
            float g = v0->color.y * w0 + v1->color.y * w1 + v2->color.y * w2;
            float b = v0->color.z * w0 + v1->color.z * w1 + v2->color.z * w2;

            // Clamp
            if (r < 0) r = 0; if (r > 1.0f) r = 1.0f;
            if (g < 0) g = 0; if (g > 1.0f) g = 1.0f;
            if (b < 0) b = 0; if (b > 1.0f) b = 1.0f;

            uint8_t ri = (uint8_t)(r * 255.0f);
            uint8_t gi = (uint8_t)(g * 255.0f);
            uint8_t bi = (uint8_t)(b * 255.0f);

            framebuf[py * fb_width + px] = 0xFF000000 | ((uint32_t)ri << 16) |
                                           ((uint32_t)gi << 8) | (uint32_t)bi;
        }
    }

    state_3d.triangles_drawn++;
}

// ============================================================
// Initialization
// ============================================================

int nv_3d_init(void) {
    gpu_state_t* g = gpu_get_state();

    _3d_memset(&state_3d, 0, sizeof(nv_3d_state_t));

    // Select 3D class
    if (g->initialized) {
        if (g->arch >= NV_ARCH_NVE0) {
            state_3d.class_3d = NVE4_3D_CLASS;
        } else if (g->arch >= NV_ARCH_NVC0) {
            state_3d.class_3d = NVC0_3D_CLASS;
        } else if (g->arch >= NV_ARCH_NV50) {
            state_3d.class_3d = NV50_3D_CLASS;
        } else {
            state_3d.class_3d = NV40_3D_CLASS;
        }

        // Bind 3D class to subchannel 3 on FIFO channel 0
        nv_fifo_state_t* fifo = nv_fifo_get_state();
        if (fifo->initialized && fifo->channels[0].active) {
            nv_fifo_channel_bind_object(0, NV_FIFO_SUBCHAN_3D, state_3d.class_3d);
        }
    }

    // Initialize transform matrices to identity
    nv_3d_load_identity(&state_3d.modelview);
    nv_3d_load_identity(&state_3d.projection);
    nv_3d_load_identity(&state_3d.mvp);

    // Default state
    state_3d.depth_func = NV50_3D_DEPTH_FUNC_LESS;
    state_3d.depth_near = 0.0f;
    state_3d.depth_far = 1.0f;
    state_3d.clear_depth = 1.0f;
    state_3d.front_face = 0x0901;  // CCW

    // Set default render target to GPU scanout buffer
    if (g->initialized && g->vram_mapped) {
        nv_3d_set_render_target(g->fb_offset,
                                 g->display_width > 0 ? g->display_width : 1024,
                                 g->display_height > 0 ? g->display_height : 768,
                                 (g->display_width > 0 ? g->display_width : 1024) * 4);
        nv_3d_set_viewport(0, 0,
                            g->display_width > 0 ? g->display_width : 1024,
                            g->display_height > 0 ? g->display_height : 768);
    }

    state_3d.initialized = 1;
    return 0;
}

void nv_3d_shutdown(void) {
    if (!state_3d.initialized) return;

    // Flush any remaining commands
    nv_fifo_state_t* fifo = nv_fifo_get_state();
    if (fifo->initialized && fifo->channels[0].active) {
        nv_fifo_wait_idle(0);
    }

    state_3d.initialized = 0;
}

// opengl.c - OpenGL 1.x Subset Implementation for Alteo OS
// Maps GL calls to NVIDIA 3D engine (hardware) or software rasterizer (fallback)
#include "opengl.h"
#include "nv_3d.h"
#include "nv_2d.h"
#include "gpu.h"
#include "graphics.h"
#include "heap.h"

// ============================================================
// Internal State
// ============================================================

// Forward declare gpu_state from gpu.c
extern gpu_state_t gpu_state;
extern nv_3d_state_t nv_3d_state;
extern nv_2d_state_t nv_2d_state;

// Framebuffer reference from graphics.c
extern uint32_t* framebuffer;
extern uint32_t  fb_pitch;
extern uint32_t  backbuf[];
extern int screen_width;
extern int screen_height;

// Software approximations (no libm)
static float gl_fabsf(float x) { return x < 0.0f ? -x : x; }
static float gl_sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    float guess = x * 0.5f;
    for (int i = 0; i < 10; i++) {
        guess = 0.5f * (guess + x / guess);
    }
    return guess;
}

// Piecewise sine approximation
static float gl_sinf(float x) {
    // Normalize to [-PI, PI]
    float pi = 3.14159265f;
    while (x > pi) x -= 2.0f * pi;
    while (x < -pi) x += 2.0f * pi;
    // Quadratic approximation
    float abs_x = gl_fabsf(x);
    float y = (4.0f / (pi * pi)) * x * (pi - abs_x);
    return y;
}

static float gl_cosf(float x) {
    return gl_sinf(x + 1.5707963f); // PI/2
}

static float gl_tanf(float x) {
    float c = gl_cosf(x);
    if (gl_fabsf(c) < 0.0001f) return 99999.0f;
    return gl_sinf(x) / c;
}

// Matrix stack
typedef struct {
    float mat[GL_MAX_MATRIX_STACK_DEPTH][16];
    int   top;
} gl_matrix_stack_t;

// Immediate mode vertex
typedef struct {
    float x, y, z;
    float r, g, b, a;
    float s, t;
    float nx, ny, nz;
} gl_vertex_t;

// GL texture object
typedef struct {
    uint32_t id;
    int      width, height;
    uint32_t* data;          // RGBA pixel data
    int      min_filter;
    int      mag_filter;
    int      wrap_s;
    int      wrap_t;
    int      active;
} gl_texture_t;

// Global GL context
static struct {
    int initialized;

    // Matrix stacks
    gl_matrix_stack_t modelview;
    gl_matrix_stack_t projection;
    gl_matrix_stack_t texture_mat;
    gl_matrix_stack_t* current_stack;
    GLenum matrix_mode;

    // Immediate mode
    GLenum    prim_mode;
    int       in_begin;
    gl_vertex_t vertices[GL_MAX_IMMEDIATE_VERTICES];
    int       vertex_count;

    // Current vertex attributes
    float cur_r, cur_g, cur_b, cur_a;
    float cur_s, cur_t;
    float cur_nx, cur_ny, cur_nz;

    // State
    int depth_test;
    int blend;
    int cull_face;
    int texture_2d;
    int scissor_test;

    GLenum depth_func;
    GLenum blend_src, blend_dst;
    GLenum cull_mode;
    GLenum front_face;

    // Clear values
    float clear_r, clear_g, clear_b, clear_a;
    float clear_depth;

    // Viewport
    int vp_x, vp_y, vp_w, vp_h;
    int sc_x, sc_y, sc_w, sc_h;

    // Textures
    gl_texture_t textures[GL_MAX_TEXTURES];
    uint32_t     bound_texture;
    uint32_t     next_texture_id;

    // Error
    GLenum error;

    // GPU available
    int use_gpu;
} gl_ctx;

// ============================================================
// Matrix Helpers
// ============================================================

static void mat4_identity(float* m) {
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_copy(float* dst, const float* src) {
    for (int i = 0; i < 16; i++) dst[i] = src[i];
}

static void mat4_multiply(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[j * 4 + i] = 0.0f;
            for (int k = 0; k < 4; k++) {
                tmp[j * 4 + i] += a[k * 4 + i] * b[j * 4 + k];
            }
        }
    }
    mat4_copy(out, tmp);
}

static float* current_matrix(void) {
    gl_matrix_stack_t* s = gl_ctx.current_stack;
    return s->mat[s->top];
}

// ============================================================
// Triangle Rasterizer (Software Fallback)
// ============================================================

static uint32_t pack_color(float r, float g, float b, float a) {
    int ri = (int)(r * 255.0f); if (ri < 0) ri = 0; if (ri > 255) ri = 255;
    int gi = (int)(g * 255.0f); if (gi < 0) gi = 0; if (gi > 255) gi = 255;
    int bi = (int)(b * 255.0f); if (bi < 0) bi = 0; if (bi > 255) bi = 255;
    int ai = (int)(a * 255.0f); if (ai < 0) ai = 0; if (ai > 255) ai = 255;
    return ((uint32_t)ai << 24) | ((uint32_t)ri << 16) | ((uint32_t)gi << 8) | (uint32_t)bi;
}

static uint32_t blend_pixel(uint32_t dst, uint32_t src) {
    int sa = (src >> 24) & 0xFF;
    int sr = (src >> 16) & 0xFF;
    int sg = (src >> 8)  & 0xFF;
    int sb = src & 0xFF;
    int da = (dst >> 24) & 0xFF;
    int dr = (dst >> 16) & 0xFF;
    int dg = (dst >> 8)  & 0xFF;
    int db = dst & 0xFF;

    int inv_sa = 255 - sa;
    int or = (sr * sa + dr * inv_sa) / 255;
    int og = (sg * sa + dg * inv_sa) / 255;
    int ob = (sb * sa + db * inv_sa) / 255;
    int oa = sa + (da * inv_sa) / 255;
    if (or > 255) or = 255;
    if (og > 255) og = 255;
    if (ob > 255) ob = 255;
    if (oa > 255) oa = 255;

    return ((uint32_t)oa << 24) | ((uint32_t)or << 16) | ((uint32_t)og << 8) | (uint32_t)ob;
}

static void gl_put_pixel(int x, int y, uint32_t color) {
    if (x < gl_ctx.vp_x || x >= gl_ctx.vp_x + gl_ctx.vp_w) return;
    if (y < gl_ctx.vp_y || y >= gl_ctx.vp_y + gl_ctx.vp_h) return;
    if (gl_ctx.scissor_test) {
        if (x < gl_ctx.sc_x || x >= gl_ctx.sc_x + gl_ctx.sc_w) return;
        if (y < gl_ctx.sc_y || y >= gl_ctx.sc_y + gl_ctx.sc_h) return;
    }

    if (gl_ctx.blend) {
        uint32_t dst = backbuf[y * screen_width + x];
        color = blend_pixel(dst, color);
    }
    backbuf[y * screen_width + x] = color;
}

// Transform vertex through modelview-projection
static void transform_vertex(gl_vertex_t* v, float* out_x, float* out_y, float* out_z, float* out_w) {
    float* mv = gl_ctx.modelview.mat[gl_ctx.modelview.top];
    float* pr = gl_ctx.projection.mat[gl_ctx.projection.top];

    // Modelview transform
    float ex = mv[0]*v->x + mv[4]*v->y + mv[8]*v->z + mv[12];
    float ey = mv[1]*v->x + mv[5]*v->y + mv[9]*v->z + mv[13];
    float ez = mv[2]*v->x + mv[6]*v->y + mv[10]*v->z + mv[14];
    float ew = mv[3]*v->x + mv[7]*v->y + mv[11]*v->z + mv[15];

    // Projection transform
    *out_x = pr[0]*ex + pr[4]*ey + pr[8]*ez + pr[12]*ew;
    *out_y = pr[1]*ex + pr[5]*ey + pr[9]*ez + pr[13]*ew;
    *out_z = pr[2]*ex + pr[6]*ey + pr[10]*ez + pr[14]*ew;
    *out_w = pr[3]*ex + pr[7]*ey + pr[11]*ez + pr[15]*ew;
}

// Rasterize a single triangle (software)
static void rasterize_triangle(gl_vertex_t* v0, gl_vertex_t* v1, gl_vertex_t* v2) {
    // Transform vertices
    float x0, y0, z0, w0;
    float x1, y1, z1, w1;
    float x2, y2, z2, w2;

    transform_vertex(v0, &x0, &y0, &z0, &w0);
    transform_vertex(v1, &x1, &y1, &z1, &w1);
    transform_vertex(v2, &x2, &y2, &z2, &w2);

    // Perspective divide
    if (gl_fabsf(w0) < 0.0001f) w0 = 0.0001f;
    if (gl_fabsf(w1) < 0.0001f) w1 = 0.0001f;
    if (gl_fabsf(w2) < 0.0001f) w2 = 0.0001f;

    float ndcx0 = x0/w0, ndcy0 = y0/w0;
    float ndcx1 = x1/w1, ndcy1 = y1/w1;
    float ndcx2 = x2/w2, ndcy2 = y2/w2;

    // NDC to screen coordinates
    float vp_x = (float)gl_ctx.vp_x;
    float vp_y = (float)gl_ctx.vp_y;
    float vp_w = (float)gl_ctx.vp_w;
    float vp_h = (float)gl_ctx.vp_h;

    float sx0 = vp_x + (ndcx0 + 1.0f) * 0.5f * vp_w;
    float sy0 = vp_y + (1.0f - ndcy0) * 0.5f * vp_h;
    float sx1 = vp_x + (ndcx1 + 1.0f) * 0.5f * vp_w;
    float sy1 = vp_y + (1.0f - ndcy1) * 0.5f * vp_h;
    float sx2 = vp_x + (ndcx2 + 1.0f) * 0.5f * vp_w;
    float sy2 = vp_y + (1.0f - ndcy2) * 0.5f * vp_h;

    // Bounding box
    int minx = (int)sx0; if ((int)sx1 < minx) minx = (int)sx1; if ((int)sx2 < minx) minx = (int)sx2;
    int maxx = (int)sx0; if ((int)sx1 > maxx) maxx = (int)sx1; if ((int)sx2 > maxx) maxx = (int)sx2;
    int miny = (int)sy0; if ((int)sy1 < miny) miny = (int)sy1; if ((int)sy2 < miny) miny = (int)sy2;
    int maxy = (int)sy0; if ((int)sy1 > maxy) maxy = (int)sy1; if ((int)sy2 > maxy) maxy = (int)sy2;

    // Clamp to viewport
    if (minx < gl_ctx.vp_x) minx = gl_ctx.vp_x;
    if (miny < gl_ctx.vp_y) miny = gl_ctx.vp_y;
    if (maxx >= gl_ctx.vp_x + gl_ctx.vp_w) maxx = gl_ctx.vp_x + gl_ctx.vp_w - 1;
    if (maxy >= gl_ctx.vp_y + gl_ctx.vp_h) maxy = gl_ctx.vp_y + gl_ctx.vp_h - 1;

    // Edge function rasterization
    float area = (sx1 - sx0) * (sy2 - sy0) - (sx2 - sx0) * (sy1 - sy0);
    if (gl_fabsf(area) < 0.001f) return; // degenerate

    float inv_area = 1.0f / area;

    for (int py = miny; py <= maxy; py++) {
        for (int px = minx; px <= maxx; px++) {
            float fpx = (float)px + 0.5f;
            float fpy = (float)py + 0.5f;

            float e0 = (sx1 - sx0) * (fpy - sy0) - (sy1 - sy0) * (fpx - sx0);
            float e1 = (sx2 - sx1) * (fpy - sy1) - (sy2 - sy1) * (fpx - sx1);
            float e2 = (sx0 - sx2) * (fpy - sy2) - (sy0 - sy2) * (fpx - sx2);

            // Check winding
            int inside;
            if (area > 0) {
                inside = (e0 >= 0 && e1 >= 0 && e2 >= 0);
            } else {
                inside = (e0 <= 0 && e1 <= 0 && e2 <= 0);
            }

            if (inside) {
                // Barycentric coordinates
                float w_v2 = e0 * inv_area;
                float w_v0 = e1 * inv_area;
                float w_v1 = e2 * inv_area;
                if (area < 0) { w_v0 = -w_v0; w_v1 = -w_v1; w_v2 = -w_v2; }

                // Interpolate color
                float r = w_v0 * v0->r + w_v1 * v1->r + w_v2 * v2->r;
                float g = w_v0 * v0->g + w_v1 * v1->g + w_v2 * v2->g;
                float b = w_v0 * v0->b + w_v1 * v1->b + w_v2 * v2->b;
                float a = w_v0 * v0->a + w_v1 * v1->a + w_v2 * v2->a;

                uint32_t color = pack_color(r, g, b, a);
                gl_put_pixel(px, py, color);
            }
        }
    }
}

// Rasterize a line
static void rasterize_line(gl_vertex_t* v0, gl_vertex_t* v1) {
    float x0c, y0c, z0c, w0c;
    float x1c, y1c, z1c, w1c;

    transform_vertex(v0, &x0c, &y0c, &z0c, &w0c);
    transform_vertex(v1, &x1c, &y1c, &z1c, &w1c);

    if (gl_fabsf(w0c) < 0.0001f) w0c = 0.0001f;
    if (gl_fabsf(w1c) < 0.0001f) w1c = 0.0001f;

    float vp_w = (float)gl_ctx.vp_w;
    float vp_h = (float)gl_ctx.vp_h;
    float vp_x = (float)gl_ctx.vp_x;
    float vp_y = (float)gl_ctx.vp_y;

    int sx0 = (int)(vp_x + (x0c/w0c + 1.0f) * 0.5f * vp_w);
    int sy0 = (int)(vp_y + (1.0f - y0c/w0c) * 0.5f * vp_h);
    int sx1 = (int)(vp_x + (x1c/w1c + 1.0f) * 0.5f * vp_w);
    int sy1 = (int)(vp_y + (1.0f - y1c/w1c) * 0.5f * vp_h);

    // Bresenham line
    int dx = sx1 - sx0; if (dx < 0) dx = -dx;
    int dy = sy1 - sy0; if (dy < 0) dy = -dy;
    int sx_step = sx0 < sx1 ? 1 : -1;
    int sy_step = sy0 < sy1 ? 1 : -1;
    int err = dx - dy;
    int steps = dx > dy ? dx : dy;
    if (steps == 0) steps = 1;

    int cx = sx0, cy = sy0;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / (float)steps;
        float r = v0->r + t * (v1->r - v0->r);
        float g = v0->g + t * (v1->g - v0->g);
        float b = v0->b + t * (v1->b - v0->b);
        float a = v0->a + t * (v1->a - v0->a);
        gl_put_pixel(cx, cy, pack_color(r, g, b, a));

        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; cx += sx_step; }
        if (e2 < dx) { err += dx; cy += sy_step; }
    }
}

// Rasterize a point
static void rasterize_point(gl_vertex_t* v) {
    float xc, yc, zc, wc;
    transform_vertex(v, &xc, &yc, &zc, &wc);
    if (gl_fabsf(wc) < 0.0001f) wc = 0.0001f;

    int sx = (int)((float)gl_ctx.vp_x + (xc/wc + 1.0f) * 0.5f * (float)gl_ctx.vp_w);
    int sy = (int)((float)gl_ctx.vp_y + (1.0f - yc/wc) * 0.5f * (float)gl_ctx.vp_h);

    gl_put_pixel(sx, sy, pack_color(v->r, v->g, v->b, v->a));
}

// Flush the accumulated primitive batch
static void flush_primitives(void) {
    int n = gl_ctx.vertex_count;
    gl_vertex_t* verts = gl_ctx.vertices;

    switch (gl_ctx.prim_mode) {
    case GL_POINTS:
        for (int i = 0; i < n; i++) {
            rasterize_point(&verts[i]);
        }
        break;

    case GL_LINES:
        for (int i = 0; i + 1 < n; i += 2) {
            rasterize_line(&verts[i], &verts[i+1]);
        }
        break;

    case GL_LINE_STRIP:
        for (int i = 0; i + 1 < n; i++) {
            rasterize_line(&verts[i], &verts[i+1]);
        }
        break;

    case GL_TRIANGLES:
        for (int i = 0; i + 2 < n; i += 3) {
            rasterize_triangle(&verts[i], &verts[i+1], &verts[i+2]);
        }
        break;

    case GL_TRIANGLE_STRIP:
        for (int i = 0; i + 2 < n; i++) {
            if (i & 1) {
                rasterize_triangle(&verts[i+1], &verts[i], &verts[i+2]);
            } else {
                rasterize_triangle(&verts[i], &verts[i+1], &verts[i+2]);
            }
        }
        break;

    case GL_TRIANGLE_FAN:
        for (int i = 1; i + 1 < n; i++) {
            rasterize_triangle(&verts[0], &verts[i], &verts[i+1]);
        }
        break;

    case GL_QUADS:
        for (int i = 0; i + 3 < n; i += 4) {
            rasterize_triangle(&verts[i], &verts[i+1], &verts[i+2]);
            rasterize_triangle(&verts[i], &verts[i+2], &verts[i+3]);
        }
        break;

    case GL_QUAD_STRIP:
        for (int i = 0; i + 3 < n; i += 2) {
            rasterize_triangle(&verts[i], &verts[i+1], &verts[i+3]);
            rasterize_triangle(&verts[i], &verts[i+3], &verts[i+2]);
        }
        break;

    case GL_POLYGON:
        // Triangulate as fan from vertex 0
        for (int i = 1; i + 1 < n; i++) {
            rasterize_triangle(&verts[0], &verts[i], &verts[i+1]);
        }
        break;
    }
}

// ============================================================
// Initialization
// ============================================================

int gl_init(void) {
    // Zero state
    for (int i = 0; i < (int)sizeof(gl_ctx); i++) {
        ((char*)&gl_ctx)[i] = 0;
    }

    // Init matrix stacks
    mat4_identity(gl_ctx.modelview.mat[0]);
    mat4_identity(gl_ctx.projection.mat[0]);
    mat4_identity(gl_ctx.texture_mat.mat[0]);
    gl_ctx.modelview.top = 0;
    gl_ctx.projection.top = 0;
    gl_ctx.texture_mat.top = 0;

    gl_ctx.matrix_mode = GL_MODELVIEW;
    gl_ctx.current_stack = &gl_ctx.modelview;

    // Default state
    gl_ctx.cur_r = 1.0f; gl_ctx.cur_g = 1.0f;
    gl_ctx.cur_b = 1.0f; gl_ctx.cur_a = 1.0f;

    gl_ctx.depth_func = GL_LESS;
    gl_ctx.blend_src = GL_ONE;
    gl_ctx.blend_dst = GL_ZERO;
    gl_ctx.cull_mode = GL_BACK;
    gl_ctx.front_face = GL_CCW;
    gl_ctx.clear_depth = 1.0f;

    gl_ctx.vp_x = 0;
    gl_ctx.vp_y = 0;
    gl_ctx.vp_w = screen_width;
    gl_ctx.vp_h = screen_height;
    gl_ctx.sc_w = screen_width;
    gl_ctx.sc_h = screen_height;

    gl_ctx.next_texture_id = 1;

    // Check for GPU
    gl_ctx.use_gpu = gpu_state.initialized;

    gl_ctx.initialized = 1;
    return 0;
}

void gl_shutdown(void) {
    // Free texture data
    for (int i = 0; i < GL_MAX_TEXTURES; i++) {
        if (gl_ctx.textures[i].data) {
            kfree(gl_ctx.textures[i].data);
            gl_ctx.textures[i].data = 0;
        }
    }
    gl_ctx.initialized = 0;
}

// ============================================================
// State Functions
// ============================================================

void glEnable(GLenum cap) {
    switch (cap) {
    case GL_DEPTH_TEST:   gl_ctx.depth_test = 1; break;
    case GL_BLEND:        gl_ctx.blend = 1; break;
    case GL_CULL_FACE:    gl_ctx.cull_face = 1; break;
    case GL_TEXTURE_2D:   gl_ctx.texture_2d = 1; break;
    case GL_SCISSOR_TEST: gl_ctx.scissor_test = 1; break;
    default: break;
    }
}

void glDisable(GLenum cap) {
    switch (cap) {
    case GL_DEPTH_TEST:   gl_ctx.depth_test = 0; break;
    case GL_BLEND:        gl_ctx.blend = 0; break;
    case GL_CULL_FACE:    gl_ctx.cull_face = 0; break;
    case GL_TEXTURE_2D:   gl_ctx.texture_2d = 0; break;
    case GL_SCISSOR_TEST: gl_ctx.scissor_test = 0; break;
    default: break;
    }
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    gl_ctx.vp_x = x;
    gl_ctx.vp_y = y;
    gl_ctx.vp_w = width;
    gl_ctx.vp_h = height;
}

void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    gl_ctx.sc_x = x;
    gl_ctx.sc_y = y;
    gl_ctx.sc_w = width;
    gl_ctx.sc_h = height;
}

void glDepthFunc(GLenum func) {
    gl_ctx.depth_func = func;
}

void glBlendFunc(GLenum sfactor, GLenum dfactor) {
    gl_ctx.blend_src = sfactor;
    gl_ctx.blend_dst = dfactor;
}

void glCullFace(GLenum mode) {
    gl_ctx.cull_mode = mode;
}

void glFrontFace(GLenum mode) {
    gl_ctx.front_face = mode;
}

void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    gl_ctx.clear_r = red;
    gl_ctx.clear_g = green;
    gl_ctx.clear_b = blue;
    gl_ctx.clear_a = alpha;
}

void glClearDepth(GLclampd depth) {
    gl_ctx.clear_depth = (float)depth;
}

void glClear(GLbitfield mask) {
    if (mask & GL_COLOR_BUFFER_BIT) {
        uint32_t color = pack_color(gl_ctx.clear_r, gl_ctx.clear_g,
                                    gl_ctx.clear_b, gl_ctx.clear_a);
        int total = screen_width * screen_height;
        for (int i = 0; i < total; i++) {
            backbuf[i] = color;
        }
    }
    // Depth/stencil clear - would need depth buffer, stub for now
}

GLenum glGetError(void) {
    GLenum e = gl_ctx.error;
    gl_ctx.error = GL_NO_ERROR;
    return e;
}

// ============================================================
// Matrix Functions
// ============================================================

void glMatrixMode(GLenum mode) {
    gl_ctx.matrix_mode = mode;
    switch (mode) {
    case GL_MODELVIEW:  gl_ctx.current_stack = &gl_ctx.modelview; break;
    case GL_PROJECTION: gl_ctx.current_stack = &gl_ctx.projection; break;
    case GL_TEXTURE:    gl_ctx.current_stack = &gl_ctx.texture_mat; break;
    default:
        gl_ctx.error = GL_INVALID_ENUM;
        break;
    }
}

void glLoadIdentity(void) {
    mat4_identity(current_matrix());
}

void glPushMatrix(void) {
    gl_matrix_stack_t* s = gl_ctx.current_stack;
    if (s->top >= GL_MAX_MATRIX_STACK_DEPTH - 1) {
        gl_ctx.error = GL_INVALID_OPERATION;
        return;
    }
    mat4_copy(s->mat[s->top + 1], s->mat[s->top]);
    s->top++;
}

void glPopMatrix(void) {
    gl_matrix_stack_t* s = gl_ctx.current_stack;
    if (s->top <= 0) {
        gl_ctx.error = GL_INVALID_OPERATION;
        return;
    }
    s->top--;
}

void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble nearv, GLdouble farv) {
    float l = (float)left, r = (float)right;
    float b = (float)bottom, t = (float)top;
    float n = (float)nearv, f = (float)farv;

    float m[16];
    mat4_identity(m);
    m[0]  =  2.0f / (r - l);
    m[5]  =  2.0f / (t - b);
    m[10] = -2.0f / (f - n);
    m[12] = -(r + l) / (r - l);
    m[13] = -(t + b) / (t - b);
    m[14] = -(f + n) / (f - n);

    float* cur = current_matrix();
    float tmp[16];
    mat4_multiply(tmp, cur, m);
    mat4_copy(cur, tmp);
}

void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
               GLdouble nearv, GLdouble farv) {
    float l = (float)left, r = (float)right;
    float b = (float)bottom, t = (float)top;
    float n = (float)nearv, f = (float)farv;

    float m[16];
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0]  = (2.0f * n) / (r - l);
    m[5]  = (2.0f * n) / (t - b);
    m[8]  = (r + l) / (r - l);
    m[9]  = (t + b) / (t - b);
    m[10] = -(f + n) / (f - n);
    m[11] = -1.0f;
    m[14] = -(2.0f * f * n) / (f - n);

    float* cur = current_matrix();
    float tmp[16];
    mat4_multiply(tmp, cur, m);
    mat4_copy(cur, tmp);
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    float m[16];
    mat4_identity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;

    float* cur = current_matrix();
    float tmp[16];
    mat4_multiply(tmp, cur, m);
    mat4_copy(cur, tmp);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    float rad = angle * 3.14159265f / 180.0f;
    float c = gl_cosf(rad);
    float s = gl_sinf(rad);

    // Normalize axis
    float len = gl_sqrtf(x*x + y*y + z*z);
    if (len < 0.0001f) return;
    x /= len; y /= len; z /= len;

    float nc = 1.0f - c;

    float m[16];
    m[0]  = x*x*nc + c;    m[4] = x*y*nc - z*s;   m[8]  = x*z*nc + y*s;  m[12] = 0;
    m[1]  = y*x*nc + z*s;  m[5] = y*y*nc + c;      m[9]  = y*z*nc - x*s;  m[13] = 0;
    m[2]  = z*x*nc - y*s;  m[6] = z*y*nc + x*s;    m[10] = z*z*nc + c;    m[14] = 0;
    m[3]  = 0;             m[7] = 0;               m[11] = 0;             m[15] = 1;

    float* cur = current_matrix();
    float tmp[16];
    mat4_multiply(tmp, cur, m);
    mat4_copy(cur, tmp);
}

void glScalef(GLfloat x, GLfloat y, GLfloat z) {
    float m[16];
    mat4_identity(m);
    m[0]  = x;
    m[5]  = y;
    m[10] = z;

    float* cur = current_matrix();
    float tmp[16];
    mat4_multiply(tmp, cur, m);
    mat4_copy(cur, tmp);
}

void glMultMatrixf(const GLfloat* m) {
    float* cur = current_matrix();
    float tmp[16];
    mat4_multiply(tmp, cur, m);
    mat4_copy(cur, tmp);
}

// ============================================================
// Immediate Mode
// ============================================================

void glBegin(GLenum mode) {
    if (gl_ctx.in_begin) {
        gl_ctx.error = GL_INVALID_OPERATION;
        return;
    }
    gl_ctx.prim_mode = mode;
    gl_ctx.vertex_count = 0;
    gl_ctx.in_begin = 1;
}

void glEnd(void) {
    if (!gl_ctx.in_begin) {
        gl_ctx.error = GL_INVALID_OPERATION;
        return;
    }
    gl_ctx.in_begin = 0;
    flush_primitives();
    gl_ctx.vertex_count = 0;
}

static void emit_vertex(float x, float y, float z) {
    if (gl_ctx.vertex_count >= GL_MAX_IMMEDIATE_VERTICES) return;
    gl_vertex_t* v = &gl_ctx.vertices[gl_ctx.vertex_count++];
    v->x = x; v->y = y; v->z = z;
    v->r = gl_ctx.cur_r; v->g = gl_ctx.cur_g;
    v->b = gl_ctx.cur_b; v->a = gl_ctx.cur_a;
    v->s = gl_ctx.cur_s; v->t = gl_ctx.cur_t;
    v->nx = gl_ctx.cur_nx; v->ny = gl_ctx.cur_ny; v->nz = gl_ctx.cur_nz;
}

void glVertex2f(GLfloat x, GLfloat y) {
    emit_vertex(x, y, 0.0f);
}

void glVertex3f(GLfloat x, GLfloat y, GLfloat z) {
    emit_vertex(x, y, z);
}

void glColor3f(GLfloat r, GLfloat g, GLfloat b) {
    gl_ctx.cur_r = r; gl_ctx.cur_g = g; gl_ctx.cur_b = b; gl_ctx.cur_a = 1.0f;
}

void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    gl_ctx.cur_r = r; gl_ctx.cur_g = g; gl_ctx.cur_b = b; gl_ctx.cur_a = a;
}

void glColor3ub(GLubyte r, GLubyte g, GLubyte b) {
    gl_ctx.cur_r = (float)r / 255.0f;
    gl_ctx.cur_g = (float)g / 255.0f;
    gl_ctx.cur_b = (float)b / 255.0f;
    gl_ctx.cur_a = 1.0f;
}

void glTexCoord2f(GLfloat s, GLfloat t) {
    gl_ctx.cur_s = s; gl_ctx.cur_t = t;
}

void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz) {
    gl_ctx.cur_nx = nx; gl_ctx.cur_ny = ny; gl_ctx.cur_nz = nz;
}

// ============================================================
// Texture Functions
// ============================================================

void glGenTextures(GLsizei n, GLuint* textures) {
    for (int i = 0; i < n; i++) {
        textures[i] = gl_ctx.next_texture_id++;
    }
}

void glDeleteTextures(GLsizei n, const GLuint* textures) {
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < GL_MAX_TEXTURES; j++) {
            if (gl_ctx.textures[j].id == textures[i]) {
                if (gl_ctx.textures[j].data) {
                    kfree(gl_ctx.textures[j].data);
                }
                gl_ctx.textures[j].data = 0;
                gl_ctx.textures[j].active = 0;
                gl_ctx.textures[j].id = 0;
                break;
            }
        }
    }
}

void glBindTexture(GLenum target, GLuint texture) {
    (void)target;
    gl_ctx.bound_texture = texture;
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                  GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid* pixels) {
    (void)target; (void)level; (void)internalformat; (void)border;
    (void)format; (void)type;

    if (gl_ctx.bound_texture == 0) return;

    // Find or allocate slot
    int slot = -1;
    for (int i = 0; i < GL_MAX_TEXTURES; i++) {
        if (gl_ctx.textures[i].id == gl_ctx.bound_texture) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < GL_MAX_TEXTURES; i++) {
            if (!gl_ctx.textures[i].active) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0) {
        gl_ctx.error = GL_OUT_OF_MEMORY;
        return;
    }

    // Free old data
    if (gl_ctx.textures[slot].data) {
        kfree(gl_ctx.textures[slot].data);
    }

    gl_ctx.textures[slot].id = gl_ctx.bound_texture;
    gl_ctx.textures[slot].width = width;
    gl_ctx.textures[slot].height = height;
    gl_ctx.textures[slot].active = 1;
    gl_ctx.textures[slot].min_filter = GL_NEAREST;
    gl_ctx.textures[slot].mag_filter = GL_NEAREST;
    gl_ctx.textures[slot].wrap_s = GL_REPEAT;
    gl_ctx.textures[slot].wrap_t = GL_REPEAT;

    if (pixels) {
        int size = width * height * 4;
        gl_ctx.textures[slot].data = (uint32_t*)kmalloc(size);
        if (gl_ctx.textures[slot].data) {
            const uint8_t* src = (const uint8_t*)pixels;
            uint32_t* dst = gl_ctx.textures[slot].data;
            for (int i = 0; i < width * height; i++) {
                uint8_t r = src[i * 4 + 0];
                uint8_t g = src[i * 4 + 1];
                uint8_t b = src[i * 4 + 2];
                uint8_t a = src[i * 4 + 3];
                dst[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                         ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    } else {
        gl_ctx.textures[slot].data = 0;
    }
}

void glTexParameteri(GLenum target, GLenum pname, GLint param) {
    (void)target;
    for (int i = 0; i < GL_MAX_TEXTURES; i++) {
        if (gl_ctx.textures[i].id == gl_ctx.bound_texture) {
            switch (pname) {
            case GL_TEXTURE_MIN_FILTER: gl_ctx.textures[i].min_filter = param; break;
            case GL_TEXTURE_MAG_FILTER: gl_ctx.textures[i].mag_filter = param; break;
            case GL_TEXTURE_WRAP_S:     gl_ctx.textures[i].wrap_s = param; break;
            case GL_TEXTURE_WRAP_T:     gl_ctx.textures[i].wrap_t = param; break;
            }
            break;
        }
    }
}

// ============================================================
// Flush / Finish
// ============================================================

void glFlush(void) {
    // In software mode, nothing to flush
    // With GPU, would kick the pushbuffer
}

void glFinish(void) {
    glFlush();
    // With GPU, would wait for fence
}

// ============================================================
// Read Pixels
// ============================================================

void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                  GLenum format, GLenum type, GLvoid* pixels) {
    (void)format; (void)type;
    uint32_t* out = (uint32_t*)pixels;
    for (int j = 0; j < height; j++) {
        int src_y = y + j;
        if (src_y < 0 || src_y >= screen_height) continue;
        for (int i = 0; i < width; i++) {
            int src_x = x + i;
            if (src_x < 0 || src_x >= screen_width) continue;
            out[j * width + i] = backbuf[src_y * screen_width + src_x];
        }
    }
}

// ============================================================
// String Query
// ============================================================

static const GLubyte gl_vendor[]     = "Alteo OS";
static const GLubyte gl_renderer[]   = "Alteo Software Rasterizer / NVIDIA NV50";
static const GLubyte gl_version[]    = "1.1 Alteo";
static const GLubyte gl_extensions[] = "";

const GLubyte* glGetString(GLenum name) {
    switch (name) {
    case GL_VENDOR:     return gl_vendor;
    case GL_RENDERER:   return gl_renderer;
    case GL_VERSION:    return gl_version;
    case GL_EXTENSIONS: return gl_extensions;
    default: return (const GLubyte*)"";
    }
}

// ============================================================
// GLU-like Helpers
// ============================================================

void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar) {
    float f = (float)fovy * 3.14159265f / 360.0f;  // half angle in radians
    float t = gl_tanf(f);
    if (gl_fabsf(t) < 0.0001f) return;
    float cotangent = 1.0f / t;
    float a = (float)aspect;
    float n = (float)zNear;
    float far_val = (float)zFar;

    float m[16];
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0]  = cotangent / a;
    m[5]  = cotangent;
    m[10] = -(far_val + n) / (far_val - n);
    m[11] = -1.0f;
    m[14] = -(2.0f * far_val * n) / (far_val - n);

    float* cur = current_matrix();
    float tmp[16];
    mat4_multiply(tmp, cur, m);
    mat4_copy(cur, tmp);
}

void gluLookAt(GLdouble eyeX, GLdouble eyeY, GLdouble eyeZ,
               GLdouble centerX, GLdouble centerY, GLdouble centerZ,
               GLdouble upX, GLdouble upY, GLdouble upZ) {
    float fx = (float)(centerX - eyeX);
    float fy = (float)(centerY - eyeY);
    float fz = (float)(centerZ - eyeZ);

    // Normalize forward
    float flen = gl_sqrtf(fx*fx + fy*fy + fz*fz);
    if (flen < 0.0001f) return;
    fx /= flen; fy /= flen; fz /= flen;

    float ux = (float)upX, uy = (float)upY, uz = (float)upZ;

    // side = forward x up
    float sx = fy*uz - fz*uy;
    float sy = fz*ux - fx*uz;
    float sz = fx*uy - fy*ux;

    // Normalize side
    float slen = gl_sqrtf(sx*sx + sy*sy + sz*sz);
    if (slen < 0.0001f) return;
    sx /= slen; sy /= slen; sz /= slen;

    // Recompute up = side x forward
    ux = sy*fz - sz*fy;
    uy = sz*fx - sx*fz;
    uz = sx*fy - sy*fx;

    float m[16];
    m[0] = sx;  m[4] = sy;  m[8]  = sz;  m[12] = 0;
    m[1] = ux;  m[5] = uy;  m[9]  = uz;  m[13] = 0;
    m[2] = -fx; m[6] = -fy; m[10] = -fz; m[14] = 0;
    m[3] = 0;   m[7] = 0;   m[11] = 0;   m[15] = 1;

    float* cur = current_matrix();
    float tmp[16];
    mat4_multiply(tmp, cur, m);
    mat4_copy(cur, tmp);

    glTranslatef(-(float)eyeX, -(float)eyeY, -(float)eyeZ);
}

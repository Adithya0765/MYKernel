// nv_3d.h - NVIDIA 3D Graphics Engine for Alteo OS
// Fixed-function pipeline (NV40-) and programmable shader pipeline (NV50+)
// Implements: vertex processing, rasterization, fragment shading
#ifndef NV_3D_H
#define NV_3D_H

#include "stdint.h"
#include "gpu.h"

// ============================================================
// 3D Object Classes
// ============================================================

// Pre-NV50 3D classes
#define NV40_3D_CLASS           0x4097      // NV40 (Curie) 3D engine
#define NV44_3D_CLASS           0x4497      // NV44 variant

// NV50+ 3D classes (Tesla)
#define NV50_3D_CLASS           0x5097      // NV50 (Tesla G80)
#define NV84_3D_CLASS           0x8297      // NV84 (Tesla G84)
#define NVA0_3D_CLASS           0x8397      // NVA0 (Tesla GT200)
#define NVA3_3D_CLASS           0x8597      // NVA3 variant

// Fermi+ 3D classes
#define NVC0_3D_CLASS           0x9097      // GF100 (Fermi)
#define NVC1_3D_CLASS           0x9197      // GF108
#define NVC8_3D_CLASS           0x9297      // GF110
#define NVE4_3D_CLASS           0xA097      // GK104 (Kepler)

// ============================================================
// 3D Engine Methods (NV50 Tesla)
// ============================================================

// Viewport
#define NV50_3D_VIEWPORT_HORIZ     0x0300   // Viewport horizontal (x | width<<16)
#define NV50_3D_VIEWPORT_VERT      0x0304   // Viewport vertical (y | height<<16)
#define NV50_3D_VIEWPORT_DEPTH_RNG 0x0308   // Depth range (near/far as float)
#define NV50_3D_SCISSOR_HORIZ      0x0380   // Scissor horizontal
#define NV50_3D_SCISSOR_VERT       0x0384   // Scissor vertical

// Vertex array (input assembly)
#define NV50_3D_VERTEX_BEGIN_GL    0x1714   // Begin primitive (GL mode)
#define NV50_3D_VERTEX_END_GL      0x1718   // End primitive
#define NV50_3D_VTX_ATTR_4F(a)    (0x1C00 + (a)*0x10)  // Vertex attribute (float4)
#define NV50_3D_VTX_ATTR_3F(a)    (0x1D00 + (a)*0x0C)  // Vertex attribute (float3)
#define NV50_3D_VTX_ATTR_2F(a)    (0x1E00 + (a)*0x08)  // Vertex attribute (float2)
#define NV50_3D_VTX_ATTR_1F(a)    (0x1F00 + (a)*0x04)  // Vertex attribute (float1)

// Vertex buffer (VBO)
#define NV50_3D_VB_ELEMENT_BASE    0x15E4   // Base element index
#define NV50_3D_VB_INSTANCE_BASE   0x15E8   // Base instance index
#define NV50_3D_VERTEX_ARRAY_START 0x1700   // Draw arrays start
#define NV50_3D_VERTEX_ARRAY_COUNT 0x1704   // Draw arrays count

// Primitive types (GL-compatible)
#define NV50_3D_PRIM_POINTS        0x01
#define NV50_3D_PRIM_LINES         0x02
#define NV50_3D_PRIM_LINE_STRIP    0x03
#define NV50_3D_PRIM_TRIANGLES     0x04
#define NV50_3D_PRIM_TRIANGLE_STRIP 0x05
#define NV50_3D_PRIM_TRIANGLE_FAN  0x06
#define NV50_3D_PRIM_QUADS         0x07
#define NV50_3D_PRIM_QUAD_STRIP    0x08

// Render target (framebuffer output)
#define NV50_3D_RT_ADDRESS_HI(i)   (0x0200 + (i)*0x20)
#define NV50_3D_RT_ADDRESS_LO(i)   (0x0204 + (i)*0x20)
#define NV50_3D_RT_FORMAT(i)       (0x0208 + (i)*0x20)
#define NV50_3D_RT_TILE_MODE(i)    (0x020C + (i)*0x20)
#define NV50_3D_RT_LAYER_STRIDE(i) (0x0210 + (i)*0x20)
#define NV50_3D_RT_PITCH(i)        (0x0214 + (i)*0x20)

// Depth/stencil buffer
#define NV50_3D_ZETA_ADDRESS_HI    0x0258
#define NV50_3D_ZETA_ADDRESS_LO    0x025C
#define NV50_3D_ZETA_FORMAT        0x0260
#define NV50_3D_ZETA_TILE_MODE     0x0264

// Depth test
#define NV50_3D_DEPTH_TEST_ENABLE  0x12CC
#define NV50_3D_DEPTH_TEST_FUNC    0x12D0
#define NV50_3D_DEPTH_WRITE_ENABLE 0x12D4

// Blending
#define NV50_3D_BLEND_ENABLE(i)    (0x1360 + (i)*4)
#define NV50_3D_BLEND_EQUATION_RGB 0x1340
#define NV50_3D_BLEND_FUNC_SRC_RGB 0x1344
#define NV50_3D_BLEND_FUNC_DST_RGB 0x1348
#define NV50_3D_BLEND_EQUATION_A   0x134C
#define NV50_3D_BLEND_FUNC_SRC_A   0x1350
#define NV50_3D_BLEND_FUNC_DST_A   0x1354
#define NV50_3D_BLEND_COLOR        0x1358

// Rasterizer
#define NV50_3D_CULL_FACE_ENABLE   0x1690
#define NV50_3D_CULL_FACE          0x1694   // 0x404=BACK, 0x405=FRONT
#define NV50_3D_FRONT_FACE         0x1698   // 0x900=CW, 0x901=CCW
#define NV50_3D_POLYGON_MODE_FRONT 0x169C
#define NV50_3D_POLYGON_MODE_BACK  0x16A0
#define NV50_3D_POLYGON_OFFSET_EN  0x16A4

// Shader program binding
#define NV50_3D_VP_START_ID        0x1414   // Vertex program start address
#define NV50_3D_FP_START_ID        0x1414   // Fragment program start address (different subchannel)
#define NV50_3D_GP_START_ID        0x1418   // Geometry program start

// Texture
#define NV50_3D_TEX_SAMPLER(i)     (0x1400 + (i)*0x20)
#define NV50_3D_TEX_TIC(i)         (0x1000 + (i)*0x20)

// Clear
#define NV50_3D_CLEAR_COLOR(i)     (0x03D0 + (i)*4)  // Clear color RGBA (4 floats)
#define NV50_3D_CLEAR_DEPTH        0x03E0   // Clear depth value (float)
#define NV50_3D_CLEAR_STENCIL      0x03E4   // Clear stencil value
#define NV50_3D_CLEAR_BUFFERS      0x19D0   // Trigger clear (bitmask)

// Clear buffer bits
#define NV50_3D_CLEAR_BUF_COLOR    (1 << 0)
#define NV50_3D_CLEAR_BUF_DEPTH    (1 << 1)
#define NV50_3D_CLEAR_BUF_STENCIL  (1 << 2)

// Depth function values
#define NV50_3D_DEPTH_FUNC_NEVER    0x0200
#define NV50_3D_DEPTH_FUNC_LESS     0x0201
#define NV50_3D_DEPTH_FUNC_EQUAL    0x0202
#define NV50_3D_DEPTH_FUNC_LEQUAL   0x0203
#define NV50_3D_DEPTH_FUNC_GREATER  0x0204
#define NV50_3D_DEPTH_FUNC_NOTEQUAL 0x0205
#define NV50_3D_DEPTH_FUNC_GEQUAL   0x0206
#define NV50_3D_DEPTH_FUNC_ALWAYS   0x0207

// Blend equation values
#define NV50_3D_BLEND_EQ_ADD        0x8006
#define NV50_3D_BLEND_EQ_SUB        0x800A
#define NV50_3D_BLEND_EQ_REV_SUB   0x800B
#define NV50_3D_BLEND_EQ_MIN        0x8007
#define NV50_3D_BLEND_EQ_MAX        0x8008

// Blend function values
#define NV50_3D_BLEND_FUNC_ZERO            0x0000
#define NV50_3D_BLEND_FUNC_ONE             0x0001
#define NV50_3D_BLEND_FUNC_SRC_COLOR       0x0300
#define NV50_3D_BLEND_FUNC_SRC_ALPHA       0x0302
#define NV50_3D_BLEND_FUNC_DST_ALPHA       0x0304
#define NV50_3D_BLEND_FUNC_DST_COLOR       0x0306
#define NV50_3D_BLEND_FUNC_ONE_MINUS_SRC_ALPHA 0x0303
#define NV50_3D_BLEND_FUNC_ONE_MINUS_DST_ALPHA 0x0305

// ============================================================
// Software Vertex / Math Types
// ============================================================

typedef struct {
    float x, y, z, w;
} nv_vec4_t;

typedef struct {
    float x, y, z;
} nv_vec3_t;

typedef struct {
    float x, y;
} nv_vec2_t;

// 4x4 matrix (column-major, OpenGL convention)
typedef struct {
    float m[16];
} nv_mat4_t;

// Vertex with position, color, texcoord, normal
typedef struct {
    nv_vec4_t position;     // Clip-space position (after transform)
    nv_vec4_t color;        // Vertex color (RGBA float)
    nv_vec2_t texcoord;     // Texture coordinate
    nv_vec3_t normal;       // Vertex normal
} nv_vertex_t;

// ============================================================
// 3D Pipeline State
// ============================================================

typedef struct {
    int      initialized;
    uint32_t class_3d;          // Active 3D engine class

    // Render target
    uint64_t rt_address;        // VRAM address of render target
    int      rt_width;
    int      rt_height;
    int      rt_pitch;
    uint32_t rt_format;         // Pixel format

    // Depth buffer
    uint64_t depth_address;
    int      depth_enabled;
    int      depth_write;
    uint32_t depth_func;

    // Viewport
    int      vp_x, vp_y, vp_w, vp_h;
    float    depth_near, depth_far;

    // Blending
    int      blend_enabled;
    uint32_t blend_eq_rgb, blend_eq_a;
    uint32_t blend_src_rgb, blend_dst_rgb;
    uint32_t blend_src_a, blend_dst_a;

    // Culling
    int      cull_enabled;
    uint32_t cull_face;
    uint32_t front_face;

    // Transform matrices (software fixed-function pipeline)
    nv_mat4_t modelview;
    nv_mat4_t projection;
    nv_mat4_t mvp;              // Combined model-view-projection

    // Clear values
    float    clear_color[4];    // RGBA
    float    clear_depth;
    int      clear_stencil;

    // Statistics
    uint64_t triangles_drawn;
    uint64_t draw_calls;
} nv_3d_state_t;

// ---- 3D Engine Initialization ----
int  nv_3d_init(void);
void nv_3d_shutdown(void);
nv_3d_state_t* nv_3d_get_state(void);

// ---- Render Target ----
void nv_3d_set_render_target(uint64_t vram_addr, int width, int height, int pitch);
void nv_3d_set_depth_buffer(uint64_t vram_addr);

// ---- Viewport ----
void nv_3d_set_viewport(int x, int y, int w, int h);
void nv_3d_set_depth_range(float near, float far);

// ---- Depth Test ----
void nv_3d_depth_test(int enable);
void nv_3d_depth_write(int enable);
void nv_3d_depth_func(uint32_t func);

// ---- Blending ----
void nv_3d_blend_enable(int enable);
void nv_3d_blend_func(uint32_t src_rgb, uint32_t dst_rgb, uint32_t src_a, uint32_t dst_a);
void nv_3d_blend_equation(uint32_t eq_rgb, uint32_t eq_a);

// ---- Culling ----
void nv_3d_cull_face(int enable, uint32_t face);
void nv_3d_front_face(uint32_t winding);

// ---- Transform (fixed-function emulation) ----
void nv_3d_load_identity(nv_mat4_t* mat);
void nv_3d_load_ortho(nv_mat4_t* mat, float l, float r, float b, float t, float n, float f);
void nv_3d_load_perspective(nv_mat4_t* mat, float fov_deg, float aspect, float near, float far);
void nv_3d_multiply_mat4(nv_mat4_t* result, const nv_mat4_t* a, const nv_mat4_t* b);
void nv_3d_set_modelview(const nv_mat4_t* mat);
void nv_3d_set_projection(const nv_mat4_t* mat);
void nv_3d_update_mvp(void);

// ---- Drawing ----
void nv_3d_clear(uint32_t buffers);
void nv_3d_clear_color(float r, float g, float b, float a);
void nv_3d_clear_depth_val(float depth);

void nv_3d_draw_begin(uint32_t primitive);
void nv_3d_draw_vertex_4f(int attr, float x, float y, float z, float w);
void nv_3d_draw_vertex_3f(int attr, float x, float y, float z);
void nv_3d_draw_vertex_2f(int attr, float x, float y);
void nv_3d_draw_end(void);
void nv_3d_draw_arrays(uint32_t primitive, int first, int count);

// ---- Software Rasterizer (fallback if no GPU) ----
void nv_3d_sw_draw_triangle(nv_vertex_t* v0, nv_vertex_t* v1, nv_vertex_t* v2,
                             uint32_t* framebuf, int fb_width, int fb_height);

#endif

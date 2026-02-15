// opengl.h - OpenGL 1.x Subset for Alteo OS
// Provides a fixed-function OpenGL-like API that maps to NVIDIA 3D engine
// Implements core GL 1.1 calls: matrix stack, immediate mode, textures, state
#ifndef OPENGL_H
#define OPENGL_H

#include "stdint.h"

// ============================================================
// OpenGL Type Definitions
// ============================================================

typedef unsigned int    GLenum;
typedef unsigned int    GLbitfield;
typedef unsigned int    GLuint;
typedef int             GLint;
typedef int             GLsizei;
typedef float           GLfloat;
typedef float           GLclampf;
typedef double          GLdouble;
typedef double          GLclampd;
typedef unsigned char   GLubyte;
typedef unsigned char   GLboolean;
typedef void            GLvoid;

#define GL_FALSE        0
#define GL_TRUE         1

// ============================================================
// Primitive Types
// ============================================================

#define GL_POINTS           0x0000
#define GL_LINES            0x0001
#define GL_LINE_STRIP       0x0003
#define GL_TRIANGLES        0x0004
#define GL_TRIANGLE_STRIP   0x0005
#define GL_TRIANGLE_FAN     0x0006
#define GL_QUADS            0x0007
#define GL_QUAD_STRIP       0x0008
#define GL_POLYGON          0x0009

// ============================================================
// Matrix Mode
// ============================================================

#define GL_MODELVIEW        0x1700
#define GL_PROJECTION       0x1701
#define GL_TEXTURE          0x1702

// ============================================================
// Clear Bits
// ============================================================

#define GL_COLOR_BUFFER_BIT     0x00004000
#define GL_DEPTH_BUFFER_BIT     0x00000100
#define GL_STENCIL_BUFFER_BIT   0x00000400

// ============================================================
// Enable/Disable Caps
// ============================================================

#define GL_DEPTH_TEST       0x0B71
#define GL_BLEND            0x0BE2
#define GL_CULL_FACE        0x0B44
#define GL_TEXTURE_2D       0x0DE1
#define GL_LIGHTING         0x0B50
#define GL_ALPHA_TEST       0x0BC0
#define GL_SCISSOR_TEST     0x0C11
#define GL_NORMALIZE        0x0BA1
#define GL_COLOR_MATERIAL   0x0B57
#define GL_FOG              0x0B60

// ============================================================
// Depth Function
// ============================================================

#define GL_NEVER            0x0200
#define GL_LESS             0x0201
#define GL_EQUAL            0x0202
#define GL_LEQUAL           0x0203
#define GL_GREATER          0x0204
#define GL_NOTEQUAL         0x0205
#define GL_GEQUAL           0x0206
#define GL_ALWAYS           0x0207

// ============================================================
// Blend Function
// ============================================================

#define GL_ZERO                     0
#define GL_ONE                      1
#define GL_SRC_COLOR                0x0300
#define GL_ONE_MINUS_SRC_COLOR      0x0301
#define GL_SRC_ALPHA                0x0302
#define GL_ONE_MINUS_SRC_ALPHA      0x0303
#define GL_DST_ALPHA                0x0304
#define GL_ONE_MINUS_DST_ALPHA      0x0305
#define GL_DST_COLOR                0x0306
#define GL_ONE_MINUS_DST_COLOR      0x0307

// ============================================================
// Face Culling
// ============================================================

#define GL_FRONT            0x0404
#define GL_BACK             0x0405
#define GL_FRONT_AND_BACK   0x0408

#define GL_CW               0x0900
#define GL_CCW              0x0901

// ============================================================
// Texture
// ============================================================

#define GL_TEXTURE_MIN_FILTER   0x2801
#define GL_TEXTURE_MAG_FILTER   0x2800
#define GL_TEXTURE_WRAP_S       0x2802
#define GL_TEXTURE_WRAP_T       0x2803
#define GL_NEAREST              0x2600
#define GL_LINEAR               0x2601
#define GL_REPEAT               0x2901
#define GL_CLAMP                0x2900
#define GL_RGBA                 0x1908
#define GL_RGB                  0x1907
#define GL_UNSIGNED_BYTE        0x1401

// ============================================================
// Error Codes
// ============================================================

#define GL_NO_ERROR         0
#define GL_INVALID_ENUM     0x0500
#define GL_INVALID_VALUE    0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_OUT_OF_MEMORY    0x0505

// ============================================================
// String Queries
// ============================================================

#define GL_VENDOR           0x1F00
#define GL_RENDERER         0x1F01
#define GL_VERSION          0x1F02
#define GL_EXTENSIONS       0x1F03

// ============================================================
// Max Constants
// ============================================================

#define GL_MAX_MATRIX_STACK_DEPTH   16
#define GL_MAX_TEXTURES             16
#define GL_MAX_IMMEDIATE_VERTICES   4096

// ============================================================
// OpenGL API Functions
// ============================================================

// ---- Initialization ----
int  gl_init(void);
void gl_shutdown(void);

// ---- State ----
void glEnable(GLenum cap);
void glDisable(GLenum cap);
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void glDepthFunc(GLenum func);
void glBlendFunc(GLenum sfactor, GLenum dfactor);
void glCullFace(GLenum mode);
void glFrontFace(GLenum mode);
void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void glClearDepth(GLclampd depth);
void glClear(GLbitfield mask);
GLenum glGetError(void);

// ---- Matrix Operations ----
void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glPushMatrix(void);
void glPopMatrix(void);
void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble near, GLdouble far);
void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
               GLdouble near, GLdouble far);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glMultMatrixf(const GLfloat* m);

// ---- Immediate Mode ----
void glBegin(GLenum mode);
void glEnd(void);
void glVertex2f(GLfloat x, GLfloat y);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glColor3f(GLfloat r, GLfloat g, GLfloat b);
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glColor3ub(GLubyte r, GLubyte g, GLubyte b);
void glTexCoord2f(GLfloat s, GLfloat t);
void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz);

// ---- Texture ----
void glGenTextures(GLsizei n, GLuint* textures);
void glDeleteTextures(GLsizei n, const GLuint* textures);
void glBindTexture(GLenum target, GLuint texture);
void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                  GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid* pixels);
void glTexParameteri(GLenum target, GLenum pname, GLint param);

// ---- Flush / Finish ----
void glFlush(void);
void glFinish(void);

// ---- Framebuffer Read ----
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                  GLenum format, GLenum type, GLvoid* pixels);

// ---- String Query ----
const GLubyte* glGetString(GLenum name);

// ---- GLU-like helpers ----
void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zNear, GLdouble zFar);
void gluLookAt(GLdouble eyeX, GLdouble eyeY, GLdouble eyeZ,
               GLdouble centerX, GLdouble centerY, GLdouble centerZ,
               GLdouble upX, GLdouble upY, GLdouble upZ);

#endif

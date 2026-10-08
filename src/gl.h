/* OpenGL 3.3 core for vrr. macOS declares and exports all of it. Windows' opengl32.dll has
   only OpenGL 1.1: the newer functions vrr calls are loaded from the driver once the context
   exists (gl_load_functions), each into a pointer that its name is #defined to. */
#ifndef VRR_GL_H
#define VRR_GL_H

#ifdef __APPLE__

#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>

static int gl_load_functions(void) { return 1; }

#else

#include <SDL.h>
#include <SDL_opengl.h>

/* The functions after OpenGL 1.1 that vrr calls */
#define VRR_GL_FUNCTIONS(X) \
  X(PFNGLACTIVETEXTUREPROC, glActiveTexture) \
  X(PFNGLATTACHSHADERPROC, glAttachShader) \
  X(PFNGLBINDBUFFERPROC, glBindBuffer) \
  X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
  X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer) \
  X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
  X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer) \
  X(PFNGLBUFFERDATAPROC, glBufferData) \
  X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) \
  X(PFNGLCOMPILESHADERPROC, glCompileShader) \
  X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
  X(PFNGLCREATESHADERPROC, glCreateShader) \
  X(PFNGLDRAWBUFFERSPROC, glDrawBuffers) \
  X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
  X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
  X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D) \
  X(PFNGLGENBUFFERSPROC, glGenBuffers) \
  X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
  X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) \
  X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
  X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
  X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
  X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
  X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
  X(PFNGLRENDERBUFFERSTORAGEPROC, glRenderbufferStorage) \
  X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample) \
  X(PFNGLSHADERSOURCEPROC, glShaderSource) \
  X(PFNGLUNIFORM1FPROC, glUniform1f) \
  X(PFNGLUNIFORM1IPROC, glUniform1i) \
  X(PFNGLUNIFORM2FPROC, glUniform2f) \
  X(PFNGLUSEPROGRAMPROC, glUseProgram) \
  X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer)

#define VRR_GL_POINTER(type, name) static type vrr_##name;
VRR_GL_FUNCTIONS(VRR_GL_POINTER)

/* Prints the first one the driver lacks and returns 0 */
static int gl_load_functions(void)
{
#define VRR_GL_LOAD(type, name) \
  if (!(vrr_##name = (type)SDL_GL_GetProcAddress(#name))) \
  { \
    fprintf(stderr, "OpenGL: no %s\n", #name); \
    return 0; \
  }
  VRR_GL_FUNCTIONS(VRR_GL_LOAD)
  return 1;
}

#define glActiveTexture vrr_glActiveTexture
#define glAttachShader vrr_glAttachShader
#define glBindBuffer vrr_glBindBuffer
#define glBindFramebuffer vrr_glBindFramebuffer
#define glBindRenderbuffer vrr_glBindRenderbuffer
#define glBindVertexArray vrr_glBindVertexArray
#define glBlitFramebuffer vrr_glBlitFramebuffer
#define glBufferData vrr_glBufferData
#define glCheckFramebufferStatus vrr_glCheckFramebufferStatus
#define glCompileShader vrr_glCompileShader
#define glCreateProgram vrr_glCreateProgram
#define glCreateShader vrr_glCreateShader
#define glDrawBuffers vrr_glDrawBuffers
#define glEnableVertexAttribArray vrr_glEnableVertexAttribArray
#define glFramebufferRenderbuffer vrr_glFramebufferRenderbuffer
#define glFramebufferTexture2D vrr_glFramebufferTexture2D
#define glGenBuffers vrr_glGenBuffers
#define glGenFramebuffers vrr_glGenFramebuffers
#define glGenRenderbuffers vrr_glGenRenderbuffers
#define glGenVertexArrays vrr_glGenVertexArrays
#define glGetShaderInfoLog vrr_glGetShaderInfoLog
#define glGetShaderiv vrr_glGetShaderiv
#define glGetUniformLocation vrr_glGetUniformLocation
#define glLinkProgram vrr_glLinkProgram
#define glRenderbufferStorage vrr_glRenderbufferStorage
#define glRenderbufferStorageMultisample vrr_glRenderbufferStorageMultisample
#define glShaderSource vrr_glShaderSource
#define glUniform1f vrr_glUniform1f
#define glUniform1i vrr_glUniform1i
#define glUniform2f vrr_glUniform2f
#define glUseProgram vrr_glUseProgram
#define glVertexAttribPointer vrr_glVertexAttribPointer

#endif

#endif

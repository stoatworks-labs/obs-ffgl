#include "gl_state_guard.h"

// oxbow's, via the submodule, so the host core and this file cannot end up
// disagreeing about which GL header the platform uses.
#include <gl/gl_headers.h>

namespace obsffgl {
namespace {

// glGetBooleanv on an enable bit, as a bool. glIsEnabled would do, but the
// two forms disagree for a couple of legacy enums and mixing them in one file
// invites the reader to wonder which; use one.
bool enabled(GLenum cap) { return glIsEnabled(cap) == GL_TRUE; }

void setEnabled(GLenum cap, bool on) {
  if (on) {
    glEnable(cap);
  } else {
    glDisable(cap);
  }
}

}  // namespace

void resetForPlugin() {
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);

  // Blending off, not "some sensible blend". FFGL's contract is that the
  // plugin owns the output pixel; an effect that wants to composite does it
  // in its own shader, and Resolume hands it a non-blending target.
  glDisable(GL_BLEND);

  // OBS composites through an sRGB-aware pipeline. FFGL predates all of that
  // and works in plain 8-bit; leaving this on applies a second encode to
  // everything the plugin writes.
  glDisable(GL_FRAMEBUFFER_SRGB);

  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);

  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);

  // Unbind libobs's sampler objects so the plugin's own texture parameters
  // are the ones that apply.
  for (int unit = 0; unit < 8; ++unit) glBindSampler(static_cast<GLuint>(unit), 0);
}

GlStateGuard::GlStateGuard() {
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo_);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo_);
  glGetIntegerv(GL_CURRENT_PROGRAM, &program_);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray_);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer_);

  // The element array binding is *part of VAO state*, so it must be read while
  // the original VAO is still bound and restored after that VAO is back. The
  // ordering below is not incidental.
  glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elementBuffer_);

  glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture_);
  for (int unit = 0; unit < kMaxTextureUnits; ++unit) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture2d_[unit]);
    glGetIntegerv(GL_SAMPLER_BINDING, &sampler_[unit]);
  }
  glActiveTexture(static_cast<GLenum>(activeTexture_));

  glGetIntegerv(GL_VIEWPORT, viewport_);
  glGetIntegerv(GL_SCISSOR_BOX, scissorBox_);

  glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb_);
  glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb_);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha_);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha_);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEquationRgb_);
  glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEquationAlpha_);

  glGetIntegerv(GL_CULL_FACE_MODE, &cullFaceMode_);
  glGetIntegerv(GL_FRONT_FACE, &frontFace_);
  glGetIntegerv(GL_DEPTH_FUNC, &depthFunc_);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment_);
  glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment_);

  GLboolean mask[4] = {};
  glGetBooleanv(GL_COLOR_WRITEMASK, mask);
  for (int i = 0; i < 4; ++i) colorMask_[i] = mask[i];

  GLboolean depthMask = GL_TRUE;
  glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
  depthMask_ = depthMask;

  blend_ = enabled(GL_BLEND);
  scissor_ = enabled(GL_SCISSOR_TEST);
  depthTest_ = enabled(GL_DEPTH_TEST);
  stencilTest_ = enabled(GL_STENCIL_TEST);
  cullFace_ = enabled(GL_CULL_FACE);
  framebufferSrgb_ = enabled(GL_FRAMEBUFFER_SRGB);
}

GlStateGuard::~GlStateGuard() {
  // VAO first: binding a VAO replaces the element array binding and the
  // enabled attribute arrays wholesale, so anything restored before it would
  // be immediately overwritten.
  glBindVertexArray(static_cast<GLuint>(vertexArray_));
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(elementBuffer_));
  glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer_));

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo_));
  glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo_));
  glUseProgram(static_cast<GLuint>(program_));

  for (int unit = 0; unit < kMaxTextureUnits; ++unit) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture2d_[unit]));
    glBindSampler(static_cast<GLuint>(unit), static_cast<GLuint>(sampler_[unit]));
  }
  glActiveTexture(static_cast<GLenum>(activeTexture_));

  glViewport(viewport_[0], viewport_[1], viewport_[2], viewport_[3]);
  glScissor(scissorBox_[0], scissorBox_[1], scissorBox_[2], scissorBox_[3]);

  glBlendFuncSeparate(static_cast<GLenum>(blendSrcRgb_), static_cast<GLenum>(blendDstRgb_),
                      static_cast<GLenum>(blendSrcAlpha_), static_cast<GLenum>(blendDstAlpha_));
  glBlendEquationSeparate(static_cast<GLenum>(blendEquationRgb_),
                          static_cast<GLenum>(blendEquationAlpha_));

  glCullFace(static_cast<GLenum>(cullFaceMode_));
  glFrontFace(static_cast<GLenum>(frontFace_));
  glDepthFunc(static_cast<GLenum>(depthFunc_));
  glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment_);
  glPixelStorei(GL_PACK_ALIGNMENT, packAlignment_);

  glColorMask(colorMask_[0], colorMask_[1], colorMask_[2], colorMask_[3]);
  glDepthMask(depthMask_);

  setEnabled(GL_BLEND, blend_);
  setEnabled(GL_SCISSOR_TEST, scissor_);
  setEnabled(GL_DEPTH_TEST, depthTest_);
  setEnabled(GL_STENCIL_TEST, stencilTest_);
  setEnabled(GL_CULL_FACE, cullFace_);
  setEnabled(GL_FRAMEBUFFER_SRGB, framebufferSrgb_);

  // Whatever the plugin left in the error queue is the plugin's business, but
  // leaving it there would make the *next* piece of code to call glGetError
  // blame itself. Drain it.
  while (glGetError() != GL_NO_ERROR) {
  }
}

}  // namespace obsffgl

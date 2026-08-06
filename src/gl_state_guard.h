#pragma once
//
// Save every piece of GL state libobs believes it owns, run somebody else's
// renderer, put it all back.
//
// This is the single most important file in the plugin, and the reason is not
// tidiness — it is that **libobs shadows its GL state and skips redundant
// binds**. `struct gs_device` (libobs-opengl/gl-subsystem.h) caches
// `cur_fbo`, `cur_textures[GS_MAX_TEXTURES]`, `cur_program`,
// `cur_vertex_buffer`, `cur_viewport` and friends, and every setter is guarded
// by a comparison against the cached value. `set_current_fbo` is the clearest
// case:
//
//     if (device->cur_fbo != fbo) { gl_bind_framebuffer(...); }
//
// So if an FFGL plugin binds its own framebuffer and leaves it bound, libobs's
// *next* `gs_set_render_target` with the same target sees a matching shadow,
// binds nothing, and renders the rest of the scene into the plugin's private
// buffer. Nothing errors. OBS just quietly draws the wrong thing somewhere
// else on the canvas, one frame later, which reads as a bug anywhere but here.
//
// FFGL plugins are *expected* to do this. oxbow's host documents it as a fact
// of the format: plugins built on the stock SDK restore bindings to 0 rather
// than to the previous value, because the SDK's Scoped* helpers do exactly
// that. There is no version of this that the plugin side can fix.
//
// The rule this file enforces: **restore, do not reset**. Setting things to 0
// would be just as wrong as leaving them trashed — libobs's shadow does not
// say 0, it says whatever OBS bound before we were called. Only restoring the
// captured values makes reality agree with the shadow again.
//
// Cost: about forty `glGet*` calls per filter per frame. Measured rather than
// assumed — see docs/verification.md. `glGet` of client-side state does not
// stall the pipeline the way `glReadPixels` does; none of these are queries
// that force a flush.

#include <cstdint>

namespace obsffgl {

/// Put GL into the state an FFGL plugin is entitled to assume.
///
/// Saving OBS's state is only half the job. The other half is that OBS's state
/// is *hostile* to a guest renderer, and an FFGL plugin will not defend itself
/// because in Resolume it never has to. Measured here: with OBS's own state
/// left in place, a plugin renders, returns FF_SUCCESS, and leaves a complete
/// framebuffer entirely black — every draw discarded before it reaches a pixel.
///
/// The offenders, all of which OBS legitimately leaves set:
///
///   - **GL_SCISSOR_TEST** with a box for whatever OBS last cropped. Clips the
///     plugin's full-screen quad to nothing.
///   - **GL_STENCIL_TEST**, **GL_DEPTH_TEST**, **GL_CULL_FACE** — any of which
///     can reject the quad outright.
///   - **glColorMask** with channels off, and **GL_FRAMEBUFFER_SRGB**, which
///     does not discard the draw but does silently recolour it.
///   - **Sampler objects.** libobs binds them; FFGL plugins set filtering on
///     the texture. A bound sampler object *overrides* the texture's own
///     parameters, so the plugin's input samples with the wrong filter and
///     wrap — and on a NEAREST/CLAMP mismatch that is a visibly wrong picture
///     rather than an obviously broken one.
///
/// Call this immediately after constructing a GlStateGuard: the guard has
/// already recorded what to put back, so anything trampled here is restored.
void resetForPlugin();

/// Captures on construction, restores on destruction. Stack-allocate it around
/// the call into FFGL and do not let it outlive the graphics thread's context.
///
/// Deliberately not copyable or movable: two live guards would restore in an
/// order nobody can reason about.
class GlStateGuard {
 public:
  GlStateGuard();
  ~GlStateGuard();

  GlStateGuard(const GlStateGuard&) = delete;
  GlStateGuard& operator=(const GlStateGuard&) = delete;

 private:
  // libobs's own limit; there is no point saving units it cannot bind.
  static constexpr int kMaxTextureUnits = 8;

  int32_t drawFbo_ = 0;
  int32_t readFbo_ = 0;
  int32_t program_ = 0;
  int32_t vertexArray_ = 0;
  int32_t arrayBuffer_ = 0;
  int32_t elementBuffer_ = 0;

  int32_t activeTexture_ = 0;
  int32_t texture2d_[kMaxTextureUnits] = {};
  int32_t sampler_[kMaxTextureUnits] = {};

  int32_t viewport_[4] = {};
  int32_t scissorBox_[4] = {};

  // Blend is split four ways in core GL and libobs uses the separate form.
  int32_t blendSrcRgb_ = 0;
  int32_t blendDstRgb_ = 0;
  int32_t blendSrcAlpha_ = 0;
  int32_t blendDstAlpha_ = 0;
  int32_t blendEquationRgb_ = 0;
  int32_t blendEquationAlpha_ = 0;

  int32_t cullFaceMode_ = 0;
  int32_t frontFace_ = 0;
  int32_t depthFunc_ = 0;
  int32_t unpackAlignment_ = 0;
  int32_t packAlignment_ = 0;

  uint8_t colorMask_[4] = {};
  uint8_t depthMask_ = 0;

  bool blend_ = false;
  bool scissor_ = false;
  bool depthTest_ = false;
  bool stencilTest_ = false;
  bool cullFace_ = false;
  // OBS renders through an sRGB-aware pipeline and toggles this per draw. An
  // FFGL plugin that leaves it on turns everything OBS draws afterwards pale.
  bool framebufferSrgb_ = false;
};

}  // namespace obsffgl

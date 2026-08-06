#include "ffgl_effect.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#include <graphics/graphics.h>
#include <graphics/vec4.h>

#include <ffgl/FFGL.h>

#include <gl/gl_headers.h>

#include "ffgl_catalog.h"
#include "gl_state_guard.h"

namespace obsffgl {
namespace {

constexpr const char* kPluginKey = "ffgl_plugin";

/// Types that are a number the operator drags. Everything FFGL calls a colour
/// component or a position is one of these too — FFGL has no vector type, it
/// has four adjacent floats.
bool isSliderType(uint32_t type) {
  return type == FF_TYPE_STANDARD || type == FF_TYPE_RED || type == FF_TYPE_GREEN ||
         type == FF_TYPE_BLUE || type == FF_TYPE_ALPHA || type == FF_TYPE_HUE ||
         type == FF_TYPE_SATURATION || type == FF_TYPE_BRIGHTNESS || type == FF_TYPE_XPOS ||
         type == FF_TYPE_YPOS || type == FF_TYPE_INTEGER || type == FF_TYPE_OPTION;
}

bool isTextType(uint32_t type) { return type == FF_TYPE_TEXT || type == FF_TYPE_FILE; }

bool isToggleType(uint32_t type) { return type == FF_TYPE_BOOLEAN || type == FF_TYPE_EVENT; }

/// FFGL's texture handle, out of an OBS texture.
///
/// **`gs_texture_get_obj` returns a pointer to the GLuint, not the GLuint.**
/// On the D3D11 backend the same call returns the `ID3D11Texture2D*` itself,
/// so the signature reads as "the object" and the GL backend is the odd one
/// out — `gl-texture2d.c` ends with `return &tex2d->base.texture;`. libobs's
/// own EGL code dereferences it, which is the only documentation there is.
///
/// Getting this wrong does not crash. It hands FFGL a pointer value as a
/// texture name, GL finds no such texture, samples zero, and the plugin
/// renders a plausible black frame — indistinguishable from a plugin that
/// simply does not work.
GLuint glTextureName(gs_texture_t* texture) {
  if (texture == nullptr) return 0;
  void* object = gs_texture_get_obj(texture);
  if (object == nullptr) return 0;
  return *static_cast<GLuint*>(object);
}

}  // namespace

FfglEffect::FfglEffect(obs_source_t* self) : self_(self) {}

FfglEffect::~FfglEffect() {
  // Destruction arrives on whatever thread released the last reference, and
  // every one of these is a GL object. obs_enter_graphics makes the context
  // current; it is re-entrant, so doing it here is safe even when the caller
  // already holds it.
  obs_enter_graphics();
  destroyInstance();
  if (inputRender_ != nullptr) gs_texrender_destroy(inputRender_);
  if (outputTexture_ != nullptr) gs_texture_destroy(outputTexture_);
  obs_leave_graphics();
}

void FfglEffect::defaults(obs_data_t* settings) {
  obs_data_set_default_string(settings, kPluginKey, "");
}

std::string FfglEffect::paramKey(const oxbow::FfglParam& param) const {
  char key[64];
  const std::string& id = library_ != nullptr ? library_->info().uniqueId : std::string("none");
  std::snprintf(key, sizeof(key), "p_%s_%u", id.c_str(), param.index);
  return key;
}

void FfglEffect::update(obs_data_t* settings) {
  requestedPath_ = obs_data_get_string(settings, kPluginKey);

  // Parameter values are read here rather than in render so that the graphics
  // thread never touches obs_data. The vectors are sized by ensureInstance, so
  // before the first frame there is nothing to read into — which is fine, the
  // first render calls back into update through obs_source_update_properties.
  if (library_ == nullptr) return;

  const std::vector<oxbow::FfglParam>& params = library_->info().params;
  for (size_t i = 0; i < params.size() && i < paramValues_.size(); ++i) {
    const oxbow::FfglParam& param = params[i];
    const std::string key = paramKey(param);

    // A key the operator has never touched is *absent*, and obs_data_get_*
    // answers 0 for absent — which would push 0 into every parameter of a
    // freshly added filter and wipe the plugin's own defaults. Ask first.
    if (!obs_data_has_user_value(settings, key.c_str())) {
      paramValues_[i] = param.defaultValue;
      textValues_[i] = param.defaultText;
      continue;
    }

    if (isTextType(param.type)) {
      textValues_[i] = obs_data_get_string(settings, key.c_str());
    } else if (isToggleType(param.type)) {
      paramValues_[i] = obs_data_get_bool(settings, key.c_str()) ? 1.0f : 0.0f;
    } else {
      paramValues_[i] = static_cast<float>(obs_data_get_double(settings, key.c_str()));
    }
  }
  paramsDirty_ = true;
}

obs_properties_t* FfglEffect::properties() {
  obs_properties_t* props = obs_properties_create();

  obs_property_t* list =
      obs_properties_add_list(props, kPluginKey, obs_module_text("Plugin"),
                              OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
  obs_property_list_add_string(list, obs_module_text("Plugin.None"), "");

  for (const CatalogEntry& entry : catalog()) {
    // The four-character id disambiguates the several plugins in this fleet
    // that ship two bundles differing only by a word ("Downpour" and
    // "Downpour Over").
    const std::string label = entry.name + "  [" + entry.uniqueId + "]";
    obs_property_list_add_string(list, label.c_str(), entry.path.c_str());
  }

  // Changing the plugin has to rebuild the rest of the dialog, because the
  // rest of the dialog *is* the plugin's parameters.
  obs_property_set_modified_callback(
      list, [](obs_properties_t*, obs_property_t*, obs_data_t*) { return true; });

  // Where we looked. A plugin that is installed but absent from the list is
  // the most likely first complaint, and this answers it without a round trip.
  std::string help = "Searched:\n";
  for (const std::string& path : searchPaths()) help += "  " + path + "\n";
  help += "\nSet OBS_FFGL_PATH to add directories.";
  obs_property_set_long_description(list, help.c_str());

  if (library_ == nullptr) return props;

  const oxbow::FfglInfo& info = library_->info();
  for (const oxbow::FfglParam& param : info.params) {
    const std::string key = paramKey(param);
    const char* label = param.name.c_str();

    if (isToggleType(param.type)) {
      obs_properties_add_bool(props, key.c_str(), label);
    } else if (param.type == FF_TYPE_TEXT) {
      obs_properties_add_text(props, key.c_str(), label, OBS_TEXT_DEFAULT);
    } else if (param.type == FF_TYPE_FILE) {
      obs_properties_add_path(props, key.c_str(), label, OBS_PATH_FILE, nullptr, nullptr);
    } else if (isSliderType(param.type)) {
      // The range is the plugin's own, from FF_GET_RANGE, and defaults to
      // 0..1 when the plugin does not answer — which is FFGL's normalised
      // convention and what Resolume shows.
      const float min = param.rangeMin;
      const float max = param.rangeMax > param.rangeMin ? param.rangeMax : param.rangeMin + 1.0f;
      const double step = (max - min) / 1000.0;
      obs_properties_add_float_slider(props, key.c_str(), label, min, max, step);
    }
  }

  return props;
}

void FfglEffect::destroyInstance() {
  instance_.reset();
  library_ = nullptr;
  loadedPath_.clear();
  instanceWidth_ = 0;
  instanceHeight_ = 0;
}

bool FfglEffect::ensureInstance(uint32_t width, uint32_t height) {
  if (requestedPath_.empty()) return false;

  if (loadedPath_ != requestedPath_) {
    destroyInstance();
    reportedFailure_ = false;

    std::string error;
    library_ = acquire(requestedPath_, error);
    if (library_ == nullptr) return false;

    loadedPath_ = requestedPath_;

    const std::vector<oxbow::FfglParam>& params = library_->info().params;
    paramValues_.assign(params.size(), 0.0f);
    textValues_.assign(params.size(), std::string());
    for (size_t i = 0; i < params.size(); ++i) {
      paramValues_[i] = params[i].defaultValue;
      textValues_[i] = params[i].defaultText;
    }
    // Seed "already pushed" with the plugin's own defaults, so the first frame
    // pushes *nothing* and only genuine changes — a saved scene, an operator
    // dragging a slider — ever reach the plugin.
    //
    // Pushing the harvested defaults back at the plugin is not the no-op it
    // looks like. The plugin's constructor has already set its own defaults;
    // what we harvested is our reading of them, and where the two disagree the
    // push silently replaces working values with wrong ones. Measured: doing
    // this to Porthole made it render at quarter scale in the corner of the
    // frame, while the identical bundle driven by oxbow — which pushes only
    // what `--set` names — fills the frame. Same rule the OFX bridge arrived
    // at independently: push only dirty values, and stay silent on the first
    // frame except for values that are genuinely off their defaults.
    paramPushed_ = paramValues_;
    textPushed_ = textValues_;
    paramsDirty_ = true;

    // Now that the parameter table exists, read the saved values into it.
    if (obs_data_t* settings = obs_source_get_settings(self_)) {
      update(settings);
      obs_data_release(settings);
    }
  }

  // FFGL plugins size their internal buffers from the viewport they were
  // instantiated with, so a resolution change is a teardown, not a resize.
  if (instance_ != nullptr && (instanceWidth_ != width || instanceHeight_ != height)) {
    instance_.reset();
  }

  if (instance_ == nullptr) {
    std::string error;
    instance_ = library_->createInstance(width, height, error);
    if (instance_ == nullptr) {
      if (!reportedFailure_) {
        blog(LOG_WARNING, "[obs-ffgl] %s: %s", loadedPath_.c_str(), error.c_str());
        reportedFailure_ = true;
      }
      return false;
    }
    instanceWidth_ = width;
    instanceHeight_ = height;
    paramsDirty_ = true;

    // A new instance has run its own constructor, so it is back at the
    // plugin's defaults — not at whatever the previous instance was showing.
    // Re-seed from those defaults so that everything the operator has actually
    // changed is pushed again, and nothing else is.
    const std::vector<oxbow::FfglParam>& params = library_->info().params;
    for (size_t i = 0; i < params.size() && i < paramPushed_.size(); ++i) {
      paramPushed_[i] = params[i].defaultValue;
      textPushed_[i] = params[i].defaultText;
    }
  }

  if (outputTexture_ != nullptr &&
      (gs_texture_get_width(outputTexture_) != width ||
       gs_texture_get_height(outputTexture_) != height)) {
    gs_texture_destroy(outputTexture_);
    outputTexture_ = nullptr;
  }
  if (outputTexture_ == nullptr) {
    outputTexture_ = gs_texture_create(width, height, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
    if (outputTexture_ == nullptr) return false;
  }

  if (inputRender_ == nullptr) {
    inputRender_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    if (inputRender_ == nullptr) return false;
  }

  return true;
}

void FfglEffect::pushDirtyParams() {
  if (!paramsDirty_ || library_ == nullptr || instance_ == nullptr) return;

  const std::vector<oxbow::FfglParam>& params = library_->info().params;
  for (size_t i = 0; i < params.size(); ++i) {
    if (isTextType(params[i].type)) {
      if (textValues_[i] != textPushed_[i]) {
        instance_->setParamText(params[i].index, textValues_[i]);
        textPushed_[i] = textValues_[i];
      }
      continue;
    }
    // NaN never compares equal, which is what makes the "never pushed"
    // sentinel work without a second parallel array of flags.
    if (!(paramValues_[i] == paramPushed_[i])) {
      instance_->setParamFloat(params[i].index, paramValues_[i]);
      paramPushed_[i] = paramValues_[i];
    }
  }
  paramsDirty_ = false;
}

void FfglEffect::tick(float seconds) { time_ += seconds; }

uint32_t FfglEffect::width() const { return instanceWidth_; }
uint32_t FfglEffect::height() const { return instanceHeight_; }

void FfglEffect::render() {
  obs_source_t* target = obs_filter_get_target(self_);
  if (target == nullptr) {
    obs_source_skip_video_filter(self_);
    return;
  }

  const uint32_t width = obs_source_get_base_width(target);
  const uint32_t height = obs_source_get_base_height(target);
  if (width == 0 || height == 0 || !ensureInstance(width, height)) {
    obs_source_skip_video_filter(self_);
    return;
  }

  // ---- 1. the input, as a texture ----------------------------------------
  //
  // A filter is handed its target to draw, not a texture, so the only way to
  // get one is to render the target into our own. Blending is forced off for
  // that pass: the texture starts cleared to transparent black and we want the
  // target's own alpha in it, not the target composited over nothing.
  // **Not** `obs_source_video_render(target)`. That re-enters the filter
  // chain this filter is part of, OBS's re-entrancy guard declines to draw,
  // and the texrender comes back entirely black — whereupon a pass-through
  // plugin like Porthole faithfully outputs black and looks broken. The
  // symptom is three errors per frame, in OBS's log rather than ours:
  //
  //     effect_setval_inline: invalid param
  //     No vertex shader specified
  //     device_draw (GL) failed
  //
  // The supported way to get "everything below me" as a texture is to open
  // the filter properly and let OBS's own machinery render it *into* our
  // texrender: process_filter_begin, then process_filter_end inside the
  // texrender's scope.
  if (!obs_source_process_filter_begin(self_, GS_RGBA, OBS_NO_DIRECT_RENDERING)) {
    obs_source_skip_video_filter(self_);
    return;
  }

  gs_texrender_reset(inputRender_);
  if (!gs_texrender_begin(inputRender_, width, height)) {
    obs_source_skip_video_filter(self_);
    return;
  }

  struct vec4 clear;
  vec4_zero(&clear);
  gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
  gs_ortho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -100.0f, 100.0f);
  gs_blend_state_push();
  gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
  obs_source_process_filter_end(self_, obs_get_base_effect(OBS_EFFECT_DEFAULT), width, height);
  gs_blend_state_pop();
  gs_texrender_end(inputRender_);

  const GLuint inputName = glTextureName(gs_texrender_get_texture(inputRender_));

  // ---- 2. point OBS at our output, then ask GL what that means ------------
  //
  // Rather than manage an FBO ourselves, let libobs make and bind one — then
  // read the binding back. That keeps libobs's `cur_fbo` shadow and reality
  // in agreement, which a private FBO would not, and it is safe because
  // `device_set_render_target` binds eagerly (gl-subsystem.c: `set_target` ->
  // `set_current_fbo` -> `gl_bind_framebuffer`), not at the next draw.
  gs_texture_t* previousTarget = gs_get_render_target();
  gs_zstencil_t* previousZs = gs_get_zstencil_target();
  gs_set_render_target(outputTexture_, nullptr);

  GLint hostFbo = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &hostFbo);

  // libobs binds only GL_DRAW_FRAMEBUFFER (gl-subsystem.c: `gl_bind_framebuffer
  // (GL_DRAW_FRAMEBUFFER, …)`), leaving GL_READ_FRAMEBUFFER pointing at
  // whatever OBS last read from. oxbow's working host binds GL_FRAMEBUFFER,
  // which is both. Match it: an FFGL plugin is entitled to assume the two
  // agree, and the SDK's own scoped-binding helpers save and restore with
  // GL_FRAMEBUFFER.
  glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(hostFbo));

  // ---- 3. run the plugin --------------------------------------------------
  bool rendered = false;
  {
    GlStateGuard guard;
    resetForPlugin();

    pushDirtyParams();
    if (library_->info().supportsSetTime) instance_->setTime(time_);

    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));

    // OBS_FFGL_DEBUG=2: paint the target magenta first. If the readback below
    // still shows magenta, our own writes reach this framebuffer and the
    // plugin drew nothing; if it shows black, the framebuffer we think we are
    // handing over is not the one being read. The two look identical from the
    // outside and have completely different causes.
    const char* debug = std::getenv("OBS_FFGL_DEBUG");
    const char debugMode = debug != nullptr ? debug[0] : '0';

    if (debugMode == '2' || debugMode == '3') {
      glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
    }

    // oxbow clears before every effect in its chain. Match it: FFGL plugins
    // that composite over their target (the fleet's "… Over" variants) read
    // what is already there, so an uncleared buffer is stale frame data.
    if (debugMode != '2' && debugMode != '3') {
      glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
      glClear(GL_COLOR_BUFFER_BIT);
    }

    if (debugMode == '3') {
      // Mode 3 skips the plugin altogether, so what reaches the screen is the
      // magenta above and nothing else. Magenta on screen means every part of
      // this path works except FFGL: the render target, the framebuffer we
      // hand over, the readback, and the draw back into OBS. Black on screen
      // means the fault is ours and no amount of staring at plugins will find
      // it.
      rendered = true;
    } else {
      // A source plugin is told it has no input textures at all. Handing one
      // to a plugin that declares FF_SOURCE makes the host disagree with the
      // plugin about how many inputs are in play, which oxbow avoids the same
      // way.
      const bool isSource = library_->info().type == FF_SOURCE;
      rendered = instance_->process(isSource ? 0 : inputName, width, height,
                                    static_cast<uint32_t>(hostFbo));
    }
  }

  // OBS_FFGL_DEBUG=4 writes the input texture and the plugin's output to
  // /tmp as PPMs, once. A picture settles in one look what pixel statistics
  // argue about for an hour — in particular whether the input the plugin was
  // handed contained anything at all.
  if (const char* debug = std::getenv("OBS_FFGL_DEBUG")) {
    static bool dumped = false;
    if (debug[0] == '4' && !dumped) {
      dumped = true;
      std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);

      auto writePpm = [&](const char* path) {
        FILE* file = std::fopen(path, "wb");
        if (file == nullptr) return;
        std::fprintf(file, "P6\n%u %u\n255\n", width, height);
        for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
          std::fwrite(&pixels[i * 4], 1, 3, file);
        }
        std::fclose(file);
      };

      // The plugin's output, straight out of the framebuffer it drew into.
      glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(hostFbo));
      glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA,
                   GL_UNSIGNED_BYTE, pixels.data());
      writePpm("/tmp/obs-ffgl-output.ppm");

      // The input, read back through a scratch FBO — the only way to see a
      // texture's contents without a shader.
      GLuint scratch = 0;
      glGenFramebuffers(1, &scratch);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, scratch);
      glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, inputName,
                             0);
      const GLenum inputStatus = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
      std::fill(pixels.begin(), pixels.end(), 0);
      glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA,
                   GL_UNSIGNED_BYTE, pixels.data());
      writePpm("/tmp/obs-ffgl-input.ppm");
      glDeleteFramebuffers(1, &scratch);

      blog(LOG_INFO, "[obs-ffgl] debug: dumped PPMs; inputFboStatus=0x%x err=0x%x", inputStatus,
           glGetError());
    }
  }

  // OBS_FFGL_DEBUG=1 reads the centre pixel straight out of the framebuffer
  // the plugin just drew into, before anything else can touch it. It is the
  // only way to tell "the plugin rendered black" apart from "the plugin
  // rendered fine and we drew the wrong thing afterwards" — the two are
  // identical from outside.
  if (std::getenv("OBS_FFGL_DEBUG") != nullptr) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(hostFbo));
    uint8_t pixel[4] = {};
    glReadPixels(static_cast<GLint>(width / 2), static_cast<GLint>(height / 2), 1, 1, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixel);
    const GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    GLint viewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, viewport);
    blog(LOG_INFO,
         "[obs-ffgl] debug: fbo=%d status=0x%x input=%u centre=%u,%u,%u,%u rendered=%d "
         "frame=%ux%u instance=%ux%u outTex=%ux%u viewportNow=%d,%d,%d,%d",
         hostFbo, status, inputName, pixel[0], pixel[1], pixel[2], pixel[3], rendered ? 1 : 0,
         width, height, instanceWidth_, instanceHeight_,
         outputTexture_ != nullptr ? gs_texture_get_width(outputTexture_) : 0,
         outputTexture_ != nullptr ? gs_texture_get_height(outputTexture_) : 0, viewport[0],
         viewport[1], viewport[2], viewport[3]);
  }

  gs_set_render_target(previousTarget, previousZs);

  if (!rendered) {
    if (!reportedFailure_) {
      blog(LOG_WARNING, "[obs-ffgl] %s: ProcessOpenGL failed", loadedPath_.c_str());
      reportedFailure_ = true;
    }
    obs_source_skip_video_filter(self_);
    return;
  }

  // ---- 4. hand the result back to OBS -------------------------------------
  // obs_source_draw sets the effect's "image" parameter itself, which is the
  // documented way for a source to put a texture on screen. Setting it by hand
  // and calling gs_draw_sprite works too, but duplicates logic that libobs
  // changes between versions for no gain here.
  gs_effect_t* draw = obs_get_base_effect(OBS_EFFECT_DEFAULT);
  while (gs_effect_loop(draw, "Draw")) {
    obs_source_draw(outputTexture_, 0, 0, width, height, false);
  }
}

}  // namespace obsffgl

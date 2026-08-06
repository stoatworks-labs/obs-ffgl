#pragma once
//
// One FFGL plugin instance living inside one OBS source.
//
// This holds everything that is per-OBS-instance — the chosen bundle, the live
// FFGL instance, the textures, the parameter values — and nothing that is
// shared. Libraries are shared and live in ffgl_catalog.
//
// **The thread split is the thing to keep straight.** OBS calls `update` and
// `properties` on the UI thread and `render`/`tick` on the graphics thread,
// and only the graphics thread has a GL context. So:
//
//   - `update` records what the operator asked for. It never creates or
//     destroys an FFGL instance, because FF_INSTANTIATE_GL needs a context.
//   - `render` notices the request has changed and acts on it, on the thread
//     that can. A plugin swap therefore takes effect on the next frame, not
//     inside the settings dialog.
//
// Getting this backwards produces a plugin that works until someone changes a
// setting while the scene is live, which is the worst time to find out.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <obs-module.h>

#include <ffgl/ffgl_host.h>

namespace obsffgl {

class FfglEffect {
 public:
  /// Which OBS object this is standing behind.
  ///
  /// An FFGL *effect* takes a texture in and gives one back, which is an OBS
  /// filter. An FFGL *source* takes no input at all, which is an OBS input —
  /// and forcing one to be a filter means the operator has to invent a source
  /// underneath it whose picture is then thrown away. Same plugin, same render
  /// call; the difference is where the frame comes from and who decides how
  /// big it is.
  enum class Mode { filter, source };

  FfglEffect(obs_source_t* self, Mode mode);
  ~FfglEffect();

  FfglEffect(const FfglEffect&) = delete;
  FfglEffect& operator=(const FfglEffect&) = delete;

  // ---- UI thread ----------------------------------------------------------
  static void defaults(obs_data_t* settings, Mode mode);
  void update(obs_data_t* settings);
  obs_properties_t* properties();

  // ---- graphics thread ----------------------------------------------------
  void render();
  void tick(float seconds);

  uint32_t width() const;
  uint32_t height() const;

 private:
  /// Bring the FFGL instance into line with `requestedPath_` and the frame
  /// size. Graphics thread only. Returns false if there is nothing to render
  /// with, in which case the caller passes the frame through untouched.
  bool ensureInstance(uint32_t width, uint32_t height);

  void destroyInstance();

  /// Push any parameter whose value has changed since the last frame. FFGL is
  /// stateful: a value set once stays set, so pushing every parameter every
  /// frame would fight any plugin that writes to its own parameters (the
  /// fleet's preset dropdowns do exactly that).
  void pushDirtyParams();

  /// Add one parameter's widget to `props` — which is the group's property
  /// list, not necessarily the root one.
  void addParamProperty(obs_properties_t* props, const oxbow::FfglParam& param);

  /// The obs_data key for one FFGL parameter. Namespaced by the plugin's own
  /// four-character id so that switching plugin and switching back does not
  /// find the other plugin's values sitting in the same slots.
  std::string paramKey(const oxbow::FfglParam& param) const;

  /// Where the frame size comes from. A filter inherits it from whatever it
  /// is filtering; a source has nothing to inherit and must be told.
  void resolveSourceSize(uint32_t& width, uint32_t& height) const;

  obs_source_t* self_ = nullptr;
  Mode mode_ = Mode::filter;

  // What the operator asked for (UI thread writes, graphics thread reads).
  std::string requestedPath_;
  uint32_t requestedWidth_ = 0;   //!< source mode; 0 means "follow the canvas"
  uint32_t requestedHeight_ = 0;

  // What we actually have (graphics thread only).
  std::string loadedPath_;
  oxbow::FfglLibrary* library_ = nullptr;
  std::unique_ptr<oxbow::FfglInstance> instance_ = nullptr;

  gs_texrender_t* inputRender_ = nullptr;
  gs_texture_t* outputTexture_ = nullptr;
  uint32_t instanceWidth_ = 0;
  uint32_t instanceHeight_ = 0;

  std::vector<float> paramValues_;
  std::vector<float> paramPushed_;
  std::vector<std::string> textValues_;
  std::vector<std::string> textPushed_;
  bool paramsDirty_ = true;

  double time_ = 0.0;

  // One complaint per broken configuration, not one per frame.
  bool reportedFailure_ = false;
};

}  // namespace obsffgl

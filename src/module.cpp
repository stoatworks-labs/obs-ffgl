// The OBS module: what OBS sees, and nothing else.
//
// Everything here is glue. The C callbacks OBS wants are thin wrappers over
// FfglEffect, so that the interesting code never has to think in void*.

#include <obs-module.h>

#include <graphics/graphics.h>

#include "ffgl_catalog.h"
#include "ffgl_effect.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-ffgl", "en-US")

MODULE_EXPORT const char* obs_module_description(void) {
  return "Run FreeFrame (FFGL) video plugins natively inside OBS";
}

namespace {

using obsffgl::FfglEffect;

const char* filterName(void*) { return obs_module_text("FfglEffect"); }

void* filterCreate(obs_data_t* settings, obs_source_t* source) {
  auto* effect = new FfglEffect(source);
  effect->update(settings);
  return effect;
}

void filterDestroy(void* data) { delete static_cast<FfglEffect*>(data); }

void filterUpdate(void* data, obs_data_t* settings) {
  static_cast<FfglEffect*>(data)->update(settings);
}

void filterDefaults(obs_data_t* settings) { FfglEffect::defaults(settings); }

obs_properties_t* filterProperties(void* data) {
  return static_cast<FfglEffect*>(data)->properties();
}

void filterTick(void* data, float seconds) { static_cast<FfglEffect*>(data)->tick(seconds); }

void filterRender(void* data, gs_effect_t* effect) {
  UNUSED_PARAMETER(effect);
  static_cast<FfglEffect*>(data)->render();
}

struct obs_source_info ffglFilter = {};

/// libobs is a C API with a Direct3D backend on Windows and OpenGL elsewhere.
/// This plugin drives FFGL — which is an OpenGL format, by definition — so it
/// can only work where OBS is on its GL backend. Refusing here, once, with the
/// reason, beats registering a filter that produces black.
bool graphicsBackendIsOpenGl() {
  obs_enter_graphics();
  const int type = gs_get_device_type();
  obs_leave_graphics();
  return type == GS_DEVICE_OPENGL;
}

}  // namespace

bool obs_module_load(void) {
  if (!graphicsBackendIsOpenGl()) {
    blog(LOG_WARNING,
         "[obs-ffgl] OBS is not running its OpenGL renderer; FFGL plugins are an "
         "OpenGL format and cannot be hosted on this backend. Nothing registered.");
    return false;
  }

  ffglFilter.id = "ffgl_effect";
  ffglFilter.type = OBS_SOURCE_TYPE_FILTER;
  // Plain OBS_SOURCE_VIDEO. OBS_SOURCE_CUSTOM_DRAW is an *input* flag — on a
  // filter it changes how OBS sets up the effect before video_render and
  // leaves the direct draw below without a shader loaded.
  ffglFilter.output_flags = OBS_SOURCE_VIDEO;
  ffglFilter.get_name = filterName;
  ffglFilter.create = filterCreate;
  ffglFilter.destroy = filterDestroy;
  ffglFilter.update = filterUpdate;
  ffglFilter.get_defaults = filterDefaults;
  ffglFilter.get_properties = filterProperties;
  ffglFilter.video_tick = filterTick;
  ffglFilter.video_render = filterRender;

  obs_register_source(&ffglFilter);

  // Deliberately NOT scanning here.
  //
  // The search path includes `~/Documents/Resolume …`, and on macOS the first
  // read of ~/Documents by a process triggers a TCC consent prompt. Doing that
  // from obs_module_load blocks OBS's startup until it is answered — measured
  // at 22 seconds on this machine the first time, against 137 ms every time
  // after — and it happens before the main window exists, so the operator sees
  // a hung OBS and a permission dialog with nothing behind it.
  //
  // Scanning lazily moves that prompt to the moment someone opens the filter's
  // properties, which is a deliberate act with obvious context.
  blog(LOG_INFO, "[obs-ffgl] loaded (plugins are scanned when first needed)");
  return true;
}

void obs_module_unload(void) {}

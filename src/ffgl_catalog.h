#pragma once
//
// Which FFGL plugins exist on this machine, and one loaded copy of each.
//
// Two separate jobs that both belong here because they share a cache:
//
//   - **Scanning.** Walk the directories FFGL hosts conventionally use, plus
//     anything the operator adds, and report what is there. This runs on the
//     UI thread when OBS builds a properties dialog, so it must not touch GL.
//   - **Loading.** `oxbow::FfglLibrary` is one dlopen plus one FFGL prototype
//     instance. FFGL's entry point is a single global `plugMain` per loaded
//     module, so a second dlopen of the same bundle would hand back the same
//     module anyway — the cache makes that explicit and lets ten OBS filters
//     share one library. Instances are per-filter; libraries are not.
//
// The split that matters for threading: `open()` never touches GL, so the
// catalog is safe from any thread. `createInstance()` and everything on
// `FfglInstance` need OBS's graphics context current, so they happen on the
// graphics thread inside video_render — never here.

#include <memory>
#include <string>
#include <vector>

#include <ffgl/ffgl_host.h>

namespace obsffgl {

/// One plugin as the dropdown sees it. Deliberately not holding the library:
/// building a properties list must not keep thirty modules resident.
struct CatalogEntry {
  std::string path;
  std::string name;      //!< the plugin's own name, up to 16 chars
  std::string uniqueId;  //!< the 4-character FFGL id
  uint32_t type = 0;     //!< FF_EFFECT or FF_SOURCE
  bool isSource = false;
};

/// Everything found, sorted by name. Cached; pass `rescan` to walk the
/// directories again (the properties dialog offers a Rescan button, because a
/// VJ who just installed a plugin should not have to restart OBS).
const std::vector<CatalogEntry>& catalog(bool rescan = false);

/// The directories that were searched, in order, for the properties dialog to
/// show. A user whose plugin did not appear needs to see where we looked far
/// more than they need another error message.
const std::vector<std::string>& searchPaths();

/// Open (or return the already-open) library for `path`. Never touches GL, so
/// this is safe to call before the graphics thread has a context. Returns
/// nullptr and sets `error` on failure; a failure is cached too, so a broken
/// bundle is not re-dlopened sixty times a second.
oxbow::FfglLibrary* acquire(const std::string& path, std::string& error);

}  // namespace obsffgl

#include "ffgl_catalog.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>

#include <obs-module.h>

#include <ffgl/FFGL.h>

namespace obsffgl {
namespace {

namespace fs = std::filesystem;

std::mutex g_mutex;
std::vector<CatalogEntry> g_entries;
std::vector<std::string> g_searchPaths;
bool g_scanned = false;

// path -> library, plus a null entry meaning "this one failed, stop trying".
std::map<std::string, std::unique_ptr<oxbow::FfglLibrary>> g_libraries;
std::map<std::string, std::string> g_failures;

std::string home() {
  const char* value = std::getenv("HOME");
  return value != nullptr ? value : std::string();
}

/// On macOS an FFGL plugin is a `.bundle`; so is an OpenFX plugin, and the
/// fleet builds both side by side into the same `build/` directory. `.ofx` is
/// part of the name rather than the extension (`Downpour.ofx.bundle`), so
/// `path.extension()` cannot tell them apart — check the stem.
bool looksLikeFfgl(const fs::path& path) {
#if defined(_WIN32)
  return path.extension() == ".dll";
#else
  if (path.extension() != ".bundle") return false;
  return fs::path(path.stem()).extension() != ".ofx";
#endif
}

void addSearchPath(std::vector<std::string>& out, const std::string& path) {
  if (path.empty()) return;
  if (std::find(out.begin(), out.end(), path) != out.end()) return;
  out.push_back(path);
}

std::vector<std::string> buildSearchPaths() {
  std::vector<std::string> paths;

  // The operator's own list first, so it can shadow anything. Colon-separated
  // on Unix, semicolon on Windows, matching what each platform's PATH does —
  // a Windows drive letter contains a colon.
  if (const char* extra = std::getenv("OBS_FFGL_PATH")) {
    const char separator =
#if defined(_WIN32)
        ';';
#else
        ':';
#endif
    std::string value(extra);
    size_t start = 0;
    while (start <= value.size()) {
      const size_t end = value.find(separator, start);
      const size_t stop = end == std::string::npos ? value.size() : end;
      addSearchPath(paths, value.substr(start, stop - start));
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }

  // Where OBS lets a plugin keep its own files. This is the directory the
  // README tells people to drop bundles into, because it is the only one that
  // is unambiguously ours and needs no admin rights.
  if (char* configured = obs_module_config_path("plugins")) {
    addSearchPath(paths, configured);
    bfree(configured);
  }

  const std::string userHome = home();

#if defined(_WIN32)
  if (const char* programFiles = std::getenv("CommonProgramFiles")) {
    addSearchPath(paths, std::string(programFiles) + "\\FreeFrame");
  }
  if (!userHome.empty()) {
    addSearchPath(paths, userHome + "\\Documents\\Resolume Arena\\Extra Effects");
    addSearchPath(paths, userHome + "\\Documents\\Resolume Avenue\\Extra Effects");
  }
#else
  // The FreeFrame convention, which is what a plugin's own installer targets.
  if (!userHome.empty()) {
    addSearchPath(paths, userHome + "/Library/Graphics/FreeFrame Plug-Ins");
  }
  addSearchPath(paths, "/Library/Graphics/FreeFrame Plug-Ins");

  // Resolume's, because in practice that is where a VJ's plugins already are,
  // and asking them to keep a second copy for OBS would be a poor trade. Only
  // Arena and Avenue actually scan these — Alley and Wire do not, which is a
  // Resolume fact rather than one of ours.
  if (!userHome.empty()) {
    addSearchPath(paths, userHome + "/Documents/Resolume Arena/Extra Effects");
    addSearchPath(paths, userHome + "/Documents/Resolume Avenue/Extra Effects");
  }
#endif

  return paths;
}

void scanLocked() {
  g_entries.clear();
  g_searchPaths = buildSearchPaths();

  for (const std::string& directory : g_searchPaths) {
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) continue;

    for (const fs::directory_entry& item : fs::directory_iterator(directory, ec)) {
      if (ec) break;
      if (!looksLikeFfgl(item.path())) continue;

      const std::string path = item.path().string();

      // Opening is the only honest test of whether a bundle is an FFGL plugin
      // — the extension is a guess. The library is dropped again immediately:
      // a scan must not leave thirty modules resident just to fill a dropdown.
      std::string error;
      std::unique_ptr<oxbow::FfglLibrary> library = oxbow::FfglLibrary::open(path, error);
      if (!library) {
        blog(LOG_DEBUG, "[obs-ffgl] skipping %s: %s", path.c_str(), error.c_str());
        continue;
      }

      const oxbow::FfglInfo& info = library->info();
      CatalogEntry entry;
      entry.path = path;
      entry.name = info.name.empty() ? item.path().stem().string() : info.name;
      entry.uniqueId = info.uniqueId;
      entry.type = info.type;
      entry.isSource = info.type == FF_SOURCE;
      g_entries.push_back(std::move(entry));
    }
  }

  std::sort(g_entries.begin(), g_entries.end(),
            [](const CatalogEntry& a, const CatalogEntry& b) { return a.name < b.name; });

  g_scanned = true;
  blog(LOG_INFO, "[obs-ffgl] catalog: %zu plugin(s) across %zu director%s", g_entries.size(),
       g_searchPaths.size(), g_searchPaths.size() == 1 ? "y" : "ies");
}

}  // namespace

const std::vector<CatalogEntry>& catalog(bool rescan) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (rescan || !g_scanned) scanLocked();
  return g_entries;
}

const std::vector<std::string>& searchPaths() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_scanned) scanLocked();
  return g_searchPaths;
}

oxbow::FfglLibrary* acquire(const std::string& path, std::string& error) {
  std::lock_guard<std::mutex> lock(g_mutex);

  auto existing = g_libraries.find(path);
  if (existing != g_libraries.end()) return existing->second.get();

  auto failed = g_failures.find(path);
  if (failed != g_failures.end()) {
    error = failed->second;
    return nullptr;
  }

  std::unique_ptr<oxbow::FfglLibrary> library = oxbow::FfglLibrary::open(path, error);
  if (!library) {
    // Remember the failure. Without this a bad path in a saved scene would
    // dlopen and fail once per frame, and the log would be the only symptom.
    g_failures[path] = error;
    blog(LOG_WARNING, "[obs-ffgl] cannot load %s: %s", path.c_str(), error.c_str());
    return nullptr;
  }

  blog(LOG_INFO, "[obs-ffgl] loaded %s (%s) from %s", library->info().name.c_str(),
       library->info().uniqueId.c_str(), path.c_str());

  oxbow::FfglLibrary* raw = library.get();
  g_libraries[path] = std::move(library);
  return raw;
}

}  // namespace obsffgl
